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
    char           *text;       /* owned: current full document text (UTF-8) */
    int             text_len;
    long            version;
    bool            dirty;      /* text changed since last analysis. didOpen/didChange
                                 * only set this; analysis is deferred so a burst of
                                 * keystrokes coalesces into one analyze (see lsp_main). */
    AnalysisResult *result;     /* latest analysis; NULL until first analyze */
    AnalysisResult *last_good;  /* most recent analysis that type-checked (result
                                 * itself when fresh is good), retained so a
                                 * transient parse/pass1 failure mid-typing does
                                 * not blank type-aware queries. May alias result.
                                 * NULL until the first good analysis. */
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

    /* Session-scoped lex cache: feed (stdlib + sibling/lsp.rsp) token arrays,
     * reused across analyses so unchanged sources aren't re-lexed each keystroke. */
    LexCache        lex_cache;
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

/* The analysis that answers type-aware queries (hover, definition, completion,
 * CodeLens): the fresh result whenever it actually type-checked, otherwise the
 * last one that did. This keeps overlays stable while you type through a
 * transient broken state (`let r2 = `, `... problem_01.`) instead of blanking
 * everything every other keystroke. Diagnostics deliberately do NOT use this —
 * they always come from the fresh `result`, so squiggles stay live. The stale
 * AST carries positions from an earlier revision; consumers locate by current
 * coordinates, so a line you have not touched still resolves while the line
 * under edit may simply miss (the same as a blank, never a crash). May return
 * NULL before the first good analysis. */
static AnalysisResult *query_result(LspDoc *doc) {
    if (doc->result && doc->result->typed) return doc->result;
    return doc->last_good;
}

/* Free both analyses a document may own. result and last_good can alias (when
 * the freshest analysis is the good one), so free the distinct one first. */
