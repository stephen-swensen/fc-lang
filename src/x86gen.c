/* x86gen.c: the native x86-64 backend.
 *
 * An alternative to the C emitter for fast development builds. It walks the
 * same typed, monomorphized AST that codegen.c walks and emits x86-64 machine
 * code for System V, position-independent, as an ELF relocatable object
 * (through x86asm.c), which elflink.c links into an executable, or the C
 * compiler links (`fcc --native -c`, then `cc out.o`). No assembly text is
 * written and no assembler runs. It does not optimize across expressions;
 * its job is to mean exactly what the emitted C means, quickly. Within an
 * expression it takes the short forms: see "Leaves" (operands that take no
 * code), "Holds", cond_jump (branching on the flags), gen_divmod_const and
 * arg_at_call. Across a function it keeps locals in registers: see
 * "Register locals".
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
 * Frame. %rbp-based. Every local binding and parameter that is not kept in
 * a register, and every temporary, has a slot at a fixed negative offset, as
 * do the callee-saved registers the function uses. Temporaries are never
 * reused within a function (monotonic allocation), except the 8-byte spill
 * slots that hold a scalar operand while its sibling is evaluated (when the
 * sibling is not a leaf and may call), which nest strictly and so are
 * reused by depth. Nothing is pushed: %rsp stays 16-byte aligned throughout
 * a body, which keeps calls and `alloca` simple.
 *
 * Calls use the System V ABI everywhere, FC to FC included, so FC functions
 * are callable from C and the debugger. A call in tail position is a jump
 * when nothing in the current frame can be referenced by the callee. */

/* mkdtemp(), mkstemp() and st_mtim under -std=c11. Must precede every #include. */
#define _DEFAULT_SOURCE

#include "x86gen.h"
#include "diag.h"

#if !defined(__linux__) || !defined(__x86_64__)

/* The objects are ELF, the calls name the Linux C library's symbols, and the
 * C header shim runs through /bin/sh, so the backend is built only on Linux
 * x86-64. Elsewhere the rest of fcc is unaffected and --native is an error. */
void x86gen_emit(Program *prog, Obj *obj, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab,
                 const char *const *include_dirs, int ninclude_dirs, bool backtraces) {
    (void)prog; (void)obj; (void)mono; (void)arena; (void)intern; (void)symtab;
    (void)include_dirs; (void)ninclude_dirs; (void)backtraces;
    diag_error_simple("--native is supported only on Linux x86-64");
}

#else /* Linux x86-64: the backend */

#include "x86asm.h"
#include "facts.h"
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

/* ================================================================
 * Text buffers (C sources for the probe and shim, format strings) and
 * the code buffers
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

static Code *g_cur;         /* where instructions go */
static Code g_fnbody;       /* the body of the function being emitted */
static Code g_cold;         /* its out-of-line failure paths */
static Code g_text;         /* finished functions */

/* The instructions by their AT&T mnemonics, operands in AT&T order (source
 * first), into the current buffer. */
#define movq(s, d)      x_mov(g_cur, 8, s, d)
#define movl(s, d)      x_mov(g_cur, 4, s, d)
#define movw(s, d)      x_mov(g_cur, 2, s, d)
#define movb(s, d)      x_mov(g_cur, 1, s, d)
#define movd(s, d)      x_mov(g_cur, 4, s, d)
#define movabsq(v, d)   x_movabs(g_cur, v, d)
#define leaq(s, d)      x_lea(g_cur, s, d)
#define addq(s, d)      x_alu(g_cur, ALU_ADD, 8, s, d)
#define addl(s, d)      x_alu(g_cur, ALU_ADD, 4, s, d)
#define subq(s, d)      x_alu(g_cur, ALU_SUB, 8, s, d)
#define andq(s, d)      x_alu(g_cur, ALU_AND, 8, s, d)
#define andl(s, d)      x_alu(g_cur, ALU_AND, 4, s, d)
#define andb(s, d)      x_alu(g_cur, ALU_AND, 1, s, d)
#define orq(s, d)       x_alu(g_cur, ALU_OR, 8, s, d)
#define orb(s, d)       x_alu(g_cur, ALU_OR, 1, s, d)
#define xorq(s, d)      x_alu(g_cur, ALU_XOR, 8, s, d)
#define xorl(s, d)      x_alu(g_cur, ALU_XOR, 4, s, d)
#define cmpq(s, d)      x_alu(g_cur, ALU_CMP, 8, s, d)
#define cmpl(s, d)      x_alu(g_cur, ALU_CMP, 4, s, d)
#define cmpb(s, d)      x_alu(g_cur, ALU_CMP, 1, s, d)
#define testq(s, d)     x_test(g_cur, 8, s, d)
#define testl(s, d)     x_test(g_cur, 4, s, d)
#define testb(s, d)     x_test(g_cur, 1, s, d)
#define imulq(s, d)     x_imul(g_cur, 8, s, d)
#define imulq3(v, s, d) x_imul3(g_cur, 8, v, s, d)
#define shlq(n, d)      x_shift(g_cur, SH_SHL, 8, n, d)
#define shrq(n, d)      x_shift(g_cur, SH_SHR, 8, n, d)
#define sarq(n, d)      x_shift(g_cur, SH_SAR, 8, n, d)
#define negq(d)         x_unary(g_cur, UN_NEG, 8, d)
#define notq(d)         x_unary(g_cur, UN_NOT, 8, d)
#define incq(d)         x_unary(g_cur, UN_INC, 8, d)
#define mulq(d)         x_unary(g_cur, UN_MUL, 8, d)
#define divq(d)         x_unary(g_cur, UN_DIV, 8, d)
#define idivq(d)        x_unary(g_cur, UN_IDIV, 8, d)
#define movzbl(s, d)    x_movx(g_cur, 1, 4, false, s, d)
#define movzwl(s, d)    x_movx(g_cur, 2, 4, false, s, d)
#define movsbq(s, d)    x_movx(g_cur, 1, 8, true, s, d)
#define movswq(s, d)    x_movx(g_cur, 2, 8, true, s, d)
#define movslq(s, d)    x_movx(g_cur, 4, 8, true, s, d)
#define setcc(cc, d)    x_setcc(g_cur, cc, d)
#define cmovq(cc, s, d) x_cmov(g_cur, cc, 8, s, d)
#define btcq(n, d)      x_btc(g_cur, 8, n, d)
#define sse(op, s, d)   x_sse(g_cur, op, s, d)
#define movss(s, d)     sse(SSE_MOVSS, s, d)
#define movsd(s, d)     sse(SSE_MOVSD, s, d)
#define movaps(s, d)    sse(SSE_MOVAPS, s, d)
#define xorps(s, d)     sse(SSE_XORPS, s, d)
#define addss(s, d)     sse(SSE_ADDSS, s, d)
#define addsd(s, d)     sse(SSE_ADDSD, s, d)
#define subsd(s, d)     sse(SSE_SUBSD, s, d)
#define ucomisd(s, d)   sse(SSE_UCOMISD, s, d)
#define ucomiss(s, d)   sse(SSE_UCOMISS, s, d)
#define cvtss2sd(s, d)  sse(SSE_CVTSS2SD, s, d)
#define cvtsd2ss(s, d)  sse(SSE_CVTSD2SS, s, d)
#define cvttsd2siq(s, d) sse(SSE_CVTTSD2SIQ, s, d)
#define cvtsi2sdq(s, d) sse(SSE_CVTSI2SDQ, s, d)
#define cvtsi2ssq(s, d) sse(SSE_CVTSI2SSQ, s, d)
#define cqo()           x_op0(g_cur, OP_CQO)
#define leave()         x_op0(g_cur, OP_LEAVE)
#define mfence()        x_op0(g_cur, OP_MFENCE)
#define rep_movsb()     x_op0(g_cur, OP_REP_MOVSB)
#define rep_stosb()     x_op0(g_cur, OP_REP_STOSB)
#define jcc(cc, l)      x_jcc(g_cur, cc, l)
#define jmp_sym(name)   (x_jmp(g_cur, asm_sym(name)), g_unreached = g_cur)
#define jmp_plt(name)   (x_jmp_plt(g_cur, asm_sym(name)), g_unreached = g_cur)
#define jmp_ind(t)      (x_jmp_ind(g_cur, t), g_unreached = g_cur)
#define call_sym(name)  x_call(g_cur, asm_sym(name), false)
#define call_plt(name)  x_call(g_cur, asm_sym(name), true)
#define call_ind(t)     x_call_ind(g_cur, t)

/* The buffer whose end control cannot reach: after an unconditional jump,
 * until the next label. A jump there would never run, so none is emitted
 * (a branch whose body ends in break or return, say, needs no jump past
 * the else). */
static Code *g_unreached;

static int new_label(void) { return asm_label(); }

static void label(int l) {
    x_label(g_cur, l);
    if (g_unreached == g_cur) g_unreached = NULL;
}

static void jmp(int l) {
    if (g_unreached == g_cur) return;
    x_jmp(g_cur, l);
    g_unreached = g_cur;
}

/* `off(%rbp)`: a frame slot. */
static Opnd fp(int32_t off) { return M(RBP, off); }

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
static bool g_backtraces;           /* --backtraces: a failure prints the FC call stack */

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

/* Registers. A local whose address is never taken may live in a register
 * instead of a frame slot (see "Register locals"): an integer in a
 * callee-saved register, a float in %xmm8-%xmm15 when no call comes while it
 * is live. g_reg_locals maps its name to the register. An integer register
 * always holds the canonical 64-bit form, so it is read and written whole.
 * The function saves the callee-saved registers it uses in frame slots and
 * restores them before it returns or jumps to a tail callee. */
static HMap g_reg_locals;           /* codegen name -> register, REG_XMM + n for %xmm<n> */
enum { REG_XMM = 16 };

static Opnd reg_home(int64_t v) { return v >= REG_XMM ? X((int)(v - REG_XMM)) : R((Reg)v); }

static const Reg SAVED_REGS[] = { RBX, R12, R13, R14, R15 };
enum { NSAVED = sizeof SAVED_REGS / sizeof SAVED_REGS[0] };
static int g_nsaved;                /* the registers the function uses */
static Reg g_save_reg[NSAVED];
static int32_t g_save_slot[NSAVED];

/* Put back the callee-saved registers the function used. */
static void restore_saved(void) {
    for (int i = 0; i < g_nsaved; i++) movq(fp(g_save_slot[i]), R(g_save_reg[i]));
}

/* Where a local lives: its register, or its frame slot, allocated on first
 * sight. */
static Opnd local_home(const char *name, Type *t) {
    size_t n = strlen(name);
    HEnt *e = hmap_get(&g_reg_locals, name, n);
    if (e) return reg_home(e->val);
    e = hmap_get(&g_locals, name, n);
    if (e) return fp((int32_t)e->val);
    int64_t s, a;
    ty_layout(t, &s, &a);
    int32_t off = val_kind(t) == VK_AGG ? slot_alloc(align_up(s, 8), a > 8 ? a : 8)
                                        : slot_alloc(8, 8);
    hmap_put(&g_locals, name, n)->val = off;
    return fp(off);
}

static bool local_find(const char *name, Opnd *home) {
    size_t n = strlen(name);
    HEnt *e = hmap_get(&g_reg_locals, name, n);
    if (e) { *home = reg_home(e->val); return true; }
    e = hmap_get(&g_locals, name, n);
    if (!e) return false;
    *home = fp((int32_t)e->val);
    return true;
}

static void local_bind(const char *name, int32_t off) {
    hmap_put(&g_locals, name, strlen(name))->val = off;
}

/* ---- Loads, stores, normalization ---- */

/* Re-extend r to canonical form for VK_INT type t. */
static void normalize_reg(Type *t, Reg r) {
    int w = int_width(t);
    bool s = int_signed(t);
    if (conc(t)->kind == TYPE_BOOL) { movzbl(R(r), R(r)); return; }
    switch (w) {
    case 1: (s ? movsbq(R(r), R(r)) : movzbl(R(r), R(r))); break;
    case 2: (s ? movswq(R(r), R(r)) : movzwl(R(r), R(r))); break;
    case 4: (s ? movslq(R(r), R(r)) : movl(R(r), R(r))); break;
    default: break;
    }
}

static void normalize(Type *t) { normalize_reg(t, RAX); }

/* Load the integer of type t at mem (or in a register local) into r,
 * extended to canonical form. */
static void load_int_to(Type *t, Opnd mem, Reg r) {
    int w = int_width(t);
    bool s = int_signed(t);
    if (mem.kind == OPND_REG) {         /* already canonical */
        if (mem.base != (int8_t)r) movq(mem, R(r));
        return;
    }
    switch (w) {
    case 1: (s ? movsbq(mem, R(r)) : movzbl(mem, R(r))); break;
    case 2: (s ? movswq(mem, R(r)) : movzwl(mem, R(r))); break;
    case 4: (s ? movslq(mem, R(r)) : movl(mem, R(r))); break;
    default: movq(mem, R(r)); break;
    }
}

static void load_int(Type *t, Opnd mem) { load_int_to(t, mem, RAX); }

static void store_int(Type *t, Opnd mem) {
    if (mem.kind == OPND_REG) movq(R(RAX), mem);
    else x_mov(g_cur, int_width(t), R(RAX), mem);
}

/* Load the value of type t stored at mem into the value registers (an
 * aggregate's address into %rax). */
static void load_val(Type *t, Opnd mem) {
    switch (val_kind(t)) {
    case VK_INT: load_int(t, mem); break;
    case VK_FLOAT:
        if (mem.kind == OPND_XMM) movaps(mem, X(0));
        else (is_f32(t) ? movss(mem, X(0)) : movsd(mem, X(0)));
        break;
    case VK_AGG: leaq(mem, R(RAX)); break;
    case VK_VOID: break;
    }
}

/* Copy `size` bytes from src_off(%src) to dst_off(%dst). Scratch: %r11, and
 * %rsi/%rdi/%rcx for large copies. */
static void copy_mem(Reg dst, int64_t dst_off, Reg src, int64_t src_off, int64_t size) {
    if (size <= 0) return;
    if (size > 128) {
        leaq(M(src, src_off), R(R11));
        leaq(M(dst, dst_off), R(RDI));
        movq(R(R11), R(RSI));
        movq(IMM(size), R(RCX));
        rep_movsb();
        return;
    }
    int64_t i = 0;
    for (; i + 8 <= size; i += 8) {
        movq(M(src, (src_off + i)), R(R11));
        movq(R(R11), M(dst, (dst_off + i)));
    }
    if (i + 4 <= size) {
        movl(M(src, (src_off + i)), R(R11));
        movl(R(R11), M(dst, (dst_off + i)));
        i += 4;
    }
    if (i + 2 <= size) {
        movw(M(src, (src_off + i)), R(R11));
        movw(R(R11), M(dst, (dst_off + i)));
        i += 2;
    }
    if (i < size) {
        movb(M(src, (src_off + i)), R(R11));
        movb(R(R11), M(dst, (dst_off + i)));
    }
}

/* Zero `size` bytes at off(%base). Large fills clobber %rax, %rcx, %rdi. */
static void zero_mem(Reg base, int64_t off, int64_t size) {
    if (size <= 0) return;
    if (size > 128) {
        leaq(M(base, off), R(RDI));
        xorl(R(RAX), R(RAX));
        movq(IMM(size), R(RCX));
        rep_stosb();
        return;
    }
    int64_t i = 0;
    for (; i + 8 <= size; i += 8) movq(IMM(0), M(base, (off + i)));
    if (i + 4 <= size) { movl(IMM(0), M(base, (off + i))); i += 4; }
    if (i + 2 <= size) { movw(IMM(0), M(base, (off + i))); i += 2; }
    if (i < size) movb(IMM(0), M(base, (off + i)));
}

/* Store the current value (registers, or the aggregate at (%rax)) at mem.
 * For an aggregate, mem must not be RIP-relative, nor based on %rax or %r11. */
static void store_at(Type *t, Opnd mem) {
    switch (val_kind(t)) {
    case VK_INT: store_int(t, mem); break;
    case VK_FLOAT:
        if (mem.kind == OPND_XMM) movaps(X(0), mem);
        else (is_f32(t) ? movss(X(0), mem) : movsd(X(0), mem));
        break;
    case VK_AGG: copy_mem((Reg)mem.base, mem.disp, RAX, 0, ty_size(t)); break;
    case VK_VOID: break;
    }
}

/* Store the current value into off(%base). The base must not be %rax for an
 * aggregate, nor %r11. */
static void store_val(Type *t, Reg base, int64_t off) { store_at(t, M(base, off)); }

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

static void load_window_r11(int w, Opnd mem) {
    switch (w) {
    case 1: movzbl(mem, R(R11)); break;
    case 2: movzwl(mem, R(R11)); break;
    case 4: movl(mem, R(R11)); break;
    default: movq(mem, R(R11)); break;
    }
}

/* Load the field at (struct_off, field_off) from `base` into %rax as its FC
 * type. The base must not be %rax, %rcx, %rdi or %r11. */
static void load_ext_field(Type *ft, ExtField *x, Reg base, int64_t struct_off, int64_t field_off) {
    if (!x->is_bit) {
        Opnd mem = M(base, field_off);
        switch (x->csize) {
        case 1: (x->csigned ? movsbq(mem, R(RAX)) : movzbl(mem, R(RAX))); break;
        case 2: (x->csigned ? movswq(mem, R(RAX)) : movzwl(mem, R(RAX))); break;
        case 4: (x->csigned ? movslq(mem, R(RAX)) : movl(mem, R(RAX))); break;
        default: movq(mem, R(RAX)); break;
        }
        normalize(ft);
        return;
    }
    int64_t wlo; int w, shift;
    bit_window(x, &wlo, &w, &shift);
    load_window_r11(w, M(base, struct_off + wlo));
    if (shift) shrq(IMM(shift), R(R11));
    int up = 64 - (int)x->bit_width;
    if (up > 0) {
        shlq(IMM(up), R(R11));
        (x->csigned ? sarq(IMM(up), R(R11)) : shrq(IMM(up), R(R11)));
    }
    movq(R(R11), R(RAX));
    normalize(ft);
}

/* Store %rax (an FC value) into the field. Same register rules. */
static void store_ext_field(ExtField *x, Reg base, int64_t struct_off, int64_t field_off) {
    if (!x->is_bit) {
        x_mov(g_cur, (int)x->csize, R(RAX), M(base, field_off));
        return;
    }
    int64_t wlo; int w, shift;
    bit_window(x, &wlo, &w, &shift);
    Opnd mem = M(base, struct_off + wlo);
    int up = 64 - (int)x->bit_width;
    movq(R(RAX), R(RCX));
    if (up > 0) { shlq(IMM(up), R(RCX)); shrq(IMM(up), R(RCX)); }
    if (shift) shlq(IMM(shift), R(RCX));
    load_window_r11(w, mem);
    uint64_t m = (x->bit_width >= 64 ? ~0ULL : ((1ULL << x->bit_width) - 1)) << shift;
    movabsq(~m, R(RDI));
    andq(R(RDI), R(R11));
    orq(R(RCX), R(R11));
    switch (w) {
    case 1: movb(R(R11), mem); break;
    case 2: movw(R(R11), mem); break;
    case 4: movl(R(R11), mem); break;
    default: movq(R(R11), mem); break;
    }
}

static bool fits_imm32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

/* Load an immediate into r. */
static void mov_imm_to(int64_t v, Reg r) {
    if (v == 0) xorl(R(r), R(r));
    else if (fits_imm32(v)) movq(IMM(v), R(r));
    else if (v > 0 && v <= (int64_t)UINT32_MAX) movl(IMM(v), R(r));
    else movabsq(v, R(r));
}

/* Load an immediate into %rax. */
static void mov_imm(int64_t v) { mov_imm_to(v, RAX); }

/* r *= n, for an element size or a count: a power of two shifts. */
static void scale_reg(int64_t n, Reg r) {
    int k = 0;
    while (k < 63 && n != (int64_t)1 << k) k++;
    if (k == 0) return;
    if (k < 63) shlq(IMM(k), R(r));
    else imulq3(n, R(r), R(r));
}

static void mov_imm_reg(int64_t v, Opnd reg64) {
    if (v >= INT32_MIN && v <= INT32_MAX) movq(IMM(v), reg64);
    else movabsq(v, reg64);
}

static HMap g_fconsts;      /* 'f' or 'd' and the bits -> label */

/* The label of a float constant in .rodata. */
static int fconst_label(double v, bool f32, uint64_t *bits) {
    char key[9];
    if (f32) {
        float f = (float)v;
        uint32_t b;
        memcpy(&b, &f, 4);
        *bits = b;
    } else {
        memcpy(bits, &v, 8);
    }
    key[0] = f32 ? 'f' : 'd';
    memcpy(key + 1, bits, 8);
    HEnt *e = hmap_get(&g_fconsts, key, 9);
    if (e) return (int)e->val;
    char *k = arena_alloc(g_arena, 9);
    memcpy(k, key, 9);
    int l = new_label();
    d_align(ASM_RODATA, f32 ? 4 : 8);
    d_def(ASM_RODATA, l);
    d_int(ASM_RODATA, *bits, f32 ? 4 : 8);
    hmap_put(&g_fconsts, k, 9)->val = l;
    return l;
}

/* A float constant as an operand: in .rodata. */
static Opnd fconst(double v, bool f32) {
    uint64_t bits;
    return RIPREL(fconst_label(v, f32, &bits), 0);
}

/* Load a float constant into %xmm<n>. */
static void float_const_to(double v, bool f32, int n) {
    uint64_t bits;
    int l = fconst_label(v, f32, &bits);
    if (bits == 0) xorps(X(n), X(n));
    else (f32 ? movss(RIPREL(l, 0), X(n)) : movsd(RIPREL(l, 0), X(n)));
}

static void float_const(double v, bool f32) { float_const_to(v, f32, 0); }

/* ---- Snapshots: hold the current value while other code runs ---- */

typedef struct { ValKind k; int32_t slot; } Snap;

static Snap snap_save(Type *t) {
    Snap s = { val_kind(t), 0 };
    switch (s.k) {
    case VK_INT: s.slot = spill_push(); movq(R(RAX), fp(s.slot)); break;
    case VK_FLOAT: s.slot = spill_push(); movq(X(0), fp(s.slot)); break;
    case VK_AGG:
        s.slot = temp_alloc(t);
        copy_mem(RBP, s.slot, RAX, 0, ty_size(t));
        break;
    case VK_VOID: break;
    }
    return s;
}

static void snap_restore(Snap s) {
    switch (s.k) {
    case VK_INT: movq(fp(s.slot), R(RAX)); spill_pop(); break;
    case VK_FLOAT: movq(fp(s.slot), X(0)); spill_pop(); break;
    case VK_AGG: leaq(fp(s.slot), R(RAX)); break;
    case VK_VOID: break;
    }
}

/* ================================================================
 * Read-only data: strings, string headers, function values
 * ================================================================ */

static HMap g_strings;      /* bytes -> label */
static HMap g_strhdrs;      /* bytes -> header label */
static HMap g_fnvals;       /* symbol -> function-value label */

/* The label of a NUL-terminated copy of `n` bytes in .rodata. */
static int str_label(const unsigned char *s, int n) {
    HEnt *e = hmap_get(&g_strings, (const char *)s, (size_t)n);
    if (e) return (int)e->val;
    char *k = arena_alloc(g_arena, (size_t)n + 1);
    memcpy(k, s, (size_t)n);
    k[n] = 0;
    int l = new_label();
    d_def(ASM_RODATA, l);
    d_bytes(ASM_RODATA, k, n + 1);
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
    d_align(ASM_RELRO, 8);
    d_def(ASM_RELRO, l);
    d_addr(ASM_RELRO, sl, 0);
    d_int(ASM_RELRO, (uint64_t)n, 8);
    hmap_put(&g_strhdrs, k, (size_t)n)->val = l;
    return l;
}

/* A function value { code, NULL } for a code symbol. */
static int fnval_label(const char *sym) {
    HEnt *e = hmap_get(&g_fnvals, sym, strlen(sym));
    if (e) return (int)e->val;
    int l = new_label();
    d_align(ASM_RELRO, 8);
    d_def(ASM_RELRO, l);
    d_addr(ASM_RELRO, asm_sym(sym), 0);
    d_zero(ASM_RELRO, 8);
    hmap_put(&g_fnvals, sym, strlen(sym))->val = l;
    return l;
}

/* The declared error codes' names, { ptr, len } each, indexed by code -
 * FC_ERROR_CODE_BASE; the names are NUL-terminated too. */
static int g_errnames_label = 0;

