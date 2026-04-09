#include "types.h"
#include <stdio.h>

static char *str_dup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    memcpy(copy, s, len);
    return copy;
}

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
PRIM(isize,   TYPE_ISIZE)
PRIM(usize,   TYPE_USIZE)
PRIM(float32, TYPE_FLOAT32)
PRIM(float64, TYPE_FLOAT64)
PRIM(bool,    TYPE_BOOL)
PRIM(void,    TYPE_VOID)
/* char is an alias for uint8 per spec */
Type *type_char(void) { return type_uint8(); }
PRIM(any_ptr, TYPE_ANY_PTR)
PRIM(error,   TYPE_ERROR)

#undef PRIM

/* str = uint8[] with alias "str" */
Type *type_str(void) {
    static Type str_type = {0};
    static bool init = false;
    if (!init) {
        str_type.kind = TYPE_SLICE;
        str_type.alias = "str";
        str_type.slice.elem = type_uint8();
        init = true;
    }
    return &str_type;
}

/* cstr = uint8* with alias "cstr" */
Type *type_cstr(void) {
    static Type cstr_type = {0};
    static bool init = false;
    if (!init) {
        cstr_type.kind = TYPE_POINTER;
        cstr_type.alias = "cstr";
        cstr_type.pointer.pointee = type_uint8();
        init = true;
    }
    return &cstr_type;
}

/* const str = const uint8[] */
Type *type_const_str(void) {
    static Type const_str_type = {0};
    static bool init = false;
    if (!init) {
        const_str_type.kind = TYPE_SLICE;
        const_str_type.alias = "str";
        const_str_type.is_const = true;
        const_str_type.slice.elem = type_uint8();
        init = true;
    }
    return &const_str_type;
}

/* const cstr = const uint8* */
Type *type_const_cstr(void) {
    static Type const_cstr_type = {0};
    static bool init = false;
    if (!init) {
        const_cstr_type.kind = TYPE_POINTER;
        const_cstr_type.alias = "cstr";
        const_cstr_type.is_const = true;
        const_cstr_type.pointer.pointee = type_uint8();
        init = true;
    }
    return &const_cstr_type;
}

bool type_is_const(Type *t) {
    return t && t->is_const;
}

Type *type_copy(Arena *a, Type *t) {
    if (!t) return NULL;
    Type *c = arena_alloc(a, sizeof(Type));
    *c = *t;
    return c;
}

Type *type_make_const(Arena *a, Type *t) {
    if (!t) return NULL;
    if (t->is_const) return t;
    if (t->kind != TYPE_POINTER && t->kind != TYPE_SLICE && t->kind != TYPE_ANY_PTR) return t;
    Type *c = arena_alloc(a, sizeof(Type));
    *c = *t;
    c->is_const = true;
    return c;
}

bool is_str_type(Type *t) {
    return t && t->kind == TYPE_SLICE && t->slice.elem &&
           t->slice.elem->kind == TYPE_UINT8;
}

bool is_cstr_type(Type *t) {
    return t && t->kind == TYPE_POINTER && t->pointer.pointee &&
           t->pointer.pointee->kind == TYPE_UINT8;
}

bool type_is_error(Type *t) {
    return t && t->kind == TYPE_ERROR;
}

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

Type *type_fixed_array(Arena *a, Type *elem, int64_t size) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = TYPE_FIXED_ARRAY;
    t->fixed_array.elem = elem;
    t->fixed_array.size = size;
    return t;
}

bool type_is_integer(Type *t) {
    return t->kind >= TYPE_INT8 && t->kind <= TYPE_USIZE;
}

bool type_is_signed(Type *t) {
    return (t->kind >= TYPE_INT8 && t->kind <= TYPE_INT64) || t->kind == TYPE_ISIZE;
}

bool type_is_unsigned(Type *t) {
    return (t->kind >= TYPE_UINT8 && t->kind <= TYPE_UINT64) || t->kind == TYPE_USIZE;
}

bool type_is_float(Type *t) {
    return t->kind == TYPE_FLOAT32 || t->kind == TYPE_FLOAT64;
}

bool type_is_numeric(Type *t) {
    return type_is_integer(t) || type_is_float(t);
}

/* Extract the name from a type for stub comparison purposes */
static const char *type_udt_name(Type *t) {
    if (t->kind == TYPE_STUB) return t->stub.name;
    if (t->kind == TYPE_STRUCT) return t->struc.name;
    if (t->kind == TYPE_UNION) return t->unio.name;
    return NULL;
}