static void doc_free_results(LspDoc *d) {
    if (d->last_good && d->last_good != d->result) analysis_free(d->last_good);
    if (d->result) analysis_free(d->result);
    d->result = NULL;
    d->last_good = NULL;
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

/* Canonical absolute path (resolves symlinks, `..`, and relative spellings) so
 * two paths to the SAME on-disk file compare equal. Falls back to a plain copy
 * when the path can't be resolved (e.g. an unsaved/virtual document). Caller
 * frees. */
static char *canon_path(const char *path) {
    char *rp = platform_realpath(path);
    return rp ? rp : dup_cstr(path);
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
/* Built-in intrinsic hover documentation                                   */
/* ======================================================================== */

/* The language's keyword-builtins (alloc, some, default, ...) and built-in
 * globals (stdin/stdout/stderr) are not user declarations, so there is no
 * decl/doc-comment to surface on hover. This static table provides one. `sig`
 * is a generic signature shown in a code fence; `doc` is the markdown body.
 * The keyword/identifier as written in source is the lookup key — this lets
 * `none` and `default` (both EXPR_DEFAULT nodes) carry distinct docs. */
typedef struct {
    const char *name;   /* exact spelling in source */
    const char *sig;    /* generic signature for the code fence */
    const char *doc;    /* markdown body: summary + details (no leading/trailing blank) */
} BuiltinDoc;

static const BuiltinDoc BUILTIN_DOCS[] = {
    { "alloc", "alloc(T) -> T*?",
      "Allocates `T` on the **heap**, zero-initialized, and returns an option that is "
      "`none` when the OS allocation fails — so a successful pointer is checked, not assumed.\n\n"
      "Forms:\n"
      "- `alloc(T)` → `T*?` — one zeroed `T`\n"
      "- `alloc(T[n])` → `T[]?` — a slice of `n` zeroed elements\n"
      "- `alloc(T, n)` → `T*?` — a raw buffer of `n` elements\n"
      "- `alloc(expr)` → `T?` — a heap copy of a struct/union/string value\n\n"
      "Unwrap with `!` once checked, and release with `free` (commonly `defer free(p)`). "
      "Lowers to C `calloc`/`malloc`." },

    { "alloca", "alloca(T) -> T*",
      "Allocates `T` on the **dynamic stack** (`__builtin_alloca`), zero-initialized. "
      "Unlike `alloc` it cannot fail, returns the value directly (no option), and is "
      "reclaimed automatically when the enclosing function returns — never `free` it.\n\n"
      "Forms:\n"
      "- `alloca(T)` → `T*`\n"
      "- `alloca(T[n])` → `T[]`\n"
      "- `alloca(T, n)` → `T*` — a raw buffer\n\n"
      "The result has stack provenance: escape analysis forbids returning it or storing it "
      "in heap/static memory. **Beware loops** — each iteration grows the frame; promote a "
      "buffer that must persist or repeat to the heap with `alloc(...)`." },

    { "free", "free(p: T*) -> void",
      "Releases heap memory obtained from `alloc` (lowers to C `free`; for a slice it frees "
      "the backing `.ptr`).\n\n"
      "Only heap pointers may be freed: escape analysis rejects freeing stack (`alloca`, "
      "`&local`), static (string-literal), or `const` memory. After freeing, the pointer is "
      "dangling — don't read it again. Pairs naturally with `defer free(p)`." },

    { "default", "default(T) -> T",
      "Produces the **zero value** of any type `T`, with no initializer to spell out:\n"
      "- numbers → `0` / `0.0`\n"
      "- booleans → `false`\n"
      "- pointers and options → null / `none`\n"
      "- structs, unions, slices, tuples → every field zeroed\n\n"
      "Useful for an explicit empty or initial value. `none(T)` is shorthand for `default(T?)`." },

    { "some", "some(x: T) -> T?",
      "Wraps a present value in an option, yielding a `T?` that holds `x`.\n\n"
      "For a pointer option (`T*?`, which uses null as its empty sentinel) a value known to be "
      "non-null is stored directly; an unprovable one is null-checked at runtime and aborts if "
      "null. Pair with `none(T)` for the empty case, and read the value back with `!` or `match`." },

    { "none", "none(T) -> T?",
      "The **empty** option of inner type `T` — a `T?` holding no value. Exactly equivalent to "
      "`default(T?)`.\n\n"
      "The type argument is required so the element type is known: write `none(i32)`, not a bare "
      "`none`. Build the present case with `some(x)`, and test for empty with a `none` match arm "
      "(or `.is_none`)." },

    { "sizeof", "sizeof(T) -> i64",
      "The size of type `T` in bytes, as an `i64` (lowers to `(int64_t)sizeof(T)`). Works on any "
      "type — primitives, pointers, slices, structs, unions. Computed by the C compiler, so it "
      "reflects the target's real layout and padding." },

    { "alignof", "alignof(T) -> i64",
      "The alignment requirement of type `T` in bytes, as an `i64` (lowers to "
      "`(int64_t)_Alignof(T)`). The address of any `T` value is a multiple of this." },

    { "assert", "assert(cond: bool[, msg: str]) -> void",
      "Checks `cond` at runtime; if it is false, prints the source file, line, and the failing "
      "condition text (plus the optional `msg`) to stderr and calls `abort()`.\n\n"
      "Forms:\n"
      "- `assert(cond)`\n"
      "- `assert(cond, \"message\")`\n\n"
      "For invariants that must hold — a violated assert ends the program. Always active; there "
      "is no disable switch." },

    { "atomic_load_acquire", "atomic_load_acquire(p: T*) -> T",
      "Atomically reads `*p` with **acquire** ordering: writes another thread released before "
      "storing to `*p` become visible afterward. `T` must be a lock-free integer or `bool` "
      "(enforced by a `_Static_assert`). Lowers to `__atomic_load_n(p, __ATOMIC_ACQUIRE)`.\n\n"
      "Pair with `atomic_store_release` to hand data between threads without a lock." },

    { "atomic_store_release", "atomic_store_release(p: T*, v: T) -> void",
      "Atomically writes `v` into `*p` with **release** ordering: writes this thread made before "
      "it are visible to any thread that later acquire-loads `*p`. `T` must be a lock-free integer "
      "or `bool`. Lowers to `__atomic_store_n(p, v, __ATOMIC_RELEASE)`.\n\n"
      "The reading side uses `atomic_load_acquire`." },

    { "stdin", "stdin: any*",
      "The process's standard input stream as an opaque `any*` (a C `FILE*`). Pass it to extern C "
      "I/O functions that expect a `FILE*`; it cannot be dereferenced in FC." },

    { "stdout", "stdout: any*",
      "The process's standard output stream as an opaque `any*` (a C `FILE*`). Pass it to extern C "
      "I/O functions that expect a `FILE*`; it cannot be dereferenced in FC." },

    { "stderr", "stderr: any*",
      "The process's standard error stream as an opaque `any*` (a C `FILE*`). Pass it to extern C "
      "I/O functions that expect a `FILE*`; it cannot be dereferenced in FC." },
};

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
    const BuiltinDoc *builtin;      /* set when the winning node is a built-in intrinsic
                                       (alloc/some/.../stdin); hover renders its doc instead
                                       of a `name: type` line. Cleared by every consider() win. */
} FindCtx;

static Type *peel_to_aggregate(Type *t);   /* defined in the completion section */

static const SrcLoc NO_LOC = {0};

static void consider(FindCtx *c, int line, int col, int span, Type *type,
                     const char *name, Symbol *sym, SrcLoc def_loc,
                     SrcLoc doc_loc, bool doc_is_field) {
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
    c->def_loc = def_loc;
    c->doc_loc = doc_loc;
    c->doc_is_field = doc_is_field;
    c->builtin = NULL;   /* a plain node wins; consider_builtin re-sets this when it wins */
}

/* Reads the contiguous identifier/keyword token at `loc` from source into `buf`,
 * returning its length (0 if `loc` is out of range). Used to recover the exact
 * keyword a built-in node was spelled with — e.g. to tell `none` from `default`,
 * which share the EXPR_DEFAULT node kind. */
static int read_ident_at(const FindCtx *c, SrcLoc loc, char *buf, size_t bufsz) {
    buf[0] = '\0';
    if (loc.line < 1 || loc.line > c->idx->count) return 0;
    int off = c->idx->starts[loc.line - 1] + (loc.col - 1);
    int end = c->idx->len;
    size_t n = 0;
    while (off >= 0 && off < end && n + 1 < bufsz) {
        char ch = c->src[off];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) break;
        buf[n++] = ch;
        off++;
    }
    buf[n] = '\0';
    return (int)n;
}

/* If `e` is a keyword-builtin node whose keyword is under the cursor, register
 * it as the hit (innermost-wins, like consider()). The keyword span is read from
 * source so the highlight covers exactly the keyword, and so `none`/`default`
 * resolve to their own docs. */
