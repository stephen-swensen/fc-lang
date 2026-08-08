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
 * matches is served from this single result, so a unit is analyzed ONCE per flush
 * regardless of how many of its files are open (the analysis already merges every
 * open doc's live buffer). Owns its analyses; keyed by unit_key(). */
typedef struct {
    char           *key;        /* owned: the unit identity (see unit_key()) */
    AnalysisResult *result;     /* latest analysis; NULL until first analyze */
    AnalysisResult *last_good;  /* most recent analysis that type-checked (result
                                 * itself when fresh is good), retained so a
                                 * transient parse/pass1 failure mid-typing does
                                 * not blank type-aware queries. May alias result.
                                 * NULL until the first good analysis. */
} UnitEntry;

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

    /* One analysis per compilation unit, shared by all its open documents (see
     * UnitEntry). Editing any file re-analyzes only its unit, once — not once per
     * open tab. */
    UnitEntry      *units;
    int             unit_count, unit_cap;

    /* URIs we last published NON-EMPTY diagnostics to (owned). A project file that
     * later goes clean, drops out of the unit, or is closed must be cleared with an
     * explicit empty publish; this set is what we diff against each cycle to know
     * which URIs need clearing. Open documents are always (re)published each cycle
     * regardless, so they don't rely on this. */
    char          **pub_uris;
    int             pub_count, pub_cap;
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

static UnitEntry *unit_find(LspServer *S, const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < S->unit_count; i++)
        if (strcmp(S->units[i].key, key) == 0) return &S->units[i];
    return NULL;
}

/* The analysis that answers type-aware queries (hover, definition, completion,
 * CodeLens) for `doc`: its unit's fresh result whenever it actually type-checked,
 * otherwise the last one that did. This keeps overlays stable while you type
 * through a transient broken state (`let r2 = `, `... problem_01.`) instead of
 * blanking everything every other keystroke. Diagnostics deliberately do NOT use
 * this — they always come from the fresh `result`, so squiggles stay live. The
 * stale AST carries positions from an earlier revision; consumers locate by
 * current coordinates, so a line you have not touched still resolves while the
 * line under edit may simply miss (the same as a blank, never a crash). May
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

/* /home/x y.fc -> file:///home/x%20y.fc  (caller frees). Percent-encodes bytes
 * outside the unreserved set (keeping '/'), mirroring uri_to_path's decode, so a
 * URI we synthesize for a project file that is NOT open still matches the one the
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

/* Snapshot a type's spelling into the message arena. type_name() hands back a
 * rotating slot that a later call reclaims, so any caller holding more than one
 * spelling — or holding one across further type_name() calls — needs its own
 * copy. Sized to the spelling rather than a fixed `char tn[512]`: a type name
 * is unbounded (nested generics, long qualified names), and a clipped one is
 * shown to the user as if it were the type. */
