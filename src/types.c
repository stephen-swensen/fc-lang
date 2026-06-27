#include "types.h"
#include <stdio.h>
#include <stdarg.h>

/* Accumulating snprintf into a fixed buffer, robust to overflow: clamps *pos to
 * [0, cap] so the remaining-size argument can never underflow to a huge size_t
 * when a deeply nested type name exceeds the buffer (the name just truncates).
 * The naive `snprintf(buf + pos, cap - (size_t)pos, ...)` idiom overflows the
 * buffer once pos > cap — reachable for e.g. wrap<wrap<...>> at depth ~50+. */
static void tn_appendf(char *buf, int *pos, int cap, const char *fmt, ...) {
    if (*pos < 0) *pos = 0;
    if (*pos >= cap) { *pos = cap; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, (size_t)(cap - *pos), fmt, ap);
    va_end(ap);
    if (n > 0) *pos += n;
    if (*pos > cap) *pos = cap;
}

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
PRIM(never,   TYPE_NEVER)
PRIM(unresolved, TYPE_UNRESOLVED)

#undef PRIM

/* str = u8[] with alias "str" */
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

/* cstr = u8* with alias "cstr" */
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

/* Recursively copy a type tree, allocating fresh nodes for every constructor
 * (struct/union/pointer/slice/option/array/function/stub) and a fresh
 * fields/variants/params array. Leaves that are never rewritten by
 * mono_resolve_type_names — primitives (singletons), type vars, any* — are
 * shared, as are stub/struct/union *type_args* (read-only when computing a
 * mangled name). This is the isolation needed so that the in-place name
 * canonicalization done by mono_resolve_type_names / discover_nested_types on a
 * monomorphized instance's concrete_type can never mutate a subtree that is
 * still shared with a pass2-live expression type or a generic template.
 *
 * Termination: a type referencing another (or the same) generic struct in a
 * field appears as a TYPE_STUB or a pointer-to-stub (a name + concrete
 * type_args, which are not recursed here), never as the full embedded
 * definition — by-value self-embedding is rejected as infinite-size — so the
 * recursion is finite. */
Type *type_deep_copy(Arena *a, Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case TYPE_STRUCT: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        int n = t->struc.field_count;
        c->struc.fields = arena_alloc(a, sizeof(StructField) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) {
            c->struc.fields[i].name = t->struc.fields[i].name;
            c->struc.fields[i].type = type_deep_copy(a, t->struc.fields[i].type);
        }
        return c;
    }
    case TYPE_UNION: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        int n = t->unio.variant_count;
        c->unio.variants = arena_alloc(a, sizeof(UnionVariant) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) {
            c->unio.variants[i].name = t->unio.variants[i].name;
            c->unio.variants[i].payload = type_deep_copy(a, t->unio.variants[i].payload);
        }
        return c;
    }
    case TYPE_POINTER: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        c->pointer.pointee = type_deep_copy(a, t->pointer.pointee);
        return c;
    }
    case TYPE_SLICE: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        c->slice.elem = type_deep_copy(a, t->slice.elem);
        return c;
    }
    case TYPE_OPTION: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        c->option.inner = type_deep_copy(a, t->option.inner);
        return c;
    }
    case TYPE_FIXED_ARRAY: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        c->fixed_array.elem = type_deep_copy(a, t->fixed_array.elem);
        return c;
    }
    case TYPE_FUNC: {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        int n = t->func.param_count;
        c->func.param_types = arena_alloc(a, sizeof(Type*) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++)
            c->func.param_types[i] = type_deep_copy(a, t->func.param_types[i]);
        c->func.return_type = type_deep_copy(a, t->func.return_type);
        return c;
    }
    case TYPE_STUB: {
        /* Fresh node so mangling its name can't corrupt a shared stub; type_args
         * are read-only for name computation, so they stay shared. */
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *t;
        return c;
    }
    default:
        /* Primitive singletons, type vars, any*, error, never — never renamed. */
        return t;
    }
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

bool type_is_never(Type *t) {
    return t && t->kind == TYPE_NEVER;
}

