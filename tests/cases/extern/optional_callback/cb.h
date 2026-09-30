#ifndef CB_H
#define CB_H
#include <stdint.h>
typedef int32_t (*cb_fn_t)(int32_t);
/* Calls the callback when one is given, as C APIs with optional hooks do. */
static inline int32_t call_opt(cb_fn_t f, int32_t x) { return f ? f(x) : -1; }
#endif
