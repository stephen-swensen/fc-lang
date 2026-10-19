/* x86gen.c: the native x86-64 backend.
 *
 * An alternative to the C emitter for fast development builds. It walks the
 * same typed, monomorphized AST that codegen.c walks and writes GNU assembler
 * text (AT&T syntax) for x86-64 System V, position-independent, so the result
 * links against the C library with a plain `cc out.s`. It does not optimize;
 * its job is to mean exactly what the emitted C means, quickly.
 *
 * Value model. Every expression leaves its value in a fixed place:
 *   - an integer, bool, enum, pointer or null-sentinel option: %rax, sign- or
 *     zero-extended to 64 bits by its type (the canonical form, kept by every
 *     operation, so widening is free and compares run at 64 bits);
 *   - f32/f64: %xmm0;
 *   - any aggregate (struct, union, tuple, slice, option, result, function
 *     value): %rax holds the address of the value. The storage is either the
 *     expression's own lvalue or a temporary in the frame.
 * Values in memory are stored at their natural width; loads extend.
 *
 * Frame. %rbp-based. Every local binding, parameter and temporary has a slot
 * at a fixed negative offset. Temporaries are never reused within a function
 * (monotonic allocation), except the 8-byte spill slots that hold a scalar
 * operand while its sibling is evaluated, which nest strictly and so are
 * reused by depth. Nothing is pushed: %rsp stays 16-byte aligned throughout a
 * body, which keeps calls and `alloca` simple.
 *
 * Calls use the System V ABI everywhere, FC to FC included, so FC functions
 * are callable from C and the debugger. A call in tail position is a jump
 * when nothing in the current frame can be referenced by the callee. */

/* mkdtemp() under -std=c11. Must precede every #include. */
#define _DEFAULT_SOURCE

#include "x86gen.h"
#include "facts.h"
#include "diag.h"
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

/* ================================================================
 * Output buffers
 * ================================================================ */

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_grow(Buf *b, size_t need) {
    if (b->len + need + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 65536;
    while (nc < b->len + need + 1) nc *= 2;
    b->p = xrealloc(b->p, nc);
    b->cap = nc;
}

static void buf_put(Buf *b, const char *s, size_t n) {
    buf_grow(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }

static void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    buf_grow(b, 128);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    if (n >= 0 && (size_t)n >= b->cap - b->len) {
        buf_grow(b, (size_t)n);
        vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
    }
    if (n > 0) b->len += (size_t)n;
    va_end(ap2);
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static Buf *g_cur;          /* where instructions go */
static Buf g_fnbody;        /* the body of the function being emitted */
static Buf g_cold;          /* its out-of-line failure paths */
static Buf g_text;          /* finished functions */
static Buf g_rodata;        /* strings and read-only constants */
static Buf g_relro;         /* constants holding addresses */
static Buf g_data;          /* writable initialized globals */
static Buf g_bss;           /* zero-initialized globals */

/* One instruction, tab-indented. */
static void ins(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_put(g_cur, "\t", 1);
    buf_vprintf(g_cur, fmt, ap);
    buf_put(g_cur, "\n", 1);
    va_end(ap);
}

static int g_label_counter = 0;
static int new_label(void) { return ++g_label_counter; }
static void label(int l) { buf_printf(g_cur, ".Lq%d:\n", l); }

/* `off(%rbp)` from a small ring, so several fit in one ins() call. */
static const char *fp(int32_t off) {
    static char ring[8][24];
    static int k = 0;
    char *s = ring[k++ & 7];
    snprintf(s, 24, "%d(%%rbp)", (int)off);
    return s;
}

/* ================================================================
 * String-keyed hash map
 * ================================================================ */

typedef struct { const char *key; size_t klen; int64_t val; void *ptr; bool used; } HEnt;
typedef struct { HEnt *e; size_t cap, n; } HMap;

static uint64_t hash_bytes(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
    return h;
}

static HEnt *hmap_slot(HMap *m, const char *k, size_t n) {
    uint64_t h = hash_bytes(k, n);
    size_t i = (size_t)h & (m->cap - 1);
    for (;;) {
        HEnt *e = &m->e[i];
        if (!e->used) return e;
        if (e->klen == n && memcmp(e->key, k, n) == 0) return e;
        i = (i + 1) & (m->cap - 1);
    }
}

static HEnt *hmap_get(HMap *m, const char *k, size_t n) {
    if (!m->cap) return NULL;
    HEnt *e = hmap_slot(m, k, n);
    return e->used ? e : NULL;
}

static HEnt *hmap_put(HMap *m, const char *k, size_t n) {
    if (m->n * 2 + 2 > m->cap) {
        HMap nm = { xcalloc(m->cap ? m->cap * 2 : 64, sizeof(HEnt)), m->cap ? m->cap * 2 : 64, 0 };
        for (size_t i = 0; i < m->cap; i++)
            if (m->e[i].used) { *hmap_slot(&nm, m->e[i].key, m->e[i].klen) = m->e[i]; nm.n++; }
        free(m->e);
        *m = nm;
    }
    HEnt *e = hmap_slot(m, k, n);
    if (!e->used) { e->used = true; e->key = k; e->klen = n; m->n++; }
    return e;
}

static void hmap_clear(HMap *m) {
    if (m->cap) memset(m->e, 0, m->cap * sizeof(HEnt));
    m->n = 0;
}

static void hmap_free(HMap *m) { free(m->e); m->e = NULL; m->cap = m->n = 0; }

/* ================================================================
 * Context and the type helpers shared in spirit with codegen.c
 * ================================================================ */

typedef struct {
    const char **var_names;
    Type **concrete;
    int count;
} SubstCtx;

static SubstCtx *g_subst = NULL;
static const char *g_lambda_suffix = NULL;
static MonoTable *g_mono = NULL;
static Arena *g_arena = NULL;
static InternTable *g_intern = NULL;
static SymbolTable *g_symtab = NULL;

static void enter_instance(MonoInstance *inst, SubstCtx *subst) {
    *subst = (SubstCtx){ inst->type_param_names, inst->type_args, inst->type_param_count };
    g_subst = subst;
    g_lambda_suffix = inst->mangled_name;
}

static void leave_instance(void) {
    g_subst = NULL;
    g_lambda_suffix = NULL;
}

static void internal_error(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = str_vsprintf(fmt, ap);
    va_end(ap);
    diag_error(loc, "native backend: %s", msg);
    free(msg);
}

static Type *subst_lookup(const char *name) {
    if (!g_subst) return NULL;
    for (int i = 0; i < g_subst->count; i++)
        if (g_subst->var_names[i] == name) return g_subst->concrete[i];
    return NULL;
}

static Type *subst_resolve(Type *t) {
    Type *bound = (t && t->kind == TYPE_TYPE_VAR) ? subst_lookup(t->type_var.name) : NULL;
    return bound ? bound : t;
}

static Type *instance_type(Type *t) {
    if (!g_subst || !t || !type_contains_type_var(t)) return t;
    t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
    if (!type_contains_type_var(t))
        mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    return t;
}

static const char *stub_instance_name(Type *t) {
    return mangle_generic_name(g_intern, instance_base_name(t),
                               t->stub.type_args, t->stub.type_arg_count);
}

static Type *resolve_struct_stub(Type *t) {
    if (!g_symtab || !t) return t;
    if (t->kind == TYPE_STUB) {
        if (t->stub.type_arg_count > 0 && !type_contains_type_var(t)) {
            const char *mangled = stub_instance_name(t);
            MonoInstance *mi = mono_find(g_mono, mangled);
            if (mi && mi->concrete_type) return mi->concrete_type;
        }
        Symbol *sym = symtab_lookup(g_symtab, t->stub.name);
        if (sym && sym->type) {
            if ((sym->type->kind == TYPE_STRUCT && sym->type->struc.field_count > 0) ||
                (sym->type->kind == TYPE_UNION && sym->type->unio.variant_count > 0) ||
                sym->type->kind == TYPE_ENUM)
                return sym->type;
        }
        MonoInstance *mi = mono_find(g_mono, t->stub.name);
        if (mi && mi->concrete_type) return mi->concrete_type;
    }
    return t;
}

static const char *generic_instance_c_name(Type *t) {
    const char *stored; Type **targs; int tac;
    if (t->kind == TYPE_STRUCT) {
        stored = t->struc.name; targs = t->struc.type_args; tac = t->struc.type_arg_count;
    } else if (t->kind == TYPE_UNION) {
        stored = t->unio.name; targs = t->unio.type_args; tac = t->unio.type_arg_count;
    } else return NULL;
    if (tac <= 0 || !targs) return stored;
    for (int i = 0; i < tac; i++)
        if (!targs[i] || type_contains_type_var(targs[i])) return stored;
    char *m = mangle_type_name(t);
    const char *r = intern_cstr(g_intern, m);
    free(m);
    return r;
}

/* The concrete type of `t` where it is used: type variables bound, stubs
 * resolved, and a generic struct or union instance replaced by its private
 * concrete type from the mono table (whose fields are concrete). */
static Type *conc(Type *t) {
    if (!t) return t;
    t = subst_resolve(t);
    if (g_subst && type_contains_type_var(t)) t = instance_type(t);
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if ((t->kind == TYPE_STRUCT && !t->struc.is_tuple && !t->struc.c_name &&
         t->struc.type_arg_count > 0) ||
        (t->kind == TYPE_UNION && t->unio.type_arg_count > 0)) {
        const char *nm = generic_instance_c_name(t);
        MonoInstance *mi = nm ? mono_find(g_mono, nm) : NULL;
        if (mi && mi->concrete_type) return mi->concrete_type;
    }
    return t;
}

static bool is_null_sentinel(Type *opt) {
    opt = conc(opt);
    if (!opt || opt->kind != TYPE_OPTION) return false;
    return option_inner_is_null_sentinel(conc(opt->option.inner));
}

static bool needs_eq_func(Type *t) {
    t = conc(t);
    return !is_null_sentinel(t) && type_needs_eq_func(t);
}

static int64_t fixarr_size(Type *t) {
    if (!t || t->kind != TYPE_FIXED_ARRAY) return 0;
    if (!t->fixed_array.size_ref) return t->fixed_array.size;
    Type *r = subst_resolve(t->fixed_array.size_ref);
    if (r && r->kind == TYPE_CONST_INT) return r->const_int.value;
    if (g_subst && r) {
        int64_t v;
        if (const_type_eval(r, g_subst->var_names, g_subst->concrete, g_subst->count, &v))
            return v;
        SrcLoc loc = {0};
        const char *msg = const_eval_take_error(&loc);
        internal_error(loc, "fixed array size did not evaluate (%s)", msg ? msg : "not a constant");
    }
    return t->fixed_array.size;
}

static Expr *widen_for_instance(Expr **slot, Type *target) {
    if (!g_subst || !target) return NULL;
    Expr *operand = *slot;
    Type *from = subst_resolve(operand->type);
    target = subst_resolve(target);
    if (!from || type_eq(from, target) || !type_is_numeric(from) || !type_is_numeric(target))
        return NULL;
    Expr *cast = arena_alloc(g_arena, sizeof(Expr));
    memset(cast, 0, sizeof *cast);
    cast->kind = EXPR_CAST;
    cast->loc = operand->loc;
    cast->type = target;
    cast->prov = operand->prov;
    cast->cast.target = target;
    cast->cast.operand = operand;
    *slot = cast;
    return operand;
}

static const char *lambda_c_name(Expr *lam) {
    if (!g_lambda_suffix) return lam->func.lifted_name;
    return arena_sprintf(g_arena, "%s__%s", lam->func.lifted_name, g_lambda_suffix);
}

static const Decl *extern_decl_of(const Symbol *s) {
    return s && s->decl && s->decl->kind == DECL_EXTERN ? s->decl : NULL;
}

/* The code symbol behind a static function value: a lambda's or FC
 * function's symbol, or an extern's C symbol (*is_extern). */
static const char *static_fn_sym(const Expr *e, bool *is_extern) {
    const Expr *t = fn_static_target(e);
    *is_extern = false;
    const Decl *ext = NULL;
    switch (t ? t->kind : EXPR_ERROR) {
    case EXPR_FUNC:
        return lambda_c_name((Expr *)t);
    case EXPR_IDENT:
        ext = extern_decl_of(t->ident.resolved_sym);
        if (!ext) return t->ident.codegen_name ? t->ident.codegen_name : t->ident.name;
        break;
    case EXPR_FIELD:
        ext = extern_decl_of(t->field.resolved_member);
        if (!ext) return t->field.codegen_name;
        break;
    default:
        internal_error(e->loc, "not a static function");
        return "";
    }
    *is_extern = true;
    return ext->ext.name;
}

static Type *union_variant_payload(Type *u, const char *variant) {
    u = conc(u);
    if (!u || u->kind != TYPE_UNION) return NULL;
    for (int v = 0; v < u->unio.variant_count; v++) {
        if (u->unio.variants[v].name != variant) continue;
        Type *payload = u->unio.variants[v].payload;
        if (payload && g_subst)
            payload = type_substitute(g_arena, payload, g_subst->var_names,
                                      g_subst->concrete, g_subst->count);
        return payload;
    }
    return NULL;
}

static int union_variant_index(Type *u, const char *variant) {
    u = conc(u);
    if (!u || u->kind != TYPE_UNION) return 0;
    for (int v = 0; v < u->unio.variant_count; v++)
        if (u->unio.variants[v].name == variant) return v;
    return 0;
}

static bool type_valueless(Type *t) {
    return !t || t->kind == TYPE_VOID || t->kind == TYPE_NEVER;
}

/* ================================================================
 * Value kinds and data layout (C rules for x86-64 System V)
 * ================================================================ */

typedef enum { VK_VOID, VK_INT, VK_FLOAT, VK_AGG } ValKind;

static ValKind val_kind(Type *t) {
    t = conc(t);
    if (!t) return VK_VOID;
    switch (t->kind) {
    case TYPE_VOID: case TYPE_NEVER: case TYPE_UNRESOLVED:
        return VK_VOID;
    case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
    case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
    case TYPE_ISIZE: case TYPE_USIZE: case TYPE_BOOL: case TYPE_ENUM:
    case TYPE_POINTER: case TYPE_ANY_PTR:
        return VK_INT;
    case TYPE_FLOAT32: case TYPE_FLOAT64:
        return VK_FLOAT;
    case TYPE_OPTION:
        return is_null_sentinel(t) ? VK_INT : VK_AGG;
    default:
        return VK_AGG;
    }
}

/* Width in bytes and signedness of a VK_INT type. */
static int int_width(Type *t) {
    t = conc(t);
    switch (t->kind) {
    case TYPE_INT8: case TYPE_UINT8: case TYPE_BOOL: return 1;
    case TYPE_INT16: case TYPE_UINT16: return 2;
    case TYPE_INT32: case TYPE_UINT32: return 4;
    case TYPE_ENUM: return int_width(t->enu.repr ? t->enu.repr : type_int32());
    default: return 8;
    }
}

static bool int_signed(Type *t) {
    t = conc(t);
    if (t->kind == TYPE_ENUM) t = t->enu.repr ? t->enu.repr : type_int32();
    switch (t->kind) {
    case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64: case TYPE_ISIZE:
        return true;
    default:
        return false;
    }
}

static bool is_f32(Type *t) { t = conc(t); return t && t->kind == TYPE_FLOAT32; }

static int64_t align_up(int64_t v, int64_t a) { return a <= 1 ? v : (v + a - 1) / a * a; }

/* How an extern struct's field sits in the C layout, when it is not simply
 * a value of its FC type at a byte offset: a bit field, or a member whose C
 * width differs from the FC type's (loads and stores convert). */
typedef struct {
    bool special;
    bool is_bit;
    int64_t csize;          /* plain member: its C size */
    bool csigned;           /* the C member is signed */
    int64_t bit_off;        /* bit field: first bit, from the struct start */
    int64_t bit_width;
    int64_t ssize;          /* the struct's size */
} ExtField;

typedef struct {
    int64_t size, align;
    int n;
    int64_t *off;
    Type **ftype;
    const char **fname;
    ExtField *ext;          /* extern structs: per field, else NULL */
    int64_t payload_off;    /* tagged unions: where the payload starts */
} Layout;

static HMap g_layouts;
static void ty_layout(Type *t, int64_t *size, int64_t *align);

static int64_t ty_size(Type *t) { int64_t s, a; ty_layout(t, &s, &a); return s; }
static int64_t ty_align(Type *t) { int64_t s, a; ty_layout(t, &s, &a); return a; }

/* Probed C layouts of extern structs, keyed by C tag: filled from the header
 * probe. Absent: the declared fields are laid out by the C rules. */
typedef struct { const char *name; int64_t off; bool scanned; int64_t csize; bool csigned;
                 int64_t bit_off, bit_width; } ProbedField;
typedef struct { int64_t size, align; int n, cap; ProbedField *f; } ProbedLayout;
static HMap g_probed_layouts;

static const char *agg_key(Type *t) {
    if (t->kind == TYPE_STRUCT) {
        if (t->struc.c_name && !t->struc.is_tuple) return t->struc.c_name;
        if (t->struc.is_tuple)
            return t->struc.name ? t->struc.name
                : tuple_canonical_name(g_intern, t->struc.fields, t->struc.field_count);
        return generic_instance_c_name(t);
    }
    return generic_instance_c_name(t);
}

static Layout *agg_layout(Type *t) {
    t = conc(t);
    const char *key = agg_key(t);
    size_t klen = strlen(key);
    /* Unions and structs live in separate C namespaces only for extern
     * tags; prefix the kind to keep them apart. */
    char *k2 = str_sprintf("%c:%s", t->kind == TYPE_UNION ? 'U' :
                           (t->struc.is_c_union ? 'C' : 'S'), key);
    klen = strlen(k2);
    HEnt *he = hmap_get(&g_layouts, k2, klen);
    if (he) { free(k2); return he->ptr; }
    Layout *L = arena_alloc(g_arena, sizeof(Layout));
    memset(L, 0, sizeof *L);
    if (t->kind == TYPE_UNION) {
        int n = t->unio.variant_count;
        L->n = n;
        L->ftype = arena_alloc(g_arena, sizeof(Type*) * (size_t)(n ? n : 1));
        L->fname = arena_alloc(g_arena, sizeof(char*) * (size_t)(n ? n : 1));
        L->off = arena_alloc(g_arena, sizeof(int64_t) * (size_t)(n ? n : 1));
        int64_t maxs = 0, maxa = 1;
        for (int i = 0; i < n; i++) {
            Type *p = t->unio.variants[i].payload;
            if (p && g_subst) p = type_substitute(g_arena, p, g_subst->var_names, g_subst->concrete, g_subst->count);
            L->ftype[i] = p;
            L->fname[i] = t->unio.variants[i].name;
            if (p) {
                int64_t s, a;
                ty_layout(p, &s, &a);
                if (s > maxs) maxs = s;
                if (a > maxa) maxa = a;
            }
        }
        bool any_payload = false;
        for (int i = 0; i < n; i++) if (L->ftype[i]) any_payload = true;
        if (!any_payload) {
            L->size = 4; L->align = 4; L->payload_off = 4;
        } else {
            int64_t al = maxa > 4 ? maxa : 4;
            L->payload_off = align_up(4, maxa);
            L->size = align_up(L->payload_off + maxs, al);
            L->align = al;
        }
        for (int i = 0; i < n; i++) L->off[i] = L->payload_off;
    } else {
        int n = t->struc.field_count;
        L->n = n;
        L->ftype = arena_alloc(g_arena, sizeof(Type*) * (size_t)(n ? n : 1));
        L->fname = arena_alloc(g_arena, sizeof(char*) * (size_t)(n ? n : 1));
        L->off = arena_alloc(g_arena, sizeof(int64_t) * (size_t)(n ? n : 1));
        ProbedLayout *pl = NULL;
        if (t->struc.c_name && !t->struc.is_tuple) {
            HEnt *pe = hmap_get(&g_probed_layouts, k2, klen);
            if (pe) pl = pe->ptr;
        }
        int64_t cur = 0, maxa = 1, maxs = 0;
        for (int i = 0; i < n; i++) {
            Type *ft = t->struc.fields[i].type;
            if (g_subst && type_contains_type_var(ft)) ft = instance_type(ft);
            L->ftype[i] = ft;
            L->fname[i] = t->struc.fields[i].name;
            int64_t s, a;
            ty_layout(ft, &s, &a);
            if (a > maxa) maxa = a;
            if (t->struc.is_c_union) {
                L->off[i] = 0;
                if (s > maxs) maxs = s;
            } else {
                cur = align_up(cur, a);
                L->off[i] = cur;
                cur += s;
            }
        }
        if (t->struc.is_c_union) cur = maxs;
        L->size = align_up(cur, maxa);
        L->align = maxa;
        if (pl) {
            L->size = pl->size;
            L->align = pl->align;
            L->ext = arena_alloc(g_arena, sizeof(ExtField) * (size_t)(n > 0 ? n : 1));
            memset(L->ext, 0, sizeof(ExtField) * (size_t)(n > 0 ? n : 1));
            for (int i = 0; i < n; i++)
                for (int j = 0; j < pl->n; j++) {
                    ProbedField *pf = &pl->f[j];
                    if (strcmp(pf->name, L->fname[i]) != 0) continue;
                    ExtField *x = &L->ext[i];
                    int64_t fsz = ty_size(L->ftype[i]);
                    if (pf->scanned) {
                        int64_t w = pf->bit_width;
                        if (pf->bit_off % 8 == 0 && (w == 8 || w == 16 || w == 32 || w == 64)) {
                            L->off[i] = pf->bit_off / 8;
                            x->csize = w / 8;
                        } else {
                            x->is_bit = true;
                            x->bit_off = pf->bit_off;
                            x->bit_width = w;
                            L->off[i] = pf->bit_off / 8;
                        }
                        x->csigned = pf->csigned;
                    } else {
                        L->off[i] = pf->off;
                        x->csize = pf->csize;
                        x->csigned = false;
                    }
                    x->ssize = L->size;
                    x->special = x->is_bit || (x->csize != fsz && val_kind(L->ftype[i]) == VK_INT);
                    break;
                }
        }
    }
    char *kcopy = arena_strdup(g_arena, k2, (int)klen);
    free(k2);
    hmap_put(&g_layouts, kcopy, klen)->ptr = L;
    return L;
}

static void ty_layout(Type *t, int64_t *size, int64_t *align) {
    t = conc(t);
    if (!t) { *size = 0; *align = 1; return; }
    switch (t->kind) {
    case TYPE_INT8: case TYPE_UINT8: case TYPE_BOOL: *size = 1; *align = 1; return;
    case TYPE_INT16: case TYPE_UINT16: *size = 2; *align = 2; return;
    case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: *size = 4; *align = 4; return;
    case TYPE_INT64: case TYPE_UINT64: case TYPE_ISIZE: case TYPE_USIZE:
    case TYPE_FLOAT64: case TYPE_POINTER: case TYPE_ANY_PTR:
        *size = 8; *align = 8; return;
    case TYPE_VOID: case TYPE_NEVER: case TYPE_UNRESOLVED:
        *size = 0; *align = 1; return;
    case TYPE_ENUM:
        ty_layout(t->enu.repr ? t->enu.repr : type_int32(), size, align);
        return;
    case TYPE_SLICE: case TYPE_FUNC:
        *size = 16; *align = 8; return;
    case TYPE_OPTION: {
        if (is_null_sentinel(t)) { *size = 8; *align = 8; return; }
        int64_t s, a;
        ty_layout(t->option.inner, &s, &a);
        *size = align_up(s + 1, a);
        *align = a;
        return;
    }
    case TYPE_RESULT: {
        Type *in = conc(t->result.inner);
        if (!in || in->kind == TYPE_VOID) { *size = 4; *align = 4; return; }
        int64_t s, a;
        ty_layout(in, &s, &a);
        int64_t al = a > 4 ? a : 4;
        *size = align_up(align_up(4, a) + s, al);
        *align = al;
        return;
    }
    case TYPE_STRUCT: case TYPE_UNION: {
        Layout *L = agg_layout(t);
        *size = L->size; *align = L->align;
        return;
    }
    case TYPE_FIXED_ARRAY: {
        int64_t s, a;
        ty_layout(t->fixed_array.elem, &s, &a);
        *size = s * fixarr_size(t);
        *align = a;
        return;
    }
    case TYPE_TYPE_VAR: case TYPE_CONST_INT: case TYPE_CONST_EXPR:
    case TYPE_STUB: case TYPE_ERROR: case TYPE_COUNT:
        break;
    }
    internal_error((SrcLoc){0}, "no layout for type %s", type_name(t));
    *size = 8; *align = 8;
}

/* The offset of a result's payload. */
static int64_t result_value_off(Type *t) {
    t = conc(t);
    Type *in = conc(t->result.inner);
    if (!in || in->kind == TYPE_VOID) return 4;
    return align_up(4, ty_align(in));
}

/* A struct or tuple field: its offset and type, and for an extern struct
 * how it sits in the C layout when that needs conversion (*x, else NULL);
 * false if absent. */
static bool field_info_x(Type *st, const char *name, int64_t *off, Type **ft, ExtField **x) {
    st = conc(st);
    if (x) *x = NULL;
    if (!st || (st->kind != TYPE_STRUCT)) return false;
    Layout *L = agg_layout(st);
    for (int i = 0; i < L->n; i++) {
        if (L->fname[i] == name || strcmp(L->fname[i], name) == 0) {
            *off = L->off[i];
            *ft = L->ftype[i];
            if (x && L->ext && L->ext[i].special) *x = &L->ext[i];
            return true;
        }
    }
    return false;
}

static bool field_info(Type *st, const char *name, int64_t *off, Type **ft) {
    return field_info_x(st, name, off, ft, NULL);
}

/* ================================================================
 * System V classification
 * ================================================================ */

typedef struct {
    int n;          /* eightbytes in registers (0..2), or -1: in memory */
    char cls[2];    /* 'I' integer, 'S' sse */
} Abi;

static void classify_leaves(Type *t, int64_t off, char cls[2]);

static void mark_leaf(int64_t off, char c, char cls[2]) {
    int i = (int)(off / 8);
    if (i < 0 || i > 1) return;
    if (c == 'I' || cls[i] == 'I') cls[i] = 'I';
    else cls[i] = 'S';
}

static void classify_leaves(Type *t, int64_t off, char cls[2]) {
    t = conc(t);
    switch (val_kind(t)) {
    case VK_VOID: return;
    case VK_INT: mark_leaf(off, 'I', cls); return;
    case VK_FLOAT: mark_leaf(off, 'S', cls); return;
    case VK_AGG: break;
    }
    switch (t->kind) {
    case TYPE_SLICE: case TYPE_FUNC:
        mark_leaf(off, 'I', cls); mark_leaf(off + 8, 'I', cls);
        return;
    case TYPE_OPTION:
        classify_leaves(t->option.inner, off, cls);
        mark_leaf(off + ty_size(t->option.inner), 'I', cls);
        return;
    case TYPE_RESULT:
        mark_leaf(off, 'I', cls);
        if (conc(t->result.inner) && conc(t->result.inner)->kind != TYPE_VOID)
            classify_leaves(t->result.inner, off + result_value_off(t), cls);
        return;
    case TYPE_FIXED_ARRAY: {
        int64_t es = ty_size(t->fixed_array.elem);
        int64_t n = fixarr_size(t);
        for (int64_t i = 0; i < n && i * es < 16; i++)
            classify_leaves(t->fixed_array.elem, off + i * es, cls);
        return;
    }
    case TYPE_STRUCT: case TYPE_UNION: {
        Layout *L = agg_layout(t);
        if (t->kind == TYPE_UNION) mark_leaf(off, 'I', cls);
        for (int i = 0; i < L->n; i++)
            if (L->ftype[i]) classify_leaves(L->ftype[i], off + L->off[i], cls);
        return;
    }
    default:
        mark_leaf(off, 'I', cls);
        return;
    }
}

static Abi abi_classify(Type *t) {
    Abi a = { 0, { 0, 0 } };
    switch (val_kind(t)) {
    case VK_VOID: return a;
    case VK_INT: a.n = 1; a.cls[0] = 'I'; return a;
    case VK_FLOAT: a.n = 1; a.cls[0] = 'S'; return a;
    case VK_AGG: break;
    }
    int64_t size = ty_size(t);
    if (size == 0) return a;
    if (size > 16) { a.n = -1; return a; }
    char cls[2] = { 0, 0 };
    classify_leaves(t, 0, cls);
    a.n = size > 8 ? 2 : 1;
    for (int i = 0; i < a.n; i++) a.cls[i] = cls[i] ? cls[i] : 'I';
    return a;
}

/* ================================================================
 * Function state: frame slots, spills, locals, labels
 * ================================================================ */

static HMap g_locals;               /* codegen name -> frame offset */
static int64_t g_frame;             /* bytes allocated below %rbp */
static int32_t *g_spills;           /* the spill slot of each depth */
static int g_spill_n, g_spill_cap, g_spill_depth;
static int g_ret_label;
static Type *g_fn_ret;              /* the function's return type */
static int32_t g_hidden_ret;        /* slot holding the hidden return pointer, or 0 */
static bool g_fn_is_main;
static bool g_tco_ok;               /* no frame address can escape: tail calls allowed */
static bool g_guards_suppressed;    /* inside `unguarded` */
static bool g_overflow_checked;     /* inside `checked` */

static int32_t slot_alloc(int64_t size, int64_t align) {
    if (align < 1) align = 1;
    if (size < 1) size = 1;
    g_frame = align_up(g_frame + size, align);
    return (int32_t)-g_frame;
}

/* A temporary for an aggregate, padded to whole eightbytes so register
 * transfers may read and write full words. */
static int32_t temp_alloc(Type *t) {
    int64_t s, a;
    ty_layout(t, &s, &a);
    return slot_alloc(align_up(s, 8), a > 8 ? a : 8);
}

static int32_t spill_push(void) {
    if (g_spill_depth == g_spill_n) {
        int32_t s = slot_alloc(8, 8);
        DA_APPEND(g_spills, g_spill_n, g_spill_cap, s);
    }
    return g_spills[g_spill_depth++];
}

static void spill_pop(void) { g_spill_depth--; }

/* The slot of a local binding, allocated on first sight. */
static int32_t local_slot(const char *name, Type *t) {
    size_t n = strlen(name);
    HEnt *e = hmap_get(&g_locals, name, n);
    if (e) return (int32_t)e->val;
    int64_t s, a;
    ty_layout(t, &s, &a);
    int32_t off = val_kind(t) == VK_AGG ? slot_alloc(align_up(s, 8), a > 8 ? a : 8)
                                        : slot_alloc(8, 8);
    hmap_put(&g_locals, name, n)->val = off;
    return off;
}

static bool local_find(const char *name, int32_t *off) {
    HEnt *e = hmap_get(&g_locals, name, strlen(name));
    if (!e) return false;
    *off = (int32_t)e->val;
    return true;
}

static void local_bind(const char *name, int32_t off) {
    hmap_put(&g_locals, name, strlen(name))->val = off;
}

/* ---- Loads, stores, normalization ---- */

static const char *reg_for_width(int w) {
    return w == 1 ? "%al" : w == 2 ? "%ax" : w == 4 ? "%eax" : "%rax";
}

/* Re-extend %rax to canonical form for VK_INT type t. */
static void normalize(Type *t) {
    int w = int_width(t);
    bool s = int_signed(t);
    if (conc(t)->kind == TYPE_BOOL) { ins("movzbl %%al, %%eax"); return; }
    switch (w) {
    case 1: ins(s ? "movsbq %%al, %%rax" : "movzbl %%al, %%eax"); break;
    case 2: ins(s ? "movswq %%ax, %%rax" : "movzwl %%ax, %%eax"); break;
    case 4: ins(s ? "movslq %%eax, %%rax" : "movl %%eax, %%eax"); break;
    default: break;
    }
}

static void load_int(Type *t, const char *mem) {
    int w = int_width(t);
    bool s = int_signed(t);
    switch (w) {
    case 1: ins(s ? "movsbq %s, %%rax" : "movzbl %s, %%eax", mem); break;
    case 2: ins(s ? "movswq %s, %%rax" : "movzwl %s, %%eax", mem); break;
    case 4: ins(s ? "movslq %s, %%rax" : "movl %s, %%eax", mem); break;
    default: ins("movq %s, %%rax", mem); break;
    }
}

static void store_int(Type *t, const char *mem) {
    ins("mov %s, %s", reg_for_width(int_width(t)), mem);
}

/* Load the value of type t stored at mem into the value registers (an
 * aggregate's address into %rax). */
static void load_val(Type *t, const char *mem) {
    switch (val_kind(t)) {
    case VK_INT: load_int(t, mem); break;
    case VK_FLOAT: ins(is_f32(t) ? "movss %s, %%xmm0" : "movsd %s, %%xmm0", mem); break;
    case VK_AGG: ins("leaq %s, %%rax", mem); break;
    case VK_VOID: break;
    }
}

/* Copy `size` bytes from src_off(%src) to dst_off(%dst). Scratch: %r11, and
 * %rsi/%rdi/%rcx for large copies. */
static void copy_mem(const char *dst, int64_t dst_off, const char *src, int64_t src_off, int64_t size) {
    if (size <= 0) return;
    if (size > 128) {
        ins("leaq %lld(%s), %%r11", (long long)src_off, src);
        ins("leaq %lld(%s), %%rdi", (long long)dst_off, dst);
        ins("movq %%r11, %%rsi");
        ins("movq $%lld, %%rcx", (long long)size);
        ins("rep movsb");
        return;
    }
    int64_t i = 0;
    for (; i + 8 <= size; i += 8) {
        ins("movq %lld(%s), %%r11", (long long)(src_off + i), src);
        ins("movq %%r11, %lld(%s)", (long long)(dst_off + i), dst);
    }
    if (i + 4 <= size) {
        ins("movl %lld(%s), %%r11d", (long long)(src_off + i), src);
        ins("movl %%r11d, %lld(%s)", (long long)(dst_off + i), dst);
        i += 4;
    }
    if (i + 2 <= size) {
        ins("movw %lld(%s), %%r11w", (long long)(src_off + i), src);
        ins("movw %%r11w, %lld(%s)", (long long)(dst_off + i), dst);
        i += 2;
    }
    if (i < size) {
        ins("movb %lld(%s), %%r11b", (long long)(src_off + i), src);
        ins("movb %%r11b, %lld(%s)", (long long)(dst_off + i), dst);
    }
}

/* Zero `size` bytes at off(%base). Large fills clobber %rax, %rcx, %rdi. */
static void zero_mem(const char *base, int64_t off, int64_t size) {
    if (size <= 0) return;
    if (size > 128) {
        ins("leaq %lld(%s), %%rdi", (long long)off, base);
        ins("xorl %%eax, %%eax");
        ins("movq $%lld, %%rcx", (long long)size);
        ins("rep stosb");
        return;
    }
    int64_t i = 0;
    for (; i + 8 <= size; i += 8) ins("movq $0, %lld(%s)", (long long)(off + i), base);
    if (i + 4 <= size) { ins("movl $0, %lld(%s)", (long long)(off + i), base); i += 4; }
    if (i + 2 <= size) { ins("movw $0, %lld(%s)", (long long)(off + i), base); i += 2; }
    if (i < size) ins("movb $0, %lld(%s)", (long long)(off + i), base);
}

/* Store the current value (registers, or the aggregate at (%rax)) into
 * off(%base). The base must not be %rax for an aggregate, nor %r11. */
static void store_val(Type *t, const char *base, int64_t off) {
    char mem[48];
    snprintf(mem, sizeof mem, "%lld(%s)", (long long)off, base);
    switch (val_kind(t)) {
    case VK_INT: store_int(t, mem); break;
    case VK_FLOAT: ins(is_f32(t) ? "movss %%xmm0, %s" : "movsd %%xmm0, %s", mem); break;
    case VK_AGG: copy_mem(base, off, "%rax", 0, ty_size(t)); break;
    case VK_VOID: break;
    }
}

/* ---- Extern struct fields that need conversion ---- */

static void bit_window(ExtField *x, int64_t *wlo, int *wbytes, int *shift) {
    int64_t lo = x->bit_off / 8;
    int64_t hi = (x->bit_off + x->bit_width - 1) / 8;
    int need = (int)(hi - lo + 1);
    int w = need <= 1 ? 1 : need <= 2 ? 2 : need <= 4 ? 4 : 8;
    if (lo + w > x->ssize && x->ssize >= w) lo = x->ssize - w;
    *wlo = lo;
    *wbytes = w;
    *shift = (int)(x->bit_off - lo * 8);
}

static void load_window_r11(int w, const char *mem) {
    switch (w) {
    case 1: ins("movzbl %s, %%r11d", mem); break;
    case 2: ins("movzwl %s, %%r11d", mem); break;
    case 4: ins("movl %s, %%r11d", mem); break;
    default: ins("movq %s, %%r11", mem); break;
    }
}

/* Load the field at (struct_off, field_off) from `base` into %rax as its FC
 * type. The base must not be %rax, %rcx, %rdi or %r11. */
static void load_ext_field(Type *ft, ExtField *x, const char *base, int64_t struct_off, int64_t field_off) {
    char mem[48];
    if (!x->is_bit) {
        snprintf(mem, sizeof mem, "%lld(%s)", (long long)field_off, base);
        switch (x->csize) {
        case 1: ins(x->csigned ? "movsbq %s, %%rax" : "movzbl %s, %%eax", mem); break;
        case 2: ins(x->csigned ? "movswq %s, %%rax" : "movzwl %s, %%eax", mem); break;
        case 4: ins(x->csigned ? "movslq %s, %%rax" : "movl %s, %%eax", mem); break;
        default: ins("movq %s, %%rax", mem); break;
        }
        normalize(ft);
        return;
    }
    int64_t wlo; int w, shift;
    bit_window(x, &wlo, &w, &shift);
    snprintf(mem, sizeof mem, "%lld(%s)", (long long)(struct_off + wlo), base);
    load_window_r11(w, mem);
    if (shift) ins("shrq $%d, %%r11", shift);
    int up = 64 - (int)x->bit_width;
    if (up > 0) {
        ins("shlq $%d, %%r11", up);
        ins(x->csigned ? "sarq $%d, %%r11" : "shrq $%d, %%r11", up);
    }
    ins("movq %%r11, %%rax");
    normalize(ft);
}

/* Store %rax (an FC value) into the field. Same register rules. */
static void store_ext_field(ExtField *x, const char *base, int64_t struct_off, int64_t field_off) {
    char mem[48];
    if (!x->is_bit) {
        snprintf(mem, sizeof mem, "%lld(%s)", (long long)field_off, base);
        ins("mov %s, %s", reg_for_width((int)x->csize), mem);
        return;
    }
    int64_t wlo; int w, shift;
    bit_window(x, &wlo, &w, &shift);
    snprintf(mem, sizeof mem, "%lld(%s)", (long long)(struct_off + wlo), base);
    int up = 64 - (int)x->bit_width;
    ins("movq %%rax, %%rcx");
    if (up > 0) { ins("shlq $%d, %%rcx", up); ins("shrq $%d, %%rcx", up); }
    if (shift) ins("shlq $%d, %%rcx", shift);
    load_window_r11(w, mem);
    uint64_t m = (x->bit_width >= 64 ? ~0ULL : ((1ULL << x->bit_width) - 1)) << shift;
    ins("movabsq $%lld, %%rdi", (long long)~m);
    ins("andq %%rdi, %%r11");
    ins("orq %%rcx, %%r11");
    switch (w) {
    case 1: ins("movb %%r11b, %s", mem); break;
    case 2: ins("movw %%r11w, %s", mem); break;
    case 4: ins("movl %%r11d, %s", mem); break;
    default: ins("movq %%r11, %s", mem); break;
    }
}

/* Load an immediate into %rax. */
static void mov_imm(int64_t v) {
    if (v == 0) ins("xorl %%eax, %%eax");
    else if (v >= INT32_MIN && v <= INT32_MAX) ins("movq $%lld, %%rax", (long long)v);
    else if (v > 0 && v <= (int64_t)UINT32_MAX) ins("movl $%lld, %%eax", (long long)v);
    else ins("movabsq $%lld, %%rax", (long long)v);
}

static void mov_imm_reg(int64_t v, const char *reg64) {
    if (v >= INT32_MIN && v <= INT32_MAX) ins("movq $%lld, %s", (long long)v, reg64);
    else ins("movabsq $%lld, %s", (long long)v, reg64);
}

static void float_const(double v, bool f32) {
    if (f32) {
        float f = (float)v;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        ins("movl $%u, %%eax", (unsigned)bits);
        ins("movd %%eax, %%xmm0");
    } else {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        ins("movabsq $%lld, %%rax", (long long)bits);
        ins("movq %%rax, %%xmm0");
    }
}

/* ---- Snapshots: hold the current value while other code runs ---- */

typedef struct { ValKind k; int32_t slot; } Snap;

static Snap snap_save(Type *t) {
    Snap s = { val_kind(t), 0 };
    switch (s.k) {
    case VK_INT: s.slot = spill_push(); ins("movq %%rax, %s", fp(s.slot)); break;
    case VK_FLOAT: s.slot = spill_push(); ins("movq %%xmm0, %s", fp(s.slot)); break;
    case VK_AGG:
        s.slot = temp_alloc(t);
        copy_mem("%rbp", s.slot, "%rax", 0, ty_size(t));
        break;
    case VK_VOID: break;
    }
    return s;
}

static void snap_restore(Snap s) {
    switch (s.k) {
    case VK_INT: ins("movq %s, %%rax", fp(s.slot)); spill_pop(); break;
    case VK_FLOAT: ins("movq %s, %%xmm0", fp(s.slot)); spill_pop(); break;
    case VK_AGG: ins("leaq %s, %%rax", fp(s.slot)); break;
    case VK_VOID: break;
    }
}

/* ================================================================
 * Read-only data: strings, string headers, function values
 * ================================================================ */

static HMap g_strings;      /* bytes -> label */
static HMap g_strhdrs;      /* bytes -> header label */
static HMap g_fnvals;       /* symbol -> function-value label */

static void emit_ascii(Buf *b, const unsigned char *s, int n) {
    buf_puts(b, "\t.string \"");
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') { char t[3] = { '\\', (char)c, 0 }; buf_puts(b, t); }
        else if (c >= 0x20 && c < 0x7f) buf_put(b, (const char *)&c, 1);
        else buf_printf(b, "\\%03o", c);
    }
    buf_puts(b, "\"\n");
}

/* The label of a NUL-terminated copy of `n` bytes in .rodata. */
static int str_label(const unsigned char *s, int n) {
    HEnt *e = hmap_get(&g_strings, (const char *)s, (size_t)n);
    if (e) return (int)e->val;
    char *k = arena_alloc(g_arena, (size_t)n + 1);
    memcpy(k, s, (size_t)n);
    k[n] = 0;
    int l = new_label();
    buf_printf(&g_rodata, ".Lq%d:\n", l);
    emit_ascii(&g_rodata, s, n);
    hmap_put(&g_strings, k, (size_t)n)->val = l;
    return l;
}

static int cstr_label(const char *s) { return str_label((const unsigned char *)s, (int)strlen(s)); }

/* A str value { ptr, len } for literal bytes, in relocated read-only data. */
static int strhdr_label(const unsigned char *s, int n) {
    HEnt *e = hmap_get(&g_strhdrs, (const char *)s, (size_t)n);
    if (e) return (int)e->val;
    int sl = str_label(s, n);
    char *k = arena_alloc(g_arena, (size_t)n + 1);
    memcpy(k, s, (size_t)n);
    k[n] = 0;
    int l = new_label();
    buf_printf(&g_relro, "\t.p2align 3\n.Lq%d:\n\t.quad .Lq%d\n\t.quad %d\n", l, sl, n);
    hmap_put(&g_strhdrs, k, (size_t)n)->val = l;
    return l;
}

/* A function value { code, NULL } for a code symbol. */
static int fnval_label(const char *sym) {
    HEnt *e = hmap_get(&g_fnvals, sym, strlen(sym));
    if (e) return (int)e->val;
    int l = new_label();
    buf_printf(&g_relro, "\t.p2align 3\n.Lq%d:\n\t.quad %s\n\t.quad 0\n", l, sym);
    hmap_put(&g_fnvals, sym, strlen(sym))->val = l;
    return l;
}

/* Decode a string literal's source text to bytes (caller frees). */
static unsigned char *decode_lit(const char *s, int slen, int *n) {
    *n = decode_str_lit(s, slen, NULL);
    unsigned char *buf = xmalloc(*n > 0 ? (size_t)*n : 1);
    decode_str_lit(s, slen, buf);
    return buf;
}

/* printf-escape raw text (a `%` doubles). */
static char *fmt_escape(const char *s, int n) {
    char *r = xmalloc((size_t)n * 2 + 1);
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '%') r[j++] = '%';
        r[j++] = s[i];
    }
    r[j] = 0;
    return r;
}

