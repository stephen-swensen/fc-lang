/* Expose POSIX/BSD extensions (notably realpath) under -std=c11, which would
 * otherwise hide them via __STRICT_ANSI__. Must precede every #include. */
#define _DEFAULT_SOURCE

#include "lsp.h"
#include "json.h"
#include "analyze.h"
#include "ast.h"
#include "pass1.h"
#include "types.h"
#include "token.h"
#include "version.h"
#include "args.h"
#include "platform.h"
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
#include <unistd.h>
#include <poll.h>
#endif

/* ======================================================================== */
/* Document store                                                           */
/* ======================================================================== */

typedef struct {
    char           *uri;        /* owned */
    char           *path;       /* owned: filesystem path decoded from uri */
    char           *real;       /* owned: canonical `path` (see canon_path), so this
                                 * document is recognized under any spelling another
                                 * file uses for it (an lsp.rsp route like
                                 * `../shared/lib.fc`, a symlink). Refreshed each
                                 * flush, since the file may not have existed at
                                 * didOpen. */
    char           *text;       /* owned: current full document text (UTF-8) */
    int             text_len;
    bool            dirty;      /* text changed since last analysis. didOpen/didChange
                                 * only set this; analysis is deferred so a burst of
                                 * keystrokes coalesces into one analyze (see lsp_main). */
    char           *unit_key;   /* owned identity of this doc's compilation unit (see
                                 * unit_key()); the analysis is owned by the matching
                                 * UnitEntry, shared by every doc with the same key.
                                 * Recomputed each flush; NULL until the first. */
} LspDoc;

typedef struct {
    LspDoc *docs;
    int count, cap;
} LspDocStore;

/* One shared analysis per compilation unit. Every open document whose unit_key
 * matches is served from this single result, so a unit is analyzed once per flush
 * however many of its files are open (the analysis already merges every open
 * doc's live buffer). Owns its analyses; keyed by unit_key(). */
typedef struct {
    char           *key;        /* owned: the unit identity (see unit_key()) */
    char          **files;      /* owned: canonical path of every source the last
                                 * analysis merged (primary + lsp.rsp inputs /
                                 * siblings / stdlib feed). An lsp.rsp reaches
                                 * across directories, so a doc can be a member of
                                 * a unit without bearing its key; this list is how
                                 * flush_dirty knows that editing that doc
                                 * invalidates this unit. Rebuilt by each
                                 * analyze_unit. */
    int             file_count, file_cap;
    AnalysisResult *result;     /* latest analysis; NULL until first analyze */
    AnalysisResult *last_good;  /* most recent analysis that type-checked (result
                                 * itself when fresh is good), retained so a lexer
                                 * abort mid-typing (a stray tab, an unterminated
                                 * string) does not blank type-aware queries. May
                                 * alias result. NULL until the first good
                                 * analysis. */
} UnitEntry;

typedef struct {
    LspDocStore store;
    bool shutdown_requested;
    Arena msg_arena;            /* reset per message: holds request + response */

    /* stdlib sources, read once at startup and merged into every analysis that
     * has no lsp.rsp, so `import ... from std::...` resolves. Each entry's
     * `text`/`filename` are owned here (malloc'd) and live for the whole
     * session. */
    AnalysisSource *stdlib;
    int             stdlib_count, stdlib_cap;

    /* Session-scoped lex cache: feed (stdlib + sibling/lsp.rsp) token arrays,
     * reused across analyses so unchanged sources aren't re-lexed each keystroke. */
    LexCache        lex_cache;

    /* One analysis per compilation unit, shared by all its open documents (see
     * UnitEntry). Editing a file re-analyzes each unit it belongs to, once per
     * flush rather than once per open tab. */
    UnitEntry      *units;
    int             unit_count, unit_cap;

    /* URIs we last published non-empty diagnostics to (owned). A project file
     * that later goes clean, drops out of the unit, or is closed must be cleared
     * with an explicit empty publish; each cycle diffs against this set to find
     * the URIs that need clearing. Open documents are republished every cycle
     * regardless. */
    char          **pub_uris;
    int             pub_count, pub_cap;
} LspServer;

static char *canon_path(const char *path);

static LspDoc *store_find(LspDocStore *s, const char *uri) {
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
    return NULL;
}

/* The open document for `path`, under whatever spelling names it. The exact
 * string is tried first, then canonical paths (each doc caches its own): an
 * lsp.rsp spells a unit's files file-relative (`../shared/lib.fc`), while the
 * editor opens the same file by its resolved path. Without the canonical match
 * the analysis would read the file's saved copy instead of the live buffer, and
 * its diagnostics would go to a synthesized URI beside the real document's. */
static LspDoc *store_find_by_path(LspDocStore *s, const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < s->count; i++)
        if (s->docs[i].path && strcmp(s->docs[i].path, path) == 0) return &s->docs[i];
    char *real = canon_path(path);
    LspDoc *hit = NULL;
    for (int i = 0; i < s->count; i++)
        if (s->docs[i].real && strcmp(s->docs[i].real, real) == 0) { hit = &s->docs[i]; break; }
    free(real);
    return hit;
}

static UnitEntry *unit_find(LspServer *S, const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < S->unit_count; i++)
        if (strcmp(S->units[i].key, key) == 0) return &S->units[i];
    return NULL;
}

/* The analysis that answers type-aware queries (hover, definition, completion,
 * CodeLens) for `doc`: its unit's fresh result whenever it type-checked,
 * otherwise the last one that did. The fresh result fails to type-check only
 * when the lexer aborted (a stray tab, an unterminated string); serving the
 * older result keeps overlays from blanking while the user types through that
 * state. Diagnostics do not use this; they always come from the fresh
 * `result`, so squiggles stay live. The stale AST carries positions from an
 * earlier revision and consumers locate by current coordinates, so an
 * untouched line still resolves while the line under edit may miss. May
 * return NULL before the first good analysis. */
static AnalysisResult *query_result(LspServer *S, LspDoc *doc) {
    UnitEntry *u = unit_find(S, doc->unit_key);
    if (!u) return NULL;
    if (u->result && u->result->typed) return u->result;
    return u->last_good;
}

/* Free both analyses a unit may own. result and last_good can alias (when the
 * freshest analysis is the good one), so free the distinct one first. */
static void unit_free_results(UnitEntry *u) {
    if (u->last_good && u->last_good != u->result) analysis_free(u->last_good);
    if (u->result) analysis_free(u->result);
    u->result = NULL;
    u->last_good = NULL;
}

/* Drop the recorded member set (rebuilt by the next analyze_unit). */
static void unit_free_files(UnitEntry *u) {
    for (int i = 0; i < u->file_count; i++) free(u->files[i]);
    free(u->files);
    u->files = NULL;
    u->file_count = u->file_cap = 0;
}

/* Is `real` (a canonical path) one of the sources this unit last analyzed? */
static bool unit_has_file(const UnitEntry *u, const char *real) {
    if (!real) return false;
    for (int i = 0; i < u->file_count; i++)
        if (strcmp(u->files[i], real) == 0) return true;
    return false;
}

/* ======================================================================== */
/* URI <-> path                                                             */
/* ======================================================================== */


/* Canonical absolute path (resolves symlinks, `..`, and relative spellings) so
 * two paths to the same on-disk file compare equal. Falls back to a plain copy
 * when the path can't be resolved (e.g. an unsaved/virtual document). Caller
 * frees. */
static char *canon_path(const char *path) {
    char *rp = platform_realpath(path);
    return rp ? rp : str_dup(path);
}

/* A character that can continue an identifier. */
static bool id_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

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
        if (p[i] == '%' && i + 2 < n && hex_digit_val(p[i + 1]) >= 0 &&
            hex_digit_val(p[i + 2]) >= 0) {
            out[j++] = (char)((hex_digit_val(p[i + 1]) << 4) | hex_digit_val(p[i + 2]));
            i += 2;
        } else {
            out[j++] = p[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* /home/x y.fc -> file:///home/x%20y.fc  (caller frees). Percent-encodes bytes
 * outside the unreserved set (keeping '/'), the inverse of uri_to_path's decode,
 * so a URI synthesized for a project file that is not open matches the one the
 * editor uses for it. */
static char *path_to_uri(const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(path);
    char *out = malloc(7 + n * 3 + 1);          /* "file://" + worst-case %XX each */
    memcpy(out, "file://", 7);
    size_t j = 7;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        if (safe) out[j++] = (char)c;
        else { out[j++] = '%'; out[j++] = hex[c >> 4]; out[j++] = hex[c & 0xF]; }
    }
    out[j] = '\0';
    return out;
}

/* The URI the client knows a file by: an open document's own URI, else one
 * built from the canonical path. A unit's file may be spelled through an
 * lsp.rsp route (`../shared/lib.fc`), and the client must see one URI per
 * file, not a second document for the route. Returns malloc'd memory. */
static char *uri_for_path(LspDocStore *s, const char *path) {
    LspDoc *open = store_find_by_path(s, path);
    if (open) return str_dup(open->uri);
    char *real = canon_path(path);
    char *uri = path_to_uri(real);
    free(real);
    return uri;
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
/* Type rendering                                                           */
/* ======================================================================== */

/* Snapshot a type's spelling into the message arena. type_name() hands back a
 * rotating slot that a later call reclaims, so any caller holding more than one
 * spelling, or holding one across further type_name() calls, needs its own
 * copy. Sized to the spelling: a type name is unbounded (nested generics, long
 * qualified names), and a clipped one would be shown to the user as the type. */
static const char *dup_type_name(Arena *a, Type *t) {
    const char *tn = type_name(t);   /* rotating slot */
    return arena_sprintf(a, "%s", tn ? tn : "?");
}

/* ======================================================================== */
/* Built-in intrinsic hover documentation                                   */
/* ======================================================================== */

/* The language's keyword-builtins (alloc, some, default, ...) and built-in
 * globals (stdin/stdout/stderr) are not user declarations, so there is no
 * decl/doc-comment to surface on hover. This static table provides one. `sig`
 * is a generic signature shown in a code fence; `doc` is the markdown body.
 * The keyword or identifier as written in source is the lookup key, which lets
 * `none` and `default` (both EXPR_DEFAULT nodes) carry distinct docs. */
typedef struct {
    const char *name;   /* spelling in source */
    const char *sig;    /* generic signature for the code fence */
    const char *doc;    /* markdown body: summary + details (no leading/trailing blank) */
} BuiltinDoc;

#include "builtin_docs.inc"

static const BuiltinDoc *builtin_doc_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof BUILTIN_DOCS / sizeof BUILTIN_DOCS[0]; i++)
        if (strcmp(BUILTIN_DOCS[i].name, name) == 0) return &BUILTIN_DOCS[i];
    return NULL;
}

/* ======================================================================== */
/* Position -> node lookup (hover / definition / completion anchor)         */
/* ======================================================================== */

typedef struct {
    int target_line, target_col;   /* 1-based, byte col */
    const char *src;
    const LineIndex *idx;
    const char *file;              /* only descend into decls from this file */
    SymbolTable *symtab;           /* analysis global symtab (resolves TYPE_STUB
                                      annotations left unresolved in the Decl tree) */

    bool found;
    int  best_span;
    int  start_line, start_col;     /* of the winning node */
    Type *type;
    const char *name;
    Symbol *sym;                    /* resolved symbol, for go-to-def (may be NULL) */
    SrcLoc def_loc;                 /* direct definition loc when there is no Symbol
                                       (block-local bindings, plain struct fields);
                                       .line == 0 means none. Takes precedence over sym. */
    SrcLoc doc_loc;                 /* site to scan for a doc comment in hover; .line == 0
                                       falls back to def_loc, then sym->decl. Distinct from
                                       def_loc so a union variant constructor can keep
                                       go-to-def on the union while its doc reads the
                                       variant line. */
    bool doc_is_field;              /* doc_loc is a struct/union field/variant line
                                       (enables trailing-comment extraction) */
    bool no_doc;                    /* suppress the hover doc scan: the winning node keeps
                                       its def_loc (for go-to-def) but has no doc of its
                                       own. Set for function parameters, whose line above
                                       holds the function decl, not the param's doc.
                                       Cleared by every consider() win. */
    const BuiltinDoc *builtin;      /* set when the winning node is a built-in intrinsic
                                       (alloc/some/.../stdin); hover renders its doc instead
                                       of a `name: type` line. Cleared by every consider() win. */
    Symbol *type_ref_sym;           /* set when the winning token names a type or module
                                       (struct/union/enum/module reference); hover renders a
                                       declaration-form header (`enum dir of i32`) instead of
                                       `name: type`. Cleared by every consider() win. */
    Symbol *companion;              /* companion module of a type_ref_sym hit (pass2's
                                       ident.companion_module); hover appends its doc as a
                                       labeled second section. Cleared by every consider() win. */
    Decl   *decl_site;              /* set when the winning token is a declaration-site
                                       name (struct/union/enum/module header line); carries
                                       kind + repr for the header when there is no Symbol.
                                       Cleared by every consider() win. */

    /* Member-completion hook: the position of the `.` of an in-progress member
     * access. During the walk, the EXPR_FIELD/EXPR_DEREF_FIELD node whose loc
     * matches is captured in op_field. Its `field.object` carries the object's
     * type for any object shape (`dict[i]`, `f()`, nested), which a position
     * lookup on the object's last character can't reach when that character is
     * a `]` or `)`. Zero op_line disables the hook. */
    int   op_line, op_col;
    Expr *op_field;
} FindCtx;

static Type *peel_to_aggregate(Type *t);   /* defined in the completion section */

static const SrcLoc NO_LOC = {0};

/* Offer a token as the hit: it wins when it covers the target position and is
 * no wider than the current winner (the innermost token wins). Returns whether
 * it won, so the caller can add what only it knows (a doc flag, the Symbol a
 * type reference names). */
static bool consider(FindCtx *c, int line, int col, int span, Type *type,
                     const char *name, Symbol *sym, SrcLoc def_loc,
                     SrcLoc doc_loc, bool doc_is_field) {
    if (line != c->target_line) return false;
    if (c->target_col < col || c->target_col >= col + span) return false;
    if (c->found && span > c->best_span) return false;
    c->found = true;
    c->best_span = span;
    c->start_line = line;
    c->start_col = col;
    c->type = type;
    c->name = name;
    c->sym = sym;
    c->def_loc = def_loc;
    c->doc_loc = doc_loc;
    c->doc_is_field = doc_is_field;
    c->no_doc = false;   /* post-set by the EXPR_IDENT winner for parameters */
    c->builtin = NULL;   /* a plain node wins; consider_builtin re-sets this when it wins */
    c->type_ref_sym = NULL;   /* post-set by the EXPR_IDENT/EXPR_FIELD/decl-site winners */
    c->companion = NULL;
    c->decl_site = NULL;
    return true;
}

static bool is_type_sym(const Symbol *s) {
    return s && (s->kind == DECL_STRUCT || s->kind == DECL_UNION || s->kind == DECL_ENUM);
}

/* A struct, union, enum or module: a symbol that hovers with a declaration-form
 * header rather than `name: type`. */
static bool is_type_or_module_sym(const Symbol *s) {
    return is_type_sym(s) || (s && s->kind == DECL_MODULE);
}

/* Whether `d` was written in `file` (every file when `file` is NULL). The merged
 * program mixes several files' decls, with overlapping line numbers, and
 * synthetic decls that have no file (a namespace reset between files); a
 * synthetic decl is in no file. */
static bool decl_in_file(const Decl *d, const char *file) {
    return !file || (d->loc.filename && strcmp(d->loc.filename, file) == 0);
}

/* Reads the contiguous identifier/keyword token at `loc` from source into `buf`,
 * returning its length (0 if `loc` is out of range). Recovers the keyword a
 * built-in node was spelled with, e.g. to tell `none` from `default`, which
 * share the EXPR_DEFAULT node kind. */