bool type_is_unresolved(Type *t) {
    return t && t->kind == TYPE_UNRESOLVED;
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
    case TYPE_STRUCT:
        /* Tuples compare structurally (arity + element types), independent of when
         * their canonical name gets interned. Named structs compare by interned name. */
        if (a->struc.is_tuple || b->struc.is_tuple) {
            if (!a->struc.is_tuple || !b->struc.is_tuple) return false;
            if (a->struc.field_count != b->struc.field_count) return false;
            for (int i = 0; i < a->struc.field_count; i++)
                if (!type_eq(a->struc.fields[i].type, b->struc.fields[i].type)) return false;
            return true;
        }
        return a->struc.name == b->struc.name;
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
    case TYPE_STRUCT:
        if (a->struc.is_tuple || b->struc.is_tuple) {
            if (!a->struc.is_tuple || !b->struc.is_tuple) return false;
            if (a->struc.field_count != b->struc.field_count) return false;
            for (int i = 0; i < a->struc.field_count; i++)
                if (!type_eq_ignore_const(a->struc.fields[i].type, b->struc.fields[i].type)) return false;
            return true;
        }
        return a->struc.name == b->struc.name;
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
    [TYPE_INT8]    = "i8",
    [TYPE_INT16]   = "i16",
    [TYPE_INT32]   = "i32",
    [TYPE_INT64]   = "i64",
    [TYPE_UINT8]   = "u8",
    [TYPE_UINT16]  = "u16",
    [TYPE_UINT32]  = "u32",
    [TYPE_UINT64]  = "u64",
    [TYPE_ISIZE]   = "isize",
    [TYPE_USIZE]   = "usize",
    [TYPE_FLOAT32] = "f32",
    [TYPE_FLOAT64] = "f64",
    [TYPE_BOOL]    = "bool",
    [TYPE_VOID]    = "void",
    [TYPE_CHAR]    = "char",
    [TYPE_ANY_PTR] = "any*",
    [TYPE_FIXED_ARRAY] = NULL,   /* compound — handled in type_name() */
    [TYPE_ERROR]   = "<error>",
    [TYPE_NEVER]   = "never",
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
            tn_appendf(buf, &pos, 256, "<");
            for (int i = 0; i < t->func.type_param_count; i++) {
                if (i > 0) tn_appendf(buf, &pos, 256, ", ");
                tn_appendf(buf, &pos, 256, "%s", t->func.type_params[i]);
            }
            tn_appendf(buf, &pos, 256, ">");
        }
        tn_appendf(buf, &pos, 256, "(");
        for (int i = 0; i < t->func.param_count; i++) {
            if (i > 0) tn_appendf(buf, &pos, 256, ", ");
            tn_appendf(buf, &pos, 256, "%s", type_name(t->func.param_types[i]));
        }
        if (t->func.is_variadic) {
            if (t->func.param_count > 0) tn_appendf(buf, &pos, 256, ", ");
            tn_appendf(buf, &pos, 256, "...");
        }
        tn_appendf(buf, &pos, 256, ") -> %s", type_name(t->func.return_type));
        return buf;
    }
    case TYPE_STRUCT: {
        if (t->struc.is_tuple) {
            static char ttbufs[4][512];
            static int ttidx = 0;
            char *buf = ttbufs[ttidx & 3]; ttidx++;
            int pos = 0;
            tn_appendf(buf, &pos, 512, "{");
            for (int i = 0; i < t->struc.field_count; i++) {
                if (i > 0) tn_appendf(buf, &pos, 512, ", ");
                tn_appendf(buf, &pos, 512, "%s", type_name(t->struc.fields[i].type));
            }
            tn_appendf(buf, &pos, 512, "}");
            return buf;
        }
        if (t->struc.type_arg_count > 0 && t->struc.qualified_name) {
            static char stbufs[4][256];
            static int stidx = 0;
            char *buf = stbufs[stidx & 3]; stidx++;
            const char *display = t->struc.qualified_name;
            int pos = 0;
            tn_appendf(buf, &pos, 256, "%s<", display);
            for (int i = 0; i < t->struc.type_arg_count; i++) {
                if (i > 0) tn_appendf(buf, &pos, 256, ", ");
                tn_appendf(buf, &pos, 256, "%s", type_name(t->struc.type_args[i]));
            }
            tn_appendf(buf, &pos, 256, ">");
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
            tn_appendf(buf, &pos, 256, "%s<", display);
            for (int i = 0; i < t->unio.type_arg_count; i++) {
                if (i > 0) tn_appendf(buf, &pos, 256, ", ");
                tn_appendf(buf, &pos, 256, "%s", type_name(t->unio.type_args[i]));
            }
            tn_appendf(buf, &pos, 256, ">");
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
    case TYPE_NEVER:     return "never";
    case TYPE_UNRESOLVED: return "<unresolved>";
    default:             return "?";
    }
}

