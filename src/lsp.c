#include "lsp.h"
#include "json.h"
#include "analyze.h"
#include "ast.h"
#include "pass1.h"
#include "types.h"
#include "token.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* ======================================================================== */
/* Document store                                                           */
/* ======================================================================== */

typedef struct {
    char           *uri;        /* owned */
    char           *path;       /* owned: filesystem path decoded from uri */
    char           *text;       /* owned: current full document text (UTF-8) */
    int             text_len;
    long            version;
    AnalysisResult *result;     /* latest analysis; NULL until first analyze */
} LspDoc;

typedef struct {
    LspDoc *docs;
    int count, cap;
} LspDocStore;

typedef struct {
    LspDocStore store;
    bool initialized;
    bool shutdown_requested;
    Arena msg_arena;            /* reset per message: holds request + response */

    /* stdlib sources, read once at startup and merged into every analysis so
     * `import ... from std::...` resolves. Each entry's `text`/`filename` are
     * owned here (malloc'd) and live for the whole session. */
    AnalysisSource *stdlib;
    int             stdlib_count, stdlib_cap;
} LspServer;

static LspDoc *store_find(LspDocStore *s, const char *uri) {
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
    return NULL;
}

static LspDoc *store_find_by_path(LspDocStore *s, const char *path) {
    for (int i = 0; i < s->count; i++)
        if (s->docs[i].path && strcmp(s->docs[i].path, path) == 0) return &s->docs[i];
    return NULL;
}

/* ======================================================================== */
/* URI <-> path                                                             */
/* ======================================================================== */

static char *read_whole_file(const char *path, int *out_len);   /* defined below */

/* Local strdup (POSIX strdup is unavailable under -std=c11 -Wpedantic). */
static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    memcpy(p, s, n);
    return p;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* file:///home/x%20y.fc -> /home/x y.fc  (caller frees) */