static int read_ident_at(const FindCtx *c, SrcLoc loc, char *buf, size_t bufsz) {
    buf[0] = '\0';
    if (loc.line < 1 || loc.line > c->idx->count) return 0;
    int off = c->idx->starts[loc.line - 1] + (loc.col - 1);
    int end = c->idx->len;
    size_t n = 0;
    while (off >= 0 && off < end && n + 1 < bufsz) {
        char ch = c->src[off];
        if (!id_char(ch)) break;
        buf[n++] = ch;
        off++;
    }
    buf[n] = '\0';
    return (int)n;
}

/* If `e` is a keyword-builtin node whose keyword is under the cursor, register
 * it as the hit (innermost-wins, like consider()). The keyword span is read from
 * source so the highlight covers just the keyword, and so `none`/`default`
 * resolve to their own docs. */
static void consider_builtin(FindCtx *c, const Expr *e) {
    char kw[32];
    int span = read_ident_at(c, e->loc, kw, sizeof kw);
    if (span <= 0) return;
    const BuiltinDoc *bd = builtin_doc_lookup(kw);
    if (bd && consider(c, e->loc.line, e->loc.col, span, e->type, bd->name, NULL,
                       NO_LOC, NO_LOC, false))
        c->builtin = bd;
}

/* Column (1-based) of the binding name in a `let [mut] name ...` whose `let`
 * keyword is at (let_line, let_col). Scans the source past the keyword(s). */
static int let_name_col(const LineIndex *idx, const char *src, int let_line, int let_col,
                        bool is_mut) {
    int off = idx->starts[let_line - 1] + (let_col - 1);
    int end = idx->len;
    int i = off;
    /* skip "let" */
    i += 3;
    while (i < end && (src[i] == ' ' || src[i] == '\t')) i++;
    if (is_mut) {
        i += 3; /* "mut" */
        while (i < end && (src[i] == ' ' || src[i] == '\t')) i++;
    }
    return (i - idx->starts[let_line - 1]) + 1;
}

static const char *unmangled_name(const char *n);   /* defined with the decl-site helpers */

/* A type annotation written in source (a param's `: T`, a slice literal's
 * element type, a cast/alloc/sizeof/default/enum_of target, a struct field or
 * union variant payload) resolves to a Type, but the AST records no token loc
 * for it. Recover the base type-name token textually: scan rightward from
 * (line, col) for an identifier spelling the named base type (peeling
 * pointer/slice/option/result/fixed-array layers), stopping where the
 * annotation context ends (`,` `)` `=` `{` `}` or a comment). A hit registers
 * like any type reference: declaration-form hover + go-to-definition. */
static void consider_type_annotation(FindCtx *c, Type *t, int line, int col) {
    if (line != c->target_line) return;   /* the base name is on the start line */
    while (t) {
        if (t->kind == TYPE_POINTER)          t = t->pointer.pointee;
        else if (t->kind == TYPE_SLICE)       t = t->slice.elem;
        else if (t->kind == TYPE_OPTION)      t = t->option.inner;
        else if (t->kind == TYPE_RESULT)      t = t->result.inner;
        else if (t->kind == TYPE_FIXED_ARRAY) t = t->fixed_array.elem;
        else break;
    }
    if (!t) return;
    const char *qn = NULL;
    Symbol *sym = NULL;
    if (t->kind == TYPE_STRUCT && !t->struc.is_tuple) {
        qn = t->struc.qualified_name ? t->struc.qualified_name : t->struc.name;
        sym = t->struc.resolved_sym;
    } else if (t->kind == TYPE_UNION) {
        qn = t->unio.qualified_name ? t->unio.qualified_name : t->unio.name;
        sym = t->unio.resolved_sym;
    } else if (t->kind == TYPE_ENUM) {
        qn = t->enu.qualified_name ? t->enu.qualified_name : t->enu.name;
        sym = t->enu.resolved_sym;
    } else if (t->kind == TYPE_STUB && c->symtab) {
        /* Struct-field/variant-payload annotations referencing a type outside
         * their own module keep their parse-time stub in the Decl tree (only
         * use-site copies get resolved). The global symtab holds every type
         * under its mangled name, and top-level types under their source name
         * too, so the stub's own name (canonicalized by pass1 where needed)
         * resolves either way. */
        qn = t->stub.qualified_name ? t->stub.qualified_name : t->stub.name;
        sym = symtab_lookup_type(c->symtab, t->stub.name);
    } else {
        return;   /* primitives/functions/type-vars: nothing to link */
    }
    if (!qn || !sym) return;
    /* Base name token = last path component of the qualified name
     * (m.point -> point, std::io.file -> file), unmangled to its source
     * spelling (a canonicalized stub may read fc__m__point). */
    const char *base = qn;
    for (const char *p = qn; *p; p++)
        if (*p == '.' || *p == ':') base = p + 1;
    base = unmangled_name(base);
    int blen = (int)strlen(base);
    if (blen == 0 || strchr(base, '<')) return;

    int off = c->idx->starts[line - 1] + (col - 1);
    int end = (line < c->idx->count) ? c->idx->starts[line] : c->idx->len;
    while (off < end) {
        char ch = c->src[off];
        if (ch == ',' || ch == ')' || ch == '=' || ch == '{' || ch == '}' ||
            ch == '\n')
            return;
        if (ch == '/' && off + 1 < end &&
            (c->src[off + 1] == '/' || c->src[off + 1] == '*'))
            return;
        if (!id_char(ch) || isdigit((unsigned char)ch)) { off++; continue; }
        int tok_start = off;
        while (off < end && id_char(c->src[off])) off++;
        if (off - tok_start == blen &&
            strncmp(c->src + tok_start, base, (size_t)blen) == 0) {
            int tok_col = (tok_start - c->idx->starts[line - 1]) + 1;
            if (consider(c, line, tok_col, blen, t, base, sym, NO_LOC, NO_LOC, false))
                c->type_ref_sym = sym;
            return;   /* first matching token is the annotation's */
        }
    }
}

/* The declaration site of variant `name` of union or enum type `t`. */
static SrcLoc variant_decl_loc(const Type *t, const char *name) {
    if (t->kind == TYPE_UNION) {
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].name == name) return t->unio.variants[i].loc;
    } else if (t->kind == TYPE_ENUM) {
        for (int i = 0; i < t->enu.variant_count; i++)
            if (t->enu.variants[i].name == name) return t->enu.variants[i].loc;
    }
    return NO_LOC;
}

/* The field-name token of a field access. Its exact source loc is recorded by
 * the parser, so hover and definition target the name itself whatever the
 * object's shape (a.b, a.b.c, s . field) or spacing. Go-to-definition
 * resolves module members (mod.member -> the member decl) and variant
 * constructors (shape.circle -> the union or enum decl) through a Symbol,
 * and a plain struct field (s.field) through the field's recorded loc. */
static void consider_field_name(FindCtx *c, Expr *e) {
    if (e->field.name_loc.line <= 0) return;
    Symbol *def = e->field.resolved_member;
    SrcLoc doc_loc = NO_LOC;
    bool doc_is_field = false;
    if (!def && e->field.is_variant_constructor && e->type &&
        (e->type->kind == TYPE_UNION || e->type->kind == TYPE_ENUM)) {
        /* Go-to-def lands on the type's decl, but the doc comment is read from
         * the variant's own line. */
        def = e->type->kind == TYPE_UNION ? e->type->unio.resolved_sym
                                          : e->type->enu.resolved_sym;
        doc_loc = variant_decl_loc(e->type, e->field.name);
        doc_is_field = doc_loc.line > 0;
    }
    SrcLoc field_def = NO_LOC;
    if (!def && e->field.object) {
        Type *ot = peel_to_aggregate(e->field.object->type);
        if (ot && ot->kind == TYPE_STRUCT)
            for (int i = 0; i < ot->struc.field_count; i++)
                if (ot->struc.fields[i].name == e->field.name) {
                    field_def = ot->struc.fields[i].loc;
                    doc_loc = field_def;       /* plain field: def == doc site */
                    doc_is_field = true;
                    break;
                }
    }
    /* Module-member type reference (m.point, gfx.mode): the member itself is
     * a type or module, not a value, so it gets the declaration-form header.
     * Variant constructors and type properties keep their value hover (their
     * `def` may be the enum or union symbol, so gate on the flags). */
    if (consider(c, e->field.name_loc.line, e->field.name_loc.col,
                 (int)strlen(e->field.name), e->type, e->field.name,
                 def, field_def, doc_loc, doc_is_field) &&
        !e->field.is_variant_constructor && !e->field.is_type_property &&
        is_type_or_module_sym(def))
        c->type_ref_sym = def;
}

/* Offer each token of e that hover and go-to-definition can land on (names,
 * field names, written type annotations, builtin keywords) to consider(),
 * then do the same for e's children. `find_ctx` is a FindCtx. */
static void find_in_expr(Expr *e, void *find_ctx) {
    if (!e) return;
    FindCtx *c = find_ctx;
    switch (e->kind) {
        case EXPR_IDENT: {
            if (!consider(c, e->loc.line, e->loc.col, (int)strlen(e->ident.name),
                          e->type, e->ident.name, e->ident.resolved_sym,
                          e->ident.resolved_local_loc, e->ident.resolved_local_loc, false))
                break;
            /* Block-locals carry a doc site (the line above their binding) so a
             * `// comment` over a `let` shows on hover. A parameter is the one
             * exception: the line above it holds the function decl or an earlier
             * param, never the param's own doc, so suppress the doc scan and let
             * it hover as name: type. Go-to-def still uses resolved_local_loc. */
            if (e->ident.resolved_local_is_param)
                c->no_doc = true;
            /* Type/module reference: render a declaration-form hover header, and
             * carry the companion module so both docs merge into one hover. */
            if (is_type_or_module_sym(e->ident.resolved_sym)) {
                c->type_ref_sym = e->ident.resolved_sym;
                c->companion = e->ident.companion_module;
            }
            /* The built-in streams resolve to no Symbol and no local binding;
             * attach their doc. */
            if (e->ident.is_std_stream)
                c->builtin = builtin_doc_lookup(e->ident.name);
            break;
        }
        case EXPR_FIELD:
        case EXPR_DEREF_FIELD:
            find_in_expr(e->field.object, c);   /* the object precedes the name */
            /* Member-completion capture: the parser stamps EXPR_FIELD.loc with the
             * operator token (`.`), so match the completion operator position
             * to grab this node regardless of the field name's completeness (the
             * in-progress name may be empty or absent). */
            if (c->op_line && e->loc.line == c->op_line && e->loc.col == c->op_col)
                c->op_field = e;
            consider_field_name(c, e);
            return;
        case EXPR_CAST:
            consider_type_annotation(c, e->cast.target, e->loc.line, e->loc.col);
            break;
        case EXPR_ENUM_OF:
            consider_builtin(c, e);   /* the enum_of keyword */
            consider_type_annotation(c, e->enum_of_expr.target, e->loc.line, e->loc.col);
            break;
        case EXPR_FUNC:
            for (int i = 0; i < e->func.param_count; i++) {
                Param *p = &e->func.params[i];
                if (p->name) {
                    consider(c, p->loc.line, p->loc.col, (int)strlen(p->name),
                             p->type, p->name, NULL, NO_LOC, NO_LOC, false);
                    /* The written annotation after `name:`: hover/def on the
                     * type name itself. */
                    consider_type_annotation(c, p->type, p->loc.line,
                                             p->loc.col + (int)strlen(p->name));
                }
            }
            break;
        case EXPR_STRUCT_LIT:
            /* The type name (at the node's loc) goes to the struct declaration. */
            if (e->struct_lit.type_name) {
                /* The token names the type: declaration-form hover header. */
                if (consider(c, e->loc.line, e->loc.col,
                             (int)strlen(e->struct_lit.type_name), e->type,
                             e->struct_lit.type_name, e->struct_lit.resolved_sym,
                             NO_LOC, NO_LOC, false) &&
                    is_type_or_module_sym(e->struct_lit.resolved_sym))
                    c->type_ref_sym = e->struct_lit.resolved_sym;
            }
            break;
        case EXPR_ARRAY_LIT:
            /* The literal starts with its written element type (`dir[8] {`). */
            consider_type_annotation(c, e->array_lit.elem_type, e->loc.line, e->loc.col);
            break;
        case EXPR_SLICE_LIT:
            consider_type_annotation(c, e->slice_lit.elem_type, e->loc.line, e->loc.col);
            break;
        case EXPR_ALLOC:
            consider_builtin(c, e);   /* the alloc/alloca keyword */
            if (e->alloc_expr.alloc_type)   /* alloc(T)/alloc(T,N): written type */
                consider_type_annotation(c, e->alloc_expr.alloc_type,
                                         e->loc.line, e->loc.col);
            break;
        case EXPR_SIZEOF:
        case EXPR_ALIGNOF:
        case EXPR_DEFAULT: {
            consider_builtin(c, e);
            /* The written type argument. `none` shares EXPR_DEFAULT but
             * spells no type; read the keyword to tell them apart. */
            Type *tt = e->kind == EXPR_SIZEOF  ? e->sizeof_expr.target
                     : e->kind == EXPR_ALIGNOF ? e->alignof_expr.target
                     :                           e->default_expr.target;
            char kw[16];
            read_ident_at(c, e->loc, kw, sizeof kw);
            if (e->kind != EXPR_DEFAULT || strcmp(kw, "default") == 0)
                consider_type_annotation(c, tt, e->loc.line, e->loc.col);
            break;
        }
        case EXPR_BITCAST:
        case EXPR_FREE:
        case EXPR_SOME:
        case EXPR_OK:
        case EXPR_ERR:
        case EXPR_ERROR_NAME:
        case EXPR_ASSERT:
        case EXPR_ATOMIC_LOAD:
        case EXPR_ATOMIC_STORE:
            consider_builtin(c, e);   /* the keyword */
            break;
        case EXPR_LET: {
            int col = let_name_col(c->idx, c->src, e->loc.line, e->loc.col,
                                   e->let_expr.let_is_mut);
            consider(c, e->loc.line, col, (int)strlen(e->let_expr.let_name),
                     e->let_expr.let_type, e->let_expr.let_name, NULL,
                     NO_LOC, e->let_expr.let_name_loc, false);
            break;
        }
        default:
            break;
    }
    expr_for_each_child(e, find_in_expr, c);
}

static void find_in_decls(Decl **decls, int n, FindCtx *c);

/* Column (1-based) of the name that follows a declaration keyword of length
 * kw_len at (line, col). `private` is consumed before the keyword loc is
 * stamped, so the keyword is always first. */
static int kw_name_col(const FindCtx *c, int line, int col, int kw_len) {
    int off = c->idx->starts[line - 1] + (col - 1) + kw_len;
    int end = c->idx->len;
    while (off < end && (c->src[off] == ' ' || c->src[off] == '\t')) off++;
    return (off - c->idx->starts[line - 1]) + 1;
}

/* pass1 mangles type-decl names in place (dir -> fc__dir, fc__m__dir); the
 * source spelling is the suffix after the last "__" (FC identifiers cannot
 * contain a double underscore, so the split is unambiguous). Module names are
 * unmangled. */
static const char *unmangled_name(const char *n) {
    const char *last = NULL;
    for (const char *p = n; (p = strstr(p, "__")) != NULL; p += 2) last = p;
    return last ? last + 2 : n;
}

/* Hover for the name on a struct/union/enum/module declaration line. There is
 * no Symbol at hand here; decl_site carries the Decl so hover can render the
 * declaration-form header and read the attached doc comment. */
static void consider_decl_name(FindCtx *c, Decl *d, int kw_len, const char *decl_name) {
    if (!decl_name) return;
    const char *src_name = unmangled_name(decl_name);
    int col = kw_name_col(c, d->loc.line, d->loc.col, kw_len);
    if (consider(c, d->loc.line, col, (int)strlen(src_name),
                 NULL, src_name, NULL, NO_LOC, d->loc, false))
        c->decl_site = d;
}