static int errnames_label(void) {
    if (g_errnames_label) return g_errnames_label;
    int n = error_code_count();
    g_errnames_label = new_label();
    int *labels = xcalloc((size_t)(n ? n : 1), sizeof(int));
    for (int i = 0; i < n; i++) labels[i] = cstr_label(error_code_info(i).qualified);
    d_align(ASM_RELRO, 8);
    d_def(ASM_RELRO, g_errnames_label);
    for (int i = 0; i < n; i++) {
        d_addr(ASM_RELRO, labels[i], 0);
        d_int(ASM_RELRO, strlen(error_code_info(i).qualified), 8);
    }
    if (n == 0) d_zero(ASM_RELRO, 8);
    free(labels);
    return g_errnames_label;
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
static Code *g_cur_saved;

#define BACKTRACE_SYM "fc_dump_backtrace"

/* Abort, under --backtraces after printing the FC call stack. */
static void call_abort(void) {
    if (g_backtraces) call_sym(BACKTRACE_SYM);
    call_plt("abort");
}

static int fail_begin(void) {
    int l = new_label();
    g_cur_saved = g_cur;
    g_cur = &g_cold;
    label(l);
    return l;
}

static void fail_end(int msg) {
    movq(GOTREL(asm_sym("stderr")), R(RDI));
    movq(M(RDI, 0), R(RDI));
    leaq(RIPREL(msg, 0), R(RSI));
    xorl(R(RAX), R(RAX));
    call_plt("fprintf");
    call_abort();
    g_cur = g_cur_saved;
}

/* A failure path with no arguments; returns the label to jump to. */
static int fail_simple(SrcLoc loc, const char *rest) {
    int m = msg_label(loc, rest);
    int l = fail_begin();
    fail_end(m);
    return l;
}

/* ---- Source lines ---- */

static HMap g_files;
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
        asm_file(id, loc.filename);
    }
    if (id == g_loc_file && loc.line == g_loc_line) return;
    g_loc_file = id;
    g_loc_line = loc.line;
    x_loc(g_cur, id, loc.line);
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
static int xp_sym(const char *cname);
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

/* ---- Leaves: operands that take no code to evaluate ----
 * A binary operator evaluates its left operand, then its right. When the
 * right one is a constant or a read of storage at a fixed place, it can be
 * read after the left operand's code has run, in that order, without a spill
 * to keep the left value meanwhile, and often by the instruction itself. */

static bool const_num(Expr *e, bool *isf, int64_t *iv, double *fv);

/* normalize(t) applied to v. */
static int64_t normalized(int64_t v, Type *t) {
    if (conc(t)->kind == TYPE_BOOL) return v & 0xff;
    bool s = int_signed(t);
    switch (int_width(t)) {
    case 1: return s ? (int64_t)(int8_t)v : (int64_t)(uint8_t)v;
    case 2: return s ? (int64_t)(int16_t)v : (int64_t)(uint16_t)v;
    case 4: return s ? (int64_t)(int32_t)v : (int64_t)(uint32_t)v;
    default: return v;
    }
}

/* An integer constant that takes no code but its load (a literal, a sizeof,
 * an enum variant, a type property, a C constant, a const type parameter,
 * or an unchecked integer conversion of one, such as the widening a literal
 * gets from its context): the value gen_expr loads into %rax. Arithmetic on
 * constants is left to the run time, which checks it. */
static bool int_leaf(Expr *e, int64_t *v) {
    switch (e->kind) {
    case EXPR_GUARD: {      /* it governs a conversion inside */
        bool sg = g_guards_suppressed, so = g_overflow_checked;
        if (e->guard.is_overflow_axis) g_overflow_checked = e->guard.enable;
        else g_guards_suppressed = !e->guard.enable;
        bool ok = int_leaf(e->guard.body, v);
        g_guards_suppressed = sg;
        g_overflow_checked = so;
        return ok;
    }
    case EXPR_CAST: {
        Type *from = conc(e->cast.operand->type), *to = conc(e->cast.target);
        if (val_kind(from) != VK_INT || val_kind(to) != VK_INT) return false;
        if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) return false;
        if (!int_leaf(e->cast.operand, v)) return false;
        *v = normalized(*v, to);
        return true;
    }
    case EXPR_TYPE_VAR_REF: {
        if (!e->type_var_ref.is_const_param) return false;
        Type tv = { 0 };
        tv.kind = TYPE_TYPE_VAR;
        tv.type_var.name = e->type_var_ref.name;
        Type *b = subst_resolve(&tv);
        if (!b || b->kind != TYPE_CONST_INT) return false;
        *v = b->const_int.value;
        return true;
    }
    case EXPR_IDENT:
        if (e->ident.is_std_stream) return false;
        /* fall through */
    case EXPR_INT_LIT: case EXPR_BOOL_LIT: case EXPR_CHAR_LIT:
    case EXPR_SIZEOF: case EXPR_ALIGNOF: case EXPR_FIELD: {
        bool isf;
        double fv;
        if (val_kind(e->type) != VK_INT || !const_num(e, &isf, v, &fv) || isf) return false;
        if (e->kind == EXPR_FIELD) *v = canon_const((uint64_t)*v, e->type);
        return true;
    }
    default:
        return false;
    }
}

/* Whether every value of integer type from is one of type to, so both have
 * the same canonical form and the conversion is free (and cannot fail). */
static bool int_widens(Type *from, Type *to) {
    if (conc(to)->kind == TYPE_BOOL) return conc(from)->kind == TYPE_BOOL;
    int wf = int_width(from), wt = int_width(to);
    if (int_signed(from)) return int_signed(to) && wt >= wf;
    return wt > wf || (wt == wf && !int_signed(to));
}

/* Storage at a fixed place: a local or a global, a field or tuple element of
 * one, or the ptr or len of a slice there, or a widening of an integer
 * there; or the same through a pointer in a register local. *m addresses it
 * (or is the register of a register local) and *t is the type gen_expr loads
 * from it, so reading it is that one load. */
static bool fixed_place(Expr *e, Opnd *m, Type **t) {
    while (e->kind == EXPR_GUARD) e = e->guard.body;
    switch (e->kind) {
    case EXPR_CAST: {
        Type *from = conc(e->cast.operand->type), *to = conc(e->cast.target);
        return val_kind(from) == VK_INT && val_kind(to) == VK_INT && int_widens(from, to) &&
               fixed_place(e->cast.operand, m, t);
    }
    case EXPR_IDENT: {
        if (e->ident.is_std_stream || fn_static_target(e)) return false;
        const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
        if (e->ident.is_local) {
            if (!local_find(name, m)) return false;
        } else {
            Symbol *s = e->ident.resolved_sym;
            if (s && s->decl && s->decl->kind == DECL_EXTERN) return false;
            *m = RIPREL(asm_sym(name), 0);
        }
        *t = e->type;
        return true;
    }
    case EXPR_FIELD: {
        if (e->field.is_type_property || e->field.is_variant_constructor || e->field.fixed_array_type)
            return false;
        if (e->field.codegen_name) {
            if (fn_static_target(e) || e->field.is_extern_const) return false;
            *m = RIPREL(asm_sym(e->field.codegen_name), 0);
            *t = e->type;
            return true;
        }
        Type *ot = conc(e->field.object->type), *objt;
        if (!fixed_place(e->field.object, m, &objt) || m->kind != OPND_MEM) return false;
        if (ot->kind == TYPE_SLICE) {
            if (val_kind(e->type) != VK_INT || int_width(e->type) != 8) return false;
            if (strcmp(e->field.name, "len") == 0) m->disp += 8;
            *t = e->type;
            return true;
        }
        int64_t off;
        ExtField *x;
        if (!field_info_x(ot, e->field.name, &off, t, &x) || x) return false;
        m->disp += off;
        return true;
    }
    case EXPR_DEREF_FIELD: {        /* through a pointer in a register local */
        if (e->field.fixed_array_type) return false;
        Type *objt, *st = conc(conc(e->field.object->type)->pointer.pointee);
        if (!fixed_place(e->field.object, m, &objt) || m->kind != OPND_REG) return false;
        Reg base = (Reg)m->base;
        if (st->kind == TYPE_SLICE) {
            if (val_kind(e->type) != VK_INT || int_width(e->type) != 8) return false;
            *m = M(base, strcmp(e->field.name, "len") == 0 ? 8 : 0);
            *t = e->type;
            return true;
        }
        int64_t off;
        ExtField *x;
        if (!field_info_x(st, e->field.name, &off, t, &x) || x) return false;
        *m = M(base, off);
        return true;
    }
    case EXPR_INDEX: {
        Type *ot = conc(e->index.object->type), *objt, *ft;
        if (ot->kind != TYPE_STRUCT || !ot->struc.is_tuple) return false;
        char nm[24];
        snprintf(nm, sizeof nm, "e%llu", (unsigned long long)e->index.index->int_lit.value);
        int64_t off;
        if (!field_info(ot, intern_cstr(g_intern, nm), &off, &ft)) return false;
        if (!fixed_place(e->index.object, m, &objt) || m->kind != OPND_MEM) return false;
        m->disp += off;
        *t = e->type;
        return true;
    }
    default:
        return false;
    }
}

/* Whether e is an integer leaf: a constant, or a fixed place. */
static bool is_int_leaf(Expr *e) {
    int64_t v;
    Opnd m;
    Type *t;
    return int_leaf(e, &v) || (fixed_place(e, &m, &t) && val_kind(t) == VK_INT);
}

/* Load an integer leaf into r; false, with nothing emitted, if e is not
 * one. */
static bool leaf_to(Expr *e, Reg r) {
    int64_t v;
    Opnd m;
    Type *t;
    if (int_leaf(e, &v)) { mov_imm_to(v, r); return true; }
    if (fixed_place(e, &m, &t) && val_kind(t) == VK_INT) { load_int_to(t, m, r); return true; }
    return false;
}

/* An integer leaf as an instruction's source operand: an immediate that
 * sign-extends from 32 bits, an 8-byte fixed place, or a register local. */
static bool leaf_src(Expr *e, Opnd *o) {
    int64_t v;
    Type *t;
    if (int_leaf(e, &v)) {
        if (!fits_imm32(v)) return false;
        *o = IMM(v);
        return true;
    }
    return fixed_place(e, o, &t) && val_kind(t) == VK_INT &&
           (int_width(t) == 8 || o->kind == OPND_REG);
}

/* Load a float leaf (a literal, or a fixed place) into %xmm<n>. Scratch:
 * %r11. */
static bool float_leaf_to(Expr *e, int n) {
    Opnd m;
    Type *t;
    while (e->kind == EXPR_GUARD) e = e->guard.body;
    if (e->kind == EXPR_FLOAT_LIT) {
        float_const_to(e->float_lit.value, e->float_lit.lit_type->kind == TYPE_FLOAT32, n);
        return true;
    }
    if (!fixed_place(e, &m, &t) || val_kind(t) != VK_FLOAT) return false;
    if (m.kind == OPND_XMM) { if (m.base != n) movaps(m, X(n)); }
    else (is_f32(t) ? movss(m, X(n)) : movsd(m, X(n)));
    return true;
}

/* Put a source operand into %rcx, where an instruction needs it there. */
static void src_to_rcx(Opnd o) {
    if (o.kind == OPND_IMM) mov_imm_to(o.disp, RCX);
    else if (!(o.kind == OPND_REG && o.base == RCX)) movq(o, R(RCX));
}

/* ---- Holds: a value kept while its sibling is evaluated ----
 * Code that makes no call leaves %r8-%r10 and %xmm2-%xmm4 alone (calls use
 * them for arguments), so a value held across such code stays in one of
 * them, nested holds in the next; else it goes to a spill slot. */

static const Reg HOLD_REGS[] = { R10, R9, R8 };
enum { NHOLD = sizeof HOLD_REGS / sizeof HOLD_REGS[0] };
static int g_hold_n;

/* Whether evaluating e makes no call (but on a failure path, which does not
 * return): a conservative test. Nothing it accepts assigns either, so code
 * it accepts has no effect, and a read may move across it. */
static bool call_free(Expr *e) {
    switch (e->kind) {
    case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_BOOL_LIT: case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT: case EXPR_CSTRING_LIT: case EXPR_IDENT: case EXPR_SIZEOF:
    case EXPR_ALIGNOF: case EXPR_TYPE_VAR_REF:
        return true;
    case EXPR_GUARD:
        return call_free(e->guard.body);
    case EXPR_CAST: {
        Type *from = conc(e->cast.operand->type), *to = conc(e->cast.target);
        if ((is_str_type(from) && is_cstr_type(to)) || (is_cstr_type(from) && is_str_type(to))) return false;
        return call_free(e->cast.operand);
    }
    case EXPR_BINARY:
        if ((e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) && needs_eq_func(e->binary.left->type))
            return false;
        return call_free(e->binary.left) && call_free(e->binary.right);
    case EXPR_UNARY_PREFIX:
        return call_free(e->unary_prefix.operand);
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        return !e->field.object || call_free(e->field.object);
    case EXPR_INDEX:
        return call_free(e->index.object) && call_free(e->index.index);
    default:
        return false;
    }
}

/* Hold %rax (%xmm0 when is_float) while `sibling` is evaluated; hold_take
 * gives the place to read it back from, and hold_pop ends the hold. */
static Opnd hold_push(Expr *sibling, bool is_float) {
    Opnd h;
    if (g_hold_n < NHOLD && call_free(sibling)) {
        h = is_float ? X(2 + g_hold_n) : R(HOLD_REGS[g_hold_n]);
        g_hold_n++;
    } else {
        h = fp(spill_push());
    }
    if (is_float) (h.kind == OPND_XMM ? movaps(X(0), h) : movq(X(0), h));
    else movq(R(RAX), h);
    return h;
}

static void hold_pop(Opnd h) {
    if (h.kind == OPND_MEM) spill_pop();
    else g_hold_n--;
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
    leaq(RIPREL(fnval_label(sym), 0), R(RAX));
}

/* A static function as a C function pointer: an FC function's trampoline
 * (no context argument), or an extern's own symbol. */
static void gen_c_fn_pointer(Expr *e) {
    bool is_extern;
    const char *sym = static_fn_sym(e, &is_extern);
    const Expr *tg = fn_static_target(e);
    const Symbol *ts = !tg ? NULL : tg->kind == EXPR_IDENT ? tg->ident.resolved_sym
                     : tg->kind == EXPR_FIELD ? tg->field.resolved_member : NULL;
    if (is_extern && ts && extern_via_shim(ts->decl)) movq(RIPREL(xp_sym(shim_ref(sym)), 0), R(RAX));
    else if (is_extern) movq(GOTREL(asm_sym(sym)), R(RAX));
    else leaq(RIPREL(asm_sym(ctramp_sym(sym, conc(e->type))), 0), R(RAX));
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
        leaq(RIPREL((int)iv, 0), R(RAX));
    } else mov_imm(canon_const((uint64_t)iv, t));
}

static void gen_ident(Expr *e) {
    if (e->ident.is_std_stream) {
        movq(GOTREL(asm_sym(e->ident.name)), R(RAX));
        movq(M(RAX, 0), R(RAX));
        return;
    }
    if (!e->ident.is_local && fn_static_target(e)) {
        gen_static_fn_value(e);
        return;
    }
    const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
    if (e->ident.is_local) {
        Opnd home;
        if (!local_find(name, &home)) {
            internal_error(e->loc, "unknown local '%s'", name);
            return;
        }
        load_val(e->type, home);
        return;
    }
    Symbol *s = e->ident.resolved_sym;
    if (s && s->decl && s->decl->kind == DECL_EXTERN) {
        gen_extern_const(s->decl->ext.name, e->type, e->loc);
        return;
    }
    load_val(e->type, RIPREL(asm_sym(name), 0));
}

/* ---- Address of an lvalue ---- */

static Opnd gen_slice_elem(Expr *e, bool sib);
static Opnd gen_ptr_elem(Expr *e, bool sib);

static void gen_addr(Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT: {
        const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
        if (e->ident.is_local) {
            Opnd home;
            if (!local_find(name, &home)) { internal_error(e->loc, "unknown local '%s'", name); return; }
            if (home.kind != OPND_MEM) { internal_error(e->loc, "the address of register local '%s'", name); return; }
            leaq(home, R(RAX));
        } else {
            leaq(RIPREL(asm_sym(name), 0), R(RAX));
        }
        return;
    }
    case EXPR_FIELD: {
        if (e->field.codegen_name) { leaq(RIPREL(asm_sym(e->field.codegen_name), 0), R(RAX)); return; }
        int64_t off; Type *ft; ExtField *x;
        gen_expr(e->field.object);
        if (field_info_x(e->field.object->type, e->field.name, &off, &ft, &x)) {
            if (x) internal_error(e->loc, "the address of C field '%s' (a bit field, or a different width)", e->field.name);
            if (off) leaq(M(RAX, off), R(RAX));
        } else internal_error(e->loc, "no field '%s'", e->field.name);
        return;
    }
    case EXPR_DEREF_FIELD: {
        int64_t off; Type *ft; ExtField *x;
        gen_expr(e->field.object);
        Type *pt = conc(e->field.object->type);
        if (field_info_x(pt->pointer.pointee, e->field.name, &off, &ft, &x)) {
            if (x) internal_error(e->loc, "the address of C field '%s' (a bit field, or a different width)", e->field.name);
            if (off) leaq(M(RAX, off), R(RAX));
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
                leaq(M(RAX, off), R(RAX));
            return;
        }
        if (ot->kind == TYPE_SLICE) gen_slice_elem(e, false);
        else gen_ptr_elem(e, false);
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

static bool sib_scale(int64_t es) { return es == 1 || es == 2 || es == 4 || es == 8; }

/* s[i] with the bounds check: the element's place, at (%rax) or, with sib,
 * indexed by %rax from %rcx. */
/* The storage of a struct's fixed-array field f when it needs no code to
 * find: in a local or a global, or through a pointer in a register local. */
static bool fixed_array_place(Expr *f, Opnd *m) {
    Type *t, *st = conc(f->field.object->type);
    if (f->kind == EXPR_DEREF_FIELD) st = conc(st->pointer.pointee);
    int64_t off;
    Type *ft;
    ExtField *x;
    if (!field_info_x(st, f->field.name, &off, &ft, &x) || x) return false;
    if (!fixed_place(f->field.object, m, &t)) return false;
    if (f->kind == EXPR_DEREF_FIELD) {
        if (m->kind != OPND_REG) return false;
        *m = M((Reg)m->base, off);
    } else {
        if (m->kind != OPND_MEM) return false;
        m->disp += off;
    }
    return true;
}

/* The index into a register, returned: %rax, or the register local that
 * holds it. The container's place is in *at: evaluated first when it takes
 * code (eval: its address or value into %rax), else just found. The place
 * is then based on %rdx. */
static Reg index_operands(Expr *e, Opnd *at, bool found, void (*eval)(Expr *)) {
    if (found) {
        Opnd m;
        Type *t;
        if (fixed_place(e->index.index, &m, &t) && m.kind == OPND_REG) return (Reg)m.base;
        gen_expr(e->index.index);
        return RAX;
    }
    eval(e->index.object);
    if (is_int_leaf(e->index.index)) {
        movq(R(RAX), R(RDX));
        leaf_to(e->index.index, RAX);
    } else {
        Opnd h = hold_push(e->index.index, false);
        gen_expr(e->index.index);
        movq(h, R(RDX));
        hold_pop(h);
    }
    *at = M(RDX, 0);
    return RAX;
}

/* The bounds check of the index in ix against len (an operand). */
static void index_check(Expr *e, Reg ix, Opnd len) {
    if (g_guards_suppressed && facts_guard_governs(e, subst_resolve)) return;
    bool uns = !int_signed(e->index.index->type);
    if (len.kind == OPND_IMM && !fits_imm32(len.disp)) {
        movabsq(len.disp, R(RCX));
        len = R(RCX);
    }
    cmpq(len, R(ix));
    int m = msg_label(e->loc, uns ? "slice index out of range: index=%llu len=%llu\n"
                                  : "slice index out of range: index=%lld len=%llu\n");
    int l = fail_begin();
    if (len.kind == OPND_IMM) mov_imm_to(len.disp, RCX);
    else if (!(len.kind == OPND_REG && len.base == RCX)) movq(len, R(RCX));
    movq(R(ix), R(RDX));
    fail_end(m);
    jcc(CC_AE, l);
}

/* The container is evaluated before the index. One found without code is
 * found after it instead, which is the same when the index has no effect
 * (call_free), or when the container is storage at a fixed address. */
static Opnd gen_slice_elem(Expr *e, bool sib) {
    Type *ot = conc(e->index.object->type), *ht;
    int64_t es = ty_size(ot->slice.elem);
    Expr *obj = e->index.object;
    bool pure = call_free(e->index.index);
    while (obj->kind == EXPR_GUARD) obj = obj->guard.body;
    if ((obj->kind == EXPR_FIELD || obj->kind == EXPR_DEREF_FIELD) && obj->field.fixed_array_type) {
        /* A fixed-array field: indexed in place, its length a constant. In a
         * local or a global it stays where it is; through a pointer, the
         * index might change the pointer. */
        Opnd arr;
        bool found = fixed_array_place(obj, &arr) && (pure || arr.base == RBP || arr.base == RIP);
        Reg ix = index_operands(e, &arr, found, gen_addr);
        index_check(e, ix, IMM(fixarr_size(obj->field.fixed_array_type)));
        if (arr.base != RIP && arr.index < 0 && sib_scale(es)) {
            Opnd el = MI((Reg)arr.base, ix, (int)es, arr.disp);
            if (sib) return el;
            leaq(el, R(RAX));
            return M(RAX, 0);
        }
        if (ix != RAX) movq(R(ix), R(RAX));
        scale_reg(es, RAX);
        leaq(arr, R(RCX));
        addq(R(RCX), R(RAX));
        return M(RAX, 0);
    }
    Opnd hdr;       /* the slice: its ptr, then its len */
    Reg ix = RAX;
    if (pure) {
        ix = index_operands(e, &hdr, fixed_place(e->index.object, &hdr, &ht), gen_expr);
    } else {
        /* The index might change the slice where it is: a copy first. */
        gen_expr(e->index.object);
        hdr = fp(temp_alloc(ot));
        copy_mem(RBP, hdr.disp, RAX, 0, 16);
        gen_expr(e->index.index);
    }
    Opnd len = hdr;
    len.disp += 8;
    index_check(e, ix, len);
    if (sib && sib_scale(es)) {
        movq(hdr, R(RCX));
        return MI(RCX, ix, (int)es, 0);
    }
    if (ix != RAX) movq(R(ix), R(RAX));
    scale_reg(es, RAX);
    addq(hdr, R(RAX));
    return M(RAX, 0);
}

/* p[i], unchecked: the element's place, at (%rax) or, with sib, indexed. */
static Opnd gen_ptr_elem(Expr *e, bool sib) {
    int64_t es = ty_size(conc(e->index.object->type)->pointer.pointee);
    gen_expr(e->index.object);
    if (leaf_to(e->index.index, RCX)) {
        if (sib && sib_scale(es)) return MI(RAX, RCX, (int)es, 0);
        scale_reg(es, RCX);
        addq(R(RCX), R(RAX));
        return M(RAX, 0);
    }
    Opnd h = hold_push(e->index.index, false);
    gen_expr(e->index.index);
    scale_reg(es, RAX);
    addq(h, R(RAX));
    hold_pop(h);
    return M(RAX, 0);
}

/* ---- Fields ---- */

static void gen_field(Expr *e) {
    Opnd m;
    Type *mt;
    if (fixed_place(e, &m, &mt)) { load_val(mt, m); return; }
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
        load_val(e->type, RIPREL(asm_sym(e->field.codegen_name), 0));
        return;
    }
    if (e->field.is_variant_constructor) {
        Type *t = conc(e->type);
        if (t->kind == TYPE_ENUM) { mov_imm(enum_variant_value(t, e->field.name)); return; }
        int32_t tmp = temp_alloc(t);
        zero_mem(RBP, tmp, ty_size(t));
        movl(IMM(union_variant_index(t, e->field.name)), fp(tmp));
        leaq(fp(tmp), R(RAX));
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
        movq(R(RAX), fp(tmp));
        movq(IMM(fixarr_size(fat)), fp(tmp + 8));
        leaq(fp(tmp), R(RAX));
        return;
    }
    if (ot->kind == TYPE_OPTION) {
        bool some = strcmp(e->field.name, "is_some") == 0;
        gen_expr(e->field.object);
        if (is_null_sentinel(ot)) testq(R(RAX), R(RAX));
        else cmpb(IMM(0), M(RAX, ty_size(ot->option.inner)));
        (some ? setcc(CC_NE, R(RAX)) : setcc(CC_E, R(RAX)));
        movzbl(R(RAX), R(RAX));
        return;
    }
    if (ot->kind == TYPE_RESULT) {
        bool okf = strcmp(e->field.name, "is_ok") == 0;
        gen_expr(e->field.object);
        cmpl(IMM(0), M(RAX, 0));
        (okf ? setcc(CC_E, R(RAX)) : setcc(CC_NE, R(RAX)));
        movzbl(R(RAX), R(RAX));
        return;
    }
    if (ot->kind == TYPE_SLICE) {
        gen_expr(e->field.object);
        (strcmp(e->field.name, "len") == 0 ? movq(M(RAX, 8), R(RAX)) : movq(M(RAX, 0), R(RAX)));
        return;
    }
    int64_t off; Type *ft; ExtField *x;
    gen_expr(e->field.object);
    if (!field_info_x(ot, e->field.name, &off, &ft, &x)) {
        internal_error(e->loc, "no field '%s' in %s", e->field.name, type_name(ot));
        return;
    }
    if (x) { movq(R(RAX), R(RDX)); load_ext_field(ft, x, RDX, 0, off); return; }
    load_val(ft, M(RAX, off));
}

static void gen_deref_field(Expr *e) {
    Type *pt = conc(e->field.object->type);
    Type *st = conc(pt->pointer.pointee);
    if (e->field.fixed_array_type) {
        gen_addr(e);
        int32_t tmp = slot_alloc(16, 8);
        movq(R(RAX), fp(tmp));
        movq(IMM(fixarr_size(e->field.fixed_array_type)), fp(tmp + 8));
        leaq(fp(tmp), R(RAX));
        return;
    }
    gen_expr(e->field.object);
    if (st->kind == TYPE_SLICE) {
        (strcmp(e->field.name, "len") == 0 ? movq(M(RAX, 8), R(RAX)) : movq(M(RAX, 0), R(RAX)));
        return;
    }
    int64_t off; Type *ft; ExtField *x;
    if (!field_info_x(st, e->field.name, &off, &ft, &x)) {
        internal_error(e->loc, "no field '%s'", e->field.name);
        return;
    }
    if (x) { movq(R(RAX), R(RDX)); load_ext_field(ft, x, RDX, 0, off); return; }
    load_val(ft, M(RAX, off));
}

/* ---- Binary operators ---- */

/* The condition a comparison operator tests, or -1. */
static int cond_for(int op, bool sgn) {
    switch (op) {
    case TOK_EQEQ: return CC_E;
    case TOK_BANGEQ: return CC_NE;
    case TOK_LT: return sgn ? CC_L : CC_B;
    case TOK_LTEQ: return sgn ? CC_LE : CC_BE;
    case TOK_GT: return sgn ? CC_G : CC_A;
    case TOK_GTEQ: return sgn ? CC_GE : CC_AE;
    default: return -1;
    }
}

static void overflow_fail_jump(SrcLoc loc, const char *what, Cond cc) {
    char *rest = str_sprintf("integer overflow in %s\n", what);
    int l = fail_simple(loc, rest);
    free(rest);
    jcc(cc, l);
}

/* Evaluate two operands: left into %rax, right into %rcx (ints), or left into
 * %xmm0 and right into %xmm1 (floats). */
static void gen_operands(Expr *l, Expr *r) {
    if (val_kind(l->type) == VK_FLOAT) {
        gen_expr(l);
        if (float_leaf_to(r, 1)) return;
        Opnd h = hold_push(r, true);
        gen_expr(r);
        movaps(X(0), X(1));
        (h.kind == OPND_XMM ? movaps(h, X(0)) : movq(h, X(0)));
        hold_pop(h);
        return;
    }
    int64_t v;
    if (int_leaf(l, &v)) {      /* a constant: nothing can change it meanwhile */
        gen_expr(r);
        movq(R(RAX), R(RCX));
        mov_imm(v);
        return;
    }
    gen_expr(l);
    if (leaf_to(r, RCX)) return;
    Opnd h = hold_push(r, false);
    gen_expr(r);
    movq(R(RAX), R(RCX));
    movq(h, R(RAX));
    hold_pop(h);
}

/* x / c and x % c for a constant c that is neither 0 nor, signed, -1: no
 * check can fail, and a power of two takes shifts instead of a division.
 * The 64-bit arithmetic on the canonical form gives the narrow type's
 * result in canonical form. */
static void gen_divmod_const(int op, bool sgn, Type *rt, uint64_t c) {
    int k = 0;
    while (k < 64 && c != (uint64_t)1 << k) k++;
    bool div = op == TOK_SLASH;
    if (k == 0) {                               /* c == 1 */
        if (!div) xorl(R(RAX), R(RAX));
        return;
    }
    if (k < 64 && !sgn) {
        if (div) shrq(IMM(k), R(RAX));
        else if (k < 32) andq(IMM((int64_t)(c - 1)), R(RAX));
        else { mov_imm_to((int64_t)(c - 1), RCX); andq(R(RCX), R(RAX)); }
        return;
    }
    if (k < 63) {
        /* Signed: rounding toward zero adds c - 1 to a negative x first.
         * %rdx = x < 0 ? c - 1 : 0. */
        movq(R(RAX), R(RDX));
        sarq(IMM(63), R(RDX));
        shrq(IMM(64 - k), R(RDX));
        addq(R(RDX), R(RAX));
        if (div) {
            sarq(IMM(k), R(RAX));
        } else {
            if (k < 32) andq(IMM((int64_t)(c - 1)), R(RAX));
            else { mov_imm_to((int64_t)(c - 1), RCX); andq(R(RCX), R(RAX)); }
            subq(R(RDX), R(RAX));
        }
        return;
    }
    mov_imm_to((int64_t)c, RCX);
    if (sgn) { cqo(); idivq(R(RCX)); }
    else { xorl(R(RDX), R(RDX)); divq(R(RCX)); }
    if (!div) movq(R(RDX), R(RAX));
    normalize(rt);
}

static void gen_divmod(Expr *e, Type *rt) {
    int op = e->binary.op;
    bool sgn = int_signed(rt);
    int64_t c;
    if (int_leaf(e->binary.right, &c) && c != 0 && !(sgn && c == -1)) {
        gen_expr(e->binary.left);
        gen_divmod_const(op, sgn, rt, (uint64_t)c);
        return;
    }
    gen_operands(e->binary.left, e->binary.right);
    bool need_zero = !(g_guards_suppressed && facts_guard_governs(e, subst_resolve));
    if (need_zero) {
        int l = fail_simple(e->loc, op == TOK_SLASH ? "divide by zero\n" : "modulo by zero\n");
        testq(R(RCX), R(RCX));
        jcc(CC_E, l);
    }
    if (!sgn) {
        xorl(R(RDX), R(RDX));
        divq(R(RCX));
        if (op == TOK_PERCENT) movq(R(RDX), R(RAX));
        normalize(rt);
        return;
    }
    int lnorm = new_label(), ldone = new_label();
    cmpq(IMM(-1), R(RCX));
    jcc(CC_NE, lnorm);
    if (op == TOK_PERCENT) {
        xorl(R(RAX), R(RAX));
    } else {
        if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
            int64_t mn; double fv; bool isf;
            type_prop_value(conc(rt), "min", &mn, &fv, &isf);
            mov_imm_reg(mn, R(RDX));
            cmpq(R(RDX), R(RAX));
            overflow_fail_jump(e->loc, "divide", CC_E);
        }
        negq(R(RAX));
        normalize(rt);
    }
    jmp(ldone);
    label(lnorm);
    cqo();
    idivq(R(RCX));
    if (op == TOK_PERCENT) movq(R(RDX), R(RAX));
    normalize(rt);
    label(ldone);
}