/* A representation-preserving widen changes no bits: it only adds `const`, or
 * widens a typed pointer to `any*` (a plain pointer cast). It never changes a
 * value's size. These are the pointer/slice/`any*` cases of widening; the
 * numeric/float widenings below are NOT representation-preserving (they change a
 * scalar's width). This is also the ONLY widening permitted on an option's inner
 * type: a struct option is a distinct `fc_option_<T>` per representation, so a
 * size-changing inner widen (e.g. `int32? → int64?`) would emit an invalid
 * struct-to-struct C cast — whereas a null-sentinel option (`T*?`, `any*?`,
 * `cstr?`) is a bare pointer for which const-add / `T* → any*` is a valid pointer
 * cast, and a slice/`str` option's const-add is a no-op (const is display-only in
 * the option's C type). */
static bool widen_repr_preserving(Type *from, Type *to) {
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

    return false;
}

bool type_can_widen(Type *from, Type *to) {
    if (from->kind == TYPE_ERROR || to->kind == TYPE_ERROR) return true;
    if (type_eq(from, to)) return true;

    /* Representation-preserving widens: const-add and T* → any*. */
    if (widen_repr_preserving(from, to)) return true;

    /* Option inner widening (e.g. int32*? → const int32*?, int32*? → any*?).
     * Restricted to representation-preserving inner widens only: a size-changing
     * numeric inner widen (int32? → int64?) would require an invalid
     * struct-to-struct cast between distinct fc_option_<T> types, so it is
     * rejected here — consistently with if/match joins, which require an exact
     * type match. */
    if (from->kind == TYPE_OPTION && to->kind == TYPE_OPTION &&
        from->option.inner && to->option.inner)
        return widen_repr_preserving(from->option.inner, to->option.inner);

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
        {"isize", 5, type_isize}, {"usize", 5, type_usize},
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
        {"i8",2, type_int8},     {"i16",3, type_int16},
        {"i32",3, type_int32},   {"i64",3, type_int64},
        {"u8",2, type_uint8},    {"u16",3, type_uint16},
        {"u32",3, type_uint32},  {"u64",3, type_uint64},
        {"f32",3, type_float32}, {"f64",3, type_float64},
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

/* A fully-substituted generic struct/union instance shares ONE subtree between a
 * field type and the matching type argument — type_substitute returns the same
 * node for both. So a chain like wrap<wrap<...<int32>>> is a linear-size DAG, but
 * a naive walk that descends through BOTH a struct's fields and its type_args
 * visits each shared node twice per level => O(2^depth). A divergent generic
 * (e.g. f(wrap{v=x}) instantiating f<wrap<'a>> -> f<wrap<wrap<'a>>> -> ...) builds
 * such chains and hung the compiler here. A visited set of struct/union/stub nodes
 * already proven type-var-FREE collapses the walk back to O(nodes): a `true`
 * result short-circuits all the way out, so only the clean (false) joins need
 * memoizing. The set holds only the branching nodes (struct/union/stub); single-
 * child constructors (pointer/option/slice/array) can't compound a revisit. */
typedef struct {
    Type **items;
    int count, cap;
    Type *inline_buf[64];   /* avoids any heap allocation for ordinary types */
} TypeVarCleanSet;

static bool clean_set_has(TypeVarCleanSet *s, Type *t) {
    for (int i = 0; i < s->count; i++)
        if (s->items[i] == t) return true;
    return false;
}

static void clean_set_add(TypeVarCleanSet *s, Type *t) {
    if (s->count == s->cap) {
        int ncap = s->cap * 2;
        Type **ni;
        if (s->items == s->inline_buf) {
            ni = malloc(sizeof(Type*) * (size_t)ncap);
            memcpy(ni, s->items, sizeof(Type*) * (size_t)s->count);
        } else {
            ni = realloc(s->items, sizeof(Type*) * (size_t)ncap);
        }
        s->items = ni;
        s->cap = ncap;
    }
    s->items[s->count++] = t;
}

static bool type_contains_type_var_memo(Type *t, TypeVarCleanSet *clean) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_TYPE_VAR: return true;
    case TYPE_POINTER:  return type_contains_type_var_memo(t->pointer.pointee, clean);
    case TYPE_SLICE:    return type_contains_type_var_memo(t->slice.elem, clean);
    case TYPE_OPTION:   return type_contains_type_var_memo(t->option.inner, clean);
    case TYPE_FIXED_ARRAY: return type_contains_type_var_memo(t->fixed_array.elem, clean);
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            if (type_contains_type_var_memo(t->func.param_types[i], clean)) return true;
        return type_contains_type_var_memo(t->func.return_type, clean);
    case TYPE_STRUCT:
        if (clean_set_has(clean, t)) return false;
        for (int i = 0; i < t->struc.field_count; i++)
            if (type_contains_type_var_memo(t->struc.fields[i].type, clean)) return true;
        for (int i = 0; i < t->struc.type_arg_count; i++)
            if (type_contains_type_var_memo(t->struc.type_args[i], clean)) return true;
        clean_set_add(clean, t);
        return false;
    case TYPE_UNION:
        if (clean_set_has(clean, t)) return false;
        for (int i = 0; i < t->unio.variant_count; i++)
            if (type_contains_type_var_memo(t->unio.variants[i].payload, clean)) return true;
        for (int i = 0; i < t->unio.type_arg_count; i++)
            if (type_contains_type_var_memo(t->unio.type_args[i], clean)) return true;
        clean_set_add(clean, t);
        return false;
    case TYPE_STUB:
        if (clean_set_has(clean, t)) return false;
        for (int i = 0; i < t->stub.type_arg_count; i++)
            if (type_contains_type_var_memo(t->stub.type_args[i], clean)) return true;
        clean_set_add(clean, t);
        return false;
    default: return false;
    }
}

