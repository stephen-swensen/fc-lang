#ifndef CB_INVOKE_H
#define CB_INVOKE_H
#include <stdint.h>

typedef int32_t (*cb_fn_t)(int32_t);

/* Takes a raw C function pointer (passed via void*) and calls it with x. */
static inline int32_t cb_invoke(void *fn, int32_t x) {
    cb_fn_t f = (cb_fn_t) fn;
    return f(x);
}

/* Stores a callback in a struct; C helper invokes the callback. */
typedef struct cb_box {
    int32_t tag;
    void *fn;
} cb_box;

static inline int32_t cb_box_invoke(struct cb_box *b, int32_t x) {
    return ((cb_fn_t) b->fn)(x);
}

#endif