/* The label of a failure message "file:line: <rest>" (rest is a printf
 * format whose own text is already escaped). */
static int msg_label(SrcLoc loc, const char *rest) {
    const char *file = loc.filename ? loc.filename : "<unknown>";
    char *ef = fmt_escape(file, (int)strlen(file));
    char *m = str_sprintf("%s:%d: %s", ef, loc.line, rest);
    int l = cstr_label(m);
    free(ef);
    free(m);
    return l;
}

/* ---- Out-of-line failure paths ----
 * fail_begin opens a cold block (returning its label for the conditional
 * jump); the caller moves the message's arguments into %rdx, %rcx, %r8, %r9;
 * fail_end prints with fprintf(stderr, ...) and aborts. Registers hold their
 * values from the jump site. */
static Buf *g_cur_saved;

static int fail_begin(void) {
    int l = new_label();
    g_cur_saved = g_cur;
    g_cur = &g_cold;
    label(l);
    return l;
}

static void fail_end(int msg) {
    ins("movq stderr@GOTPCREL(%%rip), %%rdi");
    ins("movq (%%rdi), %%rdi");
    ins("leaq .Lq%d(%%rip), %%rsi", msg);
    ins("xorl %%eax, %%eax");
    ins("call fprintf@PLT");
    ins("call abort@PLT");
    g_cur = g_cur_saved;
}

/* A failure path with no arguments; returns the label to jump to. */
static int fail_simple(SrcLoc loc, const char *rest) {
    int m = msg_label(loc, rest);
    int l = fail_begin();
    fail_end(m);
    return l;
}

/* ---- Source lines (.loc) ---- */

static HMap g_files;
static Buf g_file_dirs;
static int g_file_count;
static int g_loc_file, g_loc_line;

static void emit_loc(SrcLoc loc) {
    if (!loc.filename || loc.line <= 0) return;
    size_t n = strlen(loc.filename);
    HEnt *e = hmap_get(&g_files, loc.filename, n);
    int id;
    if (e) id = (int)e->val;
    else {
        id = ++g_file_count;
        hmap_put(&g_files, loc.filename, n)->val = id;
        buf_printf(&g_file_dirs, "\t.file %d \"", id);
        for (size_t i = 0; i < n; i++) {
            char c = loc.filename[i];
            if (c == '"' || c == '\\') buf_put(&g_file_dirs, "\\", 1);
            buf_put(&g_file_dirs, &c, 1);
        }
        buf_puts(&g_file_dirs, "\"\n");
    }
    if (id == g_loc_file && loc.line == g_loc_line) return;
    g_loc_file = id;
    g_loc_line = loc.line;
    buf_printf(g_cur, "\t.loc %d %d\n", id, loc.line);
}

/* ---- Defer scopes and loops ---- */

typedef struct DeferScope {
    Expr **defers;
    int count, cap;
    bool is_loop;
    struct DeferScope *parent;
} DeferScope;
static DeferScope *g_defer = NULL;

static void defer_push(bool is_loop) {
    DeferScope *ds = xcalloc(1, sizeof *ds);
    ds->is_loop = is_loop;
    ds->parent = g_defer;
    g_defer = ds;
}

static void defer_pop(void) {
    DeferScope *ds = g_defer;
    g_defer = ds->parent;
    free(ds->defers);
    free(ds);
}

static bool has_pending_defers(void) {
    for (DeferScope *ds = g_defer; ds; ds = ds->parent)
        if (ds->count > 0) return true;
    return false;
}

typedef struct LoopCtx {
    int brk, cont;
    int32_t res;            /* the loop's value slot, or 0 */
    Type *res_type;
    struct LoopCtx *parent;
} LoopCtx;
static LoopCtx *g_loop = NULL;

/* ================================================================
 * Expressions
 * ================================================================ */

static void gen_expr(Expr *e);
static void gen_addr(Expr *e);
static bool gen_stmts(Expr **stmts, int count, int tail);
static void gen_return(Expr *v);
static const char *eq_helper(Type *t);
static void gen_scoped_branch(Expr *branch, int tail);
static bool gen_call(Expr *e, bool want_tail);
static void gen_pat_test(Pattern *pat, int32_t off, Type *t, int fail);
static void gen_pat_bind(Pattern *pat, int32_t off, Type *t);
static const char *cwrap_sym(const Decl *ext);
static const char *ctramp_sym(const char *fn_sym, Type *ft);
static bool extern_const_value(const char *cname, Type *t, int64_t *iv, double *fv);
static bool extern_via_shim(const Decl *d);
static const char *shim_ref(const char *cname);
static void gen_interp(Expr *e, Type *alloc_opt);

enum { TAIL_VALUE, TAIL_DISCARD, TAIL_RETURN };

static void emit_scope_defers(DeferScope *ds) {
    for (int i = ds->count - 1; i >= 0; i--) gen_expr(ds->defers[i]);
}

static void emit_defers_to_loop(void) {
    for (DeferScope *ds = g_defer; ds; ds = ds->parent) {
        emit_scope_defers(ds);
        if (ds->is_loop) break;
    }
}

static void emit_defers_to_func(void) {
    for (DeferScope *ds = g_defer; ds; ds = ds->parent) emit_scope_defers(ds);
}

static bool defers_to_loop_pending(void) {
    for (DeferScope *ds = g_defer; ds; ds = ds->parent) {
        if (ds->count > 0) return true;
        if (ds->is_loop) break;
    }
    return false;
}

/* The canonical 64-bit pattern of an integer constant of type t. */
static int64_t canon_const(uint64_t v, Type *t) {
    t = conc(t);
    if (t->kind == TYPE_BOOL) return v ? 1 : 0;
    int w = int_width(t) * 8;
    return (int64_t)const_mask_extend(v, w, int_signed(t));
}

/* An enum variant's canonical value. */
static int64_t enum_variant_value(Type *et, const char *vname) {
    et = conc(et);
    for (int i = 0; i < et->enu.variant_count; i++)
        if (et->enu.variants[i].name == vname)
            return canon_const(et->enu.variants[i].value_bits, et);
    return 0;
}

/* ---- Constants for type properties ---- */

static bool type_prop_value(Type *t, const char *prop, int64_t *iv, double *fv, bool *is_float) {
    *is_float = false;
    if (strcmp(prop, "bits") == 0) {
        switch (t->kind) {
        case TYPE_INT8: case TYPE_UINT8: *iv = 8; return true;
        case TYPE_INT16: case TYPE_UINT16: *iv = 16; return true;
        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: *iv = 32; return true;
        default: *iv = 64; return true;
        }
    }
    if (type_is_integer(t)) {
        bool mn = strcmp(prop, "min") == 0, mx = strcmp(prop, "max") == 0;
        if (!mn && !mx) return false;
        switch (t->kind) {
        case TYPE_INT8:  *iv = mn ? INT8_MIN : INT8_MAX; return true;
        case TYPE_INT16: *iv = mn ? INT16_MIN : INT16_MAX; return true;
        case TYPE_INT32: *iv = mn ? INT32_MIN : INT32_MAX; return true;
        case TYPE_INT64: case TYPE_ISIZE: *iv = mn ? INT64_MIN : INT64_MAX; return true;
        case TYPE_UINT8:  *iv = mn ? 0 : UINT8_MAX; return true;
        case TYPE_UINT16: *iv = mn ? 0 : UINT16_MAX; return true;
        case TYPE_UINT32: *iv = mn ? 0 : (int64_t)UINT32_MAX; return true;
        case TYPE_UINT64: case TYPE_USIZE: *iv = mn ? 0 : -1; return true;
        default: return false;
        }
    }
    if (type_is_float(t)) {
        bool f = t->kind == TYPE_FLOAT32;
        *is_float = true;
        if (strcmp(prop, "min") == 0) { *fv = f ? 1.17549435082228750797e-38 : 2.2250738585072014e-308; return true; }
        if (strcmp(prop, "max") == 0) { *fv = f ? 3.40282346638528859812e+38 : 1.7976931348623157e+308; return true; }
        if (strcmp(prop, "epsilon") == 0) { *fv = f ? 1.1920928955078125e-07 : 2.220446049250313e-16; return true; }
        if (strcmp(prop, "nan") == 0) { *fv = NAN; return true; }
        if (strcmp(prop, "inf") == 0) { *fv = INFINITY; return true; }
        if (strcmp(prop, "neg_inf") == 0) { *fv = -INFINITY; return true; }
    }
    return false;
}

static void gen_const_of(Type *t, int64_t iv, double fv, bool is_float) {
    if (val_kind(t) == VK_FLOAT) float_const(is_float ? fv : (double)iv, is_f32(t));
    else mov_imm(canon_const((uint64_t)iv, t));
}

/* ---- Identifiers and globals ---- */

/* A static function used as an FC value: the address of a { code, NULL }
 * constant. An extern goes through its context-taking wrapper. */
static void gen_static_fn_value(Expr *e) {
    bool is_extern;
    const char *sym = static_fn_sym(e, &is_extern);
    if (is_extern) {
        const Expr *t = fn_static_target(e);
        const Symbol *s = t->kind == EXPR_IDENT ? t->ident.resolved_sym : t->field.resolved_member;
        sym = cwrap_sym(s->decl);
    }
    ins("leaq .Lq%d(%%rip), %%rax", fnval_label(sym));
}

/* A static function as a C function pointer: an FC function's trampoline
 * (no context argument), or an extern's own symbol. */
static void gen_c_fn_pointer(Expr *e) {
    bool is_extern;
    const char *sym = static_fn_sym(e, &is_extern);
    const Expr *tg = fn_static_target(e);
    const Symbol *ts = !tg ? NULL : tg->kind == EXPR_IDENT ? tg->ident.resolved_sym
                     : tg->kind == EXPR_FIELD ? tg->field.resolved_member : NULL;
    if (is_extern && ts && extern_via_shim(ts->decl)) ins("movq fc_xp_%s(%%rip), %%rax", shim_ref(sym));
    else if (is_extern) ins("movq %s@GOTPCREL(%%rip), %%rax", sym);
    else ins("leaq %s(%%rip), %%rax", ctramp_sym(sym, conc(e->type)));
}

static void gen_extern_const(const char *cname, Type *t, SrcLoc loc) {
    int64_t iv = 0; double fv = 0;
    if (!extern_const_value(cname, t, &iv, &fv)) {
        internal_error(loc, "no value for extern constant '%s'", cname);
        return;
    }
    if (val_kind(t) == VK_FLOAT) float_const(fv, is_f32(t));
    else if (is_cstr_type(conc(t))) {
        /* A string #define: the probe recorded its bytes. */
        ins("leaq .Lq%d(%%rip), %%rax", (int)iv);
    } else mov_imm(canon_const((uint64_t)iv, t));
}

static void gen_ident(Expr *e) {
    if (e->ident.is_std_stream) {
        ins("movq %s@GOTPCREL(%%rip), %%rax", e->ident.name);
        ins("movq (%%rax), %%rax");
        return;
    }
    if (!e->ident.is_local && fn_static_target(e)) {
        gen_static_fn_value(e);
        return;
    }
    const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
    if (e->ident.is_local) {
        int32_t off;
        if (!local_find(name, &off)) {
            internal_error(e->loc, "unknown local '%s'", name);
            return;
        }
        load_val(e->type, fp(off));
        return;
    }
    Symbol *s = e->ident.resolved_sym;
    if (s && s->decl && s->decl->kind == DECL_EXTERN) {
        gen_extern_const(s->decl->ext.name, e->type, e->loc);
        return;
    }
    load_val(e->type, arena_sprintf(g_arena, "%s(%%rip)", name));
}

/* ---- Address of an lvalue ---- */

static void gen_slice_elem_addr(Expr *e);

static void gen_addr(Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT: {
        const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
        if (e->ident.is_local) {
            int32_t off;
            if (!local_find(name, &off)) { internal_error(e->loc, "unknown local '%s'", name); return; }
            ins("leaq %s, %%rax", fp(off));
        } else {
            ins("leaq %s(%%rip), %%rax", name);
        }
        return;
    }
    case EXPR_FIELD: {
        if (e->field.codegen_name) { ins("leaq %s(%%rip), %%rax", e->field.codegen_name); return; }
        int64_t off; Type *ft; ExtField *x;
        gen_expr(e->field.object);
        if (field_info_x(e->field.object->type, e->field.name, &off, &ft, &x)) {
            if (x) internal_error(e->loc, "the address of C field '%s' (a bit field, or a different width)", e->field.name);
            if (off) ins("leaq %lld(%%rax), %%rax", (long long)off);
        } else internal_error(e->loc, "no field '%s'", e->field.name);
        return;
    }
    case EXPR_DEREF_FIELD: {
        int64_t off; Type *ft; ExtField *x;
        gen_expr(e->field.object);
        Type *pt = conc(e->field.object->type);
        if (field_info_x(pt->pointer.pointee, e->field.name, &off, &ft, &x)) {
            if (x) internal_error(e->loc, "the address of C field '%s' (a bit field, or a different width)", e->field.name);
            if (off) ins("leaq %lld(%%rax), %%rax", (long long)off);
        } else internal_error(e->loc, "no field '%s'", e->field.name);
        return;
    }
    case EXPR_INDEX: {
        Type *ot = conc(e->index.object->type);
        if (ot->kind == TYPE_STRUCT && ot->struc.is_tuple) {
            gen_expr(e->index.object);
            char nm[24];
            snprintf(nm, sizeof nm, "e%llu", (unsigned long long)e->index.index->int_lit.value);
            int64_t off; Type *ft;
            if (field_info(ot, intern_cstr(g_intern, nm), &off, &ft) && off)
                ins("leaq %lld(%%rax), %%rax", (long long)off);
            return;
        }
        if (ot->kind == TYPE_SLICE) { gen_slice_elem_addr(e); return; }
        /* Pointer indexing: unchecked. */
        int64_t es = ty_size(ot->pointer.pointee);
        gen_expr(e->index.object);
        int32_t s = spill_push();
        ins("movq %%rax, %s", fp(s));
        gen_expr(e->index.index);
        if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
        ins("addq %s, %%rax", fp(s));
        spill_pop();
        return;
    }
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_STAR) { gen_expr(e->unary_prefix.operand); return; }
        break;
    case EXPR_GUARD: {
        bool sg = g_guards_suppressed, so = g_overflow_checked;
        if (e->guard.is_overflow_axis) g_overflow_checked = e->guard.enable;
        else g_guards_suppressed = !e->guard.enable;
        gen_addr(e->guard.body);
        g_guards_suppressed = sg; g_overflow_checked = so;
        return;
    }
    default:
        break;
    }
    /* An rvalue aggregate already lives at an address. */
    gen_expr(e);
    if (val_kind(e->type) != VK_AGG) internal_error(e->loc, "address of a non-lvalue");
}

/* &s[i] with the bounds check. */
static void gen_slice_elem_addr(Expr *e) {
    Type *ot = conc(e->index.object->type);
    int64_t es = ty_size(ot->slice.elem);
    gen_expr(e->index.object);
    int32_t s = spill_push();
    ins("movq %%rax, %s", fp(s));
    gen_expr(e->index.index);
    ins("movq %s, %%rdx", fp(s));
    spill_pop();
    if (!(g_guards_suppressed && facts_guard_governs(e, subst_resolve))) {
        bool uns = !int_signed(e->index.index->type);
        ins("movq 8(%%rdx), %%rcx");
        ins("cmpq %%rcx, %%rax");
        int m = msg_label(e->loc, uns ? "slice index out of range: index=%llu len=%llu\n"
                                      : "slice index out of range: index=%lld len=%llu\n");
        int l = fail_begin();
        ins("movq %%rax, %%rdx");
        fail_end(m);
        ins("jae .Lq%d", l);
    }
    if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
    ins("addq (%%rdx), %%rax");
}

/* ---- Fields ---- */

