#include "common.h"
#include <assert.h>
#include <stdio.h>

/* ---- Slice length representation (--len-repr) ---- */

int g_len_repr = 64;

int64_t fc_len_max(void) {
    switch (g_len_repr) {
    case 16: return INT16_MAX;
    case 32: return INT32_MAX;
    default: return INT64_MAX;
    }
}

/* ---- String-literal decoding ---- */

int hex_digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode string-literal source text into raw bytes. The lexer has already
 * rejected malformed escapes, so every input decodes. Writes to `out` when
 * non-NULL and always returns the byte count, so the same routine sizes a
 * buffer and fills it. */
int decode_str_lit(const char *s, int slen, unsigned char *out) {
    int n = 0;
    for (int i = 0; i < slen; i++) {
        unsigned char b;
        if (s[i] == '%' && i + 1 < slen && s[i + 1] == '%') {
            b = '%';
            i++;
        } else if (s[i] == '\\' && i + 1 < slen) {
            i++;
            switch (s[i]) {
            case 'n': b = '\n'; break;
            case 't': b = '\t'; break;
            case 'r': b = '\r'; break;
            case '0': b = '\0'; break;
            case 'x':
                if (i + 2 < slen) {
                    b = (unsigned char)((hex_digit_val(s[i + 1]) << 4) |
                                        hex_digit_val(s[i + 2]));
                    i += 2;
                } else {
                    b = 'x';
                }
                break;
            default: b = (unsigned char)s[i]; break;  /* \\ \" \' */
            }
        } else {
            b = (unsigned char)s[i];
        }
        if (out) out[n] = b;
        n++;
    }
    return n;
}

_Noreturn void out_of_memory(void) {
    fprintf(stderr, "fcc: out of memory\n");
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) out_of_memory();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n ? n : 1);
    if (!r) out_of_memory();
    return r;
}

/* ---- Arena allocator ---- */

static ArenaPage *arena_new_page(size_t min_size) {
    size_t size = ARENA_PAGE_SIZE;
    if (min_size > size) size = min_size;
    ArenaPage *page = malloc(sizeof(ArenaPage) + size);
    if (!page) {
        out_of_memory();
    }
    page->next = NULL;
    page->used = 0;
    page->size = size;
    return page;
}

void arena_init(Arena *a) {
    a->first = arena_new_page(ARENA_PAGE_SIZE);
    a->current = a->first;
}

void *arena_alloc(Arena *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (a->current->used + size > a->current->size) {
        ArenaPage *page = arena_new_page(size);
        a->current->next = page;
        a->current = page;
    }

    void *ptr = a->current->data + a->current->used;
    a->current->used += size;
    memset(ptr, 0, size);
    return ptr;
}

char *arena_strdup(Arena *a, const char *s, int len) {
    /* A negative length is a caller bug (e.g. a source-span subtraction across
       unrelated buffers); the size_t cast would turn it into a huge
       allocation and an "out of memory" exit instead of a clear failure. */
    assert(len >= 0);
    char *dup = arena_alloc(a, (size_t)len + 1);
    memcpy(dup, s, (size_t)len);
    dup[len] = '\0';
    return dup;
}

void *arena_dup(Arena *a, const void *src, int count, size_t size) {
    if (count <= 0) return NULL;
    void *dup = arena_alloc(a, size * (size_t)count);
    memcpy(dup, src, size * (size_t)count);
    return dup;
}

void arena_free(Arena *a) {
    ArenaPage *page = a->first;
    while (page) {
        ArenaPage *next = page->next;
        free(page);
        page = next;
    }
    a->first = NULL;
    a->current = NULL;
}

/* ---- Exact-size string formatting ---- */

/* The formatted length. Measures on a copy of `ap`, because vsnprintf consumes
 * its va_list and the caller still needs `ap` for the real pass. */
static int format_len(const char *fmt, va_list ap) {
    va_list probe;
    va_copy(probe, ap);
    int n = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    /* vsnprintf only fails on an encoding error, which none of fcc's format
     * strings can produce; degrade to an empty string rather than a negative
     * allocation size. */
    return n < 0 ? 0 : n;
}

char *str_vsprintf(const char *fmt, va_list ap) {
    int n = format_len(fmt, ap);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        out_of_memory();
    }
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    return buf;
}

char *str_sprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *buf = str_vsprintf(fmt, ap);
    va_end(ap);
    return buf;
}