static char *uri_to_path(const char *uri) {
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        /* skip an optional authority (file://host/path) up to the next '/' */
        if (*p && *p != '/') {
            const char *slash = strchr(p, '/');
            p = slash ? slash : p + strlen(p);
        }
    }
    int n = (int)strlen(p);
    char *out = malloc((size_t)n + 1);
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] == '%' && i + 2 < n && hexval(p[i + 1]) >= 0 && hexval(p[i + 2]) >= 0) {
            out[j++] = (char)((hexval(p[i + 1]) << 4) | hexval(p[i + 2]));
            i += 2;
        } else {
            out[j++] = p[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* ======================================================================== */
/* Line index + position mapping                                            */
/*                                                                          */
/* LSP positions are 0-based (line, UTF-16 character); the compiler's        */
/* SrcLoc is 1-based (line, byte column). The line index caches each line's  */
/* byte offset and whether the document is pure ASCII (the common case, an   */
/* O(1) fast path).                                                          */
/* ======================================================================== */

typedef struct {
    int  *starts;   /* byte offset of each line (0-based line index) */
    int   count;
    int   len;      /* total byte length */
    bool  ascii;
} LineIndex;

static LineIndex line_index_build(Arena *a, const char *text, int len) {
    LineIndex idx;
    idx.len = len;
    idx.ascii = true;
    int lines = 1;
    for (int i = 0; i < len; i++) {
        if ((unsigned char)text[i] >= 0x80) idx.ascii = false;
        if (text[i] == '\n') lines++;
    }
    idx.starts = arena_alloc(a, sizeof(int) * (size_t)lines);
    idx.count = lines;
    idx.starts[0] = 0;
    int li = 1;
    for (int i = 0; i < len; i++)
        if (text[i] == '\n') idx.starts[li++] = i + 1;
    return idx;
}

static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

/* LSP (line0, char0 in UTF-16) -> SrcLoc (1-based line, 1-based byte col). */
static void lsp_to_loc(const LineIndex *idx, const char *text,
                       int line0, int char0, int *out_line1, int *out_col1) {
    if (line0 < 0) line0 = 0;
    if (line0 >= idx->count) line0 = idx->count - 1;
    int ls = idx->starts[line0];
    int le = (line0 + 1 < idx->count) ? idx->starts[line0 + 1] : idx->len;
    *out_line1 = line0 + 1;
    if (char0 < 0) char0 = 0;
    if (idx->ascii) {
        int col = char0;
        if (ls + col > le) col = le - ls;
        *out_col1 = col + 1;
        return;
    }
    int byte = ls, u16 = 0;
    while (byte < le && u16 < char0) {
        int nb = utf8_seq_len((unsigned char)text[byte]);
        int nu = (nb == 4) ? 2 : 1;
        byte += nb;
        u16 += nu;
    }
    *out_col1 = (byte - ls) + 1;
}

/* SrcLoc (1-based line, 1-based byte col) -> LSP (line0, char0 in UTF-16). */
static void loc_to_lsp(const LineIndex *idx, const char *text,
                       int line1, int col1, int *out_line0, int *out_char0) {
    int line0 = line1 - 1;
    if (line0 < 0) line0 = 0;
    if (line0 >= idx->count) line0 = idx->count - 1;
    *out_line0 = line0;
    int byte_col = col1 - 1;
    if (byte_col < 0) byte_col = 0;
    if (idx->ascii) { *out_char0 = byte_col; return; }
    int ls = idx->starts[line0];
    int b = 0, u16 = 0;
    while (b < byte_col && ls + b < idx->len) {
        int nb = utf8_seq_len((unsigned char)text[ls + b]);
        int nu = (nb == 4) ? 2 : 1;
        b += nb;
        u16 += nu;
    }
    *out_char0 = u16;
}

/* Byte offset in the document of a SrcLoc (1-based line/col). */
static int loc_byte_offset(const LineIndex *idx, int line1, int col1) {
    int line0 = line1 - 1;
    if (line0 < 0) line0 = 0;
    if (line0 >= idx->count) line0 = idx->count - 1;
    return idx->starts[line0] + (col1 - 1);
}

/* ======================================================================== */
/* JSON-RPC wire helpers                                                     */
/* ======================================================================== */

static void lsp_write(const JsonValue *msg) {
    char *buf = NULL;
    int len = 0, cap = 0;
    json_serialize(msg, &buf, &len, &cap);
    printf("Content-Length: %d\r\n\r\n", len);
    fwrite(buf, 1, (size_t)len, stdout);
    fflush(stdout);
    free(buf);
}

static void lsp_reply(Arena *a, JsonValue *id, JsonValue *result) {
    JsonValue *msg = json_object(a);
    json_object_set(a, msg, "jsonrpc", json_str(a, "2.0"));
    json_object_set(a, msg, "id", id ? id : json_null(a));
    json_object_set(a, msg, "result", result ? result : json_null(a));
    lsp_write(msg);
}

static void lsp_reply_error(Arena *a, JsonValue *id, int code, const char *message) {
    JsonValue *msg = json_object(a);
    json_object_set(a, msg, "jsonrpc", json_str(a, "2.0"));
    json_object_set(a, msg, "id", id ? id : json_null(a));
    JsonValue *err = json_object(a);
    json_object_set(a, err, "code", json_num(a, code));
    json_object_set(a, err, "message", json_str(a, message));
    json_object_set(a, msg, "error", err);
    lsp_write(msg);
}

static void lsp_notify(Arena *a, const char *method, JsonValue *params) {
    JsonValue *msg = json_object(a);
    json_object_set(a, msg, "jsonrpc", json_str(a, "2.0"));
    json_object_set(a, msg, "method", json_str(a, method));
    json_object_set(a, msg, "params", params);
    lsp_write(msg);
}

/* Build an LSP Position object. */
static JsonValue *mk_pos(Arena *a, int line0, int char0) {
    JsonValue *p = json_object(a);
    json_object_set(a, p, "line", json_num(a, line0));
    json_object_set(a, p, "character", json_num(a, char0));
    return p;
}
static JsonValue *mk_range(Arena *a, int l0, int c0, int l1, int c1) {
    JsonValue *r = json_object(a);
    json_object_set(a, r, "start", mk_pos(a, l0, c0));
    json_object_set(a, r, "end", mk_pos(a, l1, c1));
    return r;
}

/* ======================================================================== */
/* Type rendering (type_name uses rotating static buffers — copy at once)   */
/* ======================================================================== */

static void copy_type_name(Type *t, char *out, size_t n) {
    const char *tn = type_name(t);   /* rotating static buffer */
    snprintf(out, n, "%s", tn ? tn : "?");
}

/* ======================================================================== */
/* Position -> node lookup (hover / definition / completion anchor)         */
/* ======================================================================== */

typedef struct {
    int target_line, target_col;   /* 1-based, byte col */
    const char *src;
    const LineIndex *idx;
    const char *file;              /* only descend into decls from this file */

    bool found;
    int  best_span;
    int  start_line, start_col;     /* of the winning node */
    Type *type;
    const char *name;
    Symbol *sym;                    /* resolved symbol, for go-to-def (may be NULL) */
} FindCtx;

static void consider(FindCtx *c, int line, int col, int span,
                     Type *type, const char *name, Symbol *sym) {
    if (line != c->target_line) return;
    if (c->target_col < col || c->target_col >= col + span) return;
    if (c->found && span > c->best_span) return;   /* keep the innermost (smallest) */
    c->found = true;
    c->best_span = span;
    c->start_line = line;
    c->start_col = col;
    c->type = type;
    c->name = name;
    c->sym = sym;
}

/* Column (1-based) of the binding name in a `let [mut] name ...` whose `let`
 * keyword is at (line1, let_col1). Scans the source past the keyword(s). */
static int let_name_col(const FindCtx *c, int let_line, int let_col, bool is_mut) {
    int off = c->idx->starts[let_line - 1] + (let_col - 1);
    int end = c->idx->len;
    int i = off;
    /* skip "let" */
    i += 3;
    while (i < end && (c->src[i] == ' ' || c->src[i] == '\t')) i++;
    if (is_mut) {
        i += 3; /* "mut" */
        while (i < end && (c->src[i] == ' ' || c->src[i] == '\t')) i++;
    }
    return (i - c->idx->starts[let_line - 1]) + 1;
}

static void find_in_expr(Expr *e, FindCtx *c);

static void find_in_exprs(Expr **arr, int n, FindCtx *c) {
    for (int i = 0; i < n; i++) find_in_expr(arr[i], c);
}

static void find_in_expr(Expr *e, FindCtx *c) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_IDENT:
            consider(c, e->loc.line, e->loc.col, (int)strlen(e->ident.name),
                     e->type, e->ident.name, e->ident.resolved_sym);
            break;
        case EXPR_FIELD:
        case EXPR_DEREF_FIELD:
            find_in_expr(e->field.object, c);
            /* Best-effort: when the object is a simple identifier on one line,
             * the field name's position is computable, so the field's resolved
             * type can be hovered precisely. */
            if (e->field.object && e->field.object->kind == EXPR_IDENT) {
                int sep = (e->kind == EXPR_DEREF_FIELD) ? 2 : 1;  /* "->" vs "." */
                int col = e->field.object->loc.col +
                          (int)strlen(e->field.object->ident.name) + sep;
                consider(c, e->field.object->loc.line, col,
                         (int)strlen(e->field.name), e->type, e->field.name, NULL);
            }
            break;
        case EXPR_BINARY:
            find_in_expr(e->binary.left, c);
            find_in_expr(e->binary.right, c);
            break;
        case EXPR_UNARY_PREFIX:  find_in_expr(e->unary_prefix.operand, c); break;
        case EXPR_UNARY_POSTFIX: find_in_expr(e->unary_postfix.operand, c); break;
        case EXPR_CALL:
            find_in_expr(e->call.func, c);
            find_in_exprs(e->call.args, e->call.arg_count, c);
            break;
        case EXPR_INDEX:
            find_in_expr(e->index.object, c);
            find_in_expr(e->index.index, c);
            break;
        case EXPR_SLICE:
            find_in_expr(e->slice.object, c);
            find_in_expr(e->slice.lo, c);
            find_in_expr(e->slice.hi, c);
            break;
        case EXPR_CAST:    find_in_expr(e->cast.operand, c); break;
        case EXPR_IF:
            find_in_expr(e->if_expr.cond, c);
            find_in_expr(e->if_expr.then_body, c);
            find_in_expr(e->if_expr.else_body, c);
            break;
        case EXPR_MATCH:
            find_in_expr(e->match_expr.subject, c);
            for (int i = 0; i < e->match_expr.arm_count; i++) {
                find_in_expr(e->match_expr.arms[i].guard, c);
                find_in_exprs(e->match_expr.arms[i].body,
                              e->match_expr.arms[i].body_count, c);
            }
            break;
        case EXPR_LOOP: find_in_exprs(e->loop_expr.body, e->loop_expr.body_count, c); break;
        case EXPR_FOR:
            find_in_expr(e->for_expr.iter, c);
            find_in_expr(e->for_expr.range_end, c);
            find_in_exprs(e->for_expr.body, e->for_expr.body_count, c);
            break;
        case EXPR_BREAK:  find_in_expr(e->break_expr.value, c); break;
        case EXPR_RETURN: find_in_expr(e->return_expr.value, c); break;
        case EXPR_BLOCK:  find_in_exprs(e->block.stmts, e->block.count, c); break;
        case EXPR_FUNC:
            for (int i = 0; i < e->func.param_count; i++) {
                Param *p = &e->func.params[i];
                if (p->name)
                    consider(c, p->loc.line, p->loc.col, (int)strlen(p->name),
                             p->type, p->name, NULL);
            }
            find_in_exprs(e->func.body, e->func.body_count, c);
            break;
        case EXPR_STRUCT_LIT:
            for (int i = 0; i < e->struct_lit.field_count; i++)
                find_in_expr(e->struct_lit.fields[i].value, c);
            break;
        case EXPR_TUPLE_LIT: find_in_exprs(e->tuple_lit.elems, e->tuple_lit.elem_count, c); break;
        case EXPR_ARRAY_LIT:
            find_in_expr(e->array_lit.size_expr, c);
            find_in_exprs(e->array_lit.elems, e->array_lit.elem_count, c);
            break;
        case EXPR_SLICE_LIT:
            find_in_expr(e->slice_lit.ptr_expr, c);
            find_in_expr(e->slice_lit.len_expr, c);
            break;
        case EXPR_ALLOC:
            find_in_expr(e->alloc_expr.size_expr, c);
            find_in_expr(e->alloc_expr.init_expr, c);
            break;
        case EXPR_FREE: find_in_expr(e->free_expr.operand, c); break;
        case EXPR_INTERP_STRING:
            for (int i = 0; i < e->interp_string.segment_count; i++)
                if (!e->interp_string.segments[i].is_literal)
                    find_in_expr(e->interp_string.segments[i].expr, c);
            break;
        case EXPR_ASSIGN:
            find_in_expr(e->assign.target, c);
            find_in_expr(e->assign.value, c);
            break;
        case EXPR_SOME: find_in_expr(e->some_expr.value, c); break;
        case EXPR_LET: {
            int col = let_name_col(c, e->loc.line, e->loc.col, e->let_expr.let_is_mut);
            consider(c, e->loc.line, col, (int)strlen(e->let_expr.let_name),
                     e->let_expr.let_type, e->let_expr.let_name, NULL);
            find_in_expr(e->let_expr.let_init, c);
            break;
        }
        case EXPR_LET_DESTRUCT: find_in_expr(e->let_destruct.init, c); break;
        case EXPR_ASSERT:
            find_in_expr(e->assert_expr.condition, c);
            find_in_expr(e->assert_expr.message, c);
            break;
        case EXPR_DEFER: find_in_expr(e->defer_expr.value, c); break;
        case EXPR_ATOMIC_LOAD: find_in_expr(e->atomic_load.ptr, c); break;
        case EXPR_ATOMIC_STORE:
            find_in_expr(e->atomic_store.ptr, c);
            find_in_expr(e->atomic_store.value, c);
            break;
        case EXPR_GUARD: find_in_expr(e->guard.body, c); break;
        default: break;   /* literals, type-var refs, etc. */
    }
}