/* A float leaf as an instruction's source operand: a float register local,
 * a fixed place, or a literal from the constant pool. */
static bool float_src(Expr *e, Opnd *o) {
    Type *t;
    while (e->kind == EXPR_GUARD) e = e->guard.body;
    if (e->kind == EXPR_FLOAT_LIT) {
        *o = fconst(e->float_lit.value, e->float_lit.lit_type->kind == TYPE_FLOAT32);
        return true;
    }
    return fixed_place(e, o, &t) && val_kind(t) == VK_FLOAT;
}

/* Evaluate two float operands: the left into %xmm0, and the right as the
 * instruction's source (float_src) when it is a leaf, else into %xmm1.
 * With want_reg the source must be a register: an instruction comparing
 * the other way round takes it as the destination. */
static Opnd gen_float_operands(Expr *l, Expr *r, bool want_reg) {
    Opnd o;
    if (float_src(r, &o) && (!want_reg || o.kind == OPND_XMM)) {
        gen_expr(l);
        return o;
    }
    gen_operands(l, r);
    return X(1);
}

static void gen_float_binary(Expr *e) {
    int op = e->binary.op;
    bool f = is_f32(e->binary.left->type);
    Opnd src = gen_float_operands(e->binary.left, e->binary.right, op == TOK_LT || op == TOK_LTEQ);
    switch (op) {
    case TOK_PLUS: sse(f ? SSE_ADDSS : SSE_ADDSD, src, X(0)); return;
    case TOK_MINUS: sse(f ? SSE_SUBSS : SSE_SUBSD, src, X(0)); return;
    case TOK_STAR: sse(f ? SSE_MULSS : SSE_MULSD, src, X(0)); return;
    case TOK_SLASH: sse(f ? SSE_DIVSS : SSE_DIVSD, src, X(0)); return;
    default: break;
    }
    SseOp uc = f ? SSE_UCOMISS : SSE_UCOMISD;
    switch (op) {
    case TOK_EQEQ:
        sse(uc, src, X(0)); setcc(CC_E, R(RAX)); setcc(CC_NP, R(RCX)); andb(R(RCX), R(RAX));
        break;
    case TOK_BANGEQ:
        sse(uc, src, X(0)); setcc(CC_NE, R(RAX)); setcc(CC_P, R(RCX)); orb(R(RCX), R(RAX));
        break;
    case TOK_GT: sse(uc, src, X(0)); setcc(CC_A, R(RAX)); break;
    case TOK_GTEQ: sse(uc, src, X(0)); setcc(CC_AE, R(RAX)); break;
    case TOK_LT: sse(uc, X(0), src); setcc(CC_A, R(RAX)); break;
    case TOK_LTEQ: sse(uc, X(0), src); setcc(CC_AE, R(RAX)); break;
    default:
        internal_error(e->loc, "float operator %s", token_kind_name(op));
        return;
    }
    movzbl(R(RAX), R(RAX));
}

/* %rax op= src for +, - and *. */
static void gen_arith(int op, Opnd src) {
    if (op == TOK_PLUS) addq(src, R(RAX));
    else if (op == TOK_MINUS) subq(src, R(RAX));
    else if (src.kind == OPND_IMM) imulq3(src.disp, R(RAX), R(RAX));
    else imulq(src, R(RAX));
}

/* Evaluate an integer operator's operands: the left into %rax, and the right
 * as the instruction's source, an immediate or a fixed place when it is a
 * leaf, else %rcx. */
static Opnd gen_int_operands(Expr *l, Expr *r) {
    Opnd o;
    if (leaf_src(r, &o)) {
        gen_expr(l);
        return o;
    }
    gen_operands(l, r);
    return R(RCX);
}

/* Compare two integer operands: the flags for cond_for's condition. */
static void gen_int_compare(Expr *l, Expr *r) {
    Opnd lr, o;
    Type *t;
    if (fixed_place(l, &lr, &t) && lr.kind == OPND_REG && leaf_src(r, &o)) {
        if (o.kind == OPND_IMM && o.disp == 0) testq(lr, lr);
        else cmpq(o, lr);
        return;
    }
    o = gen_int_operands(l, r);
    if (o.kind == OPND_IMM && o.disp == 0) testq(R(RAX), R(RAX));
    else cmpq(o, R(RAX));
}

static void gen_binary_op(Expr *e) {
    int op = e->binary.op;
    Type *lt = conc(e->binary.left->type);
    Type *rt = conc(e->type);

    if ((op == TOK_EQEQ || op == TOK_BANGEQ) && needs_eq_func(lt)) {
        gen_expr(e->binary.left);
        Snap sn = snap_save(lt);   /* the right operand may change the left's storage */
        gen_expr(e->binary.right);
        movq(R(RAX), R(RSI));
        leaq(fp(sn.slot), R(RDI));
        call_sym(eq_helper(lt));
        movzbl(R(RAX), R(RAX));
        if (op == TOK_BANGEQ) xorl(IMM(1), R(RAX));
        return;
    }
    if (val_kind(lt) == VK_FLOAT) { gen_float_binary(e); return; }

    if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
        int lend = new_label();
        gen_expr(e->binary.left);
        testq(R(RAX), R(RAX));
        (op == TOK_AMPAMP ? jcc(CC_E, lend) : jcc(CC_NE, lend));
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
            subq(R(RCX), R(RAX));
            if (es > 1) { movq(IMM(es), R(RCX)); cqo(); idivq(R(RCX)); }
            return;
        }
        if (lt->kind == TYPE_POINTER) {
            int64_t es = ty_size(lt->pointer.pointee);
            scale_reg(es, RCX);
            (op == TOK_PLUS ? addq(R(RCX), R(RAX)) : subq(R(RCX), R(RAX)));
        } else {
            int64_t es = ty_size(rtt->pointer.pointee);
            scale_reg(es, RAX);
            addq(R(RCX), R(RAX));
        }
        return;
    }

    int cc = cond_for(op, int_signed(lt));
    if (cc >= 0) {
        gen_int_compare(e->binary.left, e->binary.right);
        setcc((Cond)cc, R(RAX));
        movzbl(R(RAX), R(RAX));
        return;
    }

    switch (op) {
    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: {
        Opnd src = gen_int_operands(e->binary.left, e->binary.right);
        const char *what = op == TOK_PLUS ? "+" : op == TOK_MINUS ? "-" : "*";
        bool checked = g_overflow_checked && facts_overflow_governs(e, subst_resolve);
        if (checked && int_width(rt) == 8) {
            if (int_signed(rt)) {
                gen_arith(op, src);
                overflow_fail_jump(e->loc, what, CC_O);
            } else if (op == TOK_STAR) {
                src_to_rcx(src);
                mulq(R(RCX));
                overflow_fail_jump(e->loc, what, CC_O);
            } else {
                gen_arith(op, src);
                overflow_fail_jump(e->loc, what, CC_B);
            }
            return;
        }
        int k = 0;
        if (!checked && op == TOK_STAR && src.kind == OPND_IMM && src.disp > 1)
            while (k < 31 && src.disp != (int64_t)1 << k) k++;
        if (k > 0 && k < 31) shlq(IMM(k), R(RAX));
        else gen_arith(op, src);
        if (checked) {
            movq(R(RAX), R(RDX));
            normalize(rt);
            cmpq(R(RAX), R(RDX));
            overflow_fail_jump(e->loc, what, CC_NE);
        } else {
            normalize(rt);
        }
        return;
    }
    case TOK_SLASH: case TOK_PERCENT:
        gen_divmod(e, rt);
        return;
    case TOK_AMP: case TOK_PIPE: case TOK_CARET: {
        Opnd src = gen_int_operands(e->binary.left, e->binary.right);
        (op == TOK_AMP ? andq(src, R(RAX)) : (op == TOK_PIPE ? orq(src, R(RAX)) : xorq(src, R(RAX))));
        return;
    }
    case TOK_LTLT: case TOK_GTGT: {
        int bits = int_width(rt) * 8;
        int64_t n;
        Opnd count = R(RCX);
        if (int_leaf(e->binary.right, &n)) {
            gen_expr(e->binary.left);
            count = IMM(n & (bits - 1));
        } else {
            gen_operands(e->binary.left, e->binary.right);
            andl(IMM(bits - 1), R(RCX));
        }
        if (op == TOK_LTLT) {
            shlq(count, R(RAX));
            normalize(rt);
        } else {
            (int_signed(rt) ? sarq(count, R(RAX)) : shrq(count, R(RAX)));
        }
        return;
    }
    default:
        internal_error(e->loc, "binary operator %s", token_kind_name(op));
    }
}

/* In a generic instance, widen the operands of e to their common type for
 * the operation; binary_unwiden puts the originals back. */
static void binary_widen(Expr *e, Expr *unw[2]) {
    unw[0] = unw[1] = NULL;
    if (g_subst && e->binary.op != TOK_LTLT && e->binary.op != TOK_GTGT) {
        Type *common = type_common_numeric(subst_resolve(e->binary.left->type),
                                           subst_resolve(e->binary.right->type));
        unw[0] = widen_for_instance(&e->binary.left, common);
        unw[1] = widen_for_instance(&e->binary.right, common);
    }
}

static void binary_unwiden(Expr *e, Expr *unw[2]) {
    if (unw[0]) e->binary.left = unw[0];
    if (unw[1]) e->binary.right = unw[1];
}

static void gen_binary(Expr *e) {
    Expr *unw[2];
    binary_widen(e, unw);
    gen_binary_op(e);
    binary_unwiden(e, unw);
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
        load_val(e->type, M(RAX, 0));
        return;
    }
    gen_expr(e->unary_prefix.operand);
    switch (op) {
    case TOK_MINUS:
        if (val_kind(rt) == VK_FLOAT) {
            if (is_f32(rt)) { movd(X(0), R(RAX)); xorl(IMM(0x80000000), R(RAX)); movd(R(RAX), X(0)); }
            else { movq(X(0), R(RAX)); btcq(63, R(RAX)); movq(R(RAX), X(0)); }
            return;
        }
        if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
            if (int_width(rt) == 8 && int_signed(rt)) {
                negq(R(RAX));
                overflow_fail_jump(e->loc, "negation", CC_O);
            } else {
                negq(R(RAX));
                movq(R(RAX), R(RDX));
                normalize(rt);
                cmpq(R(RAX), R(RDX));
                overflow_fail_jump(e->loc, "negation", CC_NE);
            }
            return;
        }
        negq(R(RAX));
        normalize(rt);
        return;
    case TOK_BANG:
        xorl(IMM(1), R(RAX));
        return;
    case TOK_TILDE:
        notq(R(RAX));
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
        movq(fp(g_hidden_ret), R(RDI));
        copy_mem(RDI, 0, RAX, 0, ty_size(t));
        movq(fp(g_hidden_ret), R(RAX));
        return;
    }
    movq(R(RAX), R(R11));
    int ni = 0, ns = 0;
    for (int k = 0; k < a.n; k++) {
        if (a.cls[k] == 'I') movq(M(R11, 8 * k), R(ni++ ? RDX : RAX));
        else movq(M(R11, 8 * k), X(ns++ ? 1 : 0));
    }
}

/* Build a function's "none" or "err(code)" failure return in place: the code
 * (for a result) is in %ecx. Leaves the value as move_to_return_regs wants. */
static void gen_failure_value(Type *ret) {
    ret = conc(ret);
    if (ret->kind == TYPE_RESULT) {
        int32_t t = temp_alloc(ret);
        int32_t code = slot_alloc(8, 8);
        movq(R(RCX), fp(code));      /* a large zero fill clobbers %rcx */
        zero_mem(RBP, t, ty_size(ret));
        movq(fp(code), R(RCX));
        movl(R(RCX), fp(t));
        leaq(fp(t), R(RAX));
    } else if (is_null_sentinel(ret)) {
        xorl(R(RAX), R(RAX));
    } else {
        int32_t t = temp_alloc(ret);
        zero_mem(RBP, t, ty_size(ret));
        leaq(fp(t), R(RAX));
    }
}

