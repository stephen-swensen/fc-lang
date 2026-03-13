#include "types.h"
#include <stdio.h>

/* Singleton primitive types */
static Type primitives[TYPE_COUNT];
static bool primitives_init = false;

static void init_primitives(void) {
    if (primitives_init) return;
    for (int i = 0; i < TYPE_COUNT; i++) {
        primitives[i].kind = (TypeKind)i;
    }
    primitives_init = true;
}

#define PRIM(name, kind_val) \
    Type *type_##name(void) { init_primitives(); return &primitives[kind_val]; }

PRIM(int8,    TYPE_INT8)
PRIM(int16,   TYPE_INT16)
PRIM(int32,   TYPE_INT32)
PRIM(int64,   TYPE_INT64)
PRIM(uint8,   TYPE_UINT8)
PRIM(uint16,  TYPE_UINT16)
PRIM(uint32,  TYPE_UINT32)
PRIM(uint64,  TYPE_UINT64)
PRIM(float32, TYPE_FLOAT32)
PRIM(float64, TYPE_FLOAT64)
PRIM(bool,    TYPE_BOOL)
PRIM(void,    TYPE_VOID)
PRIM(str,     TYPE_STR)
PRIM(cstr,    TYPE_CSTR)
PRIM(char,    TYPE_CHAR)
PRIM(any_ptr, TYPE_ANY_PTR)

#undef PRIM

Type *type_pointer(Arena *a, Type *pointee) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = TYPE_POINTER;
    t->pointer.pointee = pointee;
    return t;
}

Type *type_slice(Arena *a, Type *elem) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = TYPE_SLICE;
    t->slice.elem = elem;
    return t;
}

Type *type_option(Arena *a, Type *inner) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = TYPE_OPTION;
    t->option.inner = inner;
    return t;
}

bool type_is_integer(Type *t) {
    return t->kind >= TYPE_INT8 && t->kind <= TYPE_UINT64;
}

bool type_is_signed(Type *t) {
    return t->kind >= TYPE_INT8 && t->kind <= TYPE_INT64;
}

bool type_is_unsigned(Type *t) {
    return t->kind >= TYPE_UINT8 && t->kind <= TYPE_UINT64;
}

bool type_is_float(Type *t) {
    return t->kind == TYPE_FLOAT32 || t->kind == TYPE_FLOAT64;
}

bool type_is_numeric(Type *t) {
    return type_is_integer(t) || type_is_float(t);
}

bool type_eq(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind != b->kind) {
        /* A struct stub might need to match a union type or vice versa.
         * Both stub and real type might have different kinds if parser defaults to TYPE_STRUCT. */
        if ((a->kind == TYPE_STRUCT && b->kind == TYPE_UNION) ||
            (a->kind == TYPE_UNION && b->kind == TYPE_STRUCT)) {
            /* Compare by name — one might be a parser stub */
            const char *na = a->kind == TYPE_STRUCT ? a->struc.name : a->unio.name;
            const char *nb = b->kind == TYPE_STRUCT ? b->struc.name : b->unio.name;
            return na == nb;  /* interned string comparison */
        }
        return false;
    }
    switch (a->kind) {
    case TYPE_POINTER: return type_eq(a->pointer.pointee, b->pointer.pointee);
    case TYPE_SLICE:   return type_eq(a->slice.elem, b->slice.elem);
    case TYPE_OPTION:  return type_eq(a->option.inner, b->option.inner);
    case TYPE_STRUCT:  return a->struc.name == b->struc.name;
    case TYPE_UNION:   return a->unio.name == b->unio.name;
    default: return true;   /* primitives match by kind */
    }
}

static const char *primitive_names[] = {
    [TYPE_INT8]    = "int8",
    [TYPE_INT16]   = "int16",
    [TYPE_INT32]   = "int32",
    [TYPE_INT64]   = "int64",
    [TYPE_UINT8]   = "uint8",
    [TYPE_UINT16]  = "uint16",
    [TYPE_UINT32]  = "uint32",
    [TYPE_UINT64]  = "uint64",
    [TYPE_FLOAT32] = "float32",
    [TYPE_FLOAT64] = "float64",
    [TYPE_BOOL]    = "bool",
    [TYPE_VOID]    = "void",
    [TYPE_STR]     = "str",
    [TYPE_STR32]   = "str32",
    [TYPE_CSTR]    = "cstr",
    [TYPE_CHAR]    = "char",
    [TYPE_ANY_PTR] = "any*",
};

const char *type_name(Type *t) {
    if (t->kind < TYPE_POINTER) {
        return primitive_names[t->kind];
    }
    /* For compound types, return a placeholder */
    switch (t->kind) {
    case TYPE_POINTER: return "T*";
    case TYPE_SLICE:   return "T[]";
    case TYPE_OPTION:  return "T?";
    case TYPE_FUNC:    return "(fn)";
    case TYPE_STRUCT:  return t->struc.name;
    case TYPE_UNION:   return t->unio.name;
    default:           return "?";
    }
}

bool type_can_widen(Type *from, Type *to) {
    if (type_eq(from, to)) return true;
    TypeKind f = from->kind, t = to->kind;

    /* int8 → int16, int32, int64 */
    if (f == TYPE_INT8)   return t == TYPE_INT16 || t == TYPE_INT32 || t == TYPE_INT64;
    /* int16 → int32, int64 */
    if (f == TYPE_INT16)  return t == TYPE_INT32 || t == TYPE_INT64;
    /* int32 → int64 */
    if (f == TYPE_INT32)  return t == TYPE_INT64;

    /* uint8 → uint16, uint32, uint64, int16, int32, int64 */
    if (f == TYPE_UINT8)  return t == TYPE_UINT16 || t == TYPE_UINT32 || t == TYPE_UINT64 ||
                                 t == TYPE_INT16  || t == TYPE_INT32  || t == TYPE_INT64;
    /* uint16 → uint32, uint64, int32, int64 */
    if (f == TYPE_UINT16) return t == TYPE_UINT32 || t == TYPE_UINT64 ||
                                 t == TYPE_INT32  || t == TYPE_INT64;
    /* uint32 → uint64, int64 */
    if (f == TYPE_UINT32) return t == TYPE_UINT64 || t == TYPE_INT64;

    /* float32 → float64 */
    if (f == TYPE_FLOAT32) return t == TYPE_FLOAT64;

    return false;
}

Type *type_common_numeric(Type *a, Type *b) {
    if (type_eq(a, b)) return a;
    if (type_can_widen(a, b)) return b;
    if (type_can_widen(b, a)) return a;
    return NULL;
}

Type *type_from_int_suffix(const char *suffix, int len) {
    if (!suffix || len == 0) return type_int32();

    struct { const char *s; int l; Type *(*fn)(void); } map[] = {
        {"i8",  2, type_int8},   {"i16", 3, type_int16},
        {"i32", 3, type_int32},  {"i64", 3, type_int64},
        {"u8",  2, type_uint8},  {"u16", 3, type_uint16},
        {"u32", 3, type_uint32}, {"u64", 3, type_uint64},
    };

    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (map[i].l == len && memcmp(suffix, map[i].s, (size_t)len) == 0) {
            return map[i].fn();
        }
    }
    return NULL;
}