static void gen_field(Expr *e) {
    if (e->field.is_type_property) {
        const char *cg = e->field.codegen_name;
        char *endp;
        long long v = strtoll(cg, &endp, 10);
        if (*cg && *endp == 0) { mov_imm(canon_const((uint64_t)v, e->type)); return; }
        Expr *obj = e->field.object;
        Type *pt = obj->kind == EXPR_IDENT
            ? type_from_name(obj->ident.name, (int)strlen(obj->ident.name)) : NULL;
        int64_t iv; double fv; bool isf;
        if (pt && type_prop_value(pt, e->field.name, &iv, &fv, &isf)) {
            gen_const_of(e->type, iv, fv, isf);
            return;
        }
        internal_error(e->loc, "unknown type property '%s'", e->field.name);
        return;
    }
    if (e->field.codegen_name) {
        if (fn_static_target(e)) { gen_static_fn_value(e); return; }
        if (e->field.is_extern_const) {
            gen_extern_const(e->field.codegen_name, e->type, e->loc);
            return;
        }
        load_val(e->type, arena_sprintf(g_arena, "%s(%%rip)", e->field.codegen_name));
        return;
    }
    if (e->field.is_variant_constructor) {
        Type *t = conc(e->type);
        if (t->kind == TYPE_ENUM) { mov_imm(enum_variant_value(t, e->field.name)); return; }
        int32_t tmp = temp_alloc(t);
        zero_mem("%rbp", tmp, ty_size(t));
        ins("movl $%d, %s", union_variant_index(t, e->field.name), fp(tmp));
        ins("leaq %s, %%rax", fp(tmp));
        return;
    }
    if (e->field.object->kind == EXPR_TYPE_VAR_REF && g_subst) {
        Type *c = subst_lookup(e->field.object->type_var_ref.name);
        int64_t iv; double fv; bool isf;
        if (c && type_prop_value(conc(c), e->field.name, &iv, &fv, &isf)) {
            gen_const_of(e->type, iv, fv, isf);
            return;
        }
        internal_error(e->loc, "type has no property '%s'", e->field.name);
        return;
    }
    Type *ot = conc(e->field.object->type);
    if (e->field.fixed_array_type) {
        Type *fat = e->field.fixed_array_type;
        gen_addr(e);
        int32_t tmp = slot_alloc(16, 8);
        ins("movq %%rax, %s", fp(tmp));
        ins("movq $%lld, %s", (long long)fixarr_size(fat), fp(tmp + 8));
        ins("leaq %s, %%rax", fp(tmp));
        return;
    }
    if (ot->kind == TYPE_OPTION) {
        bool some = strcmp(e->field.name, "is_some") == 0;
        gen_expr(e->field.object);
        if (is_null_sentinel(ot)) ins("testq %%rax, %%rax");
        else ins("cmpb $0, %lld(%%rax)", (long long)ty_size(ot->option.inner));
        ins(some ? "setne %%al" : "sete %%al");
        ins("movzbl %%al, %%eax");
        return;
    }
    if (ot->kind == TYPE_RESULT) {
        bool okf = strcmp(e->field.name, "is_ok") == 0;
        gen_expr(e->field.object);
        ins("cmpl $0, (%%rax)");
        ins(okf ? "sete %%al" : "setne %%al");
        ins("movzbl %%al, %%eax");
        return;
    }
    if (ot->kind == TYPE_SLICE) {
        gen_expr(e->field.object);
        ins(strcmp(e->field.name, "len") == 0 ? "movq 8(%%rax), %%rax" : "movq (%%rax), %%rax");
        return;
    }
    int64_t off; Type *ft; ExtField *x;
    gen_expr(e->field.object);
    if (!field_info_x(ot, e->field.name, &off, &ft, &x)) {
        internal_error(e->loc, "no field '%s' in %s", e->field.name, type_name(ot));
        return;
    }
    if (x) { ins("movq %%rax, %%rdx"); load_ext_field(ft, x, "%rdx", 0, off); return; }
    char mem[48];
    snprintf(mem, sizeof mem, "%lld(%%rax)", (long long)off);
    load_val(ft, mem);
}

static void gen_deref_field(Expr *e) {
    Type *pt = conc(e->field.object->type);
    Type *st = conc(pt->pointer.pointee);
    if (e->field.fixed_array_type) {
        gen_addr(e);
        int32_t tmp = slot_alloc(16, 8);
        ins("movq %%rax, %s", fp(tmp));
        ins("movq $%lld, %s", (long long)fixarr_size(e->field.fixed_array_type), fp(tmp + 8));
        ins("leaq %s, %%rax", fp(tmp));
        return;
    }
    gen_expr(e->field.object);
    if (st->kind == TYPE_SLICE) {
        ins(strcmp(e->field.name, "len") == 0 ? "movq 8(%%rax), %%rax" : "movq (%%rax), %%rax");
        return;
    }
    int64_t off; Type *ft; ExtField *x;
    if (!field_info_x(st, e->field.name, &off, &ft, &x)) {
        internal_error(e->loc, "no field '%s'", e->field.name);
        return;
    }
    if (x) { ins("movq %%rax, %%rdx"); load_ext_field(ft, x, "%rdx", 0, off); return; }
    char mem[48];
    snprintf(mem, sizeof mem, "%lld(%%rax)", (long long)off);
    load_val(ft, mem);
}

/* ---- Binary operators ---- */

static const char *setcc_for(int op, bool sgn) {
    switch (op) {
    case TOK_EQEQ: return "sete";
    case TOK_BANGEQ: return "setne";
    case TOK_LT: return sgn ? "setl" : "setb";
    case TOK_LTEQ: return sgn ? "setle" : "setbe";
    case TOK_GT: return sgn ? "setg" : "seta";
    case TOK_GTEQ: return sgn ? "setge" : "setae";
    default: return NULL;
    }
}

static void overflow_fail_jump(SrcLoc loc, const char *what, const char *jcc) {
    char *rest = str_sprintf("integer overflow in %s\n", what);
    int l = fail_simple(loc, rest);
    free(rest);
    ins("%s .Lq%d", jcc, l);
}

/* Evaluate two operands: left into %rax, right into %rcx (ints), or left into
 * %xmm0 and right into %xmm1 (floats). */
static void gen_operands(Expr *l, Expr *r) {
    if (val_kind(l->type) == VK_FLOAT) {
        gen_expr(l);
        int32_t s = spill_push();
        ins("movq %%xmm0, %s", fp(s));
        gen_expr(r);
        ins("movaps %%xmm0, %%xmm1");
        ins("movq %s, %%xmm0", fp(s));
        spill_pop();
        return;
    }
    gen_expr(l);
    int32_t s = spill_push();
    ins("movq %%rax, %s", fp(s));
    gen_expr(r);
    ins("movq %%rax, %%rcx");
    ins("movq %s, %%rax", fp(s));
    spill_pop();
}

static void gen_divmod(Expr *e, Type *rt) {
    int op = e->binary.op;
    bool sgn = int_signed(rt);
    gen_operands(e->binary.left, e->binary.right);
    bool need_zero = !(g_guards_suppressed && facts_guard_governs(e, subst_resolve));
    if (need_zero) {
        int l = fail_simple(e->loc, op == TOK_SLASH ? "divide by zero\n" : "modulo by zero\n");
        ins("testq %%rcx, %%rcx");
        ins("jz .Lq%d", l);
    }
    if (!sgn) {
        ins("xorl %%edx, %%edx");
        ins("divq %%rcx");
        if (op == TOK_PERCENT) ins("movq %%rdx, %%rax");
        normalize(rt);
        return;
    }
    int lnorm = new_label(), ldone = new_label();
    ins("cmpq $-1, %%rcx");
    ins("jne .Lq%d", lnorm);
    if (op == TOK_PERCENT) {
        ins("xorl %%eax, %%eax");
    } else {
        if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
            int64_t mn; double fv; bool isf;
            type_prop_value(conc(rt), "min", &mn, &fv, &isf);
            mov_imm_reg(mn, "%rdx");
            ins("cmpq %%rdx, %%rax");
            overflow_fail_jump(e->loc, "divide", "je");
        }
        ins("negq %%rax");
        normalize(rt);
    }
    ins("jmp .Lq%d", ldone);
    label(lnorm);
    ins("cqo");
    ins("idivq %%rcx");
    if (op == TOK_PERCENT) ins("movq %%rdx, %%rax");
    normalize(rt);
    label(ldone);
}

static void gen_float_binary(Expr *e) {
    int op = e->binary.op;
    bool f = is_f32(e->binary.left->type);
    const char *sfx = f ? "ss" : "sd";
    gen_operands(e->binary.left, e->binary.right);
    switch (op) {
    case TOK_PLUS: ins("add%s %%xmm1, %%xmm0", sfx); return;
    case TOK_MINUS: ins("sub%s %%xmm1, %%xmm0", sfx); return;
    case TOK_STAR: ins("mul%s %%xmm1, %%xmm0", sfx); return;
    case TOK_SLASH: ins("div%s %%xmm1, %%xmm0", sfx); return;
    default: break;
    }
    const char *uc = f ? "ucomiss" : "ucomisd";
    switch (op) {
    case TOK_EQEQ:
        ins("%s %%xmm1, %%xmm0", uc); ins("sete %%al"); ins("setnp %%cl"); ins("andb %%cl, %%al");
        break;
    case TOK_BANGEQ:
        ins("%s %%xmm1, %%xmm0", uc); ins("setne %%al"); ins("setp %%cl"); ins("orb %%cl, %%al");
        break;
    case TOK_GT: ins("%s %%xmm1, %%xmm0", uc); ins("seta %%al"); break;
    case TOK_GTEQ: ins("%s %%xmm1, %%xmm0", uc); ins("setae %%al"); break;
    case TOK_LT: ins("%s %%xmm0, %%xmm1", uc); ins("seta %%al"); break;
    case TOK_LTEQ: ins("%s %%xmm0, %%xmm1", uc); ins("setae %%al"); break;
    default:
        internal_error(e->loc, "float operator %s", token_kind_name(op));
        return;
    }
    ins("movzbl %%al, %%eax");
}

static void gen_binary_op(Expr *e) {
    int op = e->binary.op;
    Type *lt = conc(e->binary.left->type);
    Type *rt = conc(e->type);

    if ((op == TOK_EQEQ || op == TOK_BANGEQ) && needs_eq_func(lt)) {
        gen_expr(e->binary.left);
        Snap sn = snap_save(lt);   /* the right operand may change the left's storage */
        gen_expr(e->binary.right);
        ins("movq %%rax, %%rsi");
        ins("leaq %s, %%rdi", fp(sn.slot));
        ins("call %s", eq_helper(lt));
        ins("movzbl %%al, %%eax");
        if (op == TOK_BANGEQ) ins("xorl $1, %%eax");
        return;
    }
    if (val_kind(lt) == VK_FLOAT) { gen_float_binary(e); return; }

    if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
        int lend = new_label();
        gen_expr(e->binary.left);
        ins("testq %%rax, %%rax");
        ins(op == TOK_AMPAMP ? "jz .Lq%d" : "jnz .Lq%d", lend);
        gen_expr(e->binary.right);
        label(lend);
        return;
    }

    /* Pointer arithmetic. */
    Type *rtt = conc(e->binary.right->type);
    if ((op == TOK_PLUS || op == TOK_MINUS) &&
        (lt->kind == TYPE_POINTER || rtt->kind == TYPE_POINTER)) {
        gen_operands(e->binary.left, e->binary.right);
        if (lt->kind == TYPE_POINTER && rtt->kind == TYPE_POINTER) {
            int64_t es = ty_size(lt->pointer.pointee);
            ins("subq %%rcx, %%rax");
            if (es > 1) { ins("movq $%lld, %%rcx", (long long)es); ins("cqo"); ins("idivq %%rcx"); }
            return;
        }
        if (lt->kind == TYPE_POINTER) {
            int64_t es = ty_size(lt->pointer.pointee);
            if (es != 1) ins("imulq $%lld, %%rcx, %%rcx", (long long)es);
            ins(op == TOK_PLUS ? "addq %%rcx, %%rax" : "subq %%rcx, %%rax");
        } else {
            int64_t es = ty_size(rtt->pointer.pointee);
            if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
            ins("addq %%rcx, %%rax");
        }
        return;
    }

    const char *cc = setcc_for(op, int_signed(lt));
    if (cc) {
        gen_operands(e->binary.left, e->binary.right);
        ins("cmpq %%rcx, %%rax");
        ins("%s %%al", cc);
        ins("movzbl %%al, %%eax");
        return;
    }

    switch (op) {
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: {
        gen_operands(e->binary.left, e->binary.right);
        const char *mn = op == TOK_PLUS ? "addq" : op == TOK_MINUS ? "subq" : "imulq";
        const char *what = op == TOK_PLUS ? "+" : op == TOK_MINUS ? "-" : "*";
        bool checked = g_overflow_checked && facts_overflow_governs(e, subst_resolve);
        if (checked && int_width(rt) == 8) {
            if (int_signed(rt)) {
                ins("%s %%rcx, %%rax", mn);
                overflow_fail_jump(e->loc, what, "jo");
            } else if (op == TOK_STAR) {
                ins("mulq %%rcx");
                overflow_fail_jump(e->loc, what, "jo");
            } else {
                ins("%s %%rcx, %%rax", mn);
                overflow_fail_jump(e->loc, what, "jc");
            }
            return;
        }
        ins("%s %%rcx, %%rax", mn);
        if (checked) {
            ins("movq %%rax, %%rdx");
            normalize(rt);
            ins("cmpq %%rax, %%rdx");
            overflow_fail_jump(e->loc, what, "jne");
        } else {
            normalize(rt);
        }
        return;
    }
    case TOK_SLASH: case TOK_PERCENT:
        gen_divmod(e, rt);
        return;
    case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        gen_operands(e->binary.left, e->binary.right);
        ins(op == TOK_AMP ? "andq %%rcx, %%rax" : op == TOK_PIPE ? "orq %%rcx, %%rax" : "xorq %%rcx, %%rax");
        return;
    case TOK_LTLT: case TOK_GTGT: {
        gen_operands(e->binary.left, e->binary.right);
        int bits = int_width(rt) * 8;
        ins("andl $%d, %%ecx", bits - 1);
        if (op == TOK_LTLT) {
            ins("shlq %%cl, %%rax");
            normalize(rt);
        } else {
            ins(int_signed(rt) ? "sarq %%cl, %%rax" : "shrq %%cl, %%rax");
        }
        return;
    }
    default:
        internal_error(e->loc, "binary operator %s", token_kind_name(op));
    }
}

static void gen_binary(Expr *e) {
    Expr *unw[2] = { NULL, NULL };
    if (g_subst && e->binary.op != TOK_LTLT && e->binary.op != TOK_GTGT) {
        Type *common = type_common_numeric(subst_resolve(e->binary.left->type),
                                           subst_resolve(e->binary.right->type));
        unw[0] = widen_for_instance(&e->binary.left, common);
        unw[1] = widen_for_instance(&e->binary.right, common);
    }
    gen_binary_op(e);
    if (unw[0]) e->binary.left = unw[0];
    if (unw[1]) e->binary.right = unw[1];
}

/* ---- Unary operators ---- */

static void gen_unary_prefix(Expr *e) {
    int op = e->unary_prefix.op;
    Type *rt = conc(e->type);
    if (op == TOK_AMP) {
        if (fn_static_target(e->unary_prefix.operand)) { gen_c_fn_pointer(e->unary_prefix.operand); return; }
        gen_addr(e->unary_prefix.operand);
        return;
    }
    if (op == TOK_STAR) {
        gen_expr(e->unary_prefix.operand);
        load_val(e->type, "(%rax)");
        return;
    }
    gen_expr(e->unary_prefix.operand);
    switch (op) {
    case TOK_MINUS:
        if (val_kind(rt) == VK_FLOAT) {
            if (is_f32(rt)) { ins("movd %%xmm0, %%eax"); ins("xorl $0x80000000, %%eax"); ins("movd %%eax, %%xmm0"); }
            else { ins("movq %%xmm0, %%rax"); ins("btcq $63, %%rax"); ins("movq %%rax, %%xmm0"); }
            return;
        }
        if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
            if (int_width(rt) == 8 && int_signed(rt)) {
                ins("negq %%rax");
                overflow_fail_jump(e->loc, "negation", "jo");
            } else {
                ins("negq %%rax");
                ins("movq %%rax, %%rdx");
                normalize(rt);
                ins("cmpq %%rax, %%rdx");
                overflow_fail_jump(e->loc, "negation", "jne");
            }
            return;
        }
        ins("negq %%rax");
        normalize(rt);
        return;
    case TOK_BANG:
        ins("xorl $1, %%eax");
        return;
    case TOK_TILDE:
        ins("notq %%rax");
        normalize(rt);
        return;
    default:
        internal_error(e->loc, "unary operator %s", token_kind_name(op));
    }
}

/* Move the current value (of the function's return type) into the return
 * registers, or through the hidden pointer. */
static void move_to_return_regs(void) {
    Type *t = g_fn_ret;
    switch (val_kind(t)) {
    case VK_INT: normalize(t); return;
    case VK_FLOAT: case VK_VOID: return;
    case VK_AGG: break;
    }
    Abi a = abi_classify(t);
    if (a.n == -1) {
        ins("movq %s, %%rdi", fp(g_hidden_ret));
        copy_mem("%rdi", 0, "%rax", 0, ty_size(t));
        ins("movq %s, %%rax", fp(g_hidden_ret));
        return;
    }
    ins("movq %%rax, %%r11");
    int ni = 0, ns = 0;
    for (int k = 0; k < a.n; k++) {
        if (a.cls[k] == 'I') ins("movq %d(%%r11), %s", 8 * k, ni++ ? "%rdx" : "%rax");
        else ins("movq %d(%%r11), %s", 8 * k, ns++ ? "%xmm1" : "%xmm0");
    }
}

/* Build a function's "none" or "err(code)" failure return in place: the code
 * (for a result) is in %ecx. Leaves the value as move_to_return_regs wants. */
static void gen_failure_value(Type *ret) {
    ret = conc(ret);
    if (ret->kind == TYPE_RESULT) {
        int32_t t = temp_alloc(ret);
        int32_t code = slot_alloc(8, 8);
        ins("movq %%rcx, %s", fp(code));      /* a large zero fill clobbers %rcx */
        zero_mem("%rbp", t, ty_size(ret));
        ins("movq %s, %%rcx", fp(code));
        ins("movl %%ecx, %s", fp(t));
        ins("leaq %s, %%rax", fp(t));
    } else if (is_null_sentinel(ret)) {
        ins("xorl %%eax, %%eax");
    } else {
        int32_t t = temp_alloc(ret);
        zero_mem("%rbp", t, ty_size(ret));
        ins("leaq %s, %%rax", fp(t));
    }
}

static void gen_unary_postfix(Expr *e) {
    Type *ot = conc(e->unary_postfix.operand->type);
    if (e->unary_postfix.op == TOK_BANG) {
        char *text = e->unary_postfix.expr_text
            ? fmt_escape(e->unary_postfix.expr_text, e->unary_postfix.expr_text_len) : str_dup("");
        gen_expr(e->unary_postfix.operand);
        if (ot->kind == TYPE_RESULT) {
            char *rest = str_sprintf("unwrap failed: %s (error code %%lld)\n", text);
            int m = msg_label(e->loc, rest);
            free(rest);
            int l = fail_begin();
            ins("movslq (%%rax), %%rdx");
            fail_end(m);
            ins("cmpl $0, (%%rax)");
            ins("jne .Lq%d", l);
            Type *in = conc(ot->result.inner);
            if (in && in->kind != TYPE_VOID) {
                char mem[48];
                snprintf(mem, sizeof mem, "%lld(%%rax)", (long long)result_value_off(ot));
                load_val(in, mem);
            }
        } else {
            char *rest = str_sprintf("unwrap failed: %s\n", text);
            int l = fail_simple(e->loc, rest);
            free(rest);
            if (is_null_sentinel(ot)) {
                ins("testq %%rax, %%rax");
                ins("jz .Lq%d", l);
            } else {
                ins("cmpb $0, %lld(%%rax)", (long long)ty_size(ot->option.inner));
                ins("je .Lq%d", l);
                load_val(ot->option.inner, "(%rax)");
            }
        }
        free(text);
        return;
    }
    /* x? : on failure, run every pending defer and return the failure at
     * the function's return type. */
    gen_expr(e->unary_postfix.operand);
    int lok = new_label();
    if (ot->kind == TYPE_RESULT) {
        ins("movl (%%rax), %%ecx");
        ins("testl %%ecx, %%ecx");
        ins("jz .Lq%d", lok);
    } else if (is_null_sentinel(ot)) {
        ins("testq %%rax, %%rax");
        ins("jnz .Lq%d", lok);
    } else {
        ins("cmpb $0, %lld(%%rax)", (long long)ty_size(ot->option.inner));
        ins("jne .Lq%d", lok);
    }
    /* The failure branch. */
    if (has_pending_defers()) {
        int32_t s = spill_push();
        ins("movq %%rcx, %s", fp(s));
        emit_defers_to_func();
        ins("movq %s, %%rcx", fp(s));
        spill_pop();
    }
    gen_failure_value(e->unary_postfix.prop_fn_ret ? e->unary_postfix.prop_fn_ret : g_fn_ret);
    move_to_return_regs();
    ins("jmp .Lq%d", g_ret_label);
    label(lok);
    if (ot->kind == TYPE_RESULT) {
        Type *in = conc(ot->result.inner);
        if (in && in->kind != TYPE_VOID) {
            char mem[48];
            snprintf(mem, sizeof mem, "%lld(%%rax)", (long long)result_value_off(ot));
            load_val(in, mem);
        }
    } else if (!is_null_sentinel(ot)) {
        load_val(ot->option.inner, "(%rax)");
    }
}

/* ================================================================
 * Calls (System V)
 * ================================================================ */

typedef struct {
    Type *type;     /* concrete type as passed */
    int32_t slot;   /* frame offset: an 8-byte scalar, or the aggregate's bytes */
    bool zero;      /* the constant 0 (a null context) */
} CallArg;

typedef struct {
    const char *sym;    /* direct target; NULL: indirect through fn_slot */
    bool plt;           /* a C library function */
    bool shim;          /* a function from a local header: through its shim pointer */
    int32_t fn_slot;
    bool variadic;
    Type *ret;
    bool tail;
} CallSpec;

static const char *IREG64[6] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
static const char *IREG32[6] = { "%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d" };

/* Evaluate an argument into a fresh slot. Returns true when it took a
 * spill slot (the caller pops it after the call). */
static bool eval_arg(Expr *a, CallArg *ca) {
    Type *t = conc(a->type);
    ca->type = t;
    ca->zero = false;
    ca->slot = 0;
    gen_expr(a);
    switch (val_kind(t)) {
    case VK_INT: ca->slot = spill_push(); ins("movq %%rax, %s", fp(ca->slot)); return true;
    case VK_FLOAT: ca->slot = spill_push(); ins("movq %%xmm0, %s", fp(ca->slot)); return true;
    case VK_AGG:
        ca->slot = temp_alloc(t);
        copy_mem("%rbp", ca->slot, "%rax", 0, ty_size(t));
        return false;
    case VK_VOID: return false;
    }
    return false;
}

/* Place the current %rax value as a scalar argument. */
static void arg_from_rax(CallArg *ca, Type *t) {
    ca->type = t;
    ca->zero = false;
    ca->slot = slot_alloc(8, 8);
    ins("movq %%rax, %s", fp(ca->slot));
}

/* Emit the call. With cs->tail, the call becomes a jump when its arguments
 * all travel in registers; returns whether it did. */
static bool emit_call(CallSpec *cs, CallArg *args, int n) {
    Abi ra = abi_classify(cs->ret);
    bool mem_ret = ra.n == -1;
    int ni = mem_ret ? 1 : 0, ns = 0;
    int *where = xcalloc((size_t)(n > 0 ? n : 1), sizeof(int));
    int64_t *soff = xcalloc((size_t)(n > 0 ? n : 1), sizeof(int64_t));
    Abi *ab = xcalloc((size_t)(n > 0 ? n : 1), sizeof(Abi));
    int64_t stack = 0;
    for (int i = 0; i < n; i++) {
        ab[i] = args[i].zero ? (Abi){ 1, { 'I', 0 } } : abi_classify(args[i].type);
        if (ab[i].n == 0) { where[i] = 0; continue; }
        if (ab[i].n > 0) {
            int ci = 0, cS = 0;
            for (int k = 0; k < ab[i].n; k++) { if (ab[i].cls[k] == 'I') ci++; else cS++; }
            if (ni + ci <= 6 && ns + cS <= 8) { where[i] = 1; ni += ci; ns += cS; continue; }
        }
        where[i] = 2;
        int64_t size = args[i].zero ? 8 : ty_size(args[i].type);
        int64_t al = args[i].zero ? 8 : ty_align(args[i].type);
        if (al < 8) al = 8;
        stack = align_up(stack, al);
        soff[i] = stack;
        stack += align_up(size, 8);
    }
    int64_t total = align_up(stack, 16);
    bool tail = cs->tail && total == 0 && (!mem_ret || g_hidden_ret);
    int32_t res = 0;
    if (mem_ret && !tail) res = temp_alloc(cs->ret);
    if (total) ins("subq $%lld, %%rsp", (long long)total);
    for (int i = 0; i < n; i++) {
        if (where[i] != 2) continue;
        if (args[i].zero) { ins("movq $0, %lld(%%rsp)", (long long)soff[i]); continue; }
        if (val_kind(args[i].type) == VK_AGG)
            copy_mem("%rsp", soff[i], "%rbp", args[i].slot, align_up(ty_size(args[i].type), 8));
        else {
            ins("movq %s, %%r11", fp(args[i].slot));
            ins("movq %%r11, %lld(%%rsp)", (long long)soff[i]);
        }
    }
    ni = mem_ret ? 1 : 0;
    ns = 0;
    for (int i = 0; i < n; i++) {
        if (where[i] != 1) continue;
        if (args[i].zero) { ins("xorl %s, %s", IREG32[ni], IREG32[ni]); ni++; continue; }
        ValKind k = val_kind(args[i].type);
        if (k == VK_INT) { ins("movq %s, %s", fp(args[i].slot), IREG64[ni++]); continue; }
        if (k == VK_FLOAT) { ins("movq %s, %%xmm%d", fp(args[i].slot), ns++); continue; }
        for (int q = 0; q < ab[i].n; q++) {
            if (ab[i].cls[q] == 'I') ins("movq %s, %s", fp(args[i].slot + 8 * q), IREG64[ni++]);
            else ins("movq %s, %%xmm%d", fp(args[i].slot + 8 * q), ns++);
        }
    }
    if (mem_ret) {
        if (tail) ins("movq %s, %%rdi", fp(g_hidden_ret));
        else ins("leaq %s, %%rdi", fp(res));
    }
    if (!cs->sym) ins("movq %s, %%r11", fp(cs->fn_slot));
    if (cs->variadic) ins("movl $%d, %%eax", ns);
    free(where); free(soff); free(ab);
    if (tail) {
        ins("leave");
        ins(".cfi_remember_state");
        ins(".cfi_def_cfa %%rsp, 8");
        if (!cs->sym) ins("jmp *%%r11");
        else if (cs->shim) ins("jmp *fc_xp_%s(%%rip)", shim_ref(cs->sym));
        else if (cs->plt) ins("jmp %s@PLT", cs->sym);
        else ins("jmp %s", cs->sym);
        ins(".cfi_restore_state");
        return true;
    }
    if (!cs->sym) ins("call *%%r11");
    else if (cs->shim) ins("call *fc_xp_%s(%%rip)", shim_ref(cs->sym));
    else if (cs->plt) ins("call %s@PLT", cs->sym);
    else ins("call %s", cs->sym);
    if (total) ins("addq $%lld, %%rsp", (long long)total);
    switch (val_kind(cs->ret)) {
    case VK_INT: normalize(cs->ret); break;
    case VK_FLOAT: case VK_VOID: break;
    case VK_AGG:
        if (mem_ret) { ins("leaq %s, %%rax", fp(res)); break; }
        if (ra.n == 0) { int32_t t = slot_alloc(8, 8); ins("leaq %s, %%rax", fp(t)); break; }
        {
            int32_t t = temp_alloc(cs->ret);
            int ii = 0, ss = 0;
            for (int q = 0; q < ra.n; q++) {
                if (ra.cls[q] == 'I') ins("movq %s, %s", ii++ ? "%rdx" : "%rax", fp(t + 8 * q));
                else ins("movq %s, %s", ss++ ? "%xmm1" : "%xmm0", fp(t + 8 * q));
            }
            ins("leaq %s, %%rax", fp(t));
        }
        break;
    }
    return false;
}

/* A runtime library call with arguments already in registers. */
static void call_c(const char *sym) { ins("call %s@PLT", sym); }

static const char *resolve_direct_name(Expr *e) {
    Expr *callee = e->call.func;
    const char *fn_name = e->call.mangled_name;
    if (!fn_name && g_subst && e->call.type_arg_count > 0) {
        Type **concrete = xmalloc(sizeof(Type*) * (size_t)e->call.type_arg_count);
        for (int i = 0; i < e->call.type_arg_count; i++)
            concrete[i] = type_substitute(g_arena, e->call.type_args[i],
                                          g_subst->var_names, g_subst->concrete, g_subst->count);
        Symbol *cs = e->call.resolved_callee;
        if (cs) {
            const char *base = (cs->decl && cs->decl->kind == DECL_LET && cs->decl->let.codegen_name)
                ? cs->decl->let.codegen_name : cs->name;
            fn_name = mangle_generic_name(g_intern, base, concrete, e->call.type_arg_count);
            if (!mono_find(g_mono, fn_name))
                internal_error(e->loc, "generic instance '%s' was not discovered", fn_name);
        }
        free(concrete);
    }
    if (!fn_name) {
        if (callee->kind == EXPR_IDENT)
            fn_name = callee->ident.codegen_name ? callee->ident.codegen_name : callee->ident.name;
        else if (callee->kind == EXPR_FIELD && callee->field.codegen_name)
            fn_name = callee->field.codegen_name;
    }
    return fn_name;
}

static ExternProtocol call_protocol(Expr *e) {
    Symbol *s = e->call.resolved_callee;
    if (s && s->kind == DECL_EXTERN && s->decl) return s->decl->ext.protocol;
    return EXT_PROTO_NONE;
}

/* Whether `return e` may become a jump. */
static bool tail_compatible(Expr *e) {
    if (e->kind != EXPR_CALL) return false;
    if (e->call.func->kind == EXPR_FIELD && e->call.func->field.is_variant_constructor) return false;
    if (e->call.is_extern_call && call_protocol(e) > EXT_PROTO_ERROR) return false;
    Type *a = conc(e->type), *b = conc(g_fn_ret);
    if (type_valueless(a) && type_valueless(b)) return a->kind == b->kind;
    return type_eq_ignore_const(a, b);
}

static void gen_protocol_wrap(Expr *e, ExternProtocol proto, Type *ret) {
    ret = conc(ret);
    Type *pay = conc(ret->result.inner);
    bool pay_void = !pay || pay->kind == TYPE_VOID;
    int32_t t = temp_alloc(ret);
    int64_t voff = result_value_off(ret);
    int32_t raw = slot_alloc(8, 8);
    ins("movq %%rax, %s", fp(raw));         /* a large zero fill clobbers %rax */
    zero_mem("%rbp", t, ty_size(ret));
    ins("movq %s, %%rax", fp(raw));
    if (proto == EXT_PROTO_STATUS) {
        ins("movl %%eax, %s", fp(t));
        ins("leaq %s, %%rax", fp(t));
        return;
    }
    if (proto == EXT_PROTO_NEG_ERRNO || proto == EXT_PROTO_HRESULT) {
        int lok = new_label(), ld = new_label();
        ins("testq %%rax, %%rax");
        ins("jns .Lq%d", lok);
        ins("movl %%eax, %s", fp(t));
        ins("jmp .Lq%d", ld);
        label(lok);
        if (!pay_void) store_val(pay, "%rbp", t + voff);
        label(ld);
        ins("leaq %s, %%rax", fp(t));
        return;
    }
    if (proto == EXT_PROTO_ERRNO_NEG1 || proto == EXT_PROTO_ERRNO_NULL) {
        int lok = new_label(), ld = new_label();
        if (proto == EXT_PROTO_ERRNO_NULL) ins("testq %%rax, %%rax");
        else ins("cmpq $-1, %%rax");
        ins("jne .Lq%d", lok);
        call_c("__errno_location");
        ins("movl (%%rax), %%eax");
        int l = fail_simple(e->loc, "err() with code 0 (0 is the ok tag; error codes must be non-zero)\n");
        ins("testl %%eax, %%eax");
        ins("jz .Lq%d", l);
        ins("movl %%eax, %s", fp(t));
        ins("jmp .Lq%d", ld);
        label(lok);
        if (!pay_void) store_val(pay, "%rbp", t + voff);
        label(ld);
        ins("leaq %s, %%rax", fp(t));
        return;
    }
    internal_error(e->loc, "error protocol not supported on this target");
}