static void gen_unary_postfix(Expr *e) {
    Type *ot = conc(e->unary_postfix.operand->type);
    if (e->unary_postfix.op == TOK_BANG) {
        char *text = e->unary_postfix.expr_text
            ? fmt_escape(e->unary_postfix.expr_text, e->unary_postfix.expr_text_len) : str_dup("");
        gen_expr(e->unary_postfix.operand);
        if (ot->kind == TYPE_RESULT) {
            /* Under --backtraces with declared errors, the code's name too. */
            bool named = g_backtraces && error_code_count() > 0;
            char *rest = str_sprintf(named ? "unwrap failed: %s (error code %%lld%%s%%s)\n"
                                           : "unwrap failed: %s (error code %%lld)\n", text);
            int m = msg_label(e->loc, rest);
            free(rest);
            int l = fail_begin();
            movslq(M(RAX, 0), R(RDX));
            if (named) {
                /* %rcx, %r8: ": " and the name, or "" and "" for a code with none */
                int lnone = new_label();
                leaq(RIPREL(cstr_label(""), 0), R(RCX));
                movq(R(RCX), R(R8));
                movq(R(RDX), R(RAX));
                subq(IMM(FC_ERROR_CODE_BASE), R(RAX));
                cmpq(IMM(error_code_count()), R(RAX));
                jcc(CC_AE, lnone);
                shlq(IMM(4), R(RAX));
                leaq(RIPREL(errnames_label(), 0), R(R8));
                movq(MI(R8, RAX, 1, 0), R(R8));
                leaq(RIPREL(cstr_label(": "), 0), R(RCX));
                label(lnone);
            }
            fail_end(m);
            cmpl(IMM(0), M(RAX, 0));
            jcc(CC_NE, l);
            Type *in = conc(ot->result.inner);
            if (in && in->kind != TYPE_VOID) {
                load_val(in, M(RAX, result_value_off(ot)));
            }
        } else {
            char *rest = str_sprintf("unwrap failed: %s\n", text);
            int l = fail_simple(e->loc, rest);
            free(rest);
            if (is_null_sentinel(ot)) {
                testq(R(RAX), R(RAX));
                jcc(CC_E, l);
            } else {
                cmpb(IMM(0), M(RAX, ty_size(ot->option.inner)));
                jcc(CC_E, l);
                load_val(ot->option.inner, M(RAX, 0));
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
        movl(M(RAX, 0), R(RCX));
        testl(R(RCX), R(RCX));
        jcc(CC_E, lok);
    } else if (is_null_sentinel(ot)) {
        testq(R(RAX), R(RAX));
        jcc(CC_NE, lok);
    } else {
        cmpb(IMM(0), M(RAX, ty_size(ot->option.inner)));
        jcc(CC_NE, lok);
    }
    /* The failure branch. */
    if (has_pending_defers()) {
        int32_t s = spill_push();
        movq(R(RCX), fp(s));
        emit_defers_to_func();
        movq(fp(s), R(RCX));
        spill_pop();
    }
    gen_failure_value(e->unary_postfix.prop_fn_ret ? e->unary_postfix.prop_fn_ret : g_fn_ret);
    move_to_return_regs();
    jmp(g_ret_label);
    label(lok);
    if (ot->kind == TYPE_RESULT) {
        Type *in = conc(ot->result.inner);
        if (in && in->kind != TYPE_VOID) {
            load_val(in, M(RAX, result_value_off(ot)));
        }
    } else if (!is_null_sentinel(ot)) {
        load_val(ot->option.inner, M(RAX, 0));
    }
}

/* ================================================================
 * Calls (System V)
 * ================================================================ */

typedef struct {
    Type *type;     /* concrete type as passed */
    int32_t slot;   /* frame offset: an 8-byte scalar, or the aggregate's bytes */
    bool zero;      /* the constant 0 (a null context) */
    Expr *leaf;     /* a scalar read when the call is made (arg_at_call) */
    bool in_acc;    /* a scalar left in %rax or %xmm0: the last one evaluated */
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

static const Reg IREG[6] = { RDI, RSI, RDX, RCX, R8, R9 };

static bool assign_pred(Expr *e, void *ctx) {
    (void)ctx;
    return e->kind == EXPR_ASSIGN || expr_any_child(e, assign_pred, NULL);
}

static HMap g_addr_taken;          /* locals whose address is taken: name -> 1 */
static bool g_addr_known;           /* g_addr_taken is filled for this function */

/* Whether argument i of n can be read when the call is made instead of in
 * its turn: a constant, or a scalar local whose address is not taken (so no
 * call can change it) and which no later argument assigns. */
static bool arg_at_call(Expr **args, int i, int n) {
    Expr *a = args[i];
    ValKind k = val_kind(a->type);
    int64_t v;
    Opnd m;
    Type *t;
    if (k == VK_INT && int_leaf(a, &v)) return true;
    Expr *b = a;
    while (b->kind == EXPR_GUARD || b->kind == EXPR_CAST)
        b = b->kind == EXPR_GUARD ? b->guard.body : b->cast.operand;
    if (k == VK_FLOAT && b->kind == EXPR_FLOAT_LIT && b == a) return true;
    if (b->kind != EXPR_IDENT || !b->ident.is_local || !g_addr_known) return false;
    if (k != VK_INT && k != VK_FLOAT) return false;
    if (!fixed_place(a, &m, &t) || val_kind(t) != k) return false;
    const char *name = b->ident.codegen_name ? b->ident.codegen_name : b->ident.name;
    if (hmap_get(&g_addr_taken, name, strlen(name))) return false;
    for (int j = i + 1; j < n; j++)
        if (args[j]->kind == EXPR_ASSIGN || expr_any_child(args[j], assign_pred, NULL)) return false;
    return true;
}

/* Evaluate an argument into a fresh slot, or not at all when it is read at
 * the call (at_call), or into %rax or %xmm0 when it is the last argument
 * evaluated (last). Returns true when it took a spill slot (the caller pops
 * it after the call). */
static bool eval_arg(Expr *a, CallArg *ca, bool at_call, bool last) {
    Type *t = conc(a->type);
    ca->type = t;
    ca->zero = false;
    ca->slot = 0;
    ca->leaf = NULL;
    ca->in_acc = false;
    if (at_call) { ca->leaf = a; return false; }
    gen_expr(a);
    if (last && (val_kind(t) == VK_INT || val_kind(t) == VK_FLOAT)) { ca->in_acc = true; return false; }
    switch (val_kind(t)) {
    case VK_INT: ca->slot = spill_push(); movq(R(RAX), fp(ca->slot)); return true;
    case VK_FLOAT: ca->slot = spill_push(); movq(X(0), fp(ca->slot)); return true;
    case VK_AGG:
        ca->slot = temp_alloc(t);
        copy_mem(RBP, ca->slot, RAX, 0, ty_size(t));
        return false;
    case VK_VOID: return false;
    }
    return false;
}

/* Place the current %rax value as a scalar argument. */
static void arg_from_rax(CallArg *ca, Type *t) {
    ca->type = t;
    ca->zero = false;
    ca->leaf = NULL;
    ca->in_acc = false;
    ca->slot = slot_alloc(8, 8);
    movq(R(RAX), fp(ca->slot));
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
    if (g_hold_n) internal_error((SrcLoc){0}, "a call while a value is held in a register");
    if (total) subq(IMM(total), R(RSP));
    /* Stack arguments first, through %r11, which leaves the value in %rax or
     * %xmm0 (in_acc) alone. */
    for (int i = 0; i < n; i++) {
        if (where[i] != 2) continue;
        Opnd at = M(RSP, soff[i]);
        if (args[i].zero) { movq(IMM(0), at); continue; }
        ValKind k = val_kind(args[i].type);
        if (args[i].in_acc) { (k == VK_FLOAT ? movq(X(0), at) : movq(R(RAX), at)); continue; }
        if (args[i].leaf && k == VK_INT) { leaf_to(args[i].leaf, R11); movq(R(R11), at); continue; }
        if (args[i].leaf) {
            Expr *lf = args[i].leaf;
            Opnd m;
            Type *ft;
            while (lf->kind == EXPR_GUARD) lf = lf->guard.body;
            if (lf->kind == EXPR_FLOAT_LIT) {
                double d = lf->float_lit.value;
                if (is_f32(args[i].type)) { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); movl(IMM(b), R(R11)); }
                else { uint64_t b; memcpy(&b, &d, 8); movabsq((int64_t)b, R(R11)); }
            } else if (fixed_place(lf, &m, &ft)) {
                (is_f32(ft) ? movl(m, R(R11)) : movq(m, R(R11)));
            }
            movq(R(R11), at);
            continue;
        }
        if (k == VK_AGG)
            copy_mem(RSP, soff[i], RBP, args[i].slot, align_up(ty_size(args[i].type), 8));
        else {
            movq(fp(args[i].slot), R(R11));
            movq(R(R11), at);
        }
    }
    /* Register arguments: the one in %rax or %xmm0 first, since loading the
     * others may use those. */
    int *reg = xcalloc((size_t)(n > 0 ? n : 1), sizeof(int));
    ni = mem_ret ? 1 : 0;
    ns = 0;
    for (int i = 0; i < n; i++) {
        if (where[i] != 1) continue;
        if (args[i].zero) { reg[i] = ni++; continue; }
        ValKind k = val_kind(args[i].type);
        if (k == VK_INT) { reg[i] = ni++; continue; }
        if (k == VK_FLOAT) { reg[i] = ns++; continue; }
        for (int q = 0; q < ab[i].n; q++) (ab[i].cls[q] == 'I' ? ni++ : ns++);
    }
    for (int i = 0; i < n; i++) {
        if (where[i] != 1 || !args[i].in_acc) continue;
        if (val_kind(args[i].type) == VK_FLOAT) { if (reg[i]) movaps(X(0), X(reg[i])); }
        else movq(R(RAX), R(IREG[reg[i]]));
    }
    ni = mem_ret ? 1 : 0;
    ns = 0;
    for (int i = 0; i < n; i++) {
        if (where[i] != 1) continue;
        if (args[i].zero) { xorl(R(IREG[ni]), R(IREG[ni])); ni++; continue; }
        ValKind k = val_kind(args[i].type);
        if (args[i].in_acc) { (k == VK_FLOAT ? ns++ : ni++); continue; }
        if (args[i].leaf) {
            if (k == VK_FLOAT) float_leaf_to(args[i].leaf, ns++);
            else leaf_to(args[i].leaf, IREG[ni++]);
            continue;
        }
        if (k == VK_INT) { movq(fp(args[i].slot), R(IREG[ni++])); continue; }
        if (k == VK_FLOAT) { movq(fp(args[i].slot), X(ns++)); continue; }
        for (int q = 0; q < ab[i].n; q++) {
            if (ab[i].cls[q] == 'I') movq(fp(args[i].slot + 8 * q), R(IREG[ni++]));
            else movq(fp(args[i].slot + 8 * q), X(ns++));
        }
    }
    free(reg);
    if (mem_ret) {
        if (tail) movq(fp(g_hidden_ret), R(RDI));
        else leaq(fp(res), R(RDI));
    }
    if (!cs->sym) movq(fp(cs->fn_slot), R(R11));
    if (cs->variadic) movl(IMM(ns), R(RAX));
    free(where); free(soff); free(ab);
    if (tail) {
        restore_saved();
        leave();
        x_cfi(g_cur, CFI_LEFT);
        if (!cs->sym) jmp_ind(R(R11));
        else if (cs->shim) jmp_ind(RIPREL(xp_sym(shim_ref(cs->sym)), 0));
        else if (cs->plt) jmp_plt(cs->sym);
        else jmp_sym(cs->sym);
        x_cfi(g_cur, CFI_RESTORE);
        return true;
    }
    if (!cs->sym) call_ind(R(R11));
    else if (cs->shim) call_ind(RIPREL(xp_sym(shim_ref(cs->sym)), 0));
    else if (cs->plt) call_plt(cs->sym);
    else call_sym(cs->sym);
    if (total) addq(IMM(total), R(RSP));
    switch (val_kind(cs->ret)) {
    case VK_INT: normalize(cs->ret); break;
    case VK_FLOAT: case VK_VOID: break;
    case VK_AGG:
        if (mem_ret) { leaq(fp(res), R(RAX)); break; }
        if (ra.n == 0) { int32_t t = slot_alloc(8, 8); leaq(fp(t), R(RAX)); break; }
        {
            int32_t t = temp_alloc(cs->ret);
            int ii = 0, ss = 0;
            for (int q = 0; q < ra.n; q++) {
                if (ra.cls[q] == 'I') movq(R(ii++ ? RDX : RAX), fp(t + 8 * q));
                else movq(X(ss++ ? 1 : 0), fp(t + 8 * q));
            }
            leaq(fp(t), R(RAX));
        }
        break;
    }
    return false;
}

/* A runtime library call with arguments already in registers. */
static void call_c(const char *sym) {
    if (g_hold_n) internal_error((SrcLoc){0}, "a call to %s while a value is held in a register", sym);
    call_plt(sym);
}

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
    movq(R(RAX), fp(raw));         /* a large zero fill clobbers %rax */
    zero_mem(RBP, t, ty_size(ret));
    movq(fp(raw), R(RAX));
    if (proto == EXT_PROTO_STATUS) {
        movl(R(RAX), fp(t));
        leaq(fp(t), R(RAX));
        return;
    }
    if (proto == EXT_PROTO_NEG_ERRNO || proto == EXT_PROTO_HRESULT) {
        int lok = new_label(), ld = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_NS, lok);
        movl(R(RAX), fp(t));
        jmp(ld);
        label(lok);
        if (!pay_void) store_val(pay, RBP, t + voff);
        label(ld);
        leaq(fp(t), R(RAX));
        return;
    }
    if (proto == EXT_PROTO_ERRNO_NEG1 || proto == EXT_PROTO_ERRNO_NULL) {
        int lok = new_label(), ld = new_label();
        if (proto == EXT_PROTO_ERRNO_NULL) testq(R(RAX), R(RAX));
        else cmpq(IMM(-1), R(RAX));
        jcc(CC_NE, lok);
        call_c("__errno_location");
        movl(M(RAX, 0), R(RAX));
        int l = fail_simple(e->loc, "err() with code 0 (0 is the ok tag; error codes must be non-zero)\n");
        testl(R(RAX), R(RAX));
        jcc(CC_E, l);
        movl(R(RAX), fp(t));
        jmp(ld);
        label(lok);
        if (!pay_void) store_val(pay, RBP, t + voff);
        label(ld);
        leaq(fp(t), R(RAX));
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
        zero_mem(RBP, t, L->size);
        gen_expr(e->call.args[0]);
        Type *pt = union_variant_payload(ut, fn->field.name);
        store_val(pt ? pt : e->call.args[0]->type, RBP, t + L->payload_off);
        movl(IMM(idx), fp(t));
        leaq(fp(t), R(RAX));
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
    bool *at = xcalloc((size_t)n + 1, sizeof(bool));
    int last = -1;          /* the last argument evaluated in its turn */
    for (int i = 0; i < n; i++) {
        Type *pt = (e->call.is_extern_call && ft && ft->kind == TYPE_FUNC && i < ft->func.param_count)
            ? conc(ft->func.param_types[i]) : NULL;
        bool own = e->call.is_extern_call && (pt ? pt->kind == TYPE_FUNC || pt->kind == TYPE_OPTION
                                                 : is_f32(e->call.args[i]->type));
        at[i] = !own && arg_at_call(e->call.args, i, n);    /* `own`: the C forms below */
        if (!at[i]) last = i;
    }
    int nspill = 0;
    CallSpec cs = { 0 };
    cs.ret = conc(e->type);
    cs.tail = want_tail;
    bool did_tail = false;
    if (e->call.is_indirect) {
        gen_expr(fn);
        int32_t fs = slot_alloc(16, 8);
        copy_mem(RBP, fs, RAX, 0, 16);
        for (int i = 0; i < n; i++) nspill += eval_arg(e->call.args[i], &args[i], at[i], i == last);
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
                    else xorl(R(RAX), R(RAX));
                    arg_from_rax(&args[i], type_any_ptr());
                } else if (!pt && is_f32(a->type)) {
                    /* A variadic float travels as double. */
                    gen_expr(a);
                    cvtss2sd(X(0), X(0));
                    args[i].type = type_float64();
                    args[i].slot = slot_alloc(8, 8);
                    movq(X(0), fp(args[i].slot));
                } else {
                    nspill += eval_arg(a, &args[i], at[i], i == last);
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
            for (int i = 0; i < n; i++) nspill += eval_arg(e->call.args[i], &args[i], at[i], i == last);
            args[n].zero = true;
            args[n].type = type_any_ptr();
            cs.sym = name;
            did_tail = emit_call(&cs, args, n + 1);
        }
    }
    while (nspill-- > 0) spill_pop();
    free(args);
    free(at);
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

static void dbl_to_xmm1(double v) { float_const_to(v, false, 1); }

/* Truncate the double in %xmm0 (in range for `to`) into %rax. */
static void cvt_double_to_int(Type *to) {
    Type *t = conc(to);
    if (t->kind == TYPE_UINT64 || t->kind == TYPE_USIZE) {
        int lbig = new_label(), ld = new_label();
        dbl_to_xmm1(9223372036854775808.0);
        ucomisd(X(1), X(0));
        jcc(CC_AE, lbig);
        cvttsd2siq(X(0), R(RAX));
        jmp(ld);
        label(lbig);
        subsd(X(1), X(0));
        cvttsd2siq(X(0), R(RAX));
        btcq(63, R(RAX));
        label(ld);
        return;
    }
    cvttsd2siq(X(0), R(RAX));
    normalize(t);
}

static void gen_float_to_int(Type *from, Type *to, bool saturate) {
    if (is_f32(from)) cvtss2sd(X(0), X(0));
    if (!saturate) { cvt_double_to_int(to); return; }
    F2i fi = f2i_info(to);
    int lnan = new_label(), llo = new_label(), lhi = new_label(), ld = new_label();
    ucomisd(X(0), X(0));
    jcc(CC_P, lnan);
    dbl_to_xmm1(fi.lo);
    ucomisd(X(1), X(0));
    jcc(CC_B, llo);
    dbl_to_xmm1(fi.hi);
    ucomisd(X(1), X(0));
    jcc(CC_AE, lhi);
    cvt_double_to_int(to);
    jmp(ld);
    label(lnan);
    xorl(R(RAX), R(RAX));
    jmp(ld);
    label(llo);
    mov_imm(canon_const((uint64_t)fi.imin, to));
    jmp(ld);
    label(lhi);
    mov_imm(canon_const((uint64_t)fi.imax, to));
    label(ld);
}

static void gen_int_to_float(Type *from, Type *to) {
    bool f = is_f32(to);
    Type *ft = conc(from);
    if (ft->kind == TYPE_UINT64 || ft->kind == TYPE_USIZE) {
        int lbig = new_label(), ld = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_S, lbig);
        (f ? cvtsi2ssq(R(RAX), X(0)) : cvtsi2sdq(R(RAX), X(0)));
        jmp(ld);
        label(lbig);
        movq(R(RAX), R(RCX));
        shrq(IMM(1), R(RCX));
        andl(IMM(1), R(RAX));
        orq(R(RAX), R(RCX));
        (f ? cvtsi2ssq(R(RCX), X(0)) : cvtsi2sdq(R(RCX), X(0)));
        (f ? addss(X(0), X(0)) : addsd(X(0), X(0)));
        label(ld);
        return;
    }
    (f ? cvtsi2ssq(R(RAX), X(0)) : cvtsi2sdq(R(RAX), X(0)));
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
            mov_imm_reg(tmax, R(RDX));
            cmpq(R(RDX), R(RAX));
            overflow_fail_jump(loc, "cast", CC_A);
        }
    } else if (to_u64) {
        testq(R(RAX), R(RAX));
        overflow_fail_jump(loc, "cast", CC_S);
    } else {
        mov_imm_reg(tmin, R(RDX));
        cmpq(R(RDX), R(RAX));
        overflow_fail_jump(loc, "cast", CC_L);
        mov_imm_reg(tmax, R(RDX));
        cmpq(R(RDX), R(RAX));
        overflow_fail_jump(loc, "cast", CC_G);
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
            movq(M(RAX, 0), R(RSI));
            movq(M(RAX, 8), R(RDX));
            if (g_overflow_checked && facts_overflow_governs(e, subst_resolve)) {
                char *rest = str_sprintf("string truncation in (cstr[%lld]) cast: len=%%lld max=%%lld\n", (long long)n);
                int m = msg_label(e->loc, rest);
                free(rest);
                int l = fail_begin();
                movq(IMM((n - 1)), R(RCX));
                fail_end(m);
                cmpq(IMM((n - 1)), R(RDX));
                jcc(CC_G, l);
            }
            int lle = new_label();
            cmpq(IMM((n - 1)), R(RDX));
            jcc(CC_LE, lle);
            movq(IMM((n - 1)), R(RDX));
            label(lle);
            int32_t cn = spill_push();
            movq(R(RDX), fp(cn));
            leaq(fp(back), R(RDI));
            call_c("memcpy");
            movq(fp(cn), R(RCX));
            spill_pop();
            leaq(fp(back), R(RAX));
            movb(IMM(0), MI(RAX, RCX, 1, 0));
        } else {
            /* (cstr) under alloca: a dynamic stack copy. */
            int32_t sp = spill_push();
            movq(R(RAX), fp(sp));
            movq(M(RAX, 8), R(RAX));
            leaq(M(RAX, 16), R(RAX));
            andq(IMM(-16), R(RAX));
            subq(R(RAX), R(RSP));
            movq(fp(sp), R(RAX));
            movq(R(RSP), R(RDI));
            movq(M(RAX, 0), R(RSI));
            movq(M(RAX, 8), R(RDX));
            call_c("memcpy");
            movq(fp(sp), R(RCX));
            movq(M(RCX, 8), R(RCX));
            movb(IMM(0), MI(RAX, RCX, 1, 0));
            spill_pop();
        }
        return;
    }
    if (is_cstr_type(from) && is_str_type(to)) {
        gen_expr(op);
        int32_t t = slot_alloc(16, 8);
        movq(R(RAX), fp(t));
        movq(R(RAX), R(RDI));
        call_c("strlen");
        movq(R(RAX), fp(t + 8));
        leaq(fp(t), R(RAX));
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
        if (is_f32(from) && !is_f32(to)) cvtss2sd(X(0), X(0));
        else if (!is_f32(from) && is_f32(to)) cvtsd2ss(X(0), X(0));
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
    if (fixed || e->struct_lit.field_count < L->n) zero_mem(RBP, t, L->size);
    for (int i = 0; i < e->struct_lit.field_count; i++) {
        int64_t off; Type *ft; ExtField *x;
        if (!field_info_x(st, e->struct_lit.fields[i].name, &off, &ft, &x)) {
            internal_error(e->loc, "no field '%s'", e->struct_lit.fields[i].name);
            continue;
        }
        if (x) {
            gen_expr(e->struct_lit.fields[i].value);
            store_ext_field(x, RBP, t, t + off);
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
            movq(M(RAX, 8), R(RDX));
            cmpq(IMM(cap), R(RDX));
            jcc(CC_G, l);
            movq(M(RAX, 0), R(RSI));
            leaq(fp(t + (int32_t)off), R(RDI));
            if (es != 1) imulq3(es, R(RDX), R(RCX));
            else movq(R(RDX), R(RCX));
            rep_movsb();
            continue;
        }
        gen_expr(v);
        store_val(ft, RBP, t + off);
    }
    leaq(fp(t), R(RAX));
}

static void gen_tuple_lit(Expr *e) {
    Type *tt = conc(e->type);
    Layout *L = agg_layout(tt);
    int32_t t = temp_alloc(tt);
    for (int i = 0; i < e->tuple_lit.elem_count && i < L->n; i++) {
        gen_expr(e->tuple_lit.elems[i]);
        store_val(L->ftype[i], RBP, t + L->off[i]);
    }
    leaq(fp(t), R(RAX));
}

/* Allocate `size_reg`-many bytes (in %rax) on the stack; the address lands
 * in %rax. */
static void dyn_alloca_rax(void) {
    addq(IMM(15), R(RAX));
    andq(IMM(-16), R(RAX));
    subq(R(RAX), R(RSP));
    movq(R(RSP), R(RAX));
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
        movq(R(RAX), fp(nslot));
    }
    bool on_frame = const_n && !dynamic && N > 0;
    int32_t back = 0;
    if (on_frame) {
        back = slot_alloc(align_up(N * es, 8), ty_align(elem) > 8 ? ty_align(elem) : 8);
        leaq(fp(back), R(RAX));
    } else if (const_n) {
        mov_imm(N * es);
        dyn_alloca_rax();
    } else {
        movq(fp(nslot), R(RAX));
        scale_reg(es, RAX);
        dyn_alloca_rax();
    }
    movq(R(RAX), fp(hdr));
    if (const_n) movq(IMM(N), fp(hdr + 8));
    else { movq(fp(nslot), R(RCX)); movq(R(RCX), fp(hdr + 8)); }
    if (e->array_lit.elem_count == 0) {
        if (on_frame) zero_mem(RBP, back, N * es);
        else {
            movq(fp(hdr), R(RDI));
            movq(fp(hdr + 8), R(RCX));
            scale_reg(es, RCX);
            xorl(R(RAX), R(RAX));
            rep_stosb();
        }
    } else {
        for (int i = 0; i < e->array_lit.elem_count; i++) {
            gen_expr(e->array_lit.elems[i]);
            if (on_frame) store_val(elem, RBP, back + i * es);
            else {
                movq(fp(hdr), R(RDX));
                store_val(elem, RDX, i * es);
            }
        }
    }
    leaq(fp(hdr), R(RAX));
}

static void gen_slice_lit(Expr *e) {
    int32_t t = slot_alloc(16, 8);
    gen_expr(e->slice_lit.ptr_expr);
    movq(R(RAX), fp(t));
    gen_expr(e->slice_lit.len_expr);
    if (!e->slice_lit.len_nonneg) {
        int m = msg_label(e->loc, "slice literal length is negative: len=%lld\n");
        int l = fail_begin();
        movq(R(RAX), R(RDX));
        fail_end(m);
        testq(R(RAX), R(RAX));
        jcc(CC_S, l);
    }
    movq(R(RAX), fp(t + 8));
    leaq(fp(t), R(RAX));
}

static void gen_some(Expr *e) {
    Type *ot = conc(e->type);
    if (is_null_sentinel(ot)) {
        gen_expr(e->some_expr.value);
        if (!ptr_value_provably_nonnull(e->some_expr.value)) {
            int l = fail_simple(e->loc, "some() of a null pointer (pointer options use null as the none sentinel)\n");
            testq(R(RAX), R(RAX));
            jcc(CC_E, l);
        }
        return;
    }
    int32_t t = temp_alloc(ot);
    Type *in = conc(ot->option.inner);
    int64_t is = ty_size(in);
    zero_mem(RBP, t, ty_size(ot));
    gen_expr(e->some_expr.value);
    store_val(in, RBP, t);
    movb(IMM(1), fp(t + (int32_t)is));
    leaq(fp(t), R(RAX));
}

static void gen_ok(Expr *e) {
    Type *rt = conc(e->type);
    int32_t t = temp_alloc(rt);
    if (e->ok_expr.value) {
        gen_expr(e->ok_expr.value);
        store_val(rt->result.inner, RBP, t + result_value_off(rt));
    }
    movl(IMM(0), fp(t));
    leaq(fp(t), R(RAX));
}

static void gen_err(Expr *e) {
    Type *rt = conc(e->type);
    int32_t t = temp_alloc(rt);
    gen_expr(e->err_expr.code);
    if (!int_value_provably_nonzero(e->err_expr.code)) {
        int l = fail_simple(e->loc, "err() with code 0 (0 is the ok tag; error codes must be non-zero)\n");
        testl(R(RAX), R(RAX));
        jcc(CC_E, l);
    }
    int32_t cs = spill_push();
    movq(R(RAX), fp(cs));
    zero_mem(RBP, t, ty_size(rt));
    movq(fp(cs), R(RAX));
    spill_pop();
    movl(R(RAX), fp(t));
    leaq(fp(t), R(RAX));
}