bool type_eq(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind == TYPE_ERROR || b->kind == TYPE_ERROR) return true;
    if (a->kind != b->kind) {
        /* A stub might need to match a struct or union type (or vice versa). */
        const char *na = type_udt_name(a);
        const char *nb = type_udt_name(b);
        if (na && nb) return na == nb;  /* interned string comparison */
        return false;
    }
    switch (a->kind) {
    case TYPE_POINTER: return a->is_const == b->is_const &&
                              type_eq(a->pointer.pointee, b->pointer.pointee);
    case TYPE_SLICE:   return a->is_const == b->is_const &&
                              type_eq(a->slice.elem, b->slice.elem);
    case TYPE_OPTION:  return type_eq(a->option.inner, b->option.inner);
    case TYPE_FIXED_ARRAY: return a->fixed_array.size == b->fixed_array.size &&
                                  type_eq(a->fixed_array.elem, b->fixed_array.elem);
    case TYPE_ANY_PTR: return a->is_const == b->is_const;
    case TYPE_STRUCT:  return a->struc.name == b->struc.name;
    case TYPE_UNION:   return a->unio.name == b->unio.name;
    case TYPE_STUB:    return a->stub.name == b->stub.name;
    case TYPE_TYPE_VAR: return a->type_var.name == b->type_var.name;
    case TYPE_FUNC:
        if (a->func.param_count != b->func.param_count) return false;
        if (a->func.is_variadic != b->func.is_variadic) return false;
        for (int i = 0; i < a->func.param_count; i++)
            if (!type_eq(a->func.param_types[i], b->func.param_types[i])) return false;
        return type_eq(a->func.return_type, b->func.return_type);
    default: return true;   /* primitives match by kind */
    }
}