static bool gen_call(Expr *e, bool want_tail) {
    Expr *fn = e->call.func;
    if (fn->kind == EXPR_FIELD && fn->field.is_variant_constructor) {
        Type *ut = conc(e->type);
        Layout *L = agg_layout(ut);
        int idx = union_variant_index(ut, fn->field.name);
        int32_t t = temp_alloc(ut);
        zero_mem("%rbp", t, L->size);
        gen_expr(e->call.args[0]);
        Type *pt = union_variant_payload(ut, fn->field.name);
        store_val(pt ? pt : e->call.args[0]->type, "%rbp", t + L->payload_off);
        ins("movl $%d, %s", idx, fp(t));
        ins("leaq %s, %%rax", fp(t));
        return false;
    }
    Type *ft = conc(fn->type);
    int n = e->call.arg_count;
    Expr **unw = NULL;
    if (g_subst && ft && ft->kind == TYPE_FUNC && n > 0) {
        unw = xcalloc((size_t)n, sizeof(Expr*));
        for (int i = 0; i < n && i < ft->func.param_count; i++)
            unw[i] = widen_for_instance(&e->call.args[i], ft->func.param_types[i]);
    }
    CallArg *args = xcalloc((size_t)n + 1, sizeof(CallArg));
    int nspill = 0;
    CallSpec cs = { 0 };
    cs.ret = conc(e->type);
    cs.tail = want_tail;
    bool did_tail = false;
    if (e->call.is_indirect) {
        gen_expr(fn);
        int32_t fs = slot_alloc(16, 8);
        copy_mem("%rbp", fs, "%rax", 0, 16);
        for (int i = 0; i < n; i++) nspill += eval_arg(e->call.args[i], &args[i]);
        args[n].type = type_any_ptr();
        args[n].slot = fs + 8;
        cs.fn_slot = fs;
        did_tail = emit_call(&cs, args, n + 1);
    } else {
        const char *name = resolve_direct_name(e);
        if (!name) { internal_error(e->loc, "unresolved callee"); free(args); return false; }
        if (e->call.is_extern_call) {
            for (int i = 0; i < n; i++) {
                Type *pt = (ft && ft->kind == TYPE_FUNC && i < ft->func.param_count)
                    ? conc(ft->func.param_types[i]) : NULL;
                Expr *a = e->call.args[i];
                if (pt && pt->kind == TYPE_FUNC) {
                    gen_c_fn_pointer(a);
                    arg_from_rax(&args[i], type_any_ptr());
                } else if (pt && pt->kind == TYPE_OPTION && pt->option.inner &&
                           conc(pt->option.inner)->kind == TYPE_FUNC) {
                    if (a->kind == EXPR_SOME) gen_c_fn_pointer(a->some_expr.value);
                    else ins("xorl %%eax, %%eax");
                    arg_from_rax(&args[i], type_any_ptr());
                } else if (!pt && is_f32(a->type)) {
                    /* A variadic float travels as double. */
                    gen_expr(a);
                    ins("cvtss2sd %%xmm0, %%xmm0");
                    args[i].type = type_float64();
                    args[i].slot = slot_alloc(8, 8);
                    ins("movq %%xmm0, %s", fp(args[i].slot));
                } else {
                    nspill += eval_arg(a, &args[i]);
                }
            }
            cs.sym = name;
            cs.plt = true;
            cs.shim = e->call.resolved_callee && extern_via_shim(e->call.resolved_callee->decl);
            cs.variadic = ft && ft->kind == TYPE_FUNC && ft->func.is_variadic;
            ExternProtocol proto = call_protocol(e);
            if (proto > EXT_PROTO_ERROR && cs.ret->kind == TYPE_RESULT) {
                Type *pay = conc(cs.ret->result.inner);
                Type *rt = cs.ret;
                cs.ret = (!pay || pay->kind == TYPE_VOID) ? type_int32() : pay;
                cs.tail = false;
                emit_call(&cs, args, n);
                gen_protocol_wrap(e, proto, rt);
            } else {
                did_tail = emit_call(&cs, args, n);
            }
        } else {
            for (int i = 0; i < n; i++) nspill += eval_arg(e->call.args[i], &args[i]);
            args[n].zero = true;
            args[n].type = type_any_ptr();
            cs.sym = name;
            did_tail = emit_call(&cs, args, n + 1);
        }
    }
    while (nspill-- > 0) spill_pop();
    free(args);
    for (int i = 0; unw && i < n; i++)
        if (unw[i]) e->call.args[i] = unw[i];
    free(unw);
    return did_tail;
}

/* ================================================================
 * Casts
 * ================================================================ */

typedef struct { double lo, hi; int64_t imin, imax; } F2i;

static F2i f2i_info(Type *t) {
    switch (conc(t)->kind) {
    case TYPE_INT8:  return (F2i){ -128.0, 128.0, INT8_MIN, INT8_MAX };
    case TYPE_INT16: return (F2i){ -32768.0, 32768.0, INT16_MIN, INT16_MAX };
    case TYPE_INT32: return (F2i){ -2147483648.0, 2147483648.0, INT32_MIN, INT32_MAX };
    case TYPE_INT64: case TYPE_ISIZE:
        return (F2i){ -9223372036854775808.0, 9223372036854775808.0, INT64_MIN, INT64_MAX };
    case TYPE_UINT8:  return (F2i){ 0.0, 256.0, 0, UINT8_MAX };
    case TYPE_UINT16: return (F2i){ 0.0, 65536.0, 0, UINT16_MAX };
    case TYPE_UINT32: return (F2i){ 0.0, 4294967296.0, 0, (int64_t)UINT32_MAX };
    default: return (F2i){ 0.0, 18446744073709551616.0, 0, -1 };   /* u64, usize */
    }
}

static void dbl_to_xmm1(double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    ins("movabsq $%lld, %%r11", (long long)bits);
    ins("movq %%r11, %%xmm1");
}

/* Truncate the double in %xmm0 (in range for `to`) into %rax. */
static void cvt_double_to_int(Type *to) {
    Type *t = conc(to);
    if (t->kind == TYPE_UINT64 || t->kind == TYPE_USIZE) {
        int lbig = new_label(), ld = new_label();
        dbl_to_xmm1(9223372036854775808.0);
        ins("ucomisd %%xmm1, %%xmm0");
        ins("jae .Lq%d", lbig);
        ins("cvttsd2siq %%xmm0, %%rax");
        ins("jmp .Lq%d", ld);
        label(lbig);
        ins("subsd %%xmm1, %%xmm0");
        ins("cvttsd2siq %%xmm0, %%rax");
        ins("btcq $63, %%rax");
        label(ld);
        return;
    }
    ins("cvttsd2siq %%xmm0, %%rax");
    normalize(t);
}

static void gen_float_to_int(Type *from, Type *to, bool saturate) {
    if (is_f32(from)) ins("cvtss2sd %%xmm0, %%xmm0");
    if (!saturate) { cvt_double_to_int(to); return; }
    F2i fi = f2i_info(to);
    int lnan = new_label(), llo = new_label(), lhi = new_label(), ld = new_label();
    ins("ucomisd %%xmm0, %%xmm0");
    ins("jp .Lq%d", lnan);
    dbl_to_xmm1(fi.lo);
    ins("ucomisd %%xmm1, %%xmm0");
    ins("jb .Lq%d", llo);
    dbl_to_xmm1(fi.hi);
    ins("ucomisd %%xmm1, %%xmm0");
    ins("jae .Lq%d", lhi);
    cvt_double_to_int(to);
    ins("jmp .Lq%d", ld);
    label(lnan);
    ins("xorl %%eax, %%eax");
    ins("jmp .Lq%d", ld);
    label(llo);
    mov_imm(canon_const((uint64_t)fi.imin, to));
    ins("jmp .Lq%d", ld);
    label(lhi);
    mov_imm(canon_const((uint64_t)fi.imax, to));
    label(ld);
}

static void gen_int_to_float(Type *from, Type *to) {
    bool f = is_f32(to);
    Type *ft = conc(from);
    if (ft->kind == TYPE_UINT64 || ft->kind == TYPE_USIZE) {
        int lbig = new_label(), ld = new_label();
        ins("testq %%rax, %%rax");
        ins("js .Lq%d", lbig);
        ins(f ? "cvtsi2ssq %%rax, %%xmm0" : "cvtsi2sdq %%rax, %%xmm0");
        ins("jmp .Lq%d", ld);
        label(lbig);
        ins("movq %%rax, %%rcx");
        ins("shrq %%rcx");
        ins("andl $1, %%eax");
        ins("orq %%rax, %%rcx");
        ins(f ? "cvtsi2ssq %%rcx, %%xmm0" : "cvtsi2sdq %%rcx, %%xmm0");
        ins(f ? "addss %%xmm0, %%xmm0" : "addsd %%xmm0, %%xmm0");
        label(ld);
        return;
    }
    ins(f ? "cvtsi2ssq %%rax, %%xmm0" : "cvtsi2sdq %%rax, %%xmm0");
}

static void gen_checked_narrow(Type *from, Type *to, SrcLoc loc) {
    from = conc(type_enum_underlying(conc(from)));
    to = conc(to);
    int64_t tmin, tmax; double fv; bool isf;
    type_prop_value(to, "min", &tmin, &fv, &isf);
    type_prop_value(to, "max", &tmax, &fv, &isf);
    bool to_u64 = (to->kind == TYPE_UINT64 || to->kind == TYPE_USIZE);
    if (!int_signed(from)) {
        if (!to_u64) {
            mov_imm_reg(tmax, "%rdx");
            ins("cmpq %%rdx, %%rax");
            overflow_fail_jump(loc, "cast", "ja");
        }
    } else if (to_u64) {
        ins("testq %%rax, %%rax");
        overflow_fail_jump(loc, "cast", "js");
    } else {
        mov_imm_reg(tmin, "%rdx");
        ins("cmpq %%rdx, %%rax");
        overflow_fail_jump(loc, "cast", "jl");
        mov_imm_reg(tmax, "%rdx");
        ins("cmpq %%rdx, %%rax");
        overflow_fail_jump(loc, "cast", "jg");
    }
    normalize(to);
}

static void gen_cast(Expr *e) {
    Expr *op = e->cast.operand;
    Type *from = conc(op->type), *to = conc(e->cast.target);
    if (is_str_type(from) && is_cstr_type(to)) {
        gen_expr(op);
        if (e->cast.bounded) {
            int64_t n = e->cast.buffer_size;
            int32_t back = slot_alloc(align_up(n, 8), 8);
            ins("movq (%%rax), %%rsi");
            ins("movq 8(%%rax), %%rdx");
            if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
                char *rest = str_sprintf("string truncation in (cstr[%lld]) cast: len=%%lld max=%%lld\n", (long long)n);
                int m = msg_label(e->loc, rest);
                free(rest);
                int l = fail_begin();
                ins("movq $%lld, %%rcx", (long long)(n - 1));
                fail_end(m);
                ins("cmpq $%lld, %%rdx", (long long)(n - 1));
                ins("jg .Lq%d", l);
            }
            int lle = new_label();
            ins("cmpq $%lld, %%rdx", (long long)(n - 1));
            ins("jle .Lq%d", lle);
            ins("movq $%lld, %%rdx", (long long)(n - 1));
            label(lle);
            int32_t cn = spill_push();
            ins("movq %%rdx, %s", fp(cn));
            ins("leaq %s, %%rdi", fp(back));
            call_c("memcpy");
            ins("movq %s, %%rcx", fp(cn));
            spill_pop();
            ins("leaq %s, %%rax", fp(back));
            ins("movb $0, (%%rax,%%rcx)");
        } else {
            /* (cstr) under alloca: a dynamic stack copy. */
            int32_t sp = spill_push();
            ins("movq %%rax, %s", fp(sp));
            ins("movq 8(%%rax), %%rax");
            ins("leaq 16(%%rax), %%rax");
            ins("andq $-16, %%rax");
            ins("subq %%rax, %%rsp");
            ins("movq %s, %%rax", fp(sp));
            ins("movq %%rsp, %%rdi");
            ins("movq (%%rax), %%rsi");
            ins("movq 8(%%rax), %%rdx");
            call_c("memcpy");
            ins("movq %s, %%rcx", fp(sp));
            ins("movq 8(%%rcx), %%rcx");
            ins("movb $0, (%%rax,%%rcx)");
            spill_pop();
        }
        return;
    }
    if (is_cstr_type(from) && is_str_type(to)) {
        gen_expr(op);
        int32_t t = slot_alloc(16, 8);
        ins("movq %%rax, %s", fp(t));
        ins("movq %%rax, %%rdi");
        call_c("strlen");
        ins("movq %%rax, %s", fp(t + 8));
        ins("leaq %s, %%rax", fp(t));
        return;
    }
    ValKind fk = val_kind(from), tk = val_kind(to);
    if (fk == VK_FLOAT && tk == VK_INT) {
        gen_expr(op);
        bool sat = !(g_guards_suppressed && facts_guard_governs(e, subst_resolve));
        gen_float_to_int(from, to, sat);
        return;
    }
    gen_expr(op);
    if (fk == VK_INT && tk == VK_INT) {
        if (g_overflow_checked && facts_overflow_governs(e, subst_resolve))
            gen_checked_narrow(from, to, e->loc);
        else
            normalize(to);
        return;
    }
    if (fk == VK_INT && tk == VK_FLOAT) { gen_int_to_float(from, to); return; }
    if (fk == VK_FLOAT && tk == VK_FLOAT) {
        if (is_f32(from) && !is_f32(to)) ins("cvtss2sd %%xmm0, %%xmm0");
        else if (!is_f32(from) && is_f32(to)) ins("cvtsd2ss %%xmm0, %%xmm0");
        return;
    }
    /* Aggregate-to-aggregate casts only move const: the bits are the same. */
}

/* ================================================================
 * Literals and constructors
 * ================================================================ */

static void gen_struct_lit(Expr *e) {
    Type *st = conc(e->type);
    Layout *L = agg_layout(st);
    int32_t t = temp_alloc(st);
    bool fixed = false;
    for (int i = 0; i < L->n; i++)
        if (conc(L->ftype[i])->kind == TYPE_FIXED_ARRAY) fixed = true;
    if (fixed || e->struct_lit.field_count < L->n) zero_mem("%rbp", t, L->size);
    for (int i = 0; i < e->struct_lit.field_count; i++) {
        int64_t off; Type *ft; ExtField *x;
        if (!field_info_x(st, e->struct_lit.fields[i].name, &off, &ft, &x)) {
            internal_error(e->loc, "no field '%s'", e->struct_lit.fields[i].name);
            continue;
        }
        if (x) {
            gen_expr(e->struct_lit.fields[i].value);
            store_ext_field(x, "%rbp", t, t + off);
            continue;
        }
        Expr *v = e->struct_lit.fields[i].value;
        Type *cft = conc(ft);
        if (cft->kind == TYPE_FIXED_ARRAY) {
            int64_t cap = fixarr_size(cft), es = ty_size(cft->fixed_array.elem);
            gen_expr(v);
            char *rest = str_sprintf("fixed-array field '%s' overflow: len=%%lld capacity=%lld\n",
                                     e->struct_lit.fields[i].name, (long long)cap);
            int m = msg_label(v->loc, rest);
            free(rest);
            int l = fail_begin();
            fail_end(m);
            ins("movq 8(%%rax), %%rdx");
            ins("cmpq $%lld, %%rdx", (long long)cap);
            ins("jg .Lq%d", l);
            ins("movq (%%rax), %%rsi");
            ins("leaq %s, %%rdi", fp(t + (int32_t)off));
            if (es != 1) ins("imulq $%lld, %%rdx, %%rcx", (long long)es);
            else ins("movq %%rdx, %%rcx");
            ins("rep movsb");
            continue;
        }
        gen_expr(v);
        store_val(ft, "%rbp", t + off);
    }
    ins("leaq %s, %%rax", fp(t));
}

static void gen_tuple_lit(Expr *e) {
    Type *tt = conc(e->type);
    Layout *L = agg_layout(tt);
    int32_t t = temp_alloc(tt);
    for (int i = 0; i < e->tuple_lit.elem_count && i < L->n; i++) {
        gen_expr(e->tuple_lit.elems[i]);
        store_val(L->ftype[i], "%rbp", t + L->off[i]);
    }
    ins("leaq %s, %%rax", fp(t));
}

/* Allocate `size_reg`-many bytes (in %rax) on the stack; the address lands
 * in %rax. */
static void dyn_alloca_rax(void) {
    ins("addq $15, %%rax");
    ins("andq $-16, %%rax");
    ins("subq %%rax, %%rsp");
    ins("movq %%rsp, %%rax");
}

/* T[N]{...}: a slice over a frame slot (fixed size), or over fresh stack
 * memory each evaluation (`dynamic`, the alloca form). */
static void gen_array_lit(Expr *e, bool dynamic) {
    Type *elem = conc(e->array_lit.elem_type);
    int64_t es = ty_size(elem);
    Expr *se = e->array_lit.size_expr;
    bool const_n = se && se->kind == EXPR_INT_LIT;
    int64_t N = const_n ? (int64_t)se->int_lit.value : 0;
    int32_t hdr = slot_alloc(16, 8);
    int32_t nslot = 0;
    if (!const_n) {
        gen_expr(se);
        nslot = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(nslot));
    }
    bool on_frame = const_n && !dynamic && N > 0;
    int32_t back = 0;
    if (on_frame) {
        back = slot_alloc(align_up(N * es, 8), ty_align(elem) > 8 ? ty_align(elem) : 8);
        ins("leaq %s, %%rax", fp(back));
    } else if (const_n) {
        mov_imm(N * es);
        dyn_alloca_rax();
    } else {
        ins("movq %s, %%rax", fp(nslot));
        if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
        dyn_alloca_rax();
    }
    ins("movq %%rax, %s", fp(hdr));
    if (const_n) ins("movq $%lld, %s", (long long)N, fp(hdr + 8));
    else { ins("movq %s, %%rcx", fp(nslot)); ins("movq %%rcx, %s", fp(hdr + 8)); }
    if (e->array_lit.elem_count == 0) {
        if (on_frame) zero_mem("%rbp", back, N * es);
        else {
            ins("movq %s, %%rdi", fp(hdr));
            ins("movq %s, %%rcx", fp(hdr + 8));
            if (es != 1) ins("imulq $%lld, %%rcx, %%rcx", (long long)es);
            ins("xorl %%eax, %%eax");
            ins("rep stosb");
        }
    } else {
        for (int i = 0; i < e->array_lit.elem_count; i++) {
            gen_expr(e->array_lit.elems[i]);
            if (on_frame) store_val(elem, "%rbp", back + i * es);
            else {
                ins("movq %s, %%rdx", fp(hdr));
                store_val(elem, "%rdx", i * es);
            }
        }
    }
    ins("leaq %s, %%rax", fp(hdr));
}

static void gen_slice_lit(Expr *e) {
    int32_t t = slot_alloc(16, 8);
    gen_expr(e->slice_lit.ptr_expr);
    ins("movq %%rax, %s", fp(t));
    gen_expr(e->slice_lit.len_expr);
    if (!e->slice_lit.len_nonneg) {
        int m = msg_label(e->loc, "slice literal length is negative: len=%lld\n");
        int l = fail_begin();
        ins("movq %%rax, %%rdx");
        fail_end(m);
        ins("testq %%rax, %%rax");
        ins("js .Lq%d", l);
    }
    ins("movq %%rax, %s", fp(t + 8));
    ins("leaq %s, %%rax", fp(t));
}

static void gen_some(Expr *e) {
    Type *ot = conc(e->type);
    if (is_null_sentinel(ot)) {
        gen_expr(e->some_expr.value);
        if (!ptr_value_provably_nonnull(e->some_expr.value)) {
            int l = fail_simple(e->loc, "some() of a null pointer (pointer options use null as the none sentinel)\n");
            ins("testq %%rax, %%rax");
            ins("jz .Lq%d", l);
        }
        return;
    }
    int32_t t = temp_alloc(ot);
    Type *in = conc(ot->option.inner);
    int64_t is = ty_size(in);
    zero_mem("%rbp", t, ty_size(ot));
    gen_expr(e->some_expr.value);
    store_val(in, "%rbp", t);
    ins("movb $1, %s", fp(t + (int32_t)is));
    ins("leaq %s, %%rax", fp(t));
}

static void gen_ok(Expr *e) {
    Type *rt = conc(e->type);
    int32_t t = temp_alloc(rt);
    if (e->ok_expr.value) {
        gen_expr(e->ok_expr.value);
        store_val(rt->result.inner, "%rbp", t + result_value_off(rt));
    }
    ins("movl $0, %s", fp(t));
    ins("leaq %s, %%rax", fp(t));
}

static void gen_err(Expr *e) {
    Type *rt = conc(e->type);
    int32_t t = temp_alloc(rt);
    gen_expr(e->err_expr.code);
    if (!int_value_provably_nonzero(e->err_expr.code)) {
        int l = fail_simple(e->loc, "err() with code 0 (0 is the ok tag; error codes must be non-zero)\n");
        ins("testl %%eax, %%eax");
        ins("jz .Lq%d", l);
    }
    int32_t cs = spill_push();
    ins("movq %%rax, %s", fp(cs));
    zero_mem("%rbp", t, ty_size(rt));
    ins("movq %s, %%rax", fp(cs));
    spill_pop();
    ins("movl %%eax, %s", fp(t));
    ins("leaq %s, %%rax", fp(t));
}

static void gen_default(Type *t) {
    switch (val_kind(t)) {
    case VK_INT: ins("xorl %%eax, %%eax"); return;
    case VK_FLOAT: ins("xorps %%xmm0, %%xmm0"); return;
    case VK_VOID: return;
    case VK_AGG: {
        int32_t s = temp_alloc(t);
        zero_mem("%rbp", s, ty_size(t));
        ins("leaq %s, %%rax", fp(s));
        return;
    }
    }
}

/* The layout of a capturing lambda's context: each capture's offset. */
static int64_t ctx_layout(Expr *lam, int64_t *offs, int64_t *align_out) {
    int64_t cur = 0, maxa = 1;
    for (int i = 0; i < lam->func.capture_count; i++) {
        int64_t s, a;
        ty_layout(lam->func.captures[i].type, &s, &a);
        cur = align_up(cur, a);
        if (offs) offs[i] = cur;
        cur += s;
        if (a > maxa) maxa = a;
    }
    if (align_out) *align_out = maxa;
    return align_up(cur, maxa);
}

/* Store each capture's current value into the context at (%rdx base +
 * offset) or a frame slot. */
static void fill_captures(Expr *lam, const char *base, int64_t base_off) {
    int n = lam->func.capture_count;
    int64_t *offs = xcalloc((size_t)(n ? n : 1), sizeof(int64_t));
    ctx_layout(lam, offs, NULL);
    for (int i = 0; i < n; i++) {
        Capture *c = &lam->func.captures[i];
        int32_t loc;
        if (!local_find(c->codegen_name, &loc)) {
            internal_error(lam->loc, "capture '%s' not in scope", c->name);
            continue;
        }
        load_val(c->type, fp(loc));
        if (strcmp(base, "%rbp") != 0) {
            /* the base pointer lives in a spill slot passed as base_off */
            ins("movq %s, %%rdx", fp((int32_t)base_off));
            store_val(c->type, "%rdx", offs[i]);
        } else {
            store_val(c->type, "%rbp", base_off + offs[i]);
        }
    }
    free(offs);
}

static void gen_func_value(Expr *e) {
    const char *sym = lambda_c_name(e);
    if (e->func.capture_count == 0) {
        ins("leaq .Lq%d(%%rip), %%rax", fnval_label(sym));
        return;
    }
    int64_t al;
    int64_t size = ctx_layout(e, NULL, &al);
    int32_t ctx = slot_alloc(align_up(size, 8), al > 8 ? al : 8);
    fill_captures(e, "%rbp", ctx);
    int32_t fv = slot_alloc(16, 8);
    ins("leaq %s(%%rip), %%rax", sym);
    ins("movq %%rax, %s", fp(fv));
    ins("leaq %s, %%rax", fp(ctx));
    ins("movq %%rax, %s", fp(fv + 8));
    ins("leaq %s, %%rax", fp(fv));
}

/* ================================================================
 * Allocation
 * ================================================================ */

/* %rdi = max(%rax, 1): an element count for a heap request that may be 0. */
static void alloc_n_rdi(void) {
    ins("movq %%rax, %%rdi");
    ins("testq %%rdi, %%rdi");
    ins("movl $1, %%ecx");
    ins("cmovle %%rcx, %%rdi");
}

/* Build an option over a slice from %rax (pointer, NULL on failure) and the
 * length in `len_slot`; leaves the option's address in %rax. */
static void some_slice_or_none(Type *opt, int32_t len_slot) {
    int32_t t = temp_alloc(opt);
    zero_mem("%rbp", t, ty_size(opt));     /* small: does not touch %rax */
    int lnone = new_label();
    ins("testq %%rax, %%rax");
    ins("jz .Lq%d", lnone);
    ins("movq %%rax, %s", fp(t));
    ins("movq %s, %%rcx", fp(len_slot));
    ins("movq %%rcx, %s", fp(t + 8));
    ins("movb $1, %s", fp(t + 16));
    label(lnone);
    ins("leaq %s, %%rax", fp(t));
}

static void gen_alloc(Expr *e) {
    Expr *init = e->alloc_expr.init_expr;
    Type *at = e->alloc_expr.alloc_type ? conc(e->alloc_expr.alloc_type) : NULL;
    if (e->alloc_expr.is_stack) {
        if (at && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
            gen_expr(e->alloc_expr.size_expr);
            int64_t es = ty_size(at);
            if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
            dyn_alloca_rax();
        } else if (at && e->alloc_expr.size_expr) {
            int64_t es = ty_size(at);
            gen_expr(e->alloc_expr.size_expr);
            int32_t t = slot_alloc(16, 8);
            ins("movq %%rax, %s", fp(t + 8));
            if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
            int32_t bytes = slot_alloc(8, 8);
            ins("movq %%rax, %s", fp(bytes));
            dyn_alloca_rax();
            ins("movq %%rax, %s", fp(t));
            ins("movq %%rax, %%rdi");
            ins("movq %s, %%rcx", fp(bytes));
            ins("xorl %%eax, %%eax");
            ins("rep stosb");
            ins("leaq %s, %%rax", fp(t));
        } else if (init && init->kind == EXPR_INTERP_STRING) {
            gen_interp(init, NULL);
        } else if (init && init->kind == EXPR_ARRAY_LIT) {
            gen_array_lit(init, true);
        } else {
            gen_expr(init);
        }
        return;
    }
    if (at && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
        gen_expr(e->alloc_expr.size_expr);
        alloc_n_rdi();
        ins("movq $%lld, %%rsi", (long long)ty_size(at));
        call_c("calloc");
        return;
    }
    if (at && e->alloc_expr.size_expr) {
        gen_expr(e->alloc_expr.size_expr);
        int32_t ls = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(ls));
        alloc_n_rdi();
        ins("movq $%lld, %%rsi", (long long)ty_size(at));
        call_c("calloc");
        some_slice_or_none(conc(e->type), ls);
        return;
    }
    if (at) {
        ins("movl $1, %%edi");
        ins("movq $%lld, %%rsi", (long long)ty_size(at));
        call_c("calloc");
        return;
    }
    if (init->kind == EXPR_STRING_LIT) {
        int n;
        unsigned char *b = decode_lit(init->string_lit.value, init->string_lit.length, &n);
        int sl = str_label(b, n);
        free(b);
        int32_t ls = slot_alloc(8, 8);
        ins("movq $%d, %s", n, fp(ls));
        ins("movl $%d, %%edi", n > 0 ? n : 1);
        call_c("malloc");
        int lnull = new_label();
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        ins("movq %%rax, %%rdi");
        ins("leaq .Lq%d(%%rip), %%rsi", sl);
        ins("movl $%d, %%edx", n);
        call_c("memcpy");
        label(lnull);
        some_slice_or_none(conc(e->type), ls);
        return;
    }
    if (init->kind == EXPR_CSTRING_LIT) {
        int n;
        unsigned char *b = decode_lit(init->cstring_lit.value, init->cstring_lit.length, &n);
        int sl = str_label(b, n);
        free(b);
        ins("movl $%d, %%edi", n + 1);
        call_c("malloc");
        int lnull = new_label();
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        ins("movq %%rax, %%rdi");
        ins("leaq .Lq%d(%%rip), %%rsi", sl);
        ins("movl $%d, %%edx", n + 1);
        call_c("memcpy");
        label(lnull);
        return;
    }
    if (init->kind == EXPR_CAST && is_cstr_type(conc(init->type))) {
        gen_expr(init->cast.operand);
        int32_t s = slot_alloc(16, 8);
        copy_mem("%rbp", s, "%rax", 0, 16);
        ins("movq %s, %%rdi", fp(s + 8));
        ins("incq %%rdi");
        call_c("malloc");
        int lnull = new_label();
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        int32_t p = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(p));
        ins("movq %%rax, %%rdi");
        ins("movq %s, %%rsi", fp(s));
        ins("movq %s, %%rdx", fp(s + 8));
        call_c("memcpy");
        ins("movq %s, %%rcx", fp(s + 8));
        ins("movb $0, (%%rax,%%rcx)");
        ins("movq %s, %%rax", fp(p));
        label(lnull);
        return;
    }
    if (init->kind == EXPR_INTERP_STRING) { gen_interp(init, conc(e->type)); return; }
    if (init->kind == EXPR_ARRAY_LIT) {
        Type *elem = conc(init->array_lit.elem_type);
        int64_t es = ty_size(elem);
        int64_t N = init->array_lit.size_expr->int_lit.value;
        int ec = init->array_lit.elem_count;
        int32_t ls = slot_alloc(8, 8);
        ins("movq $%lld, %s", (long long)N, fp(ls));
        if (ec == 0) {
            ins("movq $%lld, %%rdi", (long long)(N > 0 ? N : 1));
            ins("movq $%lld, %%rsi", (long long)es);
            call_c("calloc");
        } else {
            ins("movq $%lld, %%rdi", (long long)(N * es > 0 ? N * es : 1));
            call_c("malloc");
        }
        int32_t p = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(p));
        int lnull = new_label();
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        for (int i = 0; i < ec; i++) {
            gen_expr(init->array_lit.elems[i]);
            ins("movq %s, %%rdx", fp(p));
            store_val(elem, "%rdx", i * es);
        }
        label(lnull);
        ins("movq %s, %%rax", fp(p));
        some_slice_or_none(conc(e->type), ls);
        return;
    }
    if (init->kind == EXPR_FUNC || e->alloc_expr.closure_src) {
        bool literal = init->kind == EXPR_FUNC;
        Expr *lam = literal ? init : e->alloc_expr.closure_src;
        int64_t al;
        int64_t size = ctx_layout(lam, NULL, &al);
        Type *opt = conc(e->type);
        int32_t src = 0;
        if (!literal) {
            gen_expr(init);
            src = slot_alloc(16, 8);
            copy_mem("%rbp", src, "%rax", 0, 16);
        }
        ins("movq $%lld, %%rdi", (long long)(size > 0 ? size : 1));
        call_c("malloc");
        int32_t p = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(p));
        int32_t t = temp_alloc(opt);
        zero_mem("%rbp", t, ty_size(opt));
        int lnull = new_label();
        ins("movq %s, %%rax", fp(p));
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        if (literal) {
            fill_captures(lam, "%rdx", p);
            ins("leaq %s(%%rip), %%rax", lambda_c_name(lam));
        } else {
            ins("movq %s, %%rdi", fp(p));
            ins("movq %s, %%rsi", fp(src + 8));
            ins("movq $%lld, %%rdx", (long long)size);
            call_c("memcpy");
            ins("movq %s, %%rax", fp(src));
        }
        ins("movq %%rax, %s", fp(t));
        ins("movq %s, %%rax", fp(p));
        ins("movq %%rax, %s", fp(t + 8));
        ins("movb $1, %s", fp(t + 16));
        label(lnull);
        ins("leaq %s, %%rax", fp(t));
        return;
    }
    Type *it = conc(init->type);
    if (init->kind == EXPR_STRUCT_LIT || it->kind == TYPE_UNION) {
        int64_t sz = ty_size(it);
        ins("movq $%lld, %%rdi", (long long)(sz > 0 ? sz : 1));
        call_c("malloc");
        int32_t p = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(p));
        int lnull = new_label();
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        gen_expr(init);
        ins("movq %s, %%rdx", fp(p));
        store_val(it, "%rdx", 0);
        label(lnull);
        ins("movq %s, %%rax", fp(p));
        return;
    }
    if (it->kind == TYPE_SLICE) {
        int64_t es = ty_size(it->slice.elem);
        gen_expr(init);
        int32_t s = slot_alloc(16, 8);
        copy_mem("%rbp", s, "%rax", 0, 16);
        ins("movq %s, %%rax", fp(s + 8));
        alloc_n_rdi();
        if (es != 1) ins("imulq $%lld, %%rdi, %%rdi", (long long)es);
        call_c("malloc");
        int lnull = new_label();
        int32_t p = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(p));
        ins("testq %%rax, %%rax");
        ins("jz .Lq%d", lnull);
        ins("movq %%rax, %%rdi");
        ins("movq %s, %%rsi", fp(s));
        ins("movq %s, %%rdx", fp(s + 8));
        if (es != 1) ins("imulq $%lld, %%rdx, %%rdx", (long long)es);
        call_c("memcpy");
        label(lnull);
        ins("movq %s, %%rax", fp(p));
        some_slice_or_none(conc(e->type), s + 8);
        return;
    }
    internal_error(e->loc, "alloc form not supported");
}