static const char *dup_type_name(Arena *a, Type *t) {
    const char *tn = type_name(t);   /* rotating static buffer */
    return arena_sprintf(a, "%s", tn ? tn : "?");
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

    { "ok", "ok(x: T) -> T!",
      "Wraps a success value in a result, yielding a `T!` that holds `x` with error code 0.\n\n"
      "A result is `ok(T) | err(i32)` — repr `{ err: i32, value: T }` where `err == 0` means ok "
      "(C's \"0 on success\"). Pair with `err(T, code)` for the failure case, and read the value "
      "back with `match` or `!` (which aborts with the code on `err`)." },

    { "err", "err(T, code: i32) -> T!",
      "The **failure** result of payload type `T`, carrying a non-zero `i32` error code.\n\n"
      "The type argument is required so the success type is known: write `err(i32, 2)`, not a "
      "bare `err`. Code 0 is the ok tag and therefore unrepresentable — a provably-zero code is "
      "a compile error, an unprovable one is checked at runtime and aborts if zero. Build the "
      "success case with `ok(x)`, match failure with an `err(e)` arm (literal codes like "
      "`err(2)` work too), or test with `.is_err`." },

    { "error_name", "error_name(e: i32) -> str?",
      "The fully-qualified name of a declared error code — `some(\"file_io.not_found\")` for a "
      "constant declared in an `error` group, `none` for anything else (reserved-range platform "
      "passthrough codes like errno/Win32, and negative codes).\n\n"
      "Backed by a static name table emitted only when `error_name` is used (or unconditionally "
      "under `--backtraces`, where unwrap aborts also print the name) — a static cost, no runtime "
      "machinery. Format the numeric fallback yourself: codes below 65536 belong to the platform." },

    { "sizeof", "sizeof(T) -> i64",
      "The size of type `T` in bytes, as an `i64` (lowers to `(int64_t)sizeof(T)`). Works on any "
      "type — primitives, pointers, slices, structs, unions. Computed by the C compiler, so it "
      "reflects the target's real layout and padding." },

    { "alignof", "alignof(T) -> i64",
      "The alignment requirement of type `T` in bytes, as an `i64` (lowers to "
      "`(int64_t)_Alignof(T)`). The address of any `T` value is a multiple of this." },

    { "bitcast", "bitcast(T, x) -> T",
      "Reinterprets the raw bytes of `x` as type `T` — e.g. inspect an `f32`'s bits as a "
      "`u32` for hashing or serialization (`bitcast(u32, x)`), or vice versa.\n\n"
      "`T` and `x`'s type must both be **equal-size fixed-width scalars** (a fixed-width "
      "integer, float, or `char`); a size mismatch is a compile error, and `isize`/`usize` "
      "(target-defined width) and `bool` are not accepted. Unlike the value cast `(T) x`, no "
      "conversion happens — the bits are unchanged. Every bit pattern is a valid result, so "
      "there is no runtime failure (no `checked`/`unguarded` variant). Lowers to a C11 union "
      "type-pun that compilers fold to a register move (zero cost); it is *not* the "
      "strict-aliasing UB of a pointer-cast reinterpret." },

    { "enum_of", "enum_of(E, x) -> E?",
      "The membership-checked integer\u2192enum conversion \u2014 and the **only** one: a plain "
      "cast `(E) x` is a compile error. Accepts any integer type for `x` and yields `E?`: "
      "`some` of the matching variant when `x` equals a declared value, `none` otherwise.\n\n"
      "This is the boundary operation for C-shaped input (bytes from a data file, an int from "
      "an extern call): the check happens once at the edge, so every value of an enum type is "
      "a declared variant and matches over enums stay exhaustive without `_`. Lowers to a "
      "small static `switch` over the declared values (a range check when they are "
      "contiguous)." },

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
    bool no_doc;                    /* suppress the hover doc scan entirely — the winning
                                       node keeps its def_loc (for go-to-def) but has no doc
                                       of its own. Set for function parameters, whose line
                                       above holds the function decl, not the param's doc.
                                       Cleared by every consider() win. */
    const BuiltinDoc *builtin;      /* set when the winning node is a built-in intrinsic
                                       (alloc/some/.../stdin); hover renders its doc instead
                                       of a `name: type` line. Cleared by every consider() win. */
    Symbol *type_ref_sym;           /* set when the winning token names a TYPE or MODULE
                                       (struct/union/enum/module reference); hover renders a
                                       declaration-form header (`enum dir of i32`) instead of
                                       `name: type`. Cleared by every consider() win. */
    Symbol *companion;              /* companion module of a type_ref_sym hit (pass2's
                                       ident.companion_module); hover appends its doc as a
                                       labeled second section. Cleared by every consider() win. */
    Decl   *decl_site;              /* set when the winning token is a declaration-site
                                       NAME (struct/union/enum/module header line); carries
                                       kind + repr for the header when there is no Symbol.
                                       Cleared by every consider() win. */

    /* Member-completion hook: the operator (`.`/`->`) position of an in-progress
     * member access. During the walk, the EXPR_FIELD/EXPR_DEREF_FIELD node whose
     * loc matches is captured in op_field — its `field.object` carries the object
     * expression's type for ANY object shape (`dict[i]`, `f()`, nested), which a
     * position lookup on the object's last character can't reach when that char is
     * a `]`/`)`. Zero op_line disables the hook. */
    int   op_line, op_col;
    Expr *op_field;
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
    c->no_doc = false;   /* post-set by the EXPR_IDENT winner for parameters */
    c->builtin = NULL;   /* a plain node wins; consider_builtin re-sets this when it wins */
    c->type_ref_sym = NULL;   /* post-set by the EXPR_IDENT/EXPR_FIELD/decl-site winners */
    c->companion = NULL;
    c->decl_site = NULL;
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
    c->no_doc = false;
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
         * use-site copies get resolved). Types are dual-registered in the
         * global symtab under source and mangled names, so the stub's own
         * name (canonicalized by pass1 where needed) resolves either way. */
        qn = t->stub.qualified_name ? t->stub.qualified_name : t->stub.name;
        sym = symtab_lookup_kind(c->symtab, t->stub.name, DECL_STRUCT);
        if (!sym) sym = symtab_lookup_kind(c->symtab, t->stub.name, DECL_UNION);
        if (!sym) sym = symtab_lookup_kind(c->symtab, t->stub.name, DECL_ENUM);
    } else {
        return;   /* primitives/functions/type-vars: nothing to link */
    }
    if (!qn || !sym) return;
    /* Base name token = last path component of the qualified name
     * (m.point -> point, std::io.file -> file), unmangled to its source
     * spelling (a canonicalized stub may read m__point). */
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
        bool ident0 = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      ch == '_';
        if (!ident0) { off++; continue; }
        int tok_start = off;
        while (off < end) {
            char tc = c->src[off];
            bool identc = (tc >= 'a' && tc <= 'z') || (tc >= 'A' && tc <= 'Z') ||
                          (tc >= '0' && tc <= '9') || tc == '_';
            if (!identc) break;
            off++;
        }
        if (off - tok_start == blen &&
            strncmp(c->src + tok_start, base, (size_t)blen) == 0) {
            int tok_col = (tok_start - c->idx->starts[line - 1]) + 1;
            consider(c, line, tok_col, blen, t, base, sym, NO_LOC, NO_LOC, false);
            if (c->found && c->start_line == line && c->start_col == tok_col)
                c->type_ref_sym = sym;
            return;   /* first matching token is the annotation's */
        }
    }
}

static void find_in_expr(Expr *e, FindCtx *c);

static void find_in_exprs(Expr **arr, int n, FindCtx *c) {
    for (int i = 0; i < n; i++) find_in_expr(arr[i], c);
}