bool type_contains_type_var(Type *t) {
    TypeVarCleanSet clean;
    clean.items = clean.inline_buf;
    clean.count = 0;
    clean.cap = (int)(sizeof(clean.inline_buf) / sizeof(clean.inline_buf[0]));
    bool r = type_contains_type_var_memo(t, &clean);
    if (clean.items != clean.inline_buf) free(clean.items);
    return r;
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
        ns->struc.is_c_union = t->struc.is_c_union;
        ns->struc.is_tuple = t->struc.is_tuple;
        ns->struc.fields = fields;
        ns->struc.field_count = t->struc.field_count;
        ns->struc.type_args = new_targs;
        ns->struc.type_arg_count = new_targ_count;
        ns->struc.resolved_sym = t->struc.resolved_sym;
        /* For a substituted tuple, the canonical name is re-derived later by
         * mono_resolve_type_names / register_concrete_tuple (which hold the
         * intern table). type_eq compares tuples structurally in the meantime. */
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

/* Concatenate b onto a (a is realloc'd in place, b is borrowed). */
static char *mangle_cat(char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = realloc(a, la + lb + 1);
    memcpy(r + la, b, lb + 1);
    return r;
}

/* Append a length-prefixed, self-delimiting encoding of `piece` ("5_int32")
 * onto `acc`. `piece` is consumed (freed). The leading decimal length keeps a
 * join of components injective even when components themselves contain '_'
 * separators: the boundary between components is always implied by the length,
 * never inferred from a '_'. */
static char *mangle_append_piece(char *acc, char *piece) {
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%zu_", strlen(piece));
    acc = mangle_cat(acc, hdr);
    acc = mangle_cat(acc, piece);
    free(piece);
    return acc;
}

/* Returns a type name suitable for mangling. The result is always a malloc'd
 * string that the caller must free (even for primitive types).
 *
 * The mangling is INJECTIVE: distinct types always produce distinct strings,
 * so two unrelated generic instantiations can never dedupe to one C symbol.
 * Two devices guarantee this:
 *   - Every structural type constructor (pointer/slice/option/const/array/
 *     function/any*) is tagged with a leading "__" sequence. A user-written
 *     identifier can never contain "__" (the lexer reserves it), and a
 *     module-mangled name only ever has "__" followed by a name segment, which
 *     starts with a letter or '_' — never a digit. So a structural tag can be
 *     neither forged by nor confused with a leaf (struct/union/primitive) name:
 *     e.g. a pointer `int32*` mangles to "__pint32", which a user struct named
 *     `ptr_int32` (mangling to "ptr_int32") can never collide with.
 *   - Multi-component manglings (function params, plus the generic/tuple joins
 *     below) length-prefix each component, so a '_' inside one component can
 *     never be mistaken for a component boundary. */
char *mangle_type_name(Type *t) {
    if (!t) return str_dup("void");
    /* Const types get a "__c" structural tag. Only pointer/slice/any* carry a
     * const qualifier, so the inner mangling always begins with its own "__"
     * tag (or "str"/"cstr"), keeping the result unambiguous. */
    if (t->is_const) {
        Type tmp = *t;
        tmp.is_const = false;
        char *inner = mangle_type_name(&tmp);
        char *r = mangle_cat(str_dup("__c"), inner);
        free(inner);
        return r;
    }
    if (is_str_type(t)) return str_dup("str");
    if (is_cstr_type(t)) return str_dup("cstr");
    switch (t->kind) {
    case TYPE_INT8:    return str_dup("i8");
    case TYPE_INT16:   return str_dup("i16");
    case TYPE_INT32:   return str_dup("i32");
    case TYPE_INT64:   return str_dup("i64");
    case TYPE_UINT8:   return str_dup("u8");
    case TYPE_UINT16:  return str_dup("u16");
    case TYPE_UINT32:  return str_dup("u32");
    case TYPE_UINT64:  return str_dup("u64");
    case TYPE_ISIZE:   return str_dup("isize");
    case TYPE_USIZE:   return str_dup("usize");
    case TYPE_FLOAT32: return str_dup("f32");
    case TYPE_FLOAT64: return str_dup("f64");
    case TYPE_BOOL:    return str_dup("bool");
    case TYPE_CHAR:    return str_dup("char");
    case TYPE_VOID:    return str_dup("void");
    case TYPE_NEVER:   return str_dup("never"); /* defensive: never monomorphized */
    case TYPE_STRUCT:  return str_dup(t->struc.name);
    case TYPE_UNION:   return str_dup(t->unio.name);
    case TYPE_STUB:
        /* A generic-instance stub (box<i32>) must mangle identically to its
         * monomorphized struct (box__3_i32): base "__" lp(arg)*, mirroring
         * mangle_generic_name. Without recursing into the args, a stub nested in
         * another instance's type args (box<box<i32>>, where the inner arg is
         * still an unresolved stub) would drop its args to the bare base name —
         * making distinct instantiations collide and mis-resolving the C type
         * name. Concrete top-level decl field types keep their args as base-name
         * stubs (never rewritten in place), so this never double-mangles. */
        if (t->stub.type_arg_count > 0) {
            char *r = mangle_cat(str_dup(t->stub.name), "__");
            for (int i = 0; i < t->stub.type_arg_count; i++)
                r = mangle_append_piece(r, mangle_type_name(t->stub.type_args[i]));
            return r;
        }
        return str_dup(t->stub.name);
    case TYPE_TYPE_VAR: return str_dup(t->type_var.name);
    case TYPE_ANY_PTR: return str_dup("__y");
    case TYPE_ERROR:   return str_dup("__err"); /* defensive: never monomorphized */
    case TYPE_POINTER: {
        char *inner = mangle_type_name(t->pointer.pointee);
        char *r = mangle_cat(str_dup("__p"), inner);
        free(inner);
        return r;
    }
    case TYPE_SLICE: {
        char *inner = mangle_type_name(t->slice.elem);
        char *r = mangle_cat(str_dup("__l"), inner);
        free(inner);
        return r;
    }
    case TYPE_OPTION: {
        char *inner = mangle_type_name(t->option.inner);
        char *r = mangle_cat(str_dup("__o"), inner);
        free(inner);
        return r;
    }
    case TYPE_FIXED_ARRAY: {
        char *inner = mangle_type_name(t->fixed_array.elem);
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "__a%lld_", (long long)t->fixed_array.size);
        char *r = mangle_cat(str_dup(hdr), inner);
        free(inner);
        return r;
    }
    case TYPE_FUNC: {
        /* "__f" <nparams> "_" lp(param)* lp(ret) [ "_v" if variadic ]. The
         * param count tells the join where the params end and the return type
         * begins; each param/return is length-prefixed (mangle_append_piece). */
        char hdr[24];
        snprintf(hdr, sizeof(hdr), "__f%d_", t->func.param_count);
        char *r = str_dup(hdr);
        for (int i = 0; i < t->func.param_count; i++)
            r = mangle_append_piece(r, mangle_type_name(t->func.param_types[i]));
        r = mangle_append_piece(r, mangle_type_name(t->func.return_type));
        if (t->func.is_variadic) r = mangle_cat(r, "_v");
        return r;
    }
    default: return str_dup("__unk"); /* defensive: TYPE_COUNT etc. */
    }
}