static void find_in_decls(Decl **decls, int n, FindCtx *c);

static void find_in_decl(Decl *d, FindCtx *c) {
    if (!d) return;
    /* The merged program holds decls from several files with overlapping line
     * numbers; only descend into the open document's decls. */
    if (c->file && d->loc.filename && strcmp(d->loc.filename, c->file) != 0) return;
    switch (d->kind) {
        case DECL_LET:
            if (d->let.name) {
                int col = let_name_col(c, d->loc.line, d->loc.col, d->let.is_mut);
                consider(c, d->loc.line, col, (int)strlen(d->let.name),
                         d->let.resolved_type, d->let.name, NULL);
            }
            find_in_expr(d->let.init, c);
            break;
        case DECL_MODULE:
            find_in_decls(d->module.decls, d->module.decl_count, c);
            break;
        default: break;
    }
}

static void find_in_decls(Decl **decls, int n, FindCtx *c) {
    for (int i = 0; i < n; i++) find_in_decl(decls[i], c);
}

/* Run the position lookup against a document's analysis. */
static bool locate(LspDoc *doc, const LineIndex *idx, int line0, int char0, FindCtx *out) {
    if (!doc->result || !doc->result->program) return false;
    int line1, col1;
    lsp_to_loc(idx, doc->text, line0, char0, &line1, &col1);
    FindCtx c = {0};
    c.target_line = line1;
    c.target_col = col1;
    c.src = doc->text;
    c.idx = idx;
    c.file = doc->path;
    find_in_decls(doc->result->program->decls, doc->result->program->decl_count, &c);
    *out = c;
    return c.found;
}

/* ======================================================================== */
/* Diagnostics                                                              */
/* ======================================================================== */

static void publish_diagnostics(LspServer *S, LspDoc *doc) {
    Arena *a = &S->msg_arena;
    LineIndex idx = line_index_build(a, doc->text, doc->text_len);
    JsonValue *arr = json_array(a);

    if (doc->result) {
        for (int i = 0; i < doc->result->diag_count; i++) {
            Diagnostic *d = &doc->result->diags[i];
            /* Keep only diagnostics for the open file (stdlib is presumed clean;
             * NULL filename defaults to the primary document). */
            if (d->loc.filename && strcmp(d->loc.filename, doc->path) != 0) continue;

            int line1 = d->loc.line > 0 ? d->loc.line : 1;
            int col1  = d->loc.col  > 0 ? d->loc.col  : 1;
            int start_byte = loc_byte_offset(&idx, line1, col1);
            /* Extend the squiggle over the identifier/token at the location. */
            int end_byte = start_byte;
            while (end_byte < doc->text_len &&
                   (isalnum((unsigned char)doc->text[end_byte]) ||
                    doc->text[end_byte] == '_'))
                end_byte++;
            if (end_byte == start_byte) end_byte = start_byte + 1;
            int end_col1 = col1 + (end_byte - start_byte);

            int sl, sc, el, ec;
            loc_to_lsp(&idx, doc->text, line1, col1, &sl, &sc);
            loc_to_lsp(&idx, doc->text, line1, end_col1, &el, &ec);

            JsonValue *diag = json_object(a);
            json_object_set(a, diag, "range", mk_range(a, sl, sc, el, ec));
            json_object_set(a, diag, "severity", json_num(a, 1)); /* Error */
            json_object_set(a, diag, "source", json_str(a, "fcc"));
            json_object_set(a, diag, "message", json_str(a, d->message));
            json_array_push(a, arr, diag);
        }
    }

    JsonValue *params = json_object(a);
    json_object_set(a, params, "uri", json_str(a, doc->uri));
    json_object_set(a, params, "diagnostics", arr);
    lsp_notify(a, "textDocument/publishDiagnostics", params);
}

/* Collect sibling `.fc` files in the same directory as `doc_path` (excluding it).
 * Returns malloc'd path strings in a malloc'd array. Non-recursive on purpose:
 * recursing a workspace would pull in unrelated programs (test files, examples,
 * multiple `let main`s) and produce duplicate-symbol noise. For a flat project
 * (e.g. main.fc + prelude.fc) this is the whole compilation unit. */
