#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* On Windows, fcc must link against UCRT, the same policy as the emitted FC
 * programs (see the prelude guard in codegen.c). Checking here reports the
 * problem when fcc is built rather than when its first FC program is. The
 * check comes after the standard headers because _UCRT is defined by
 * MinGW-w64's <_mingw.h>, which <stdint.h> and the others pull in, not by the
 * compiler itself. */
#if defined(_WIN32) && !defined(_UCRT)
#error "FC on Windows requires the UCRT runtime; msvcrt is not supported."
#endif

/* ---- Checked allocation ---- */

/* Report "fcc: out of memory" and exit. The x* allocators call it on failure,
 * so their results are never NULL. */
_Noreturn void out_of_memory(void);
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);

/* ---- Arena allocator ---- */

#define ARENA_PAGE_SIZE (64 * 1024)

typedef struct ArenaPage {
    struct ArenaPage *next;
    size_t used;
    size_t size;
    char data[];
} ArenaPage;

typedef struct Arena {
    ArenaPage *first;
    ArenaPage *current;
} Arena;

void arena_init(Arena *a);
void *arena_alloc(Arena *a, size_t size);
char *arena_strdup(Arena *a, const char *s, int len);
/* Copy `count` elements of `size` bytes into the arena; NULL when count is 0.
 * Typically moves a DA_APPEND scratch array into the AST before freeing it. */
void *arena_dup(Arena *a, const void *src, int count, size_t size);
void arena_free(Arena *a);

/* ---- Exact-size string formatting ---- */

/* printf-format into a fresh allocation sized to the result.
 *
 * Use these instead of `char buf[N]` + snprintf wherever the formatted text
 * embeds an FC identifier, a qualified name, a type spelling, or a filesystem
 * path. Those are unbounded, snprintf reports a cut only through a return
 * value that is easy to drop, and a truncated name is not invalid but a
 * different valid name: `a.bcd` cut to `a.bc` resolves to another symbol, and
 * two namespaces cut to a common prefix mangle onto one C symbol.
 *
 * `str_sprintf` returns malloc'd memory the caller frees. `arena_sprintf`
 * returns memory that lives as long as the arena. Both abort on allocation
 * failure, like arena_alloc, so callers never see NULL. */
char *str_sprintf(const char *fmt, ...);
char *arena_sprintf(Arena *a, const char *fmt, ...);

/* va_list form of str_sprintf, for the varargs functions that forward. `ap` is
 * consumed (the caller still owns the va_end). */
char *str_vsprintf(const char *fmt, va_list ap);

/* Append printf-formatted text to a malloc'd string and return the result,
 * which may have moved; the old pointer is consumed. Passing NULL starts a
 * fresh string, so a build loop needs no special first iteration. Use it to
 * assemble a name, path, or diagnostic descriptor of unknown final length. */
char *str_appendf(char *acc, const char *fmt, ...);

/* ---- Strings and files ---- */

/* malloc'd, NUL-terminated copies of `s` and of its first `n` bytes. */
char *str_dup(const char *s);
char *str_ndup(const char *s, size_t n);

/* The whole file as a malloc'd, NUL-terminated buffer, or NULL if it cannot
 * be read. `len` (optional) receives the byte count. */
char *read_file(const char *path, int *len);

/* ---- String interning ---- */

typedef struct InternEntry {
    const char *str;
    int length;
    uint32_t hash;
} InternEntry;

typedef struct InternTable {
    InternEntry *entries;
    int count;
    int capacity;
    Arena *arena;
} InternTable;

void intern_init(InternTable *t, Arena *a);
const char *intern(InternTable *t, const char *s, int len);
const char *intern_cstr(InternTable *t, const char *s);

/* Format and intern in one step, for name building (a qualified name, a
 * mangled prefix, a namespace path) where only the interned result is kept.
 * Sized to the result like str_sprintf, so no component is clipped. */
const char *intern_sprintf(InternTable *t, const char *fmt, ...);

/* A namespace as the program writes it ("acme::gfx") from the joined form the
 * compiler keys it by ("acme__gfx"). The parser joins a nested namespace's
 * parts with "__", which no identifier contains, so the split is exact. */
const char *ns_display(InternTable *t, const char *ns);
/* The same, as a malloc'd string the caller frees (for a diagnostic). */
char *ns_display_dup(const char *ns);

/* ---- Slice length representation (--len-repr) ----
 *
 * Every slice/string length has type i64 on every profile; the type checker
 * never sees this setting. --len-repr selects only the stored width of the len
 * field in the emitted C (fc_len_t), so small targets get native-width slices
 * and guards. Invariant: every stored len is proven in [0, fc_len_max()] when
 * the slice is constructed (statically when the value is known at compile
 * time, by an abort guard otherwise). Reads then widen losslessly and bounds
 * checks compare at the stored width. The default is 64. */
extern int g_len_repr;            /* 16, 32, or 64 */
int64_t fc_len_max(void);         /* INT16_MAX / INT32_MAX / INT64_MAX */

/* The value of hex digit `c` (0-15), or -1 when it is not one. */
int hex_digit_val(char c);

/* Decoded byte length of string-literal source text (escapes collapsed, `%%`
 * read as `%`); when `out` is non-NULL, also writes the bytes. pass2's
 * capacity checks and codegen's emission both use this one routine, so a
 * length always matches the bytes emitted. */
int decode_str_lit(const char *s, int slen, unsigned char *out);

/* ---- C identifier hygiene ---- */

/* True if `name` is a C reserved word (a C11 or C23 keyword, or another
 * implementation-reserved spelling) and so cannot be any C identifier,
 * including a struct/union member, a parameter, or a local. FC allows these
 * spellings as identifiers (e.g. `register`, `restrict`), so codegen escapes
 * them before they reach C. */
bool is_c_reserved(const char *name);

/* Map a user identifier to a C-safe spelling: the interned "fc__"+name when
 * `name` is a C reserved word, otherwise `name` unchanged. No FC identifier
 * contains `__` (the lexer rejects it), so the escaped form cannot collide
 * with a user identifier. The mapping is deterministic and idempotent, so
 * declaration and use sites that both call it agree without shared state. */
const char *c_safe_ident(InternTable *t, const char *name);

/* True if `name` starts with `fc__`, the root of every FC declaration's
 * emitted name (mangle_root in pass1.c). Such a name spells its whole
 * declaration path (`fc__ns__mod__type`), so it identifies one declaration on
 * its own: it resolves from any namespace, and the ns_prefix filter that
 * disambiguates source names across namespaces must not hide it. No
 * user-written name starts with `fc__`: the lexer rejects `__` in FC
 * identifiers, and the parser rejects an extern C name with this prefix. */
bool is_mangled_root_name(const char *name);

/* The last path component of a mangled name (`fc__m__point` -> `point`), the
 * declaration's source spelling; a name with no "__" is returned as is. */
const char *mangled_source_name(const char *cname);

/* ---- Dynamic array ---- */

#define DA_APPEND(arr, len, cap, val) do {          \
    if ((len) >= (cap)) {                           \
        (cap) = (cap) ? (cap) * 2 : 8;             \
        (arr) = xrealloc((arr), (size_t)(cap) * sizeof(*(arr))); \
    }                                               \
    (arr)[(len)++] = (val);                         \
} while (0)

#define DA_FREE(arr) do { free(arr); (arr) = NULL; } while (0)
