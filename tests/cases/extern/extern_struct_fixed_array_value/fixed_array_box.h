#ifndef FC_TEST_FIXED_ARRAY_BOX_H
#define FC_TEST_FIXED_ARRAY_BOX_H

#include <string.h>

/* A C struct whose fixed-array field uses plain `char` (not `unsigned char`).
 * FC declares the matching field as `uint8[32]`, creating the signedness gap
 * that exposes the codegen bug when the struct is accessed by value. */
struct fc_box {
    char  name[32];
    int   filled;
};

static inline int fc_box_fill(struct fc_box *b, const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof b->name) n = sizeof b->name - 1;
    memcpy(b->name, s, n);
    b->name[n] = '\0';
    b->filled = (int)n;
    return b->filled;
}

#endif