static void gen_default(Type *t) {
    switch (val_kind(t)) {
    case VK_INT: xorl(R(RAX), R(RAX)); return;
    case VK_FLOAT: xorps(X(0), X(0)); return;
    case VK_VOID: return;
    case VK_AGG: {
        int32_t s = temp_alloc(t);
        zero_mem(RBP, s, ty_size(t));
        leaq(fp(s), R(RAX));
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
static void fill_captures(Expr *lam, Reg base, int64_t base_off) {
    int n = lam->func.capture_count;
    int64_t *offs = xcalloc((size_t)(n ? n : 1), sizeof(int64_t));
    ctx_layout(lam, offs, NULL);
    for (int i = 0; i < n; i++) {
        Capture *c = &lam->func.captures[i];
        Opnd home;
        if (!local_find(c->codegen_name, &home)) {
            internal_error(lam->loc, "capture '%s' not in scope", c->name);
            continue;
        }
        load_val(c->type, home);
        if (base != RBP) {
            /* the base pointer lives in a spill slot passed as base_off */
            movq(fp((int32_t)base_off), R(RDX));
            store_val(c->type, RDX, offs[i]);
        } else {
            store_val(c->type, RBP, base_off + offs[i]);
        }
    }
    free(offs);
}

static void gen_func_value(Expr *e) {
    const char *sym = lambda_c_name(e);
    if (e->func.capture_count == 0) {
        leaq(RIPREL(fnval_label(sym), 0), R(RAX));
        return;
    }
    int64_t al;
    int64_t size = ctx_layout(e, NULL, &al);
    int32_t ctx = slot_alloc(align_up(size, 8), al > 8 ? al : 8);
    fill_captures(e, RBP, ctx);
    int32_t fv = slot_alloc(16, 8);
    leaq(RIPREL(asm_sym(sym), 0), R(RAX));
    movq(R(RAX), fp(fv));
    leaq(fp(ctx), R(RAX));
    movq(R(RAX), fp(fv + 8));
    leaq(fp(fv), R(RAX));
}

/* ================================================================
 * Allocation
 * ================================================================ */

/* %rdi = max(%rax, 1): an element count for a heap request that may be 0. */
static void alloc_n_rdi(void) {
    movq(R(RAX), R(RDI));
    testq(R(RDI), R(RDI));
    movl(IMM(1), R(RCX));
    cmovq(CC_LE, R(RCX), R(RDI));
}

/* Build an option over a slice from %rax (pointer, NULL on failure) and the
 * length in `len_slot`; leaves the option's address in %rax. */
static void some_slice_or_none(Type *opt, int32_t len_slot) {
    int32_t t = temp_alloc(opt);
    zero_mem(RBP, t, ty_size(opt));     /* small: does not touch %rax */
    int lnone = new_label();
    testq(R(RAX), R(RAX));
    jcc(CC_E, lnone);
    movq(R(RAX), fp(t));
    movq(fp(len_slot), R(RCX));
    movq(R(RCX), fp(t + 8));
    movb(IMM(1), fp(t + 16));
    label(lnone);
    leaq(fp(t), R(RAX));
}

static void gen_alloc(Expr *e) {
    Expr *init = e->alloc_expr.init_expr;
    Type *at = e->alloc_expr.alloc_type ? conc(e->alloc_expr.alloc_type) : NULL;
    if (e->alloc_expr.is_stack) {
        if (at && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
            gen_expr(e->alloc_expr.size_expr);
            int64_t es = ty_size(at);
            scale_reg(es, RAX);
            dyn_alloca_rax();
        } else if (at && e->alloc_expr.size_expr) {
            int64_t es = ty_size(at);
            gen_expr(e->alloc_expr.size_expr);
            int32_t t = slot_alloc(16, 8);
            movq(R(RAX), fp(t + 8));
            scale_reg(es, RAX);
            int32_t bytes = slot_alloc(8, 8);
            movq(R(RAX), fp(bytes));
            dyn_alloca_rax();
            movq(R(RAX), fp(t));
            movq(R(RAX), R(RDI));
            movq(fp(bytes), R(RCX));
            xorl(R(RAX), R(RAX));
            rep_stosb();
            leaq(fp(t), R(RAX));
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
        movq(IMM(ty_size(at)), R(RSI));
        call_c("calloc");
        return;
    }
    if (at && e->alloc_expr.size_expr) {
        gen_expr(e->alloc_expr.size_expr);
        int32_t ls = slot_alloc(8, 8);
        movq(R(RAX), fp(ls));
        alloc_n_rdi();
        movq(IMM(ty_size(at)), R(RSI));
        call_c("calloc");
        some_slice_or_none(conc(e->type), ls);
        return;
    }
    if (at) {
        movl(IMM(1), R(RDI));
        movq(IMM(ty_size(at)), R(RSI));
        call_c("calloc");
        return;
    }
    if (init->kind == EXPR_STRING_LIT) {
        int n;
        unsigned char *b = decode_lit(init->string_lit.value, init->string_lit.length, &n);
        int sl = str_label(b, n);
        free(b);
        int32_t ls = slot_alloc(8, 8);
        movq(IMM(n), fp(ls));
        movl(IMM(n > 0 ? n : 1), R(RDI));
        call_c("malloc");
        int lnull = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        movq(R(RAX), R(RDI));
        leaq(RIPREL(sl, 0), R(RSI));
        movl(IMM(n), R(RDX));
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
        movl(IMM(n + 1), R(RDI));
        call_c("malloc");
        int lnull = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        movq(R(RAX), R(RDI));
        leaq(RIPREL(sl, 0), R(RSI));
        movl(IMM(n + 1), R(RDX));
        call_c("memcpy");
        label(lnull);
        return;
    }
    if (init->kind == EXPR_CAST && is_cstr_type(conc(init->type))) {
        gen_expr(init->cast.operand);
        int32_t s = slot_alloc(16, 8);
        copy_mem(RBP, s, RAX, 0, 16);
        movq(fp(s + 8), R(RDI));
        incq(R(RDI));
        call_c("malloc");
        int lnull = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        int32_t p = slot_alloc(8, 8);
        movq(R(RAX), fp(p));
        movq(R(RAX), R(RDI));
        movq(fp(s), R(RSI));
        movq(fp(s + 8), R(RDX));
        call_c("memcpy");
        movq(fp(s + 8), R(RCX));
        movb(IMM(0), MI(RAX, RCX, 1, 0));
        movq(fp(p), R(RAX));
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
        movq(IMM(N), fp(ls));
        if (ec == 0) {
            movq(IMM((N > 0 ? N : 1)), R(RDI));
            movq(IMM(es), R(RSI));
            call_c("calloc");
        } else {
            movq(IMM((N * es > 0 ? N * es : 1)), R(RDI));
            call_c("malloc");
        }
        int32_t p = slot_alloc(8, 8);
        movq(R(RAX), fp(p));
        int lnull = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        for (int i = 0; i < ec; i++) {
            gen_expr(init->array_lit.elems[i]);
            movq(fp(p), R(RDX));
            store_val(elem, RDX, i * es);
        }
        label(lnull);
        movq(fp(p), R(RAX));
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
            copy_mem(RBP, src, RAX, 0, 16);
        }
        movq(IMM((size > 0 ? size : 1)), R(RDI));
        call_c("malloc");
        int32_t p = slot_alloc(8, 8);
        movq(R(RAX), fp(p));
        int32_t t = temp_alloc(opt);
        zero_mem(RBP, t, ty_size(opt));
        int lnull = new_label();
        movq(fp(p), R(RAX));
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        if (literal) {
            fill_captures(lam, RDX, p);
            leaq(RIPREL(asm_sym(lambda_c_name(lam)), 0), R(RAX));
        } else {
            movq(fp(p), R(RDI));
            movq(fp(src + 8), R(RSI));
            movq(IMM(size), R(RDX));
            call_c("memcpy");
            movq(fp(src), R(RAX));
        }
        movq(R(RAX), fp(t));
        movq(fp(p), R(RAX));
        movq(R(RAX), fp(t + 8));
        movb(IMM(1), fp(t + 16));
        label(lnull);
        leaq(fp(t), R(RAX));
        return;
    }
    Type *it = conc(init->type);
    if (init->kind == EXPR_STRUCT_LIT || it->kind == TYPE_UNION) {
        int64_t sz = ty_size(it);
        movq(IMM((sz > 0 ? sz : 1)), R(RDI));
        call_c("malloc");
        int32_t p = slot_alloc(8, 8);
        movq(R(RAX), fp(p));
        int lnull = new_label();
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        gen_expr(init);
        movq(fp(p), R(RDX));
        store_val(it, RDX, 0);
        label(lnull);
        movq(fp(p), R(RAX));
        return;
    }
    if (it->kind == TYPE_SLICE) {
        int64_t es = ty_size(it->slice.elem);
        gen_expr(init);
        int32_t s = slot_alloc(16, 8);
        copy_mem(RBP, s, RAX, 0, 16);
        movq(fp(s + 8), R(RAX));
        alloc_n_rdi();
        scale_reg(es, RDI);
        call_c("malloc");
        int lnull = new_label();
        int32_t p = slot_alloc(8, 8);
        movq(R(RAX), fp(p));
        testq(R(RAX), R(RAX));
        jcc(CC_E, lnull);
        movq(R(RAX), R(RDI));
        movq(fp(s), R(RSI));
        movq(fp(s + 8), R(RDX));
        scale_reg(es, RDX);
        call_c("memcpy");
        label(lnull);
        movq(fp(p), R(RAX));
        some_slice_or_none(conc(e->type), s + 8);
        return;
    }
    internal_error(e->loc, "alloc form not supported");
}

static void gen_free(Expr *e) {
    Type *ot = conc(e->free_expr.operand->type);
    gen_expr(e->free_expr.operand);
    if (ot->kind == TYPE_SLICE) movq(M(RAX, 0), R(RDI));
    else if (ot->kind == TYPE_FUNC) movq(M(RAX, 8), R(RDI));
    else movq(R(RAX), R(RDI));
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
            copy_mem(RBP, sslot[i], RAX, 0, ty_size(t));
        } else {
            sslot[i] = slot_alloc(8, 8);
            (val_kind(t) == VK_FLOAT ? movq(X(0), fp(sslot[i])) : movq(R(RAX), fp(sslot[i])));
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
                movq(IMM(prec), R(RCX));
                fail_end(m);
                movq(fp(sslot[i] + 8), R(RDX));
                cmpq(IMM(prec), R(RDX));
                jcc(CC_G, l);
            } else {
                int lok = new_label();
                movq(fp(sslot[i]), R(RDI));
                xorl(R(RSI), R(RSI));
                movl(IMM(prec + 1), R(RDX));
                call_c("memchr");
                testq(R(RAX), R(RAX));
                jcc(CC_NE, lok);
                movq(fp(sslot[i]), R(RDI));
                call_c("strlen");
                int l = fail_begin();
                movq(R(RAX), R(RDX));
                movq(IMM(prec), R(RCX));
                fail_end(m);
                jmp(l);
                label(lok);
            }
        }
    }

    /* The byte budget. */
    int32_t flen = slot_alloc(8, 8);
    int64_t konst = 0;
    movq(IMM(0), fp(flen));
    for (int i = 0; i < nseg; i++) {
        InterpSegment *seg = &segs[i];
        Type *t = seg->is_literal ? NULL : instance_type(seg->expr->type);
        InterpSegBudget b = interp_seg_budget(seg, t);
        if (!b.runtime) { konst += b.bytes; continue; }
        if (is_str_type(stype[i])) movq(fp(sslot[i] + 8), R(RAX));
        else { movq(fp(sslot[i]), R(RDI)); call_c("strlen"); }
        if (b.bytes > 0) {
            movq(IMM(b.bytes), R(RCX));
            cmpq(R(RCX), R(RAX));
            cmovq(CC_L, R(RCX), R(RAX));
        }
        addq(R(RAX), fp(flen));
    }
    if (konst) addq(IMM(konst), fp(flen));

    /* The buffer. */
    int32_t buf = slot_alloc(8, 8);
    int64_t bsize = 0;
    bool fixed = !heap && interp_const_buffer_size(e, instance_type, &bsize) &&
                 !e->interp_string.wrapped;
    if (heap) {
        movq(fp(flen), R(RDI));
        incq(R(RDI));
        call_c("malloc");
    } else if (fixed) {
        int32_t back = slot_alloc(align_up(bsize + 1, 8), 8);
        leaq(fp(back), R(RAX));
    } else {
        movq(fp(flen), R(RAX));
        incq(R(RAX));
        dyn_alloca_rax();
    }
    movq(R(RAX), fp(buf));

    /* The format string and arguments. */
    Buf fmt = { 0 };
    CallArg *args = xcalloc((size_t)(3 + 2 * nseg + 64), sizeof(CallArg));
    int na = 0, acap = 3 + 2 * nseg + 64;
    args[na].type = type_any_ptr(); args[na++].slot = buf;
    int32_t size_slot = slot_alloc(8, 8);
    movq(fp(flen), R(RAX));
    incq(R(RAX));
    movq(R(RAX), fp(size_slot));
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
            movq(fp(sslot[i] + 8), R(RAX));
            if (sp.precision >= 0) {
                movq(IMM(sp.precision), R(RCX));
                cmpq(R(RCX), R(RAX));
                cmovq(CC_G, R(RCX), R(RAX));
            }
            movq(R(RAX), fp(ls));
            args[na].type = type_int32(); args[na++].slot = ls;
            int32_t ps = slot_alloc(8, 8);
            movq(fp(sslot[i]), R(RAX));
            movq(R(RAX), fp(ps));
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
            movq(fp(sslot[i]), X(0));
            if (is_f32(t)) cvtss2sd(X(0), X(0));
            movq(X(0), fp(as));
            args[na].type = type_float64(); args[na++].slot = as;
            continue;
        }
        movq(fp(sslot[i]), R(RAX));
        if (type_is_integer(t) && interp_conv_is_unsigned(conv, t) && int_signed(t)) {
            int w = int_width(t);
            if (w == 1) movzbl(R(RAX), R(RAX));
            else if (w == 2) movzwl(R(RAX), R(RAX));
            else if (w == 4) movl(R(RAX), R(RAX));
        }
        movq(R(RAX), fp(as));
        args[na].type = type_int64(); args[na++].slot = as;
    }
    int fl = str_label((const unsigned char *)(fmt.p ? fmt.p : ""), (int)fmt.len);
    buf_free(&fmt);
    leaq(RIPREL(fl, 0), R(RAX));
    movq(R(RAX), fp(fmt_slot));

    int32_t fw = slot_alloc(8, 8);
    int lskip = new_label();
    if (heap) {
        movq(IMM(0), fp(fw));
        cmpq(IMM(0), fp(buf));
        jcc(CC_E, lskip);
    }
    CallSpec cs = { 0 };
    cs.sym = "snprintf";
    cs.plt = true;
    cs.variadic = true;
    cs.ret = type_int32();
    emit_call(&cs, args, na);
    movq(R(RAX), fp(fw));
    label(lskip);
    free(args);
    free(sslot);
    free(stype);

    if (is_cstr) { movq(fp(buf), R(RAX)); return; }
    /* len = (fw >= 0 && fw <= flen) ? fw : flen */
    movq(fp(fw), R(RAX));
    movq(fp(flen), R(RCX));
    int lk = new_label(), luse = new_label();
    testq(R(RAX), R(RAX));
    jcc(CC_S, luse);
    cmpq(R(RCX), R(RAX));
    jcc(CC_LE, lk);
    label(luse);
    movq(R(RCX), R(RAX));
    label(lk);
    if (heap) {
        int32_t ls = slot_alloc(8, 8);
        movq(R(RAX), fp(ls));
        movq(fp(buf), R(RAX));
        some_slice_or_none(alloc_opt, ls);
        return;
    }
    int32_t t = slot_alloc(16, 8);
    movq(R(RAX), fp(t + 8));
    movq(fp(buf), R(RAX));
    movq(R(RAX), fp(t));
    leaq(fp(t), R(RAX));
}

/* ================================================================
 * Assignment
 * ================================================================ */

/* Store a leaf value of type t at (%rax): false, with nothing emitted, if
 * value is not one. */
static bool store_leaf(Type *t, Expr *value) {
    int64_t v;
    int w = int_width(t);
    switch (val_kind(t)) {
    case VK_INT:
        if (int_leaf(value, &v) && (w < 8 || fits_imm32(v))) {
            x_mov(g_cur, w, IMM(v), M(RAX, 0));
            return true;
        }
        Opnd m;
        Type *mt;
        if (fixed_place(value, &m, &mt) && m.kind == OPND_REG) {
            x_mov(g_cur, w, m, M(RAX, 0));
            return true;
        }
        if (!leaf_to(value, RCX)) return false;
        x_mov(g_cur, w, R(RCX), M(RAX, 0));
        return true;
    case VK_FLOAT:
        if (!float_leaf_to(value, 0)) return false;
        store_at(t, M(RAX, 0));
        return true;
    default:
        return false;
    }
}

static void gen_assign(Expr *e) {
    Expr *target = e->assign.target;
    if ((target->kind == EXPR_FIELD || target->kind == EXPR_DEREF_FIELD) &&
        target->field.fixed_array_type) {
        Type *fat = target->field.fixed_array_type;
        int64_t cap = fixarr_size(fat), es = ty_size(fat->fixed_array.elem);
        gen_addr(target);
        int32_t dst = slot_alloc(8, 8);
        movq(R(RAX), fp(dst));
        gen_expr(e->assign.value);
        int32_t src = slot_alloc(16, 8);
        copy_mem(RBP, src, RAX, 0, 16);
        char *rest = str_sprintf("fixed-array field '%s' overflow: len=%%lld capacity=%lld\n",
                                 target->field.name, (long long)cap);
        int m = msg_label(e->loc, rest);
        free(rest);
        int l = fail_begin();
        fail_end(m);
        movq(fp(src + 8), R(RDX));
        cmpq(IMM(cap), R(RDX));
        jcc(CC_G, l);
        /* zero the whole field, then copy the source over it */
        movq(fp(dst), R(RDI));
        movq(IMM((cap * es)), R(RCX));
        xorl(R(RAX), R(RAX));
        rep_stosb();
        movq(fp(dst), R(RDI));
        movq(fp(src), R(RSI));
        movq(fp(src + 8), R(RCX));
        scale_reg(es, RCX);
        rep_movsb();
        return;
    }
    if ((target->kind == EXPR_FIELD && !target->field.codegen_name) || target->kind == EXPR_DEREF_FIELD) {
        Type *st = conc(target->field.object->type);
        if (target->kind == EXPR_DEREF_FIELD) st = conc(st->pointer.pointee);
        int64_t off; Type *ft; ExtField *x;
        if (field_info_x(st, target->field.name, &off, &ft, &x) && x) {
            gen_expr(target->field.object);
            int32_t s = spill_push();
            movq(R(RAX), fp(s));
            gen_expr(e->assign.value);
            movq(fp(s), R(RDX));
            spill_pop();
            store_ext_field(x, RDX, 0, off);
            return;
        }
    }
    Opnd m;
    Type *mt;
    /* A place through a register pointer is found before the value is
     * evaluated, so the value must not change the pointer. */
    if (fixed_place(target, &m, &mt) && (val_kind(target->type) != VK_AGG || m.base == RBP) &&
        (m.kind != OPND_MEM || m.base == RBP || m.base == RIP ||
         !(e->assign.value->kind == EXPR_ASSIGN || expr_any_child(e->assign.value, assign_pred, NULL)))) {
        if (m.kind == OPND_REG && leaf_to(e->assign.value, (Reg)m.base)) return;
        if (m.kind == OPND_XMM && float_leaf_to(e->assign.value, m.base)) return;
        gen_expr(e->assign.value);
        store_at(target->type, m);
        return;
    }
    gen_addr(target);
    if (store_leaf(target->type, e->assign.value)) return;
    Opnd h = hold_push(e->assign.value, false);
    gen_expr(e->assign.value);
    Reg base = RDX;
    if (h.kind == OPND_REG) base = (Reg)h.base;
    else movq(h, R(RDX));
    store_val(target->type, base, 0);
    hold_pop(h);
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
    if (v >= INT32_MIN && v <= INT32_MAX) cmpq(IMM(v), R(RAX));
    else { mov_imm_reg(v, R(RCX)); cmpq(R(RCX), R(RAX)); }
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
        jcc(CC_NE, fail);
        return;
    case PAT_CHAR_LIT:
        load_int(t, fp(off));
        cmp_rax_imm(pat->char_lit.value);
        jcc(CC_NE, fail);
        return;
    case PAT_BOOL_LIT:
        load_int(t, fp(off));
        cmp_rax_imm(pat->bool_lit.value ? 1 : 0);
        jcc(CC_NE, fail);
        return;
    case PAT_STRING_LIT: {
        int n;
        unsigned char *b = decode_lit(pat->string_lit.value, pat->string_lit.length, &n);
        int sl = str_label(b, n);
        free(b);
        cmpq(IMM(n), fp(off + 8));
        jcc(CC_NE, fail);
        if (n > 0) {
            movq(fp(off), R(RDI));
            leaq(RIPREL(sl, 0), R(RSI));
            movl(IMM(n), R(RDX));
            call_c("memcmp");
            testl(R(RAX), R(RAX));
            jcc(CC_NE, fail);
        }
        return;
    }
    case PAT_SOME:
        if (is_null_sentinel(t)) {
            cmpq(IMM(0), fp(off));
            jcc(CC_E, fail);
        } else {
            cmpb(IMM(0), fp(off + (int32_t)ty_size(t->option.inner)));
            jcc(CC_E, fail);
        }
        if (pat->some_pat.inner) gen_pat_test(pat->some_pat.inner, off, t->option.inner, fail);
        return;
    case PAT_NONE:
        if (is_null_sentinel(t)) cmpq(IMM(0), fp(off));
        else cmpb(IMM(0), fp(off + (int32_t)ty_size(t->option.inner)));
        jcc(CC_NE, fail);
        return;
    case PAT_OK:
        cmpl(IMM(0), fp(off));
        jcc(CC_NE, fail);
        if (pat->some_pat.inner)
            gen_pat_test(pat->some_pat.inner, off + (int32_t)result_value_off(t), t->result.inner, fail);
        return;
    case PAT_ERR:
        cmpl(IMM(0), fp(off));
        jcc(CC_E, fail);
        if (pat->some_pat.inner) gen_pat_test(pat->some_pat.inner, off, type_int32(), fail);
        return;
    case PAT_VARIANT:
        if (t->kind == TYPE_ENUM) {
            load_int(t, fp(off));
            cmp_rax_imm(enum_variant_value(t, pat->variant.variant));
            jcc(CC_NE, fail);
            return;
        }
        cmpl(IMM(union_variant_index(t, pat->variant.variant)), fp(off));
        jcc(CC_NE, fail);
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
                load_ext_field(ft, x, RBP, off, off + fo);
                int32_t v = slot_alloc(8, 8);
                movq(R(RAX), fp(v));
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
            if (!pattern_has_predicate(pat->or_pat.alts[i])) { jmp(ok); break; }
            if (i == pat->or_pat.alt_count - 1) {
                gen_pat_test(pat->or_pat.alts[i], off, t, fail);
            } else {
                int nf = new_label();
                gen_pat_test(pat->or_pat.alts[i], off, t, nf);
                jmp(ok);
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
        Opnd home = local_home(nm, t);
        if (val_kind(t) == VK_AGG) copy_mem(RBP, home.disp, RBP, off, ty_size(t));
        else { load_val(t, fp(off)); store_at(t, home); }
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
                load_ext_field(ft, x, RBP, off, off + fo);
                int32_t v = slot_alloc(8, 8);
                movq(R(RAX), fp(v));
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
        copy_mem(RBP, s, RAX, 0, ty_size(t));
        return s;
    }
    case VK_FLOAT: { int32_t s = slot_alloc(8, 8); movq(X(0), fp(s)); return s; }
    case VK_INT: { int32_t s = slot_alloc(8, 8); movq(R(RAX), fp(s)); return s; }
    case VK_VOID: return slot_alloc(8, 8);
    }
    return 0;
}

/* ================================================================
 * Control flow
 * ================================================================ */

/* Jump to target when the comparison e is `when`, from the flags it sets,
 * with no bool made of it: false, with nothing emitted, if e is not a
 * comparison of numbers. */
static bool cond_compare(Expr *e, bool when, int target) {
    int op = e->binary.op;
    if (cond_for(op, false) < 0) return false;
    Expr *unw[2];
    binary_widen(e, unw);
    Type *lt = conc(e->binary.left->type);
    bool done = true;
    if (needs_eq_func(lt)) {
        done = false;
    } else if (val_kind(lt) == VK_INT) {
        gen_int_compare(e->binary.left, e->binary.right);
        Cond cc = (Cond)cond_for(op, int_signed(lt));
        jcc(when ? cc : (Cond)(cc ^ 1), target);    /* cc ^ 1 is cc's opposite */
    } else if (val_kind(lt) == VK_FLOAT) {
        SseOp uc = is_f32(lt) ? SSE_UCOMISS : SSE_UCOMISD;
        Opnd src = gen_float_operands(e->binary.left, e->binary.right, op == TOK_LT || op == TOK_LTEQ);
        /* Unordered (a NaN) sets ZF, PF and CF: every comparison is false
         * but !=, and only parity tells unordered from equal. */
        if (op == TOK_EQEQ || op == TOK_BANGEQ) {
            sse(uc, src, X(0));
            if ((op == TOK_EQEQ) == when) {
                int skip = new_label();
                jcc(CC_P, skip);
                jcc(CC_E, target);
                label(skip);
            } else {
                jcc(CC_P, target);
                jcc(CC_NE, target);
            }
        } else {
            bool gt = op == TOK_GT || op == TOK_GTEQ;
            (gt ? sse(uc, src, X(0)) : sse(uc, X(0), src));
            Cond cc = op == TOK_GT || op == TOK_LT ? CC_A : CC_AE;
            jcc(when ? cc : (Cond)(cc ^ 1), target);
        }
    } else {
        done = false;
    }
    binary_unwiden(e, unw);
    return done;
}

/* Jump to target when cond is `when`, else fall through. A comparison, and
 * &&, || and ! over them, branch on the flags directly. */
static void cond_jump(Expr *cond, bool when, int target) {
    int64_t v;
    if (int_leaf(cond, &v)) {
        if ((v != 0) == when) jmp(target);
        return;
    }
    if (cond->kind == EXPR_UNARY_PREFIX && cond->unary_prefix.op == TOK_BANG) {
        cond_jump(cond->unary_prefix.operand, !when, target);
        return;
    }
    if (cond->kind == EXPR_BINARY) {
        int op = cond->binary.op;
        if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
            /* a && b is true when both are, a || b false when both are */
            if (when == (op == TOK_PIPEPIPE)) {
                cond_jump(cond->binary.left, when, target);
                cond_jump(cond->binary.right, when, target);
            } else {
                int skip = new_label();
                cond_jump(cond->binary.left, !when, skip);
                cond_jump(cond->binary.right, when, target);
                label(skip);
            }
            return;
        }
        if (cond_compare(cond, when, target)) return;
    }
    gen_expr(cond);
    testl(R(RAX), R(RAX));
    jcc(when ? CC_NE : CC_E, target);
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
    /* `if c then break` (or continue) with no defers to run on the way out:
     * one branch to the loop's label. */
    Expr *then = e->if_expr.then_body;
    if (then->kind == EXPR_BLOCK && then->block.count == 1) then = then->block.stmts[0];
    if (!e->if_expr.else_body && g_loop && !defers_to_loop_pending() &&
        ((then->kind == EXPR_BREAK && !then->break_expr.value) || then->kind == EXPR_CONTINUE)) {
        cond_jump(e->if_expr.cond, true, then->kind == EXPR_BREAK ? g_loop->brk : g_loop->cont);
        return;
    }
    bool valued = !type_valueless(e->type);
    int lelse = new_label(), lend = new_label();
    if (!e->if_expr.else_body && !valued) {
        cond_jump(e->if_expr.cond, false, lend);
        gen_scoped_branch(e->if_expr.then_body, TAIL_DISCARD);
        label(lend);
        return;
    }
    cond_jump(e->if_expr.cond, false, lelse);
    gen_scoped_branch(e->if_expr.then_body,
                      valued && !type_is_never(e->if_expr.then_body->type) ? TAIL_VALUE : TAIL_DISCARD);
    jmp(lend);
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
        if (arm->guard) cond_jump(arm->guard, false, next);
        defer_push(false);
        int mode = tail ? TAIL_RETURN : (valued ? TAIL_VALUE : TAIL_DISCARD);
        bool done = gen_stmts(arm->body, arm->body_count, mode);
        if (!done) {
            emit_scope_defers(g_defer);
            if (tail) jmp(g_ret_label);
        }
        defer_pop();
        jmp(lend);
        label(next);
        if (!arm->guard && !pattern_has_predicate(arm->pattern)) { exhausted = true; break; }
        if (!any_guard && last) exhausted = true;
    }
    if (!exhausted) call_abort();
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
    jmp(lc.cont);
    label(lc.brk);
    g_loop = lc.parent;
    if (valued) {
        if (val_kind(lc.res_type) == VK_AGG) leaq(fp(lc.res), R(RAX));
        else if (val_kind(lc.res_type) == VK_FLOAT) movq(fp(lc.res), X(0));
        else movq(fp(lc.res), R(RAX));
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
                copy_mem(RBP, g_loop->res, RAX, 0, ty_size(g_loop->res_type));
            else if (val_kind(g_loop->res_type) == VK_FLOAT) movq(X(0), fp(g_loop->res));
            else movq(R(RAX), fp(g_loop->res));
        }
    } else {
        emit_defers_to_loop();
    }
    jmp(g_loop->brk);
}