bool type_eq_ignore_const(Type *a, Type *b) {
    if (a == b) return true;
    if (a->kind == TYPE_ERROR || b->kind == TYPE_ERROR) return true;
    if (a->kind != b->kind) {
        /* A stub might need to match a struct or union type (or vice versa). */
        const char *na = type_udt_name(a);
        const char *nb = type_udt_name(b);
        if (na && nb) return na == nb;
        return false;
    }
    switch (a->kind) {
    case TYPE_POINTER: return type_eq_ignore_const(a->pointer.pointee, b->pointer.pointee);
    case TYPE_SLICE:   return type_eq_ignore_const(a->slice.elem, b->slice.elem);
    case TYPE_OPTION:  return type_eq_ignore_const(a->option.inner, b->option.inner);
    case TYPE_FIXED_ARRAY: return a->fixed_array.size == b->fixed_array.size &&
                                  type_eq_ignore_const(a->fixed_array.elem, b->fixed_array.elem);
    case TYPE_STRUCT:  return a->struc.name == b->struc.name;
    case TYPE_UNION:   return a->unio.name == b->unio.name;
    case TYPE_STUB:    return a->stub.name == b->stub.name;
    case TYPE_TYPE_VAR: return a->type_var.name == b->type_var.name;
    case TYPE_FUNC:
        if (a->func.param_count != b->func.param_count) return false;
        if (a->func.is_variadic != b->func.is_variadic) return false;
        for (int i = 0; i < a->func.param_count; i++)
            if (!type_eq_ignore_const(a->func.param_types[i], b->func.param_types[i])) return false;
        return type_eq_ignore_const(a->func.return_type, b->func.return_type);
    default: return true;
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
    [TYPE_ISIZE]   = "isize",
    [TYPE_USIZE]   = "usize",
    [TYPE_FLOAT32] = "float32",
    [TYPE_FLOAT64] = "float64",
    [TYPE_BOOL]    = "bool",
    [TYPE_VOID]    = "void",
    [TYPE_CHAR]    = "char",
    [TYPE_ANY_PTR] = "any*",
    [TYPE_FIXED_ARRAY] = NULL,   /* compound — handled in type_name() */
    [TYPE_ERROR]   = "<error>",
};

const char *type_name(Type *t) {
    if (!t) return "<null-type>";
    /* Handle const types */
    if (t->is_const) {
        if (is_str_type(t)) return "const str";
        if (is_cstr_type(t)) return "const cstr";
        static char cbufs[4][256];
        static int cidx = 0;
        char *buf = cbufs[cidx & 3]; cidx++;
        if (t->kind == TYPE_POINTER)
            snprintf(buf, 256, "const %s*", type_name(t->pointer.pointee));
        else if (t->kind == TYPE_SLICE)
            snprintf(buf, 256, "const %s[]", type_name(t->slice.elem));
        else if (t->kind == TYPE_ANY_PTR)
            snprintf(buf, 256, "const any*");
        else
            snprintf(buf, 256, "const ?");
        return buf;
    }
    if (t->alias) return t->alias;
    if (t->kind < TYPE_POINTER) {
        return primitive_names[t->kind];
    }
    /* For compound types, check known aliases */
    if (is_str_type(t)) return "str";
    if (is_cstr_type(t)) return "cstr";
    /* For compound types, build a recursive name */
    switch (t->kind) {
    case TYPE_POINTER: {
        static char pbufs[4][256];
        static int pidx = 0;
        char *buf = pbufs[pidx & 3]; pidx++;
        snprintf(buf, 256, "%s*", type_name(t->pointer.pointee));
        return buf;
    }
    case TYPE_SLICE: {
        static char sbufs[4][256];
        static int sidx = 0;
        char *buf = sbufs[sidx & 3]; sidx++;
        snprintf(buf, 256, "%s[]", type_name(t->slice.elem));
        return buf;
    }
    case TYPE_OPTION: {
        static char obufs[4][256];
        static int oidx = 0;
        char *buf = obufs[oidx & 3]; oidx++;
        snprintf(buf, 256, "%s?", type_name(t->option.inner));
        return buf;
    }
    case TYPE_FIXED_ARRAY: {
        static char fabufs[4][256];
        static int faidx = 0;
        char *buf = fabufs[faidx & 3]; faidx++;
        snprintf(buf, 256, "%s[%lld]", type_name(t->fixed_array.elem),
                 (long long)t->fixed_array.size);
        return buf;
    }
    case TYPE_FUNC: {
        static char bufs[2][256];
        static int bidx = 0;
        char *buf = bufs[bidx & 1]; bidx++;
        int pos = 0;
        if (t->func.type_param_count > 0) {
            pos += snprintf(buf + pos, 256 - (size_t)pos, "<");
            for (int i = 0; i < t->func.type_param_count; i++) {
                if (i > 0) pos += snprintf(buf + pos, 256 - (size_t)pos, ", ");
                pos += snprintf(buf + pos, 256 - (size_t)pos, "%s", t->func.type_params[i]);
            }
            pos += snprintf(buf + pos, 256 - (size_t)pos, ">");
        }
        pos += snprintf(buf + pos, 256 - (size_t)pos, "(");
        for (int i = 0; i < t->func.param_count; i++) {
            if (i > 0) pos += snprintf(buf + pos, 256 - (size_t)pos, ", ");
            pos += snprintf(buf + pos, 256 - (size_t)pos, "%s", type_name(t->func.param_types[i]));
        }
        if (t->func.is_variadic) {
            if (t->func.param_count > 0) pos += snprintf(buf + pos, 256 - (size_t)pos, ", ");
            pos += snprintf(buf + pos, 256 - (size_t)pos, "...");
        }
        pos += snprintf(buf + pos, 256 - (size_t)pos, ") -> %s", type_name(t->func.return_type));
        return buf;
    }
    case TYPE_STRUCT: {
        if (t->struc.type_arg_count > 0 && t->struc.qualified_name) {
            static char stbufs[4][256];
            static int stidx = 0;
            char *buf = stbufs[stidx & 3]; stidx++;
            const char *display = t->struc.qualified_name;
            int pos = 0;
            pos += snprintf(buf + pos, 256 - (size_t)pos, "%s<", display);
            for (int i = 0; i < t->struc.type_arg_count; i++) {
                if (i > 0) pos += snprintf(buf + pos, 256 - (size_t)pos, ", ");
                pos += snprintf(buf + pos, 256 - (size_t)pos, "%s", type_name(t->struc.type_args[i]));
            }
            snprintf(buf + pos, 256 - (size_t)pos, ">");
            return buf;
        }
        if (t->struc.qualified_name) return t->struc.qualified_name;
        return t->struc.name;
    }
    case TYPE_UNION: {
        if (t->unio.type_arg_count > 0 && t->unio.qualified_name) {
            static char utbufs[4][256];
            static int utidx = 0;
            char *buf = utbufs[utidx & 3]; utidx++;
            const char *display = t->unio.qualified_name;
            int pos = 0;
            pos += snprintf(buf + pos, 256 - (size_t)pos, "%s<", display);
            for (int i = 0; i < t->unio.type_arg_count; i++) {
                if (i > 0) pos += snprintf(buf + pos, 256 - (size_t)pos, ", ");
                pos += snprintf(buf + pos, 256 - (size_t)pos, "%s", type_name(t->unio.type_args[i]));
            }
            snprintf(buf + pos, 256 - (size_t)pos, ">");
            return buf;
        }
        if (t->unio.qualified_name) return t->unio.qualified_name;
        return t->unio.name;
    }
    case TYPE_STUB: {
        if (t->stub.qualified_name) return t->stub.qualified_name;
        return t->stub.name;
    }
    case TYPE_ANY_PTR:   return "any*";
    case TYPE_TYPE_VAR:  return t->type_var.name;
    case TYPE_ERROR:     return "<error>";
    default:             return "?";
    }
}

bool type_can_widen(Type *from, Type *to) {
    if (from->kind == TYPE_ERROR || to->kind == TYPE_ERROR) return true;
    if (type_eq(from, to)) return true;

    /* non-const pointer/slice/any* → const pointer/slice/any* */
    if (to->is_const && !from->is_const) {
        if (from->kind == TYPE_POINTER && to->kind == TYPE_POINTER &&
            type_eq(from->pointer.pointee, to->pointer.pointee))
            return true;
        if (from->kind == TYPE_SLICE && to->kind == TYPE_SLICE &&
            type_eq(from->slice.elem, to->slice.elem))
            return true;
        if (from->kind == TYPE_ANY_PTR && to->kind == TYPE_ANY_PTR)
            return true;
    }

    /* T* → any* (like C's T* → void*), respecting const */
    if (to->kind == TYPE_ANY_PTR && from->kind == TYPE_POINTER) {
        /* const T* → const any* OK, T* → any* OK, const T* → any* NOT OK */
        if (from->is_const && !to->is_const) return false;
        return true;
    }

    /* option inner widening (e.g., int32*? → const int32*?) */
    if (from->kind == TYPE_OPTION && to->kind == TYPE_OPTION &&
        from->option.inner && to->option.inner)
        return type_can_widen(from->option.inner, to->option.inner);

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
    if (a->kind == TYPE_ERROR) return b;
    if (b->kind == TYPE_ERROR) return a;
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
        {"i",   1, type_isize},  {"u",   1, type_usize},
    };

    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (map[i].l == len && memcmp(suffix, map[i].s, (size_t)len) == 0) {
            return map[i].fn();
        }
    }
    return NULL;
}

