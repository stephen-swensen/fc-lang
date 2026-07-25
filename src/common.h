#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* On Windows, fcc must link against UCRT — same policy as the emitted FC
 * programs (see prelude guard in codegen.c). Fail at compiler build time
 * rather than letting users discover the gap later when their first FC
 * program hits the matching guard. The check sits *after* the standard
 * headers because _UCRT is defined in MinGW-w64's <_mingw.h> (pulled in
 * transitively by <stdint.h> et al.), not by the compiler itself. */
#if defined(_WIN32) && !defined(_UCRT)
#error "FC on Windows requires the UCRT runtime; msvcrt is not supported."
#endif

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
void arena_free(Arena *a);

/* ---- Exact-size string formatting ---- */

/* printf-format into a fresh allocation sized exactly to the result.
 *
 * These are the replacement for the `char buf[N]` + snprintf idiom anywhere the
 * formatted text embeds an FC identifier, a qualified name, a type spelling, or
 * a filesystem path. All of those are unbounded, and snprintf reports a cut only
 * through a return value that name-building code routinely drops — so the
 * failure is silent, and a truncated *name* does not become invalid, it becomes
 * a **different valid name**: `a.bcd` cut to `a.bc` resolves to a real other
 * symbol, and two namespaces cut to a common prefix mangle onto one C symbol.
 *
 * `str_sprintf` returns malloc'd memory the caller frees — the right shape when
 * the result is interned and then discarded. `arena_sprintf` returns arena
 * memory that lives as long as the arena. Both abort on allocation failure,
 * matching arena_alloc, so callers never see NULL. */
char *str_sprintf(const char *fmt, ...);
char *arena_sprintf(Arena *a, const char *fmt, ...);

/* va_list form of str_sprintf, for the varargs functions that forward. `ap` is
 * consumed (the caller still owns the va_end). */
char *str_vsprintf(const char *fmt, va_list ap);

/* Append printf-formatted text to a malloc'd string, returning the (possibly
 * moved) result and freeing nothing the caller still holds — the old pointer is
 * consumed. Passing NULL starts a fresh string, so a build loop needs no special
 * first iteration. This is the growing counterpart to str_sprintf: the way to
 * assemble a name, path, or diagnostic descriptor of unknown final length. */
char *str_appendf(char *acc, const char *fmt, ...);

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

/* Format and intern in one step. This is the shape almost every name-building
 * site wants — a qualified name, a mangled prefix, a namespace path — where the
 * formatted text is a temporary and only the interned result is kept. Sized
 * exactly (see str_sprintf), so no component can be silently clipped. */
const char *intern_sprintf(InternTable *t, const char *fmt, ...);

/* ---- C identifier hygiene ---- */

/* True if `name` is a C reserved word (C11/C23 keyword or implementation-
 * reserved spelling) that cannot appear as any C identifier — including a
 * struct/union member, a parameter, or a block-scope variable. FC permits
 * these spellings as user identifiers (e.g. `register`, `restrict`), so the
 * codegen must escape them before they reach C scope. */
bool is_c_reserved(const char *name);

/* Map a user identifier to a C-safe spelling. If `name` is a C reserved word,
 * returns the interned "fc__"+name; otherwise returns `name` unchanged. The
 * escaped form lives in the compiler-reserved `__` (double-underscore)
 * namespace — the lexer forbids `__` in FC identifiers — so it can never
 * collide with a user identifier, and (being keyword-free) is a valid C
 * identifier. Deterministic and idempotent: declaration and use sites that
 * both call this agree without any shared state. */
const char *c_safe_ident(InternTable *t, const char *name);

/* ---- Dynamic array ---- */

#define DA_APPEND(arr, len, cap, val) do {          \
    if ((len) >= (cap)) {                           \
        (cap) = (cap) ? (cap) * 2 : 8;             \
        (arr) = realloc((arr), (cap) * sizeof(*(arr))); \
    }                                               \
    (arr)[(len)++] = (val);                         \
} while (0)

#define DA_FREE(arr) do { free(arr); (arr) = NULL; } while (0)