/* One identifier written in an import statement. pass1 stamped what the
 * statement resolved to (import.resolved_*), so this is a pure position match
 * with no re-resolution. `span` is the written token's length while `name` is
 * the symbol's own spelling: for an `as` alias the two differ, and hovering the
 * alias should report the thing it aliases, which is what the reader came to
 * look up. An unresolved import contributes nothing (there is no symbol to
 * describe, and the diagnostic already says so). */
static void consider_import_ident(FindCtx *c, SrcLoc loc, int span,
                                  const char *name, Symbol *sym, Symbol *companion) {
    if (loc.line == 0 || span <= 0 || !name || !sym) return;
    Type *t = sym->type;
    if (!t && sym->decl && sym->decl->kind == DECL_LET) t = sym->decl->let.resolved_type;
    if (!consider(c, loc.line, loc.col, span, t, name, sym, NO_LOC, NO_LOC, false)) return;
    /* A type or module reference hovers in declaration form (`struct point`,
     * `module io`), same as a use-site reference to it would. */
    if (is_type_or_module_sym(sym)) {
        c->type_ref_sym = sym;
        c->companion = companion;
    }
}

/* The identifiers of an import statement: the imported name, its `as` alias,
 * and every module segment of the `from` route, each answering for the module
 * it names, so `from a.b.c` hovers and jumps per segment. A namespace path
 * (`std::`) names no declaration, so its segments are not offered. */
static void consider_import(FindCtx *c, Decl *d) {
    const char *name = d->import.name;
    Symbol *sym = d->import.resolved_sym;
    Symbol *comp = d->import.resolved_companion;
    if (name) {
        consider_import_ident(c, d->import.name_loc, (int)strlen(name), name, sym, comp);
        if (d->import.alias)
            consider_import_ident(c, d->import.alias_loc, (int)strlen(d->import.alias),
                                  name, sym, comp);
    }
    if (d->import.from_module)
        consider_import_ident(c, d->import.module_loc, (int)strlen(d->import.from_module),
                              d->import.from_module, d->import.resolved_module, NULL);
    for (int i = 0; i < d->import.route_count; i++) {
        const ImportRouteSeg *seg = &d->import.route[i];
        consider_import_ident(c, seg->loc, (int)strlen(seg->name), seg->name,
                              seg->sym, NULL);
    }
}

static void find_in_decl(Decl *d, FindCtx *c) {
    if (!d) return;
    /* The merged program holds decls from several files with overlapping line
     * numbers; only descend into the open document's decls. */
    if (!decl_in_file(d, c->file)) return;
    switch (d->kind) {
        case DECL_LET:
            if (d->let.name) {
                int col = let_name_col(c->idx, c->src, d->loc.line, d->loc.col, d->let.is_mut);
                consider(c, d->loc.line, col, (int)strlen(d->let.name),
                         d->let.resolved_type, d->let.name, NULL, NO_LOC, d->loc, false);
            }
            find_in_expr(d->let.init, c);
            break;
        case DECL_MODULE:
            /* Error groups are parser-desugared modules; their keyword is
             * `error` (5 chars), a real module's is `module` (6). */
            consider_decl_name(c, d, d->module.is_error_group ? 5 : 6, d->module.name);
            find_in_decls(d->module.decls, d->module.decl_count, c);
            break;
        case DECL_STRUCT:
            if (!d->struc.is_extern) {
                consider_decl_name(c, d, 6, d->struc.name);
                for (int i = 0; i < d->struc.field_count; i++) {
                    StructField *f = &d->struc.fields[i];
                    if (f->name && f->loc.line > 0)
                        consider_type_annotation(c, f->type, f->loc.line,
                                                 f->loc.col + (int)strlen(f->name));
                }
            }
            break;
        case DECL_UNION:
            consider_decl_name(c, d, 5, d->unio.name);
            for (int i = 0; i < d->unio.variant_count; i++) {
                UnionVariant *v = &d->unio.variants[i];
                if (v->name && v->payload && v->loc.line > 0)
                    consider_type_annotation(c, v->payload, v->loc.line,
                                             v->loc.col + (int)strlen(v->name));
            }
            break;
        case DECL_ENUM:
            consider_decl_name(c, d, 4, d->enu.name);
            break;
        case DECL_IMPORT:
            consider_import(c, d);
            break;
        default: break;
    }
}

static void find_in_decls(Decl **decls, int n, FindCtx *c) {
    for (int i = 0; i < n; i++) find_in_decl(decls[i], c);
}

/* Companion module of a type Symbol: the same-scope DECL_MODULE sharing the
 * type's source name. pass2 stamps this on ident references
 * (ident.companion_module); this recovers it for reference hits that carry
 * only the type symbol: annotations, struct-literal type names, module-member
 * type paths. */
static Symbol *companion_of_type_sym(AnalysisResult *r, Symbol *ts) {
    if (!ts) return NULL;
    if (ts->kind != DECL_STRUCT && ts->kind != DECL_UNION && ts->kind != DECL_ENUM)
        return NULL;
    const char *iname = intern_cstr(&r->intern, unmangled_name(ts->name));
    Symbol *m = ts->parent
        ? symtab_lookup_kind(ts->parent->members, iname, DECL_MODULE)
        : symtab_lookup_module(&r->symtab, iname, ts->ns_prefix);
    if (m && m->decl && m->decl->kind == DECL_MODULE && m->decl->module.is_error_group)
        return NULL;   /* error groups only look like modules */
    return m;
}

/* Run the position lookup against a document's (unit) analysis. */
static bool locate(LspServer *S, LspDoc *doc, const LineIndex *idx, int line0, int char0, FindCtx *out) {
    AnalysisResult *r = query_result(S, doc);
    if (!r || !r->program) return false;
    int line1, col1;
    lsp_to_loc(idx, doc->text, line0, char0, &line1, &col1);
    FindCtx c = {0};
    c.target_line = line1;
    c.target_col = col1;
    c.src = doc->text;
    c.idx = idx;
    c.file = doc->path;
    c.symtab = &r->symtab;
    find_in_decls(r->program->decls, r->program->decl_count, &c);
    /* A type reference that didn't come through pass2's ident path (a written
     * annotation, a struct-literal type name, a module-member path) carries no
     * companion; recover it so every use-site type hover merges the pair.
     * Declaration-site names don't merge: a module only becomes a companion
     * where a reference resolves it as one, so each half's decl hover shows
     * only its own doc. */
    if (c.found && !c.companion && c.type_ref_sym) {
        Symbol *m = companion_of_type_sym(r, c.type_ref_sym);
        if (m) c.companion = m;
    }
    *out = c;
    return c.found;
}

/* ======================================================================== */
/* Diagnostics                                                              */
/* ======================================================================== */

/* One diagnostic pinned to a file, aggregated across every open document's unit
 * analysis so diagnostics can be published project-wide (not just for the open
 * buffer). `file`/`msg` borrow arena storage owned by the source analysis. */
typedef struct { const char *file; int line, col; const char *msg; } AggDiag;

/* Emit one textDocument/publishDiagnostics for `uri`. `ds`/`n` are the diagnostics
 * for that file (n == 0 clears it and needs no text); `text`/`text_len` supply the
 * bytes for mapping 1-based (line, byte-col) locations to LSP (line, UTF-16-char)
 * ranges, so it must be the content that was analyzed for this file. */
static void emit_diagnostics(LspServer *S, const char *uri, const char *text,
                             int text_len, const AggDiag *ds, int n) {
    Arena *a = &S->msg_arena;
    JsonValue *arr = json_array(a);
    if (n > 0) {
        LineIndex idx = line_index_build(a, text, text_len);
        for (int i = 0; i < n; i++) {
            int line1 = ds[i].line > 0 ? ds[i].line : 1;
            int col1  = ds[i].col  > 0 ? ds[i].col  : 1;
            int start_byte = loc_byte_offset(&idx, line1, col1);
            /* Extend the squiggle over the identifier/token at the location. */
            int end_byte = start_byte;
            while (end_byte < text_len &&
                   id_char(text[end_byte]))
                end_byte++;
            if (end_byte == start_byte) end_byte = start_byte + 1;
            int end_col1 = col1 + (end_byte - start_byte);

            int sl, sc, el, ec;
            loc_to_lsp(&idx, text, line1, col1, &sl, &sc);
            loc_to_lsp(&idx, text, line1, end_col1, &el, &ec);

            JsonValue *diag = json_object(a);
            json_object_set(a, diag, "range", mk_range(a, sl, sc, el, ec));
            json_object_set(a, diag, "severity", json_num(a, 1)); /* Error */
            json_object_set(a, diag, "source", json_str(a, "fcc"));
            json_object_set(a, diag, "message", json_str(a, ds[i].msg));
            json_array_push(a, arr, diag);
        }
    }

    JsonValue *params = json_object(a);
    json_object_set(a, params, "uri", json_str(a, uri));
    json_object_set(a, params, "diagnostics", arr);
    lsp_notify(a, "textDocument/publishDiagnostics", params);
}

#if !defined(_WIN32)
/* The directory part of a document path, malloc'd, or NULL when there is none
 * to search (no '/', or a file directly under the root). */
static char *doc_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash > path ? str_ndup(path, (size_t)(slash - path)) : NULL;
}
#endif

/* Walk up from the directory of `doc_path` looking for an `lsp.rsp` response
 * file (the by-convention name the LSP discovers; equivalent to `fcc @lsp.rsp`).
 * Nearest ancestor wins. Returns a malloc'd path (caller frees) or NULL. */
static char *find_lsp_rsp(const char *doc_path) {
#if defined(_WIN32)
    (void)doc_path;
    return NULL;
#else
    char *dirbuf = doc_dir(doc_path);
    if (!dirbuf) return NULL;
    /* Resolve the directory (the file itself may be unsaved/virtual). */
    char *dir = realpath(dirbuf, NULL);
    free(dirbuf);
    if (!dir) return NULL;

    char *result = NULL;
    for (;;) {
        char *path = str_sprintf("%s/lsp.rsp", dir);
        if (access(path, R_OK) == 0) { result = path; break; }
        free(path);
        char *up = strrchr(dir, '/');
        if (!up || up == dir) break;        /* reached the filesystem root */
        *up = '\0';
    }
    free(dir);
    return result;
#endif
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
    char *dir = doc_dir(doc_path);
    if (!dir) return;
    DIR *d = opendir(dir);
    if (!d) { free(dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 4 || strcmp(nm + l - 3, ".fc") != 0) continue;
        char *full = str_sprintf("%s/%s", dir, nm);
        struct stat st;
        if (strcmp(full, doc_path) != 0 &&                  /* not the open file */
            !(stat(full, &st) == 0 && S_ISDIR(st.st_mode)))
            DA_APPEND(*out, *count, *cap, full);
        else
            free(full);
    }
    closedir(d);
    free(dir);
#endif
}

/* Identity of a document's compilation unit, so docs that resolve to the same
 * unit are analyzed together, once. Two docs share a unit iff they discover the
 * same `lsp.rsp` (its listed inputs are the unit) or, with no lsp.rsp, live in
 * the same directory (the sibling-heuristic unit). The `R:`/`D:` prefixes keep an
 * rsp path from colliding with a directory path. Caller frees.
 *
 * On Windows the rsp/sibling discovery is compiled out, so each document is its
 * own unit, keyed by its path. */
static char *unit_key(LspDoc *doc) {
#if defined(_WIN32)
    return str_sprintf("F:%s", doc->path);
#else
    char *rsp = find_lsp_rsp(doc->path);
    if (rsp) {
        char *c = canon_path(rsp);
        free(rsp);
        char *k = str_sprintf("R:%s", c);
        free(c);
        return k;
    }
    /* Directory of doc->path (canonicalized so symlinked spellings coincide). */
    char *dir = doc_dir(doc->path);
    if (!dir) return str_dup(doc->path);   /* no directory: unique key */
    char *c = canon_path(dir);
    free(dir);
    char *k = str_sprintf("D:%s", c);
    free(c);
    return k;
#endif
}

static UnitEntry *unit_find_or_create(LspServer *S, const char *key) {
    UnitEntry *u = unit_find(S, key);
    if (u) return u;
    UnitEntry e = {0};
    e.key = str_dup(key);
    DA_APPEND(S->units, S->unit_count, S->unit_cap, e);
    return &S->units[S->unit_count - 1];
}

/* Drop every unit no open document references any more (all its files closed),
 * freeing its analyses. The next publish cycle clears their diagnostics (the
 * URIs fall out of the aggregate and are emptied). */
static void unit_prune(LspServer *S) {
    for (int i = 0; i < S->unit_count;) {
        bool used = false;
        for (int d = 0; d < S->store.count; d++)
            if (S->store.docs[d].unit_key &&
                strcmp(S->store.docs[d].unit_key, S->units[i].key) == 0) { used = true; break; }
        if (used) { i++; continue; }
        unit_free_results(&S->units[i]);
        unit_free_files(&S->units[i]);
        free(S->units[i].key);
        S->units[i] = S->units[--S->unit_count];
    }
}

/* The sources and settings one unit analysis merges, with the storage they
 * borrow from, which must outlive the analyze() call. */
typedef struct {
    AnalysisSource *extra;              /* every source but the primary document */
    int n, cap;
    char **disk_bufs;                   /* text read from disk for extra[] */
    int db, dbcap;
    char **sibs;                        /* sibling paths extra[].filename may point at */
    int sc, scap;
    Flag *flags;                        /* conditional-compilation flags */
    int flag_count, flag_cap;
    ExpandedArgs rsp_expanded;          /* an lsp.rsp's tokens, which flags borrow */
    CompileArgs rsp_ca;
    bool have_rsp;
    char *rsp_err;                      /* set when an lsp.rsp exists but is unusable */
    int len_repr;
} UnitSources;

/* Add a unit member other than the primary document: the live text of `open`,
 * the editor's buffer for it, else the file at `path` on disk. */
static void add_member_source(UnitSources *us, LspDoc *open, const char *path) {
    if (open) {
        AnalysisSource src = { open->path, open->text, open->text_len };
        DA_APPEND(us->extra, us->n, us->cap, src);
        return;
    }
    int len;
    char *buf = read_file(path, &len);
    if (!buf) return;
    AnalysisSource src = { path, buf, len };
    DA_APPEND(us->extra, us->n, us->cap, src);
    DA_APPEND(us->disk_bufs, us->db, us->dbcap, buf);
}

/* The unit an lsp.rsp above `doc` defines, as `fcc @lsp.rsp` would compile it:
 * its inputs, --flags and --len-repr. Returns false when there is none; when
 * it exists but is unusable, us->rsp_err says why. */
static bool gather_rsp_sources(LspServer *S, LspDoc *doc, UnitSources *us) {
    char *rsp_path = find_lsp_rsp(doc->path);
    if (!rsp_path) return false;
    /* Sized to the path: a clipped "@..." names a different file (or none),
     * and args_expand would report that as a broken response file. */
    char *at = str_sprintf("@%s", rsp_path);
    char *fake_argv[] = { (char *)"fcc", at };
    char *aerr = NULL;
    if (args_expand(2, fake_argv, &us->rsp_expanded, &aerr) &&
        args_parse(&us->rsp_expanded, &us->rsp_ca)) {
        us->have_rsp = true;
        us->flags = us->rsp_ca.flags;
        us->flag_count = us->rsp_ca.flag_count;
        us->len_repr = us->rsp_ca.len_repr;

        char *doc_real = canon_path(doc->path);
        for (int i = 0; i < us->rsp_ca.input_count; i++) {
            char *in_real = canon_path(us->rsp_ca.inputs[i]);
            /* The open document arrives as the primary source; never add it
             * again, and its live buffer must win over the on-disk copy. */
            if (strcmp(doc_real, in_real) == 0) { free(in_real); continue; }
            /* Match by canonical path: the rsp spells its inputs file-relative
             * (`../shared/lib.fc`) while the editor opens the file by its
             * resolved path, and a raw-spelling miss would feed the on-disk
             * copy and ignore the buffer's unsaved edits. */
            LspDoc *od = store_find_by_path(&S->store, in_real);
            free(in_real);
            add_member_source(us, od != doc ? od : NULL, us->rsp_ca.inputs[i]);
        }
        free(doc_real);
    } else {
        /* A broken lsp.rsp must not silently behave as if absent: the caller
         * falls back to the heuristic, and the reason is surfaced on the open
         * file. */
        const char *m = aerr ? aerr : (us->rsp_ca.error ? us->rsp_ca.error : "parse failed");
        us->rsp_err = str_dup(m);
        free(aerr);
        args_compile_free(&us->rsp_ca);
        args_expand_free(&us->rsp_expanded);
    }
    free(at);
    free(rsp_path);
    return us->have_rsp;
}