Type *type_from_name(const char *s, int len) {
    struct { const char *n; int l; Type *(*fn)(void); } map[] = {
        {"int8",4, type_int8},     {"int16",5, type_int16},
        {"int32",5, type_int32},   {"int64",5, type_int64},
        {"uint8",5, type_uint8},   {"uint16",6, type_uint16},
        {"uint32",6, type_uint32}, {"uint64",6, type_uint64},
        {"float32",7, type_float32}, {"float64",7, type_float64},
        {"bool",4, type_bool},     {"char",4, type_char},
        {"str",3, type_str},       {"cstr",4, type_cstr},
        {"any",3, type_any_ptr},
        {"isize",5, type_isize},   {"usize",5, type_usize},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (map[i].l == len && memcmp(s, map[i].n, (size_t)len) == 0) {
            return map[i].fn ? map[i].fn() : NULL;
        }
    }
    return NULL;
}

Type *type_type_var(Arena *a, const char *name) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = TYPE_TYPE_VAR;
    t->type_var.name = name;
    return t;
}

bool type_needs_eq_func(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_STRUCT:
    case TYPE_UNION:
    case TYPE_SLICE:
    case TYPE_FUNC:
    case TYPE_FIXED_ARRAY:
        return true;
    case TYPE_OPTION:
        /* Pointer options use C native == (NULL for none) */
        return !(t->option.inner && t->option.inner->kind == TYPE_POINTER);
    case TYPE_STUB:
        return false;  /* unresolved stubs don't need eq functions */
    default:
        return false;
    }
}

