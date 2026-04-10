#ifndef TEST_PACKED_H
#define TEST_PACKED_H
#include <stdint.h>

/* Packed struct: no padding, fields laid out byte-by-byte.
   Unpadded size = 2 + 2 + 4 + 4 + 1 = 13 bytes.
   Without __attribute__((packed)) the C compiler would pad to 16. */
struct __attribute__((packed)) packed_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  flags;
};

_Static_assert(sizeof(struct packed_header) == 13,
               "packed_header must be 13 bytes");

#endif