/* The zero-config unit: host flags, the .fc files beside `doc`, and the stdlib
 * feed. */
static void gather_heuristic_sources(LspServer *S, LspDoc *doc, UnitSources *us) {
    platform_detect_flags(&us->flags, &us->flag_count, &us->flag_cap);

    collect_sibling_fc(doc->path, &us->sibs, &us->sc, &us->scap);
    for (int i = 0; i < us->sc; i++) {
        LspDoc *od = store_find_by_path(&S->store, us->sibs[i]);
        if (od == doc) continue;
        add_member_source(us, od, us->sibs[i]);
    }

    /* stdlib feed. Drop any feed file that is the same source as the open
     * document or a sibling, most commonly because Go To Definition opened a
     * stdlib module the feed already provides. Analyzing that file twice would
     * report spurious redefinition errors and waste an analysis.
     *
     * A feed entry is a duplicate if it matches a "have" entry by either:
     *   - canonical absolute path (realpath): the same on-disk file however its
     *     path was spelled or symlinked, including an open document with unsaved
     *     edits (the path matches though the buffer differs from disk, and the
     *     live buffer must win); or
     *   - byte-identical content: a separate copy of the same source at a
     *     different path (the repo's stdlib/data.fc while the feed resolves to
     *     the installed /usr/local/share/.../data.fc). The content check compares
     *     lengths first, so a full memcmp runs only for a same-length candidate.
     * Two different files that merely share a basename (a project's own
     * `data.fc` and the stdlib's) match neither key and are both kept; matching
     * by basename would shadow `std::data`. */
    int sib_n = us->n;                              /* siblings precede any feed entries */
    int have_n = sib_n + 1;
    char       **have_path = malloc(sizeof *have_path * (size_t)have_n);
    const char **have_text = malloc(sizeof *have_text * (size_t)have_n);
    int         *have_len  = malloc(sizeof *have_len  * (size_t)have_n);
    have_path[0] = canon_path(doc->path);
    have_text[0] = doc->text;
    have_len[0]  = doc->text_len;
    for (int j = 0; j < sib_n; j++) {
        have_path[1 + j] = canon_path(us->extra[j].filename);
        have_text[1 + j] = us->extra[j].text;
        have_len[1 + j]  = us->extra[j].len;
    }

    for (int i = 0; i < S->stdlib_count; i++) {
        char *feed_real = canon_path(S->stdlib[i].filename);
        bool dup = false;
        for (int j = 0; j < have_n; j++) {
            if (strcmp(have_path[j], feed_real) == 0 ||
                (have_len[j] == S->stdlib[i].len &&
                 memcmp(have_text[j], S->stdlib[i].text,
                        (size_t)S->stdlib[i].len) == 0)) {
                dup = true;
                break;
            }
        }
        free(feed_real);
        if (!dup) DA_APPEND(us->extra, us->n, us->cap, S->stdlib[i]);
    }
    for (int j = 0; j < have_n; j++) free(have_path[j]);
    free(have_path);
    free(have_text);
    free(have_len);
}

static void unit_sources_free(UnitSources *us) {
    for (int i = 0; i < us->db; i++) free(us->disk_bufs[i]);
    free(us->disk_bufs);
    for (int i = 0; i < us->sc; i++) free(us->sibs[i]);
    free(us->sibs);
    free(us->extra);
    free(us->rsp_err);
    if (us->have_rsp) {
        args_compile_free(&us->rsp_ca);      /* frees the flags array */
        args_expand_free(&us->rsp_expanded); /* frees the tokens flags borrowed */
    } else {
        free(us->flags);
    }
}

/* (Re)run the analysis for one compilation unit, storing it on `u`. `doc` is the
 * primary document (any open doc in the unit), which analyze() lexes fresh;
 * every other open doc in the unit is merged as an extra source using its live
 * buffer, so this single analysis serves all of the unit's open documents.
 *
 * The compilation unit comes from one of two sources:
 *   - an `lsp.rsp` response file discovered by walking up from the open file.
 *     It is authoritative: its listed inputs (plus the open buffer), with no
 *     sibling glob and no blanket stdlib feed, so the editor resolves names the
 *     same way `fcc @lsp.rsp` does on the CLI; or
 *   - with no lsp.rsp, the zero-config heuristic: the open file, its sibling
 *     .fc files (open-buffer text preferred over disk), and the stdlib feed. */
static void analyze_unit(LspServer *S, UnitEntry *u, LspDoc *doc) {
    /* The previous fresh result is retired below, after the new analysis, so it
     * can be kept as last_good when the new one fails to type-check. */
    AnalysisResult *prev = u->result;

    /* Membership is rebuilt from this run's sources (see the record below). */
    unit_free_files(u);

    UnitSources us = { .len_repr = 64 };
    if (!gather_rsp_sources(S, doc, &us))
        gather_heuristic_sources(S, doc, &us);

    /* Record the unit's member set (canonical, so any spelling of a file matches):
     * every source this analysis merges. flush_dirty consults it so that editing
     * a file that belongs to a unit it does not key (a shared library another
     * directory's lsp.rsp lists) still re-runs that unit, instead of leaving it
     * serving an AST whose positions do not match the file's current text. */
    DA_APPEND(u->files, u->file_count, u->file_cap, canon_path(doc->path));
    for (int i = 0; i < us.n; i++)
        DA_APPEND(u->files, u->file_count, u->file_cap, canon_path(us.extra[i].filename));

    u->result = analyze(doc->text, doc->text_len, doc->path, us.extra, us.n, us.flags,
                        us.flag_count, us.len_repr, &S->lex_cache);

    /* lsp.rsp existed but couldn't be used: attach one file-level diagnostic so
     * the editor explains the fallback instead of silently differing. */
    if (us.rsp_err) {
        /* Sized to the message: rsp_err quotes a path from the response file. */
        Diagnostic d;
        d.loc = (SrcLoc){ .filename = u->result->filename, .line = 1, .col = 1 };
        d.message = arena_sprintf(&u->result->arena, "lsp.rsp ignored: %s", us.rsp_err);
        DA_APPEND(u->result->diags, u->result->diag_count, u->result->diag_cap, d);
    }

    /* Retain the last analysis that type-checked. When the fresh one is good it
     * becomes the new last_good (and the previous good copy is freed); when it
     * is degraded (a lexer abort, so nothing was type-checked) the prior good
     * one is kept so type-aware queries keep answering. prev and last_good may
     * alias, hence the !=-guarded frees. */
    if (u->result->typed) {
        if (u->last_good && u->last_good != prev) analysis_free(u->last_good);
        u->last_good = u->result;
        if (prev && prev != u->result) analysis_free(prev);
    } else {
        if (prev && prev != u->last_good) analysis_free(prev);
    }

    unit_sources_free(&us);
    /* Diagnostics are published project-wide once per flush cycle (see
     * publish_project_diagnostics), not per-document here. */
}

/* Publish diagnostics for every file in the project, open buffer or not, so an
 * edit that breaks (or fixes) a file the user has not opened still surfaces (or
 * clears) there, and errors cascade to all dependent files. Each unit analysis
 * computes diagnostics for every file in its unit, keyed by filename; these are
 * aggregated across all units (deduped, since units may overlap), then emitted
 * as one notification per file's URI. Files that went clean, dropped out of the
 * unit, or were closed are cleared with an explicit empty publish, diffed
 * against S->pub_uris (the set last published non-empty).
 *
 * stdlib feed files are never surfaced: presumed clean, not the user's code, and
 * often under a read-only install path. A stdlib file the user opened is a
 * normal document, found by store_find_by_path, and handled as one. */
static void publish_project_diagnostics(LspServer *S) {
    /* 1. Aggregate + dedup diagnostics across every unit's fresh analysis, keyed
     *    by file. */
    AggDiag *agg = NULL;
    int an = 0, acap = 0;
    for (int ui = 0; ui < S->unit_count; ui++) {
        AnalysisResult *r = S->units[ui].result;
        if (!r) continue;
        for (int i = 0; i < r->diag_count; i++) {
            /* A NULL filename defaults to that analysis's primary document. */
            const char *file = r->diags[i].loc.filename ? r->diags[i].loc.filename
                                                        : r->filename;
            if (!store_find_by_path(&S->store, file)) {   /* not open: drop if stdlib */
                bool is_std = false;
                for (int k = 0; k < S->stdlib_count; k++)
                    if (strcmp(S->stdlib[k].filename, file) == 0) { is_std = true; break; }
                if (is_std) continue;
            }
            int line = r->diags[i].loc.line, col = r->diags[i].loc.col;
            const char *msg = r->diags[i].message;
            bool dup = false;
            for (int j = 0; j < an; j++)
                if (agg[j].line == line && agg[j].col == col &&
                    strcmp(agg[j].file, file) == 0 && strcmp(agg[j].msg, msg) == 0) {
                    dup = true; break;
                }
            if (dup) continue;
            AggDiag ad = { file, line, col, msg };
            DA_APPEND(agg, an, acap, ad);
        }
    }

    /* 2. One notification per distinct errored file; collect its URI into the new
     *    non-empty set `now`. */
    char **now = NULL;
    int now_n = 0, now_cap = 0;
    for (int i = 0; i < an; i++) {
        bool seen = false;                        /* file already emitted this cycle? */
        for (int j = 0; j < i; j++)
            if (strcmp(agg[j].file, agg[i].file) == 0) { seen = true; break; }
        if (seen) continue;

        const char *file = agg[i].file;
        AggDiag *fd = NULL;                        /* this file's diagnostics */
        int fn = 0, fcap = 0;
        for (int j = i; j < an; j++)
            if (strcmp(agg[j].file, file) == 0) DA_APPEND(fd, fn, fcap, agg[j]);

        LspDoc *od = store_find_by_path(&S->store, file);
        char *uri = uri_for_path(&S->store, file);
        const char *text;
        int tlen;
        char *owned = NULL;
        if (od) {                                  /* open: its live text */
            text = od->text;
            tlen = od->text_len;
        } else {                                   /* not open: read from disk */
            owned = read_file(file, &tlen);
            text = owned;
        }
        if (text) {                                /* (a deleted non-open file is skipped
                                                    * here and cleared via step 4) */
            emit_diagnostics(S, uri, text, tlen, fd, fn);
            DA_APPEND(now, now_n, now_cap, str_dup(uri));
        }
        free(owned);
        free(uri);
        free(fd);
    }

    /* 3. Every open document is (re)published each cycle: one with no
     *    diagnostics needs an explicit empty publish to clear a prior squiggle. */
    for (int d = 0; d < S->store.count; d++) {
        const char *uri = S->store.docs[d].uri;
        bool in_now = false;
        for (int j = 0; j < now_n; j++)
            if (strcmp(now[j], uri) == 0) { in_now = true; break; }
        if (!in_now) emit_diagnostics(S, uri, "", 0, NULL, 0);
    }

    /* 4. Clear any file published non-empty last cycle that is neither errored
     *    in this cycle nor an open doc already cleared in step 3. */
    for (int p = 0; p < S->pub_count; p++) {
        const char *uri = S->pub_uris[p];
        bool in_now = false;
        for (int j = 0; j < now_n; j++)
            if (strcmp(now[j], uri) == 0) { in_now = true; break; }
        if (in_now) continue;
        bool is_open = false;
        for (int d = 0; d < S->store.count; d++)
            if (strcmp(S->store.docs[d].uri, uri) == 0) { is_open = true; break; }
        if (!is_open) emit_diagnostics(S, uri, "", 0, NULL, 0);
    }

    /* 5. Adopt the new non-empty set. */
    for (int p = 0; p < S->pub_count; p++) free(S->pub_uris[p]);
    free(S->pub_uris);
    S->pub_uris = now;
    S->pub_count = now_n;
    S->pub_cap = now_cap;

    free(agg);
}

/* The document a unit's analysis runs with as its primary source: the first
 * open document bearing its key (the unit's other open documents are merged
 * live, so the choice only decides where unlocated diagnostics land). */
static LspDoc *primary_doc_for(LspServer *S, const char *key) {
    for (int p = 0; p < S->store.count; p++)
        if (S->store.docs[p].unit_key && strcmp(S->store.docs[p].unit_key, key) == 0)
            return &S->store.docs[p];
    return NULL;
}

static bool key_in(const char **keys, int n, const char *key) {
    for (int j = 0; j < n; j++)
        if (strcmp(keys[j], key) == 0) return true;
    return false;
}

/* Re-analyze every compilation unit that has a changed document, then publish
 * diagnostics project-wide. Called when the input queue drains and before
 * answering any type-aware request, so deferred edits are realized once per
 * quiet point and a burst of keystrokes collapses into a single analysis.
 *
 * A change in one file can create or clear errors in another file that
 * references it (a shared `prelude.fc` that `main.fc` imports), so the whole
 * unit is re-analyzed, not just the edited file. One analysis of a unit already
 * merges every open document's live buffer, so each unit is analyzed once
 * (with any of its open docs as the primary), not once per open tab. Only
 * units containing a dirty doc are re-run; untouched units keep their result.
 *
 * "Containing" means membership, not key equality. A document keys the unit it
 * would form on its own, but an lsp.rsp reaches across directories, so a shared
 * file is routinely a member of units it does not key (a library directory
 * listed by several programs' lsp.rsp files). A unit that is not re-run keeps
 * serving an analysis of the file's previous revision while queries read the
 * file's current text, so a hover would resolve a stale declaration line
 * against fresh source. The second pass below therefore re-runs every unit
 * that lists a dirty file. */
static void flush_dirty(LspServer *S) {
    bool any_dirty = false;
    for (int i = 0; i < S->store.count; i++)
        if (S->store.docs[i].dirty) { any_dirty = true; break; }
    if (!any_dirty) return;

    /* Refresh each open doc's unit identity (an lsp.rsp may have appeared/changed)
     * and its canonical path (the file may not have existed at didOpen). */
    for (int i = 0; i < S->store.count; i++) {
        char *k = unit_key(&S->store.docs[i]);
        free(S->store.docs[i].unit_key);
        S->store.docs[i].unit_key = k;
        char *r = canon_path(S->store.docs[i].path);
        free(S->store.docs[i].real);
        S->store.docs[i].real = r;
    }

    /* Analyze each dirty unit once. A unit is dirty if any of its docs changed;
     * `done` guards against analyzing a unit twice when several of its docs
     * are dirty. */
    const char **done = NULL;
    int dn = 0, dcap = 0;
    for (int i = 0; i < S->store.count; i++) {
        if (!S->store.docs[i].dirty) continue;
        const char *key = S->store.docs[i].unit_key;
        if (key_in(done, dn, key)) continue;
        DA_APPEND(done, dn, dcap, key);
        analyze_unit(S, unit_find_or_create(S, key), primary_doc_for(S, key));
    }

    /* Then every other existing unit that lists a dirty file among its sources
     * (see the comment above this function). Indexed, not pointer-held: the pass
     * above may have grown S->units. A unit only exists while some open doc keys
     * it, so a primary is always found. */
    for (int ui = 0; ui < S->unit_count; ui++) {
        const char *key = S->units[ui].key;
        if (key_in(done, dn, key)) continue;

        bool touched = false;
        for (int i = 0; i < S->store.count && !touched; i++)
            touched = S->store.docs[i].dirty &&
                      unit_has_file(&S->units[ui], S->store.docs[i].real);
        if (!touched) continue;

        LspDoc *primary = primary_doc_for(S, key);
        if (!primary) continue;
        DA_APPEND(done, dn, dcap, key);
        analyze_unit(S, &S->units[ui], primary);
    }
    free(done);

    for (int i = 0; i < S->store.count; i++) S->store.docs[i].dirty = false;

    unit_prune(S);                              /* drop units with no open docs */
    publish_project_diagnostics(S);
}

