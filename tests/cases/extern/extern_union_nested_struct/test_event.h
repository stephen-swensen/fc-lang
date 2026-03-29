#ifndef TEST_EVENT_H
#define TEST_EVENT_H
#include <stdint.h>

struct keysym {
    int32_t scancode;
    int32_t sym;
};

struct key_event {
    uint32_t type;
    uint32_t timestamp;
    struct keysym keysym;
};

union event {
    uint32_t type;
    struct key_event key;
};

#endif
