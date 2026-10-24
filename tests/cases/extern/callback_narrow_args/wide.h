#ifndef WIDE_H
#define WIDE_H
#include <stdint.h>
/* Call f with junk in its argument register above the argument. The System
 * V ABI leaves those bits undefined, so a callee must not read them. The call
 * goes through a pointer whose parameter is 64 bits wide, which puts the
 * junk there. */
typedef int64_t (*i32_fn)(int32_t);
typedef int64_t (*u8_fn)(uint8_t);
typedef int64_t (*wide_fn)(int64_t);
static inline int64_t call_i32_junk(i32_fn f) { return ((wide_fn)(void (*)(void))f)((int64_t)0x123456780000002aLL); }
static inline int64_t call_i32_neg(i32_fn f) { return ((wide_fn)(void (*)(void))f)((int64_t)0x00000000fffffff6LL); }
static inline int64_t call_u8_junk(u8_fn f) { return ((wide_fn)(void (*)(void))f)((int64_t)0x7f7f7f7f7f7f7fc8LL); }
#endif
