#ifndef TEST_UNION_H
#define TEST_UNION_H
#include <stdint.h>

struct partial_a {
    int32_t x;
    int32_t y;
    int32_t z;
};

struct partial_b {
    float f;
    double d;
};

struct partial_c {
    int64_t big;
    int32_t small;
};

union big_union {
    int32_t tag;
    struct partial_a a;
    struct partial_b b;
    struct partial_c c;
    uint8_t raw[16];
};

#endif
