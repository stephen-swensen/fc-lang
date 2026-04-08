#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- Dynamic array ---- */

#define DA_APPEND(arr, len, cap, val) do {          \
    if ((len) >= (cap)) {                           \
        (cap) = (cap) ? (cap) * 2 : 8;             \
        (arr) = realloc((arr), (cap) * sizeof(*(arr))); \
    }                                               \
    (arr)[(len)++] = (val);                         \
} while (0)

#define DA_FREE(arr) do { free(arr); (arr) = NULL; } while (0)