static void gen_continue(void) {
    if (!g_loop) return;
    emit_defers_to_loop();
    jmp(g_loop->cont);
}

static void gen_for(Expr *e) {
    LoopCtx lc = { new_label(), new_label(), 0, NULL, g_loop };
    int ltop = new_label();
    if (e->for_expr.range_end) {
        /* The start, into the loop variable (which the end cannot name),
         * then the end, each once. */
        Type *vt = conc(e->for_expr.iter->type);
        const char *vn = e->for_expr.var_codegen_name ? e->for_expr.var_codegen_name : e->for_expr.var;
        Opnd iv = local_home(vn, vt);
        if (iv.kind != OPND_REG || !leaf_to(e->for_expr.iter, (Reg)iv.base)) {
            gen_expr(e->for_expr.iter);
            store_at(vt, iv);
        }
        int64_t n;
        Opnd end;
        if (int_leaf(e->for_expr.range_end, &n) && fits_imm32(n)) {
            end = IMM(n);
        } else {
            gen_expr(e->for_expr.range_end);
            end = fp(slot_alloc(8, 8));
            movq(R(RAX), end);
        }
        label(ltop);
        Opnd cur = iv;
        if (iv.kind != OPND_REG) { load_int(vt, iv); cur = R(RAX); }
        cmpq(end, cur);
        (int_signed(vt) ? jcc(CC_GE, lc.brk) : jcc(CC_AE, lc.brk));
        g_loop = &lc;
        defer_push(true);
        bool done = gen_stmts(e->for_expr.body, e->for_expr.body_count, TAIL_DISCARD);
        if (!done) emit_scope_defers(g_defer);
        defer_pop();
        g_loop = lc.parent;
        label(lc.cont);
        if (iv.kind == OPND_REG) {
            addq(IMM(1), iv);       /* below the end before, so within the type after */
        } else {
            load_int(vt, iv);
            addq(IMM(1), R(RAX));
            normalize(vt);
            store_at(vt, iv);
        }
        jmp(ltop);
        label(lc.brk);
        return;
    }
    Type *it = conc(e->for_expr.iter->type);
    Type *elem = conc(it->slice.elem);
    int64_t es = ty_size(elem);
    gen_expr(e->for_expr.iter);
    int32_t sl = slot_alloc(16, 8);
    copy_mem(RBP, sl, RAX, 0, 16);
    int32_t cnt = slot_alloc(8, 8);
    movq(IMM(0), fp(cnt));
    const char *en = e->for_expr.var_pattern ? e->for_expr.elem_tmp
        : (e->for_expr.var_codegen_name ? e->for_expr.var_codegen_name : e->for_expr.var);
    Opnd ev = local_home(en, elem);
    label(ltop);
    movq(fp(cnt), R(RAX));
    cmpq(fp(sl + 8), R(RAX));
    jcc(CC_GE, lc.brk);
    scale_reg(es, RAX);
    addq(fp(sl), R(RAX));
    load_val(elem, M(RAX, 0));
    store_at(elem, ev);
    if (e->for_expr.var_pattern) gen_pat_bind(e->for_expr.var_pattern, (int32_t)ev.disp, elem);
    if (e->for_expr.index_var) {
        const char *in = e->for_expr.index_codegen_name ? e->for_expr.index_codegen_name : e->for_expr.index_var;
        Opnd ix = local_home(in, type_int64());
        movq(fp(cnt), R(RAX));
        movq(R(RAX), ix);
    }
    g_loop = &lc;
    defer_push(true);
    bool done = gen_stmts(e->for_expr.body, e->for_expr.body_count, TAIL_DISCARD);
    if (!done) emit_scope_defers(g_defer);
    defer_pop();
    g_loop = lc.parent;
    label(lc.cont);
    incq(fp(cnt));
    jmp(ltop);
    label(lc.brk);
}

/* ================================================================
 * Statements
 * ================================================================ */

static void gen_let(Expr *s) {
    Type *t = s->let_expr.let_type ? s->let_expr.let_type : s->let_expr.let_init->type;
    const char *name = s->let_expr.codegen_name ? s->let_expr.codegen_name : s->let_expr.let_name;
    if (type_valueless(conc(t))) { gen_expr(s->let_expr.let_init); return; }
    Opnd home = local_home(name, conc(t));
    int64_t v;
    if (home.kind == OPND_REG && leaf_to(s->let_expr.let_init, (Reg)home.base)) return;
    if (home.kind == OPND_XMM && float_leaf_to(s->let_expr.let_init, home.base)) return;
    if (val_kind(t) == VK_INT && int_leaf(s->let_expr.let_init, &v) && (int_width(t) < 8 || fits_imm32(v))) {
        x_mov(g_cur, int_width(t), IMM(v), home);
        return;
    }
    gen_expr(s->let_expr.let_init);
    store_at(t, home);
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
                cond_jump(v->if_expr.cond, false, lelse);
                gen_scoped_branch(v->if_expr.then_body, TAIL_RETURN);
                jmp(g_ret_label);
                label(lelse);
                if (v->if_expr.else_body) gen_scoped_branch(v->if_expr.else_body, TAIL_RETURN);
                jmp(g_ret_label);
                return;
            }
            break;
        case EXPR_MATCH:
            gen_match(v, true);
            jmp(g_ret_label);
            return;
        case EXPR_BLOCK:
            gen_scoped_branch(v, TAIL_RETURN);
            jmp(g_ret_label);
            return;
        case EXPR_CALL:
            if (tail_compatible(v)) {
                if (!gen_call(v, true)) {
                    if (!type_valueless(v->type)) move_to_return_regs();
                    jmp(g_ret_label);
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
    jmp(g_ret_label);
}

/* ================================================================
 * The expression dispatcher
 * ================================================================ */

static void gen_assert(Expr *e) {
    int lok = new_label();
    cond_jump(e->assert_expr.condition, true, lok);
    char *text = fmt_escape(e->assert_expr.expr_text ? e->assert_expr.expr_text : "",
                            e->assert_expr.expr_text ? e->assert_expr.expr_text_len : 0);
    if (e->assert_expr.message) {
        gen_expr(e->assert_expr.message);
        char *rest = str_sprintf("assertion failed: %s: %%.*s\n", text);
        int m = msg_label(e->loc, rest);
        free(rest);
        movq(M(RAX, 8), R(RDX));
        movq(M(RAX, 0), R(RCX));
        movq(GOTREL(asm_sym("stderr")), R(RDI));
        movq(M(RDI, 0), R(RDI));
        leaq(RIPREL(m, 0), R(RSI));
        xorl(R(RAX), R(RAX));
        call_c("fprintf");
        call_abort();
    } else {
        char *rest = str_sprintf("assertion failed: %s\n", text);
        int m = msg_label(e->loc, rest);
        free(rest);
        movq(GOTREL(asm_sym("stderr")), R(RDI));
        movq(M(RDI, 0), R(RDI));
        leaq(RIPREL(m, 0), R(RSI));
        xorl(R(RAX), R(RAX));
        call_c("fprintf");
        call_abort();
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
    movq(R(RAX), fp(vs));
    zero_mem(RBP, t, ty_size(opt));
    movq(fp(vs), R(RAX));
    int lnone = new_label(), lfound = new_label();
    if (!u64_repr && op_uwide) { testq(R(RAX), R(RAX)); jcc(CC_S, lnone); }
    if (u64_repr && !op_uwide) { testq(R(RAX), R(RAX)); jcc(CC_S, lnone); }
    for (int i = 0; i < et->enu.variant_count; i++) {
        uint64_t bits = et->enu.variants[i].value_bits;
        int64_t v = u64_repr ? (int64_t)bits : canon_const(bits, repr);
        cmp_rax_imm(v);
        jcc(CC_E, lfound);
    }
    jmp(lnone);
    label(lfound);
    store_int(et, fp(t));
    movb(IMM(1), fp(t + (int32_t)ty_size(et)));
    label(lnone);
    leaq(fp(t), R(RAX));
}

static void gen_error_name(Expr *e) {
    Type *opt = conc(e->type);
    int n = error_code_count();
    int names = errnames_label();
    gen_expr(e->error_name_expr.code);
    int32_t t = temp_alloc(opt);
    int32_t cs = slot_alloc(8, 8);
    movq(R(RAX), fp(cs));
    zero_mem(RBP, t, ty_size(opt));
    int lnone = new_label();
    movq(fp(cs), R(RAX));
    subq(IMM(FC_ERROR_CODE_BASE), R(RAX));
    cmpq(IMM(n), R(RAX));
    jcc(CC_AE, lnone);
    shlq(IMM(4), R(RAX));
    leaq(RIPREL(names, 0), R(RCX));
    addq(R(RCX), R(RAX));
    movq(M(RAX, 0), R(RCX));
    movq(R(RCX), fp(t));
    movq(M(RAX, 8), R(RCX));
    movq(R(RCX), fp(t + 8));
    movb(IMM(1), fp(t + 16));
    label(lnone);
    leaq(fp(t), R(RAX));
}

static void gen_bitcast(Expr *e) {
    Type *from = conc(e->bitcast_expr.operand->type), *to = conc(e->bitcast_expr.target);
    gen_expr(e->bitcast_expr.operand);
    ValKind fk = val_kind(from), tk = val_kind(to);
    if (fk == VK_FLOAT && tk == VK_INT) {
        (is_f32(from) ? movd(X(0), R(RAX)) : movq(X(0), R(RAX)));
        normalize(to);
    } else if (fk == VK_INT && tk == VK_FLOAT) {
        (is_f32(to) ? movd(R(RAX), X(0)) : movq(R(RAX), X(0)));
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
        leaq(RIPREL(strhdr_label(b, n), 0), R(RAX));
        free(b);
        return;
    }
    case EXPR_CSTRING_LIT: {
        int n;
        unsigned char *b = decode_lit(e->cstring_lit.value, e->cstring_lit.length, &n);
        leaq(RIPREL(str_label(b, n), 0), R(RAX));
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
        Type *ot = conc(e->index.object->type), *mt;
        Opnd m;
        if (fixed_place(e, &m, &mt)) { load_val(mt, m); return; }
        if (ot->kind == TYPE_SLICE) { load_val(e->type, gen_slice_elem(e, true)); return; }
        if (ot->kind == TYPE_POINTER) { load_val(e->type, gen_ptr_elem(e, true)); return; }
        gen_addr(e);
        if (!(ot->kind == TYPE_STRUCT && ot->struc.is_tuple) || val_kind(e->type) != VK_AGG)
            load_val(e->type, M(RAX, 0));
        return;
    }
    case EXPR_SLICE: {
        Type *ot = conc(e->slice.object->type);
        int64_t es = ty_size(ot->slice.elem);
        gen_expr(e->slice.object);
        int32_t sh = slot_alloc(16, 8);
        copy_mem(RBP, sh, RAX, 0, 16);
        int32_t lo = slot_alloc(8, 8);
        if (e->slice.lo) gen_expr(e->slice.lo); else xorl(R(RAX), R(RAX));
        movq(R(RAX), fp(lo));
        if (e->slice.hi) gen_expr(e->slice.hi); else movq(fp(sh + 8), R(RAX));
        movq(fp(lo), R(RCX));
        if (!(g_guards_suppressed && facts_guard_governs(e, subst_resolve))) {
            int m = msg_label(e->loc, "subslice out of range: lo=%lld hi=%lld len=%lld\n");
            int l = fail_begin();
            movq(R(RCX), R(RDX));
            movq(R(RAX), R(RCX));
            movq(fp(sh + 8), R(R8));
            fail_end(m);
            cmpq(R(RAX), R(RCX));
            jcc(CC_A, l);
            cmpq(fp(sh + 8), R(RAX));
            jcc(CC_A, l);
        }
        int32_t t = slot_alloc(16, 8);
        subq(R(RCX), R(RAX));
        movq(R(RAX), fp(t + 8));
        scale_reg(es, RCX);
        addq(fp(sh), R(RCX));
        movq(R(RCX), fp(t));
        leaq(fp(t), R(RAX));
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
        load_val(e->type, M(RAX, 0));
        return;
    case EXPR_ATOMIC_STORE: {
        gen_expr(e->atomic_store.ptr);
        int32_t s = spill_push();
        movq(R(RAX), fp(s));
        gen_expr(e->atomic_store.value);
        movq(fp(s), R(RDX));
        spill_pop();
        store_val(e->atomic_store.value->type, RDX, 0);
        mfence();
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
    const char *disp;   /* --backtraces: its name in a frame, or NULL to leave it out */
    SrcLoc def;         /* and where it is defined */
} FnSpec;

/* ---- Register locals ----
 * Before a function's code is generated, its locals are ranked by how
 * often they are read or written, a use inside a loop counting eight times
 * one outside it, and integer locals whose address is never taken get the
 * callee-saved registers, most used first. A call leaves those intact, so
 * nothing is kept around calls; the cost is saving and restoring each
 * register the function uses, which the uses of its locals must outweigh.
 *
 * Locals share a register when their live ranges are apart. The walk
 * numbers the nodes in source order; a local is live from its binding (after
 * its initializer) to its last use, and through the end of every loop that
 * uses it but does not bind it, since the next iteration reads it again. A
 * use inside a defer, which runs at the scope's exits, counts as live to the
 * function's end. A few reads come later than their place in the source,
 * and the range covers them: a container found without code is indexed
 * after its index is evaluated, an assignment's target is found after its
 * value, and an argument may be read when the call is made (arg_at_call).
 * Otherwise code runs in source order, except for loops and defers, so a
 * local is never read outside its range.
 *
 * Float locals get %xmm8-%xmm15, which nothing else uses but a callee, so
 * only a local whose range no call falls inside can have one; they cost no
 * saving. The walk records where calls happen: after the operands for a
 * call, or anywhere in a node's span for one that calls midway (alloc). A
 * call in a defer counts where the defer is written: a local live at an exit
 * that runs it is live there too, as it was bound before and is used
 * after. */

typedef struct {
    const char *name;
    int64_t weight;
    int def, last;          /* the live range, in walk positions */
    bool bound, ok, addr;   /* addr: its address is taken */
    bool is_float;
} RegCand;

typedef struct { int lo, hi; } PosSpan;

typedef struct {
    RegCand *c;
    int n, cap;
    HMap idx;
    int pos;                /* the walk position */
    int in_defer;
    PosSpan *calls;         /* where calls happen */
    int ncalls, capcalls;
} RegCands;

enum { REG_MIN_WEIGHT = 3, POS_END = 1 << 30 };

static int64_t ra_weight(int depth) { return (int64_t)1 << (3 * (depth < 10 ? depth : 10)); }

static RegCand *ra_cand(RegCands *rc, const char *name) {
    size_t n = strlen(name);
    HEnt *e = hmap_get(&rc->idx, name, n);
    if (e) return &rc->c[e->val];
    RegCand c = { name, 0, rc->pos, rc->pos, false, true, false, false };
    DA_APPEND(rc->c, rc->n, rc->cap, c);
    hmap_put(&rc->idx, name, n)->val = rc->n - 1;
    return &rc->c[rc->n - 1];
}

static void ra_use(RegCands *rc, const char *name, int depth) {
    RegCand *c = ra_cand(rc, name);
    c->weight += ra_weight(depth);
    int at = rc->in_defer ? POS_END : rc->pos;
    if (at > c->last) c->last = at;
}

/* A binding site: where gen_* gives the name its home. */
static void ra_bind(RegCands *rc, const char *name, Type *t, int depth) {
    RegCand *c = ra_cand(rc, name);
    if (!c->bound) c->def = rc->pos;
    c->bound = true;
    ValKind k = val_kind(t);
    if (k != VK_INT && k != VK_FLOAT) c->ok = false;
    if (k == VK_FLOAT) c->is_float = true;
    c->weight += ra_weight(depth);
    if (rc->pos > c->last) c->last = rc->pos;
}

/* The bindings of a pattern over a value of type t, as gen_pat_bind makes
 * them. */
static void ra_pat(RegCands *rc, Pattern *pat, Type *t, int depth) {
    t = conc(t);
    switch (pat->kind) {
    case PAT_BINDING:
        ra_bind(rc, pat->binding.codegen_name ? pat->binding.codegen_name : pat->binding.name, t, depth);
        return;
    case PAT_SOME:
        if (pat->some_pat.inner) ra_pat(rc, pat->some_pat.inner, t->option.inner, depth);
        return;
    case PAT_OK:
        if (pat->some_pat.inner) ra_pat(rc, pat->some_pat.inner, t->result.inner, depth);
        return;
    case PAT_ERR:
        if (pat->some_pat.inner) ra_pat(rc, pat->some_pat.inner, type_int32(), depth);
        return;
    case PAT_VARIANT:
        if (pat->variant.payload && t->kind == TYPE_UNION) {
            Type *pt = union_variant_payload(t, pat->variant.variant);
            if (pt) ra_pat(rc, pat->variant.payload, pt, depth);
        }
        return;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++) {
            int64_t fo; Type *ft; ExtField *x;
            if (field_info_x(t, pat->struc.fields[i].name, &fo, &ft, &x))
                ra_pat(rc, pat->struc.fields[i].pattern, ft, depth);
        }
        return;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            int64_t fo;
            ra_pat(rc, pat->tuple_pat.patterns[i], tuple_elem_type(pat, t, i, &fo), depth);
        }
        return;
    default:
        return;
    }
}

/* &e: the local whose storage it addresses, if any, cannot be a register. */
static void ra_addr(RegCands *rc, Expr *e) {
    for (;;) {
        switch (e->kind) {
        case EXPR_GUARD: e = e->guard.body; continue;
        case EXPR_FIELD:
            if (e->field.codegen_name) return;
            e = e->field.object;
            continue;
        case EXPR_INDEX: {
            Type *ot = conc(e->index.object->type);
            if (ot->kind != TYPE_STRUCT || !ot->struc.is_tuple) return;
            e = e->index.object;
            continue;
        }
        case EXPR_IDENT:
            if (e->ident.is_local) {
                RegCand *c = ra_cand(rc, e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
                c->ok = false;
                c->addr = true;
            }
            return;
        default:
            return;
        }
    }
}

/* A loop from walk position start to here: a local it uses but that is
 * bound before it stays live to its end. */
static void ra_loop_end(RegCands *rc, int start) {
    for (int i = 0; i < rc->n; i++) {
        RegCand *c = &rc->c[i];
        if (c->def < start && c->last >= start && c->last < rc->pos) c->last = rc->pos;
    }
}

static bool pat_has_string(Pattern *p) {
    if (p->kind == PAT_STRING_LIT) return true;
    switch (p->kind) {
    case PAT_SOME: case PAT_OK: case PAT_ERR:
        return p->some_pat.inner && pat_has_string(p->some_pat.inner);
    case PAT_VARIANT:
        return p->variant.payload && pat_has_string(p->variant.payload);
    case PAT_STRUCT:
        for (int i = 0; i < p->struc.field_count; i++)
            if (pat_has_string(p->struc.fields[i].pattern)) return true;
        return false;
    case PAT_TUPLE:
        for (int i = 0; i < p->tuple_pat.pattern_count; i++)
            if (pat_has_string(p->tuple_pat.patterns[i])) return true;
        return false;
    case PAT_OR:
        for (int i = 0; i < p->or_pat.alt_count; i++)
            if (pat_has_string(p->or_pat.alts[i])) return true;
        return false;
    default:
        return false;
    }
}

/* Whether the code for e itself calls a function: 1 once its operands are
 * evaluated, 2 at some point among them, 0 not at all (a failure path that
 * calls and does not return aside). Every kind is listed, so a new one
 * cannot be missed. */
static int ra_calls(Expr *e) {
    switch (e->kind) {
    case EXPR_CALL: case EXPR_FREE: case EXPR_INTERP_STRING:
        return 1;
    case EXPR_ALLOC:
        return 2;
    case EXPR_CAST: {
        Type *from = conc(e->cast.operand->type), *to = conc(e->cast.target);
        return (is_str_type(from) && is_cstr_type(to)) || (is_cstr_type(from) && is_str_type(to));
    }
    case EXPR_BINARY:
        return (e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) && needs_eq_func(e->binary.left->type);
    case EXPR_MATCH:            /* a string pattern compares with memcmp */
        for (int i = 0; i < e->match_expr.arm_count; i++)
            if (pat_has_string(e->match_expr.arms[i].pattern)) return 2;
        return 0;
    case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_BOOL_LIT: case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT: case EXPR_CSTRING_LIT: case EXPR_VOID_LIT: case EXPR_IDENT:
    case EXPR_UNARY_PREFIX: case EXPR_UNARY_POSTFIX: case EXPR_FIELD: case EXPR_INDEX:
    case EXPR_SLICE: case EXPR_IF: case EXPR_LOOP: case EXPR_FOR: case EXPR_BREAK:
    case EXPR_CONTINUE: case EXPR_RETURN: case EXPR_BLOCK: case EXPR_FUNC:
    case EXPR_STRUCT_LIT: case EXPR_TUPLE_LIT: case EXPR_ARRAY_LIT: case EXPR_SLICE_LIT:
    case EXPR_SIZEOF: case EXPR_ALIGNOF: case EXPR_BITCAST: case EXPR_ENUM_OF:
    case EXPR_DEFAULT: case EXPR_ASSIGN: case EXPR_SOME: case EXPR_OK: case EXPR_ERR:
    case EXPR_ERROR_NAME: case EXPR_DEREF_FIELD: case EXPR_LET: case EXPR_LET_DESTRUCT:
    case EXPR_TYPE_VAR_REF: case EXPR_ASSERT: case EXPR_STATIC_ASSERT: case EXPR_DEFER:
    case EXPR_IGNORE: case EXPR_ATOMIC_LOAD: case EXPR_ATOMIC_STORE: case EXPR_GUARD:
    case EXPR_ERROR:
        return 0;
    }
    return 2;
}

/* The local a place found without code (fixed_place) is based on, if any. */
static Expr *place_root(Expr *e) {
    for (;;) {
        switch (e->kind) {
        case EXPR_GUARD: e = e->guard.body; continue;
        case EXPR_CAST: e = e->cast.operand; continue;
        case EXPR_FIELD: case EXPR_DEREF_FIELD:
            if (e->field.codegen_name || !e->field.object) return NULL;
            e = e->field.object;
            continue;
        case EXPR_INDEX: {
            Type *ot = conc(e->index.object->type);
            if (ot->kind != TYPE_STRUCT || !ot->struc.is_tuple) return NULL;
            e = e->index.object;
            continue;
        }
        case EXPR_IDENT:
            return e->ident.is_local ? e : NULL;
        default:
            return NULL;
        }
    }
}

/* A place read after the code that follows it in the source: its local is
 * live to here. */
static void ra_read_late(RegCands *rc, Expr *place) {
    Expr *id = place_root(place);
    if (!id) return;
    RegCand *c = ra_cand(rc, id->ident.codegen_name ? id->ident.codegen_name : id->ident.name);
    if (rc->pos > c->last) c->last = rc->pos;
}

static void ra_call_at(RegCands *rc, int lo, int hi) {
    PosSpan sp = { lo, hi };
    DA_APPEND(rc->calls, rc->ncalls, rc->capcalls, sp);
}

/* Whether a call happens while c is live. */
static bool ra_crosses_call(RegCands *rc, RegCand *c) {
    for (int i = 0; i < rc->ncalls; i++)
        if (c->def < rc->calls[i].hi && c->last > rc->calls[i].lo) return true;
    return false;
}

static void ra_expr(RegCands *rc, Expr *e, int depth);

typedef struct { RegCands *rc; int depth; } RaWalk;

static void ra_child(Expr *e, void *ctx) { RaWalk *w = ctx; ra_expr(w->rc, e, w->depth); }

static void ra_body(RegCands *rc, Expr **body, int n, int depth) {
    for (int i = 0; i < n; i++) ra_expr(rc, body[i], depth);
}

static void ra_node(RegCands *rc, Expr *e, int depth);

static void ra_expr(RegCands *rc, Expr *e, int depth) {
    if (!e) return;
    int start = ++rc->pos;
    ra_node(rc, e, depth);
    switch (e->kind) {
    case EXPR_INDEX: rc->pos++; ra_read_late(rc, e->index.object); break;
    case EXPR_ASSIGN: rc->pos++; ra_read_late(rc, e->assign.target); break;
    case EXPR_CALL:
        rc->pos++;
        for (int i = 0; i < e->call.arg_count; i++) ra_read_late(rc, e->call.args[i]);
        break;
    default: break;
    }
    switch (ra_calls(e)) {
    case 1: rc->pos++; ra_call_at(rc, rc->pos, rc->pos); break;
    case 2: rc->pos++; ra_call_at(rc, start, rc->pos); break;
    default: break;
    }
}

static void ra_node(RegCands *rc, Expr *e, int depth) {
    switch (e->kind) {
    case EXPR_IDENT:
        if (e->ident.is_local) ra_use(rc, e->ident.codegen_name ? e->ident.codegen_name : e->ident.name, depth);
        return;
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_AMP) ra_addr(rc, e->unary_prefix.operand);
        break;
    case EXPR_FUNC:             /* a function of its own; the captures are read here */
        for (int i = 0; i < e->func.capture_count; i++) ra_use(rc, e->func.captures[i].codegen_name, depth);
        return;
    case EXPR_LET: {
        ra_expr(rc, e->let_expr.let_init, depth);
        rc->pos++;
        Type *t = conc(e->let_expr.let_type ? e->let_expr.let_type : e->let_expr.let_init->type);
        if (!type_valueless(t))
            ra_bind(rc, e->let_expr.codegen_name ? e->let_expr.codegen_name : e->let_expr.let_name, t, depth);
        return;
    }
    case EXPR_LET_DESTRUCT:
        ra_expr(rc, e->let_destruct.init, depth);
        rc->pos++;
        ra_pat(rc, e->let_destruct.pattern, e->let_destruct.init_type, depth);
        return;
    case EXPR_MATCH:
        ra_expr(rc, e->match_expr.subject, depth);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            rc->pos++;
            ra_pat(rc, arm->pattern, e->match_expr.subject->type, depth);
            ra_expr(rc, arm->guard, depth);
            ra_body(rc, arm->body, arm->body_count, depth);
        }
        return;
    case EXPR_DEFER:
        rc->in_defer++;
        ra_expr(rc, e->defer_expr.value, depth);
        rc->in_defer--;
        return;
    case EXPR_LOOP: {
        int start = ++rc->pos;
        ra_body(rc, e->loop_expr.body, e->loop_expr.body_count, depth + 1);
        rc->pos++;
        ra_loop_end(rc, start);
        return;
    }
    case EXPR_FOR: {
        /* A range's start goes into the loop variable before the end is
         * evaluated (gen_for). */
        int in = depth + 1;
        const char *vn = e->for_expr.var_codegen_name ? e->for_expr.var_codegen_name : e->for_expr.var;
        ra_expr(rc, e->for_expr.iter, depth);
        rc->pos++;
        if (e->for_expr.range_end) ra_bind(rc, vn, conc(e->for_expr.iter->type), in);
        ra_expr(rc, e->for_expr.range_end, depth);
        int start = ++rc->pos;
        if (e->for_expr.range_end) {
            ra_use(rc, vn, in);             /* the test and the step */
            ra_use(rc, vn, in);
        } else {
            Type *elem = conc(conc(e->for_expr.iter->type)->slice.elem);
            if (e->for_expr.var_pattern) ra_pat(rc, e->for_expr.var_pattern, elem, in);
            else ra_bind(rc, vn, elem, in);
            if (e->for_expr.index_var)
                ra_bind(rc, e->for_expr.index_codegen_name ? e->for_expr.index_codegen_name
                                                           : e->for_expr.index_var, type_int64(), in);
        }
        ra_body(rc, e->for_expr.body, e->for_expr.body_count, in);
        rc->pos++;
        ra_loop_end(rc, start);
        return;
    }
    default:
        break;
    }
    RaWalk w = { rc, depth };
    expr_for_each_child(e, ra_child, &w);
}

/* Most used first; then in source order, for a stable choice. */
static int ra_heavier(const void *a, const void *b) {
    const RegCand *x = *(RegCand *const *)a, *y = *(RegCand *const *)b;
    if (x->weight != y->weight) return x->weight > y->weight ? -1 : 1;
    return x < y ? -1 : x > y;
}

/* Choose the function's register locals and give each register used a
 * save slot. */
static void assign_registers(FnSpec *f) {
    RegCands rc = { 0 };
    for (int i = 0; i < f->nparams; i++) {
        Param *p = &f->params[i];
        ra_bind(&rc, p->codegen_name ? p->codegen_name : p->name, conc(p->type), 0);
    }
    if (f->lambda) {
        for (int i = 0; i < f->lambda->func.capture_count; i++) {
            Capture *c = &f->lambda->func.captures[i];
            ra_bind(&rc, c->codegen_name, conc(c->type), 0);
        }
        if (f->lambda->func.self_codegen_name) ra_cand(&rc, f->lambda->func.self_codegen_name)->ok = false;
    }
    ra_body(&rc, f->body, f->body_count, 0);
    for (int i = 0; i < rc.n; i++)
        if (rc.c[i].addr) hmap_put(&g_addr_taken, rc.c[i].name, strlen(rc.c[i].name))->val = 1;
    g_addr_known = true;
    /* Most used first, each into the first register whose locals' ranges
     * it does not meet; then the callee-saved registers whose locals are used
     * too little to pay for saving one are left unused. Registers 0..NSAVED
     * are SAVED_REGS, the rest %xmm8 on. */
    enum { NREG = NSAVED + 8 };
    RegCand **taken[NREG] = { 0 };
    int ntaken[NREG] = { 0 }, captaken[NREG] = { 0 };
    int64_t wsum[NREG] = { 0 };
    RegCand **order = xcalloc((size_t)(rc.n ? rc.n : 1), sizeof(RegCand *));
    int norder = 0;
    for (int i = 0; i < rc.n; i++)
        if (rc.c[i].bound && rc.c[i].ok) order[norder++] = &rc.c[i];
    qsort(order, (size_t)norder, sizeof *order, ra_heavier);
    for (int i = 0; i < norder; i++) {
        RegCand *best = order[i];
        if (best->is_float && ra_crosses_call(&rc, best)) continue;
        for (int r = best->is_float ? NSAVED : 0; r < (best->is_float ? NREG : NSAVED); r++) {
            bool meets = false;
            for (int k = 0; k < ntaken[r] && !meets; k++)
                meets = best->def <= taken[r][k]->last && taken[r][k]->def <= best->last;
            if (meets) continue;
            DA_APPEND(taken[r], ntaken[r], captaken[r], best);
            wsum[r] += best->weight;
            break;
        }
    }
    for (int r = NSAVED; r < NREG; r++) {
        for (int k = 0; k < ntaken[r]; k++)
            hmap_put(&g_reg_locals, taken[r][k]->name, strlen(taken[r][k]->name))->val = REG_XMM + 8 + (r - NSAVED);
        free(taken[r]);
    }
    g_nsaved = 0;
    for (int r = 0; r < NSAVED; r++) {
        if (ntaken[r] && wsum[r] >= REG_MIN_WEIGHT) {
            for (int k = 0; k < ntaken[r]; k++)
                hmap_put(&g_reg_locals, taken[r][k]->name, strlen(taken[r][k]->name))->val = SAVED_REGS[r];
            g_save_reg[g_nsaved] = SAVED_REGS[r];
            g_save_slot[g_nsaved++] = slot_alloc(8, 8);
        }
        free(taken[r]);
    }
    free(order);
    free(rc.c);
    free(rc.calls);
    hmap_free(&rc.idx);
}

static void begin_function(void) {
    hmap_clear(&g_locals);
    hmap_clear(&g_reg_locals);
    hmap_clear(&g_addr_taken);
    g_addr_known = false;
    g_nsaved = 0;
    g_frame = 0;
    g_spill_n = g_spill_depth = 0;
    g_hold_n = 0;
    g_hidden_ret = 0;
    g_ret_label = new_label();
    g_guards_suppressed = g_overflow_checked = false;
    g_defer = NULL;
    g_loop = NULL;
    g_loc_file = g_loc_line = 0;
    code_reset(&g_fnbody);
    code_reset(&g_cold);
    g_cur = &g_fnbody;
    g_unreached = NULL;
}

/* The function into the text: prologue, body, epilogue, then the cold
 * failure paths, under one unwind entry. Returns the label of its end. */
static int end_function(const char *sym, bool global) {
    Code *o = g_cur = &g_text;
    int s = asm_sym(sym);
    if (global) asm_sym_global(s);
    x_align(o, 16);
    x_func(o, s);
    x_cfi(o, CFI_START);
    x_op0(o, OP_PUSH_RBP);
    x_cfi(o, CFI_PUSHED);
    movq(R(RSP), R(RBP));
    x_cfi(o, CFI_FRAME);
    int64_t frame = align_up(g_frame, 16);
    if (frame) subq(IMM(frame), R(RSP));
    for (int i = 0; i < g_nsaved; i++) {
        movq(R(g_save_reg[i]), fp(g_save_slot[i]));
        x_cfi_saved(o, g_save_reg[i], g_save_slot[i] - 16);     /* the CFA is %rbp + 16 */
    }
    x_drop_final_jmp(&g_fnbody, g_ret_label);
    code_append(o, &g_fnbody);
    label(g_ret_label);
    restore_saved();
    leave();
    x_cfi(o, CFI_LEFT);
    x_op0(o, OP_RET);
    x_cfi(o, CFI_RESTORE);
    code_append(o, &g_cold);
    int end = new_label();
    x_label(o, end);
    x_cfi(o, CFI_END);
    x_func_end(o, s);
    g_cur = NULL;
    return end;
}

/* ---- Backtraces ----
 * Under --backtraces each FC function gets an entry in a table: its code's
 * range, its name in a frame (NULL for one left out, an extern's wrapper)
 * and where it is defined. A failure calls fc_dump_backtrace before it
 * aborts; that walks the stack with the C library's backtrace(), which
 * unwinds through the functions' .eh_frame entries, names each return
 * address by the entry whose range holds it, and stops at main. A frame no
 * entry holds (the C library's, a shim function's) is printed as its
 * address. The output is the C backend's. */

typedef struct { int sym, end; const char *name; SrcLoc def; } TraceEntry;
static TraceEntry *g_traces;
static int g_ntraces, g_traces_cap;

static void trace_add(const char *sym, int end, const char *name, SrcLoc def) {
    TraceEntry t = { asm_sym(sym), end, name, def };
    DA_APPEND(g_traces, g_ntraces, g_traces_cap, t);
}

/* The table, { start, end, name, file, line } per function, and the helper
 * that prints a backtrace from it. */
static void emit_backtrace_helper(void) {
    enum { ENTRY = 40, NFRAMES = 64 };
    int tab = new_label();
    d_align(ASM_RELRO, 8);
    d_def(ASM_RELRO, tab);
    for (int i = 0; i < g_ntraces; i++) {
        TraceEntry *t = &g_traces[i];
        d_addr(ASM_RELRO, t->sym, 0);
        d_addr(ASM_RELRO, t->end, 0);
        if (t->name) d_addr(ASM_RELRO, cstr_label(t->name), 0);
        else d_zero(ASM_RELRO, 8);
        d_addr(ASM_RELRO, cstr_label(t->def.filename ? t->def.filename : "<unknown>"), 0);
        d_int(ASM_RELRO, (uint64_t)(t->def.line > 0 ? t->def.line : 0), 8);
    }
    if (g_ntraces == 0) d_zero(ASM_RELRO, 8);

    begin_function();
    int32_t frames = slot_alloc(8 * NFRAMES, 16);
    int32_t n = slot_alloc(8, 8), i = slot_alloc(8, 8), printed = slot_alloc(8, 8), ent = slot_alloc(8, 8);
    int ltop = new_label(), lsearch = new_label(), lnext = new_label();
    int lunknown = new_label(), lfound = new_label(), lcont = new_label();
    leaq(fp(frames), R(RDI));
    movl(IMM(NFRAMES), R(RSI));
    call_plt("backtrace");
    movslq(R(RAX), R(RAX));
    movq(R(RAX), fp(n));
    cmpq(IMM(2), R(RAX));
    jcc(CC_L, g_ret_label);
    leaq(RIPREL(cstr_label("backtrace:\n"), 0), R(RDI));
    movq(GOTREL(asm_sym("stderr")), R(RSI));
    movq(M(RSI, 0), R(RSI));
    call_plt("fputs");
    movq(IMM(0), fp(printed));
    movq(IMM(1), fp(i));            /* frame 0 is this function */
    label(ltop);
    movq(fp(i), R(RAX));
    cmpq(fp(n), R(RAX));
    jcc(CC_GE, g_ret_label);
    /* The entry holding the return address less one: the call. */
    movq(MI(RBP, RAX, 8, frames), R(RDX));
    subq(IMM(1), R(RDX));
    leaq(RIPREL(tab, 0), R(RSI));
    mov_imm_to(g_ntraces, RCX);
    label(lsearch);
    testq(R(RCX), R(RCX));
    jcc(CC_E, lunknown);
    cmpq(M(RSI, 0), R(RDX));
    jcc(CC_B, lnext);
    cmpq(M(RSI, 8), R(RDX));
    jcc(CC_B, lfound);
    label(lnext);
    addq(IMM(ENTRY), R(RSI));
    subq(IMM(1), R(RCX));
    jmp(lsearch);
    label(lunknown);
    movq(GOTREL(asm_sym("stderr")), R(RDI));
    movq(M(RDI, 0), R(RDI));
    leaq(RIPREL(cstr_label("  #%d %p\n"), 0), R(RSI));
    movq(fp(printed), R(RDX));
    movq(fp(i), R(RAX));
    movq(MI(RBP, RAX, 8, frames), R(RCX));
    xorl(R(RAX), R(RAX));
    call_plt("fprintf");
    incq(fp(printed));
    jmp(lcont);
    label(lfound);
    cmpq(IMM(0), M(RSI, 16));
    jcc(CC_E, lcont);               /* a frame left out */
    movq(R(RSI), fp(ent));
    movq(GOTREL(asm_sym("stderr")), R(RDI));
    movq(M(RDI, 0), R(RDI));
    leaq(RIPREL(cstr_label("  #%d %-24s defined at %s:%d\n"), 0), R(RSI));
    movq(fp(printed), R(RDX));
    movq(fp(ent), R(RAX));
    movq(M(RAX, 16), R(RCX));
    movq(M(RAX, 24), R(R8));
    movq(M(RAX, 32), R(R9));
    xorl(R(RAX), R(RAX));
    call_plt("fprintf");
    incq(fp(printed));
    movq(fp(ent), R(RAX));          /* main is the last frame shown */
    leaq(RIPREL(cstr_label("main"), 0), R(RCX));
    cmpq(R(RCX), M(RAX, 16));
    jcc(CC_E, g_ret_label);
    label(lcont);
    incq(fp(i));
    jmp(ltop);
    end_function(BACKTRACE_SYM, false);
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
    assign_registers(f);

    Abi ra = abi_classify(g_fn_ret);
    int ni = 0, ns = 0;
    int64_t stack = 16;
    if (ra.n == -1) {
        g_hidden_ret = slot_alloc(8, 8);
        movq(R(RDI), fp(g_hidden_ret));
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
                Opnd home = local_home(name, pt);
                ValKind vk = val_kind(pt);
                if (vk == VK_INT && home.kind == OPND_REG) {
                    movq(R(IREG[ni++]), home);
                    normalize_reg(pt, (Reg)home.base);  /* the caller need not have */
                }
                else if (vk == VK_INT) movq(R(IREG[ni++]), home);
                else if (vk == VK_FLOAT && home.kind == OPND_XMM) movaps(X(ns++), home);
                else if (vk == VK_FLOAT) movq(X(ns++), home);
                else
                    for (int k = 0; k < a.n; k++) {
                        if (a.cls[k] == 'I') movq(R(IREG[ni++]), fp(home.disp + 8 * k));
                        else movq(X(ns++), fp(home.disp + 8 * k));
                    }
                continue;
            }
        }
        int64_t al = ty_align(pt);
        if (al < 8) al = 8;
        stack = align_up(stack, al);
        Opnd home;
        bool reg = local_find(name, &home);
        if (reg && home.kind == OPND_REG) load_int_to(pt, M(RBP, stack), (Reg)home.base);
        else if (reg && home.kind == OPND_XMM) (is_f32(pt) ? movss(M(RBP, stack), home) : movsd(M(RBP, stack), home));
        else local_bind(name, (int32_t)stack);
        stack += align_up(ty_size(pt), 8);
    }
    int32_t ctx_slot = 0;
    if (f->has_ctx) {
        ctx_slot = slot_alloc(8, 8);
        if (ni < 6) movq(R(IREG[ni++]), fp(ctx_slot));
        else {
            movq(M(RBP, stack), R(RAX));
            movq(R(RAX), fp(ctx_slot));
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
            Opnd home = local_home(c->codegen_name, ct);
            movq(fp(ctx_slot), R(RDX));
            if (val_kind(ct) == VK_AGG) copy_mem(RBP, home.disp, RDX, offs[i], ty_size(ct));
            else {
                load_val(ct, M(RDX, offs[i]));
                store_at(ct, home);
            }
        }
        free(offs);
    }
    if (f->lambda && f->lambda->func.self_codegen_name && f->lambda->func.self_referenced) {
        int32_t s = slot_alloc(16, 8);
        local_bind(f->lambda->func.self_codegen_name, s);
        leaq(RIPREL(asm_sym(f->sym), 0), R(RAX));
        movq(R(RAX), fp(s));
        if (f->lambda->func.capture_count > 0) {
            movq(fp(ctx_slot), R(RAX));
            movq(R(RAX), fp(s + 8));
        } else {
            movq(IMM(0), fp(s + 8));
        }
    }

    defer_push(false);
    bool done = gen_stmts(f->body, f->body_count, TAIL_RETURN);
    if (!done) emit_scope_defers(g_defer);
    defer_pop();
    if (f->is_main && !done) xorl(R(RAX), R(RAX));
    int end = end_function(f->sym, f->global);
    if (g_backtraces) trace_add(f->sym, end, f->disp, f->def);
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
    f.disp = "<lambda>";
    f.def = lam->loc;
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
    int name = asm_sym(arena_sprintf(g_arena, "fc_ctramp_%s", t->sym));
    g_cur = &g_text;
    x_align(g_cur, 16);
    x_func(g_cur, name);
    if (ok) {
        xorl(R(IREG[ni]), R(IREG[ni]));
        jmp_sym(t->sym);
    } else {
        internal_error((SrcLoc){0}, "C callback '%s' has too many arguments for the native backend", t->sym);
        x_op0(g_cur, OP_UD2);
    }
    x_func_end(g_cur, name);
    g_cur = NULL;
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
    Opnd am = M(RAX, off), bm = M(RCX, off);
    switch (val_kind(t)) {
    case VK_VOID: return;
    case VK_INT:
        movq(M(RBP, -16), R(RCX));
        movq(M(RBP, -8), R(RAX));
        load_int(t, bm);
        movq(R(RAX), R(RDX));
        movq(M(RBP, -8), R(RAX));
        load_int(t, am);
        cmpq(R(RDX), R(RAX));
        jcc(CC_NE, lfalse);
        return;
    case VK_FLOAT: {
        bool f = is_f32(t);
        movq(M(RBP, -8), R(RAX));
        movq(M(RBP, -16), R(RCX));
        (f ? movss(am, X(0)) : movsd(am, X(0)));
        (f ? ucomiss(bm, X(0)) : ucomisd(bm, X(0)));
        jcc(CC_NE, lfalse);
        jcc(CC_P, lfalse);
        return;
    }
    case VK_AGG:
        break;
    }
    movq(M(RBP, -8), R(RDI));
    movq(M(RBP, -16), R(RSI));
    if (off) { addq(IMM(off), R(RDI)); addq(IMM(off), R(RSI)); }
    call_sym(eq_helper(t));
    testb(R(RAX), R(RAX));
    jcc(CC_E, lfalse);
}