static void consider_builtin(FindCtx *c, const Expr *e) {
    char kw[32];
    int span = read_ident_at(c, e->loc, kw, sizeof kw);
    if (span <= 0) return;
    const BuiltinDoc *bd = builtin_doc_lookup(kw);
    if (!bd) return;
    if (e->loc.line != c->target_line) return;
    if (c->target_col < e->loc.col || c->target_col >= e->loc.col + span) return;
    if (c->found && span > c->best_span) return;   /* keep the innermost (smallest) */
    c->found = true;
    c->best_span = span;
    c->start_line = e->loc.line;
    c->start_col = e->loc.col;
    c->type = e->type;
    c->name = bd->name;
    c->sym = NULL;
    c->def_loc = NO_LOC;
    c->doc_loc = NO_LOC;
    c->doc_is_field = false;
    c->builtin = bd;
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
                     e->type, e->ident.name, e->ident.resolved_sym,
                     e->ident.resolved_local_loc, e->ident.resolved_local_loc, false);
            /* Built-in globals (stdin/stdout/stderr) resolve to no Symbol and no
             * local binding. If the cursor landed on one, attach its doc. (Keyword
             * builtins can't appear as idents, so only these globals match here.) */
            if (!e->ident.resolved_sym && e->ident.resolved_local_loc.line == 0 &&
                c->found && c->start_line == e->loc.line && c->start_col == e->loc.col) {
                const BuiltinDoc *bd = builtin_doc_lookup(e->ident.name);
                if (bd) c->builtin = bd;
            }
            break;
        case EXPR_FIELD:
        case EXPR_DEREF_FIELD:
            find_in_expr(e->field.object, c);
            /* The field-name token's exact source loc is recorded by the parser,
             * so hover/definition target the field name precisely regardless of the
             * object expression's shape (a.b, a.b.c, s . field) or spacing. Go-to-
             * definition resolves: module members (mod.member -> the member decl)
             * and variant constructors (shape.circle -> the union decl) via a
             * Symbol; plain struct-field access (s.field) jumps to the field's
             * declaration in the struct body via its recorded StructField loc. */
            if (e->field.name_loc.line > 0) {
                Symbol *def = e->field.resolved_member;
                SrcLoc doc_loc = NO_LOC;
                bool doc_is_field = false;
                if (!def && e->field.is_variant_constructor &&
                    e->type && e->type->kind == TYPE_UNION) {
                    /* Variant constructor: go-to-def lands on the union decl (Symbol),
                     * but the doc comment is read from the variant's own line. */
                    def = e->type->unio.resolved_sym;
                    for (int i = 0; i < e->type->unio.variant_count; i++)
                        if (e->type->unio.variants[i].name == e->field.name) {
                            doc_loc = e->type->unio.variants[i].loc;
                            doc_is_field = true;
                            break;
                        }
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
                consider(c, e->field.name_loc.line, e->field.name_loc.col,
                         (int)strlen(e->field.name), e->type, e->field.name,
                         def, field_def, doc_loc, doc_is_field);
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
                             p->type, p->name, NULL, NO_LOC, NO_LOC, false);
            }
            find_in_exprs(e->func.body, e->func.body_count, c);
            break;
        case EXPR_STRUCT_LIT:
            /* The type name (at the node's loc) goes to the struct declaration. */
            if (e->struct_lit.type_name)
                consider(c, e->loc.line, e->loc.col,
                         (int)strlen(e->struct_lit.type_name), e->type,
                         e->struct_lit.type_name, e->struct_lit.resolved_sym,
                         NO_LOC, NO_LOC, false);
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
            consider_builtin(c, e);   /* the alloc/alloca keyword */
            find_in_expr(e->alloc_expr.size_expr, c);
            find_in_expr(e->alloc_expr.init_expr, c);
            break;
        case EXPR_FREE:
            consider_builtin(c, e);
            find_in_expr(e->free_expr.operand, c);
            break;
        case EXPR_SIZEOF:
        case EXPR_ALIGNOF:
        case EXPR_DEFAULT:
            /* sizeof/alignof/default (and `none`, which desugars to EXPR_DEFAULT)
             * take only a type argument — nothing further to descend into. */
            consider_builtin(c, e);
            break;
        case EXPR_INTERP_STRING:
            for (int i = 0; i < e->interp_string.segment_count; i++)
                if (!e->interp_string.segments[i].is_literal)
                    find_in_expr(e->interp_string.segments[i].expr, c);
            break;
        case EXPR_ASSIGN:
            find_in_expr(e->assign.target, c);
            find_in_expr(e->assign.value, c);
            break;
        case EXPR_SOME:
            consider_builtin(c, e);
            find_in_expr(e->some_expr.value, c);
            break;
        case EXPR_LET: {
            int col = let_name_col(c, e->loc.line, e->loc.col, e->let_expr.let_is_mut);
            consider(c, e->loc.line, col, (int)strlen(e->let_expr.let_name),
                     e->let_expr.let_type, e->let_expr.let_name, NULL,
                     NO_LOC, e->let_expr.let_name_loc, false);
            find_in_expr(e->let_expr.let_init, c);
            break;
        }
        case EXPR_LET_DESTRUCT: find_in_expr(e->let_destruct.init, c); break;
        case EXPR_ASSERT:
            consider_builtin(c, e);
            find_in_expr(e->assert_expr.condition, c);
            find_in_expr(e->assert_expr.message, c);
            break;
        case EXPR_DEFER: find_in_expr(e->defer_expr.value, c); break;
        case EXPR_ATOMIC_LOAD:
            consider_builtin(c, e);
            find_in_expr(e->atomic_load.ptr, c);
            break;
        case EXPR_ATOMIC_STORE:
            consider_builtin(c, e);
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
                         d->let.resolved_type, d->let.name, NULL, NO_LOC, d->loc, false);
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
    AnalysisResult *r = query_result(doc);
    if (!r || !r->program) return false;
    int line1, col1;
    lsp_to_loc(idx, doc->text, line0, char0, &line1, &col1);
    FindCtx c = {0};
    c.target_line = line1;
    c.target_col = col1;
    c.src = doc->text;
    c.idx = idx;
    c.file = doc->path;
    find_in_decls(r->program->decls, r->program->decl_count, &c);
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

