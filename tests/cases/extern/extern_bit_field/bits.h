#ifndef BITS_H
#define BITS_H
#include <stdint.h>

/* C struct with bit fields. FC mirrors the fields as plain uint32 — the
   bit widths belong to the C compiler, which handles shift/mask automatically
   at each field access. */
struct reg {
    unsigned int enable   : 1;
    unsigned int mode     : 3;   /* 0..7 */
    unsigned int priority : 4;   /* 0..15 */
    unsigned int reserved : 24;
};

#endif