/* True if more input is already waiting, so the loop should keep reading (and
 * coalesce) rather than analyze. Relies on stdin being unbuffered (see
 * lsp_main): with no stdio read-ahead, a poll at the fd level reflects the true
 * pending state. Windows has no portable way to poll stdin, so it reports
 * "nothing pending" and every message flushes immediately, without coalescing. */
static bool input_pending(void) {
#if defined(_WIN32)
    return false;
#else
    struct pollfd p = { .fd = 0, .events = POLLIN, .revents = 0 };
    return poll(&p, 1, 0) > 0;
#endif
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
        nd.uri = str_dup(uri);
        nd.path = uri_to_path(uri);
        nd.real = canon_path(nd.path);   /* refreshed each flush; see flush_dirty */
        DA_APPEND(S->store.docs, S->store.count, S->store.cap, nd);
        doc = &S->store.docs[S->store.count - 1];
    } else {
        free(doc->text);
    }
    JsonValue *tv = json_get(td, "text");
    doc->text_len = (tv && tv->kind == JSON_STRING) ? tv->str.len : (int)strlen(text);
    doc->text = str_ndup(text, (size_t)doc->text_len);

    doc->dirty = true;   /* analyzed at the next idle flush (see lsp_main) */
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
    doc->text = str_ndup(tv->str.s, (size_t)doc->text_len);

    doc->dirty = true;   /* analyzed at the next idle flush (see lsp_main) */
}

static void handle_did_close(LspServer *S, JsonValue *params) {
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    if (!uri) return;
    bool removed = false;
    for (int i = 0; i < S->store.count; i++) {
        if (strcmp(S->store.docs[i].uri, uri) == 0) {
            LspDoc *d = &S->store.docs[i];
            free(d->uri); free(d->path); free(d->real); free(d->text); free(d->unit_key);
            S->store.docs[i] = S->store.docs[--S->store.count];
            removed = true;
            break;
        }
    }
    if (!removed) return;

    if (S->store.count > 0) {
        /* The closed file may still be a unit member on disk, whose last-saved
         * content becomes authoritative (unsaved edits are discarded). Re-analyze
         * the remaining open documents so its diagnostics reflect disk, then
         * republish project-wide: a still-broken dependency keeps its error; a
         * clean or unreferenced file is cleared by the publish cycle. Marking
         * every doc dirty re-runs each affected unit once, and close is rare. */
        for (int i = 0; i < S->store.count; i++) S->store.docs[i].dirty = true;
        flush_dirty(S);
    } else {
        /* Last document closed: nothing anchors the project view, so drop every
         * unit and clear every URI published (including the closed file's). */
        unit_prune(S);              /* no docs remain -> frees all unit analyses */
        for (int p = 0; p < S->pub_count; p++)
            emit_diagnostics(S, S->pub_uris[p], "", 0, NULL, 0);
        for (int p = 0; p < S->pub_count; p++) free(S->pub_uris[p]);
        free(S->pub_uris);
        S->pub_uris = NULL;
        S->pub_count = S->pub_cap = 0;
        emit_diagnostics(S, uri, "", 0, NULL, 0);
    }
}

/* ======================================================================== */
/* Doc comments (hover)                                                      */
/*                                                                          */
/* FC has no structured doc-comment syntax and the lexer discards comments,  */
/* so the server reads them from source text at the definition site: the    */
/* contiguous run of `//` lines directly above a definition (a `let`,       */
/* `struct`, `union` or field), plus, for a struct or union field, a `//`    */
/* trailing the field's own line.                                           */
/* ======================================================================== */

/* Resolve the source text + length for `path`: the open document, then any
 * other open buffer, then the on-disk file. Sets *out_owned when the returned
 * buffer was read from disk and must be freed by the caller. Returns NULL if
 * the file cannot be read. */
static const char *doc_file_text(LspServer *S, LspDoc *doc, const char *path,
                                 int *out_len, bool *out_owned) {
    *out_owned = false;
    if (!path || (doc->path && strcmp(path, doc->path) == 0)) {
        *out_len = doc->text_len;
        return doc->text;
    }
    LspDoc *od = store_find_by_path(&S->store, path);
    if (od) { *out_len = od->text_len; return od->text; }
    char *buf = read_file(path, out_len);
    if (!buf) return NULL;
    *out_owned = true;
    return buf;
}

/* One trimmed comment line: content after `//` (and one optional space), with
 * trailing whitespace removed. Returns false if [ls,le) is not a `// ` line. */
static bool comment_line_content(const char *text, int ls, int le,
                                 const char **out, int *out_len) {
    int s = ls;
    while (s < le && (text[s] == ' ' || text[s] == '\t')) s++;
    if (s + 1 >= le || text[s] != '/' || text[s + 1] != '/') return false;
    int cs = s + 2;
    if (cs < le && text[cs] == ' ') cs++;
    while (le > cs && (text[le - 1] == ' ' || text[le - 1] == '\t')) le--;
    *out = text + cs;
    *out_len = le - cs;
    return true;
}

/* Byte range [*ls,*le) of 1-based line `ln`, with the trailing newline trimmed. */
static void line_span(const LineIndex *idx, const char *text, int text_len,
                      int ln, int *ls, int *le) {
    *ls = idx->starts[ln - 1];
    *le = (ln < idx->count) ? idx->starts[ln] : text_len;
    while (*le > *ls && (text[*le - 1] == '\n' || text[*le - 1] == '\r')) (*le)--;
}

/* Build the doc-comment markdown for a definition on line `def_line1`, or NULL.
 * Gathers contiguous `//` lines above the definition (stopping at a blank or
 * non-comment line) in source order; when want_trailing, also appends a `//`
 * trailing the definition line (struct/union fields). */
static char *extract_doc_comment(Arena *a, const char *text, int text_len,
                                 const LineIndex *idx, int def_line1,
                                 bool want_trailing) {
    enum { MAX_LINES = 64 };
    const char *above[MAX_LINES];
    int above_len[MAX_LINES], n = 0;

    for (int ln = def_line1 - 1; ln >= 1 && n < MAX_LINES; ln--) {
        int ls, le;
        line_span(idx, text, text_len, ln, &ls, &le);
        const char *content; int clen;
        if (comment_line_content(text, ls, le, &content, &clen)) {
            above[n] = content;
            above_len[n] = clen;
            n++;
        } else {
            break;   /* blank or code line terminates the block */
        }
    }

    const char *trail = NULL; int trail_len = 0;
    if (want_trailing && def_line1 >= 1 && def_line1 <= idx->count) {
        int ls, le;
        line_span(idx, text, text_len, def_line1, &ls, &le);
        bool in_str = false;
        for (int i = ls; i + 1 < le; i++) {
            char ch = text[i];
            if (ch == '"' && (i == ls || text[i - 1] != '\\')) in_str = !in_str;
            else if (!in_str && ch == '/' && text[i + 1] == '/') {
                const char *content; int clen;
                if (comment_line_content(text, i, le, &content, &clen) && clen > 0) {
                    trail = content; trail_len = clen;
                }
                break;
            }
        }
    }

    if (n == 0 && !trail) return NULL;

    size_t need = 1;
    for (int i = 0; i < n; i++) need += (size_t)above_len[i] + 3;   /* + "  \n" hard break */
    if (trail) need += (size_t)trail_len;
    char *buf = arena_alloc(a, need);
    int off = 0;
    for (int i = n - 1; i >= 0; i--) {      /* reverse: bottom-up -> source order */
        memcpy(buf + off, above[i], (size_t)above_len[i]);
        off += above_len[i];
        if (i > 0 || trail) { buf[off++] = ' '; buf[off++] = ' '; buf[off++] = '\n'; }
    }
    if (trail) { memcpy(buf + off, trail, (size_t)trail_len); off += trail_len; }
    buf[off] = '\0';
    return buf;
}

/* ======================================================================== */
/* Handlers: hover / definition                                             */
/* ======================================================================== */

/* The doc comment at `site`, which may be in another file than the open `doc`
 * (a sibling or the stdlib): read whichever buffer backs it, reusing the open
 * document's line index `idx` when the site is in it. */
static char *doc_comment_at(LspServer *S, LspDoc *doc, const LineIndex *idx, Arena *a,
                            SrcLoc site, bool is_field) {
    int flen = 0; bool owned = false;
    const char *ftext = doc_file_text(S, doc, site.filename, &flen, &owned);
    if (!ftext) return NULL;
    LineIndex fidx = (ftext == doc->text) ? *idx : line_index_build(a, ftext, flen);
    char *md = extract_doc_comment(a, ftext, flen, &fidx, site.line, is_field);
    if (owned) free((void *)ftext);
    return md;
}

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
    /* type_ref_sym alone is enough to render: a module symbol carries no value
     * type, but its declaration-form header (`module io`) is the whole hover. */
    if (!locate(S, doc, &idx, (int)line, (int)ch, &hit) ||
        (!hit.type && !hit.builtin && !hit.decl_site && !hit.type_ref_sym)) {
        lsp_reply(a, id, json_null(a));
        return;
    }

    char *md;
    if (hit.builtin) {
        /* Built-in intrinsic: generic signature fence + prose, then the concrete
         * result type of this occurrence when it carries information (skip void
         * builtins like free/assert, and any node pass2 couldn't type). */
        const BuiltinDoc *bd = hit.builtin;
        const char *rt = NULL;
        if (hit.type && !type_is_error(hit.type) && hit.type->kind != TYPE_VOID)
            rt = dup_type_name(a, hit.type);
        md = rt ? arena_sprintf(a, "```fc\n%s\n```\n\n%s\n\n*Result type: `%s`*",
                                bd->sig, bd->doc, rt)
                : arena_sprintf(a, "```fc\n%s\n```\n\n%s", bd->sig, bd->doc);
    } else {
        /* Doc comment at the definition site. */
        char *doc_md = NULL;
        if (!hit.no_doc) {
            SrcLoc site = NO_LOC; bool site_is_field = false;
            if (hit.doc_loc.line > 0)          { site = hit.doc_loc; site_is_field = hit.doc_is_field; }
            else if (hit.def_loc.line > 0)     { site = hit.def_loc; }
            else if (hit.sym && hit.sym->decl) { site = hit.sym->decl->loc; }
            if (site.line > 0)
                doc_md = doc_comment_at(S, doc, &idx, a, site, site_is_field);
        }

        const char *nm = hit.name ? hit.name : "";

        if (hit.type_ref_sym || hit.decl_site) {
            /* The hovered token names a type or module, not a value: render a
             * declaration-form header (`enum dir of i32`, `module io`) instead
             * of the value form `name: type`. When the name is one half of a
             * companion pair, append the module's doc as a second, labeled
             * section so both halves of the pair read as one hover. */
            Decl *dd = hit.type_ref_sym ? hit.type_ref_sym->decl : hit.decl_site;
            DeclKind dk = hit.type_ref_sym ? hit.type_ref_sym->kind
                                           : hit.decl_site->kind;
            const char *header;
            if (dk == DECL_ENUM) {
                Type *repr = (dd && dd->kind == DECL_ENUM && dd->enu.repr)
                           ? dd->enu.repr : type_int32();
                header = arena_sprintf(a, "enum %s of %s", nm, type_name(repr));
            } else {
                const char *kw = dk == DECL_STRUCT ? "struct"
                               : dk == DECL_UNION  ? "union"
                               : (dd && dd->kind == DECL_MODULE &&
                                  dd->module.is_error_group) ? "error"
                               : "module";
                header = arena_sprintf(a, "%s %s", kw, nm);
            }

            /* Companion module's doc, read at its own declaration line. */
            char *comp_md = NULL;
            if (hit.companion && hit.companion->decl &&
                hit.companion->decl->loc.line > 0)
                comp_md = doc_comment_at(S, doc, &idx, a, hit.companion->decl->loc, false);

            if (hit.companion) {
                /* Render the module half whenever the pair exists: a companion
                 * without its own doc comment still gets its `module name`
                 * fence, so the pairing itself is always visible. HOVER_RULE
                 * (builtin_docs.inc) separates the two sections. */
                char *comp_sec = comp_md
                    ? arena_sprintf(a, HOVER_RULE "\n\n```fc\nmodule %s\n```\n\n%s",
                                    nm, comp_md)
                    : arena_sprintf(a, HOVER_RULE "\n\n```fc\nmodule %s\n```", nm);
                md = doc_md
                    ? arena_sprintf(a, "```fc\n%s\n```\n\n%s\n\n%s",
                                    header, doc_md, comp_sec)
                    : arena_sprintf(a, "```fc\n%s\n```\n\n%s", header, comp_sec);
            } else {
                md = doc_md ? arena_sprintf(a, "```fc\n%s\n```\n\n%s", header, doc_md)
                            : arena_sprintf(a, "```fc\n%s\n```", header);
            }
        } else {
            const char *tn = dup_type_name(a, hit.type);
            md = doc_md ? arena_sprintf(a, "```fc\n%s: %s\n```\n\n%s", nm, tn, doc_md)
                        : arena_sprintf(a, "```fc\n%s: %s\n```", nm, tn);
        }
    }

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
    if (!locate(S, doc, &idx, (int)line, (int)ch, &hit)) {
        lsp_reply(a, id, json_null(a));
        return;
    }

    /* A direct definition loc (block-local binding, plain struct field) takes
     * precedence; otherwise fall back to a resolved Symbol's declaration. */
    SrcLoc dl;
    if (hit.def_loc.line > 0)
        dl = hit.def_loc;
    else if (hit.sym && hit.sym->decl)
        dl = hit.sym->decl->loc;
    else {
        lsp_reply(a, id, json_null(a));
        return;
    }
    const char *def_path = dl.filename ? dl.filename : doc->path;
    int dline = dl.line > 0 ? dl.line : 1;
    int dcol  = dl.col  > 0 ? dl.col  : 1;

    /* Map the definition location through its own file's text (the open
     * buffer, another open buffer, or the file on disk), so a line with
     * non-ASCII text before the name gets the right UTF-16 column. The name
     * itself is ASCII, so its byte length is its UTF-16 length. */
    int dl0 = dline - 1, dc0 = dcol - 1;
    int def_len = 0;
    bool def_owned = false;
    const char *def_text = doc_file_text(S, doc, def_path, &def_len, &def_owned);
    if (def_text == doc->text) {
        loc_to_lsp(&idx, doc->text, dline, dcol, &dl0, &dc0);
    } else if (def_text) {
        LineIndex def_idx = line_index_build(a, def_text, def_len);
        loc_to_lsp(&def_idx, def_text, dline, dcol, &dl0, &dc0);
    }
    if (def_owned) free((char *)def_text);

    char *def_uri = uri_for_path(&S->store, def_path);
    JsonValue *loc = json_object(a);
    json_object_set(a, loc, "uri", json_str(a, def_uri));
    free(def_uri);
    json_object_set(a, loc, "range", mk_range(a, dl0, dc0,
                    dl0, dc0 + (hit.name ? (int)strlen(hit.name) : 1)));
    lsp_reply(a, id, loc);
}

/* ======================================================================== */
/* Handlers: CodeLens and inlay hints (each binding's inferred type)        */
/* ======================================================================== */

typedef struct {
    Arena *a;
    JsonValue *arr;
    const LineIndex *idx;
    const char *src;
    const char *file;   /* only emit hints for decls from this file */
    bool inlay;         /* false = CodeLens (type above), true = inlay hint (type inline) */
    int  lo_line, hi_line; /* inlay only: requested LSP 0-based line range (inclusive) */
} LensCtx;