static void collect_sibling_fc(const char *doc_path, char ***out, int *count, int *cap) {
#if defined(_WIN32)
    (void)doc_path; (void)out; (void)count; (void)cap;
#else
    const char *slash = strrchr(doc_path, '/');
    if (!slash) return;
    int dlen = (int)(slash - doc_path);
    char dir[4096];
    if (dlen <= 0 || dlen >= (int)sizeof dir) return;
    memcpy(dir, doc_path, (size_t)dlen);
    dir[dlen] = '\0';

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 4 || strcmp(nm + l - 3, ".fc") != 0) continue;
        char full[4096];
        if (snprintf(full, sizeof full, "%s/%s", dir, nm) >= (int)sizeof full) continue;
        if (strcmp(full, doc_path) == 0) continue;          /* the open file itself */
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) continue;
        DA_APPEND(*out, *count, *cap, dup_cstr(full));
    }
    closedir(d);
#endif
}

/* (Re)run analysis for a document and publish diagnostics. The compilation unit
 * is the open file + its sibling .fc files (open-buffer text preferred over
 * disk) + the stdlib feed. This makes cross-file references (a shared `prelude`
 * module, `let main` in another file) resolve. */
static void analyze_doc(LspServer *S, LspDoc *doc) {
    if (doc->result) { analysis_free(doc->result); doc->result = NULL; }

    AnalysisSource *extra = NULL;
    int n = 0, cap = 0;
    char **disk_bufs = NULL;    /* text buffers read from disk, freed after analyze */
    int db = 0, dbcap = 0;
    char **sibs = NULL;
    int sc = 0, scap = 0;

    collect_sibling_fc(doc->path, &sibs, &sc, &scap);
    for (int i = 0; i < sc; i++) {
        LspDoc *od = store_find_by_path(&S->store, sibs[i]);
        if (od == doc) continue;
        if (od) {                                   /* open in the editor: live text */
            AnalysisSource s = { od->path, od->text, od->text_len };
            DA_APPEND(extra, n, cap, s);
        } else {                                    /* on disk */
            int len;
            char *buf = read_whole_file(sibs[i], &len);
            if (!buf) continue;
            AnalysisSource s = { sibs[i], buf, len };
            DA_APPEND(extra, n, cap, s);
            DA_APPEND(disk_bufs, db, dbcap, buf);
        }
    }

    /* stdlib feed (skip any already present by path, e.g. when editing in the
     * stdlib directory itself). */
    for (int i = 0; i < S->stdlib_count; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++)
            if (strcmp(extra[j].filename, S->stdlib[i].filename) == 0) { dup = true; break; }
        if (!dup) DA_APPEND(extra, n, cap, S->stdlib[i]);
    }

    doc->result = analyze(doc->text, doc->text_len, doc->path, extra, n);

    for (int i = 0; i < db; i++) free(disk_bufs[i]);
    free(disk_bufs);
    for (int i = 0; i < sc; i++) free(sibs[i]);
    free(sibs);
    free(extra);

    publish_diagnostics(S, doc);
}

/* ======================================================================== */
/* Handlers: text sync                                                       */
/* ======================================================================== */

static void handle_did_open(LspServer *S, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    const char *text = json_get_str(td, "text");
    if (!uri || !text) return;

    LspDoc *doc = store_find(&S->store, uri);
    if (!doc) {
        LspDoc nd = {0};
        nd.uri = dup_cstr(uri);
        nd.path = uri_to_path(uri);
        DA_APPEND(S->store.docs, S->store.count, S->store.cap, nd);
        doc = &S->store.docs[S->store.count - 1];
    } else {
        free(doc->text);
    }
    JsonValue *tdv = json_get(td, "version");
    doc->version = (tdv && tdv->kind == JSON_NUMBER) ? (long)tdv->num : 0;
    JsonValue *tv = json_get(td, "text");
    doc->text_len = (tv && tv->kind == JSON_STRING) ? tv->str.len : (int)strlen(text);
    doc->text = malloc((size_t)doc->text_len + 1);
    memcpy(doc->text, text, (size_t)doc->text_len);
    doc->text[doc->text_len] = '\0';

    analyze_doc(S, doc);
}

static void handle_did_change(LspServer *S, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    if (!uri) return;
    LspDoc *doc = store_find(&S->store, uri);
    if (!doc) return;

    /* Full-document sync: take the last content change's full text. */
    JsonValue *changes = json_get(params, "contentChanges");
    int nch = json_array_len(changes);
    if (nch <= 0) return;
    JsonValue *last = json_index(changes, nch - 1);
    JsonValue *tv = json_get(last, "text");
    if (!tv || tv->kind != JSON_STRING) return;

    free(doc->text);
    doc->text_len = tv->str.len;
    doc->text = malloc((size_t)doc->text_len + 1);
    memcpy(doc->text, tv->str.s, (size_t)doc->text_len);
    doc->text[doc->text_len] = '\0';

    JsonValue *vv = json_get(td, "version");
    if (vv && vv->kind == JSON_NUMBER) doc->version = (long)vv->num;

    analyze_doc(S, doc);
}

static void handle_did_close(LspServer *S, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    if (!uri) return;
    for (int i = 0; i < S->store.count; i++) {
        if (strcmp(S->store.docs[i].uri, uri) == 0) {
            /* Clear diagnostics for the closed file. */
            JsonValue *params2 = json_object(&S->msg_arena);
            json_object_set(&S->msg_arena, params2, "uri", json_str(&S->msg_arena, uri));
            json_object_set(&S->msg_arena, params2, "diagnostics", json_array(&S->msg_arena));
            lsp_notify(&S->msg_arena, "textDocument/publishDiagnostics", params2);

            LspDoc *d = &S->store.docs[i];
            if (d->result) analysis_free(d->result);
            free(d->uri); free(d->path); free(d->text);
            S->store.docs[i] = S->store.docs[--S->store.count];
            return;
        }
    }
}

/* ======================================================================== */
/* Handlers: hover / definition                                             */
/* ======================================================================== */