static void find_in_expr(Expr *e, FindCtx *c) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_IDENT: {
            consider(c, e->loc.line, e->loc.col, (int)strlen(e->ident.name),
                     e->type, e->ident.name, e->ident.resolved_sym,
                     e->ident.resolved_local_loc, e->ident.resolved_local_loc, false);
            /* Block-locals carry a doc site (the line above their binding) so a
             * `// comment` over a `let` shows on hover. A parameter is the one
             * exception: the line above it holds the function decl or an earlier
             * param, never the param's own doc, so suppress the doc scan — it
             * hovers as name: type. Go-to-def still uses resolved_local_loc. */
            if (e->ident.resolved_local_is_param && c->found &&
                c->start_line == e->loc.line && c->start_col == e->loc.col)
                c->no_doc = true;
            /* Type/module reference: render a declaration-form hover header, and
             * carry the companion module so both docs merge into one hover. */
            if (e->ident.resolved_sym && c->found &&
                c->start_line == e->loc.line && c->start_col == e->loc.col &&
                (e->ident.resolved_sym->kind == DECL_STRUCT ||
                 e->ident.resolved_sym->kind == DECL_UNION ||
                 e->ident.resolved_sym->kind == DECL_ENUM ||
                 e->ident.resolved_sym->kind == DECL_MODULE)) {
                c->type_ref_sym = e->ident.resolved_sym;
                c->companion = e->ident.companion_module;
            }
            /* Built-in globals (stdin/stdout/stderr) resolve to no Symbol and no
             * local binding. If the cursor landed on one, attach its doc. (Keyword
             * builtins can't appear as idents, so only these globals match here.) */
            if (!e->ident.resolved_sym && e->ident.resolved_local_loc.line == 0 &&
                c->found && c->start_line == e->loc.line && c->start_col == e->loc.col) {
                const BuiltinDoc *bd = builtin_doc_lookup(e->ident.name);
                if (bd) c->builtin = bd;
            }
            break;
        }
        case EXPR_FIELD:
        case EXPR_DEREF_FIELD:
            find_in_expr(e->field.object, c);
            /* Member-completion capture: the parser stamps EXPR_FIELD.loc with the
             * operator token (`.`/`->`), so match the completion operator position
             * to grab this node regardless of the field name's completeness (the
             * in-progress name may be empty or absent). */
            if (c->op_line && e->loc.line == c->op_line && e->loc.col == c->op_col)
                c->op_field = e;
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
                if (!def && e->field.is_variant_constructor &&
                    e->type && e->type->kind == TYPE_ENUM) {
                    /* Enum variant: same shape — def on the enum decl, doc from
                     * the variant's own line. */
                    def = e->type->enu.resolved_sym;
                    for (int i = 0; i < e->type->enu.variant_count; i++)
                        if (e->type->enu.variants[i].name == e->field.name) {
                            doc_loc = e->type->enu.variants[i].loc;
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
                /* Module-member TYPE reference (m.point, gfx.mode): the member
                 * itself is a type/module, not a value — declaration-form header.
                 * Variant constructors and type properties keep their value hover
                 * (their `def` may be the enum/union symbol, so gate on the flags). */
                if (c->found && def && !e->field.is_variant_constructor &&
                    !e->field.is_type_property &&
                    c->start_line == e->field.name_loc.line &&
                    c->start_col == e->field.name_loc.col &&
                    (def->kind == DECL_STRUCT || def->kind == DECL_UNION ||
                     def->kind == DECL_ENUM || def->kind == DECL_MODULE)) {
                    c->type_ref_sym = def;
                }
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
        case EXPR_CAST:
            consider_type_annotation(c, e->cast.target, e->loc.line, e->loc.col);
            find_in_expr(e->cast.operand, c);
            break;
        case EXPR_BITCAST:
            consider_builtin(c, e);   /* the bitcast keyword */
            find_in_expr(e->bitcast_expr.operand, c);
            break;
        case EXPR_ENUM_OF:
            consider_builtin(c, e);   /* the enum_of keyword */
            consider_type_annotation(c, e->enum_of_expr.target, e->loc.line, e->loc.col);
            find_in_expr(e->enum_of_expr.operand, c);
            break;
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
                if (p->name) {
                    consider(c, p->loc.line, p->loc.col, (int)strlen(p->name),
                             p->type, p->name, NULL, NO_LOC, NO_LOC, false);
                    /* The written annotation after `name:` — hover/def on the
                     * type name itself. */
                    consider_type_annotation(c, p->type, p->loc.line,
                                             p->loc.col + (int)strlen(p->name));
                }
            }
            find_in_exprs(e->func.body, e->func.body_count, c);
            break;
        case EXPR_STRUCT_LIT:
            /* The type name (at the node's loc) goes to the struct declaration. */
            if (e->struct_lit.type_name) {
                consider(c, e->loc.line, e->loc.col,
                         (int)strlen(e->struct_lit.type_name), e->type,
                         e->struct_lit.type_name, e->struct_lit.resolved_sym,
                         NO_LOC, NO_LOC, false);
                /* The token names the type: declaration-form hover header. */
                if (c->found && e->struct_lit.resolved_sym &&
                    c->start_line == e->loc.line && c->start_col == e->loc.col &&
                    (e->struct_lit.resolved_sym->kind == DECL_STRUCT ||
                     e->struct_lit.resolved_sym->kind == DECL_UNION))
                    c->type_ref_sym = e->struct_lit.resolved_sym;
            }
            for (int i = 0; i < e->struct_lit.field_count; i++)
                find_in_expr(e->struct_lit.fields[i].value, c);
            break;
        case EXPR_TUPLE_LIT: find_in_exprs(e->tuple_lit.elems, e->tuple_lit.elem_count, c); break;
        case EXPR_ARRAY_LIT:
            /* The literal starts with its written element type (`dir[8] {`). */
            consider_type_annotation(c, e->array_lit.elem_type, e->loc.line, e->loc.col);
            find_in_expr(e->array_lit.size_expr, c);
            find_in_exprs(e->array_lit.elems, e->array_lit.elem_count, c);
            break;
        case EXPR_SLICE_LIT:
            consider_type_annotation(c, e->slice_lit.elem_type, e->loc.line, e->loc.col);
            find_in_expr(e->slice_lit.ptr_expr, c);
            find_in_expr(e->slice_lit.len_expr, c);
            break;
        case EXPR_ALLOC:
            consider_builtin(c, e);   /* the alloc/alloca keyword */
            if (e->alloc_expr.alloc_type)   /* alloc(T)/alloc(T,N): written type */
                consider_type_annotation(c, e->alloc_expr.alloc_type,
                                         e->loc.line, e->loc.col);
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
            {
                /* The written type argument. `none` shares EXPR_DEFAULT but
                 * spells no type — read the keyword to tell them apart. */
                Type *tt = e->kind == EXPR_SIZEOF  ? e->sizeof_expr.target
                         : e->kind == EXPR_ALIGNOF ? e->alignof_expr.target
                         :                           e->default_expr.target;
                char kw[16];
                read_ident_at(c, e->loc, kw, sizeof kw);
                if (e->kind != EXPR_DEFAULT || strcmp(kw, "default") == 0)
                    consider_type_annotation(c, tt, e->loc.line, e->loc.col);
            }
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
        case EXPR_OK:
            consider_builtin(c, e);
            find_in_expr(e->ok_expr.value, c);
            break;
        case EXPR_ERR:
            consider_builtin(c, e);
            find_in_expr(e->err_expr.code, c);
            break;
        case EXPR_ERROR_NAME:
            consider_builtin(c, e);
            find_in_expr(e->error_name_expr.code, c);
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
        case EXPR_IGNORE: find_in_expr(e->ignore_expr.value, c); break;
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

/* Column (1-based) of the name that follows a declaration keyword of length
 * kw_len at (line, col). `private` is consumed before the keyword loc is
 * stamped, so the keyword is always first. */
static int kw_name_col(const FindCtx *c, int line, int col, int kw_len) {
    int off = c->idx->starts[line - 1] + (col - 1) + kw_len;
    int end = c->idx->len;
    while (off < end && (c->src[off] == ' ' || c->src[off] == '\t')) off++;
    return (off - c->idx->starts[line - 1]) + 1;
}

/* pass1 mangles type-decl names in place (dir -> fc__dir / m__dir); the source
 * spelling is the suffix after the last "__" (FC identifiers cannot contain a
 * double underscore, so the split is unambiguous). Module names are unmangled. */
static const char *unmangled_name(const char *n) {
    const char *last = NULL;
    for (const char *p = n; (p = strstr(p, "__")) != NULL; p += 2) last = p;
    return last ? last + 2 : n;
}

/* Hover for the NAME on a struct/union/enum/module declaration line. There is
 * no Symbol at hand here; decl_site carries the Decl so hover can render the
 * declaration-form header and read the attached doc comment. */
static void consider_decl_name(FindCtx *c, Decl *d, int kw_len, const char *decl_name) {
    if (!decl_name) return;
    const char *src_name = unmangled_name(decl_name);
    int col = kw_name_col(c, d->loc.line, d->loc.col, kw_len);
    consider(c, d->loc.line, col, (int)strlen(src_name),
             NULL, src_name, NULL, NO_LOC, d->loc, false);
    if (c->found && c->start_line == d->loc.line && c->start_col == col)
        c->decl_site = d;
}

/* One identifier written in an import statement. pass1 stamped what the
 * statement resolved to (import.resolved_*), so this is a pure position match —
 * no re-resolution here. `span` is the written token's length while `name` is
 * the symbol's own spelling: for an `as` alias the two differ, and hovering the
 * alias should report the thing it aliases, which is what the reader came to
 * look up. An unresolved import contributes nothing (there is no symbol to
 * describe, and the diagnostic already says so). */
static void consider_import_ident(FindCtx *c, SrcLoc loc, int span,
                                  const char *name, Symbol *sym, Symbol *companion) {
    if (loc.line == 0 || span <= 0 || !name || !sym) return;
    Type *t = sym->type;
    if (!t && sym->decl && sym->decl->kind == DECL_LET) t = sym->decl->let.resolved_type;
    consider(c, loc.line, loc.col, span, t, name, sym, NO_LOC, NO_LOC, false);
    if (!c->found || c->start_line != loc.line || c->start_col != loc.col) return;
    /* A type or module reference hovers in declaration form (`struct point`,
     * `module io`), same as a use-site reference to it would. */
    if (sym->kind == DECL_STRUCT || sym->kind == DECL_UNION ||
        sym->kind == DECL_ENUM   || sym->kind == DECL_MODULE) {
        c->type_ref_sym = sym;
        c->companion = companion;
    }
}

/* The identifiers of an import statement: the imported name, its `as` alias,
 * and every module segment of the `from` route — each answering for the module
 * it names, so `from a.b.c` hovers and jumps per segment. A namespace path
 * (`std::`) names no declaration, so its segments are deliberately not offered. */
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
 * only the type symbol — annotations, struct-literal type names,
 * module-member type paths. */
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
    /* A type REFERENCE that didn't come through pass2's ident path (a written
     * annotation, a struct-literal type name, a module-member path) carries no
     * companion; recover it so every use-site type hover merges the pair.
     * Declaration-site names deliberately don't merge: a module only becomes
     * a companion where a reference resolves it as one, so each half's decl
     * hover shows only its own doc. */
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
 * for that file (n == 0 clears it — no text needed); `text`/`text_len` supply the
 * bytes used to map 1-based (line, byte-col) locations to LSP (line, UTF-16-char)
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
                   (isalnum((unsigned char)text[end_byte]) || text[end_byte] == '_'))
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
    if (dlen <= 0) return NULL;
    /* Sized to the path rather than capped at 4096: over the cap this silently
     * returned NULL, so a project nested deeply enough lost lsp.rsp discovery
     * and fell back to the sibling heuristic with no explanation. */
    char *dirbuf = str_sprintf("%.*s", dlen, doc_path);

    /* Resolve the directory (the file itself may be unsaved/virtual). */
    char *dir = realpath(dirbuf, NULL);
    free(dirbuf);
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
    if (dlen <= 0) return;
    char *dir = str_sprintf("%.*s", dlen, doc_path);

    DIR *d = opendir(dir);
    if (!d) { free(dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 4 || strcmp(nm + l - 3, ".fc") != 0) continue;
        /* Sized to fit: over the old cap the sibling was silently skipped, so a
         * deep path quietly shrank the compilation unit. */
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

/* Identity of a document's compilation unit, so docs that resolve to the SAME
 * unit are analyzed together, once. Two docs share a unit iff they discover the
 * same `lsp.rsp` (its listed inputs are the unit) or — with no lsp.rsp — live in
 * the same directory (the sibling-heuristic unit). The `R:`/`D:` prefixes keep an
 * rsp path from ever colliding with a directory path. Caller frees.
 *
 * On Windows the rsp/sibling discovery is compiled out, so each document is its
 * own unit (keyed by its path) — matching the existing per-file behavior there. */
static char *unit_key(LspDoc *doc) {
#if defined(_WIN32)
    size_t n = strlen(doc->path);
    char *k = malloc(n + 3);
    memcpy(k, "F:", 2);
    memcpy(k + 2, doc->path, n + 1);
    return k;
#else
    char *rsp = find_lsp_rsp(doc->path);
    if (rsp) {
        char *c = canon_path(rsp);
        free(rsp);
        size_t n = strlen(c);
        char *k = malloc(n + 3);
        memcpy(k, "R:", 2);
        memcpy(k + 2, c, n + 1);
        free(c);
        return k;
    }
    /* Directory of doc->path (canonicalized so symlinked spellings coincide). */
    const char *slash = strrchr(doc->path, '/');
    int dlen = slash ? (int)(slash - doc->path) : 0;
    if (dlen <= 0) return dup_cstr(doc->path);   /* no directory: unique key */
    /* Sized to fit. This string is a *unit key*: over the old cap two documents
     * in different deep directories both fell back to their own path, splitting
     * one project into per-file units (and re-analyzing each separately). */
    char *dirbuf = str_sprintf("%.*s", dlen, doc->path);
    char *c = canon_path(dirbuf);
    free(dirbuf);
    char *k = str_sprintf("D:%s", c);
    free(c);
    return k;
#endif
}

static UnitEntry *unit_find_or_create(LspServer *S, const char *key) {
    UnitEntry *u = unit_find(S, key);
    if (u) return u;
    UnitEntry e = {0};
    e.key = dup_cstr(key);
    DA_APPEND(S->units, S->unit_count, S->unit_cap, e);
    return &S->units[S->unit_count - 1];
}

/* Drop every unit no open document references any more (all its files closed),
 * freeing its analyses. Their now-absent diagnostics are cleared by the next
 * publish cycle (the URIs fall out of the aggregate and are emptied). */
static void unit_prune(LspServer *S) {
    for (int i = 0; i < S->unit_count;) {
        bool used = false;
        for (int d = 0; d < S->store.count; d++)
            if (S->store.docs[d].unit_key &&
                strcmp(S->store.docs[d].unit_key, S->units[i].key) == 0) { used = true; break; }
        if (used) { i++; continue; }
        unit_free_results(&S->units[i]);
        free(S->units[i].key);
        S->units[i] = S->units[--S->unit_count];
    }
}

/* (Re)run the analysis for one compilation unit, storing it on `u`. `doc` is the
 * chosen primary document (any open doc in the unit) — the fresh-lexed source;
 * every OTHER open doc in the unit is merged via the extras below using its live
 * buffer, so this single analysis serves all of the unit's open documents.
 *
 * The compilation unit comes from one of two sources:
 *   - an `lsp.rsp` response file discovered by walking up from the open file —
 *     authoritative: exactly its listed inputs (plus the open buffer), with no
 *     sibling glob and no blanket stdlib feed, so the editor resolves names
 *     identically to `fcc @lsp.rsp` on the CLI; or
 *   - (no lsp.rsp) the zero-config heuristic: the open file + its sibling .fc
 *     files (open-buffer text preferred over disk) + the full stdlib feed. */
static void analyze_unit(LspServer *S, UnitEntry *u, LspDoc *doc) {
    /* The previous fresh result is retired below, AFTER the new analysis, so it
     * can be kept as last_good when the new one fails to type-check. */
    AnalysisResult *prev = u->result;

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
        /* Sized to the path: a clipped "@..." names a different file (or none),
         * and args_expand would report that as a broken response file. */
        char *at = str_sprintf("@%s", rsp_path);
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
        free(at);
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

    u->result = analyze(doc->text, doc->text_len, doc->path, extra, n, flags, flag_count,
                        &S->lex_cache);

    /* lsp.rsp existed but couldn't be used: attach one file-level diagnostic so
     * the editor explains the fallback instead of silently differing. */
    if (rsp_err) {
        /* rsp_err quotes a path from the response file — unbounded. */
        Diagnostic d;
        d.loc = (SrcLoc){ .filename = u->result->filename, .line = 1, .col = 1 };
        d.message = arena_sprintf(&u->result->arena, "lsp.rsp ignored: %s", rsp_err);
        DA_APPEND(u->result->diags, u->result->diag_count, u->result->diag_cap, d);
        free(rsp_err);
    }

    /* Retain the last analysis that type-checked. When the fresh one is good it
     * becomes the new last_good (and the previous good copy is freed); when it is
     * degraded (parse abort or pass1-gated pass2) we hold onto the prior good one
     * so type-aware queries keep answering. prev/last_good may alias, hence the
     * !=-guarded frees so nothing is freed twice or while still in use. */
    if (u->result->typed) {
        if (u->last_good && u->last_good != prev) analysis_free(u->last_good);
        u->last_good = u->result;
        if (prev && prev != u->result) analysis_free(prev);
    } else {
        if (prev && prev != u->last_good) analysis_free(prev);
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
    /* Diagnostics are published project-wide once per flush cycle (see
     * publish_project_diagnostics), not per-document here. */
}

/* Publish diagnostics for EVERY file in the project — open buffer or not — so an
 * edit that breaks (or fixes) a file the user has not opened still surfaces (or
 * clears) there, and errors cascade to all dependent files. Each unit analysis
 * computes diagnostics for every file in its unit, keyed by filename; here we
 * aggregate those across all units (deduped, in case units overlap), then emit one
 * notification per file's URI. Files that went clean, dropped out of the unit, or
 * were closed are cleared with an explicit empty publish, diffed against
 * S->pub_uris (the set we last published NON-EMPTY).
 *
 * stdlib feed files are never surfaced: presumed clean, not the user's code, and
 * often under a read-only install path. (A stdlib file the user actually OPENED is
 * a normal document, found by store_find_by_path, and handled as one.) */
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
     *    NON-EMPTY set `now`. */
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
        char *uri;
        const char *text;
        int tlen;
        char *owned = NULL;
        if (od) {                                  /* open: use its exact URI + live text */
            uri = dup_cstr(od->uri);
            text = od->text;
            tlen = od->text_len;
        } else {                                   /* not open: synthesize URI, read disk */
            uri = path_to_uri(file);
            owned = read_whole_file(file, &tlen);
            text = owned;
        }
        if (text) {                                /* (a deleted non-open file is skipped
                                                    * here and cleared via step 4) */
            emit_diagnostics(S, uri, text, tlen, fd, fn);
            DA_APPEND(now, now_n, now_cap, dup_cstr(uri));
        }
        free(owned);
        free(uri);
        free(fd);
    }

    /* 3. Every OPEN document must be (re)published each cycle: one with no
     *    diagnostics needs an explicit empty publish to clear a prior squiggle. */
    for (int d = 0; d < S->store.count; d++) {
        const char *uri = S->store.docs[d].uri;
        bool in_now = false;
        for (int j = 0; j < now_n; j++)
            if (strcmp(now[j], uri) == 0) { in_now = true; break; }
        if (!in_now) emit_diagnostics(S, uri, "", 0, NULL, 0);
    }

    /* 4. Clear any file we published NON-EMPTY last cycle that is neither errored
     *    now nor an open doc already cleared in step 3. */
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

    /* 5. Adopt the new NON-EMPTY set. */
    for (int p = 0; p < S->pub_count; p++) free(S->pub_uris[p]);
    free(S->pub_uris);
    S->pub_uris = now;
    S->pub_count = now_n;
    S->pub_cap = now_cap;

    free(agg);
}