/* Column (1-based) just past the binding identifier that begins at `name_col`,
 * so an inline type hint renders as `let x: T` directly after the name. */
static int name_end_col(const LineIndex *idx, const char *src, int line, int name_col) {
    int off = idx->starts[line - 1] + (name_col - 1);
    int i = off;
    while (i < idx->len) {
        char ch = src[i];
        if (id_char(ch)) i++;
        else break;
    }
    return (i - idx->starts[line - 1]) + 1;
}

static void lens_emit(LensCtx *lc, int let_line, int let_col, bool is_mut,
                      Type *type, bool init_is_lambda) {
    if (!type) return;
    const char *title;
    /* For a lambda binding the parameter types are written at the definition
     * site, so the only new information is the inferred return type: render
     * `:-> ret`. Everything else, including a plain function-reference binding
     * (`let f = g`) whose params are not visible here, shows the full type.
     * Hover always reports the full type. */
    if (init_is_lambda && type->kind == TYPE_FUNC && type->func.return_type) {
        /* Inline keeps a space after the colon to match the plain `: T` hints
         * (`let f: -> ret`); the standalone CodeLens reads fine tight (`:-> ret`). */
        title = arena_sprintf(lc->a, lc->inlay ? ": -> %s" : ":-> %s",
                              type_name(type->func.return_type));
    } else {
        title = arena_sprintf(lc->a, ": %s", type_name(type));
    }

    int name_col = let_name_col(lc->idx, lc->src, let_line, let_col, is_mut);

    if (lc->inlay) {
        /* Hint sits just after the binding name: `let x` -> `let x: T`. */
        int end_col = name_end_col(lc->idx, lc->src, let_line, name_col);
        int l0, c0;
        loc_to_lsp(lc->idx, lc->src, let_line, end_col, &l0, &c0);
        if (l0 < lc->lo_line || l0 > lc->hi_line) return;   /* outside requested range */
        JsonValue *h = json_object(lc->a);
        json_object_set(lc->a, h, "position", mk_pos(lc->a, l0, c0));
        json_object_set(lc->a, h, "label", json_str(lc->a, title));
        json_object_set(lc->a, h, "kind", json_num(lc->a, 1));   /* InlayHintKind.Type */
        json_array_push(lc->a, lc->arr, h);
        return;
    }

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

static void lens_expr(Expr *e, void *lens_ctx) {
    if (!e) return;
    if (e->kind == EXPR_LET)
        lens_emit(lens_ctx, e->loc.line, e->loc.col, e->let_expr.let_is_mut,
                  e->let_expr.let_type,
                  e->let_expr.let_init && e->let_expr.let_init->kind == EXPR_FUNC);
    expr_for_each_child(e, lens_expr, lens_ctx);
}

static void lens_decls(Decl **decls, int n, LensCtx *lc) {
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        if (!d) continue;
        /* skip decls merged in from stdlib / other files */
        if (!decl_in_file(d, lc->file)) continue;
        if (d->kind == DECL_LET) {
            lens_emit(lc, d->loc.line, d->loc.col, d->let.is_mut, d->let.resolved_type,
                      d->let.init && d->let.init->kind == EXPR_FUNC);
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
    AnalysisResult *r = doc ? query_result(S, doc) : NULL;
    if (r && r->program) {
        LineIndex idx = line_index_build(a, doc->text, doc->text_len);
        LensCtx lc = { .a = a, .arr = arr, .idx = &idx, .src = doc->text,
                       .file = doc->path };
        lens_decls(r->program->decls, r->program->decl_count, &lc);
    }
    lsp_reply(a, id, arr);
}

/* Inlay hints carry the same per-binding inferred type as the CodeLens, but
 * rendered inline after the name (`let x: T`) rather than on the line above.
 * The client (extension.js) shows at most one of the two per the `fc.typeDisplay`
 * setting; the server always offers both and lets the editor choose. */
static void handle_inlayhint(LspServer *S, JsonValue *id, JsonValue *params) {
    Arena *a = &S->msg_arena;
    JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_get_str(td, "uri");
    LspDoc *doc = uri ? store_find(&S->store, uri) : NULL;
    JsonValue *arr = json_array(a);
    AnalysisResult *r = doc ? query_result(S, doc) : NULL;
    if (r && r->program) {
        long lo = 0, hi = (1L << 30);
        JsonValue *range = json_get(params, "range");
        if (range) {
            json_get_int(json_get(range, "start"), "line", &lo);
            json_get_int(json_get(range, "end"), "line", &hi);
        }
        LineIndex idx = line_index_build(a, doc->text, doc->text_len);
        LensCtx lc = { .a = a, .arr = arr, .idx = &idx, .src = doc->text,
                       .file = doc->path, .inlay = true,
                       .lo_line = (int)lo, .hi_line = (int)hi };
        lens_decls(r->program->decls, r->program->decl_count, &lc);
    }
    lsp_reply(a, id, arr);
}

/* ======================================================================== */
/* Handler: completion                                                       */
/* ======================================================================== */

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

/* A union's variants, or an enum's variants and its `count` property (a
 * declared `count` variant takes the name, as it does in pass2). Returns
 * whether `t` was a union or an enum. */
static bool add_variant_members(Arena *a, JsonValue *arr, Type *t) {
    if (t->kind == TYPE_UNION) {
        for (int i = 0; i < t->unio.variant_count; i++)
            add_item(a, arr, t->unio.variants[i].name, CIK_ENUMMEMBER, NULL);
        return true;
    }
    if (t->kind != TYPE_ENUM) return false;
    bool has_count_variant = false;
    for (int i = 0; i < t->enu.variant_count; i++) {
        add_item(a, arr, t->enu.variants[i].name, CIK_ENUMMEMBER, NULL);
        if (strcmp(t->enu.variants[i].name, "count") == 0)
            has_count_variant = true;
    }
    if (!has_count_variant)
        add_item(a, arr, "count", CIK_FIELD, "i32");
    return true;
}

static int sym_kind_to_cik(const Symbol *s) {
    switch (s->kind) {
        case DECL_MODULE: return CIK_MODULE;
        case DECL_STRUCT: return CIK_STRUCT;
        case DECL_UNION:  return CIK_ENUM;
        case DECL_ENUM:   return CIK_ENUM;
        case DECL_LET:
            return (s->type && s->type->kind == TYPE_FUNC) ? CIK_FUNCTION : CIK_VARIABLE;
        default:          return CIK_VARIABLE;
    }
}

/* pass1 registers every struct, union and enum type a second time under its
 * mangled C name (`fc__x` at file scope, `fc__mod__x` in a module) so
 * canonicalized type stubs resolve directly (see register_type_decl in
 * pass1.c). That twin's symbol name is the type's mangled name, while the
 * user-facing entry is keyed by the source name. A twin can never be written
 * as a bare identifier, so completion skips it. Names are interned, so a
 * pointer compare suffices. */
static bool sym_is_mangled_type_twin(const Symbol *s) {
    if (!s->type) return false;
    if (s->kind == DECL_STRUCT && s->type->kind == TYPE_STRUCT)
        return s->name == s->type->struc.name;
    if (s->kind == DECL_UNION && s->type->kind == TYPE_UNION)
        return s->name == s->type->unio.name;
    if (s->kind == DECL_ENUM && s->type->kind == TYPE_ENUM)
        return s->name == s->type->enu.name;
    return false;
}

/* Emit a module's public members as completion items (used for both plain
 * module objects and a companion-typed name's module half). */
static void emit_module_members(Arena *a, JsonValue *arr, Symbol *mod) {
    SymbolTable *m = mod->members;
    for (int i = 0; i < m->count; i++) {
        if (m->symbols[i].is_private) continue;
        if (sym_is_mangled_type_twin(&m->symbols[i])) continue;
        const char *detail = m->symbols[i].type
            ? dup_type_name(a, m->symbols[i].type) : NULL;
        add_item(a, arr, m->symbols[i].name, sym_kind_to_cik(&m->symbols[i]), detail);
    }
}

/* Peel options, results and pointers (up to four layers) to reach the struct,
 * union or enum a member access applies to. Returns the first type that is
 * none of those wrappers. */
static Type *peel_to_aggregate(Type *t) {
    for (int i = 0; t && i < 4; i++) {
        if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION ||
            t->kind == TYPE_ENUM) return t;
        if (t->kind == TYPE_OPTION)  { t = t->option.inner; continue; }
        if (t->kind == TYPE_RESULT)  { t = t->result.inner; continue; }
        if (t->kind == TYPE_POINTER) { t = t->pointer.pointee; continue; }
        break;
    }
    return t;
}

/* The identifiers a function mentions or binds, deduplicated by interned
 * pointer. Completion offers them as the function's locals. */
typedef struct {
    const char **names;
    int count;
    int cap;
} NameList;

static void harvest_add(NameList *l, const char *name) {
    if (!name) return;
    for (int i = 0; i < l->count; i++) if (l->names[i] == name) return;
    DA_APPEND(l->names, l->count, l->cap, name);
}

static void harvest_pattern(Pattern *p, void *list) {
    if (p->kind == PAT_BINDING) harvest_add(list, p->binding.name);
    pattern_for_each_child(p, harvest_pattern, list);
}

static void harvest_expr(Expr *e, void *list) {
    if (!e) return;
    NameList *l = list;
    switch (e->kind) {
    case EXPR_IDENT:
        harvest_add(l, e->ident.name);
        break;
    case EXPR_LET:
        harvest_add(l, e->let_expr.let_name);
        break;
    case EXPR_LET_DESTRUCT:
        if (e->let_destruct.pattern) harvest_pattern(e->let_destruct.pattern, l);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.param_count; i++)
            harvest_add(l, e->func.params[i].name);
        break;
    case EXPR_FOR:
        harvest_add(l, e->for_expr.var);
        harvest_add(l, e->for_expr.index_var);
        if (e->for_expr.var_pattern) harvest_pattern(e->for_expr.var_pattern, l);
        break;
    case EXPR_MATCH:
        for (int i = 0; i < e->match_expr.arm_count; i++)
            if (e->match_expr.arms[i].pattern)
                harvest_pattern(e->match_expr.arms[i].pattern, l);
        break;
    default:
        break;
    }
    expr_for_each_child(e, harvest_expr, list);
}

/* Synthetic, type-level properties of a primitive numeric type name accessed
 * like a module: i32.min/max/bits, f64.nan/inf/neg_inf/epsilon, etc. The list
 * should match what type_property_c (types.c) accepts. Returns true iff `tn` is
 * an integer/float type (the object was a numeric type name), so the caller
 * stops here. */
static bool complete_type_properties(Arena *a, JsonValue *arr, Type *tn) {
    if (!tn) return false;
    bool is_int = type_is_integer(tn), is_float = type_is_float(tn);
    if (!is_int && !is_float) return false;          /* bool/char/str/etc.: none */
    const char *ty = dup_type_name(a, tn);
    add_item(a, arr, "bits", CIK_FIELD, "i32");       /* bit width */
    add_item(a, arr, "min",  CIK_FIELD, ty);          /* smallest value */
    add_item(a, arr, "max",  CIK_FIELD, ty);          /* largest value */
    if (is_float) {
        add_item(a, arr, "epsilon", CIK_FIELD, ty);
        add_item(a, arr, "nan",     CIK_FIELD, ty);
        add_item(a, arr, "inf",     CIK_FIELD, ty);
        add_item(a, arr, "neg_inf", CIK_FIELD, ty);
    }
    return true;
}

/* Member completion after '.' or '::'. `dot_byte` is the position of the
 * operator's first char; the object expression ends at dot_byte-1. Offers, by
 * object kind: a numeric type name's properties; module members; a type
 * name's companion-module members and variants; a slice's len/ptr; an
 * option's is_some/is_none; a result's is_ok/is_err; a struct's fields; a
 * union's or enum's variants. A '.' on a pointer auto-derefs one level to the
 * pointee's members. */
static bool complete_members(LspServer *S, LspDoc *doc, const LineIndex *idx,
                             int dot_byte, JsonValue *arr) {
    Arena *a = &S->msg_arena;
    int anchor = dot_byte - 1;
    if (anchor < 0) return false;

    /* Numeric type-name properties (i32.max, f64.nan). The object before a '.'
     * is a reserved type keyword, never a binding, so read it from source
     * without resolving a node. */
    const char *txt = doc->text;
    int s = anchor;
    while (s >= 0 && id_char(txt[s])) s--;
    s++;
    if (anchor >= s)
        if (complete_type_properties(a, arr, type_from_name(txt + s, anchor - s + 1)))
            return true;

    /* One AST walk resolves two things: the anchor node (the object's last char,
     * for simple `a.b` / `::` paths) and, via the op-position hook, the field
     * node at the operator, whose `field.object` gives the object type for any
     * shape (`dict[i]`, `f()`, nested); the anchor can't when the object ends
     * in `]` or `)`. */
    int line0 = 0;
    for (int i = 0; i < idx->count; i++)
        if (idx->starts[i] <= anchor) line0 = i; else break;
    int oline0 = 0;
    for (int i = 0; i < idx->count; i++)
        if (idx->starts[i] <= dot_byte) oline0 = i; else break;
    FindCtx c = {0};
    c.target_line = line0 + 1;
    c.target_col = anchor - idx->starts[line0] + 1;
    c.op_line = oline0 + 1;
    c.op_col = dot_byte - idx->starts[oline0] + 1;
    c.src = doc->text;
    c.idx = idx;
    c.file = doc->path;
    AnalysisResult *r = query_result(S, doc);
    if (!r || !r->program) return false;
    find_in_decls(r->program->decls, r->program->decl_count, &c);

    Expr *obj = c.op_field ? c.op_field->field.object : NULL;

    /* Module members ('.'/'::'): the object resolves to a module. Either the
     * anchor landed on the module name ('::' paths, simple `mod.`), or the
     * field node's object is a module-typed ident or a nested `a.b` module
     * member. (A union with a companion module keeps its variants below:
     * resolved_sym is the union, not the module, so `mod` stays NULL here.) */
    Symbol *mod = NULL;
    if (c.sym && c.sym->kind == DECL_MODULE) mod = c.sym;
    else if (obj && obj->kind == EXPR_IDENT && obj->ident.resolved_sym &&
             obj->ident.resolved_sym->kind == DECL_MODULE)
        mod = obj->ident.resolved_sym;
    else if (obj && obj->kind == EXPR_FIELD && obj->field.resolved_member &&
             obj->field.resolved_member->kind == DECL_MODULE)
        mod = obj->field.resolved_member;
    if (mod && mod->members) {
        emit_module_members(a, arr, mod);
        return true;
    }

    /* Type-name object (an ident or module path resolving to a struct/union/
     * enum): what pass2 accepts after the '.' is the companion module's
     * members (`point.origin`) and, for unions/enums, variant constructors;
     * never the struct's fields (those need a value, so `point.x` is a
     * compile error). Offer the same here instead of falling through to the
     * value dispatch below, which would offer the fields. */
    Symbol *tsym = NULL;
    if (obj && obj->kind == EXPR_IDENT && is_type_sym(obj->ident.resolved_sym))
        tsym = obj->ident.resolved_sym;
    else if (obj && obj->kind == EXPR_FIELD && is_type_sym(obj->field.resolved_member) &&
             !obj->field.is_variant_constructor && !obj->field.is_type_property)
        tsym = obj->field.resolved_member;
    else if (!obj && is_type_sym(c.sym))
        tsym = c.sym;
    if (tsym) {
        Symbol *comp = (obj && obj->kind == EXPR_IDENT)
                     ? obj->ident.companion_module : NULL;
        if (!comp) comp = companion_of_type_sym(r, tsym);
        if (comp && comp->members)
            emit_module_members(a, arr, comp);
        if (tsym->type) add_variant_members(a, arr, tsym->type);
        return true;
    }

    /* Value dispatch on the object's type: the field node's object (any shape),
     * else the anchored node (simple idents the field capture didn't reach). */
    Type *t = (obj && obj->type) ? obj->type : c.type;
    /* '.' auto-derefs one pointer level (pass2 rewrites `.` on a pointer to
     * EXPR_DEREF_FIELD). Numeric type names and modules were handled above, so
     * a pointer type here is always a value's. */
    if (t && t->kind == TYPE_POINTER)
        t = t->pointer.pointee;
    if (!t) return false;

    /* Slice fat-pointer fields (covers str = u8[]). */
    if (t->kind == TYPE_SLICE) {
        add_item(a, arr, "len", CIK_FIELD, "i64");
        add_item(a, arr, "ptr", CIK_FIELD,
                 arena_sprintf(a, "%s*", t->slice.elem ? type_name(t->slice.elem)
                                                       : "any"));
        return true;
    }
    /* Option discriminant fields (the value itself needs `!` to unwrap). */
    if (t->kind == TYPE_OPTION) {
        add_item(a, arr, "is_some", CIK_FIELD, "bool");
        add_item(a, arr, "is_none", CIK_FIELD, "bool");
        return true;
    }
    /* Result discriminant fields (the value itself needs `!` or match). */
    if (t->kind == TYPE_RESULT) {
        add_item(a, arr, "is_ok", CIK_FIELD, "bool");
        add_item(a, arr, "is_err", CIK_FIELD, "bool");
        return true;
    }
    /* Struct fields (tuples are indexed, not named). */
    if (t->kind == TYPE_STRUCT && !t->struc.is_tuple) {
        for (int i = 0; i < t->struc.field_count; i++)
            add_item(a, arr, t->struc.fields[i].name, CIK_FIELD,
                     dup_type_name(a, t->struc.fields[i].type));
        return true;
    }
    return add_variant_members(a, arr, t);
}