/* memcmp of n bytes at off(A) and off(B); jumps to lfalse if they differ. */
static void eq_memcmp_bytes(int64_t off, int64_t n, int lfalse) {
    if (n <= 0) return;
    movq(M(RBP, -8), R(RDI));
    movq(M(RBP, -16), R(RSI));
    if (off) { addq(IMM(off), R(RDI)); addq(IMM(off), R(RSI)); }
    movq(IMM(n), R(RDX));
    call_c("memcmp");
    testl(R(RAX), R(RAX));
    jcc(CC_NE, lfalse);
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
    movq(M(RBP, -8), R(RAX));
    movq(R(RAX), M(RBP, -40));
    movq(M(RBP, -16), R(RAX));
    movq(R(RAX), M(RBP, -48));
    if (off) { addq(IMM(off), M(RBP, -8)); addq(IMM(off), M(RBP, -16)); }
    movq(IMM(0), M(RBP, -32));
    label(ltop);
    movq(M(RBP, -32), R(RAX));
    if (count_in_slot) cmpq(M(RBP, -24), R(RAX));
    else cmpq(IMM(count), R(RAX));
    jcc(CC_GE, ldone);
    eq_cmp(elem, 0, lfalse);
    addq(IMM(es), M(RBP, -8));
    addq(IMM(es), M(RBP, -16));
    incq(M(RBP, -32));
    jmp(ltop);
    label(ldone);
    movq(M(RBP, -40), R(RAX));
    movq(R(RAX), M(RBP, -8));
    movq(M(RBP, -48), R(RAX));
    movq(R(RAX), M(RBP, -16));
}

static void emit_eq_helper(EqHelper *h) {
    Type *t = conc(h->type);
    begin_function();
    g_frame = 48;
    int ltrue = new_label(), lfalse = new_label();
    movq(R(RDI), M(RBP, -8));
    movq(R(RSI), M(RBP, -16));
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
                movq(M(RBP, -16), R(RDX));
                load_ext_field(ft, &L->ext[i], RDX, 0, L->off[i]);
                movq(R(RAX), R(RSI));
                movq(M(RBP, -8), R(RDX));
                load_ext_field(ft, &L->ext[i], RDX, 0, L->off[i]);
                cmpq(R(RSI), R(RAX));
                jcc(CC_NE, lfalse);
            } else {
                eq_cmp(ft, L->off[i], lfalse);
            }
        }
        break;
    }
    case TYPE_UNION: {
        Layout *L = agg_layout(t);
        movq(M(RBP, -8), R(RAX));
        movq(M(RBP, -16), R(RCX));
        movl(M(RAX, 0), R(RDX));
        cmpl(M(RCX, 0), R(RDX));
        jcc(CC_NE, lfalse);
        for (int i = 0; i < L->n; i++) {
            if (!L->ftype[i]) continue;
            int lnext = new_label();
            movq(M(RBP, -8), R(RAX));
            cmpl(IMM(i), M(RAX, 0));
            jcc(CC_NE, lnext);
            eq_cmp(L->ftype[i], L->payload_off, lfalse);
            jmp(ltrue);
            label(lnext);
        }
        break;
    }
    case TYPE_SLICE: {
        Type *elem = conc(t->slice.elem);
        movq(M(RBP, -8), R(RAX));
        movq(M(RBP, -16), R(RCX));
        movq(M(RAX, 8), R(RDX));
        cmpq(M(RCX, 8), R(RDX));
        jcc(CC_NE, lfalse);
        testq(R(RDX), R(RDX));
        jcc(CC_E, ltrue);
        if (plain_bytes_eq(elem)) {
            movq(M(RAX, 0), R(RDI));
            movq(M(RCX, 0), R(RSI));
            int64_t es = ty_size(elem);
            scale_reg(es, RDX);
            call_c("memcmp");
            testl(R(RAX), R(RAX));
            jcc(CC_NE, lfalse);
        } else {
            movq(R(RDX), M(RBP, -24));
            movq(M(RAX, 0), R(RAX));
            movq(R(RAX), M(RBP, -8));
            movq(M(RCX, 0), R(RCX));
            movq(R(RCX), M(RBP, -16));
            eq_elements(elem, 0, 0, true, lfalse);
        }
        break;
    }
    case TYPE_OPTION: {
        int64_t is = ty_size(t->option.inner);
        movq(M(RBP, -8), R(RAX));
        movq(M(RBP, -16), R(RCX));
        movzbl(M(RAX, is), R(RDX));
        cmpb(M(RCX, is), R(RDX));
        jcc(CC_NE, lfalse);
        testb(R(RDX), R(RDX));
        jcc(CC_E, ltrue);
        eq_cmp(t->option.inner, 0, lfalse);
        break;
    }
    case TYPE_RESULT: {
        movq(M(RBP, -8), R(RAX));
        movq(M(RBP, -16), R(RCX));
        movl(M(RAX, 0), R(RDX));
        cmpl(M(RCX, 0), R(RDX));
        jcc(CC_NE, lfalse);
        Type *in = conc(t->result.inner);
        if (in && in->kind != TYPE_VOID) {
            testl(R(RDX), R(RDX));
            jcc(CC_NE, ltrue);
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
    movl(IMM(1), R(RAX));
    jmp(g_ret_label);
    label(lfalse);
    xorl(R(RAX), R(RAX));
    end_function(h->name, false);
}

/* ================================================================
 * Global data
 * ================================================================ */

typedef struct { int64_t off; int sym; int64_t addend; } Reloc;
typedef struct {
    unsigned char *data;
    int64_t size;
    Reloc *rel;
    int nrel, caprel;
} DataImg;

static void img_put(DataImg *img, int64_t off, uint64_t v, int w) {
    for (int i = 0; i < w && off + i < img->size; i++) img->data[off + i] = (unsigned char)(v >> (8 * i));
}

static void img_reloc(DataImg *img, int64_t off, int sym, int64_t addend) {
    Reloc r = { off, sym, addend };
    DA_APPEND(img->rel, img->nrel, img->caprel, r);
}

/* The data object `img` at label or symbol `sym` in a section. */
static void emit_img(int sec, int sym, DataImg *img, int64_t align) {
    d_align(sec, align >= 16 ? 16 : align >= 8 ? 8 : align >= 4 ? 4 : align >= 2 ? 2 : 1);
    d_def(sec, sym);
    int64_t base = d_size(sec);
    d_bytes(sec, img->data, img->size);
    for (int k = 0; k < img->nrel; k++)
        d_addr_at(sec, base + img->rel[k].off, img->rel[k].sym, img->rel[k].addend);
}

/* Slice literal backings a global's image refers to, emitted ahead of it in
 * its section. */
typedef struct { int label; DataImg img; int64_t align; } Backing;
typedef struct { Backing *items; int count, cap; } Backings;

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

static bool ser(Expr *e, Type *t, DataImg *img, int64_t off, Backings *sec);

/* A slice literal's elements as their own data object; returns its label. */
static int ser_backing(Expr *al, Backings *sec) {
    Type *elem = conc(al->array_lit.elem_type);
    int64_t es = ty_size(elem);
    int64_t N = al->array_lit.size_expr->int_lit.value;
    DataImg img = { 0 };
    img.size = N * es;
    img.data = xcalloc((size_t)(img.size > 0 ? img.size : 1), 1);
    bool ok = true;
    for (int i = 0; i < al->array_lit.elem_count && ok; i++)
        ok = ser(al->array_lit.elems[i], elem, &img, i * es, sec);
    if (!ok) {
        free(img.data);
        free(img.rel);
        return -1;
    }
    Backing b = { new_label(), img, ty_align(elem) };
    DA_APPEND(sec->items, sec->count, sec->cap, b);
    return b.label;
}

static bool ser(Expr *e, Type *t, DataImg *img, int64_t off, Backings *sec) {
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
            img_reloc(img, off, l, 0);
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
            img_reloc(img, off, asm_sym(ctramp_sym(sym, conc(e->unary_prefix.operand->type))), 0);
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
        img_reloc(img, off, l, 0);
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
            img_reloc(img, off, l, 0);
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
        img_reloc(img, off, asm_sym(sym), 0);
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
        Backings bk = { 0 };
        bool ok = ser(d->let.init, t, &img, 0, &bk);
        if (ok) {
            int sec = d->let.is_mut ? ASM_DATA : (g_img_has_reloc ? ASM_RELRO : ASM_RODATA);
            for (int i = 0; i < bk.count; i++)
                emit_img(sec, bk.items[i].label, &bk.items[i].img, bk.items[i].align);
            emit_img(sec, asm_sym(name), &img, align);
        }
        for (int i = 0; i < bk.count; i++) { free(bk.items[i].img.data); free(bk.items[i].img.rel); }
        free(bk.items);
        free(img.data);
        free(img.rel);
        if (ok) return;
    }
    d_align(ASM_BSS, align >= 16 ? 16 : align >= 8 ? 8 : align >= 4 ? 4 : align >= 2 ? 2 : 1);
    d_def(ASM_BSS, asm_sym(name));
    d_zero(ASM_BSS, size);
    DA_APPEND(g_runtime_globals, g_rg_n, g_rg_cap, d);
}