static void handle_hover(LspServer *S, JsonValue *id, JsonValue *params) {
    Arena *a = &S->msg_arena;
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    LspDoc *doc = uri ? store_find(&S->store, uri) : NULL;
    long line = 0, ch = 0;
    JsonValue *pos = json_get(params, "position");
    json_get_int(pos, "line", &line);
    json_get_int(pos, "character", &ch);

    if (!doc) { lsp_reply(a, id, json_null(a)); return; }
    LineIndex idx = line_index_build(a, doc->text, doc->text_len);
    FindCtx hit;
    if (!locate(doc, &idx, (int)line, (int)ch, &hit) || !hit.type) {
        lsp_reply(a, id, json_null(a));
        return;
    }

    char tn[512];
    copy_type_name(hit.type, tn, sizeof tn);
    char md[640];
    snprintf(md, sizeof md, "```fc\n%s: %s\n```",
             hit.name ? hit.name : "", tn);

    int sl, sc, el, ec;
    loc_to_lsp(&idx, doc->text, hit.start_line, hit.start_col, &sl, &sc);
    loc_to_lsp(&idx, doc->text, hit.start_line, hit.start_col + hit.best_span, &el, &ec);

    JsonValue *contents = json_object(a);
    json_object_set(a, contents, "kind", json_str(a, "markdown"));
    json_object_set(a, contents, "value", json_str(a, md));
    JsonValue *res = json_object(a);
    json_object_set(a, res, "contents", contents);
    json_object_set(a, res, "range", mk_range(a, sl, sc, el, ec));
    lsp_reply(a, id, res);
}

static void handle_definition(LspServer *S, JsonValue *id, JsonValue *params) {
    Arena *a = &S->msg_arena;
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    LspDoc *doc = uri ? store_find(&S->store, uri) : NULL;
    long line = 0, ch = 0;
    JsonValue *pos = json_get(params, "position");
    json_get_int(pos, "line", &line);
    json_get_int(pos, "character", &ch);

    if (!doc) { lsp_reply(a, id, json_null(a)); return; }
    LineIndex idx = line_index_build(a, doc->text, doc->text_len);
    FindCtx hit;
    if (!locate(doc, &idx, (int)line, (int)ch, &hit) ||
        !hit.sym || !hit.sym->decl) {
        lsp_reply(a, id, json_null(a));   /* locals have no resolved_sym in v1 */
        return;
    }

    SrcLoc dl = hit.sym->decl->loc;
    const char *def_path = dl.filename ? dl.filename : doc->path;
    int dline = dl.line > 0 ? dl.line : 1;
    int dcol  = dl.col  > 0 ? dl.col  : 1;

    /* Map the definition location through the right file's text: same file uses
     * the live doc text; other files (stdlib) fall back to a trivial byte=col
     * mapping (good enough — those are ASCII). */
    int dl0, dc0;
    if (strcmp(def_path, doc->path) == 0) {
        loc_to_lsp(&idx, doc->text, dline, dcol, &dl0, &dc0);
    } else {
        dl0 = dline - 1;
        dc0 = dcol - 1;
    }

    /* Build a file:// uri for the definition path. */
    char def_uri[2048];
    if (strcmp(def_path, doc->path) == 0)
        snprintf(def_uri, sizeof def_uri, "%s", doc->uri);
    else
        snprintf(def_uri, sizeof def_uri, "file://%s", def_path);

    JsonValue *loc = json_object(a);
    json_object_set(a, loc, "uri", json_str(a, def_uri));
    json_object_set(a, loc, "range", mk_range(a, dl0, dc0,
                    dl0, dc0 + (hit.name ? (int)strlen(hit.name) : 1)));
    lsp_reply(a, id, loc);
}

/* ======================================================================== */
/* Handler: CodeLens (inferred type shown above each binding)               */
/* ======================================================================== */

typedef struct {
    Arena *a;
    JsonValue *arr;
    const LineIndex *idx;
    const char *src;
    const char *file;   /* only emit lenses for decls from this file */
} LensCtx;

static void lens_emit(LensCtx *lc, int let_line, int let_col, bool is_mut, Type *type) {
    if (!type) return;
    char tn[512];
    copy_type_name(type, tn, sizeof tn);
    char title[560];
    snprintf(title, sizeof title, ": %s", tn);

    FindCtx fc = {0};
    fc.idx = lc->idx;
    fc.src = lc->src;
    int name_col = let_name_col(&fc, let_line, let_col, is_mut);

    int l0, c0;
    loc_to_lsp(lc->idx, lc->src, let_line, name_col, &l0, &c0);

    JsonValue *lens = json_object(lc->a);
    /* Range on the binding line: VSCode renders the lens on the line above. */
    json_object_set(lc->a, lens, "range", mk_range(lc->a, l0, c0, l0, c0));
    JsonValue *cmd = json_object(lc->a);
    json_object_set(lc->a, cmd, "title", json_str(lc->a, title));
    json_object_set(lc->a, cmd, "command", json_str(lc->a, ""));
    json_object_set(lc->a, lens, "command", cmd);
    json_array_push(lc->a, lc->arr, lens);
}

static void lens_expr(Expr *e, LensCtx *lc);
static void lens_exprs(Expr **arr, int n, LensCtx *lc) {
    for (int i = 0; i < n; i++) lens_expr(arr[i], lc);
}

static void lens_expr(Expr *e, LensCtx *lc) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_LET:
            lens_emit(lc, e->loc.line, e->loc.col, e->let_expr.let_is_mut,
                      e->let_expr.let_type);
            lens_expr(e->let_expr.let_init, lc);
            break;
        case EXPR_BINARY: lens_expr(e->binary.left, lc); lens_expr(e->binary.right, lc); break;
        case EXPR_UNARY_PREFIX:  lens_expr(e->unary_prefix.operand, lc); break;
        case EXPR_UNARY_POSTFIX: lens_expr(e->unary_postfix.operand, lc); break;
        case EXPR_CALL:
            lens_expr(e->call.func, lc);
            lens_exprs(e->call.args, e->call.arg_count, lc);
            break;
        case EXPR_FIELD: case EXPR_DEREF_FIELD: lens_expr(e->field.object, lc); break;
        case EXPR_INDEX: lens_expr(e->index.object, lc); lens_expr(e->index.index, lc); break;
        case EXPR_SLICE:
            lens_expr(e->slice.object, lc); lens_expr(e->slice.lo, lc); lens_expr(e->slice.hi, lc);
            break;
        case EXPR_CAST: lens_expr(e->cast.operand, lc); break;
        case EXPR_IF:
            lens_expr(e->if_expr.cond, lc);
            lens_expr(e->if_expr.then_body, lc);
            lens_expr(e->if_expr.else_body, lc);
            break;
        case EXPR_MATCH:
            lens_expr(e->match_expr.subject, lc);
            for (int i = 0; i < e->match_expr.arm_count; i++) {
                lens_expr(e->match_expr.arms[i].guard, lc);
                lens_exprs(e->match_expr.arms[i].body, e->match_expr.arms[i].body_count, lc);
            }
            break;
        case EXPR_LOOP: lens_exprs(e->loop_expr.body, e->loop_expr.body_count, lc); break;
        case EXPR_FOR:
            lens_expr(e->for_expr.iter, lc);
            lens_expr(e->for_expr.range_end, lc);
            lens_exprs(e->for_expr.body, e->for_expr.body_count, lc);
            break;
        case EXPR_BREAK:  lens_expr(e->break_expr.value, lc); break;
        case EXPR_RETURN: lens_expr(e->return_expr.value, lc); break;
        case EXPR_BLOCK:  lens_exprs(e->block.stmts, e->block.count, lc); break;
        case EXPR_FUNC:   lens_exprs(e->func.body, e->func.body_count, lc); break;
        case EXPR_ASSIGN: lens_expr(e->assign.target, lc); lens_expr(e->assign.value, lc); break;
        case EXPR_SOME:   lens_expr(e->some_expr.value, lc); break;
        case EXPR_DEFER:  lens_expr(e->defer_expr.value, lc); break;
        case EXPR_GUARD:  lens_expr(e->guard.body, lc); break;
        case EXPR_ASSERT:
            lens_expr(e->assert_expr.condition, lc);
            lens_expr(e->assert_expr.message, lc);
            break;
        default: break;
    }
}