/* Dedup set over interned names (all symbol/identifier strings share the
 * analysis intern table, so pointer compare suffices). Returns false if `name`
 * was already present. */
typedef struct { const char **names; int n, cap; } NameSet;
static bool nameset_add(NameSet *s, const char *name) {
    if (!name) return false;
    for (int i = 0; i < s->n; i++) if (s->names[i] == name) return false;
    DA_APPEND(s->names, s->n, s->cap, name);
    return true;
}

static int import_kind_to_cik(DeclKind k) {
    switch (k) {
        case DECL_MODULE: return CIK_MODULE;
        case DECL_STRUCT: return CIK_STRUCT;
        case DECL_UNION:  return CIK_ENUM;
        case DECL_ENUM:   return CIK_ENUM;
        default:          return CIK_VARIABLE;
    }
}

/* Find the module Symbol for a module Decl within `scope` (the global symtab, or
 * a parent module's member table). Match by decl pointer to sidestep name/ns
 * ambiguity. */
static Symbol *module_sym_for(SymbolTable *scope, const Decl *d) {
    if (!scope) return NULL;
    for (int i = 0; i < scope->count; i++)
        if (scope->symbols[i].kind == DECL_MODULE && scope->symbols[i].decl == d)
            return &scope->symbols[i];
    return NULL;
}

static void emit_imports(Arena *a, JsonValue *items, NameSet *seen,
                         ImportTable *imp) {
    if (!imp) return;
    for (int i = 0; i < imp->count; i++) {
        ImportRef *ref = &imp->entries[i];
        if (!nameset_add(seen, ref->local_name)) continue;
        add_item(a, items, ref->local_name, import_kind_to_cik(ref->kind), NULL);
    }
}

/* Add the bare names in scope at `target_line`: walk the decl tree by line span
 * (open-file decls only), emitting each enclosing module's members + imports and
 * then, for the innermost enclosing function, its locals (params/lets/for-vars).
 * `scope` is the symbol table whose module decls in `decls` resolve through.
 * This follows FC's lexical resolution: a module's siblings are visible bare
 * within it, and a function's bindings within its body. */
static void complete_scope(Arena *a, JsonValue *items, NameSet *seen,
                           Decl **decls, int n, SymbolTable *scope,
                           const char *file, int target_line) {
    /* The container is the last decl from this file that starts at or before the
     * cursor (decls are in source order, so its span reaches the next sibling).
     * Require a matching filename and a real (1-based) line: the merged program
     * mixes in other files' decls and synthetic, line-0 / NULL-filename decls
     * (e.g. a stdlib `namespace std` wrapper) that must not win the line race. */
    Decl *enc = NULL;
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        if (!d) continue;
        if (!decl_in_file(d, file)) continue;
        if (d->loc.line <= 0 || d->loc.line > target_line) continue;
        enc = d;
    }
    if (!enc) return;

    if (enc->kind == DECL_MODULE) {
        Symbol *ms = module_sym_for(scope, enc);
        if (!ms) return;
        if (ms->members) {
            SymbolTable *m = ms->members;
            for (int i = 0; i < m->count; i++) {
                if (m->symbols[i].is_private) continue;
                if (sym_is_mangled_type_twin(&m->symbols[i])) continue;
                if (!nameset_add(seen, m->symbols[i].name)) continue;
                add_item(a, items, m->symbols[i].name,
                         sym_kind_to_cik(&m->symbols[i]),
                         m->symbols[i].type ? dup_type_name(a, m->symbols[i].type)
                                            : NULL);
            }
        }
        emit_imports(a, items, seen, ms->imports);
        /* Descend: a nested module/function may further narrow the scope. */
        complete_scope(a, items, seen, enc->module.decls, enc->module.decl_count,
                       ms->members, file, target_line);
    } else if (enc->kind == DECL_LET) {
        /* The enclosing function: harvest its bindings (and referenced names). */
        NameList locals = {0};
        harvest_expr(enc->let.init, &locals);
        for (int i = 0; i < locals.count; i++)
            if (nameset_add(seen, locals.names[i]))
                add_item(a, items, locals.names[i], CIK_VARIABLE, NULL);
        free(locals.names);
    }
}

/* ======================================================================== */
/* Import-statement completion                                              */
/* ======================================================================== */

/* An import statement is its own resolution world. What may be written on the
 * left of `from` is decided entirely by the `from` clause (a module's members,
 * or a namespace's top-level symbols), and what may be written on the right is
 * a route of modules, not an expression. So an import line is completed here
 * in full, including its `.` and `::` positions, which must never reach
 * complete_members, and it offers only these candidates or none: the
 * lexical-scope list is meaningless here.
 *
 * The statement is read from the source line rather than the AST. A half-typed
 * import (`import x from `, `from a.`) is the state the parser recovers from,
 * so its Decl is either a DECL_ERROR or is missing the part being completed.
 * The lookups the text then drives are the ones pass1 performs
 * (process_member_import and the import resolution it is called from), read
 * out of the symbol table.
 *
 * Known limit: the `from` clause decides the name list, so until it is written
 * there is nothing to resolve names against and the left side offers nothing. */

/* A name as written in the document (not interned, so no pointer compare). */
typedef struct { const char *s; int n; } TextRef;


static TextRef tr_of(const char *s) {
    TextRef t = { s, (int)strlen(s) };
    return t;
}
static bool tr_eq_str(TextRef t, const char *name) {
    return name && t.n > 0 && (int)strlen(name) == t.n &&
           memcmp(t.s, name, (size_t)t.n) == 0;
}

/* Dedup set over written text (NameSet's pointer compare needs interned names,
 * which the text scanned out of the buffer is not). */
typedef struct { TextRef *v; int n, cap; } TextSet;
static bool tset_add(TextSet *s, TextRef t) {
    for (int i = 0; i < s->n; i++)
        if (s->v[i].n == t.n && memcmp(s->v[i].s, t.s, (size_t)t.n) == 0) return false;
    DA_APPEND(s->v, s->n, s->cap, t);
    return true;
}

/* A `from` clause as written: `[ns::[ns::]...][head][.seg...]`. */
typedef struct {
    char    *ns;        /* mangled namespace path ("a__b"); NULL when unwritten */
    TextRef  head;      /* head module; .n == 0 for a bare `from ns::` */
    TextRef *segs;      /* dotted route segments after the head */
    int      seg_count, seg_cap;
} FromText;

static void from_text_free(FromText *f) { free(f->ns); free(f->segs); }

/* Parse the text in [s,e) as a from clause, as parse_from_clause does: parts
 * separated by `::` are namespace segments, except a trailing one not followed
 * by `::`, which is the head module; a `.` route may follow the head. Every
 * component may be empty (the cursor is mid-typing), which the callers read as
 * "unwritten". */
static void from_text_parse(const char *s, const char *e, FromText *out) {
    memset(out, 0, sizeof *out);
    const char *p = s;
    for (;;) {
        while (p < e && isspace((unsigned char)*p)) p++;
        const char *w = p;
        while (p < e && id_char(*p)) p++;
        TextRef part = { w, (int)(p - w) };
        const char *q = p;
        while (q < e && isspace((unsigned char)*q)) q++;
        if (q + 1 < e && q[0] == ':' && q[1] == ':') {
            if (part.n)
                out->ns = str_appendf(out->ns, out->ns ? "__%.*s" : "%.*s",
                                      part.n, part.s);
            p = q + 2;
            continue;
        }
        out->head = part;
        p = q;
        break;
    }
    while (p < e && *p == '.') {
        p++;
        while (p < e && isspace((unsigned char)*p)) p++;
        const char *w = p;
        while (p < e && id_char(*p)) p++;
        TextRef seg = { w, (int)(p - w) };
        DA_APPEND(out->segs, out->seg_count, out->seg_cap, seg);
        while (p < e && isspace((unsigned char)*p)) p++;
    }
}

/* Namespace prefixes are interned, but a prefix typed into the buffer is not,
 * so namespace identity compares by string here. Both NULL = global. */
static bool ns_eq(const char *sym_ns, const char *ns) {
    if (!sym_ns || !ns) return sym_ns == ns;
    return strcmp(sym_ns, ns) == 0;
}

static Symbol *find_top_module(SymbolTable *st, TextRef name, const char *ns) {
    for (int i = 0; i < st->count; i++) {
        Symbol *s = &st->symbols[i];
        if (s->kind != DECL_MODULE || !tr_eq_str(name, s->name)) continue;
        if (ns_eq(s->ns_prefix, ns)) return s;
    }
    return NULL;
}

static Symbol *find_member_module(SymbolTable *members, TextRef name) {
    if (!members) return NULL;
    for (int i = 0; i < members->count; i++)
        if (members->symbols[i].kind == DECL_MODULE &&
            tr_eq_str(name, members->symbols[i].name))
            return &members->symbols[i];
    return NULL;
}

/* The module a written `from` clause reads from, counting route segments
 * [0,upto), resolved as process_member_import does: the head resolves in the
 * namespace written before `::` (else the enclosing one), falling back to a
 * whole-module import already in this scope; every further segment is a
 * module member of its predecessor. NULL when any part is unwritten or
 * unresolved. */
static Symbol *resolve_from_text(AnalysisResult *r, ImportTable *scope_imports,
                                 const char *cur_ns, const FromText *f, int upto) {
    if (f->head.n == 0) return NULL;
    const char *ns = f->ns ? f->ns : cur_ns;
    Symbol *mod = find_top_module(&r->symtab, f->head, ns);
    if (!mod && scope_imports) {
        for (int i = 0; i < scope_imports->count; i++) {
            ImportRef *ref = &scope_imports->entries[i];
            if (ref->kind != DECL_MODULE || !ref->sym->members) continue;
            if (!tr_eq_str(f->head, ref->local_name)) continue;
            mod = ref->sym;
            break;
        }
    }
    for (int i = 0; mod && i < upto; i++)
        mod = find_member_module(mod->members, f->segs[i]);
    return mod;
}

/* The innermost module whose body contains `line` (NULL at file level). The
 * same line-span walk complete_scope uses, kept separate because this one
 * wants the module Symbol rather than its members. */
static Symbol *enclosing_module_at(Decl **decls, int n, SymbolTable *scope,
                                   const char *file, int line) {
    Decl *enc = NULL;
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        if (!d) continue;
        if (!decl_in_file(d, file)) continue;
        if (d->loc.line <= 0 || d->loc.line > line) continue;
        enc = d;
    }
    if (!enc || enc->kind != DECL_MODULE) return NULL;
    Symbol *ms = module_sym_for(scope, enc);
    if (!ms) return NULL;
    Symbol *inner = enclosing_module_at(enc->module.decls, enc->module.decl_count,
                                        ms->members, file, line);
    return inner ? inner : ms;
}

/* The namespace in force at `line` of `file` (the `current_ns` pass1 resolves
 * the file's imports in). */
static const char *file_ns_at(AnalysisResult *r, const char *file, int line) {
    const char *ns = NULL;
    if (!r->program) return NULL;
    for (int i = 0; i < r->program->decl_count; i++) {
        Decl *d = r->program->decls[i];
        if (!d || d->kind != DECL_NAMESPACE) continue;
        if (!decl_in_file(d, file)) continue;
        if (d->loc.line <= 0 || d->loc.line > line) continue;
        ns = d->ns.name;
    }
    return ns;
}

static ImportTable *file_imports_for(AnalysisResult *r, const char *file) {
    for (int i = 0; i < r->file_scopes.count; i++) {
        FileImportScope *fs = &r->file_scopes.scopes[i];
        if (fs->filename && file && strcmp(fs->filename, file) != 0) continue;
        return &fs->imports;
    }
    return NULL;
}

/* Namespace segments that may follow `prefix` (NULL = the first segment of every
 * namespace). Paths are stored mangled (`acme::graphics` as `acme__graphics`), so
 * a segment runs to the next `__`. */
static void emit_ns_segments(Arena *a, JsonValue *items, TextSet *seen,
                             SymbolTable *st, const char *prefix) {
    size_t plen = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < st->count; i++) {
        const char *ns = st->symbols[i].ns_prefix;
        if (!ns) continue;
        const char *seg = ns;
        if (prefix) {
            if (strncmp(ns, prefix, plen) != 0 || ns[plen] != '_' || ns[plen + 1] != '_')
                continue;
            seg = ns + plen + 2;
        }
        const char *end = strstr(seg, "__");
        TextRef t = { seg, end ? (int)(end - seg) : (int)strlen(seg) };
        if (t.n == 0 || !tset_add(seen, t)) continue;
        add_item(a, items, arena_sprintf(a, "%.*s", t.n, t.s), CIK_MODULE, "namespace");
    }
}

static void emit_modules_in_ns(Arena *a, JsonValue *items, TextSet *seen,
                               SymbolTable *st, const char *ns) {
    for (int i = 0; i < st->count; i++) {
        Symbol *s = &st->symbols[i];
        if (s->kind != DECL_MODULE || s->is_private || !s->name) continue;
        if (!ns_eq(s->ns_prefix, ns)) continue;
        if (!tset_add(seen, tr_of(s->name))) continue;
        add_item(a, items, s->name, CIK_MODULE, NULL);
    }
}

static void emit_nested_modules(Arena *a, JsonValue *items, TextSet *seen,
                                Symbol *mod) {
    SymbolTable *m = mod->members;
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        Symbol *s = &m->symbols[i];
        if (s->kind != DECL_MODULE || s->is_private || !s->name) continue;
        if (!tset_add(seen, tr_of(s->name))) continue;
        add_item(a, items, s->name, CIK_MODULE, NULL);
    }
}

/* Modules a whole-module import already brought into this scope: heads pass1
 * accepts for a `from` clause alongside the symbol table's own. */
static void emit_import_modules(Arena *a, JsonValue *items, TextSet *seen,
                                ImportTable *imp) {
    if (!imp) return;
    for (int i = 0; i < imp->count; i++) {
        ImportRef *ref = &imp->entries[i];
        if (ref->kind != DECL_MODULE || !ref->sym->members) continue;
        if (!tset_add(seen, tr_of(ref->local_name))) continue;
        add_item(a, items, ref->local_name, CIK_MODULE, NULL);
    }
}

