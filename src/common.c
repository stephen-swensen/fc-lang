#include "common.h"
#include <stdio.h>

/* ---- Arena allocator ---- */

static ArenaPage *arena_new_page(size_t min_size) {
    size_t size = ARENA_PAGE_SIZE;
    if (min_size > size) size = min_size;
    ArenaPage *page = malloc(sizeof(ArenaPage) + size);
    if (!page) {
        fprintf(stderr, "fc: out of memory\n");
        exit(1);
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
    char *dup = arena_alloc(a, (size_t)len + 1);
    memcpy(dup, s, (size_t)len);
    dup[len] = '\0';
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
            /* Empty slot — insert */
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