static void lens_decls(Decl **decls, int n, LensCtx *lc) {
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        if (!d) continue;
        /* skip decls merged in from stdlib / other files */
        if (lc->file && d->loc.filename && strcmp(d->loc.filename, lc->file) != 0) continue;
        if (d->kind == DECL_LET) {
            lens_emit(lc, d->loc.line, d->loc.col, d->let.is_mut, d->let.resolved_type);
            lens_expr(d->let.init, lc);
        } else if (d->kind == DECL_MODULE) {
            lens_decls(d->module.decls, d->module.decl_count, lc);
        }
    }
}

static void handle_codelens(LspServer *S, JsonValue *id, JsonValue *params) {
    Arena *a = &S->msg_arena;
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    LspDoc *doc = uri ? store_find(&S->store, uri) : NULL;
    JsonValue *arr = json_array(a);
    if (doc && doc->result && doc->result->program) {
        LineIndex idx = line_index_build(a, doc->text, doc->text_len);
        LensCtx lc = { .a = a, .arr = arr, .idx = &idx, .src = doc->text,
                       .file = doc->path };
        lens_decls(doc->result->program->decls,
                   doc->result->program->decl_count, &lc);
    }
    lsp_reply(a, id, arr);
}

/* ======================================================================== */
/* Handler: completion                                                       */
/* ======================================================================== */

static const char *KEYWORDS[] = {
    /* mirrors lexer.c keyword set + builtins + primitive type names */
    "let", "mut", "struct", "union", "module", "namespace", "import", "from",
    "as", "extern", "private", "match", "with", "when", "if", "then", "else",
    "for", "in", "loop", "do", "break", "continue", "return", "defer", "some",
    "true", "false", "none", "void", "guarded", "unguarded", "checked",
    "unchecked", "alloc", "alloca", "free", "sizeof", "alignof", "default",
    "const", "assert", "atomic_load_acquire", "atomic_store_release",
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
    "isize", "usize", "float32", "float64", "bool", "char", "str", "cstr", "any",
};
static const int KEYWORD_COUNT = (int)(sizeof KEYWORDS / sizeof KEYWORDS[0]);

/* LSP CompletionItemKind values */
enum { CIK_TEXT = 1, CIK_METHOD = 2, CIK_FUNCTION = 3, CIK_FIELD = 5,
       CIK_VARIABLE = 6, CIK_MODULE = 9, CIK_ENUM = 13, CIK_KEYWORD = 14,
       CIK_STRUCT = 22, CIK_ENUMMEMBER = 20 };

static void add_item(Arena *a, JsonValue *arr, const char *label, int kind,
                     const char *detail) {
    JsonValue *it = json_object(a);
    json_object_set(a, it, "label", json_str(a, label));
    json_object_set(a, it, "kind", json_num(a, kind));
    if (detail) json_object_set(a, it, "detail", json_str(a, detail));
    json_array_push(a, arr, it);
}

static int sym_kind_to_cik(const Symbol *s) {
    switch (s->kind) {
        case DECL_MODULE: return CIK_MODULE;
        case DECL_STRUCT: return CIK_STRUCT;
        case DECL_UNION:  return CIK_ENUM;
        case DECL_LET:
            return (s->type && s->type->kind == TYPE_FUNC) ? CIK_FUNCTION : CIK_VARIABLE;
        default:          return CIK_VARIABLE;
    }
}

/* Peel option/pointer one level to reach a struct/union for member access. */
static Type *peel_to_aggregate(Type *t) {
    for (int i = 0; t && i < 4; i++) {
        if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION) return t;
        if (t->kind == TYPE_OPTION)  { t = t->option.inner; continue; }
        if (t->kind == TYPE_POINTER) { t = t->pointer.pointee; continue; }
        break;
    }
    return t;
}

/* Harvest interned identifier names from the open file (dedup by pointer). */
static void harvest_expr(Expr *e, const char ***names, int *n, int *cap);
static void harvest_exprs(Expr **arr, int k, const char ***names, int *n, int *cap) {
    for (int i = 0; i < k; i++) harvest_expr(arr[i], names, n, cap);
}
static void harvest_add(const char *name, const char ***names, int *n, int *cap) {
    if (!name) return;
    for (int i = 0; i < *n; i++) if ((*names)[i] == name) return;  /* interned */
    DA_APPEND(*names, *n, *cap, name);
}
static void harvest_expr(Expr *e, const char ***names, int *n, int *cap) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_IDENT: harvest_add(e->ident.name, names, n, cap); break;
        case EXPR_LET:
            harvest_add(e->let_expr.let_name, names, n, cap);
            harvest_expr(e->let_expr.let_init, names, n, cap);
            break;
        case EXPR_BINARY: harvest_expr(e->binary.left, names, n, cap); harvest_expr(e->binary.right, names, n, cap); break;
        case EXPR_UNARY_PREFIX: harvest_expr(e->unary_prefix.operand, names, n, cap); break;
        case EXPR_UNARY_POSTFIX: harvest_expr(e->unary_postfix.operand, names, n, cap); break;
        case EXPR_CALL: harvest_expr(e->call.func, names, n, cap); harvest_exprs(e->call.args, e->call.arg_count, names, n, cap); break;
        case EXPR_FIELD: case EXPR_DEREF_FIELD: harvest_expr(e->field.object, names, n, cap); break;
        case EXPR_INDEX: harvest_expr(e->index.object, names, n, cap); harvest_expr(e->index.index, names, n, cap); break;
        case EXPR_SLICE: harvest_expr(e->slice.object, names, n, cap); harvest_expr(e->slice.lo, names, n, cap); harvest_expr(e->slice.hi, names, n, cap); break;
        case EXPR_CAST: harvest_expr(e->cast.operand, names, n, cap); break;
        case EXPR_IF: harvest_expr(e->if_expr.cond, names, n, cap); harvest_expr(e->if_expr.then_body, names, n, cap); harvest_expr(e->if_expr.else_body, names, n, cap); break;
        case EXPR_MATCH:
            harvest_expr(e->match_expr.subject, names, n, cap);
            for (int i = 0; i < e->match_expr.arm_count; i++)
                harvest_exprs(e->match_expr.arms[i].body, e->match_expr.arms[i].body_count, names, n, cap);
            break;
        case EXPR_LOOP: harvest_exprs(e->loop_expr.body, e->loop_expr.body_count, names, n, cap); break;
        case EXPR_FOR: harvest_expr(e->for_expr.iter, names, n, cap); harvest_exprs(e->for_expr.body, e->for_expr.body_count, names, n, cap); break;
        case EXPR_BREAK: harvest_expr(e->break_expr.value, names, n, cap); break;
        case EXPR_RETURN: harvest_expr(e->return_expr.value, names, n, cap); break;
        case EXPR_BLOCK: harvest_exprs(e->block.stmts, e->block.count, names, n, cap); break;
        case EXPR_FUNC:
            for (int i = 0; i < e->func.param_count; i++)
                harvest_add(e->func.params[i].name, names, n, cap);
            harvest_exprs(e->func.body, e->func.body_count, names, n, cap);
            break;
        case EXPR_ASSIGN: harvest_expr(e->assign.target, names, n, cap); harvest_expr(e->assign.value, names, n, cap); break;
        case EXPR_SOME: harvest_expr(e->some_expr.value, names, n, cap); break;
        case EXPR_DEFER: harvest_expr(e->defer_expr.value, names, n, cap); break;
        case EXPR_GUARD: harvest_expr(e->guard.body, names, n, cap); break;
        default: break;
    }
}