bool type_contains_type_var(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_TYPE_VAR: return true;
    case TYPE_POINTER:  return type_contains_type_var(t->pointer.pointee);
    case TYPE_SLICE:    return type_contains_type_var(t->slice.elem);
    case TYPE_OPTION:   return type_contains_type_var(t->option.inner);
    case TYPE_FIXED_ARRAY: return type_contains_type_var(t->fixed_array.elem);
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            if (type_contains_type_var(t->func.param_types[i])) return true;
        return type_contains_type_var(t->func.return_type);
    case TYPE_STRUCT:
        for (int i = 0; i < t->struc.field_count; i++)
            if (type_contains_type_var(t->struc.fields[i].type)) return true;
        for (int i = 0; i < t->struc.type_arg_count; i++)
            if (type_contains_type_var(t->struc.type_args[i])) return true;
        return false;
    case TYPE_UNION:
        for (int i = 0; i < t->unio.variant_count; i++)
            if (type_contains_type_var(t->unio.variants[i].payload)) return true;
        for (int i = 0; i < t->unio.type_arg_count; i++)
            if (type_contains_type_var(t->unio.type_args[i])) return true;
        return false;
    case TYPE_STUB:
        for (int i = 0; i < t->stub.type_arg_count; i++)
            if (type_contains_type_var(t->stub.type_args[i])) return true;
        return false;
    default: return false;
    }
}

void type_collect_vars(Type *t, const char ***vars, int *count, int *cap) {
    if (!t) return;
    switch (t->kind) {
    case TYPE_TYPE_VAR:
        /* Add if not already present */
        for (int i = 0; i < *count; i++)
            if ((*vars)[i] == t->type_var.name) return;
        DA_APPEND(*vars, *count, *cap, t->type_var.name);
        return;
    case TYPE_POINTER: type_collect_vars(t->pointer.pointee, vars, count, cap); return;
    case TYPE_SLICE:   type_collect_vars(t->slice.elem, vars, count, cap); return;
    case TYPE_OPTION:  type_collect_vars(t->option.inner, vars, count, cap); return;
    case TYPE_FIXED_ARRAY: type_collect_vars(t->fixed_array.elem, vars, count, cap); return;
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            type_collect_vars(t->func.param_types[i], vars, count, cap);
        type_collect_vars(t->func.return_type, vars, count, cap);
        return;
    case TYPE_STRUCT:
        for (int i = 0; i < t->struc.field_count; i++)
            type_collect_vars(t->struc.fields[i].type, vars, count, cap);
        for (int i = 0; i < t->struc.type_arg_count; i++)
            type_collect_vars(t->struc.type_args[i], vars, count, cap);
        return;
    case TYPE_UNION:
        for (int i = 0; i < t->unio.variant_count; i++)
            type_collect_vars(t->unio.variants[i].payload, vars, count, cap);
        for (int i = 0; i < t->unio.type_arg_count; i++)
            type_collect_vars(t->unio.type_args[i], vars, count, cap);
        return;
    case TYPE_STUB:
        for (int i = 0; i < t->stub.type_arg_count; i++)
            type_collect_vars(t->stub.type_args[i], vars, count, cap);
        return;
    default: return;
    }
}

