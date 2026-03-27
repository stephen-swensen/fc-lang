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
    TYPE_ISIZE,
    TYPE_USIZE,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_CHAR,
    TYPE_POINTER,
    TYPE_SLICE,
    TYPE_OPTION,
    TYPE_FUNC,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ANY_PTR,
    TYPE_TYPE_VAR,
    TYPE_FIXED_ARRAY, /* fixed-size inline array: T[N] */
    TYPE_ERROR,      /* poison type for error recovery */

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
    const char *alias;  /* display name override (e.g. "str" for uint8[], "cstr" for uint8*) */
    bool is_const;      /* const qualifier for pointer/slice types */
    union {
        struct { Type *pointee; } pointer;
        struct { Type *elem; } slice;
        struct { Type *inner; } option;
        struct {
            Type **param_types;
            int param_count;
            Type *return_type;
            bool is_variadic;
        } func;
        struct {
            const char *name;
            const char *base_name;      /* original FC name before mangling (for diagnostics) */
            const char *qualified_name;  /* fully qualified FC path (e.g. "std::types.tuple2") */
            const char *c_name;         /* C struct/union tag name for extern types, NULL for normal */
            bool is_c_union;            /* true for extern union (untagged C union layout) */
            StructField *fields;
            int field_count;
            Type **type_args;
            int type_arg_count;
        } struc;
        struct {
            const char *name;
            const char *base_name;      /* original FC name before mangling (for diagnostics) */
            const char *qualified_name;  /* fully qualified FC path (e.g. "geometry.shape") */
            UnionVariant *variants;
            int variant_count;
            Type **type_args;
            int type_arg_count;
        } unio;
        struct { Type *elem; int64_t size; } fixed_array;
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
Type *type_isize(void);
Type *type_usize(void);
Type *type_float32(void);
Type *type_float64(void);
Type *type_bool(void);
Type *type_void(void);
Type *type_str(void);
Type *type_str32(void);
Type *type_cstr(void);
Type *type_const_str(void);
Type *type_const_cstr(void);
Type *type_char(void);
Type *type_any_ptr(void);
Type *type_error(void);
bool type_is_error(Type *t);
bool type_is_const(Type *t);

/* Type construction */
Type *type_pointer(Arena *a, Type *pointee);
Type *type_slice(Arena *a, Type *elem);
Type *type_option(Arena *a, Type *inner);
Type *type_fixed_array(Arena *a, Type *elem, int64_t size);

/* Alias type helpers: str = uint8[], cstr = uint8*, str32 = uint32[] */
bool is_str_type(Type *t);
bool is_cstr_type(Type *t);
bool is_str32_type(Type *t);

/* Const helpers */
Type *type_copy(Arena *a, Type *t);
Type *type_make_const(Arena *a, Type *t);

/* Queries */
bool type_is_integer(Type *t);
bool type_is_signed(Type *t);
bool type_is_unsigned(Type *t);
bool type_is_float(Type *t);
bool type_is_numeric(Type *t);
bool type_eq(Type *a, Type *b);
bool type_eq_ignore_const(Type *a, Type *b);
const char *type_name(Type *t);

/* Implicit widening: can 'from' widen to 'to' without explicit cast? */
bool type_can_widen(Type *from, Type *to);

/* Find the common (wider) numeric type for two types, or NULL if no widening possible */
Type *type_common_numeric(Type *a, Type *b);

/* Map a type suffix string (e.g., "i8", "u64") to a type, or NULL */
Type *type_from_int_suffix(const char *suffix, int len);

/* Map a type name string (e.g., "int32", "bool", "str") to a type, or NULL */
Type *type_from_name(const char *s, int len);

/* Type variable constructor */
Type *type_type_var(Arena *a, const char *name);

/* Does this type need a generated eq function (as opposed to C native ==)? */
bool type_needs_eq_func(Type *t);

/* Check if a type contains any type variables (recursively) */
bool type_contains_type_var(Type *t);

/* Collect unique type variable names from a type in order of first appearance */
void type_collect_vars(Type *t, const char ***vars, int *count, int *cap);

/* Substitute type variables: replace TYPE_TYPE_VAR with concrete types */
Type *type_substitute(Arena *a, Type *t, const char **var_names, Type **concrete, int count);

/* Mangle a type name for use in C identifiers.
 * Returns a malloc'd string that the caller must free. */
char *mangle_type_name(Type *t);

/* Build mangled name for a generic instantiation, e.g. "fc_identity_int32" */
const char *mangle_generic_name(Arena *a, InternTable *intern,
                                const char *base, Type **type_args, int count);