char *arena_sprintf(Arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = format_len(fmt, ap);
    char *buf = arena_alloc(a, (size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

char *str_appendf(char *acc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = format_len(fmt, ap);
    size_t base = acc ? strlen(acc) : 0;
    char *buf = realloc(acc, base + (size_t)n + 1);
    if (!buf) {
        out_of_memory();
    }
    vsnprintf(buf + base, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

/* ---- Strings and files ---- */

char *str_ndup(const char *s, size_t n) {
    char *copy = malloc(n + 1);
    if (!copy) out_of_memory();
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

char *str_dup(const char *s) {
    return str_ndup(s, strlen(s));
}

char *read_file(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)size + 1);
        if (!buf) out_of_memory();
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
        if (len) *len = (int)got;
    }
    fclose(f);
    return buf;
}

/* ---- String interning ---- */

static uint32_t fnv1a(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

void intern_init(InternTable *t, Arena *a) {
    t->capacity = 256;
    t->count = 0;
    t->arena = a;
    t->entries = calloc((size_t)t->capacity, sizeof(InternEntry));
}

static void intern_grow(InternTable *t) {
    int new_cap = t->capacity * 2;
    InternEntry *new_entries = calloc((size_t)new_cap, sizeof(InternEntry));

    for (int i = 0; i < t->capacity; i++) {
        if (t->entries[i].str) {
            uint32_t idx = t->entries[i].hash & (uint32_t)(new_cap - 1);
            while (new_entries[idx].str) {
                idx = (idx + 1) & (uint32_t)(new_cap - 1);
            }
            new_entries[idx] = t->entries[i];
        }
    }

    free(t->entries);
    t->entries = new_entries;
    t->capacity = new_cap;
}

const char *intern(InternTable *t, const char *s, int len) {
    if (t->count * 2 >= t->capacity) {
        intern_grow(t);
    }

    uint32_t h = fnv1a(s, len);
    uint32_t idx = h & (uint32_t)(t->capacity - 1);

    for (;;) {
        InternEntry *e = &t->entries[idx];
        if (!e->str) {
            /* Empty slot: insert */
            char *interned = arena_strdup(t->arena, s, len);
            e->str = interned;
            e->length = len;
            e->hash = h;
            t->count++;
            return interned;
        }
        if (e->hash == h && e->length == len && memcmp(e->str, s, (size_t)len) == 0) {
            return e->str;
        }
        idx = (idx + 1) & (uint32_t)(t->capacity - 1);
    }
}

const char *intern_cstr(InternTable *t, const char *s) {
    return intern(t, s, (int)strlen(s));
}

const char *intern_sprintf(InternTable *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = format_len(fmt, ap);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        out_of_memory();
    }
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    const char *result = intern(t, buf, n);
    free(buf);
    return result;
}

char *ns_display_dup(const char *ns) {
    /* Each "__" (2 bytes) becomes "::" (2 bytes), so the length is unchanged. */
    char *buf = str_dup(ns);
    char *w = buf;
    for (const char *r = ns; *r; ) {
        if (r[0] == '_' && r[1] == '_') { *w++ = ':'; *w++ = ':'; r += 2; }
        else *w++ = *r++;
    }
    *w = '\0';
    return buf;
}

const char *ns_display(InternTable *t, const char *ns) {
    if (!ns || !strstr(ns, "__")) return ns;
    char *buf = ns_display_dup(ns);
    const char *out = intern_cstr(t, buf);
    free(buf);
    return out;
}

/* ---- C identifier hygiene ---- */

/* C reserved spellings that must be escaped before reaching C. Covers the C11
 * keywords, the `_Capital` keyword family, and the C23 keywords plus the
 * <stdbool.h>/<assert.h> macros the prelude includes
 * (bool/true/false/static_assert). `bool` is a legal FC field or binding name;
 * the other three are FC keywords, listed so the escape stays correct if that
 * changes. Spellings containing `__` are omitted because the lexer rejects
 * `__` in FC identifiers. */
static const char *const C_RESERVED[] = {
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
    "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
    "alignas", "alignof", "asm", "auto", "bool", "break", "case", "char",
    "const", "constexpr", "continue", "default", "do", "double", "else",
    "enum", "extern", "false", "float", "for", "goto", "if", "inline", "int",
    "long", "nullptr", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "static_assert", "struct", "switch", "thread_local",
    "true", "typedef", "typeof", "typeof_unqual", "union", "unsigned", "void",
    "volatile", "while",
};

bool is_c_reserved(const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < sizeof(C_RESERVED) / sizeof(C_RESERVED[0]); i++) {
        if (strcmp(name, C_RESERVED[i]) == 0) return true;
    }
    return false;
}

const char *c_safe_ident(InternTable *t, const char *name) {
    if (!is_c_reserved(name)) return name;
    return intern_sprintf(t, "fc__%s", name);
}

/* c_safe_ident's escape also starts with `fc__`, but it is applied only to
 * member names (per-type namespaces), which never enter the symbol tables this
 * predicate gates. */
bool is_mangled_root_name(const char *name) {
    return name && strncmp(name, "fc__", 4) == 0;
}

/* Split non-overlapping from the left, the way the mangler joins components:
 * a component may start with `_`, so scanning for the last "__" would take
 * `fc__a___x` (component `_x`) apart one character off. */
const char *mangled_source_name(const char *cname) {
    const char *tail = cname;
    for (const char *p = cname; p[0] && p[1]; ) {
        if (p[0] == '_' && p[1] == '_') { tail = p + 2; p += 2; }
        else p++;
    }
    return tail;
}