/* Walk up from the directory of `doc_path` looking for an `lsp.rsp` response
 * file (the by-convention name the LSP discovers; equivalent to `fcc @lsp.rsp`).
 * Nearest ancestor wins. Returns a malloc'd path (caller frees) or NULL. */
static char *find_lsp_rsp(const char *doc_path) {
#if defined(_WIN32)
    (void)doc_path;
    return NULL;
#else
    const char *slash = strrchr(doc_path, '/');
    if (!slash) return NULL;
    int dlen = (int)(slash - doc_path);
    if (dlen <= 0 || dlen >= 4096) return NULL;
    char dirbuf[4096];
    memcpy(dirbuf, doc_path, (size_t)dlen);
    dirbuf[dlen] = '\0';

    /* Resolve the directory (the file itself may be unsaved/virtual). */
    char *dir = realpath(dirbuf, NULL);
    if (!dir) return NULL;

    char *result = NULL;
    for (;;) {
        size_t dn = strlen(dir);
        char *path = malloc(dn + sizeof("/lsp.rsp"));
        memcpy(path, dir, dn);
        memcpy(path + dn, "/lsp.rsp", sizeof("/lsp.rsp"));
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

/* (Re)run analysis for a document and publish diagnostics.
 *
 * The compilation unit comes from one of two sources:
 *   - an `lsp.rsp` response file discovered by walking up from the open file —
 *     authoritative: exactly its listed inputs (plus the open buffer), with no
 *     sibling glob and no blanket stdlib feed, so the editor resolves names
 *     identically to `fcc @lsp.rsp` on the CLI; or
 *   - (no lsp.rsp) the zero-config heuristic: the open file + its sibling .fc
 *     files (open-buffer text preferred over disk) + the full stdlib feed. */
static void analyze_doc(LspServer *S, LspDoc *doc) {
    /* The previous fresh result is retired below, AFTER the new analysis, so it
     * can be kept as last_good when the new one fails to type-check. */
    AnalysisResult *prev = doc->result;

    AnalysisSource *extra = NULL;
    int n = 0, cap = 0;
    char **disk_bufs = NULL;    /* text buffers read from disk, freed after analyze */
    int db = 0, dbcap = 0;
    char **sibs = NULL;         /* sibling paths; some back extra[].filename, so */
    int sc = 0, scap = 0;       /* they must outlive analyze() (freed at the end) */

    /* Conditional-compilation flags; when from lsp.rsp the `rsp_*` values own the
     * token storage the flags' name/value pointers borrow, so they must outlive
     * the analyze() call (freed at the end). */
    Flag *flags = NULL;
    int flag_count = 0, flag_cap = 0;
    ExpandedArgs rsp_expanded = {0};
    CompileArgs  rsp_ca = {0};
    bool have_rsp = false;
    char *rsp_err = NULL;       /* set when an lsp.rsp exists but is unusable */

    char *rsp_path = find_lsp_rsp(doc->path);
    if (rsp_path) {
        char at[4096];
        snprintf(at, sizeof at, "@%s", rsp_path);
        char *fake_argv[] = { (char *)"fcc", at };
        char *aerr = NULL;
        if (args_expand(2, fake_argv, &rsp_expanded, &aerr) &&
            args_parse(&rsp_expanded, &rsp_ca)) {
            have_rsp = true;
            flags = rsp_ca.flags;
            flag_count = rsp_ca.flag_count;

            char *doc_real = canon_path(doc->path);
            for (int i = 0; i < rsp_ca.input_count; i++) {
                char *in_real = canon_path(rsp_ca.inputs[i]);
                /* The open document arrives as the primary source; never add it
                 * again, and its live buffer must win over the on-disk copy. */
                bool is_open = (strcmp(doc_real, in_real) == 0);
                free(in_real);
                if (is_open) continue;
                LspDoc *od = store_find_by_path(&S->store, rsp_ca.inputs[i]);
                if (od && od != doc) {              /* open elsewhere: live text */
                    AnalysisSource s = { od->path, od->text, od->text_len };
                    DA_APPEND(extra, n, cap, s);
                } else {                            /* read from disk */
                    int len;
                    char *buf = read_whole_file(rsp_ca.inputs[i], &len);
                    if (!buf) continue;
                    AnalysisSource s = { rsp_ca.inputs[i], buf, len };
                    DA_APPEND(extra, n, cap, s);
                    DA_APPEND(disk_bufs, db, dbcap, buf);
                }
            }
            free(doc_real);
        } else {
            /* A broken lsp.rsp must not silently behave as if absent: fall back
             * to the heuristic but surface the reason on the open file below. */
            const char *m = aerr ? aerr : (rsp_ca.error ? rsp_ca.error : "parse failed");
            rsp_err = dup_cstr(m);
            free(aerr);
            args_compile_free(&rsp_ca);
            args_expand_free(&rsp_expanded);
        }
        free(rsp_path);
    }

    if (!have_rsp) {
        /* Zero-config heuristic: host flags + sibling .fc files + stdlib feed. */
        platform_detect_flags(&flags, &flag_count, &flag_cap);

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

        /* stdlib feed. Drop any feed file that is really the SAME source as the open
         * document or a sibling — most commonly because you followed Go To Definition
         * into a stdlib module, so the editor opened the very file the feed already
         * provides. Without this it is analyzed twice => pass1 "redefinition" => pass2
         * never runs (no diagnostics AND no hover/CodeLens; the error is attributed to
         * the feed copy and filtered out, so nothing surfaces).
         *
         * A feed entry is a duplicate if it matches a "have" entry by EITHER:
         *   - canonical absolute path (realpath) — the same on-disk file regardless of
         *     how its path was spelled or symlinked, and crucially for an open document
         *     with unsaved edits (path matches even though the buffer differs from
         *     disk, and the live buffer must win); OR
         *   - byte-identical content — a separate copy of the same source at a
         *     different path (e.g. opening the repo's stdlib/data.fc while the feed
         *     resolves to the *installed* /usr/local/share/.../data.fc; the two files
         *     are identical but their realpaths differ). The content check is gated on
         *     an equal length first, so a full memcmp runs only for a genuine
         *     same-length candidate — never for an ordinary edit.
         * Two DIFFERENT files that merely share a basename — a project's own `data.fc`
         * vs the stdlib's — match neither key and are correctly kept (basename-matching
         * would wrongly shadow `std::data`). */
        int sib_n = n;                              /* siblings precede any feed entries */
        int have_n = sib_n + 1;
        char       **have_path = malloc(sizeof *have_path * (size_t)have_n);
        const char **have_text = malloc(sizeof *have_text * (size_t)have_n);
        int         *have_len  = malloc(sizeof *have_len  * (size_t)have_n);
        have_path[0] = canon_path(doc->path);
        have_text[0] = doc->text;
        have_len[0]  = doc->text_len;
        for (int j = 0; j < sib_n; j++) {
            have_path[1 + j] = canon_path(extra[j].filename);
            have_text[1 + j] = extra[j].text;
            have_len[1 + j]  = extra[j].len;
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
            if (!dup) DA_APPEND(extra, n, cap, S->stdlib[i]);
        }
        for (int j = 0; j < have_n; j++) free(have_path[j]);
        free(have_path);
        free(have_text);
        free(have_len);
    }

    doc->result = analyze(doc->text, doc->text_len, doc->path, extra, n, flags, flag_count,
                          &S->lex_cache);

    /* lsp.rsp existed but couldn't be used: attach one file-level diagnostic so
     * the editor explains the fallback instead of silently differing. */
    if (rsp_err) {
        char m[512];
        snprintf(m, sizeof m, "lsp.rsp ignored: %s", rsp_err);
        Diagnostic d;
        d.loc = (SrcLoc){ .filename = doc->result->filename, .line = 1, .col = 1 };
        d.message = arena_strdup(&doc->result->arena, m, (int)strlen(m));
        DA_APPEND(doc->result->diags, doc->result->diag_count, doc->result->diag_cap, d);
        free(rsp_err);
    }

    /* Retain the last analysis that type-checked. When the fresh one is good it
     * becomes the new last_good (and the previous good copy is freed); when it is
     * degraded (parse abort or pass1-gated pass2) we hold onto the prior good one
     * so type-aware queries keep answering. prev/last_good may alias, hence the
     * !=-guarded frees so nothing is freed twice or while still in use. */
    if (doc->result->typed) {
        if (doc->last_good && doc->last_good != prev) analysis_free(doc->last_good);
        doc->last_good = doc->result;
        if (prev && prev != doc->result) analysis_free(prev);
    } else {
        if (prev && prev != doc->last_good) analysis_free(prev);
    }

    for (int i = 0; i < db; i++) free(disk_bufs[i]);
    free(disk_bufs);
    for (int i = 0; i < sc; i++) free(sibs[i]);
    free(sibs);
    free(extra);
    if (have_rsp) {
        args_compile_free(&rsp_ca);     /* frees the flags array */
        args_expand_free(&rsp_expanded); /* frees the tokens flags borrowed */
    } else {
        free(flags);
    }

    publish_diagnostics(S, doc);
}

/* Analyze every document whose text changed since its last analysis (and publish
 * its diagnostics). Called when the input queue drains and before answering any
 * type-aware request, so deferred edits are realized exactly once per quiet
 * point — collapsing a burst of keystrokes into a single analysis. */
static void flush_dirty(LspServer *S) {
    for (int i = 0; i < S->store.count; i++) {
        LspDoc *doc = &S->store.docs[i];
        if (doc->dirty) {
            doc->dirty = false;
            analyze_doc(S, doc);
        }
    }
}

/* True if more input is already waiting, so we should keep reading (and coalesce)
 * rather than analyze now. Relies on stdin being unbuffered (see lsp_main): with
 * no stdio read-ahead, a poll at the fd level reflects the true pending state.
 * On Windows we can't poll stdin portably, so report "nothing pending" — every
 * message then flushes immediately, i.e. the original un-coalesced behavior. */
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
    doc->text = malloc((size_t)doc->text_len + 1);
    memcpy(doc->text, tv->str.s, (size_t)doc->text_len);
    doc->text[doc->text_len] = '\0';

    JsonValue *vv = json_get(td, "version");
    if (vv && vv->kind == JSON_NUMBER) doc->version = (long)vv->num;

    doc->dirty = true;   /* analyzed at the next idle flush (see lsp_main) */
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
            doc_free_results(d);
            free(d->uri); free(d->path); free(d->text);
            S->store.docs[i] = S->store.docs[--S->store.count];
            return;
        }
    }
}