/* Re-analyze every compilation unit that has a changed document, then publish
 * diagnostics project-wide. Called when the input queue drains and before
 * answering any type-aware request, so deferred edits are realized exactly once
 * per quiet point — collapsing a burst of keystrokes into a single analysis.
 *
 * A change in one file can create OR clear errors in another file that references
 * it — e.g. editing a shared `prelude.fc` that `main.fc` imports — so the WHOLE
 * unit must be re-analyzed, not just the edited file. But because one analysis of
 * a unit already merges every open document's *live* buffer, we analyze each unit
 * ONCE (with any of its open docs as the fresh primary), not once per open tab:
 * N tabs of the same project cost one analysis, not N. Only units containing a
 * dirty doc are re-run; untouched units keep their result. publish_project_
 * diagnostics then surfaces every unit's result on all its files, open or not. */
static void flush_dirty(LspServer *S) {
    bool any_dirty = false;
    for (int i = 0; i < S->store.count; i++)
        if (S->store.docs[i].dirty) { any_dirty = true; break; }
    if (!any_dirty) return;

    /* Refresh each open doc's unit identity (an lsp.rsp may have appeared/changed). */
    for (int i = 0; i < S->store.count; i++) {
        char *k = unit_key(&S->store.docs[i]);
        free(S->store.docs[i].unit_key);
        S->store.docs[i].unit_key = k;
    }

    /* Analyze each DIRTY unit once. A unit is dirty if any of its docs changed;
     * the primary is the first open doc bearing that key (every other open doc in
     * the unit is merged live, so the choice only affects which file NULL-located
     * diagnostics default to). `done` guards against analyzing a unit twice when
     * several of its docs are dirty. */
    const char **done = NULL;
    int dn = 0, dcap = 0;
    for (int i = 0; i < S->store.count; i++) {
        if (!S->store.docs[i].dirty) continue;
        const char *key = S->store.docs[i].unit_key;
        bool seen = false;
        for (int j = 0; j < dn; j++)
            if (strcmp(done[j], key) == 0) { seen = true; break; }
        if (seen) continue;
        DA_APPEND(done, dn, dcap, key);

        LspDoc *primary = NULL;
        for (int p = 0; p < S->store.count; p++)
            if (S->store.docs[p].unit_key &&
                strcmp(S->store.docs[p].unit_key, key) == 0) { primary = &S->store.docs[p]; break; }
        if (!primary) continue;                 /* unreachable: doc i has this key */
        analyze_unit(S, unit_find_or_create(S, key), primary);
    }
    free(done);

    for (int i = 0; i < S->store.count; i++) S->store.docs[i].dirty = false;

    unit_prune(S);                              /* drop units with no open docs */
    publish_project_diagnostics(S);
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
    bool removed = false;
    for (int i = 0; i < S->store.count; i++) {
        if (strcmp(S->store.docs[i].uri, uri) == 0) {
            LspDoc *d = &S->store.docs[i];
            free(d->uri); free(d->path); free(d->text); free(d->unit_key);
            S->store.docs[i] = S->store.docs[--S->store.count];
            removed = true;
            break;
        }
    }
    if (!removed) return;

    if (S->store.count > 0) {
        /* The closed file may still be a unit member on disk — its last-saved
         * content is now authoritative (any unsaved edits are discarded). Re-analyze
         * the remaining open documents so its diagnostics reflect disk, then
         * republish project-wide: a still-broken dependency keeps its error; a
         * now-clean or no-longer-referenced file is cleared by the publish cycle.
         * (Marking all dirty re-runs each affected unit once — cheap; close is rare.) */
        for (int i = 0; i < S->store.count; i++) S->store.docs[i].dirty = true;
        flush_dirty(S);
    } else {
        /* Last document closed: nothing anchors the project view, so drop every
         * unit and clear every URI we published (incl. the just-closed file's). */
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
    /* type_ref_sym alone is enough to render: a MODULE symbol carries no value
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
        /* Doc comment at the definition site. The site may live in another file (a
         * sibling or the stdlib), so read whichever buffer backs it; reuse the open
         * doc's line index when the site is in the open file. */
        char *doc_md = NULL;
        if (!hit.no_doc) {
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

        if (hit.type_ref_sym || hit.decl_site) {
            /* The hovered token names a TYPE or MODULE, not a value: render a
             * declaration-form header (`enum dir of i32`, `module sfx`) instead
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
                hit.companion->decl->loc.line > 0) {
                SrcLoc csite = hit.companion->decl->loc;
                int flen = 0; bool owned = false;
                const char *ftext = doc_file_text(S, doc, csite.filename, &flen, &owned);
                if (ftext) {
                    LineIndex fidx = (ftext == doc->text) ? idx
                                   : line_index_build(a, ftext, flen);
                    comp_md = extract_doc_comment(a, ftext, flen, &fidx, csite.line, false);
                    if (owned) free((void *)ftext);
                }
            }

            if (hit.companion) {
                /* Render the module half whenever the pair exists — a companion
                 * without its own doc comment still gets its `module name`
                 * fence, so the pairing itself is always visible. A rule
                 * separates the pair's sections. Drawn with U+2500 text
                 * rather than markdown `---`: VSCode colors a markdown <hr>
                 * with the theme's editorHoverWidget.border, which is
                 * invisible in themes that draw borderless hovers. */
                #define HOVER_RULE "────────────────────────────────"
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

    /* Build a file:// uri for the definition path. Sized to the path: a clipped
     * URI still looks like a URI, so the editor would silently open the wrong
     * file (or nothing) rather than report a problem. */
    const char *def_uri = strcmp(def_path, doc->path) == 0
        ? doc->uri
        : arena_sprintf(a, "file://%s", def_path);

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
    const char *title;
    /* For a lambda binding the parameter types are written explicitly at the
     * definition site, so the only new information is the inferred return type:
     * render `:-> ret`. Everything else — including a plain function-reference
     * binding (`let f = g`), whose params are NOT visible here — shows the full
     * type. Hover is unaffected and always reports the full type. */
    if (init_is_lambda && type->kind == TYPE_FUNC && type->func.return_type) {
        /* Inline keeps a space after the colon to match the plain `: T` hints
         * (`let f: -> ret`); the standalone CodeLens reads fine tight (`:-> ret`). */
        title = arena_sprintf(lc->a, lc->inlay ? ": -> %s" : ":-> %s",
                              type_name(type->func.return_type));
    } else {
        title = arena_sprintf(lc->a, ": %s", type_name(type));
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
        case EXPR_BITCAST: lens_expr(e->bitcast_expr.operand, lc); break;
        case EXPR_ENUM_OF: lens_expr(e->enum_of_expr.operand, lc); break;
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
        case EXPR_OK:     lens_expr(e->ok_expr.value, lc); break;
        case EXPR_ERR:    lens_expr(e->err_expr.code, lc); break;
        case EXPR_ERROR_NAME: lens_expr(e->error_name_expr.code, lc); break;
        case EXPR_DEFER:  lens_expr(e->defer_expr.value, lc); break;
        case EXPR_IGNORE: lens_expr(e->ignore_expr.value, lc); break;
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

static const char *KEYWORDS[] = {
    /* mirrors lexer.c keyword set + builtins + primitive type names */
    "let", "mut", "struct", "union", "enum", "module", "namespace", "import", "from",
    "as", "extern", "private", "match", "with", "when", "if", "then", "else",
    "for", "in", "loop", "do", "break", "continue", "return", "defer", "ignore", "some",
    "true", "false", "none", "void", "guarded", "unguarded", "checked",
    "unchecked", "alloc", "alloca", "free", "sizeof", "alignof", "bitcast",
    "default", "const", "assert", "atomic_load_acquire", "atomic_store_release",
    "ok", "err", "error", "error_name", "enum_of",
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
        case DECL_ENUM:   return CIK_ENUM;
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

/* Peel option/pointer one level to reach a struct/union for member access. */
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
        case EXPR_BITCAST: harvest_expr(e->bitcast_expr.operand, names, n, cap); break;
        case EXPR_ENUM_OF: harvest_expr(e->enum_of_expr.operand, names, n, cap); break;
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
        case EXPR_OK: harvest_expr(e->ok_expr.value, names, n, cap); break;
        case EXPR_ERR: harvest_expr(e->err_expr.code, names, n, cap); break;
        case EXPR_ERROR_NAME: harvest_expr(e->error_name_expr.code, names, n, cap); break;
        case EXPR_DEFER: harvest_expr(e->defer_expr.value, names, n, cap); break;
        case EXPR_IGNORE: harvest_expr(e->ignore_expr.value, names, n, cap); break;
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

/* Member completion after '.', '::' (dot) or '->' (arrow). `dot_byte` is the
 * position of the operator's first char; the object expression ends at
 * dot_byte-1. Offers, by object kind: a numeric type name's properties; module
 * members; a slice's len/ptr; an option's is_some/is_none; a struct's fields; a
 * union's variants. A '.' on a pointer auto-derefs one level to the pointee
 * struct's members (the retired '->' did this explicitly). `arrow` is still
 * honored only to suppress type-name/module completion, which '->' never had. */
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

    /* One AST walk resolves two things: the anchor node (the object's last char,
     * for simple `a.b` / `::` paths) and — via the op-position hook — the field
     * node at the operator, whose `field.object` gives the object type for ANY
     * shape (`dict[i]`, `f()`, nested), which the anchor can't when the object
     * ends in `]`/`)`. */
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

    /* Module members ('.'/'::'): the object resolves to a module — the anchor
     * landed on the module name ('::' paths, simple `mod.`), or the field node's
     * object is a module-typed ident or a nested `a.b` module member. (A union
     * with a companion module keeps its variants below: resolved_sym is the
     * union, not the module, so `mod` stays NULL here.) */
    Symbol *mod = NULL;
    if (!arrow) {
        if (c.sym && c.sym->kind == DECL_MODULE) mod = c.sym;
        else if (obj && obj->kind == EXPR_IDENT && obj->ident.resolved_sym &&
                 obj->ident.resolved_sym->kind == DECL_MODULE)
            mod = obj->ident.resolved_sym;
        else if (obj && obj->kind == EXPR_FIELD && obj->field.resolved_member &&
                 obj->field.resolved_member->kind == DECL_MODULE)
            mod = obj->field.resolved_member;
    }
    if (mod && mod->members) {
        emit_module_members(a, arr, mod);
        return true;
    }

    /* Type-NAME object (an ident or module path resolving to a struct/union/
     * enum): what pass2 accepts after the '.' is the companion module's
     * members (i128.parse) and, for unions/enums, variant constructors —
     * never the struct's fields (those need a value, and `i128.limbs` is a
     * compile error). Mirror that here instead of falling through to the
     * value dispatch below, which would offer the fields. */
    Symbol *tsym = NULL;
    if (!arrow) {
        if (obj && obj->kind == EXPR_IDENT && obj->ident.resolved_sym &&
            (obj->ident.resolved_sym->kind == DECL_STRUCT ||
             obj->ident.resolved_sym->kind == DECL_UNION ||
             obj->ident.resolved_sym->kind == DECL_ENUM))
            tsym = obj->ident.resolved_sym;
        else if (obj && obj->kind == EXPR_FIELD && obj->field.resolved_member &&
                 !obj->field.is_variant_constructor && !obj->field.is_type_property &&
                 (obj->field.resolved_member->kind == DECL_STRUCT ||
                  obj->field.resolved_member->kind == DECL_UNION ||
                  obj->field.resolved_member->kind == DECL_ENUM))
            tsym = obj->field.resolved_member;
        else if (!obj && c.sym &&
                 (c.sym->kind == DECL_STRUCT || c.sym->kind == DECL_UNION ||
                  c.sym->kind == DECL_ENUM))
            tsym = c.sym;
    }
    if (tsym) {
        Symbol *comp = (obj && obj->kind == EXPR_IDENT)
                     ? obj->ident.companion_module : NULL;
        if (!comp) comp = companion_of_type_sym(r, tsym);
        if (comp && comp->members)
            emit_module_members(a, arr, comp);
        Type *tt = tsym->type;
        if (tt && tt->kind == TYPE_UNION) {
            for (int i = 0; i < tt->unio.variant_count; i++)
                add_item(a, arr, tt->unio.variants[i].name, CIK_ENUMMEMBER, NULL);
        } else if (tt && tt->kind == TYPE_ENUM) {
            bool has_count_variant = false;
            for (int i = 0; i < tt->enu.variant_count; i++) {
                add_item(a, arr, tt->enu.variants[i].name, CIK_ENUMMEMBER, NULL);
                if (strcmp(tt->enu.variants[i].name, "count") == 0)
                    has_count_variant = true;
            }
            if (!has_count_variant)
                add_item(a, arr, "count", CIK_FIELD, "i32");
        }
        return true;
    }

    /* Value dispatch on the object's type: the field node's object (any shape),
     * else the anchored node (simple idents the field capture didn't reach). */
    Type *t = (obj && obj->type) ? obj->type : c.type;
    /* '.' auto-derefs a single pointer level to the pointee struct (matching
     * pass2's `.`-on-pointer rewrite; the old `->` accessor is retired). A
     * numeric type name or module was handled above and is never pointer-typed
     * here, so dereferencing unconditionally is safe for '.'. */
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
    /* Union variants (bare-name construction site). */
    if (t->kind == TYPE_UNION) {
        for (int i = 0; i < t->unio.variant_count; i++)
            add_item(a, arr, t->unio.variants[i].name, CIK_ENUMMEMBER, NULL);
        return true;
    }
    /* Enum variants + the count type property (a declared `count` variant wins,
     * mirroring pass2's precedence, so dedup by name-first ordering). */
    if (t->kind == TYPE_ENUM) {
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
        char *full = str_sprintf("%s/%s", dir, nm);   /* sized to fit */
        struct stat st;
        int len;
        char *buf = NULL;
        if (!(stat(full, &st) == 0 && S_ISDIR(st.st_mode)))
            buf = read_whole_file(full, &len);
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
        free(S.store.docs[i].uri);
        free(S.store.docs[i].path);
        free(S.store.docs[i].text);
        free(S.store.docs[i].unit_key);
    }
    free(S.store.docs);
    for (int i = 0; i < S.unit_count; i++) {
        unit_free_results(&S.units[i]);
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