/* Member completion after '.' / '::': resolve the node just left of the dot. */
static bool complete_members(LspServer *S, LspDoc *doc, const LineIndex *idx,
                             int dot_byte, JsonValue *arr) {
    Arena *a = &S->msg_arena;
    /* The identifier/expression ends at dot_byte-1. Locate it. */
    int line1 = 1, col1 = 1;
    /* derive line/col from byte: find line via the index */
    int anchor = dot_byte - 1;
    if (anchor < 0) return false;
    int line0 = 0;
    for (int i = 0; i < idx->count; i++)
        if (idx->starts[i] <= anchor) line0 = i; else break;
    line1 = line0 + 1;
    col1 = anchor - idx->starts[line0] + 1;

    FindCtx c = {0};
    c.target_line = line1;
    c.target_col = col1;
    c.src = doc->text;
    c.idx = idx;
    c.file = doc->path;
    if (!doc->result || !doc->result->program) return false;
    find_in_decls(doc->result->program->decls,
                  doc->result->program->decl_count, &c);
    if (!c.found) return false;

    /* Module members. */
    if (c.sym && c.sym->kind == DECL_MODULE && c.sym->members) {
        SymbolTable *m = c.sym->members;
        for (int i = 0; i < m->count; i++) {
            if (m->symbols[i].is_private) continue;
            char detail[512];
            detail[0] = '\0';
            if (m->symbols[i].type) copy_type_name(m->symbols[i].type, detail, sizeof detail);
            add_item(a, arr, m->symbols[i].name, sym_kind_to_cik(&m->symbols[i]),
                     detail[0] ? detail : NULL);
        }
        return true;
    }

    /* Struct fields / union variants. */
    Type *t = peel_to_aggregate(c.type);
    if (t && t->kind == TYPE_STRUCT) {
        for (int i = 0; i < t->struc.field_count; i++) {
            char detail[512];
            copy_type_name(t->struc.fields[i].type, detail, sizeof detail);
            add_item(a, arr, t->struc.fields[i].name, CIK_FIELD, detail);
        }
        return true;
    }
    if (t && t->kind == TYPE_UNION) {
        for (int i = 0; i < t->unio.variant_count; i++)
            add_item(a, arr, t->unio.variants[i].name, CIK_ENUMMEMBER, NULL);
        return true;
    }
    return false;
}

static void handle_completion(LspServer *S, JsonValue *id, JsonValue *params) {
    Arena *a = &S->msg_arena;
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    LspDoc *doc = uri ? store_find(&S->store, uri) : NULL;
    long line = 0, ch = 0;
    JsonValue *pos = json_get(params, "position");
    json_get_int(pos, "line", &line);
    json_get_int(pos, "character", &ch);

    JsonValue *items = json_array(a);
    if (!doc) { lsp_reply(a, id, items); return; }

    LineIndex idx = line_index_build(a, doc->text, doc->text_len);
    int line1, col1;
    lsp_to_loc(&idx, doc->text, (int)line, (int)ch, &line1, &col1);
    int cur = loc_byte_offset(&idx, line1, col1);

    /* Member context: the char immediately before the cursor is '.' (or the
     * trigger char preceding an in-progress member name). Scan back over an
     * in-progress identifier to find a '.' or '::'. */
    int b = cur - 1;
    while (b >= 0 && (isalnum((unsigned char)doc->text[b]) || doc->text[b] == '_')) b--;
    bool member = false;
    int dot_byte = -1;
    if (b >= 0 && doc->text[b] == '.') { member = true; dot_byte = b; }
    else if (b >= 1 && doc->text[b] == ':' && doc->text[b - 1] == ':') {
        member = true; dot_byte = b - 1;
    }

    if (member && complete_members(S, doc, &idx, dot_byte, items)) {
        lsp_reply(a, id, items);
        return;
    }

    /* Global context: keywords + top-level symbols + file identifiers. */
    for (int i = 0; i < KEYWORD_COUNT; i++)
        add_item(a, items, KEYWORDS[i], CIK_KEYWORD, NULL);

    if (doc->result) {
        SymbolTable *st = &doc->result->symtab;
        for (int i = 0; i < st->count; i++) {
            if (st->symbols[i].is_private) continue;
            if (!st->symbols[i].name) continue;
            char detail[512]; detail[0] = '\0';
            if (st->symbols[i].type) copy_type_name(st->symbols[i].type, detail, sizeof detail);
            add_item(a, items, st->symbols[i].name, sym_kind_to_cik(&st->symbols[i]),
                     detail[0] ? detail : NULL);
        }
        if (doc->result->program) {
            const char **names = NULL; int n = 0, cap = 0;
            for (int i = 0; i < doc->result->program->decl_count; i++) {
                Decl *d = doc->result->program->decls[i];
                if (!d || d->kind != DECL_LET) continue;
                if (d->loc.filename && strcmp(d->loc.filename, doc->path) != 0) continue;
                harvest_expr(d->let.init, &names, &n, &cap);
            }
            for (int i = 0; i < n; i++) add_item(a, items, names[i], CIK_VARIABLE, NULL);
            free(names);
        }
    }

    JsonValue *list = json_object(a);
    json_object_set(a, list, "isIncomplete", json_bool(a, false));
    json_object_set(a, list, "items", items);
    lsp_reply(a, id, list);
}

