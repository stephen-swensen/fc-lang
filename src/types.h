#pragma once
#include "common.h"

typedef enum {
    TYPE_INT8,
    TYPE_INT16,
    TYPE_INT32,
    TYPE_INT64,
    TYPE_UINT8,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_UINT64,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_STR,
    TYPE_STR32,
    TYPE_CSTR,
    TYPE_CHAR,
    TYPE_POINTER,
    TYPE_SLICE,
    TYPE_OPTION,
    TYPE_FUNC,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ANY_PTR,
    TYPE_TYPE_VAR,

    TYPE_COUNT
} TypeKind;

typedef struct Type Type;
typedef struct StructField StructField;
typedef struct UnionVariant UnionVariant;

struct StructField {
    const char *name;
    Type *type;
};

struct UnionVariant {
    const char *name;
    Type *payload;  /* NULL if no payload */
};

struct Type {
    TypeKind kind;
    union {
        struct { Type *pointee; } pointer;
        struct { Type *elem; } slice;
        struct { Type *inner; } option;
        struct {
            Type **param_types;
            int param_count;
            Type *return_type;
        } func;
        struct {
            const char *name;
            StructField *fields;
            int field_count;
        } struc;
        struct {
            const char *name;
            UnionVariant *variants;
            int variant_count;
        } unio;
        struct { const char *name; } type_var;
    };
};

/* Get singleton types for primitives */
Type *type_int8(void);
Type *type_int16(void);
Type *type_int32(void);
Type *type_int64(void);
Type *type_uint8(void);
Type *type_uint16(void);
Type *type_uint32(void);
Type *type_uint64(void);
Type *type_float32(void);
Type *type_float64(void);
Type *type_bool(void);
Type *type_void(void);
Type *type_str(void);
Type *type_cstr(void);
Type *type_char(void);
Type *type_any_ptr(void);

/* Type construction */
Type *type_pointer(Arena *a, Type *pointee);
Type *type_slice(Arena *a, Type *elem);
Type *type_option(Arena *a, Type *inner);

/* Queries */
bool type_is_integer(Type *t);
bool type_is_signed(Type *t);
bool type_is_unsigned(Type *t);
bool type_is_float(Type *t);
bool type_is_numeric(Type *t);
bool type_eq(Type *a, Type *b);
const char *type_name(Type *t);

/* Implicit widening: can 'from' widen to 'to' without explicit cast? */
bool type_can_widen(Type *from, Type *to);

/* Find the common (wider) numeric type for two types, or NULL if no widening possible */
Type *type_common_numeric(Type *a, Type *b);

/* Map a type suffix string (e.g., "i8", "u64") to a type, or NULL */
Type *type_from_int_suffix(const char *suffix, int len);