/* ======================================================================== */
/* Doc comments (hover)                                                      */
/*                                                                          */
/* FC has no structured doc-comment syntax; the lexer discards comments. So  */
/* the server reads them straight from source text at the definition site:   */
/* the contiguous run of `//` lines immediately above a definition (the way  */
/* a reader documents a `let`/`struct`/`union`/field), plus, for a struct or */
/* union field, a `//` trailing the field's own line.                        */
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
    char *buf = read_whole_file(path, out_len);
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
    if (!locate(doc, &idx, (int)line, (int)ch, &hit) || (!hit.type && !hit.builtin)) {
        lsp_reply(a, id, json_null(a));
        return;
    }

    char *md;
    if (hit.builtin) {
        /* Built-in intrinsic: generic signature fence + prose, then the concrete
         * result type of this occurrence when it carries information (skip void
         * builtins like free/assert, and any node pass2 couldn't type). */
        const BuiltinDoc *bd = hit.builtin;
        char rt[256]; rt[0] = '\0';
        if (hit.type && !type_is_error(hit.type) && hit.type->kind != TYPE_VOID)
            copy_type_name(hit.type, rt, sizeof rt);
        const char *fmt = rt[0] ? "```fc\n%s\n```\n\n%s\n\n*Result type: `%s`*"
                                : "```fc\n%s\n```\n\n%s";
        int need = snprintf(NULL, 0, fmt, bd->sig, bd->doc, rt) + 1;
        md = arena_alloc(a, (size_t)need);
        snprintf(md, (size_t)need, fmt, bd->sig, bd->doc, rt);
    } else {
        char tn[512];
        copy_type_name(hit.type, tn, sizeof tn);

        /* Doc comment at the definition site. The site may live in another file (a
         * sibling or the stdlib), so read whichever buffer backs it; reuse the open
         * doc's line index when the site is in the open file. */
        char *doc_md = NULL;
        {
            SrcLoc site = NO_LOC; bool site_is_field = false;
            if (hit.doc_loc.line > 0)          { site = hit.doc_loc; site_is_field = hit.doc_is_field; }
            else if (hit.def_loc.line > 0)     { site = hit.def_loc; }
            else if (hit.sym && hit.sym->decl) { site = hit.sym->decl->loc; }
            if (site.line > 0) {
                int flen = 0; bool owned = false;
                const char *ftext = doc_file_text(S, doc, site.filename, &flen, &owned);
                if (ftext) {
                    LineIndex fidx = (ftext == doc->text) ? idx
                                   : line_index_build(a, ftext, flen);
                    doc_md = extract_doc_comment(a, ftext, flen, &fidx, site.line, site_is_field);
                    if (owned) free((void *)ftext);
                }
            }
        }

        const char *nm = hit.name ? hit.name : "";
        const char *fmt = doc_md ? "```fc\n%s: %s\n```\n\n%s" : "```fc\n%s: %s\n```";
        int need = snprintf(NULL, 0, fmt, nm, tn, doc_md) + 1;
        md = arena_alloc(a, (size_t)need);
        snprintf(md, (size_t)need, fmt, nm, tn, doc_md);
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
    if (!locate(doc, &idx, (int)line, (int)ch, &hit)) {
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
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_') i++;
        else break;
    }
    return (i - idx->starts[line - 1]) + 1;
}

