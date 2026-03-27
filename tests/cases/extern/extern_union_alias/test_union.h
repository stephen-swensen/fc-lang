#ifndef TEST_UNION_H
#define TEST_UNION_H
#include <stdint.h>

union IntOrFloat {
    int32_t i;
    float f;
};

#endif