const char *mangle_generic_name(Arena *a, InternTable *intern_tbl,
                                const char *base, Type **type_args, int count) {
    /* base "__" lp(arg)*  — injective. The "__" boundary is unambiguous: every
     * arg piece begins with its decimal length (a digit), whereas any "__"
     * occurring *inside* a module-mangled base is followed by a name segment
     * (a letter or '_', never a digit). Length-prefixing each arg then keeps the
     * join itself injective. So neither `pair<foo_bar,baz>` vs `pair<foo,bar_baz>`
     * nor a base whose own name embeds "__<arg-like>" can collide. */
    char *buf = str_dup(base);
    if (count > 0) {
        buf = mangle_cat(buf, "__");
        for (int i = 0; i < count; i++)
            buf = mangle_append_piece(buf, mangle_type_name(type_args[i]));
    }
    (void)a;
    const char *result = intern_cstr(intern_tbl, buf);
    free(buf);
    return result;
}

const char *tuple_canonical_name(Arena *a, InternTable *intern_tbl,
                                 StructField *fields, int n) {
    (void)a;
    /* "fc_tupleN" "__" lp(field-type)* — same injective join as generics. */
    char base[24];
    snprintf(base, sizeof(base), "fc_tuple%d", n);
    char *buf = str_dup(base);
    if (n > 0) {
        buf = mangle_cat(buf, "__");
        for (int i = 0; i < n; i++)
            buf = mangle_append_piece(buf, mangle_type_name(fields[i].type));
    }
    const char *result = intern_cstr(intern_tbl, buf);
    free(buf);
    return result;
}

Type *type_tuple(Arena *a, Type **elems, int n) {
    Type *t = arena_alloc(a, sizeof(Type));
    t->kind = TYPE_STRUCT;
    t->struc.is_tuple = true;
    t->struc.fields = arena_alloc(a, sizeof(StructField) * (size_t)(n > 0 ? n : 1));
    t->struc.field_count = n;
    for (int i = 0; i < n; i++) {
        char nm[16];
        int len = snprintf(nm, sizeof(nm), "e%d", i);
        char *fn = arena_alloc(a, (size_t)len + 1);
        memcpy(fn, nm, (size_t)len + 1);
        t->struc.fields[i].name = fn;
        t->struc.fields[i].type = elems[i];
    }
    return t;
}