Type *type_substitute(Arena *a, Type *t, const char **var_names, Type **concrete, int count) {
    if (!t) return NULL;
    switch (t->kind) {
    case TYPE_TYPE_VAR:
        for (int i = 0; i < count; i++)
            if (var_names[i] == t->type_var.name)
                return concrete[i];
        return t; /* unbound type var — leave as is */
    case TYPE_POINTER: {
        Type *inner = type_substitute(a, t->pointer.pointee, var_names, concrete, count);
        if (inner == t->pointer.pointee) return t;
        Type *result = type_pointer(a, inner);
        result->is_const = t->is_const;
        return result;
    }
    case TYPE_SLICE: {
        Type *inner = type_substitute(a, t->slice.elem, var_names, concrete, count);
        if (inner == t->slice.elem) return t;
        Type *result = type_slice(a, inner);
        result->is_const = t->is_const;
        return result;
    }
    case TYPE_OPTION: {
        Type *inner = type_substitute(a, t->option.inner, var_names, concrete, count);
        if (inner == t->option.inner) return t;
        return type_option(a, inner);
    }
    case TYPE_FIXED_ARRAY: {
        Type *inner = type_substitute(a, t->fixed_array.elem, var_names, concrete, count);
        if (inner == t->fixed_array.elem) return t;
        return type_fixed_array(a, inner, t->fixed_array.size);
    }
    case TYPE_FUNC: {
        bool changed = false;
        Type **params = arena_alloc(a, sizeof(Type*) * (size_t)t->func.param_count);
        for (int i = 0; i < t->func.param_count; i++) {
            params[i] = type_substitute(a, t->func.param_types[i], var_names, concrete, count);
            if (params[i] != t->func.param_types[i]) changed = true;
        }
        Type *ret = type_substitute(a, t->func.return_type, var_names, concrete, count);
        if (ret != t->func.return_type) changed = true;
        if (!changed) return t;
        Type *nf = arena_alloc(a, sizeof(Type));
        nf->kind = TYPE_FUNC;
        nf->func.param_types = params;
        nf->func.param_count = t->func.param_count;
        nf->func.return_type = ret;
        nf->func.is_variadic = t->func.is_variadic;
        nf->func.type_params = t->func.type_params;
        nf->func.type_param_count = t->func.type_param_count;
        return nf;
    }
    case TYPE_STRUCT: {
        bool changed = false;
        StructField *fields = arena_alloc(a, sizeof(StructField) * (size_t)t->struc.field_count);
        for (int i = 0; i < t->struc.field_count; i++) {
            fields[i].name = t->struc.fields[i].name;
            fields[i].type = type_substitute(a, t->struc.fields[i].type, var_names, concrete, count);
            if (fields[i].type != t->struc.fields[i].type) changed = true;
        }
        /* Also substitute type_args if present */
        Type **new_targs = NULL;
        int new_targ_count = t->struc.type_arg_count;
        if (new_targ_count > 0) {
            new_targs = arena_alloc(a, sizeof(Type*) * (size_t)new_targ_count);
            for (int i = 0; i < new_targ_count; i++) {
                new_targs[i] = type_substitute(a, t->struc.type_args[i], var_names, concrete, count);
                if (new_targs[i] != t->struc.type_args[i]) changed = true;
            }
        }
        if (!changed) return t;
        Type *ns = arena_alloc(a, sizeof(Type));
        ns->kind = TYPE_STRUCT;
        ns->struc.name = t->struc.name;
        ns->struc.qualified_name = t->struc.qualified_name;
        ns->struc.c_name = t->struc.c_name;
        ns->struc.fields = fields;
        ns->struc.field_count = t->struc.field_count;
        ns->struc.type_args = new_targs;
        ns->struc.type_arg_count = new_targ_count;
        ns->struc.resolved_sym = t->struc.resolved_sym;
        return ns;
    }
    case TYPE_UNION: {
        bool changed = false;
        UnionVariant *vars = arena_alloc(a, sizeof(UnionVariant) * (size_t)t->unio.variant_count);
        for (int i = 0; i < t->unio.variant_count; i++) {
            vars[i].name = t->unio.variants[i].name;
            vars[i].payload = type_substitute(a, t->unio.variants[i].payload, var_names, concrete, count);
            if (vars[i].payload != t->unio.variants[i].payload) changed = true;
        }
        /* Also substitute type_args if present */
        Type **new_targs = NULL;
        int new_targ_count = t->unio.type_arg_count;
        if (new_targ_count > 0) {
            new_targs = arena_alloc(a, sizeof(Type*) * (size_t)new_targ_count);
            for (int i = 0; i < new_targ_count; i++) {
                new_targs[i] = type_substitute(a, t->unio.type_args[i], var_names, concrete, count);
                if (new_targs[i] != t->unio.type_args[i]) changed = true;
            }
        }
        if (!changed) return t;
        Type *nu = arena_alloc(a, sizeof(Type));
        nu->kind = TYPE_UNION;
        nu->unio.name = t->unio.name;
        nu->unio.qualified_name = t->unio.qualified_name;
        nu->unio.variants = vars;
        nu->unio.variant_count = t->unio.variant_count;
        nu->unio.type_args = new_targs;
        nu->unio.type_arg_count = new_targ_count;
        nu->unio.resolved_sym = t->unio.resolved_sym;
        return nu;
    }
    case TYPE_STUB: {
        bool changed = false;
        Type **new_targs = NULL;
        int new_targ_count = t->stub.type_arg_count;
        if (new_targ_count > 0) {
            new_targs = arena_alloc(a, sizeof(Type*) * (size_t)new_targ_count);
            for (int i = 0; i < new_targ_count; i++) {
                new_targs[i] = type_substitute(a, t->stub.type_args[i], var_names, concrete, count);
                if (new_targs[i] != t->stub.type_args[i]) changed = true;
            }
        }
        if (!changed) return t;
        Type *ns = arena_alloc(a, sizeof(Type));
        ns->kind = TYPE_STUB;
        ns->stub.name = t->stub.name;
        ns->stub.qualified_name = t->stub.qualified_name;
        ns->stub.type_args = new_targs;
        ns->stub.type_arg_count = new_targ_count;
        return ns;
    }
    default: return t;
    }
}