/* Candidates for `import <name> from mod`: the module's public members. */
static void emit_module_import_names(Arena *a, JsonValue *items, TextSet *used,
                                     Symbol *mod) {
    SymbolTable *m = mod->members;
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        Symbol *s = &m->symbols[i];
        if (s->is_private || !s->name) continue;
        if (sym_is_mangled_type_twin(s)) continue;
        if (!tset_add(used, tr_of(s->name))) continue;
        add_item(a, items, s->name, sym_kind_to_cik(s),
                 s->type ? dup_type_name(a, s->type) : NULL);
    }
}

/* Candidates for `import <name> from ns::`, which names a whole top-level
 * symbol rather than a member: a module, struct, union or enum (a companion
 * pair is two symbols of one name, hence the dedup). */
static void emit_ns_import_names(Arena *a, JsonValue *items, TextSet *used,
                                 SymbolTable *st, const char *ns) {
    for (int i = 0; i < st->count; i++) {
        Symbol *s = &st->symbols[i];
        if (s->is_private || !s->name) continue;
        if (s->kind != DECL_MODULE && s->kind != DECL_STRUCT &&
            s->kind != DECL_UNION && s->kind != DECL_ENUM) continue;
        if (!ns_eq(s->ns_prefix, ns)) continue;
        if (sym_is_mangled_type_twin(s)) continue;
        if (!tset_add(used, tr_of(s->name))) continue;
        add_item(a, items, s->name, sym_kind_to_cik(s), NULL);
    }
}

/* True if the standalone keyword `kw` appears in [s,e) of `txt`. */
static bool has_word(const char *txt, int s, int e, const char *kw) {
    int n = (int)strlen(kw);
    for (int i = s; i + n <= e; i++) {
        if (strncmp(txt + i, kw, (size_t)n) != 0) continue;
        if (i > 0 && id_char(txt[i - 1])) continue;
        if (i + n < e && id_char(txt[i + n])) continue;
        return true;
    }
    return false;
}

/* The first identifier written in [s,e): an import item's bound name, the part
 * before any `as`. .n == 0 when the item is empty or starts with `*`. */
static TextRef first_ident(const char *txt, int s, int e) {
    while (s < e && isspace((unsigned char)txt[s])) s++;
    int w = s;
    while (s < e && id_char(txt[s])) s++;
    TextRef t = { txt + w, s - w };
    return t;
}

/* Complete inside the import statement on `line1` (1-based), with the cursor at
 * byte `cur`. Returns true when the cursor is in an import statement; the reply
 * is then only what this appended, possibly nothing. */
static bool complete_import(LspServer *S, LspDoc *doc, const LineIndex *idx,
                            int line1, int cur, JsonValue *items) {
    Arena *a = &S->msg_arena;
    const char *txt = doc->text;
    int ls = idx->starts[line1 - 1];
    int le = (line1 < idx->count) ? idx->starts[line1] : idx->len;
    while (le > ls && (txt[le - 1] == '\n' || txt[le - 1] == '\r')) le--;
    if (cur < ls || cur > le) return false;

    /* An import statement holds no string literal, so a `//` on the line can
     * only start a comment: the statement ends there, and a cursor past it is
     * in the comment, which is nobody's context (the fall-through's global list
     * is what every other comment in the file gets). */
    for (int i = ls; i + 1 < le; i++)
        if (txt[i] == '/' && txt[i + 1] == '/') { le = i; break; }
    if (cur > le) return false;

    int p = ls;
    while (p < le && (txt[p] == ' ' || txt[p] == '\t')) p++;
    if (le - p < 6 || strncmp(txt + p, "import", 6) != 0) return false;
    int kw_end = p + 6;
    if (kw_end < le && id_char(txt[kw_end])) return false;
    /* Inside the `import` keyword itself the word being typed is the keyword. */
    if (cur <= kw_end) return false;

    /* `from` is a keyword, so no written name can spell it: the first standalone
     * occurrence is the clause separator. */
    int from_start = -1, from_end = -1;
    for (int i = kw_end; i + 4 <= le; i++) {
        if (strncmp(txt + i, "from", 4) != 0) continue;
        if (id_char(txt[i - 1])) continue;
        if (i + 4 < le && id_char(txt[i + 4])) continue;
        from_start = i; from_end = i + 4;
        break;
    }
    /* Cursor inside the `from` keyword: neither side, and nothing to offer. */
    if (from_start >= 0 && cur > from_start && cur <= from_end) return true;

    AnalysisResult *r = query_result(S, doc);
    if (!r) return true;

    Symbol *encl = r->program
        ? enclosing_module_at(r->program->decls, r->program->decl_count,
                              &r->symtab, doc->path, line1)
        : NULL;
    const char *cur_ns = encl ? encl->ns_prefix : file_ns_at(r, doc->path, line1);
    ImportTable *scope_imports = encl ? encl->imports
                                      : file_imports_for(r, doc->path);
    TextSet seen = {0};

    if (from_start >= 0 && cur > from_end) {
        /* Route region: the separator immediately left of the word being typed
         * decides which continuation is legal. */
        int w = cur;
        while (w > from_end && id_char(txt[w - 1])) w--;
        int q = w;
        while (q > from_end && (txt[q - 1] == ' ' || txt[q - 1] == '\t')) q--;

        if (q > from_end && txt[q - 1] == '.') {
            /* `from head[.seg...].`: the nested modules of what precedes it. */
            FromText f;
            from_text_parse(txt + from_end, txt + (q - 1), &f);
            Symbol *mod = resolve_from_text(r, scope_imports, cur_ns, &f,
                                            f.seg_count);
            if (mod) emit_nested_modules(a, items, &seen, mod);
            from_text_free(&f);
        } else if (q - 1 > from_end && txt[q - 1] == ':' && txt[q - 2] == ':') {
            /* `from ns::`: modules of that namespace, and its next segment.
             * The `::` stays inside the parsed text so the whole path reads as
             * a namespace rather than the last part reading as a head. */
            FromText f;
            from_text_parse(txt + from_end, txt + q, &f);
            emit_modules_in_ns(a, items, &seen, &r->symtab, f.ns);
            emit_ns_segments(a, items, &seen, &r->symtab, f.ns);
            from_text_free(&f);
        } else {
            /* The head: modules visible where pass1 looks one up, plus the
             * namespaces a `::` path may start with. */
            emit_modules_in_ns(a, items, &seen, &r->symtab, cur_ns);
            emit_import_modules(a, items, &seen, scope_imports);
            emit_ns_segments(a, items, &seen, &r->symtab, NULL);
        }
        free(seen.v);
        return true;
    }

    /* Name region: `*` | name [as alias] {, name [as alias]}. */
    int reg_end = (from_start >= 0) ? from_start : le;
    if (cur > reg_end) cur = reg_end;          /* trailing space before `from` */
    int istart = kw_end;
    for (int i = kw_end; i < cur; i++)
        if (txt[i] == ',') istart = i + 1;
    for (int i = kw_end; i < cur; i++)
        if (txt[i] == '*') { free(seen.v); return true; }   /* wildcard: no names */
    /* Past an `as`, the word being typed is a new name being introduced. */
    if (has_word(txt, istart, cur, "as")) { free(seen.v); return true; }

    if (from_start >= 0) {
        /* Names already written in the list are not offered again; the item
         * under the cursor does not count as written. */
        int item = kw_end;
        for (int i = kw_end; i <= reg_end; i++) {
            if (i != reg_end && txt[i] != ',') continue;
            if (item != istart) {
                TextRef t = first_ident(txt, item, i);
                if (t.n) tset_add(&seen, t);
            }
            item = i + 1;
        }
        FromText f;
        from_text_parse(txt + from_end, txt + le, &f);
        if (f.head.n) {
            Symbol *mod = resolve_from_text(r, scope_imports, cur_ns, &f,
                                            f.seg_count);
            if (mod) emit_module_import_names(a, items, &seen, mod);
        } else if (f.ns) {
            emit_ns_import_names(a, items, &seen, &r->symtab, f.ns);
        }
        from_text_free(&f);
    }
    free(seen.v);
    return true;
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

    /* CompletionContext.triggerKind: 1=Invoked (Ctrl+Space or typing a word),
     * 2=TriggerCharacter, 3=re-trigger. An auto-pop by a trigger character
     * should only surface member completion: if the cursor isn't at a member
     * access (a lone `:`, a float-literal `.`), stay quiet instead of listing
     * every global. A client that sends no context defaults to Invoked, which
     * falls through to scope completion. */
    long trig_kind = 1;
    JsonValue *cctx = json_get(params, "context");
    if (cctx) json_get_int(cctx, "triggerKind", &trig_kind);
    bool auto_trigger = (trig_kind == 2);

    LineIndex idx = line_index_build(a, doc->text, doc->text_len);
    int line1, col1;
    lsp_to_loc(&idx, doc->text, (int)line, (int)ch, &line1, &col1);
    int cur = loc_byte_offset(&idx, line1, col1);

    /* An import statement resolves against its `from` clause, not lexical scope,
     * so it is completed in full before anything below sees it: a '.'/'::' in an
     * import route is a module path, never an expression, and an import line
     * must never fall through to the global list. */
    if (complete_import(S, doc, &idx, line1, cur, items)) {
        lsp_reply(a, id, items);
        return;
    }

    /* Member context: scan back over an in-progress member name to the operator
     * just before the object, '.' or '::'. dot_byte is the operator's first
     * byte, so the object ends at dot_byte-1 either way. */
    int b = cur - 1;
    while (b >= 0 && id_char(doc->text[b])) b--;
    int dot_byte = -1;
    if (b >= 0 && doc->text[b] == '.') dot_byte = b;
    else if (b >= 1 && doc->text[b] == ':' && doc->text[b - 1] == ':') dot_byte = b - 1;

    /* Member access offers only the object's members, never the global list. */
    if (dot_byte >= 0) {
        complete_members(S, doc, &idx, dot_byte, items);
        lsp_reply(a, id, items);
        return;
    }
    /* Right after `->` (a lambda body, function type or match arm) there is
     * nothing to offer. Checked regardless of triggerKind: once VSCode has a
     * suggest session open from word typing, it re-queries as Invoked while
     * the user types through `->`, and the global list would show up there. */
    if (b >= 1 && doc->text[b] == '>' && doc->text[b - 1] == '-') {
        lsp_reply(a, id, items);
        return;
    }
    /* Not a member context. A trigger character that lands here (a lone `:`
     * that isn't `::`) has nothing to offer, so an auto-pop stays quiet; an
     * explicit invoke or word typing falls through to scope completion. */
    if (auto_trigger) { lsp_reply(a, id, items); return; }

    /* Global context: keywords, then everything in lexical scope at the cursor:
     * enclosing-module members + imports and the enclosing function's locals
     * (complete_scope), file-level imports, and top-level symbols. A NameSet
     * dedups across all of these so a name in scope two ways is offered once. */
    for (int i = 0; i < lexer_keyword_count(); i++)
        add_item(a, items, lexer_keyword(i), CIK_KEYWORD, NULL);
    for (int i = 0; i < type_primitive_count(); i++)
        add_item(a, items, type_primitive_name(i), CIK_KEYWORD, NULL);

    AnalysisResult *r = query_result(S, doc);
    if (r) {
        NameSet seen = {0};

        /* In-scope module members / imports / function locals (innermost-first). */
        if (r->program)
            complete_scope(a, items, &seen, r->program->decls,
                           r->program->decl_count, &r->symtab, doc->path, line1);

        /* File-level imports visible to this file. */
        for (int i = 0; i < r->file_scopes.count; i++) {
            FileImportScope *fs = &r->file_scopes.scopes[i];
            if (fs->filename && doc->path && strcmp(fs->filename, doc->path) != 0)
                continue;
            emit_imports(a, items, &seen, &fs->imports);
        }

        /* Top-level (global) declarations. */
        SymbolTable *st = &r->symtab;
        for (int i = 0; i < st->count; i++) {
            if (st->symbols[i].is_private) continue;
            if (!st->symbols[i].name) continue;
            if (sym_is_mangled_type_twin(&st->symbols[i])) continue;
            if (!nameset_add(&seen, st->symbols[i].name)) continue;
            add_item(a, items, st->symbols[i].name, sym_kind_to_cik(&st->symbols[i]),
                     st->symbols[i].type ? dup_type_name(a, st->symbols[i].type)
                                         : NULL);
        }
        free(seen.names);
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
    json_object_set(a, caps, "inlayHintProvider", json_bool(a, true));
    JsonValue *comp = json_object(a);
    JsonValue *trig = json_array(a);
    json_array_push(a, trig, json_str(a, "."));   /* value/module/type-name members */
    json_array_push(a, trig, json_str(a, ":"));   /* `::` namespace/module path */
    json_object_set(a, comp, "triggerCharacters", trig);
    json_object_set(a, caps, "completionProvider", comp);

    JsonValue *info = json_object(a);
    json_object_set(a, info, "name", json_str(a, "fcc-lsp"));
    json_object_set(a, info, "version", json_str(a, fcc_version_string()));

    JsonValue *res = json_object(a);
    json_object_set(a, res, "capabilities", caps);
    json_object_set(a, res, "serverInfo", info);
    lsp_reply(a, id, res);
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

    /* Any request expecting a reply (hover, definition, completion, ...) must
     * answer against current types, so realize deferred edits first. No-op when
     * nothing is dirty (e.g. initialize, or a request with no pending change). */
    if (is_request) flush_dirty(S);

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
    } else if (strcmp(method, "textDocument/inlayHint") == 0) {
        handle_inlayhint(S, id, params);
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
        char *full = str_sprintf("%s/%s", dir, nm);   /* sized to fit */
        struct stat st;
        int len;
        char *buf = NULL;
        if (!(stat(full, &st) == 0 && S_ISDIR(st.st_mode)))
            buf = read_file(full, &len);
        if (!buf) { free(full); continue; }
        AnalysisSource src = { full, buf, len };
        DA_APPEND(S->stdlib, S->stdlib_count, S->stdlib_cap, src);
        any = true;
    }
    closedir(d);
    return any;
#endif
}

/* Load the standard library once so std:: imports resolve. The directory is
 * FCC_STDLIB_DIR if set, else the install datadir, else a repo-relative
 * ./stdlib. A missing stdlib is not fatal: analysis still works for files that
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
    /* Unbuffer stdin so input_pending()'s fd-level poll is accurate: with stdio
     * read-ahead, a buffered next message would be invisible to poll and defeat
     * coalescing. LSP message volume is tiny, so byte-granular reads cost nothing. */
    setvbuf(stdin, NULL, _IONBF, 0);

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

            /* didOpen/didChange only mark the doc dirty; analyze once the client
             * pauses. While more messages are already queued, keep draining so a
             * fast burst of edits collapses into a single analysis. */
            if (!input_pending()) flush_dirty(&S);
        }
    }

    /* Cleanup */
    for (int i = 0; i < S.store.count; i++) {
        free(S.store.docs[i].uri);
        free(S.store.docs[i].path);
        free(S.store.docs[i].real);
        free(S.store.docs[i].text);
        free(S.store.docs[i].unit_key);
    }
    free(S.store.docs);
    for (int i = 0; i < S.unit_count; i++) {
        unit_free_results(&S.units[i]);
        unit_free_files(&S.units[i]);
        free(S.units[i].key);
    }
    free(S.units);
    for (int i = 0; i < S.stdlib_count; i++) {
        free((void *)S.stdlib[i].filename);
        free((void *)S.stdlib[i].text);
    }
    free(S.stdlib);
    for (int i = 0; i < S.pub_count; i++) free(S.pub_uris[i]);
    free(S.pub_uris);
    lexcache_free(&S.lex_cache);
    arena_free(&S.msg_arena);

    /* A clean shutdown+exit returns 0; an exit without prior shutdown is 1. */
    return (exiting && S.shutdown_requested) ? 0 : (exiting ? 1 : 0);
}