/* ======================================================================== */
/* initialize                                                                */
/* ======================================================================== */

static void handle_initialize(LspServer *S, JsonValue *id) {
    Arena *a = &S->msg_arena;
    JsonValue *caps = json_object(a);
    json_object_set(a, caps, "textDocumentSync", json_num(a, 1)); /* Full */
    json_object_set(a, caps, "hoverProvider", json_bool(a, true));
    json_object_set(a, caps, "definitionProvider", json_bool(a, true));
    JsonValue *cl = json_object(a);
    json_object_set(a, cl, "resolveProvider", json_bool(a, false));
    json_object_set(a, caps, "codeLensProvider", cl);
    JsonValue *comp = json_object(a);
    JsonValue *trig = json_array(a);
    json_array_push(a, trig, json_str(a, "."));
    json_array_push(a, trig, json_str(a, ":"));
    json_object_set(a, comp, "triggerCharacters", trig);
    json_object_set(a, caps, "completionProvider", comp);

    JsonValue *info = json_object(a);
    json_object_set(a, info, "name", json_str(a, "fcc-lsp"));
    json_object_set(a, info, "version", json_str(a, fcc_version_string()));

    JsonValue *res = json_object(a);
    json_object_set(a, res, "capabilities", caps);
    json_object_set(a, res, "serverInfo", info);
    lsp_reply(a, id, res);
    S->initialized = true;
}

/* ======================================================================== */
/* Dispatch                                                                  */
/* ======================================================================== */

static void dispatch(LspServer *S, JsonValue *req) {
    const char *method = json_get_str(req, "method");
    if (!method) return;
    JsonValue *id = json_get(req, "id");
    JsonValue *params = json_get(req, "params");
    bool is_request = (id != NULL);

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(S, id);
    } else if (strcmp(method, "initialized") == 0) {
        /* notification, no-op */
    } else if (strcmp(method, "shutdown") == 0) {
        S->shutdown_requested = true;
        lsp_reply(&S->msg_arena, id, json_null(&S->msg_arena));
    } else if (strcmp(method, "exit") == 0) {
        /* handled by the loop */
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(S, params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(S, params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(S, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(S, id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(S, id, params);
    } else if (strcmp(method, "textDocument/codeLens") == 0) {
        handle_codelens(S, id, params);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(S, id, params);
    } else if (is_request) {
        lsp_reply_error(&S->msg_arena, id, -32601, "method not found");
    }
    /* unknown notifications are ignored */
}

/* ======================================================================== */
/* stdlib discovery                                                          */
/* ======================================================================== */

static char *read_whole_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    *out_len = (int)rd;
    return buf;
}

/* Load every `.fc` file in a directory into the stdlib feed. Returns true if it
 * loaded at least one. Scans the directory rather than a fixed name list so new
 * stdlib modules (e.g. random) are picked up automatically. */
static bool load_stdlib_from_dir(LspServer *S, const char *dir) {
#if defined(_WIN32)
    (void)S; (void)dir;
    return false;
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    bool any = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 4 || strcmp(nm + l - 3, ".fc") != 0) continue;
        char full[4096];
        if (snprintf(full, sizeof full, "%s/%s", dir, nm) >= (int)sizeof full) continue;
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) continue;
        int len;
        char *buf = read_whole_file(full, &len);
        if (!buf) continue;
        AnalysisSource src = { dup_cstr(full), buf, len };
        DA_APPEND(S->stdlib, S->stdlib_count, S->stdlib_cap, src);
        any = true;
    }
    closedir(d);
    return any;
#endif
}

/* Load the standard library once so std:: imports resolve. The directory is
 * FCC_STDLIB_DIR if set, else the install datadir, else a repo-relative
 * ./stdlib. Missing stdlib is not fatal — analysis still works for files that
 * do not import std::. */
static void load_stdlib(LspServer *S) {
    const char *env = getenv("FCC_STDLIB_DIR");
    if (env && *env && load_stdlib_from_dir(S, env)) return;
#ifdef FCC_DATADIR
    if (load_stdlib_from_dir(S, FCC_DATADIR "/fcc/stdlib")) return;
#endif
    load_stdlib_from_dir(S, "stdlib");
}

/* ======================================================================== */
/* Message loop                                                              */
/* ======================================================================== */

static bool ci_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
        s++; prefix++;
    }
    return true;
}

/* Read headers up to the blank separator. Returns false at EOF. */
static bool read_headers(long *out_len) {
    char line[1024];
    *out_len = -1;
    bool saw_any = false;
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) return saw_any;              /* blank line ends headers */
        saw_any = true;
        if (ci_prefix(line, "content-length:"))
            *out_len = strtol(line + 15, NULL, 10);
    }
    return false;
}

int lsp_main(void) {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    LspServer S = {0};
    arena_init(&S.msg_arena);
    load_stdlib(&S);

    bool exiting = false;
    for (;;) {
        long content_length;
        if (!read_headers(&content_length)) break;       /* EOF */
        if (content_length < 0) continue;                /* malformed header set */

        char *body = malloc((size_t)content_length + 1);
        size_t got = fread(body, 1, (size_t)content_length, stdin);
        body[got] = '\0';
        if (got != (size_t)content_length) { free(body); break; }

        /* Reset the per-message arena (frees previous request + response trees). */
        arena_free(&S.msg_arena);
        arena_init(&S.msg_arena);

        JsonValue *req = json_parse(&S.msg_arena, body, (int)content_length);
        free(body);
        if (req) {
            const char *method = json_get_str(req, "method");
            dispatch(&S, req);
            if (method && strcmp(method, "exit") == 0) { exiting = true; break; }
        }
    }

    /* Cleanup */
    for (int i = 0; i < S.store.count; i++) {
        if (S.store.docs[i].result) analysis_free(S.store.docs[i].result);
        free(S.store.docs[i].uri);
        free(S.store.docs[i].path);
        free(S.store.docs[i].text);
    }
    free(S.store.docs);
    for (int i = 0; i < S.stdlib_count; i++) {
        free((void *)S.stdlib[i].filename);
        free((void *)S.stdlib[i].text);
    }
    free(S.stdlib);
    arena_free(&S.msg_arena);

    /* A clean shutdown+exit returns 0; an exit without prior shutdown is 1. */
    return (exiting && S.shutdown_requested) ? 0 : (exiting ? 1 : 0);
}