static void lens_emit(LensCtx *lc, int let_line, int let_col, bool is_mut,
                      Type *type, bool init_is_lambda) {
    if (!type) return;
    char title[560];
    /* For a lambda binding the parameter types are written explicitly at the
     * definition site, so the only new information is the inferred return type:
     * render `:-> ret`. Everything else — including a plain function-reference
     * binding (`let f = g`), whose params are NOT visible here — shows the full
     * type. Hover is unaffected and always reports the full type. */
    if (init_is_lambda && type->kind == TYPE_FUNC && type->func.return_type) {
        char rt[512];
        copy_type_name(type->func.return_type, rt, sizeof rt);
        /* Inline keeps a space after the colon to match the plain `: T` hints
         * (`let f: -> ret`); the standalone CodeLens reads fine tight (`:-> ret`). */
        snprintf(title, sizeof title, lc->inlay ? ": -> %s" : ":-> %s", rt);
    } else {
        char tn[512];
        copy_type_name(type, tn, sizeof tn);
        snprintf(title, sizeof title, ": %s", tn);
    }

    FindCtx fc = {0};
    fc.idx = lc->idx;
    fc.src = lc->src;
    int name_col = let_name_col(&fc, let_line, let_col, is_mut);

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

static void lens_expr(Expr *e, LensCtx *lc);
static void lens_exprs(Expr **arr, int n, LensCtx *lc) {
    for (int i = 0; i < n; i++) lens_expr(arr[i], lc);
}

static void lens_expr(Expr *e, LensCtx *lc) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_LET:
            lens_emit(lc, e->loc.line, e->loc.col, e->let_expr.let_is_mut,
                      e->let_expr.let_type,
                      e->let_expr.let_init && e->let_expr.let_init->kind == EXPR_FUNC);
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
    AnalysisResult *r = doc ? query_result(doc) : NULL;
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
    AnalysisResult *r = doc ? query_result(doc) : NULL;
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

static const char *KEYWORDS[] = {
    /* mirrors lexer.c keyword set + builtins + primitive type names */
    "let", "mut", "struct", "union", "module", "namespace", "import", "from",
    "as", "extern", "private", "match", "with", "when", "if", "then", "else",
    "for", "in", "loop", "do", "break", "continue", "return", "defer", "some",
    "true", "false", "none", "void", "guarded", "unguarded", "checked",
    "unchecked", "alloc", "alloca", "free", "sizeof", "alignof", "default",
    "const", "assert", "atomic_load_acquire", "atomic_store_release",
    "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
    "isize", "usize", "f32", "f64", "bool", "char", "str", "cstr", "any",
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

/* pass1 registers every struct/union type a SECOND time under its mangled C
 * name — file-scope `fc__x`, module-scoped `mod__x` — so canonicalized type
 * stubs resolve directly (see register_struct_sym / register_module_members in
 * pass1.c). That twin's symbol name IS the type's mangled name, whereas the
 * user-facing entry is keyed by the source name. These twins are never writable
 * as bare identifiers, so completion must skip them — otherwise it offers
 * `vgagraph__huffnode` (and twice over, since the module-member table holds both
 * keys and both register into the global symtab). Pointer compare: names are
 * interned. */
static bool sym_is_mangled_type_twin(const Symbol *s) {
    if (!s->type) return false;
    if (s->kind == DECL_STRUCT && s->type->kind == TYPE_STRUCT)
        return s->name == s->type->struc.name;
    if (s->kind == DECL_UNION && s->type->kind == TYPE_UNION)
        return s->name == s->type->unio.name;
    return false;
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
/* Synthetic, type-level properties of a primitive numeric type name accessed
 * like a module: i32.min/max/bits, f64.nan/inf/neg_inf/epsilon, etc. Mirrors
 * resolve_type_property() in pass2.c. Returns true iff `tn` is an integer/float
 * type (the object was a numeric type name), so the caller stops here. */
static bool complete_type_properties(Arena *a, JsonValue *arr, Type *tn) {
    if (!tn) return false;
    bool is_int = type_is_integer(tn), is_float = type_is_float(tn);
    if (!is_int && !is_float) return false;          /* bool/char/str/etc.: none */
    char ty[128]; copy_type_name(tn, ty, sizeof ty);
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

/* Member completion after '.', '::' (dot) or '->' (arrow). `dot_byte` is the
 * position of the operator's first char; the object expression ends at
 * dot_byte-1. Offers, by object kind: a numeric type name's properties; module
 * members; a slice's len/ptr; an option's is_some/is_none; a struct's fields; a
 * union's variants. Arrow ('->') dereferences one pointer level to the pointee
 * struct (and never matches a type name or module). */
static bool complete_members(LspServer *S, LspDoc *doc, const LineIndex *idx,
                             int dot_byte, bool arrow, JsonValue *arr) {
    Arena *a = &S->msg_arena;
    int anchor = dot_byte - 1;
    if (anchor < 0) return false;

    /* Numeric type-name properties (i32.max, f64.nan). The object before a '.'
     * is a reserved type keyword — never a binding — so read it from source
     * without resolving a node. Arrow can't precede a type name. */
    if (!arrow) {
        const char *txt = doc->text;
        int s = anchor;
        while (s >= 0 && (isalnum((unsigned char)txt[s]) || txt[s] == '_')) s--;
        s++;
        if (anchor >= s)
            if (complete_type_properties(a, arr, type_from_name(txt + s, anchor - s + 1)))
                return true;
    }

    /* Derive line/col from the anchor byte and locate the object node. */
    int line0 = 0;
    for (int i = 0; i < idx->count; i++)
        if (idx->starts[i] <= anchor) line0 = i; else break;
    FindCtx c = {0};
    c.target_line = line0 + 1;
    c.target_col = anchor - idx->starts[line0] + 1;
    c.src = doc->text;
    c.idx = idx;
    c.file = doc->path;
    AnalysisResult *r = query_result(doc);
    if (!r || !r->program) return false;
    find_in_decls(r->program->decls, r->program->decl_count, &c);
    if (!c.found) return false;

    /* Module members ('.'/'::' only). */
    if (!arrow && c.sym && c.sym->kind == DECL_MODULE && c.sym->members) {
        SymbolTable *m = c.sym->members;
        for (int i = 0; i < m->count; i++) {
            if (m->symbols[i].is_private) continue;
            if (sym_is_mangled_type_twin(&m->symbols[i])) continue;
            char detail[512];
            detail[0] = '\0';
            if (m->symbols[i].type) copy_type_name(m->symbols[i].type, detail, sizeof detail);
            add_item(a, arr, m->symbols[i].name, sym_kind_to_cik(&m->symbols[i]),
                     detail[0] ? detail : NULL);
        }
        return true;
    }

    Type *t = c.type;
    /* '->' dereferences exactly one pointer level (pass2: -> requires a pointer
     * to struct). '.' on a pointer is a type error in FC, so offer nothing. */
    if (arrow) {
        if (!t || t->kind != TYPE_POINTER) return false;
        t = t->pointer.pointee;
    }
    if (!t) return false;

    /* Slice fat-pointer fields (covers str = u8[]). */
    if (t->kind == TYPE_SLICE) {
        add_item(a, arr, "len", CIK_FIELD, "i64");
        char ety[128]; ety[0] = '\0';
        if (t->slice.elem) copy_type_name(t->slice.elem, ety, sizeof ety);
        char pdetail[160];
        snprintf(pdetail, sizeof pdetail, "%s*", ety[0] ? ety : "any");
        add_item(a, arr, "ptr", CIK_FIELD, pdetail);
        return true;
    }
    /* Option discriminant fields (the value itself needs `!` to unwrap). */
    if (t->kind == TYPE_OPTION) {
        add_item(a, arr, "is_some", CIK_FIELD, "bool");
        add_item(a, arr, "is_none", CIK_FIELD, "bool");
        return true;
    }
    /* Struct fields (tuples are indexed, not named). */
    if (t->kind == TYPE_STRUCT && !t->struc.is_tuple) {
        for (int i = 0; i < t->struc.field_count; i++) {
            char detail[512];
            copy_type_name(t->struc.fields[i].type, detail, sizeof detail);
            add_item(a, arr, t->struc.fields[i].name, CIK_FIELD, detail);
        }
        return true;
    }
    /* Union variants (bare-name construction site). */
    if (t->kind == TYPE_UNION) {
        for (int i = 0; i < t->unio.variant_count; i++)
            add_item(a, arr, t->unio.variants[i].name, CIK_ENUMMEMBER, NULL);
        return true;
    }
    return false;
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
 * This mirrors FC's lexical resolution: a module's siblings are visible bare
 * within it, and a function's bindings within its body. */
static void complete_scope(Arena *a, JsonValue *items, NameSet *seen,
                           Decl **decls, int n, SymbolTable *scope,
                           const char *file, int target_line) {
    /* The container is the last decl from this file that starts at or before the
     * cursor (decls are in source order, so its span reaches the next sibling).
     * Require a matching filename AND a real (1-based) line: the merged program
     * mixes in other files' decls and synthetic, line-0 / NULL-filename decls
     * (e.g. a stdlib `namespace std` wrapper) that must not win the line race. */
    Decl *enc = NULL;
    for (int i = 0; i < n; i++) {
        Decl *d = decls[i];
        if (!d) continue;
        if (file && (!d->loc.filename || strcmp(d->loc.filename, file) != 0)) continue;
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
                char detail[512]; detail[0] = '\0';
                if (m->symbols[i].type)
                    copy_type_name(m->symbols[i].type, detail, sizeof detail);
                add_item(a, items, m->symbols[i].name,
                         sym_kind_to_cik(&m->symbols[i]), detail[0] ? detail : NULL);
            }
        }
        emit_imports(a, items, seen, ms->imports);
        /* Descend: a nested module/function may further narrow the scope. */
        complete_scope(a, items, seen, enc->module.decls, enc->module.decl_count,
                       ms->members, file, target_line);
    } else if (enc->kind == DECL_LET) {
        /* The enclosing function: harvest its bindings (and referenced names). */
        const char **names = NULL; int nn = 0, cap = 0;
        harvest_expr(enc->let.init, &names, &nn, &cap);
        for (int i = 0; i < nn; i++)
            if (nameset_add(seen, names[i]))
                add_item(a, items, names[i], CIK_VARIABLE, NULL);
        free(names);
    }
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

    /* CompletionContext.triggerKind: 1=Invoked (Ctrl+Space or 24x7 word typing),
     * 2=TriggerCharacter, 3=re-trigger. An auto-pop by a trigger character should
     * only ever surface member completion — if the cursor isn't actually at a
     * member access (a lambda/match/type `->`, a comparison `>`, a lone `:`, a
     * float-literal `.`), stay quiet instead of dumping the global list. Absent
     * context (non-VSCode clients) defaults to Invoked, preserving fall-through. */
    long trig_kind = 1;
    JsonValue *cctx = json_get(params, "context");
    if (cctx) json_get_int(cctx, "triggerKind", &trig_kind);
    bool auto_trigger = (trig_kind == 2);

    LineIndex idx = line_index_build(a, doc->text, doc->text_len);
    int line1, col1;
    lsp_to_loc(&idx, doc->text, (int)line, (int)ch, &line1, &col1);
    int cur = loc_byte_offset(&idx, line1, col1);

    /* Member context: scan back over an in-progress member name to the operator
     * just before the object — '.', '::', or '->'. dot_byte is the operator's
     * first byte, so the object ends at dot_byte-1 in every case. */
    int b = cur - 1;
    while (b >= 0 && (isalnum((unsigned char)doc->text[b]) || doc->text[b] == '_')) b--;
    bool member = false, arrow = false;
    int dot_byte = -1;
    if (b >= 0 && doc->text[b] == '.') { member = true; dot_byte = b; }
    else if (b >= 1 && doc->text[b] == ':' && doc->text[b - 1] == ':') {
        member = true; dot_byte = b - 1;
    }
    else if (b >= 1 && doc->text[b] == '>' && doc->text[b - 1] == '-') {
        member = true; arrow = true; dot_byte = b - 1;
    }

    if (member) {
        /* Member access offers ONLY the object's members — never the global list.
         * Typing past a `.`/`::`/`->` shows members or nothing: a lambda/match/
         * function-type `->` (no object to dereference) yields an empty reply,
         * which dismisses the suggest widget instead of dumping every global.
         * This is independent of triggerKind on purpose: once VSCode has a suggest
         * session open (from word-typing) it re-queries as Invoked even as you
         * type through `->`, so a triggerKind gate alone would leak the globals. */
        complete_members(S, doc, &idx, dot_byte, arrow, items);
        lsp_reply(a, id, items);
        return;
    }
    /* Not a member context. A trigger character that lands here (a lone `:` that
     * isn't `::`, a comparison `>`) has nothing to offer, so an auto-pop stays
     * quiet; an explicit invoke / word-typing falls through to scope completion. */
    if (auto_trigger) { lsp_reply(a, id, items); return; }

    /* Global context: keywords, then everything in lexical scope at the cursor —
     * enclosing-module members + imports and the enclosing function's locals
     * (complete_scope), file-level imports, and top-level symbols. A NameSet
     * dedups across all of these so a name in scope two ways is offered once. */
    for (int i = 0; i < KEYWORD_COUNT; i++)
        add_item(a, items, KEYWORDS[i], CIK_KEYWORD, NULL);

    AnalysisResult *r = query_result(doc);
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
            char detail[512]; detail[0] = '\0';
            if (st->symbols[i].type) copy_type_name(st->symbols[i].type, detail, sizeof detail);
            add_item(a, items, st->symbols[i].name, sym_kind_to_cik(&st->symbols[i]),
                     detail[0] ? detail : NULL);
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
    json_array_push(a, trig, json_str(a, ">"));   /* completes `->`; the second char
                                                   * fires it (handle_completion checks
                                                   * the preceding `-`). A `>` not
                                                   * forming `->` falls through to the
                                                   * ordinary in-scope completion. */
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

    /* Any request expecting a reply (hover, definition, completion, …) must
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
        doc_free_results(&S.store.docs[i]);
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
    lexcache_free(&S.lex_cache);
    arena_free(&S.msg_arena);

    /* A clean shutdown+exit returns 0; an exit without prior shutdown is 1. */
    return (exiting && S.shutdown_requested) ? 0 : (exiting ? 1 : 0);
}
