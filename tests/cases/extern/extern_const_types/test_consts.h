#ifndef TEST_CONSTS_H
#define TEST_CONSTS_H

#include <stdint.h>
#include <stdbool.h>

/* Integer constants — various widths */
#define CONST_I8   (-42)
#define CONST_I16  (-1000)
#define CONST_I32  (-100000)
#define CONST_I64  (-1000000000LL)
#define CONST_U8   200
#define CONST_U16  50000
#define CONST_U32  3000000000U
#define CONST_U64  9000000000000000000ULL

/* Float constants */
#define CONST_F32  3.14f
#define CONST_F64  2.718281828

/* Bool constant */
#define CONST_TRUE  true
#define CONST_FALSE false

/* Pointer constant */
#define CONST_NULL  ((void*)0)

/* String constant */
#define CONST_VERSION "1.0.0"

/* Bitwise flags */
#define FLAG_A  0x01
#define FLAG_B  0x02
#define FLAG_C  0x04

#endif