/* Returns a type name suitable for mangling. The result is always a malloc'd
 * string that the caller must free (even for primitive types). */
char *mangle_type_name(Type *t) {
    if (!t) return str_dup("void");
    /* Const types get a "const_" prefix */
    if (t->is_const) {
        Type tmp = *t;
        tmp.is_const = false;
        char *inner = mangle_type_name(&tmp);
        int needed = snprintf(NULL, 0, "const_%s", inner) + 1;
        char *buf = malloc((size_t)needed);
        snprintf(buf, (size_t)needed, "const_%s", inner);
        free(inner);
        return buf;
    }
    if (is_str_type(t)) return str_dup("str");
    if (is_cstr_type(t)) return str_dup("cstr");
    switch (t->kind) {
    case TYPE_INT8:    return str_dup("int8");
    case TYPE_INT16:   return str_dup("int16");
    case TYPE_INT32:   return str_dup("int32");
    case TYPE_INT64:   return str_dup("int64");
    case TYPE_UINT8:   return str_dup("uint8");
    case TYPE_UINT16:  return str_dup("uint16");
    case TYPE_UINT32:  return str_dup("uint32");
    case TYPE_UINT64:  return str_dup("uint64");
    case TYPE_ISIZE:   return str_dup("isize");
    case TYPE_USIZE:   return str_dup("usize");
    case TYPE_FLOAT32: return str_dup("f32");
    case TYPE_FLOAT64: return str_dup("f64");
    case TYPE_BOOL:    return str_dup("bool");
    case TYPE_VOID:    return str_dup("void");
    case TYPE_STRUCT:  return str_dup(t->struc.name);
    case TYPE_UNION:   return str_dup(t->unio.name);
    case TYPE_STUB:    return str_dup(t->stub.name);
    case TYPE_TYPE_VAR: return str_dup(t->type_var.name);
    case TYPE_FIXED_ARRAY: {
        char *inner = mangle_type_name(t->fixed_array.elem);
        int needed = snprintf(NULL, 0, "fixarr%lld_%s", (long long)t->fixed_array.size, inner) + 1;
        char *buf = malloc((size_t)needed);
        snprintf(buf, (size_t)needed, "fixarr%lld_%s", (long long)t->fixed_array.size, inner);
        free(inner);
        return buf;
    }
    default: {
        const char *prefix;
        char *inner;
        if (t->kind == TYPE_POINTER) {
            prefix = "ptr_"; inner = mangle_type_name(t->pointer.pointee);
        } else if (t->kind == TYPE_SLICE) {
            prefix = "slice_"; inner = mangle_type_name(t->slice.elem);
        } else if (t->kind == TYPE_OPTION) {
            prefix = "opt_"; inner = mangle_type_name(t->option.inner);
        } else {
            return str_dup("unknown");
        }
        int needed = snprintf(NULL, 0, "%s%s", prefix, inner) + 1;
        char *buf = malloc((size_t)needed);
        snprintf(buf, (size_t)needed, "%s%s", prefix, inner);
        free(inner);
        return buf;
    }
    }
}

const char *mangle_generic_name(Arena *a, InternTable *intern_tbl,
                                const char *base, Type **type_args, int count) {
    /* Measure total length needed */
    char **names = malloc(sizeof(char*) * (size_t)count);
    int needed = (int)strlen(base);
    for (int i = 0; i < count; i++) {
        names[i] = mangle_type_name(type_args[i]);
        needed += 1 + (int)strlen(names[i]); /* "_" + name */
    }
    char *buf = malloc((size_t)(needed + 1));
    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(needed + 1 - pos), "%s", base);
    for (int i = 0; i < count; i++) {
        pos += snprintf(buf + pos, (size_t)(needed + 1 - pos), "_%s", names[i]);
        free(names[i]);
    }
    free(names);
    (void)a;
    const char *result = intern_cstr(intern_tbl, buf);
    free(buf);
    return result;
}
