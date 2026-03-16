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
PRIM(str32,   TYPE_STR32)
PRIM(cstr,    TYPE_CSTR)
/* char is an alias for uint8 per spec */
Type *type_char(void) { return type_uint8(); }
PRIM(any_ptr, TYPE_ANY_PTR)
PRIM(error,   TYPE_ERROR)

#undef PRIM

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
    if (a->kind == TYPE_ERROR || b->kind == TYPE_ERROR) return true;
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
    case TYPE_TYPE_VAR: return a->type_var.name == b->type_var.name;
    case TYPE_FUNC:
        if (a->func.param_count != b->func.param_count) return false;
        for (int i = 0; i < a->func.param_count; i++)
            if (!type_eq(a->func.param_types[i], b->func.param_types[i])) return false;
        return type_eq(a->func.return_type, b->func.return_type);
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
    [TYPE_ERROR]   = "<error>",
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
    case TYPE_FUNC: {
        static char bufs[2][256];
        static int bidx = 0;
        char *buf = bufs[bidx & 1]; bidx++;
        int pos = 0;
        pos += snprintf(buf + pos, 256 - (size_t)pos, "(");
        for (int i = 0; i < t->func.param_count; i++) {
            if (i > 0) pos += snprintf(buf + pos, 256 - (size_t)pos, ", ");
            pos += snprintf(buf + pos, 256 - (size_t)pos, "%s", type_name(t->func.param_types[i]));
        }
        pos += snprintf(buf + pos, 256 - (size_t)pos, ") -> %s", type_name(t->func.return_type));
        return buf;
    }
    case TYPE_STRUCT:    return t->struc.name;
    case TYPE_UNION:     return t->unio.name;
    case TYPE_TYPE_VAR:  return t->type_var.name;
    case TYPE_ERROR:     return "<error>";
    default:             return "?";
    }
}

bool type_can_widen(Type *from, Type *to) {
    if (from->kind == TYPE_ERROR || to->kind == TYPE_ERROR) return true;
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
    };

    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (map[i].l == len && memcmp(suffix, map[i].s, (size_t)len) == 0) {
            return map[i].fn();
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
    case TYPE_STR:
    case TYPE_STR32:
    case TYPE_FUNC:
        return true;
    case TYPE_OPTION:
        /* Pointer options use C native == (NULL for none) */
        return !(t->option.inner && t->option.inner->kind == TYPE_POINTER);
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
        return type_pointer(a, inner);
    }
    case TYPE_SLICE: {
        Type *inner = type_substitute(a, t->slice.elem, var_names, concrete, count);
        if (inner == t->slice.elem) return t;
        return type_slice(a, inner);
    }
    case TYPE_OPTION: {
        Type *inner = type_substitute(a, t->option.inner, var_names, concrete, count);
        if (inner == t->option.inner) return t;
        return type_option(a, inner);
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
        ns->struc.fields = fields;
        ns->struc.field_count = t->struc.field_count;
        ns->struc.type_args = new_targs;
        ns->struc.type_arg_count = new_targ_count;
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
        nu->unio.variants = vars;
        nu->unio.variant_count = t->unio.variant_count;
        nu->unio.type_args = new_targs;
        nu->unio.type_arg_count = new_targ_count;
        return nu;
    }
    default: return t;
    }
}

const char *mangle_type_name(Type *t) {
    if (!t) return "void";
    switch (t->kind) {
    case TYPE_INT8:    return "int8";
    case TYPE_INT16:   return "int16";
    case TYPE_INT32:   return "int32";
    case TYPE_INT64:   return "int64";
    case TYPE_UINT8:   return "uint8";
    case TYPE_UINT16:  return "uint16";
    case TYPE_UINT32:  return "uint32";
    case TYPE_UINT64:  return "uint64";
    case TYPE_FLOAT32: return "f32";
    case TYPE_FLOAT64: return "f64";
    case TYPE_BOOL:    return "bool";
    case TYPE_VOID:    return "void";
    case TYPE_STR:     return "str";
    case TYPE_CSTR:    return "cstr";
    case TYPE_STRUCT:  return t->struc.name;
    case TYPE_UNION:   return t->unio.name;
    case TYPE_TYPE_VAR: return t->type_var.name;
    default: {
        /* Rotating buffers to avoid corruption during recursive calls */
        static char bufs[4][256];
        static int bidx = 0;
        char *buf = bufs[bidx & 3]; bidx++;
        if (t->kind == TYPE_POINTER) {
            snprintf(buf, 256, "ptr_%s", mangle_type_name(t->pointer.pointee));
        } else if (t->kind == TYPE_SLICE) {
            snprintf(buf, 256, "slice_%s", mangle_type_name(t->slice.elem));
        } else if (t->kind == TYPE_OPTION) {
            snprintf(buf, 256, "opt_%s", mangle_type_name(t->option.inner));
        } else {
            snprintf(buf, 256, "unknown");
        }
        return buf;
    }
    }
}

const char *mangle_generic_name(Arena *a, InternTable *intern_tbl,
                                const char *base, Type **type_args, int count) {
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", base);
    for (int i = 0; i < count; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "_%s",
                        mangle_type_name(type_args[i]));
    }
    (void)a;
    return intern_cstr(intern_tbl, buf);
}