static void gen_free(Expr *e) {
    Type *ot = conc(e->free_expr.operand->type);
    gen_expr(e->free_expr.operand);
    if (ot->kind == TYPE_SLICE) ins("movq (%%rax), %%rdi");
    else if (ot->kind == TYPE_FUNC) ins("movq 8(%%rax), %%rdi");
    else ins("movq %%rax, %%rdi");
    call_c("free");
}

/* ================================================================
 * String interpolation (snprintf, as the emitted C does)
 * ================================================================ */

static void fmt_put_byte(Buf *b, unsigned char c) {
    if (c == '%') buf_put(b, "%%", 2);
    else buf_put(b, (const char *)&c, 1);
}

static void gen_interp(Expr *e, Type *alloc_opt) {
    bool heap = alloc_opt != NULL;
    bool is_cstr = e->interp_string.is_cstr;
    int nseg = e->interp_string.segment_count;
    InterpSegment *segs = e->interp_string.segments;
    int32_t *sslot = xcalloc((size_t)(nseg ? nseg : 1), sizeof(int32_t));
    Type **stype = xcalloc((size_t)(nseg ? nseg : 1), sizeof(Type*));

    /* Each interpolated expression once, in source order. */
    for (int i = 0; i < nseg; i++) {
        if (segs[i].is_literal || segs[i].conversion == 'T') continue;
        Type *t = conc(segs[i].expr->type);
        stype[i] = t;
        gen_expr(segs[i].expr);
        if (val_kind(t) == VK_AGG) {
            sslot[i] = temp_alloc(t);
            copy_mem("%rbp", sslot[i], "%rax", 0, ty_size(t));
        } else {
            sslot[i] = slot_alloc(8, 8);
            ins(val_kind(t) == VK_FLOAT ? "movq %%xmm0, %s" : "movq %%rax, %s", fp(sslot[i]));
        }
    }

    if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
        for (int i = 0; i < nseg; i++) {
            if (segs[i].is_literal || segs[i].conversion == 'T') continue;
            int prec = interp_seg_trunc_prec(&segs[i]);
            if (prec < 0) continue;
            char *rest = str_sprintf("string truncation in %%%%.%ds segment: len=%%lld max=%%lld\n", prec);
            int m = msg_label(segs[i].expr->loc, rest);
            free(rest);
            if (is_str_type(stype[i])) {
                int l = fail_begin();
                ins("movq $%d, %%rcx", prec);
                fail_end(m);
                ins("movq %s, %%rdx", fp(sslot[i] + 8));
                ins("cmpq $%d, %%rdx", prec);
                ins("jg .Lq%d", l);
            } else {
                int lok = new_label();
                ins("movq %s, %%rdi", fp(sslot[i]));
                ins("xorl %%esi, %%esi");
                ins("movl $%d, %%edx", prec + 1);
                call_c("memchr");
                ins("testq %%rax, %%rax");
                ins("jnz .Lq%d", lok);
                ins("movq %s, %%rdi", fp(sslot[i]));
                call_c("strlen");
                int l = fail_begin();
                ins("movq %%rax, %%rdx");
                ins("movq $%d, %%rcx", prec);
                fail_end(m);
                ins("jmp .Lq%d", l);
                label(lok);
            }
        }
    }

    /* The byte budget. */
    int32_t flen = slot_alloc(8, 8);
    int64_t konst = 0;
    ins("movq $0, %s", fp(flen));
    for (int i = 0; i < nseg; i++) {
        InterpSegment *seg = &segs[i];
        Type *t = seg->is_literal ? NULL : instance_type(seg->expr->type);
        InterpSegBudget b = interp_seg_budget(seg, t);
        if (!b.runtime) { konst += b.bytes; continue; }
        if (is_str_type(stype[i])) ins("movq %s, %%rax", fp(sslot[i] + 8));
        else { ins("movq %s, %%rdi", fp(sslot[i])); call_c("strlen"); }
        if (b.bytes > 0) {
            ins("movq $%lld, %%rcx", (long long)b.bytes);
            ins("cmpq %%rcx, %%rax");
            ins("cmovl %%rcx, %%rax");
        }
        ins("addq %%rax, %s", fp(flen));
    }
    if (konst) ins("addq $%lld, %s", (long long)konst, fp(flen));

    /* The buffer. */
    int32_t buf = slot_alloc(8, 8);
    int64_t bsize = 0;
    bool fixed = !heap && interp_const_buffer_size(e, instance_type, &bsize) &&
                 !e->interp_string.wrapped;
    if (heap) {
        ins("movq %s, %%rdi", fp(flen));
        ins("incq %%rdi");
        call_c("malloc");
    } else if (fixed) {
        int32_t back = slot_alloc(align_up(bsize + 1, 8), 8);
        ins("leaq %s, %%rax", fp(back));
    } else {
        ins("movq %s, %%rax", fp(flen));
        ins("incq %%rax");
        dyn_alloca_rax();
    }
    ins("movq %%rax, %s", fp(buf));

    /* The format string and arguments. */
    Buf fmt = { 0 };
    CallArg *args = xcalloc((size_t)(3 + 2 * nseg + 64), sizeof(CallArg));
    int na = 0, acap = 3 + 2 * nseg + 64;
    args[na].type = type_any_ptr(); args[na++].slot = buf;
    int32_t size_slot = slot_alloc(8, 8);
    ins("movq %s, %%rax", fp(flen));
    ins("incq %%rax");
    ins("movq %%rax, %s", fp(size_slot));
    args[na].type = type_int64(); args[na++].slot = size_slot;
    int32_t fmt_slot = slot_alloc(8, 8);
    args[na].type = type_any_ptr(); args[na++].slot = fmt_slot;
    for (int i = 0; i < nseg; i++) {
        InterpSegment *seg = &segs[i];
        if (seg->is_literal) {
            int n;
            unsigned char *bytes = decode_lit(seg->text, seg->text_length, &n);
            for (int j = 0; j < n; j++) {
                if (bytes[j] == 0) {
                    buf_puts(&fmt, "%c");
                    if (na >= acap) { acap *= 2; args = xrealloc(args, sizeof(CallArg) * (size_t)acap); }
                    args[na].type = type_int32(); args[na].zero = true; args[na].slot = 0; na++;
                } else fmt_put_byte(&fmt, bytes[j]);
            }
            free(bytes);
            continue;
        }
        if (seg->conversion == 'T') {
            const char *tn = type_name(instance_type(seg->expr->type));
            for (const char *p = tn; *p; p++) fmt_put_byte(&fmt, (unsigned char)*p);
            continue;
        }
        if (na + 2 >= acap) { acap *= 2; args = xrealloc(args, sizeof(CallArg) * (size_t)acap); }
        Type *t = stype[i];
        char conv = seg->conversion;
        if (conv == 's' && is_str_type(t)) {
            InterpSpec sp;
            interp_seg_spec(seg, &sp);
            buf_printf(&fmt, "%%%.*s.*s", sp.prec_at, seg->text);
            int32_t ls = slot_alloc(8, 8);
            ins("movq %s, %%rax", fp(sslot[i] + 8));
            if (sp.precision >= 0) {
                ins("movq $%lld, %%rcx", (long long)sp.precision);
                ins("cmpq %%rcx, %%rax");
                ins("cmovg %%rcx, %%rax");
            }
            ins("movq %%rax, %s", fp(ls));
            args[na].type = type_int32(); args[na++].slot = ls;
            int32_t ps = slot_alloc(8, 8);
            ins("movq %s, %%rax", fp(sslot[i]));
            ins("movq %%rax, %s", fp(ps));
            args[na].type = type_any_ptr(); args[na++].slot = ps;
            continue;
        }
        bool int_number = type_is_integer(t) &&
            (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o');
        if (int_number) {
            buf_put(&fmt, "%", 1);
            buf_put(&fmt, seg->text, (size_t)seg->text_length - 1);
            char cc = seg->text[seg->text_length - 1];
            if ((cc == 'd' || cc == 'i') && interp_conv_is_unsigned(cc, t)) cc = 'u';
            buf_printf(&fmt, "ll%c", cc);
        } else {
            buf_put(&fmt, "%", 1);
            buf_put(&fmt, seg->text, (size_t)seg->text_length);
        }
        int32_t as = slot_alloc(8, 8);
        if (val_kind(t) == VK_FLOAT) {
            ins("movq %s, %%xmm0", fp(sslot[i]));
            if (is_f32(t)) ins("cvtss2sd %%xmm0, %%xmm0");
            ins("movq %%xmm0, %s", fp(as));
            args[na].type = type_float64(); args[na++].slot = as;
            continue;
        }
        ins("movq %s, %%rax", fp(sslot[i]));
        if (type_is_integer(t) && interp_conv_is_unsigned(conv, t) && int_signed(t)) {
            int w = int_width(t);
            if (w == 1) ins("movzbl %%al, %%eax");
            else if (w == 2) ins("movzwl %%ax, %%eax");
            else if (w == 4) ins("movl %%eax, %%eax");
        }
        ins("movq %%rax, %s", fp(as));
        args[na].type = type_int64(); args[na++].slot = as;
    }
    int fl = str_label((const unsigned char *)(fmt.p ? fmt.p : ""), (int)fmt.len);
    buf_free(&fmt);
    ins("leaq .Lq%d(%%rip), %%rax", fl);
    ins("movq %%rax, %s", fp(fmt_slot));

    int32_t fw = slot_alloc(8, 8);
    int lskip = new_label();
    if (heap) {
        ins("movq $0, %s", fp(fw));
        ins("cmpq $0, %s", fp(buf));
        ins("je .Lq%d", lskip);
    }
    CallSpec cs = { 0 };
    cs.sym = "snprintf";
    cs.plt = true;
    cs.variadic = true;
    cs.ret = type_int32();
    emit_call(&cs, args, na);
    ins("movq %%rax, %s", fp(fw));
    label(lskip);
    free(args);
    free(sslot);
    free(stype);

    if (is_cstr) { ins("movq %s, %%rax", fp(buf)); return; }
    /* len = (fw >= 0 && fw <= flen) ? fw : flen */
    ins("movq %s, %%rax", fp(fw));
    ins("movq %s, %%rcx", fp(flen));
    int lk = new_label(), luse = new_label();
    ins("testq %%rax, %%rax");
    ins("js .Lq%d", luse);
    ins("cmpq %%rcx, %%rax");
    ins("jle .Lq%d", lk);
    label(luse);
    ins("movq %%rcx, %%rax");
    label(lk);
    if (heap) {
        int32_t ls = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(ls));
        ins("movq %s, %%rax", fp(buf));
        some_slice_or_none(alloc_opt, ls);
        return;
    }
    int32_t t = slot_alloc(16, 8);
    ins("movq %%rax, %s", fp(t + 8));
    ins("movq %s, %%rax", fp(buf));
    ins("movq %%rax, %s", fp(t));
    ins("leaq %s, %%rax", fp(t));
}

/* ================================================================
 * Assignment
 * ================================================================ */

static void gen_assign(Expr *e) {
    Expr *target = e->assign.target;
    if ((target->kind == EXPR_FIELD || target->kind == EXPR_DEREF_FIELD) &&
        target->field.fixed_array_type) {
        Type *fat = target->field.fixed_array_type;
        int64_t cap = fixarr_size(fat), es = ty_size(fat->fixed_array.elem);
        gen_addr(target);
        int32_t dst = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(dst));
        gen_expr(e->assign.value);
        int32_t src = slot_alloc(16, 8);
        copy_mem("%rbp", src, "%rax", 0, 16);
        char *rest = str_sprintf("fixed-array field '%s' overflow: len=%%lld capacity=%lld\n",
                                 target->field.name, (long long)cap);
        int m = msg_label(e->loc, rest);
        free(rest);
        int l = fail_begin();
        fail_end(m);
        ins("movq %s, %%rdx", fp(src + 8));
        ins("cmpq $%lld, %%rdx", (long long)cap);
        ins("jg .Lq%d", l);
        /* zero the whole field, then copy the source over it */
        ins("movq %s, %%rdi", fp(dst));
        ins("movq $%lld, %%rcx", (long long)(cap * es));
        ins("xorl %%eax, %%eax");
        ins("rep stosb");
        ins("movq %s, %%rdi", fp(dst));
        ins("movq %s, %%rsi", fp(src));
        ins("movq %s, %%rcx", fp(src + 8));
        if (es != 1) ins("imulq $%lld, %%rcx, %%rcx", (long long)es);
        ins("rep movsb");
        return;
    }
    if ((target->kind == EXPR_FIELD && !target->field.codegen_name) || target->kind == EXPR_DEREF_FIELD) {
        Type *st = conc(target->field.object->type);
        if (target->kind == EXPR_DEREF_FIELD) st = conc(st->pointer.pointee);
        int64_t off; Type *ft; ExtField *x;
        if (field_info_x(st, target->field.name, &off, &ft, &x) && x) {
            gen_expr(target->field.object);
            int32_t s = spill_push();
            ins("movq %%rax, %s", fp(s));
            gen_expr(e->assign.value);
            ins("movq %s, %%rdx", fp(s));
            spill_pop();
            store_ext_field(x, "%rdx", 0, off);
            return;
        }
    }
    gen_addr(target);
    int32_t s = spill_push();
    ins("movq %%rax, %s", fp(s));
    gen_expr(e->assign.value);
    ins("movq %s, %%rdx", fp(s));
    spill_pop();
    store_val(target->type, "%rdx", 0);
}

/* ================================================================
 * Patterns
 * ================================================================ */

static bool pattern_has_predicate(Pattern *pat) {
    switch (pat->kind) {
    case PAT_WILDCARD: case PAT_BINDING: case PAT_ERROR:
        return false;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            if (pattern_has_predicate(pat->struc.fields[i].pattern)) return true;
        return false;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            if (pattern_has_predicate(pat->tuple_pat.patterns[i])) return true;
        return false;
    case PAT_OR:
        for (int i = 0; i < pat->or_pat.alt_count; i++)
            if (!pattern_has_predicate(pat->or_pat.alts[i])) return false;
        return true;
    default:
        return true;
    }
}

static void cmp_rax_imm(int64_t v) {
    if (v >= INT32_MIN && v <= INT32_MAX) ins("cmpq $%lld, %%rax", (long long)v);
    else { mov_imm_reg(v, "%rcx"); ins("cmpq %%rcx, %%rax"); }
}

static Type *tuple_elem_type(Pattern *pat, Type *t, int i, int64_t *off) {
    Layout *L = agg_layout(t);
    *off = i < L->n ? L->off[i] : 0;
    return i < L->n ? L->ftype[i] : pat->tuple_pat.resolved_types[i];
}

static void gen_pat_test(Pattern *pat, int32_t off, Type *t, int fail) {
    t = conc(t);
    switch (pat->kind) {
    case PAT_WILDCARD: case PAT_BINDING: case PAT_ERROR: case PAT_CONST_PATH:
        return;
    case PAT_INT_LIT:
        load_int(t, fp(off));
        cmp_rax_imm(canon_const(pat->int_lit.value, t));
        ins("jne .Lq%d", fail);
        return;
    case PAT_CHAR_LIT:
        load_int(t, fp(off));
        cmp_rax_imm(pat->char_lit.value);
        ins("jne .Lq%d", fail);
        return;
    case PAT_BOOL_LIT:
        load_int(t, fp(off));
        cmp_rax_imm(pat->bool_lit.value ? 1 : 0);
        ins("jne .Lq%d", fail);
        return;
    case PAT_STRING_LIT: {
        int n;
        unsigned char *b = decode_lit(pat->string_lit.value, pat->string_lit.length, &n);
        int sl = str_label(b, n);
        free(b);
        ins("cmpq $%d, %s", n, fp(off + 8));
        ins("jne .Lq%d", fail);
        if (n > 0) {
            ins("movq %s, %%rdi", fp(off));
            ins("leaq .Lq%d(%%rip), %%rsi", sl);
            ins("movl $%d, %%edx", n);
            call_c("memcmp");
            ins("testl %%eax, %%eax");
            ins("jne .Lq%d", fail);
        }
        return;
    }
    case PAT_SOME:
        if (is_null_sentinel(t)) {
            ins("cmpq $0, %s", fp(off));
            ins("je .Lq%d", fail);
        } else {
            ins("cmpb $0, %s", fp(off + (int32_t)ty_size(t->option.inner)));
            ins("je .Lq%d", fail);
        }
        if (pat->some_pat.inner) gen_pat_test(pat->some_pat.inner, off, t->option.inner, fail);
        return;
    case PAT_NONE:
        if (is_null_sentinel(t)) ins("cmpq $0, %s", fp(off));
        else ins("cmpb $0, %s", fp(off + (int32_t)ty_size(t->option.inner)));
        ins("jne .Lq%d", fail);
        return;
    case PAT_OK:
        ins("cmpl $0, %s", fp(off));
        ins("jne .Lq%d", fail);
        if (pat->some_pat.inner)
            gen_pat_test(pat->some_pat.inner, off + (int32_t)result_value_off(t), t->result.inner, fail);
        return;
    case PAT_ERR:
        ins("cmpl $0, %s", fp(off));
        ins("je .Lq%d", fail);
        if (pat->some_pat.inner) gen_pat_test(pat->some_pat.inner, off, type_int32(), fail);
        return;
    case PAT_VARIANT:
        if (t->kind == TYPE_ENUM) {
            load_int(t, fp(off));
            cmp_rax_imm(enum_variant_value(t, pat->variant.variant));
            ins("jne .Lq%d", fail);
            return;
        }
        ins("cmpl $%d, %s", union_variant_index(t, pat->variant.variant), fp(off));
        ins("jne .Lq%d", fail);
        if (pat->variant.payload) {
            Type *pt = union_variant_payload(t, pat->variant.variant);
            if (pt) gen_pat_test(pat->variant.payload, off + (int32_t)agg_layout(t)->payload_off, pt, fail);
        }
        return;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++) {
            int64_t fo; Type *ft; ExtField *x;
            if (!field_info_x(t, pat->struc.fields[i].name, &fo, &ft, &x)) continue;
            if (x) {
                if (!pattern_has_predicate(pat->struc.fields[i].pattern)) continue;
                load_ext_field(ft, x, "%rbp", off, off + fo);
                int32_t v = slot_alloc(8, 8);
                ins("movq %%rax, %s", fp(v));
                gen_pat_test(pat->struc.fields[i].pattern, v, ft, fail);
                continue;
            }
            gen_pat_test(pat->struc.fields[i].pattern, off + (int32_t)fo, ft, fail);
        }
        return;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            int64_t fo;
            Type *ft = tuple_elem_type(pat, t, i, &fo);
            gen_pat_test(pat->tuple_pat.patterns[i], off + (int32_t)fo, ft, fail);
        }
        return;
    case PAT_OR: {
        int ok = new_label();
        for (int i = 0; i < pat->or_pat.alt_count; i++) {
            if (!pattern_has_predicate(pat->or_pat.alts[i])) { ins("jmp .Lq%d", ok); break; }
            if (i == pat->or_pat.alt_count - 1) {
                gen_pat_test(pat->or_pat.alts[i], off, t, fail);
            } else {
                int nf = new_label();
                gen_pat_test(pat->or_pat.alts[i], off, t, nf);
                ins("jmp .Lq%d", ok);
                label(nf);
            }
        }
        label(ok);
        return;
    }
    }
}

static void gen_pat_bind(Pattern *pat, int32_t off, Type *t) {
    t = conc(t);
    switch (pat->kind) {
    case PAT_BINDING: {
        const char *nm = pat->binding.codegen_name ? pat->binding.codegen_name : pat->binding.name;
        int32_t s = local_slot(nm, t);
        if (val_kind(t) == VK_AGG) copy_mem("%rbp", s, "%rbp", off, ty_size(t));
        else { load_val(t, fp(off)); store_val(t, "%rbp", s); }
        return;
    }
    case PAT_SOME:
        if (pat->some_pat.inner) gen_pat_bind(pat->some_pat.inner, off, t->option.inner);
        return;
    case PAT_OK:
        if (pat->some_pat.inner)
            gen_pat_bind(pat->some_pat.inner, off + (int32_t)result_value_off(t), t->result.inner);
        return;
    case PAT_ERR:
        if (pat->some_pat.inner) gen_pat_bind(pat->some_pat.inner, off, type_int32());
        return;
    case PAT_VARIANT:
        if (pat->variant.payload && t->kind == TYPE_UNION) {
            Type *pt = union_variant_payload(t, pat->variant.variant);
            if (pt) gen_pat_bind(pat->variant.payload, off + (int32_t)agg_layout(t)->payload_off, pt);
        }
        return;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++) {
            int64_t fo; Type *ft; ExtField *x;
            if (!field_info_x(t, pat->struc.fields[i].name, &fo, &ft, &x)) continue;
            if (x) {
                load_ext_field(ft, x, "%rbp", off, off + fo);
                int32_t v = slot_alloc(8, 8);
                ins("movq %%rax, %s", fp(v));
                gen_pat_bind(pat->struc.fields[i].pattern, v, ft);
                continue;
            }
            gen_pat_bind(pat->struc.fields[i].pattern, off + (int32_t)fo, ft);
        }
        return;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            int64_t fo;
            Type *ft = tuple_elem_type(pat, t, i, &fo);
            gen_pat_bind(pat->tuple_pat.patterns[i], off + (int32_t)fo, ft);
        }
        return;
    default:
        return;
    }
}

/* Put the current value in a frame slot of its own (a match subject, a
 * destructured value): returns the slot. */
static int32_t value_to_slot(Type *t) {
    t = conc(t);
    switch (val_kind(t)) {
    case VK_AGG: {
        int32_t s = temp_alloc(t);
        copy_mem("%rbp", s, "%rax", 0, ty_size(t));
        return s;
    }
    case VK_FLOAT: { int32_t s = slot_alloc(8, 8); ins("movq %%xmm0, %s", fp(s)); return s; }
    case VK_INT: { int32_t s = slot_alloc(8, 8); ins("movq %%rax, %s", fp(s)); return s; }
    case VK_VOID: return slot_alloc(8, 8);
    }
    return 0;
}

/* ================================================================
 * Control flow
 * ================================================================ */

static void cond_jump_false(Expr *cond, int target) {
    gen_expr(cond);
    ins("testl %%eax, %%eax");
    ins("jz .Lq%d", target);
}

/* A branch body (one expression or a block) in its own defer scope. With
 * TAIL_VALUE its value is left in the value registers. */
static void gen_scoped_branch(Expr *branch, int tail) {
    Expr **stmts = &branch;
    int count = 1;
    if (branch->kind == EXPR_BLOCK) { stmts = branch->block.stmts; count = branch->block.count; }
    defer_push(false);
    bool done = gen_stmts(stmts, count, tail);
    if (!done) emit_scope_defers(g_defer);
    defer_pop();
}

static void gen_if(Expr *e) {
    bool valued = !type_valueless(e->type);
    int lelse = new_label(), lend = new_label();
    cond_jump_false(e->if_expr.cond, lelse);
    gen_scoped_branch(e->if_expr.then_body,
                      valued && !type_is_never(e->if_expr.then_body->type) ? TAIL_VALUE : TAIL_DISCARD);
    ins("jmp .Lq%d", lend);
    label(lelse);
    if (e->if_expr.else_body)
        gen_scoped_branch(e->if_expr.else_body,
                          valued && !type_is_never(e->if_expr.else_body->type) ? TAIL_VALUE : TAIL_DISCARD);
    label(lend);
}

static void gen_match(Expr *e, bool tail) {
    Type *st = conc(e->match_expr.subject->type);
    gen_expr(e->match_expr.subject);
    int32_t sub = value_to_slot(st);
    bool valued = !type_valueless(e->type);
    bool any_guard = false;
    for (int i = 0; i < e->match_expr.arm_count; i++)
        if (e->match_expr.arms[i].guard) any_guard = true;
    int lend = new_label();
    bool exhausted = false;
    for (int i = 0; i < e->match_expr.arm_count; i++) {
        MatchArm *arm = &e->match_expr.arms[i];
        bool last = i == e->match_expr.arm_count - 1;
        int next = new_label();
        bool test = any_guard || !last;
        if (test && pattern_has_predicate(arm->pattern)) gen_pat_test(arm->pattern, sub, st, next);
        gen_pat_bind(arm->pattern, sub, st);
        if (arm->guard) cond_jump_false(arm->guard, next);
        defer_push(false);
        int mode = tail ? TAIL_RETURN : (valued ? TAIL_VALUE : TAIL_DISCARD);
        bool done = gen_stmts(arm->body, arm->body_count, mode);
        if (!done) {
            emit_scope_defers(g_defer);
            if (tail) ins("jmp .Lq%d", g_ret_label);
        }
        defer_pop();
        ins("jmp .Lq%d", lend);
        label(next);
        if (!arm->guard && !pattern_has_predicate(arm->pattern)) { exhausted = true; break; }
        if (!any_guard && last) exhausted = true;
    }
    if (!exhausted) call_c("abort");
    label(lend);
}

static void gen_loop(Expr *e) {
    LoopCtx lc = { new_label(), new_label(), 0, NULL, g_loop };
    bool valued = e->type && conc(e->type)->kind != TYPE_VOID && !type_valueless(e->type);
    if (valued) {
        lc.res_type = conc(e->type);
        lc.res = val_kind(lc.res_type) == VK_AGG ? temp_alloc(lc.res_type) : slot_alloc(8, 8);
    }
    g_loop = &lc;
    label(lc.cont);
    defer_push(true);
    bool done = gen_stmts(e->loop_expr.body, e->loop_expr.body_count, TAIL_DISCARD);
    if (!done) emit_scope_defers(g_defer);
    defer_pop();
    ins("jmp .Lq%d", lc.cont);
    label(lc.brk);
    g_loop = lc.parent;
    if (valued) {
        if (val_kind(lc.res_type) == VK_AGG) ins("leaq %s, %%rax", fp(lc.res));
        else if (val_kind(lc.res_type) == VK_FLOAT) ins("movq %s, %%xmm0", fp(lc.res));
        else ins("movq %s, %%rax", fp(lc.res));
    }
}

static void gen_break(Expr *value) {
    if (!g_loop) { internal_error(value ? value->loc : (SrcLoc){0}, "break outside a loop"); return; }
    if (value) {
        gen_expr(value);
        if (defers_to_loop_pending()) {
            Snap sn = snap_save(value->type);
            emit_defers_to_loop();
            snap_restore(sn);
        }
        if (g_loop->res) {
            if (val_kind(g_loop->res_type) == VK_AGG)
                copy_mem("%rbp", g_loop->res, "%rax", 0, ty_size(g_loop->res_type));
            else if (val_kind(g_loop->res_type) == VK_FLOAT) ins("movq %%xmm0, %s", fp(g_loop->res));
            else ins("movq %%rax, %s", fp(g_loop->res));
        }
    } else {
        emit_defers_to_loop();
    }
    ins("jmp .Lq%d", g_loop->brk);
}

static void gen_continue(void) {
    if (!g_loop) return;
    emit_defers_to_loop();
    ins("jmp .Lq%d", g_loop->cont);
}

