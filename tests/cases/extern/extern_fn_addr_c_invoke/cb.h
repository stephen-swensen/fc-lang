#ifndef CB_H
#define CB_H
#include <stdint.h>
typedef int32_t (*cb_fn_t)(int32_t);
/* Calls a raw C function pointer passed as void*. */
static inline int32_t cb_invoke(void *fn, int32_t x) { return ((cb_fn_t) fn)(x); }
static int32_t negate(int32_t x) { return -x; }
static int32_t cmp_i32(const void *a, const void *b) {
    return *(const int32_t *)a - *(const int32_t *)b;
}
#endif