/* The C entry point. Like the emitted C's main, it runs the initializers
 * that are not static data first (module constants, then the entry file's
 * globals in order), in its own frame: a global's slice literal backing or
 * interpolation buffer lives there for the whole program. Then it builds
 * the str[] arguments and calls fc_main. */
static void emit_c_main(SrcLoc main_def) {
    begin_function();
    g_fn_ret = type_int32();
    g_tco_ok = false;
    int32_t argc = slot_alloc(8, 8), argv = slot_alloc(8, 8);
    movslq(R(RDI), R(RDI));
    movq(R(RDI), fp(argc));
    movq(R(RSI), fp(argv));
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_rg_n; i++) {
            Decl *d = g_runtime_globals[i];
            if ((pass == 0) != d->let.is_module_member) continue;
            if (!d->let.init) continue;
            emit_loc(d->loc);
            const char *name = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            defer_push(false);
            gen_expr(d->let.init);
            leaq(RIPREL(asm_sym(name), 0), R(RDX));
            store_val(d->let.resolved_type, RDX, 0);
            defer_pop();
        }
    }
    int32_t arr = slot_alloc(8, 8), idx = slot_alloc(8, 8);
    int ltop = new_label(), lgo = new_label();
    movq(fp(argc), R(RAX));
    shlq(IMM(4), R(RAX));
    dyn_alloca_rax();
    movq(R(RAX), fp(arr));
    movq(IMM(0), fp(idx));
    label(ltop);
    movq(fp(idx), R(RAX));
    cmpq(fp(argc), R(RAX));
    jcc(CC_GE, lgo);
    movq(fp(argv), R(RCX));
    movq(MI(RCX, RAX, 8, 0), R(RDI));
    shlq(IMM(4), R(RAX));
    addq(fp(arr), R(RAX));
    movq(R(RDI), M(RAX, 0));
    call_c("strlen");
    movq(fp(idx), R(RCX));
    shlq(IMM(4), R(RCX));
    addq(fp(arr), R(RCX));
    movq(R(RAX), M(RCX, 8));
    incq(fp(idx));
    jmp(ltop);
    label(lgo);
    movq(fp(arr), R(RDI));
    movq(fp(argc), R(RSI));
    call_sym("fc_main");
    int end = end_function("main", true);
    /* A failing initializer runs in it: shown as main. */
    if (g_backtraces) trace_add("main", end, "main", main_def);
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

/* ---- The program's C environment: its defines, includes, include path
 * (the sources' directories, then -I), and which headers sit beside its
 * sources (whose functions the C library cannot supply; a header found
 * through -I is a library's, like a system header). ---- */
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

/* Append " -I<dir>" to the include path, `dir` quoted as one shell word. */
static void add_include_dir(const char *dir) {
    buf_puts(&g_ciflags, " -I'");
    for (const char *p = dir; *p; p++) {
        if (*p == '\'') buf_puts(&g_ciflags, "'\\''");
        else buf_put(&g_ciflags, p, 1);
    }
    buf_puts(&g_ciflags, "'");
}

static void build_cenv(Program *prog, Decl **all, int all_count,
                       const char *const *include_dirs, int ninclude_dirs) {
    for (int i = 0; i < all_count; i++) {
        const char *fn = all[i]->loc.filename;
        if (!fn) continue;
        const char *slash = strrchr(fn, '/');
        char *dir = slash ? str_ndup(fn, (size_t)(slash - fn)) : str_dup(".");
        if (!hmap_get(&g_src_dirs, dir, strlen(dir))) {
            hmap_put(&g_src_dirs, dir, strlen(dir));
            add_include_dir(dir);
        } else free(dir);
    }
    for (int i = 0; i < ninclude_dirs; i++) add_include_dir(include_dirs[i]);
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

/* The shim's pointer to a C function from a local header. */
static int xp_sym(const char *cname) {
    char *name = str_sprintf("fc_xp_%s", cname);
    int s = asm_sym(name);
    free(name);
    return s;
}

/* ---- The C compiler's cache ----
 * The probe's output and the shim's object are kept in $XDG_CACHE_HOME/fcc
 * (else ~/.cache/fcc), keyed by the C compiler, the include path (the -I
 * options, and CPATH and C_INCLUDE_PATH, which the compiler reads too) and
 * the C source. An entry also lists every file the compiler read, system
 * headers included, with its size and times, and every place in the header
 * search where a new file would have been read instead of one of them. It is
 * used only while each file is unchanged and each place still empty: editing
 * a header, or adding one that shadows it, compiles again. An entry is
 *     fcc-cache 2
 *     <size> <mtime s> <mtime ns> <ctime s> <ctime ns> <path>     (per file)
 *     ! <path>                                    (per place that must not exist)
 *     -
 *     <the output> */

#define CACHE_MAGIC "fcc-cache 2\n"

static void put_dep(Buf *b, const char *path, const struct stat *st) {
    buf_printf(b, "%lld %lld %ld %lld %ld %s\n", (long long)st->st_size,
               (long long)st->st_mtim.tv_sec, (long)st->st_mtim.tv_nsec,
               (long long)st->st_ctim.tv_sec, (long)st->st_ctim.tv_nsec, path);
}

/* The output in the cache entry at `path`, with its length in *len; NULL if
 * there is no entry or a file it lists has changed since. */
static char *cache_read(const char *path, int *len) {
    int n = 0;
    char *e = read_file(path, &n);
    if (!e) return NULL;
    size_t ml = strlen(CACHE_MAGIC);
    bool ok = (size_t)n >= ml && memcmp(e, CACHE_MAGIC, ml) == 0;
    char *p = e + ml, *end = e + n;
    Buf now = { 0 };
    while (ok) {
        char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) { ok = false; break; }
        if (eol - p == 1 && *p == '-') { p = eol + 1; break; }
        if (eol - p > 2 && p[0] == '!' && p[1] == ' ') {
            char *place = str_ndup(p + 2, (size_t)(eol - p - 2));
            struct stat st;
            ok = stat(place, &st) != 0;
            free(place);
            p = eol + 1;
            continue;
        }
        /* the path follows five numbers; the line must read the same now */
        char *q = p;
        for (int i = 0; i < 5 && q; i++) {
            q = memchr(q, ' ', (size_t)(eol - q));
            if (q) q++;
        }
        char *dep = q ? str_ndup(q, (size_t)(eol - q)) : NULL;
        struct stat st;
        now.len = 0;
        ok = dep && stat(dep, &st) == 0;
        if (ok) {
            put_dep(&now, dep, &st);
            ok = now.len == (size_t)(eol + 1 - p) && memcmp(now.p, p, now.len) == 0;
        }
        free(dep);
        p = eol + 1;
    }
    buf_free(&now);
    if (!ok) { free(e); return NULL; }
    *len = (int)(end - p);
    memmove(e, p, (size_t)*len);
    e[*len] = '\0';
    return e;
}

/* Whether a file was last changed before `start`, when the compiler began,
 * by a margin: one changed later may have changed while the compiler read it. */
static bool settled(const struct stat *st, time_t start) {
    return st->st_mtim.tv_sec < start - 1 && st->st_ctim.tv_sec < start - 1;
}

/* The compiler's header search, in its order, from the list that -Wp,-v
 * has gcc and clang print to stderr (`err`): the directories it searched,
 * without trailing slashes, and those it skipped for not existing. False
 * when `err` holds no list. */
typedef struct { char **dirs; int n, cap; char **missing; int nmissing, capmissing; } Search;

static bool search_read(const char *err, Search *s) {
    static const char skipped[] = "ignoring nonexistent directory \"";
    bool in_list = false;
    for (const char *p = err; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof skipped && memcmp(p, skipped, sizeof skipped - 1) == 0 &&
            p[len - 1] == '"') {
            char *dir = str_ndup(p + sizeof skipped - 1, len - sizeof skipped);
            DA_APPEND(s->missing, s->nmissing, s->capmissing, dir);
        } else if (strncmp(p, "#include ", 9) == 0 && len > 29 &&
                   memcmp(p + len - 20, " search starts here:", 20) == 0) {
            in_list = true;
        } else if (len == 19 && memcmp(p, "End of search list.", 19) == 0) {
            return in_list;
        } else if (in_list && len > 1 && p[0] == ' ') {
            while (len > 2 && p[len - 1] == '/') len--;
            DA_APPEND(s->dirs, s->n, s->cap, str_ndup(p + 1, len - 1));
        }
        p += len + (eol != NULL);
    }
    return false;
}

static void search_free(Search *s) {
    for (int i = 0; i < s->n; i++) free(s->dirs[i]);
    for (int i = 0; i < s->nmissing; i++) free(s->missing[i]);
    free(s->dirs);
    free(s->missing);
}

/* The name `path` has relative to directory `dir` (as the compiler spells a
 * header it found there: "inc/x.h" in "inc", "x.h" in "."), or NULL when the
 * path is not inside it. */
static const char *name_in(const char *dir, const char *path) {
    if (strcmp(dir, ".") == 0) {
        if (path[0] == '/') return NULL;
        while (path[0] == '.' && path[1] == '/') path += 2;
        return path;
    }
    size_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0 || (path[n] != '/' && dir[n - 1] != '/')) return NULL;
    path += n;
    while (*path == '/') path++;
    return *path ? path : NULL;
}

/* Append to *b the places where a file would shadow the header the compiler
 * read at `path`: its name in each directory searched before the one it was
 * found in. A place that is empty must stay so ("! <place>"), and a missing
 * directory on its way stands for every place inside it ("! /usr/local/
 * include/SDL2" for all of SDL's headers); one that holds a file (which
 * #include_next reads past) must stay as it is. A path inside
 * two directories of the search (/usr/include/x86_64-linux-gnu/bits/types.h)
 * is covered under each name it may have been included by. `seen` keeps the
 * places recorded, with keys the caller frees. False when a file in one of
 * them changed too close to `start`. */
static bool cache_shadows(const char *path, const Search *s, time_t start, HMap *seen, Buf *b) {
    for (int k = 0; k < s->n; k++) {
        const char *name = name_in(s->dirs[k], path);
        if (!name) continue;
        for (int j = 0; j < k; j++) {
            char *place = NULL;
            struct stat st;
            bool exists = true;
            for (const char *c = name; exists; c++) {
                if (*c && *c != '/') continue;
                free(place);
                place = str_sprintf("%s/%.*s", s->dirs[j], (int)(c - name), name);
                exists = stat(place, &st) == 0;
                if (!*c) break;
            }
            if (hmap_get(seen, place, strlen(place))) { free(place); continue; }
            hmap_put(seen, place, strlen(place));
            if (!exists) buf_printf(b, "! %s\n", place);
            else if (settled(&st, start)) put_dep(b, place, &st);
            else return false;
        }
    }
    return true;
}

/* Append an entry line to *b for each file the dependency file `depfile`
 * (written by -MD -MT x) lists, except those under `skip`, the work
 * directory, and for each place that would shadow one in the header search
 * `s`. A directory the compiler skipped for not existing must still not
 * exist: created, it may shadow anything. False if a file cannot be read, or
 * was changed too close to `start`, when the compiler began: it may have
 * changed while the compiler read it. */
static bool cache_deps(const char *depfile, const char *skip, time_t start,
                       const Search *s, Buf *b) {
    char *d = read_file(depfile, NULL);
    bool ok = d && strncmp(d, "x:", 2) == 0;
    Buf name = { 0 };
    HMap seen = { 0 };
    for (const char *p = ok ? d + 2 : ""; ok; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || (*p == '\\' && p[1] == '\n'))
            p += *p == '\\' ? 2 : 1;
        if (!*p) break;
        /* a name, in make's quoting: "\ " and "\#" for the character, "$$" for '$' */
        name.len = 0;
        for (; *p && *p != ' ' && *p != '\t' && *p != '\n' && !(*p == '\\' && p[1] == '\n'); p++) {
            if ((*p == '\\' && (p[1] == ' ' || p[1] == '#')) || (*p == '$' && p[1] == '$')) p++;
            buf_put(&name, p, 1);
        }
        if (strncmp(name.p, skip, strlen(skip)) == 0) continue;
        struct stat st;
        ok = stat(name.p, &st) == 0 && settled(&st, start);
        if (ok) put_dep(b, name.p, &st);
        if (ok) ok = cache_shadows(name.p, s, start, &seen, b);
    }
    for (int i = 0; ok && i < s->nmissing; i++) {
        struct stat st;
        ok = stat(s->missing[i], &st) != 0;
        buf_printf(b, "! %s\n", s->missing[i]);
    }
    for (size_t i = 0; i < seen.cap; i++) if (seen.e[i].used) free((char *)seen.e[i].key);
    hmap_free(&seen);
    buf_free(&name);
    free(d);
    return ok;
}

/* Write a cache entry whole: into a new file beside it, renamed over it, so
 * an fcc running alongside never reads part of one. */
static void cache_write(const char *dir, const char *path, const Buf *entry) {
    char *mk = str_sprintf("mkdir -p '%s'", dir);
    bool have_dir = system(mk) == 0;
    free(mk);
    if (!have_dir) return;
    char *tmp = str_sprintf("%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "wb");
        bool ok = f && fwrite(entry->p, 1, entry->len, f) == entry->len;
        if (f) ok = fclose(f) == 0 && ok;
        else close(fd);
        if (!ok || rename(tmp, path) != 0) remove(tmp);
    }
    free(tmp);
}

/* Compile C source `src` with the program's include path and return the
 * output: the program's stdout ("probe") or the relocatable object ("shim"),
 * with its length in *len. Cached, so the C compiler runs only when the
 * source or a header it includes changes. NULL on failure (reported). */
static char *cc_cached(const char *kind, Buf *src, int *len) {
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    const char *ifl = g_ciflags.p ? g_ciflags.p : "";
    const char *env_paths[] = { "CPATH", "C_INCLUDE_PATH" };
    Buf key = { 0 };
    buf_puts(&key, kind); buf_put(&key, "", 1);
    buf_puts(&key, cc); buf_put(&key, "", 1);
    buf_puts(&key, ifl); buf_put(&key, "", 1);
    for (int i = 0; i < 2; i++) {
        const char *v = getenv(env_paths[i]);
        buf_puts(&key, v ? v : ""); buf_put(&key, "", 1);
    }
    buf_put(&key, src->p, src->len);
    uint64_t h = hash_bytes(key.p, key.len);
    buf_free(&key);
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char *cache_dir = xdg && *xdg ? str_sprintf("%s/fcc", xdg)
                    : str_sprintf("%s/.cache/fcc", home ? home : "/tmp");
    char *cache = str_sprintf("%s/%s-%016llx", cache_dir, kind, (unsigned long long)h);
    char *text = cache_read(cache, len);
    if (text) { free(cache); free(cache_dir); return text; }
    const char *tmp = getenv("TMPDIR");
    char *work = str_sprintf("%s/fcc-%s-XXXXXX", tmp && *tmp ? tmp : "/tmp", kind);
    if (!mkdtemp(work)) {
        diag_error_simple("native backend: cannot create a work directory for the C %s", kind);
        free(work); free(cache); free(cache_dir);
        return NULL;
    }
    char *cpath = str_sprintf("%s/x.c", work);
    FILE *f = fopen(cpath, "w");
    if (f) { fwrite(src->p, 1, src->len, f); fclose(f); }
    /* -Wp,-v: the header search, for the cache entry, printed before
     * anything else the compiler says */
    char *cmd = strcmp(kind, "probe") == 0
        ? str_sprintf("%s -w -Wp,-v -MD -MF '%s/deps' -MT x -o '%s/x' '%s'%s 2>'%s/err' && "
                      "'%s/x' >'%s/out'",
                      cc, work, work, cpath, ifl, work, work, work)
        : str_sprintf("%s -w -Wp,-v -c -O1 -fPIC -fno-asynchronous-unwind-tables -MD -MF '%s/deps' "
                      "-MT x -o '%s/out' '%s'%s 2>'%s/err'",
                      cc, work, work, cpath, ifl, work);
    time_t start = time(NULL);
    int rc = system(cmd);
    char *errp = str_sprintf("%s/err", work);
    char *err = read_file(errp, NULL);
    free(errp);
    if (rc != 0) {
        const char *msg = err ? err : "";
        const char *list_end = strstr(msg, "End of search list.\n");
        if (list_end) msg = list_end + 20;
        diag_error_simple("native backend: the C %s failed:\n%s", kind, msg);
    } else {
        char *outp = str_sprintf("%s/out", work);
        text = read_file(outp, len);
        free(outp);
        if (text) {
            Buf entry = { 0 };
            buf_puts(&entry, CACHE_MAGIC);
            char *depf = str_sprintf("%s/deps", work);
            char *skip = str_sprintf("%s/", work);
            Search search = { 0 };
            if (err && search_read(err, &search) && cache_deps(depf, skip, start, &search, &entry)) {
                buf_puts(&entry, "-\n");
                buf_put(&entry, text, (size_t)*len);
                cache_write(cache_dir, cache, &entry);
            }
            search_free(&search);
            free(skip); free(depf);
            buf_free(&entry);
        }
    }
    free(err);
    char *rm = str_sprintf("rm -f '%s/x' '%s/x.c' '%s/err' '%s/out' '%s/deps'; rmdir '%s'",
                           work, work, work, work, work, work);
    if (system(rm) != 0) { /* leftovers in TMPDIR are harmless */ }
    free(rm); free(cmd); free(cpath); free(work); free(cache); free(cache_dir);
    return text;
}

/* The shim: a pointer to each extern from a local header, compiled to an
 * object and merged into the output (header functions are often static or
 * inline, so taking their address makes the C compiler emit them). */
static char *build_shim(int *len) {
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
    char *obj = cc_cached("shim", &src, len);
    buf_free(&src);
    return obj;
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
    int tlen = 0;
    char *text = cc_cached("probe", &src, &tlen);
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

void x86gen_emit(Program *prog, Obj *obj, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab,
                 const char *const *include_dirs, int ninclude_dirs, bool backtraces) {
    g_backtraces = backtraces;
    g_mono = mono;
    g_arena = arena;
    g_intern = intern;
    g_symtab = symtab;
    if (g_len_repr != 64) {
        diag_error_simple("the native backend supports only --len-repr 64");
        return;
    }

    Decl **all = NULL;
    int n = 0, cap = 0;
    flatten(prog->decls, prog->decl_count, &all, &n, &cap);

    asm_init(obj);
    build_cenv(prog, all, n, include_dirs, ninclude_dirs);
    run_probe(prog, all, n, mono);
    if (diag_error_count() > 0) goto done;

    /* Globals */
    for (int i = 0; i < n; i++) {
        Decl *d = all[i];
        if (d->kind == DECL_LET && !decl_is_function(d) && !is_generic(d)) emit_global(d);
    }

    /* Functions */
    bool has_main = false;
    SrcLoc main_def = { 0 };
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
        f.disp = d->let.name;
        f.def = d->loc;
        if (!d->let.is_module_member && strcmp(d->let.name, "main") == 0) {
            f.sym = "fc_main";
            f.is_main = true;
            f.ret = type_int32();
            has_main = true;
            main_def = d->loc;
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
        f.def = tmpl->loc;
        if (g_backtraces) {     /* name<T1, T2> */
            const char *disp = arena_sprintf(g_arena, "%s<", tmpl->let.name);
            for (int j = 0; j < inst->type_param_count; j++) {
                const char *tn = type_name(inst->type_args[j]);
                disp = arena_sprintf(g_arena, "%s%s%s", disp, j ? ", " : "", tn ? tn : "?");
            }
            f.disp = arena_sprintf(g_arena, "%s>", disp);
        }
        emit_function(&f);
        ExprList il = { 0 };
        for (int j = 0; j < fn->func.body_count; j++) collect_lambdas(fn->func.body[j], &il);
        for (int j = 0; j < il.count; j++) emit_lambda(il.items[j]);
        free(il.items);
        leave_instance();
    }

    if (has_main) emit_c_main(main_def);

    /* Helpers requested along the way, which may request more. */
    int done_eq = 0, done_tr = 0, done_cw = 0;
    while (done_eq < g_eq_n || done_tr < g_tramp_n || done_cw < g_cwrap_n) {
        while (done_cw < g_cwrap_n) emit_cwrap(g_cwraps[done_cw++]);
        while (done_tr < g_tramp_n) emit_trampoline(&g_tramps[done_tr++]);
        while (done_eq < g_eq_n) emit_eq_helper(&g_eqs[done_eq++]);
    }

    if (g_backtraces) emit_backtrace_helper();

    int shim_len = 0;
    char *shim = build_shim(&shim_len);

    /* The object file. */
    char cwd[4096];
    asm_finish(&g_text, getcwd(cwd, sizeof cwd) ? cwd : NULL, "fcc");
    if (shim) {
        char *err = NULL;
        if (!obj_merge(obj, (const unsigned char *)shim, (size_t)shim_len, &err))
            diag_error_simple("native backend: cannot merge the C shim: %s", err);
        free(err);
        free(shim);
    }

done:
    asm_free();
    code_free(&g_fnbody); code_free(&g_cold); code_free(&g_text);
    hmap_free(&g_locals); hmap_free(&g_reg_locals); hmap_free(&g_addr_taken); hmap_free(&g_fconsts); hmap_free(&g_strings); hmap_free(&g_strhdrs); hmap_free(&g_fnvals);
    hmap_free(&g_layouts); hmap_free(&g_files); hmap_free(&g_probe); hmap_free(&g_probed_layouts);
    free(g_spills); g_spills = NULL; g_spill_cap = 0;
    free(g_eqs); g_eqs = NULL; g_eq_n = g_eq_cap = 0;
    free(g_tramps); g_tramps = NULL; g_tramp_n = g_tramp_cap = 0;
    free(g_cwraps); g_cwraps = NULL; g_cwrap_n = g_cwrap_cap = 0;
    free(g_runtime_globals); g_runtime_globals = NULL; g_rg_n = g_rg_cap = 0;
    free(g_traces); g_traces = NULL; g_ntraces = g_traces_cap = 0;
    hmap_free(&g_shim_used);
    free_cenv();
    free(all);
}

#endif /* Linux x86-64 */