static void gen_for(Expr *e) {
    LoopCtx lc = { new_label(), new_label(), 0, NULL, g_loop };
    int ltop = new_label();
    if (e->for_expr.range_end) {
        Type *vt = conc(e->for_expr.iter->type);
        gen_expr(e->for_expr.range_end);
        int32_t end = slot_alloc(8, 8);
        ins("movq %%rax, %s", fp(end));
        const char *vn = e->for_expr.var_codegen_name ? e->for_expr.var_codegen_name : e->for_expr.var;
        int32_t iv = local_slot(vn, vt);
        gen_expr(e->for_expr.iter);
        store_val(vt, "%rbp", iv);
        label(ltop);
        load_int(vt, fp(iv));
        ins("cmpq %s, %%rax", fp(end));
        ins(int_signed(vt) ? "jge .Lq%d" : "jae .Lq%d", lc.brk);
        g_loop = &lc;
        defer_push(true);
        bool done = gen_stmts(e->for_expr.body, e->for_expr.body_count, TAIL_DISCARD);
        if (!done) emit_scope_defers(g_defer);
        defer_pop();
        g_loop = lc.parent;
        label(lc.cont);
        load_int(vt, fp(iv));
        ins("addq $1, %%rax");
        normalize(vt);
        store_val(vt, "%rbp", iv);
        ins("jmp .Lq%d", ltop);
        label(lc.brk);
        return;
    }
    Type *it = conc(e->for_expr.iter->type);
    Type *elem = conc(it->slice.elem);
    int64_t es = ty_size(elem);
    gen_expr(e->for_expr.iter);
    int32_t sl = slot_alloc(16, 8);
    copy_mem("%rbp", sl, "%rax", 0, 16);
    int32_t cnt = slot_alloc(8, 8);
    ins("movq $0, %s", fp(cnt));
    const char *en = e->for_expr.var_pattern ? e->for_expr.elem_tmp
        : (e->for_expr.var_codegen_name ? e->for_expr.var_codegen_name : e->for_expr.var);
    int32_t ev = local_slot(en, elem);
    label(ltop);
    ins("movq %s, %%rax", fp(cnt));
    ins("cmpq %s, %%rax", fp(sl + 8));
    ins("jge .Lq%d", lc.brk);
    if (es != 1) ins("imulq $%lld, %%rax, %%rax", (long long)es);
    ins("addq %s, %%rax", fp(sl));
    load_val(elem, "(%rax)");
    store_val(elem, "%rbp", ev);
    if (e->for_expr.var_pattern) gen_pat_bind(e->for_expr.var_pattern, ev, elem);
    if (e->for_expr.index_var) {
        const char *in = e->for_expr.index_codegen_name ? e->for_expr.index_codegen_name : e->for_expr.index_var;
        int32_t ix = local_slot(in, type_int64());
        ins("movq %s, %%rax", fp(cnt));
        ins("movq %%rax, %s", fp(ix));
    }
    g_loop = &lc;
    defer_push(true);
    bool done = gen_stmts(e->for_expr.body, e->for_expr.body_count, TAIL_DISCARD);
    if (!done) emit_scope_defers(g_defer);
    defer_pop();
    g_loop = lc.parent;
    label(lc.cont);
    ins("incq %s", fp(cnt));
    ins("jmp .Lq%d", ltop);
    label(lc.brk);
}

/* ================================================================
 * Statements
 * ================================================================ */

static void gen_let(Expr *s) {
    Type *t = s->let_expr.let_type ? s->let_expr.let_type : s->let_expr.let_init->type;
    const char *name = s->let_expr.codegen_name ? s->let_expr.codegen_name : s->let_expr.let_name;
    if (type_valueless(conc(t))) { gen_expr(s->let_expr.let_init); return; }
    int32_t off = local_slot(name, conc(t));
    gen_expr(s->let_expr.let_init);
    store_val(t, "%rbp", off);
}

static void gen_let_destruct(Expr *s) {
    Type *t = conc(s->let_destruct.init_type);
    gen_expr(s->let_destruct.init);
    int32_t slot = value_to_slot(t);
    gen_pat_bind(s->let_destruct.pattern, slot, t);
}

/* Whether the last statement of a function body is its result. */
static bool tail_returns(Expr *s) {
    if (!type_valueless(s->type)) return true;
    Type *r = conc(g_fn_ret);
    if (!r || r->kind != TYPE_VOID) return false;
    return s->kind == EXPR_CALL || s->kind == EXPR_IF || s->kind == EXPR_MATCH ||
           s->kind == EXPR_BLOCK || s->kind == EXPR_GUARD;
}

/* Statements of a block in the current defer scope. Returns true when the
 * scope's defers have been emitted (or control cannot reach the end). */
static bool gen_stmts(Expr **stmts, int count, int tail) {
    int value_idx = count > 0 && stmts[count - 1]->kind != EXPR_DEFER ? count - 1 : -1;
    for (int i = 0; i < count; i++) {
        Expr *s = stmts[i];
        bool last = i == count - 1;
        emit_loc(s->loc);
        switch (s->kind) {
        case EXPR_DEFER:
            DA_APPEND(g_defer->defers, g_defer->count, g_defer->cap, s->defer_expr.value);
            continue;
        case EXPR_RETURN:
            gen_return(s->return_expr.value);
            if (last) return true;
            continue;
        case EXPR_BREAK:
            gen_break(s->break_expr.value);
            if (last) return true;
            continue;
        case EXPR_CONTINUE:
            gen_continue();
            if (last) return true;
            continue;
        case EXPR_LET:
            gen_let(s);
            continue;
        case EXPR_LET_DESTRUCT:
            gen_let_destruct(s);
            continue;
        default:
            break;
        }
        if (last && tail == TAIL_RETURN && tail_returns(s)) {
            gen_return(s);
            return true;
        }
        if (last && tail == TAIL_VALUE && i == value_idx && !type_valueless(s->type)) {
            gen_expr(s);
            if (g_defer && g_defer->count > 0) {
                Snap sn = snap_save(s->type);
                emit_scope_defers(g_defer);
                snap_restore(sn);
            }
            return true;
        }
        gen_expr(s);
    }
    return false;
}

static void gen_return(Expr *v) {
    if (v && g_tco_ok && !has_pending_defers()) {
        switch (v->kind) {
        case EXPR_IF:
            if (v->if_expr.else_body || type_valueless(v->type)) {
                int lelse = new_label();
                cond_jump_false(v->if_expr.cond, lelse);
                gen_scoped_branch(v->if_expr.then_body, TAIL_RETURN);
                ins("jmp .Lq%d", g_ret_label);
                label(lelse);
                if (v->if_expr.else_body) gen_scoped_branch(v->if_expr.else_body, TAIL_RETURN);
                ins("jmp .Lq%d", g_ret_label);
                return;
            }
            break;
        case EXPR_MATCH:
            gen_match(v, true);
            ins("jmp .Lq%d", g_ret_label);
            return;
        case EXPR_BLOCK:
            gen_scoped_branch(v, TAIL_RETURN);
            ins("jmp .Lq%d", g_ret_label);
            return;
        case EXPR_CALL:
            if (tail_compatible(v)) {
                if (!gen_call(v, true)) {
                    if (!type_valueless(v->type)) move_to_return_regs();
                    ins("jmp .Lq%d", g_ret_label);
                }
                return;
            }
            break;
        default:
            break;
        }
    }
    if (v) gen_expr(v);
    bool has_value = v && !type_valueless(v->type);
    if (has_pending_defers()) {
        if (has_value) {
            Snap sn = snap_save(v->type);
            emit_defers_to_func();
            snap_restore(sn);
        } else {
            emit_defers_to_func();
        }
    }
    if (has_value) move_to_return_regs();
    ins("jmp .Lq%d", g_ret_label);
}

/* ================================================================
 * The expression dispatcher
 * ================================================================ */

static void gen_assert(Expr *e) {
    int lok = new_label();
    gen_expr(e->assert_expr.condition);
    ins("testl %%eax, %%eax");
    ins("jnz .Lq%d", lok);
    char *text = fmt_escape(e->assert_expr.expr_text ? e->assert_expr.expr_text : "",
                            e->assert_expr.expr_text ? e->assert_expr.expr_text_len : 0);
    if (e->assert_expr.message) {
        gen_expr(e->assert_expr.message);
        char *rest = str_sprintf("assertion failed: %s: %%.*s\n", text);
        int m = msg_label(e->loc, rest);
        free(rest);
        ins("movq 8(%%rax), %%rdx");
        ins("movq (%%rax), %%rcx");
        ins("movq stderr@GOTPCREL(%%rip), %%rdi");
        ins("movq (%%rdi), %%rdi");
        ins("leaq .Lq%d(%%rip), %%rsi", m);
        ins("xorl %%eax, %%eax");
        call_c("fprintf");
        call_c("abort");
    } else {
        char *rest = str_sprintf("assertion failed: %s\n", text);
        int m = msg_label(e->loc, rest);
        free(rest);
        ins("movq stderr@GOTPCREL(%%rip), %%rdi");
        ins("movq (%%rdi), %%rdi");
        ins("leaq .Lq%d(%%rip), %%rsi", m);
        ins("xorl %%eax, %%eax");
        call_c("fprintf");
        call_c("abort");
    }
    free(text);
    label(lok);
}

static void gen_enum_of(Expr *e) {
    Type *et = conc(e->enum_of_expr.target);
    Type *ot = conc(e->enum_of_expr.operand->type);
    Type *opt = conc(e->type);
    Type *repr = et->enu.repr ? et->enu.repr : type_int32();
    bool u64_repr = repr->kind == TYPE_UINT64;
    bool op_uwide = ot->kind == TYPE_UINT64 || ot->kind == TYPE_USIZE;
    gen_expr(e->enum_of_expr.operand);
    int32_t t = temp_alloc(opt);
    int32_t vs = slot_alloc(8, 8);
    ins("movq %%rax, %s", fp(vs));
    zero_mem("%rbp", t, ty_size(opt));
    ins("movq %s, %%rax", fp(vs));
    int lnone = new_label(), lfound = new_label();
    if (!u64_repr && op_uwide) { ins("testq %%rax, %%rax"); ins("js .Lq%d", lnone); }
    if (u64_repr && !op_uwide) { ins("testq %%rax, %%rax"); ins("js .Lq%d", lnone); }
    for (int i = 0; i < et->enu.variant_count; i++) {
        uint64_t bits = et->enu.variants[i].value_bits;
        int64_t v = u64_repr ? (int64_t)bits : canon_const(bits, repr);
        cmp_rax_imm(v);
        ins("je .Lq%d", lfound);
    }
    ins("jmp .Lq%d", lnone);
    label(lfound);
    store_int(et, fp(t));
    ins("movb $1, %s", fp(t + (int32_t)ty_size(et)));
    label(lnone);
    ins("leaq %s, %%rax", fp(t));
}

static int g_errnames_label = 0;

static void gen_error_name(Expr *e) {
    Type *opt = conc(e->type);
    int n = error_code_count();
    if (!g_errnames_label) {
        g_errnames_label = new_label();
        int *labels = xcalloc((size_t)(n ? n : 1), sizeof(int));
        for (int i = 0; i < n; i++) labels[i] = cstr_label(error_code_info(i).qualified);
        buf_printf(&g_relro, "\t.p2align 3\n.Lq%d:\n", g_errnames_label);
        for (int i = 0; i < n; i++)
            buf_printf(&g_relro, "\t.quad .Lq%d\n\t.quad %d\n", labels[i],
                       (int)strlen(error_code_info(i).qualified));
        if (n == 0) buf_puts(&g_relro, "\t.quad 0\n");
        free(labels);
    }
    gen_expr(e->error_name_expr.code);
    int32_t t = temp_alloc(opt);
    int32_t cs = slot_alloc(8, 8);
    ins("movq %%rax, %s", fp(cs));
    zero_mem("%rbp", t, ty_size(opt));
    int lnone = new_label();
    ins("movq %s, %%rax", fp(cs));
    ins("subq $%d, %%rax", FC_ERROR_CODE_BASE);
    ins("cmpq $%d, %%rax", n);
    ins("jae .Lq%d", lnone);
    ins("shlq $4, %%rax");
    ins("leaq .Lq%d(%%rip), %%rcx", g_errnames_label);
    ins("addq %%rcx, %%rax");
    ins("movq (%%rax), %%rcx");
    ins("movq %%rcx, %s", fp(t));
    ins("movq 8(%%rax), %%rcx");
    ins("movq %%rcx, %s", fp(t + 8));
    ins("movb $1, %s", fp(t + 16));
    label(lnone);
    ins("leaq %s, %%rax", fp(t));
}

static void gen_bitcast(Expr *e) {
    Type *from = conc(e->bitcast_expr.operand->type), *to = conc(e->bitcast_expr.target);
    gen_expr(e->bitcast_expr.operand);
    ValKind fk = val_kind(from), tk = val_kind(to);
    if (fk == VK_FLOAT && tk == VK_INT) {
        ins(is_f32(from) ? "movd %%xmm0, %%eax" : "movq %%xmm0, %%rax");
        normalize(to);
    } else if (fk == VK_INT && tk == VK_FLOAT) {
        ins(is_f32(to) ? "movd %%eax, %%xmm0" : "movq %%rax, %%xmm0");
    } else if (fk == VK_INT && tk == VK_INT) {
        normalize(to);
    }
}

static void gen_expr(Expr *e) {
    switch (e->kind) {
    case EXPR_STATIC_ASSERT: case EXPR_VOID_LIT:
        return;
    case EXPR_TYPE_VAR_REF: {
        Type tv = { 0 };
        tv.kind = TYPE_TYPE_VAR;
        tv.type_var.name = e->type_var_ref.name;
        Type *b = subst_resolve(&tv);
        if (e->type_var_ref.is_const_param && b && b->kind == TYPE_CONST_INT) {
            mov_imm(b->const_int.value);
            return;
        }
        internal_error(e->loc, "unresolved type variable %s", e->type_var_ref.name);
        return;
    }
    case EXPR_INT_LIT:
        mov_imm(canon_const(e->int_lit.value, e->int_lit.lit_type ? e->int_lit.lit_type : e->type));
        return;
    case EXPR_FLOAT_LIT:
        float_const(e->float_lit.value, e->float_lit.lit_type->kind == TYPE_FLOAT32);
        return;
    case EXPR_BOOL_LIT: mov_imm(e->bool_lit.value ? 1 : 0); return;
    case EXPR_CHAR_LIT: mov_imm(e->char_lit.value); return;
    case EXPR_STRING_LIT: {
        int n;
        unsigned char *b = decode_lit(e->string_lit.value, e->string_lit.length, &n);
        ins("leaq .Lq%d(%%rip), %%rax", strhdr_label(b, n));
        free(b);
        return;
    }
    case EXPR_CSTRING_LIT: {
        int n;
        unsigned char *b = decode_lit(e->cstring_lit.value, e->cstring_lit.length, &n);
        ins("leaq .Lq%d(%%rip), %%rax", str_label(b, n));
        free(b);
        return;
    }
    case EXPR_IDENT: gen_ident(e); return;
    case EXPR_BINARY: gen_binary(e); return;
    case EXPR_UNARY_PREFIX: gen_unary_prefix(e); return;
    case EXPR_UNARY_POSTFIX: gen_unary_postfix(e); return;
    case EXPR_CALL: gen_call(e, false); return;
    case EXPR_IF: gen_if(e); return;
    case EXPR_BLOCK: {
        defer_push(false);
        bool done = gen_stmts(e->block.stmts, e->block.count,
                              type_valueless(e->type) ? TAIL_DISCARD : TAIL_VALUE);
        if (!done) emit_scope_defers(g_defer);
        defer_pop();
        return;
    }
    case EXPR_GUARD: {
        bool sg = g_guards_suppressed, so = g_overflow_checked;
        if (e->guard.is_overflow_axis) g_overflow_checked = e->guard.enable;
        else g_guards_suppressed = !e->guard.enable;
        gen_expr(e->guard.body);
        g_guards_suppressed = sg;
        g_overflow_checked = so;
        return;
    }
    case EXPR_CAST: gen_cast(e); return;
    case EXPR_FIELD: gen_field(e); return;
    case EXPR_DEREF_FIELD: gen_deref_field(e); return;
    case EXPR_INDEX: {
        Type *ot = conc(e->index.object->type);
        gen_addr(e);
        if (!(ot->kind == TYPE_STRUCT && ot->struc.is_tuple) || val_kind(e->type) != VK_AGG)
            load_val(e->type, "(%rax)");
        return;
    }
    case EXPR_SLICE: {
        Type *ot = conc(e->slice.object->type);
        int64_t es = ty_size(ot->slice.elem);
        gen_expr(e->slice.object);
        int32_t sh = slot_alloc(16, 8);
        copy_mem("%rbp", sh, "%rax", 0, 16);
        int32_t lo = slot_alloc(8, 8);
        if (e->slice.lo) gen_expr(e->slice.lo); else ins("xorl %%eax, %%eax");
        ins("movq %%rax, %s", fp(lo));
        if (e->slice.hi) gen_expr(e->slice.hi); else ins("movq %s, %%rax", fp(sh + 8));
        ins("movq %s, %%rcx", fp(lo));
        if (!(g_guards_suppressed && facts_guard_governs(e, subst_resolve))) {
            int m = msg_label(e->loc, "subslice out of range: lo=%lld hi=%lld len=%lld\n");
            int l = fail_begin();
            ins("movq %%rcx, %%rdx");
            ins("movq %%rax, %%rcx");
            ins("movq %s, %%r8", fp(sh + 8));
            fail_end(m);
            ins("cmpq %%rax, %%rcx");
            ins("ja .Lq%d", l);
            ins("cmpq %s, %%rax", fp(sh + 8));
            ins("ja .Lq%d", l);
        }
        int32_t t = slot_alloc(16, 8);
        ins("subq %%rcx, %%rax");
        ins("movq %%rax, %s", fp(t + 8));
        if (es != 1) ins("imulq $%lld, %%rcx, %%rcx", (long long)es);
        ins("addq %s, %%rcx", fp(sh));
        ins("movq %%rcx, %s", fp(t));
        ins("leaq %s, %%rax", fp(t));
        return;
    }
    case EXPR_SOME: gen_some(e); return;
    case EXPR_OK: gen_ok(e); return;
    case EXPR_ERR: gen_err(e); return;
    case EXPR_ERROR_NAME: gen_error_name(e); return;
    case EXPR_ARRAY_LIT: gen_array_lit(e, false); return;
    case EXPR_SLICE_LIT: gen_slice_lit(e); return;
    case EXPR_STRUCT_LIT: gen_struct_lit(e); return;
    case EXPR_TUPLE_LIT: gen_tuple_lit(e); return;
    case EXPR_LOOP: gen_loop(e); return;
    case EXPR_FOR: gen_for(e); return;
    case EXPR_MATCH: gen_match(e, false); return;
    case EXPR_BREAK: gen_break(e->break_expr.value); return;
    case EXPR_CONTINUE: gen_continue(); return;
    case EXPR_RETURN: gen_return(e->return_expr.value); return;
    case EXPR_IGNORE: gen_expr(e->ignore_expr.value); return;
    case EXPR_DEFER:
        internal_error(e->loc, "defer emitted as an expression");
        return;
    case EXPR_SIZEOF: mov_imm(ty_size(e->sizeof_expr.target)); return;
    case EXPR_ALIGNOF: mov_imm(ty_align(e->alignof_expr.target)); return;
    case EXPR_BITCAST: gen_bitcast(e); return;
    case EXPR_ENUM_OF: gen_enum_of(e); return;
    case EXPR_DEFAULT: gen_default(e->default_expr.target); return;
    case EXPR_FREE: gen_free(e); return;
    case EXPR_ASSERT: gen_assert(e); return;
    case EXPR_ATOMIC_LOAD:
        gen_expr(e->atomic_load.ptr);
        load_val(e->type, "(%rax)");
        return;
    case EXPR_ATOMIC_STORE: {
        gen_expr(e->atomic_store.ptr);
        int32_t s = spill_push();
        ins("movq %%rax, %s", fp(s));
        gen_expr(e->atomic_store.value);
        ins("movq %s, %%rdx", fp(s));
        spill_pop();
        store_val(e->atomic_store.value->type, "%rdx", 0);
        ins("mfence");
        return;
    }
    case EXPR_ALLOC: gen_alloc(e); return;
    case EXPR_ASSIGN: gen_assign(e); return;
    case EXPR_LET: gen_let(e); return;
    case EXPR_LET_DESTRUCT: gen_let_destruct(e); return;
    case EXPR_FUNC: gen_func_value(e); return;
    case EXPR_INTERP_STRING: gen_interp(e, NULL); return;
    case EXPR_ERROR:
        internal_error(e->loc, "parse-error node reached codegen");
        return;
    }
}

/* ================================================================
 * Frame escapes: whether a tail call could see this frame
 * ================================================================ */

static bool frame_escapes(Expr *e);

static bool fe_pred(Expr *child, void *ctx) { (void)ctx; return frame_escapes(child); }

/* Whether an lvalue's storage is in the frame (a local, or a field of one). */
static bool fe_root_local(Expr *e) {
    while (e) {
        switch (e->kind) {
        case EXPR_IDENT: return e->ident.is_local;
        case EXPR_FIELD:
            if (e->field.codegen_name) return false;
            e = e->field.object;
            break;
        case EXPR_INDEX: {
            Type *ot = e->index.object->type;
            if (ot && ot->kind == TYPE_STRUCT && ot->struc.is_tuple) { e = e->index.object; break; }
            return false;
        }
        case EXPR_DEREF_FIELD: return false;
        case EXPR_UNARY_PREFIX: if (e->unary_prefix.op == TOK_STAR) return false; return true;
        case EXPR_GUARD: e = e->guard.body; break;
        default: return true;
        }
    }
    return true;
}

static bool frame_escapes(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_AMP && !fn_static_target(e->unary_prefix.operand) &&
            fe_root_local(e->unary_prefix.operand))
            return true;
        break;
    case EXPR_ARRAY_LIT: case EXPR_INTERP_STRING:
        return true;
    case EXPR_CAST:
        if (e->cast.operand->type && is_str_type(e->cast.operand->type) && is_cstr_type(e->cast.target))
            return true;
        break;
    case EXPR_ALLOC: {
        if (e->alloc_expr.is_stack) return true;
        if (frame_escapes(e->alloc_expr.size_expr)) return true;
        Expr *init = e->alloc_expr.init_expr;
        if (!init) return false;
        if (init->kind == EXPR_ARRAY_LIT || init->kind == EXPR_INTERP_STRING ||
            init->kind == EXPR_CAST || init->kind == EXPR_FUNC)
            return init->kind == EXPR_FUNC ? false : expr_any_child(init, fe_pred, NULL);
        return frame_escapes(init);
    }
    case EXPR_FUNC:
        return e->func.capture_count > 0 && !e->func.heap_alloc;
    case EXPR_FIELD:
        if (e->field.fixed_array_type && fe_root_local(e->field.object)) return true;
        break;
    default:
        break;
    }
    return expr_any_child(e, fe_pred, NULL);
}

/* ================================================================
 * Functions
 * ================================================================ */

typedef struct {
    const char *sym;
    bool global;
    Param *params;
    int nparams;
    Type *ret;
    bool has_ctx;
    Expr **body;
    int body_count;
    Expr *lambda;       /* a lifted lambda: its captures and self binding */
    bool is_main;
} FnSpec;

static void begin_function(void) {
    hmap_clear(&g_locals);
    g_frame = 0;
    g_spill_n = g_spill_depth = 0;
    g_hidden_ret = 0;
    g_ret_label = new_label();
    g_guards_suppressed = g_overflow_checked = false;
    g_defer = NULL;
    g_loop = NULL;
    g_loc_file = g_loc_line = 0;
    g_fnbody.len = 0;
    g_cold.len = 0;
    g_cur = &g_fnbody;
}

static void end_function(const char *sym, bool global) {
    Buf *o = &g_text;
    buf_puts(o, "\t.text\n\t.p2align 4\n");
    if (global) buf_printf(o, "\t.globl %s\n", sym);
    buf_printf(o, "\t.type %s, @function\n%s:\n", sym, sym);
    buf_puts(o, "\t.cfi_startproc\n\tpushq %rbp\n\t.cfi_def_cfa_offset 16\n"
                "\t.cfi_offset %rbp, -16\n\tmovq %rsp, %rbp\n\t.cfi_def_cfa_register %rbp\n");
    int64_t frame = align_up(g_frame, 16);
    if (frame) buf_printf(o, "\tsubq $%lld, %%rsp\n", (long long)frame);
    buf_put(o, g_fnbody.p ? g_fnbody.p : "", g_fnbody.len);
    buf_printf(o, ".Lq%d:\n\tleave\n\t.cfi_remember_state\n\t.cfi_def_cfa %%rsp, 8\n\tret\n"
                  "\t.cfi_restore_state\n", g_ret_label);
    buf_put(o, g_cold.p ? g_cold.p : "", g_cold.len);
    buf_printf(o, "\t.cfi_endproc\n\t.size %s, .-%s\n", sym, sym);
    g_cur = NULL;
}

static bool body_escapes(Expr **body, int n) {
    for (int i = 0; i < n; i++) if (frame_escapes(body[i])) return true;
    return false;
}

static void emit_function(FnSpec *f) {
    begin_function();
    g_fn_ret = f->is_main ? type_int32() : conc(f->ret);
    g_fn_is_main = f->is_main;
    g_tco_ok = !f->is_main && !body_escapes(f->body, f->body_count);

    Abi ra = abi_classify(g_fn_ret);
    int ni = 0, ns = 0;
    int64_t stack = 16;
    if (ra.n == -1) {
        g_hidden_ret = slot_alloc(8, 8);
        ins("movq %%rdi, %s", fp(g_hidden_ret));
        ni = 1;
    }
    for (int i = 0; i < f->nparams; i++) {
        Param *p = &f->params[i];
        const char *name = p->codegen_name ? p->codegen_name : p->name;
        Type *pt = conc(p->type);
        Abi a = abi_classify(pt);
        if (a.n == 0) { local_bind(name, slot_alloc(8, 8)); continue; }
        if (a.n > 0) {
            int ci = 0, cS = 0;
            for (int k = 0; k < a.n; k++) { if (a.cls[k] == 'I') ci++; else cS++; }
            if (ni + ci <= 6 && ns + cS <= 8) {
                int32_t off = local_slot(name, pt);
                ValKind vk = val_kind(pt);
                if (vk == VK_INT) ins("movq %s, %s", IREG64[ni++], fp(off));
                else if (vk == VK_FLOAT) ins("movq %%xmm%d, %s", ns++, fp(off));
                else
                    for (int k = 0; k < a.n; k++) {
                        if (a.cls[k] == 'I') ins("movq %s, %s", IREG64[ni++], fp(off + 8 * k));
                        else ins("movq %%xmm%d, %s", ns++, fp(off + 8 * k));
                    }
                continue;
            }
        }
        int64_t al = ty_align(pt);
        if (al < 8) al = 8;
        stack = align_up(stack, al);
        local_bind(name, (int32_t)stack);
        stack += align_up(ty_size(pt), 8);
    }
    int32_t ctx_slot = 0;
    if (f->has_ctx) {
        ctx_slot = slot_alloc(8, 8);
        if (ni < 6) ins("movq %s, %s", IREG64[ni++], fp(ctx_slot));
        else {
            ins("movq %lld(%%rbp), %%rax", (long long)stack);
            ins("movq %%rax, %s", fp(ctx_slot));
        }
    }
    if (f->lambda && f->lambda->func.capture_count > 0) {
        Expr *lam = f->lambda;
        int n = lam->func.capture_count;
        int64_t *offs = xcalloc((size_t)n, sizeof(int64_t));
        ctx_layout(lam, offs, NULL);
        for (int i = 0; i < n; i++) {
            Capture *c = &lam->func.captures[i];
            Type *ct = conc(c->type);
            int32_t off = local_slot(c->codegen_name, ct);
            ins("movq %s, %%rdx", fp(ctx_slot));
            if (val_kind(ct) == VK_AGG) copy_mem("%rbp", off, "%rdx", offs[i], ty_size(ct));
            else {
                char mem[48];
                snprintf(mem, sizeof mem, "%lld(%%rdx)", (long long)offs[i]);
                load_val(ct, mem);
                store_val(ct, "%rbp", off);
            }
        }
        free(offs);
    }
    if (f->lambda && f->lambda->func.self_codegen_name && f->lambda->func.self_referenced) {
        int32_t s = slot_alloc(16, 8);
        local_bind(f->lambda->func.self_codegen_name, s);
        ins("leaq %s(%%rip), %%rax", f->sym);
        ins("movq %%rax, %s", fp(s));
        if (f->lambda->func.capture_count > 0) {
            ins("movq %s, %%rax", fp(ctx_slot));
            ins("movq %%rax, %s", fp(s + 8));
        } else {
            ins("movq $0, %s", fp(s + 8));
        }
    }

    defer_push(false);
    bool done = gen_stmts(f->body, f->body_count, TAIL_RETURN);
    if (!done) emit_scope_defers(g_defer);
    defer_pop();
    if (f->is_main && !done) ins("xorl %%eax, %%eax");
    end_function(f->sym, f->global);
}

/* ---- Lifted lambdas, instances, wrappers and trampolines ---- */

typedef struct { Expr **items; int count, cap; } ExprList;

static void collect_lambdas(Expr *e, void *ctx) {
    if (!e) return;
    ExprList *l = ctx;
    if (e->kind == EXPR_FUNC && e->func.lifted_name) {
        for (int i = 0; i < l->count; i++) if (l->items[i] == e) return;
        DA_APPEND(l->items, l->count, l->cap, e);
    }
    expr_for_each_child(e, collect_lambdas, ctx);
}

static void emit_lambda(Expr *lam) {
    FnSpec f = { 0 };
    f.sym = lambda_c_name(lam);
    f.params = lam->func.params;
    f.nparams = lam->func.param_count;
    f.ret = lam->type->func.return_type;
    f.has_ctx = true;
    f.body = lam->func.body;
    f.body_count = lam->func.body_count;
    f.lambda = lam;
    emit_function(&f);
}

/* Trampolines: an FC function with no context argument, for C. */
typedef struct { const char *sym; Type *ft; } Tramp;
static Tramp *g_tramps;
static int g_tramp_n, g_tramp_cap;

static const char *ctramp_sym(const char *fn_sym, Type *ft) {
    const char *name = arena_sprintf(g_arena, "fc_ctramp_%s", fn_sym);
    for (int i = 0; i < g_tramp_n; i++) if (strcmp(g_tramps[i].sym, fn_sym) == 0) return name;
    Tramp t = { fn_sym, ft };
    DA_APPEND(g_tramps, g_tramp_n, g_tramp_cap, t);
    return name;
}

static void emit_trampoline(Tramp *t) {
    Type *ft = conc(t->ft);
    int ni = 0, ns = 0;
    Abi ra = abi_classify(ft->func.return_type);
    if (ra.n == -1) ni = 1;
    bool ok = true;
    for (int i = 0; i < ft->func.param_count; i++) {
        Abi a = abi_classify(ft->func.param_types[i]);
        if (a.n <= 0) continue;
        int ci = 0, cS = 0;
        for (int k = 0; k < a.n; k++) { if (a.cls[k] == 'I') ci++; else cS++; }
        if (ni + ci <= 6 && ns + cS <= 8) { ni += ci; ns += cS; }
    }
    if (ni >= 6) ok = false;
    const char *name = arena_sprintf(g_arena, "fc_ctramp_%s", t->sym);
    buf_printf(&g_text, "\t.text\n\t.p2align 4\n\t.type %s, @function\n%s:\n", name, name);
    if (ok) buf_printf(&g_text, "\txorl %s, %s\n\tjmp %s\n", IREG32[ni], IREG32[ni], t->sym);
    else {
        internal_error((SrcLoc){0}, "C callback '%s' has too many arguments for the native backend", t->sym);
        buf_puts(&g_text, "\tud2\n");
    }
    buf_printf(&g_text, "\t.size %s, .-%s\n", name, name);
}

