#ifndef TEST_UNION_H
#define TEST_UNION_H
#include <stdint.h>

struct key_event {
    uint32_t type;
    uint32_t timestamp;
    int32_t keycode;
};

struct mouse_event {
    uint32_t type;
    uint32_t timestamp;
    int32_t x;
    int32_t y;
};

union event {
    uint32_t type;
    struct key_event key;
    struct mouse_event mouse;
};

#endif
