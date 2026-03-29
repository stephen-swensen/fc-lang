#ifndef TEST_STRUCT_H
#define TEST_STRUCT_H
#include <stdint.h>

struct point {
    int32_t x;
    int32_t y;
};

struct rect {
    struct point origin;
    struct point size;
};

#endif