/* Extern wrappers: an extern as an FC function value. */
static const Symbol **g_cwraps;
static int g_cwrap_n, g_cwrap_cap;

static const char *cwrap_sym(const Decl *ext) {
    const char *name = arena_sprintf(g_arena, "fc_cwrap_%s", ext->ext.name);
    for (int i = 0; i < g_cwrap_n; i++) if (g_cwraps[i]->decl == ext) return name;
    /* find its symbol */
    Symbol *found = NULL;
    for (int i = 0; i < g_symtab->count && !found; i++)
        if (g_symtab->symbols[i].decl == ext) found = &g_symtab->symbols[i];
    if (!found) {
        Symbol *s = arena_alloc(g_arena, sizeof(Symbol));
        memset(s, 0, sizeof *s);
        s->name = ext->ext.name;
        s->kind = DECL_EXTERN;
        s->decl = (Decl *)ext;
        s->type = ext->ext.type;
        found = s;
    }
    DA_APPEND(g_cwraps, g_cwrap_n, g_cwrap_cap, found);
    return name;
}

static void emit_cwrap(const Symbol *sym) {
    const Decl *d = sym->decl;
    Type *ft = d->ext.type;
    int n = ft->func.param_count;
    Param *params = arena_alloc(g_arena, sizeof(Param) * (size_t)(n > 0 ? n : 1));
    Expr **args = arena_alloc(g_arena, sizeof(Expr *) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        memset(&params[i], 0, sizeof params[i]);
        params[i].name = params[i].codegen_name = arena_sprintf(g_arena, "_p%d", i);
        params[i].type = ft->func.param_types[i];
        Expr *a = arena_alloc(g_arena, sizeof(Expr));
        memset(a, 0, sizeof *a);
        a->kind = EXPR_IDENT;
        a->loc = d->loc;
        a->type = params[i].type;
        a->ident.name = a->ident.codegen_name = params[i].codegen_name;
        a->ident.is_local = true;
        args[i] = a;
    }
    Expr *callee = arena_alloc(g_arena, sizeof(Expr));
    memset(callee, 0, sizeof *callee);
    callee->kind = EXPR_IDENT;
    callee->loc = d->loc;
    callee->type = ft;
    callee->ident.name = callee->ident.codegen_name = d->ext.name;
    callee->ident.resolved_sym = (Symbol *)sym;
    Expr *call = arena_alloc(g_arena, sizeof(Expr));
    memset(call, 0, sizeof *call);
    call->kind = EXPR_CALL;
    call->loc = d->loc;
    call->type = ft->func.return_type;
    call->call.func = callee;
    call->call.args = args;
    call->call.arg_count = n;
    call->call.is_extern_call = true;
    call->call.resolved_callee = (Symbol *)sym;
    Expr **body = arena_alloc(g_arena, sizeof(Expr *));
    body[0] = call;
    FnSpec f = { 0 };
    f.sym = arena_sprintf(g_arena, "fc_cwrap_%s", d->ext.name);
    f.params = params;
    f.nparams = n;
    f.ret = ft->func.return_type;
    f.has_ctx = true;
    f.body = body;
    f.body_count = 1;
    emit_function(&f);
}

/* ================================================================
 * Equality helpers: bool fc_xeq_<T>(const T *a, const T *b)
 * ================================================================ */

typedef struct { const char *name; Type *type; } EqHelper;
static EqHelper *g_eqs;
static int g_eq_n, g_eq_cap;

static const char *eq_helper(Type *t) {
    t = conc(t);
    Type nc = *t;
    nc.is_const = false;
    char *m = mangle_type_name(&nc);
    const char *name = arena_sprintf(g_arena, "fc_xeq_%s", m);
    free(m);
    for (int i = 0; i < g_eq_n; i++) if (strcmp(g_eqs[i].name, name) == 0) return name;
    Type *copy = arena_alloc(g_arena, sizeof(Type));
    *copy = nc;
    EqHelper h = { name, copy };
    DA_APPEND(g_eqs, g_eq_n, g_eq_cap, h);
    return name;
}

/* Compare the values of type t at aoff(*A) and boff(*B), where the frame
 * slots -8 and -16 hold the base pointers A and B; jump to lfalse when they
 * differ. */
static void eq_cmp(Type *t, int64_t off, int lfalse) {
    t = conc(t);
    char am[48], bm[48];
    snprintf(am, sizeof am, "%lld(%%rax)", (long long)off);
    snprintf(bm, sizeof bm, "%lld(%%rcx)", (long long)off);
    switch (val_kind(t)) {
    case VK_VOID: return;
    case VK_INT:
        ins("movq -16(%%rbp), %%rcx");
        ins("movq -8(%%rbp), %%rax");
        load_int(t, bm);
        ins("movq %%rax, %%rdx");
        ins("movq -8(%%rbp), %%rax");
        load_int(t, am);
        ins("cmpq %%rdx, %%rax");
        ins("jne .Lq%d", lfalse);
        return;
    case VK_FLOAT: {
        bool f = is_f32(t);
        ins("movq -8(%%rbp), %%rax");
        ins("movq -16(%%rbp), %%rcx");
        ins(f ? "movss %s, %%xmm0" : "movsd %s, %%xmm0", am);
        ins(f ? "ucomiss %s, %%xmm0" : "ucomisd %s, %%xmm0", bm);
        ins("jne .Lq%d", lfalse);
        ins("jp .Lq%d", lfalse);
        return;
    }
    case VK_AGG:
        break;
    }
    ins("movq -8(%%rbp), %%rdi");
    ins("movq -16(%%rbp), %%rsi");
    if (off) { ins("addq $%lld, %%rdi", (long long)off); ins("addq $%lld, %%rsi", (long long)off); }
    ins("call %s", eq_helper(t));
    ins("testb %%al, %%al");
    ins("jz .Lq%d", lfalse);
}

/* memcmp of n bytes at off(A) and off(B); jumps to lfalse if they differ. */
static void eq_memcmp_bytes(int64_t off, int64_t n, int lfalse) {
    if (n <= 0) return;
    ins("movq -8(%%rbp), %%rdi");
    ins("movq -16(%%rbp), %%rsi");
    if (off) { ins("addq $%lld, %%rdi", (long long)off); ins("addq $%lld, %%rsi", (long long)off); }
    ins("movq $%lld, %%rdx", (long long)n);
    call_c("memcmp");
    ins("testl %%eax, %%eax");
    ins("jne .Lq%d", lfalse);
}

static bool plain_bytes_eq(Type *elem) {
    elem = conc(elem);
    return !needs_eq_func(elem) && !type_is_float(elem);
}

/* Elementwise comparison of `count` elements (count in slot -24, or a
 * constant when count_slot is 0) of type elem starting at off(A)/off(B). */
static void eq_elements(Type *elem, int64_t off, int64_t count, bool count_in_slot, int lfalse) {
    int64_t es = ty_size(elem);
    int ltop = new_label(), ldone = new_label();
    /* save the bases, advance per element */
    ins("movq -8(%%rbp), %%rax");
    ins("movq %%rax, -40(%%rbp)");
    ins("movq -16(%%rbp), %%rax");
    ins("movq %%rax, -48(%%rbp)");
    if (off) { ins("addq $%lld, -8(%%rbp)", (long long)off); ins("addq $%lld, -16(%%rbp)", (long long)off); }
    ins("movq $0, -32(%%rbp)");
    label(ltop);
    ins("movq -32(%%rbp), %%rax");
    if (count_in_slot) ins("cmpq -24(%%rbp), %%rax");
    else ins("cmpq $%lld, %%rax", (long long)count);
    ins("jge .Lq%d", ldone);
    eq_cmp(elem, 0, lfalse);
    ins("addq $%lld, -8(%%rbp)", (long long)es);
    ins("addq $%lld, -16(%%rbp)", (long long)es);
    ins("incq -32(%%rbp)");
    ins("jmp .Lq%d", ltop);
    label(ldone);
    ins("movq -40(%%rbp), %%rax");
    ins("movq %%rax, -8(%%rbp)");
    ins("movq -48(%%rbp), %%rax");
    ins("movq %%rax, -16(%%rbp)");
}

static void emit_eq_helper(EqHelper *h) {
    Type *t = conc(h->type);
    begin_function();
    g_frame = 48;
    int ltrue = new_label(), lfalse = new_label();
    ins("movq %%rdi, -8(%%rbp)");
    ins("movq %%rsi, -16(%%rbp)");
    switch (t->kind) {
    case TYPE_STRUCT: {
        Layout *L = agg_layout(t);
        if (t->struc.is_c_union) { eq_memcmp_bytes(0, L->size, lfalse); break; }
        for (int i = 0; i < L->n; i++) {
            Type *ft = conc(L->ftype[i]);
            if (ft->kind == TYPE_FIXED_ARRAY) {
                Type *elem = conc(ft->fixed_array.elem);
                if (plain_bytes_eq(elem)) eq_memcmp_bytes(L->off[i], ty_size(ft), lfalse);
                else eq_elements(elem, L->off[i], fixarr_size(ft), false, lfalse);
            } else if (L->ext && L->ext[i].special) {
                ins("movq -16(%%rbp), %%rdx");
                load_ext_field(ft, &L->ext[i], "%rdx", 0, L->off[i]);
                ins("movq %%rax, %%rsi");
                ins("movq -8(%%rbp), %%rdx");
                load_ext_field(ft, &L->ext[i], "%rdx", 0, L->off[i]);
                ins("cmpq %%rsi, %%rax");
                ins("jne .Lq%d", lfalse);
            } else {
                eq_cmp(ft, L->off[i], lfalse);
            }
        }
        break;
    }
    case TYPE_UNION: {
        Layout *L = agg_layout(t);
        ins("movq -8(%%rbp), %%rax");
        ins("movq -16(%%rbp), %%rcx");
        ins("movl (%%rax), %%edx");
        ins("cmpl (%%rcx), %%edx");
        ins("jne .Lq%d", lfalse);
        for (int i = 0; i < L->n; i++) {
            if (!L->ftype[i]) continue;
            int lnext = new_label();
            ins("movq -8(%%rbp), %%rax");
            ins("cmpl $%d, (%%rax)", i);
            ins("jne .Lq%d", lnext);
            eq_cmp(L->ftype[i], L->payload_off, lfalse);
            ins("jmp .Lq%d", ltrue);
            label(lnext);
        }
        break;
    }
    case TYPE_SLICE: {
        Type *elem = conc(t->slice.elem);
        ins("movq -8(%%rbp), %%rax");
        ins("movq -16(%%rbp), %%rcx");
        ins("movq 8(%%rax), %%rdx");
        ins("cmpq 8(%%rcx), %%rdx");
        ins("jne .Lq%d", lfalse);
        ins("testq %%rdx, %%rdx");
        ins("jz .Lq%d", ltrue);
        if (plain_bytes_eq(elem)) {
            ins("movq (%%rax), %%rdi");
            ins("movq (%%rcx), %%rsi");
            int64_t es = ty_size(elem);
            if (es != 1) ins("imulq $%lld, %%rdx, %%rdx", (long long)es);
            call_c("memcmp");
            ins("testl %%eax, %%eax");
            ins("jne .Lq%d", lfalse);
        } else {
            ins("movq %%rdx, -24(%%rbp)");
            ins("movq (%%rax), %%rax");
            ins("movq %%rax, -8(%%rbp)");
            ins("movq (%%rcx), %%rcx");
            ins("movq %%rcx, -16(%%rbp)");
            eq_elements(elem, 0, 0, true, lfalse);
        }
        break;
    }
    case TYPE_OPTION: {
        int64_t is = ty_size(t->option.inner);
        ins("movq -8(%%rbp), %%rax");
        ins("movq -16(%%rbp), %%rcx");
        ins("movzbl %lld(%%rax), %%edx", (long long)is);
        ins("cmpb %lld(%%rcx), %%dl", (long long)is);
        ins("jne .Lq%d", lfalse);
        ins("testb %%dl, %%dl");
        ins("jz .Lq%d", ltrue);
        eq_cmp(t->option.inner, 0, lfalse);
        break;
    }
    case TYPE_RESULT: {
        ins("movq -8(%%rbp), %%rax");
        ins("movq -16(%%rbp), %%rcx");
        ins("movl (%%rax), %%edx");
        ins("cmpl (%%rcx), %%edx");
        ins("jne .Lq%d", lfalse);
        Type *in = conc(t->result.inner);
        if (in && in->kind != TYPE_VOID) {
            ins("testl %%edx, %%edx");
            ins("jnz .Lq%d", ltrue);
            eq_cmp(in, result_value_off(t), lfalse);
        }
        break;
    }
    case TYPE_FUNC:
        eq_memcmp_bytes(0, 16, lfalse);
        break;
    default:
        internal_error((SrcLoc){0}, "no equality for type %s", type_name(t));
        break;
    }
    label(ltrue);
    ins("movl $1, %%eax");
    ins("jmp .Lq%d", g_ret_label);
    label(lfalse);
    ins("xorl %%eax, %%eax");
    end_function(h->name, false);
}

/* ================================================================
 * Global data
 * ================================================================ */

typedef struct { int64_t off; const char *sym; int64_t addend; } Reloc;
typedef struct {
    unsigned char *data;
    int64_t size;
    Reloc *rel;
    int nrel, caprel;
} DataImg;

static void img_put(DataImg *img, int64_t off, uint64_t v, int w) {
    for (int i = 0; i < w && off + i < img->size; i++) img->data[off + i] = (unsigned char)(v >> (8 * i));
}

static void img_reloc(DataImg *img, int64_t off, const char *sym, int64_t addend) {
    Reloc r = { off, sym, addend };
    DA_APPEND(img->rel, img->nrel, img->caprel, r);
}

static void emit_img(Buf *b, const char *name, DataImg *img, int64_t align) {
    buf_printf(b, "\t.p2align %d\n%s:\n", align >= 16 ? 4 : align >= 8 ? 3 : align >= 4 ? 2 : align >= 2 ? 1 : 0, name);
    int64_t i = 0;
    while (i < img->size) {
        int r = -1;
        for (int k = 0; k < img->nrel; k++) if (img->rel[k].off == i) { r = k; break; }
        if (r >= 0) {
            if (img->rel[r].addend) buf_printf(b, "\t.quad %s+%lld\n", img->rel[r].sym, (long long)img->rel[r].addend);
            else buf_printf(b, "\t.quad %s\n", img->rel[r].sym);
            i += 8;
            continue;
        }
        /* a run without relocations */
        int64_t j = i;
        int64_t next_rel = img->size;
        for (int k = 0; k < img->nrel; k++) if (img->rel[k].off > i && img->rel[k].off < next_rel) next_rel = img->rel[k].off;
        bool allzero = true;
        for (j = i; j < next_rel; j++) if (img->data[j]) { allzero = false; break; }
        if (allzero) { buf_printf(b, "\t.zero %lld\n", (long long)(next_rel - i)); i = next_rel; continue; }
        while (i < next_rel) {
            buf_puts(b, "\t.byte ");
            for (int c = 0; c < 16 && i < next_rel; c++, i++) buf_printf(b, c ? ",%u" : "%u", img->data[i]);
            buf_puts(b, "\n");
        }
    }
}

static bool g_img_has_reloc;

/* A numeric constant: an integer (in *iv, the canonical pattern of the
 * expression's type) or a float (*fv). */
static bool const_num(Expr *e, bool *isf, int64_t *iv, double *fv) {
    switch (e->kind) {
    case EXPR_INT_LIT:
        *isf = false;
        *iv = canon_const(e->int_lit.value, e->int_lit.lit_type ? e->int_lit.lit_type : e->type);
        return true;
    case EXPR_FLOAT_LIT:
        *isf = true;
        *fv = e->float_lit.lit_type->kind == TYPE_FLOAT32 ? (double)(float)e->float_lit.value
                                                          : e->float_lit.value;
        return true;
    case EXPR_BOOL_LIT: *isf = false; *iv = e->bool_lit.value; return true;
    case EXPR_CHAR_LIT: *isf = false; *iv = e->char_lit.value; return true;
    case EXPR_SIZEOF: *isf = false; *iv = ty_size(e->sizeof_expr.target); return true;
    case EXPR_ALIGNOF: *isf = false; *iv = ty_align(e->alignof_expr.target); return true;
    case EXPR_GUARD: return const_num(e->guard.body, isf, iv, fv);
    case EXPR_FIELD:
        if (e->field.is_type_property) {
            char *endp;
            long long v = strtoll(e->field.codegen_name, &endp, 10);
            if (*e->field.codegen_name && *endp == 0) { *isf = false; *iv = v; return true; }
            Expr *obj = e->field.object;
            Type *pt = obj->kind == EXPR_IDENT ? type_from_name(obj->ident.name, (int)strlen(obj->ident.name)) : NULL;
            if (!pt || !type_prop_value(pt, e->field.name, iv, fv, isf)) return false;
            if (!*isf) *iv = canon_const((uint64_t)*iv, e->type);
            return true;
        }
        if (e->field.is_variant_constructor && conc(e->type)->kind == TYPE_ENUM) {
            *isf = false;
            *iv = enum_variant_value(e->type, e->field.name);
            return true;
        }
        if (e->field.is_extern_const && val_kind(e->type) != VK_AGG && !is_cstr_type(conc(e->type))) {
            if (!extern_const_value(e->field.codegen_name, e->type, iv, fv)) return false;
            *isf = val_kind(e->type) == VK_FLOAT;
            if (!*isf) *iv = canon_const((uint64_t)*iv, e->type);
            return true;
        }
        return false;
    case EXPR_IDENT: {
        Symbol *s = e->ident.resolved_sym;
        if (!e->ident.is_local && s && s->decl && s->decl->kind == DECL_EXTERN &&
            val_kind(e->type) != VK_AGG && !is_cstr_type(conc(e->type)) && s->decl->ext.type &&
            s->decl->ext.type->kind != TYPE_FUNC) {
            if (!extern_const_value(s->decl->ext.name, e->type, iv, fv)) return false;
            *isf = val_kind(e->type) == VK_FLOAT;
            if (!*isf) *iv = canon_const((uint64_t)*iv, e->type);
            return true;
        }
        return false;
    }
    case EXPR_CAST: {
        bool f2; int64_t i2; double d2;
        if (!const_num(e->cast.operand, &f2, &i2, &d2)) return false;
        Type *to = conc(e->cast.target), *from = conc(e->cast.operand->type);
        if (val_kind(to) == VK_FLOAT) {
            *isf = true;
            if (f2) *fv = d2;
            else if (val_kind(from) == VK_INT && !int_signed(from) && int_width(from) == 8) *fv = (double)(uint64_t)i2;
            else *fv = (double)i2;
            if (is_f32(to)) *fv = (double)(float)*fv;
            return true;
        }
        if (val_kind(to) != VK_INT) return false;
        *isf = false;
        if (f2) {
            /* saturating, as the runtime conversion */
            F2i fi = f2i_info(to);
            if (d2 != d2) *iv = 0;
            else if (d2 < fi.lo) *iv = fi.imin;
            else if (d2 >= fi.hi) *iv = fi.imax;
            else if ((conc(to)->kind == TYPE_UINT64 || conc(to)->kind == TYPE_USIZE) && d2 >= 9223372036854775808.0)
                *iv = (int64_t)(uint64_t)d2;
            else *iv = (int64_t)d2;
            *iv = canon_const((uint64_t)*iv, to);
            return true;
        }
        *iv = canon_const((uint64_t)i2, to);
        return true;
    }
    case EXPR_UNARY_PREFIX: {
        bool f2; int64_t i2; double d2;
        if (!const_num(e->unary_prefix.operand, &f2, &i2, &d2)) return false;
        *isf = f2;
        switch (e->unary_prefix.op) {
        case TOK_MINUS:
            if (f2) { *fv = -d2; if (is_f32(e->type)) *fv = (double)(float)*fv; }
            else *iv = canon_const((uint64_t)0 - (uint64_t)i2, e->type);
            return true;
        case TOK_TILDE: *iv = canon_const(~(uint64_t)i2, e->type); return !f2;
        case TOK_BANG: *iv = !i2; return !f2;
        default: return false;
        }
    }
    case EXPR_BINARY: {
        bool fl, fr; int64_t il, ir; double dl, dr;
        if (!const_num(e->binary.left, &fl, &il, &dl) || !const_num(e->binary.right, &fr, &ir, &dr))
            return false;
        Type *lt = conc(e->binary.left->type);
        if (fl || fr) {
            if (!(fl && fr)) return false;
            bool f = is_f32(lt);
            double r;
            switch (e->binary.op) {
            case TOK_PLUS: r = dl + dr; break;
            case TOK_MINUS: r = dl - dr; break;
            case TOK_STAR: r = dl * dr; break;
            case TOK_SLASH: r = dl / dr; break;
            case TOK_EQEQ: *isf = false; *iv = dl == dr; return true;
            case TOK_BANGEQ: *isf = false; *iv = dl != dr; return true;
            case TOK_LT: *isf = false; *iv = dl < dr; return true;
            case TOK_GT: *isf = false; *iv = dl > dr; return true;
            case TOK_LTEQ: *isf = false; *iv = dl <= dr; return true;
            case TOK_GTEQ: *isf = false; *iv = dl >= dr; return true;
            default: return false;
            }
            *isf = true;
            *fv = f ? (double)(float)r : r;
            return true;
        }
        if (val_kind(lt) != VK_INT || !const_binary_op_supported(e->binary.op)) return false;
        uint64_t out;
        if (const_binary_op(e->binary.op, (uint64_t)il, (uint64_t)ir, int_width(lt) * 8,
                            int_signed(lt), &out) != CONST_OP_OK)
            return false;
        *isf = false;
        *iv = const_op_yields_bool(e->binary.op) ? (int64_t)out : canon_const(out, e->type);
        return true;
    }
    default:
        return false;
    }
}

static bool ser(Expr *e, Type *t, DataImg *img, int64_t off, Buf *sec);

/* A slice literal's elements as their own data object; returns its label. */
static int ser_backing(Expr *al, Buf *sec) {
    Type *elem = conc(al->array_lit.elem_type);
    int64_t es = ty_size(elem);
    int64_t N = al->array_lit.size_expr->int_lit.value;
    DataImg img = { 0 };
    img.size = N * es;
    img.data = xcalloc((size_t)(img.size > 0 ? img.size : 1), 1);
    bool ok = true;
    for (int i = 0; i < al->array_lit.elem_count && ok; i++)
        ok = ser(al->array_lit.elems[i], elem, &img, i * es, sec);
    int l = 0;
    if (ok) {
        l = new_label();
        char nm[24];
        snprintf(nm, sizeof nm, ".Lq%d", l);
        emit_img(sec, nm, &img, ty_align(elem));
    }
    free(img.data);
    free(img.rel);
    return ok ? l : -1;
}

static bool ser(Expr *e, Type *t, DataImg *img, int64_t off, Buf *sec) {
    t = conc(t);
    ValKind k = val_kind(t);
    if (e->kind == EXPR_GUARD) return ser(e->guard.body, t, img, off, sec);
    if (e->kind == EXPR_DEFAULT) return true;    /* zero */
    if (e->kind == EXPR_CAST && val_kind(conc(e->cast.operand->type)) == VK_AGG &&
        k == VK_AGG)
        return ser(e->cast.operand, t, img, off, sec);
    if (k == VK_INT || k == VK_FLOAT) {
        if (k == VK_INT && fn_static_target(e) == NULL && e->kind == EXPR_CSTRING_LIT) {
            int n;
            unsigned char *b = decode_lit(e->cstring_lit.value, e->cstring_lit.length, &n);
            int l = str_label(b, n);
            free(b);
            img_reloc(img, off, arena_sprintf(g_arena, ".Lq%d", l), 0);
            g_img_has_reloc = true;
            return true;
        }
        if (k == VK_INT && e->kind == EXPR_SOME && is_null_sentinel(t))
            return ser(e->some_expr.value, conc(t->option.inner), img, off, sec);
        if (k == VK_INT && e->kind == EXPR_UNARY_PREFIX && e->unary_prefix.op == TOK_AMP &&
            fn_static_target(e->unary_prefix.operand)) {
            bool is_extern;
            const char *sym = static_fn_sym(e->unary_prefix.operand, &is_extern);
            if (is_extern) return false;
            img_reloc(img, off, ctramp_sym(sym, conc(e->unary_prefix.operand->type)), 0);
            g_img_has_reloc = true;
            return true;
        }
        bool isf; int64_t iv; double fv;
        if (!const_num(e, &isf, &iv, &fv)) return false;
        if (k == VK_FLOAT) {
            double d = isf ? fv : (double)iv;
            if (is_f32(t)) { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); img_put(img, off, b, 4); }
            else { uint64_t b; memcpy(&b, &d, 8); img_put(img, off, b, 8); }
        } else {
            if (isf) return false;
            img_put(img, off, (uint64_t)iv, int_width(t));
        }
        return true;
    }
    if (k != VK_AGG) return true;
    switch (e->kind) {
    case EXPR_STRING_LIT: {
        int n;
        unsigned char *b = decode_lit(e->string_lit.value, e->string_lit.length, &n);
        int l = str_label(b, n);
        free(b);
        img_reloc(img, off, arena_sprintf(g_arena, ".Lq%d", l), 0);
        img_put(img, off + 8, (uint64_t)n, 8);
        g_img_has_reloc = true;
        return true;
    }
    case EXPR_ARRAY_LIT: {
        int64_t N = e->array_lit.size_expr && e->array_lit.size_expr->kind == EXPR_INT_LIT
            ? (int64_t)e->array_lit.size_expr->int_lit.value : -1;
        if (N < 0) return false;
        if (e->array_lit.elem_count > 0 || N > 0) {
            int l = ser_backing(e, sec);
            if (l < 0) return false;
            img_reloc(img, off, arena_sprintf(g_arena, ".Lq%d", l), 0);
            g_img_has_reloc = true;
        }
        img_put(img, off + 8, (uint64_t)N, 8);
        return true;
    }
    case EXPR_STRUCT_LIT: {
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            int64_t fo; Type *ft; ExtField *x;
            if (!field_info_x(t, e->struct_lit.fields[i].name, &fo, &ft, &x) || x) return false;
            Type *cft = conc(ft);
            Expr *v = e->struct_lit.fields[i].value;
            if (cft->kind == TYPE_FIXED_ARRAY) {
                Type *elem = conc(cft->fixed_array.elem);
                int64_t es = ty_size(elem);
                if (v->kind == EXPR_DEFAULT) continue;
                if (v->kind == EXPR_STRING_LIT) {
                    int n;
                    unsigned char *b = decode_lit(v->string_lit.value, v->string_lit.length, &n);
                    for (int j = 0; j < n && j < fixarr_size(cft); j++) img_put(img, off + fo + j, b[j], 1);
                    free(b);
                    continue;
                }
                if (v->kind == EXPR_ARRAY_LIT) {
                    for (int j = 0; j < v->array_lit.elem_count; j++)
                        if (!ser(v->array_lit.elems[j], elem, img, off + fo + j * es, sec)) return false;
                    continue;
                }
                return false;
            }
            if (!ser(v, ft, img, off + fo, sec)) return false;
        }
        return true;
    }
    case EXPR_TUPLE_LIT: {
        Layout *L = agg_layout(t);
        for (int i = 0; i < e->tuple_lit.elem_count && i < L->n; i++)
            if (!ser(e->tuple_lit.elems[i], L->ftype[i], img, off + L->off[i], sec)) return false;
        return true;
    }
    case EXPR_SOME: {
        Type *in = conc(t->option.inner);
        if (!ser(e->some_expr.value, in, img, off, sec)) return false;
        img_put(img, off + ty_size(in), 1, 1);
        return true;
    }
    case EXPR_OK:
        if (e->ok_expr.value && !ser(e->ok_expr.value, t->result.inner, img, off + result_value_off(t), sec))
            return false;
        return true;
    case EXPR_ERR: {
        bool isf; int64_t iv; double fv;
        if (!const_num(e->err_expr.code, &isf, &iv, &fv) || isf) return false;
        img_put(img, off, (uint64_t)iv, 4);
        return true;
    }
    case EXPR_FIELD:
        if (e->field.is_variant_constructor && t->kind == TYPE_UNION) {
            img_put(img, off, (uint64_t)union_variant_index(t, e->field.name), 4);
            return true;
        }
        if (fn_static_target(e)) break;
        return false;
    case EXPR_CALL:
        if (e->call.func->kind == EXPR_FIELD && e->call.func->field.is_variant_constructor &&
            t->kind == TYPE_UNION) {
            const char *vn = e->call.func->field.name;
            img_put(img, off, (uint64_t)union_variant_index(t, vn), 4);
            Type *pt = union_variant_payload(t, vn);
            return ser(e->call.args[0], pt, img, off + agg_layout(t)->payload_off, sec);
        }
        return false;
    default:
        break;
    }
    if (t->kind == TYPE_FUNC && (e->kind == EXPR_FUNC || fn_static_target(e))) {
        if (e->kind == EXPR_FUNC && e->func.capture_count > 0) return false;
        bool is_extern;
        const char *sym = static_fn_sym(e, &is_extern);
        if (is_extern) {
            const Expr *tg = fn_static_target(e);
            const Symbol *s = tg->kind == EXPR_IDENT ? tg->ident.resolved_sym : tg->field.resolved_member;
            sym = cwrap_sym(s->decl);
        }
        img_reloc(img, off, sym, 0);
        g_img_has_reloc = true;
        return true;
    }
    return false;
}

static Decl **g_runtime_globals;
static int g_rg_n, g_rg_cap;

static void emit_global(Decl *d) {
    const char *name = d->let.codegen_name ? d->let.codegen_name : d->let.name;
    Type *t = conc(d->let.resolved_type);
    int64_t size = ty_size(t), align = ty_align(t);
    if (size == 0) size = 1;
    if (d->let.is_module_member && d->let.init) {
        DataImg img = { 0 };
        img.size = size;
        img.data = xcalloc((size_t)size, 1);
        g_img_has_reloc = false;
        Buf tmp = { 0 };
        bool ok = ser(d->let.init, t, &img, 0, &tmp);
        if (ok) {
            Buf *sec = d->let.is_mut ? &g_data : (g_img_has_reloc ? &g_relro : &g_rodata);
            if (tmp.len) buf_put(sec, tmp.p, tmp.len);
            emit_img(sec, name, &img, align);
        }
        buf_free(&tmp);
        free(img.data);
        free(img.rel);
        if (ok) return;
    }
    buf_printf(&g_bss, "\t.p2align %d\n%s:\n\t.zero %lld\n",
               align >= 16 ? 4 : align >= 8 ? 3 : align >= 4 ? 2 : align >= 2 ? 1 : 0,
               name, (long long)size);
    DA_APPEND(g_runtime_globals, g_rg_n, g_rg_cap, d);
}

/* The C entry point. Like the emitted C's main, it runs the initializers
 * that are not static data first (module constants, then the entry file's
 * globals in order), in its own frame: a global's slice literal backing or
 * interpolation buffer lives there for the whole program. Then it builds
 * the str[] arguments and calls fc_main. */
static void emit_c_main(void) {
    begin_function();
    g_fn_ret = type_int32();
    g_tco_ok = false;
    int32_t argc = slot_alloc(8, 8), argv = slot_alloc(8, 8);
    ins("movslq %%edi, %%rdi");
    ins("movq %%rdi, %s", fp(argc));
    ins("movq %%rsi, %s", fp(argv));
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_rg_n; i++) {
            Decl *d = g_runtime_globals[i];
            if ((pass == 0) != d->let.is_module_member) continue;
            if (!d->let.init) continue;
            emit_loc(d->loc);
            const char *name = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            defer_push(false);
            gen_expr(d->let.init);
            ins("leaq %s(%%rip), %%rdx", name);
            store_val(d->let.resolved_type, "%rdx", 0);
            defer_pop();
        }
    }
    int32_t arr = slot_alloc(8, 8), idx = slot_alloc(8, 8);
    int ltop = new_label(), lgo = new_label();
    ins("movq %s, %%rax", fp(argc));
    ins("shlq $4, %%rax");
    dyn_alloca_rax();
    ins("movq %%rax, %s", fp(arr));
    ins("movq $0, %s", fp(idx));
    label(ltop);
    ins("movq %s, %%rax", fp(idx));
    ins("cmpq %s, %%rax", fp(argc));
    ins("jge .Lq%d", lgo);
    ins("movq %s, %%rcx", fp(argv));
    ins("movq (%%rcx,%%rax,8), %%rdi");
    ins("shlq $4, %%rax");
    ins("addq %s, %%rax", fp(arr));
    ins("movq %%rdi, (%%rax)");
    call_c("strlen");
    ins("movq %s, %%rcx", fp(idx));
    ins("shlq $4, %%rcx");
    ins("addq %s, %%rcx", fp(arr));
    ins("movq %%rax, 8(%%rcx)");
    ins("incq %s", fp(idx));
    ins("jmp .Lq%d", ltop);
    label(lgo);
    ins("movq %s, %%rdi", fp(arr));
    ins("movq %s, %%rsi", fp(argc));
    ins("call fc_main");
    end_function("main", true);
}

/* ================================================================
 * The C header probe
 *
 * A value only a C header knows (a #define or an enumerator) and the
 * layout of an extern struct are read by compiling and running a small C
 * program that prints them. The answer is cached by the probe's source
 * text, so the C compiler runs only when the declarations change.
 * ================================================================ */

typedef struct { char kind; int64_t iv; double fv; } ProbeVal;
static HMap g_probe;

static bool extern_const_value(const char *cname, Type *t, int64_t *iv, double *fv) {
    HEnt *e = hmap_get(&g_probe, cname, strlen(cname));
    if (!e) return false;
    ProbeVal *pv = e->ptr;
    *iv = pv->iv;
    *fv = pv->fv;
    if (pv->kind == 'F' && val_kind(t) != VK_FLOAT) *iv = (int64_t)pv->fv;
    if (pv->kind != 'F' && val_kind(t) == VK_FLOAT) *fv = (double)pv->iv;
    return true;
}

typedef struct { const char *cname; Type *type; } ConstRef;
typedef struct { ConstRef *items; int count, cap; } ConstRefs;

static void collect_const_refs(Expr *e, void *ctx) {
    if (!e) return;
    ConstRefs *r = ctx;
    const char *cname = NULL;
    Type *t = NULL;
    if (e->kind == EXPR_FIELD && e->field.is_extern_const) {
        cname = e->field.codegen_name;
        t = e->type;
    } else if (e->kind == EXPR_IDENT && !e->ident.is_local && e->ident.resolved_sym &&
               e->ident.resolved_sym->decl && e->ident.resolved_sym->decl->kind == DECL_EXTERN &&
               e->ident.resolved_sym->decl->ext.type &&
               e->ident.resolved_sym->decl->ext.type->kind != TYPE_FUNC) {
        cname = e->ident.resolved_sym->decl->ext.name;
        t = e->type;
    }
    if (cname) {
        bool seen = false;
        for (int i = 0; i < r->count; i++) if (strcmp(r->items[i].cname, cname) == 0) seen = true;
        if (!seen) { ConstRef c = { cname, t }; DA_APPEND(r->items, r->count, r->cap, c); }
    }
    expr_for_each_child(e, collect_const_refs, ctx);
}

/* ---- The program's C environment: its defines, includes, include path,
 * and which headers sit beside its sources (whose functions the C library
 * cannot supply). ---- */
static Buf g_cdefs, g_cincs, g_ciflags;
static HMap g_cseen;            /* "D:macro" -> value, "I:header" */
static HMap g_local_headers;    /* headers found beside a source file */
static HMap g_extern_lib;       /* extern Decl* (pointer bytes) -> its module's header */
static HMap g_src_dirs;

static void cenv_module(Decl *d, const char *lib) {
    if (d->kind == DECL_EXTERN) {
        if (lib) {
            HEnt *e = hmap_put(&g_extern_lib, arena_dup(g_arena, &d, 1, sizeof d), sizeof d);
            e->ptr = (void *)lib;
        }
        return;
    }
    if (d->kind != DECL_MODULE) return;
    if (d->module.define_macro) {
        char *key = str_sprintf("D:%s", d->module.define_macro);
        HEnt *e = hmap_get(&g_cseen, key, strlen(key));
        if (!e) {
            hmap_put(&g_cseen, key, strlen(key))->ptr = (void *)d->module.define_value;
            buf_printf(&g_cdefs, "#define %s %s\n", d->module.define_macro, d->module.define_value);
        } else {
            if (strcmp((const char *)e->ptr, d->module.define_value) != 0)
                diag_error(d->loc, "conflicting define for '%s': '%s' vs '%s'",
                           d->module.define_macro, (const char *)e->ptr, d->module.define_value);
            free(key);
        }
    }
    if (d->module.from_lib) {
        lib = d->module.from_lib;
        char *key = str_sprintf("I:%s", lib);
        if (!hmap_get(&g_cseen, key, strlen(key))) {
            hmap_put(&g_cseen, key, strlen(key));
            buf_printf(&g_cincs, "#include <%s>\n", lib);
            for (size_t i = 0; i < g_src_dirs.cap; i++) {
                if (!g_src_dirs.e[i].used) continue;
                char *path = str_sprintf("%s/%s", g_src_dirs.e[i].key, lib);
                bool here = access(path, F_OK) == 0;
                free(path);
                if (here) { hmap_put(&g_local_headers, lib, strlen(lib)); break; }
            }
        } else free(key);
    }
    for (int i = 0; i < d->module.decl_count; i++) cenv_module(d->module.decls[i], lib);
}

static void build_cenv(Program *prog, Decl **all, int all_count) {
    for (int i = 0; i < all_count; i++) {
        const char *fn = all[i]->loc.filename;
        if (!fn) continue;
        const char *slash = strrchr(fn, '/');
        char *dir = slash ? str_ndup(fn, (size_t)(slash - fn)) : str_dup(".");
        if (!hmap_get(&g_src_dirs, dir, strlen(dir))) {
            hmap_put(&g_src_dirs, dir, strlen(dir));
            buf_printf(&g_ciflags, " -I'%s'", dir);
        } else free(dir);
    }
    for (int i = 0; i < prog->decl_count; i++) cenv_module(prog->decls[i], NULL);
}

static void free_cenv(void) {
    buf_free(&g_cdefs); buf_free(&g_cincs); buf_free(&g_ciflags);
    for (size_t i = 0; i < g_cseen.cap; i++) if (g_cseen.e[i].used) free((char *)g_cseen.e[i].key);
    for (size_t i = 0; i < g_src_dirs.cap; i++) if (g_src_dirs.e[i].used) free((char *)g_src_dirs.e[i].key);
    hmap_free(&g_cseen); hmap_free(&g_src_dirs); hmap_free(&g_local_headers); hmap_free(&g_extern_lib);
}

/* Whether an extern comes from a header beside the sources, so its code is
 * compiled into the shim rather than taken from a library. */
static bool extern_via_shim(const Decl *d) {
    if (!d) return false;
    HEnt *e = hmap_get(&g_extern_lib, (const char *)&d, sizeof d);
    if (!e) return false;
    const char *lib = e->ptr;
    return hmap_get(&g_local_headers, lib, strlen(lib)) != NULL;
}

static HMap g_shim_used;    /* C names reached through the shim */

static const char *shim_ref(const char *cname) {
    if (!hmap_get(&g_shim_used, cname, strlen(cname))) hmap_put(&g_shim_used, cname, strlen(cname));
    return cname;
}

/* Compile C source `src` with the program's include path and return the
 * output: the program's stdout ("probe") or the assembly ("shim"). Cached by
 * the source, so the C compiler runs only when it changes. NULL on failure
 * (reported). */
static char *cc_cached(const char *kind, Buf *src) {
    uint64_t h = hash_bytes(src->p, src->len);
    h ^= hash_bytes(g_ciflags.p ? g_ciflags.p : "", g_ciflags.len) * 31;
    h ^= hash_bytes(kind, strlen(kind)) * 131;
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char *cache_dir = xdg && *xdg ? str_sprintf("%s/fcc", xdg)
                    : str_sprintf("%s/.cache/fcc", home ? home : "/tmp");
    char *cache = str_sprintf("%s/%s-%016llx.txt", cache_dir, kind, (unsigned long long)h);
    char *text = read_file(cache, NULL);
    if (text) { free(cache); free(cache_dir); return text; }
    const char *tmp = getenv("TMPDIR");
    char *work = str_sprintf("%s/fcc-%s-XXXXXX", tmp && *tmp ? tmp : "/tmp", kind);
    if (!mkdtemp(work)) {
        diag_error((SrcLoc){0}, "native backend: cannot create a work directory for the C %s", kind);
        free(work); free(cache); free(cache_dir);
        return NULL;
    }
    char *cpath = str_sprintf("%s/x.c", work);
    FILE *f = fopen(cpath, "w");
    if (f) { fwrite(src->p, 1, src->len, f); fclose(f); }
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    const char *ifl = g_ciflags.p ? g_ciflags.p : "";
    char *cmd = strcmp(kind, "probe") == 0
        ? str_sprintf("%s -w -o '%s/x' '%s'%s 2>'%s/err' && '%s/x' >'%s/out'",
                      cc, work, cpath, ifl, work, work, work)
        : str_sprintf("%s -w -S -O1 -fPIC -fno-asynchronous-unwind-tables -o '%s/out' '%s'%s 2>'%s/err'",
                      cc, work, cpath, ifl, work);
    int rc = system(cmd);
    if (rc != 0) {
        char *errp = str_sprintf("%s/err", work);
        char *err = read_file(errp, NULL);
        diag_error((SrcLoc){0}, "native backend: the C %s failed:\n%s", kind, err ? err : "");
        free(err);
        free(errp);
    } else {
        char *outp = str_sprintf("%s/out", work);
        text = read_file(outp, NULL);
        free(outp);
        if (text) {
            char *mk = str_sprintf("mkdir -p '%s'", cache_dir);
            if (system(mk) == 0) {
                FILE *cf = fopen(cache, "w");
                if (cf) { fputs(text, cf); fclose(cf); }
            }
            free(mk);
        }
    }
    char *rm = str_sprintf("rm -f '%s/x' '%s/x.c' '%s/err' '%s/out'; rmdir '%s'",
                           work, work, work, work, work);
    if (system(rm) != 0) { /* leftovers in TMPDIR are harmless */ }
    free(rm); free(cmd); free(cpath); free(work); free(cache); free(cache_dir);
    return text;
}

/* The shim: a pointer to each extern from a local header, compiled to
 * assembly and appended to the output (header functions are often static or
 * inline, so taking their address makes the C compiler emit them). */
static char *build_shim(void) {
    if (g_shim_used.n == 0) return NULL;
    Buf src = { 0 };
    buf_puts(&src, "/* fcc native-backend shim */\n");
    if (g_cdefs.len) buf_put(&src, g_cdefs.p, g_cdefs.len);
    if (g_cincs.len) buf_put(&src, g_cincs.p, g_cincs.len);
    /* in a stable order, so the cache hits */
    const char **names = xcalloc(g_shim_used.n, sizeof(char*));
    size_t k = 0;
    for (size_t i = 0; i < g_shim_used.cap; i++) if (g_shim_used.e[i].used) names[k++] = g_shim_used.e[i].key;
    for (size_t i = 1; i < k; i++)
        for (size_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            const char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
        }
    for (size_t i = 0; i < k; i++) buf_printf(&src, "void *fc_xp_%s = (void *)&%s;\n", names[i], names[i]);
    free(names);
    char *text = cc_cached("shim", &src);
    buf_free(&src);
    return text;
}

static void probe_parse(const char *text) {
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = str_ndup(p, len);
        char kind = line[0];
        char *rest = line + 2;
        char *sp = strchr(rest, ' ');
        if (len > 2 && sp) {
            *sp = 0;
            const char *name = rest;
            const char *val = sp + 1;
            if (kind == 'C' || kind == 'U' || kind == 'F' || kind == 'S') {
                ProbeVal *pv = arena_alloc(g_arena, sizeof(ProbeVal));
                pv->kind = kind;
                pv->iv = 0;
                pv->fv = 0;
                if (kind == 'C') pv->iv = strtoll(val, NULL, 10);
                else if (kind == 'U') pv->iv = (int64_t)strtoull(val, NULL, 10);
                else if (kind == 'F') pv->fv = strtod(val, NULL);
                else {
                    size_t hl = strlen(val);
                    unsigned char *bytes = xmalloc(hl / 2 + 1);
                    int n = 0;
                    for (size_t i = 0; i + 1 < hl; i += 2) {
                        unsigned v;
                        sscanf(val + i, "%2x", &v);
                        bytes[n++] = (unsigned char)v;
                    }
                    pv->iv = str_label(bytes, n);
                    free(bytes);
                }
                char *k = arena_strdup(g_arena, name, (int)strlen(name));
                hmap_put(&g_probe, k, strlen(k))->ptr = pv;
            } else if (kind == 'L') {
                /* L <key> <size> <align>, then its O and B lines */
                long long sz = 0, al = 0;
                sscanf(val, "%lld %lld", &sz, &al);
                ProbedLayout *pl = arena_alloc(g_arena, sizeof(ProbedLayout));
                memset(pl, 0, sizeof *pl);
                pl->size = sz;
                pl->align = al;
                char *k = arena_strdup(g_arena, name, (int)strlen(name));
                hmap_put(&g_probed_layouts, k, strlen(k))->ptr = pl;
            } else if (kind == 'O' || kind == 'B') {
                /* O <key> <field> <offset> <size>  |  B <key> <field> <bit> <width> <signed> */
                char *fname = xmalloc(strlen(val) + 1);
                long long a = 0, b = 0, c = 0;
                int got = sscanf(val, "%s %lld %lld %lld", fname, &a, &b, &c);
                if (got >= 3) {
                    HEnt *le = hmap_get(&g_probed_layouts, name, strlen(name));
                    if (le) {
                        ProbedLayout *pl = le->ptr;
                        if (pl->n == pl->cap) {
                            int nc = pl->cap ? pl->cap * 2 : 8;
                            ProbedField *nf = arena_alloc(g_arena, sizeof(ProbedField) * (size_t)nc);
                            if (pl->n) memcpy(nf, pl->f, sizeof(ProbedField) * (size_t)pl->n);
                            pl->f = nf;
                            pl->cap = nc;
                        }
                        ProbedField *pf = &pl->f[pl->n++];
                        memset(pf, 0, sizeof *pf);
                        pf->name = arena_strdup(g_arena, fname, (int)strlen(fname));
                        if (kind == 'O') { pf->off = a; pf->csize = b; }
                        else { pf->scanned = true; pf->bit_off = a; pf->bit_width = b; pf->csigned = c != 0; }
                    }
                }
                free(fname);
            }
        }
        free(line);
        if (!eol) break;
        p = eol + 1;
    }
}

/* The extern struct fields the program reads or writes, "S:tag|field": the
 * probe asks only about these, as the C compiler checks only these. */
static HMap g_field_refs;

static void note_field(Type *st, const char *fname) {
    st = conc(st);
    if (!st || st->kind != TYPE_STRUCT || !st->struc.c_name || st->struc.is_tuple) return;
    char *k = str_sprintf("%c:%s|%s", st->struc.is_c_union ? 'C' : 'S', st->struc.c_name, fname);
    if (!hmap_get(&g_field_refs, k, strlen(k))) hmap_put(&g_field_refs, k, strlen(k));
    else free(k);
}

static void note_eq_type(Type *t, int depth) {
    t = conc(t);
    if (!t || depth > 16) return;
    switch (t->kind) {
    case TYPE_STRUCT:
        for (int i = 0; i < t->struc.field_count; i++) {
            note_field(t, t->struc.fields[i].name);
            note_eq_type(t->struc.fields[i].type, depth + 1);
        }
        break;
    case TYPE_UNION:
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload) note_eq_type(t->unio.variants[i].payload, depth + 1);
        break;
    case TYPE_OPTION: note_eq_type(t->option.inner, depth + 1); break;
    case TYPE_RESULT: note_eq_type(t->result.inner, depth + 1); break;
    case TYPE_SLICE: note_eq_type(t->slice.elem, depth + 1); break;
    case TYPE_FIXED_ARRAY: note_eq_type(t->fixed_array.elem, depth + 1); break;
    default: break;
    }
}

static void note_pat(Pattern *p, Type *t) {
    t = conc(t);
    if (!p || !t || type_contains_type_var(t)) return;   /* extern structs are never generic */
    switch (p->kind) {
    case PAT_STRUCT:
        for (int i = 0; i < p->struc.field_count; i++) {
            note_field(t, p->struc.fields[i].name);
            int64_t off; Type *ft;
            if (t->kind == TYPE_STRUCT && field_info(t, p->struc.fields[i].name, &off, &ft))
                note_pat(p->struc.fields[i].pattern, ft);
        }
        break;
    case PAT_SOME: if (t->kind == TYPE_OPTION) note_pat(p->some_pat.inner, t->option.inner); break;
    case PAT_OK: if (t->kind == TYPE_RESULT) note_pat(p->some_pat.inner, t->result.inner); break;
    case PAT_VARIANT: if (p->variant.payload) note_pat(p->variant.payload, union_variant_payload(t, p->variant.variant)); break;
    case PAT_TUPLE:
        for (int i = 0; i < p->tuple_pat.pattern_count; i++)
            note_pat(p->tuple_pat.patterns[i], p->tuple_pat.resolved_types ? p->tuple_pat.resolved_types[i] : NULL);
        break;
    case PAT_OR:
        for (int i = 0; i < p->or_pat.alt_count; i++) note_pat(p->or_pat.alts[i], t);
        break;
    default: break;
    }
}

static void collect_field_refs(Expr *e, void *ctx) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_FIELD:
        if (!e->field.codegen_name && !e->field.is_variant_constructor && !e->field.is_type_property)
            note_field(e->field.object->type, e->field.name);
        break;
    case EXPR_DEREF_FIELD: {
        Type *pt = conc(e->field.object->type);
        if (pt && pt->kind == TYPE_POINTER) note_field(pt->pointer.pointee, e->field.name);
        break;
    }
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++) note_field(e->type, e->struct_lit.fields[i].name);
        break;
    case EXPR_BINARY:
        if (e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) note_eq_type(e->binary.left->type, 0);
        break;
    case EXPR_MATCH:
        for (int i = 0; i < e->match_expr.arm_count; i++)
            note_pat(e->match_expr.arms[i].pattern, e->match_expr.subject->type);
        break;
    case EXPR_LET_DESTRUCT:
        note_pat(e->let_destruct.pattern, e->let_destruct.init_type);
        break;
    case EXPR_FOR:
        if (e->for_expr.var_pattern) {
            Type *it = conc(e->for_expr.iter->type);
            if (it && it->kind == TYPE_SLICE) note_pat(e->for_expr.var_pattern, it->slice.elem);
        }
        break;
    default:
        break;
    }
    expr_for_each_child(e, collect_field_refs, ctx);
}

static void collect_extern_structs(Decl *d, Decl ***out, int *n, int *cap) {
    if (d->kind == DECL_MODULE) {
        for (int i = 0; i < d->module.decl_count; i++)
            collect_extern_structs(d->module.decls[i], out, n, cap);
    } else if (d->kind == DECL_STRUCT && d->struc.is_extern) {
        DA_APPEND(*out, *n, *cap, d);
    }
}

static void run_probe(Program *prog, Decl **all, int all_count, MonoTable *mono) {
    ConstRefs refs = { 0 };
    for (int i = 0; i < all_count; i++)
        if (all[i]->kind == DECL_LET && all[i]->let.init) {
            collect_const_refs(all[i]->let.init, &refs);
            collect_field_refs(all[i]->let.init, NULL);
        }
    (void)mono;
    Decl **structs = NULL;
    int ns = 0, ncap = 0;
    for (int i = 0; i < prog->decl_count; i++)
        collect_extern_structs(prog->decls[i], &structs, &ns, &ncap);
    if (refs.count == 0 && ns == 0) {
        for (size_t i = 0; i < g_field_refs.cap; i++) if (g_field_refs.e[i].used) free((char *)g_field_refs.e[i].key);
        hmap_free(&g_field_refs);
        free(refs.items);
        return;
    }

    Buf src = { 0 };
    buf_puts(&src, "/* fcc native-backend header probe */\n");
    if (g_cdefs.len) buf_put(&src, g_cdefs.p, g_cdefs.len);
    if (g_cincs.len) buf_put(&src, g_cincs.p, g_cincs.len);
    buf_puts(&src, "#include <stdio.h>\n#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n"
                   "static void hx(const char *s) { while (*s) printf(\"%02x\", (unsigned char)*s++); }\n"
                   "static void bits(const char *k, const char *f, const unsigned char *b, size_t n, int neg) {\n"
                   "    int lo = -1, cnt = 0;\n"
                   "    for (size_t i = 0; i < n * 8; i++) if ((b[i / 8] >> (i % 8)) & 1) { if (lo < 0) lo = (int)i; cnt++; }\n"
                   "    printf(\"B %s %s %d %d %d\\n\", k, f, lo, cnt, neg);\n"
                   "}\n"
                   "int main(void) {\n");
    for (int i = 0; i < refs.count; i++) {
        Type *t = conc(refs.items[i].type);
        const char *c = refs.items[i].cname;
        if (is_cstr_type(t))
            buf_printf(&src, "    printf(\"S %s \"); hx((const char *)(%s)); printf(\"\\n\");\n", c, c);
        else if (type_is_float(t))
            buf_printf(&src, "    printf(\"F %s %%a\\n\", (double)(%s));\n", c, c);
        else if (t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR)
            buf_printf(&src, "    printf(\"U %s %%llu\\n\", (unsigned long long)(uintptr_t)(%s));\n", c, c);
        else if (val_kind(t) == VK_INT && !int_signed(t) && int_width(t) == 8)
            buf_printf(&src, "    printf(\"U %s %%llu\\n\", (unsigned long long)(%s));\n", c, c);
        else
            buf_printf(&src, "    printf(\"C %s %%lld\\n\", (long long)(%s));\n", c, c);
    }
    for (int i = 0; i < ns; i++) {
        Decl *d = structs[i];
        const char *kw = d->struc.is_c_union ? "union" : "struct";
        const char *tag = d->struc.c_name ? d->struc.c_name : d->struc.name;
        char *key = arena_sprintf(g_arena, "%c:%s", d->struc.is_c_union ? 'C' : 'S', tag);
        buf_printf(&src, "    printf(\"L %s %%zu %%zu\\n\", sizeof(%s %s), _Alignof(%s %s));\n",
                   key, kw, tag, kw, tag);
        for (int f = 0; f < d->struc.field_count; f++) {
            const char *fn = d->struc.fields[f].name;
            char *rk = str_sprintf("%s|%s", key, fn);
            bool used = hmap_get(&g_field_refs, rk, strlen(rk)) != NULL;
            free(rk);
            if (!used) continue;
            Type *ft = conc(d->struc.fields[f].type);
            bool scan = ft && (type_is_integer(ft) || ft->kind == TYPE_BOOL || ft->kind == TYPE_ENUM);
            if (scan)
                buf_printf(&src, "    { %s %s s; memset(&s, 0, sizeof s); s.%s = -1; "
                                 "bits(\"%s\", \"%s\", (const unsigned char *)&s, sizeof s, s.%s < 0); }\n",
                           kw, tag, fn, key, fn, fn);
            else
                buf_printf(&src, "    printf(\"O %s %s %%zu %%zu\\n\", offsetof(%s %s, %s), "
                                 "sizeof(((%s %s *)0)->%s));\n",
                           key, fn, kw, tag, fn, kw, tag, fn);
        }
    }
    buf_puts(&src, "    return 0;\n}\n");
    char *text = cc_cached("probe", &src);
    if (text) probe_parse(text);
    free(text);
    buf_free(&src);
    for (size_t i = 0; i < g_field_refs.cap; i++) if (g_field_refs.e[i].used) free((char *)g_field_refs.e[i].key);
    hmap_free(&g_field_refs);
    free(refs.items);
    free(structs);
}

/* ================================================================
 * Driver
 * ================================================================ */

static void flatten(Decl **decls, int n, Decl ***out, int *count, int *cap) {
    for (int i = 0; i < n; i++) {
        if (decls[i]->kind == DECL_MODULE) flatten(decls[i]->module.decls, decls[i]->module.decl_count, out, count, cap);
        else DA_APPEND(*out, *count, *cap, decls[i]);
    }
}

static bool is_generic(Decl *d) {
    switch (d->kind) {
    case DECL_STRUCT: return d->struc.is_generic;
    case DECL_UNION: return d->unio.is_generic;
    case DECL_LET: return d->let.is_generic;
    default: return false;
    }
}

void x86gen_emit(Program *prog, FILE *out, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab) {
    g_mono = mono;
    g_arena = arena;
    g_intern = intern;
    g_symtab = symtab;
    if (g_len_repr != 64) {
        diag_error((SrcLoc){0}, "the native backend supports only --len-repr 64");
        return;
    }

    Decl **all = NULL;
    int n = 0, cap = 0;
    flatten(prog->decls, prog->decl_count, &all, &n, &cap);

    build_cenv(prog, all, n);
    run_probe(prog, all, n, mono);
    if (diag_error_count() > 0) { free_cenv(); free(all); return; }

    /* Globals */
    for (int i = 0; i < n; i++) {
        Decl *d = all[i];
        if (d->kind == DECL_LET && !decl_is_function(d) && !is_generic(d)) emit_global(d);
    }

    /* Functions */
    bool has_main = false;
    ExprList lambdas = { 0 };
    for (int i = 0; i < n; i++) {
        Decl *d = all[i];
        if (d->kind != DECL_LET || is_generic(d)) continue;
        if (d->let.init) collect_lambdas(d->let.init, &lambdas);
        if (!decl_is_function(d)) continue;
        Expr *fn = d->let.init;
        FnSpec f = { 0 };
        f.params = fn->func.params;
        f.nparams = fn->func.param_count;
        f.body = fn->func.body;
        f.body_count = fn->func.body_count;
        if (!d->let.is_module_member && strcmp(d->let.name, "main") == 0) {
            f.sym = "fc_main";
            f.is_main = true;
            f.ret = type_int32();
            has_main = true;
        } else {
            f.sym = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            f.ret = d->let.resolved_type->func.return_type;
            f.has_ctx = true;
        }
        emit_function(&f);
    }
    for (int i = 0; i < lambdas.count; i++) {
        /* The top-level function's own literal is not a lifted lambda. */
        emit_lambda(lambdas.items[i]);
    }
    free(lambdas.items);

    /* Generic instances */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst;
        enter_instance(inst, &subst);
        FnSpec f = { 0 };
        f.sym = inst->mangled_name;
        f.params = fn->func.params;
        f.nparams = fn->func.param_count;
        f.ret = fn->type->func.return_type;
        f.has_ctx = true;
        f.body = fn->func.body;
        f.body_count = fn->func.body_count;
        emit_function(&f);
        ExprList il = { 0 };
        for (int j = 0; j < fn->func.body_count; j++) collect_lambdas(fn->func.body[j], &il);
        for (int j = 0; j < il.count; j++) emit_lambda(il.items[j]);
        free(il.items);
        leave_instance();
    }

    if (has_main) emit_c_main();

    /* Helpers requested along the way, which may request more. */
    int done_eq = 0, done_tr = 0, done_cw = 0;
    while (done_eq < g_eq_n || done_tr < g_tramp_n || done_cw < g_cwrap_n) {
        while (done_cw < g_cwrap_n) emit_cwrap(g_cwraps[done_cw++]);
        while (done_tr < g_tramp_n) emit_trampoline(&g_tramps[done_tr++]);
        while (done_eq < g_eq_n) emit_eq_helper(&g_eqs[done_eq++]);
    }

    char *shim = build_shim();

    /* Assemble the file. */
    fputs("# generated by fcc (native x86-64 backend)\n", out);
    if (g_file_dirs.len) fwrite(g_file_dirs.p, 1, g_file_dirs.len, out);
    if (g_rodata.len) { fputs("\t.section .rodata\n", out); fwrite(g_rodata.p, 1, g_rodata.len, out); }
    if (g_relro.len) { fputs("\t.section .data.rel.ro,\"aw\"\n", out); fwrite(g_relro.p, 1, g_relro.len, out); }
    if (g_data.len) { fputs("\t.data\n", out); fwrite(g_data.p, 1, g_data.len, out); }
    if (g_bss.len) { fputs("\t.bss\n", out); fwrite(g_bss.p, 1, g_bss.len, out); }
    if (g_text.len) fwrite(g_text.p, 1, g_text.len, out);
    if (shim) { fputs("# ---- C shim (compiled by cc) ----\n", out); fputs(shim, out); free(shim); }
    fputs("\t.section .note.GNU-stack,\"\",@progbits\n", out);

    buf_free(&g_fnbody); buf_free(&g_cold); buf_free(&g_text); buf_free(&g_rodata);
    buf_free(&g_relro); buf_free(&g_data); buf_free(&g_bss); buf_free(&g_file_dirs);
    hmap_free(&g_locals); hmap_free(&g_strings); hmap_free(&g_strhdrs); hmap_free(&g_fnvals);
    hmap_free(&g_layouts); hmap_free(&g_files); hmap_free(&g_probe); hmap_free(&g_probed_layouts);
    free(g_spills); g_spills = NULL; g_spill_cap = 0;
    free(g_eqs); g_eqs = NULL; g_eq_n = g_eq_cap = 0;
    free(g_tramps); g_tramps = NULL; g_tramp_n = g_tramp_cap = 0;
    free(g_cwraps); g_cwraps = NULL; g_cwrap_n = g_cwrap_cap = 0;
    free(g_runtime_globals); g_runtime_globals = NULL; g_rg_n = g_rg_cap = 0;
    hmap_free(&g_shim_used);
    free_cenv();
    free(all);
}
