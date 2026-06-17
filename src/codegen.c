#include "codegen.h"
#include "monomorph.h"
#include "pass1.h"
#include "diag.h"
#include <inttypes.h>
#include <string.h>

/* Substitution context for monomorphized emission */
typedef struct {
    const char **var_names;
    Type **concrete;
    int count;
} SubstCtx;
static SubstCtx *g_subst = NULL;
static MonoTable *g_mono = NULL;
static Arena *g_arena = NULL;
static InternTable *g_intern = NULL;
static SymbolTable *g_symtab = NULL;

/* Forward declaration for TypeSet (defined later) */
typedef struct TypeSet TypeSet;
static TypeSet *g_eq_set = NULL;

static int indent_level = 0;
static int temp_counter = 0;

/* File-level non-function globals whose initialization is hoisted into C main */
static Decl **g_file_globals = NULL;
static int g_file_global_count = 0;

/* True while emitting a module-member initializer at C file scope.  Flips
 * EXPR_ARRAY_LIT and EXPR_STRUCT_LIT-with-fixed-array-fields onto an
 * aggregate-initializer emission path instead of the default statement-
 * expression path, which C11 rejects at file scope. */
static bool g_const_context = false;

/* Backing arrays for file-scope slice-typed array literals.  Collected by
 * a pre-pass over module-member inits (including nested array lits), emitted
 * as `static T _fc_const_backing_N[] = {...};` ahead of the module-member
 * definitions.  Each entry's AST node carries the backing name; the slice
 * header emitted at the use site references it. */
static Expr **g_const_backings = NULL;
static int g_const_backing_count = 0;
static int g_const_backing_cap = 0;
static int g_const_backing_counter = 0;

/* Hoisted let-mut declarations: emitted at function top, assignments at original site */
typedef struct {
    const char *codegen_name;
    Type *type;
} HoistedDecl;

static HoistedDecl *g_hoisted = NULL;
static int g_hoisted_count = 0;
static int g_hoisted_cap = 0;

/* Function-entry backing arrays for stack array literals and constant-size
 * interpolation buffers.  Each entry is the array-literal or interp-string node;
 * a fixed C array is emitted at function top and the node's use site references
 * it by name (via codegen_backing_name).  This replaces a per-evaluation
 * __builtin_alloca so the slot is reused across loop iterations — stack use stays
 * bounded instead of growing the frame until the function returns.  The backing
 * keeps function lifetime, matching the lifetime alloca already gave the produced
 * slice/str, so escape analysis stays valid.  Collected fresh per function body. */
static Expr **g_fn_backings = NULL;
static int g_fn_backing_count = 0;
static int g_fn_backing_cap = 0;
static int g_fn_backing_counter = 0;

/* Block-scoped defer tracking */
typedef struct DeferScope DeferScope;
struct DeferScope {
    Expr **defers;      /* array of deferred expressions */
    int count;
    int cap;
    bool is_loop;       /* true for loop/for body scopes */
    DeferScope *parent;
};
static DeferScope *g_defer_scope = NULL;

/* Storage-class prefix for emitted FC functions, monomorphs, lambdas, and
 * trampolines.  Normally `static` so the C compiler can DCE/inline freely.
 * With --backtraces we add `noinline` so every call appears as a distinct
 * frame in the backtrace — without it, inlined calls would silently vanish
 * from the backtrace.  Functions stay `static`: address resolution happens via
 * an FC-emitted symbol table keyed by `&fn` addresses, not the dynamic
 * symbol table, so `-rdynamic` is not required.  Set in codegen_emit(). */
static const char *g_fn_attr = "static __attribute__((unused)) ";

static void emit_type(Type *t, FILE *out);
static void emit_indent(FILE *out);
static void emit_expr(Expr *e, FILE *out);
static Type *resolve_struct_stub(Type *t);
static bool type_valueless(Type *t);
static bool interp_const_buffer_size(Expr *e, int64_t *out_size);

static bool is_hoisted(const char *codegen_name) {
    for (int i = 0; i < g_hoisted_count; i++)
        if (g_hoisted[i].codegen_name == codegen_name) return true;
    return false;
}

/* ---- Defer scope helpers ---- */

static void defer_scope_push(bool is_loop) {
    DeferScope *ds = calloc(1, sizeof(DeferScope));
    ds->is_loop = is_loop;
    ds->parent = g_defer_scope;
    g_defer_scope = ds;
}

static void defer_scope_pop(void) {
    DeferScope *ds = g_defer_scope;
    g_defer_scope = ds->parent;
    free(ds->defers);
    free(ds);
}

static void defer_scope_add(Expr *e) {
    DeferScope *ds = g_defer_scope;
    DA_APPEND(ds->defers, ds->count, ds->cap, e);
}

/* True if a deferred expression lowers to a C *statement* (bare `for`/`while`/
 * `if`) rather than an expression, so it must be emitted bare instead of wrapped
 * in `(void)(…)`. Mirrors the statement-position handling in emit_block_stmts.
 * Blocks and matches always emit as statement-expressions `({…})`, so they are
 * safe to wrap and need no special-casing here. */
static bool defer_emits_statement(Expr *e) {
    switch (e->kind) {
    case EXPR_FOR:  return true;
    case EXPR_LOOP:
    case EXPR_IF:   return type_valueless(e->type);
    default:        return false;
    }
}

/* Emit defers for a single scope in LIFO order */
static void emit_scope_defers(DeferScope *ds, FILE *out) {
    for (int i = ds->count - 1; i >= 0; i--) {
        emit_indent(out);
        Expr *d = ds->defers[i];
        if (defer_emits_statement(d)) {
            /* Statement-emitting form: `(void)(while(1){…})` would be invalid C. */
            emit_expr(d, out);
            fprintf(out, "\n");
        } else {
            fprintf(out, "(void)(");
            emit_expr(d, out);
            fprintf(out, ");\n");
        }
    }
}

/* Emit defers from current scope outward up to and including loop boundary.
 * Used for break and continue. */
static void emit_defers_to_loop(FILE *out) {
    for (DeferScope *ds = g_defer_scope; ds; ds = ds->parent) {
        emit_scope_defers(ds, out);
        if (ds->is_loop) break;
    }
}

/* Emit defers from current scope outward through ALL scopes.
 * Used for return. */
static void emit_defers_to_func(FILE *out) {
    for (DeferScope *ds = g_defer_scope; ds; ds = ds->parent) {
        emit_scope_defers(ds, out);
    }
}

/* Check if any defer scope in the chain has pending defers */
static bool has_pending_defers(void) {
    for (DeferScope *ds = g_defer_scope; ds; ds = ds->parent) {
        if (ds->count > 0) return true;
    }
    return false;
}

static void collect_hoisted_pat(Pattern *pat, Type *type);

/* Recursively collect all let-mut bindings from a function body */
static void collect_hoisted_bindings(Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_LET:
        collect_hoisted_bindings(e->let_expr.let_init);
        if (e->let_expr.let_is_mut && e->let_expr.codegen_name && e->let_expr.let_type) {
            HoistedDecl d = { e->let_expr.codegen_name, e->let_expr.let_type };
            DA_APPEND(g_hoisted, g_hoisted_count, g_hoisted_cap, d);
        }
        break;
    case EXPR_LET_DESTRUCT:
        collect_hoisted_bindings(e->let_destruct.init);
        /* Destructured mut bindings: collect individual pattern bindings */
        if (e->let_destruct.is_mut)
            collect_hoisted_pat(e->let_destruct.pattern, e->let_destruct.init_type);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_hoisted_bindings(e->block.stmts[i]);
        break;
    case EXPR_IF:
        collect_hoisted_bindings(e->if_expr.cond);
        collect_hoisted_bindings(e->if_expr.then_body);
        collect_hoisted_bindings(e->if_expr.else_body);
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_hoisted_bindings(e->loop_expr.body[i]);
        break;
    case EXPR_FOR:
        collect_hoisted_bindings(e->for_expr.iter);
        if (e->for_expr.range_end) collect_hoisted_bindings(e->for_expr.range_end);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_hoisted_bindings(e->for_expr.body[i]);
        break;
    case EXPR_MATCH:
        collect_hoisted_bindings(e->match_expr.subject);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_hoisted_bindings(e->match_expr.arms[i].body[j]);
        break;
    case EXPR_BINARY:
        collect_hoisted_bindings(e->binary.left);
        collect_hoisted_bindings(e->binary.right);
        break;
    case EXPR_UNARY_PREFIX:
        collect_hoisted_bindings(e->unary_prefix.operand);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_hoisted_bindings(e->unary_postfix.operand);
        break;
    case EXPR_CALL:
        collect_hoisted_bindings(e->call.func);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_hoisted_bindings(e->call.args[i]);
        break;
    case EXPR_INDEX:
        collect_hoisted_bindings(e->index.object);
        collect_hoisted_bindings(e->index.index);
        break;
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        collect_hoisted_bindings(e->field.object);
        break;
    case EXPR_ASSIGN:
        collect_hoisted_bindings(e->assign.target);
        collect_hoisted_bindings(e->assign.value);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_hoisted_bindings(e->return_expr.value);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_hoisted_bindings(e->break_expr.value);
        break;
    case EXPR_DEFER:
        collect_hoisted_bindings(e->defer_expr.value);
        break;
    case EXPR_CAST:
        /* (cstr[N]) bounded str→cstr cast: hoist a fixed uint8[N] backing to
         * function entry so the truncating copy reuses one slot across loop
         * iterations (and so the produced cstr keeps function-frame lifetime). */
        if (e->cast.buffer_size > 0) {
            if (!e->cast.codegen_backing_name) {
                char buf[40];
                int n = snprintf(buf, sizeof buf, "_fc_back_%d", g_fn_backing_counter++);
                e->cast.codegen_backing_name = arena_strdup(g_arena, buf, n);
            }
            DA_APPEND(g_fn_backings, g_fn_backing_count, g_fn_backing_cap, e);
        }
        collect_hoisted_bindings(e->cast.operand);
        break;
    case EXPR_SOME:
        collect_hoisted_bindings(e->some_expr.value);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_hoisted_bindings(e->struct_lit.fields[i].value);
        break;
    case EXPR_ARRAY_LIT:
        /* Hoist a fixed backing array (size N is a compile-time literal, enforced
         * in pass2) to function entry so the slot is reused per loop iteration.
         * Skip zero-length literals — C has no zero-length arrays — and let them
         * fall back to alloca at the use site. */
        if (e->array_lit.size_expr &&
            e->array_lit.size_expr->kind == EXPR_INT_LIT &&
            e->array_lit.size_expr->int_lit.value > 0) {
            if (!e->array_lit.codegen_backing_name) {
                char buf[40];
                int n = snprintf(buf, sizeof buf, "_fc_back_%d", g_fn_backing_counter++);
                e->array_lit.codegen_backing_name = arena_strdup(g_arena, buf, n);
            }
            DA_APPEND(g_fn_backings, g_fn_backing_count, g_fn_backing_cap, e);
        }
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_hoisted_bindings(e->array_lit.elems[i]);
        break;
    case EXPR_SLICE_LIT:
        collect_hoisted_bindings(e->slice_lit.ptr_expr);
        collect_hoisted_bindings(e->slice_lit.len_expr);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_hoisted_bindings(e->alloc_expr.size_expr);
        if (e->alloc_expr.init_expr) {
            /* alloc(T[N]{...}) and alloc("...%d") emit their array/interp init
             * straight into the heap buffer — no stack backing.  Recurse past the
             * top node into its children so nested stack temporaries are still
             * hoisted, but don't give the heap-bound init itself a backing. */
            Expr *init = e->alloc_expr.init_expr;
            if (init->kind == EXPR_ARRAY_LIT) {
                for (int i = 0; i < init->array_lit.elem_count; i++)
                    collect_hoisted_bindings(init->array_lit.elems[i]);
            } else if (init->kind == EXPR_INTERP_STRING) {
                for (int i = 0; i < init->interp_string.segment_count; i++)
                    if (!init->interp_string.segments[i].is_literal &&
                        init->interp_string.segments[i].expr)
                        collect_hoisted_bindings(init->interp_string.segments[i].expr);
            } else {
                collect_hoisted_bindings(init);
            }
        }
        break;
    case EXPR_FREE:
        collect_hoisted_bindings(e->free_expr.operand);
        break;
    case EXPR_ATOMIC_LOAD:
        collect_hoisted_bindings(e->atomic_load.ptr);
        break;
    case EXPR_ATOMIC_STORE:
        collect_hoisted_bindings(e->atomic_store.ptr);
        collect_hoisted_bindings(e->atomic_store.value);
        break;
    case EXPR_ASSERT:
        collect_hoisted_bindings(e->assert_expr.condition);
        if (e->assert_expr.message) collect_hoisted_bindings(e->assert_expr.message);
        break;
    case EXPR_SLICE:
        collect_hoisted_bindings(e->slice.object);
        if (e->slice.lo) collect_hoisted_bindings(e->slice.lo);
        if (e->slice.hi) collect_hoisted_bindings(e->slice.hi);
        break;
    case EXPR_INTERP_STRING: {
        /* Constant-size buffer (no runtime-length %s) → hoist a fixed backing
         * array to function entry, reused per iteration.  Runtime-sized buffers
         * stay on alloca (documented: grows per loop iteration; use alloc(s)! to
         * promote to the heap). */
        int64_t bsize = 0;
        if (interp_const_buffer_size(e, &bsize)) {
            if (!e->interp_string.codegen_backing_name) {
                char buf[40];
                int n = snprintf(buf, sizeof buf, "_fc_back_%d", g_fn_backing_counter++);
                e->interp_string.codegen_backing_name = arena_strdup(g_arena, buf, n);
            }
            e->interp_string.backing_size = bsize;
            DA_APPEND(g_fn_backings, g_fn_backing_count, g_fn_backing_cap, e);
        }
        for (int i = 0; i < e->interp_string.segment_count; i++)
            if (!e->interp_string.segments[i].is_literal && e->interp_string.segments[i].expr)
                collect_hoisted_bindings(e->interp_string.segments[i].expr);
        break;
    }
    case EXPR_FUNC:
        /* Don't recurse into nested lambdas — they get their own hoisting scope */
        break;
    default:
        break;
    }
}

/* Collect hoisted bindings from destructuring pattern */
static void collect_hoisted_pat(Pattern *pat, Type *type) {
    if (!pat || !type) return;
    switch (pat->kind) {
    case PAT_BINDING:
        if (pat->binding.name) {
            HoistedDecl d = { pat->binding.name, type };
            DA_APPEND(g_hoisted, g_hoisted_count, g_hoisted_cap, d);
        }
        break;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            collect_hoisted_pat(pat->struc.fields[i].pattern, pat->struc.fields[i].resolved_type);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            collect_hoisted_pat(pat->tuple_pat.patterns[i], pat->tuple_pat.resolved_types[i]);
        break;
    case PAT_SOME:
        if (pat->some_pat.inner && type->kind == TYPE_OPTION)
            collect_hoisted_pat(pat->some_pat.inner, type->option.inner);
        break;
    case PAT_VARIANT:
        if (pat->variant.payload && type->kind == TYPE_UNION) {
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->variant.variant) {
                    collect_hoisted_pat(pat->variant.payload, type->unio.variants[v].payload);
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
}

/* Emit hoisted declarations at the top of a function body */
static void emit_hoisted_decls(FILE *out) {
    for (int i = 0; i < g_hoisted_count; i++) {
        emit_indent(out);
        emit_type(g_hoisted[i].type, out);
        Type *t = g_hoisted[i].type;
        /* Use appropriate zero initializer for the type */
        bool is_scalar = type_is_numeric(t) || t->kind == TYPE_BOOL ||
            t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR;
        if (is_scalar)
            fprintf(out, " %s = 0;\n", g_hoisted[i].codegen_name);
        else
            fprintf(out, " %s = {0};\n", g_hoisted[i].codegen_name);
    }
}

/* Emit the function-entry backing arrays for stack array literals and
 * constant-size interpolation buffers (see g_fn_backings).  Each is a plain
 * fixed C array whose single slot is reused on every loop iteration. */
static void emit_fn_backing_decls(FILE *out) {
    for (int i = 0; i < g_fn_backing_count; i++) {
        Expr *e = g_fn_backings[i];
        emit_indent(out);
        if (e->kind == EXPR_ARRAY_LIT) {
            emit_type(e->array_lit.elem_type, out);
            fprintf(out, " %s[%" PRIu64 "];\n",
                    e->array_lit.codegen_backing_name,
                    e->array_lit.size_expr->int_lit.value);
        } else if (e->kind == EXPR_CAST) { /* (cstr[N]) */
            fprintf(out, "uint8_t %s[%d];\n",
                    e->cast.codegen_backing_name, e->cast.buffer_size);
        } else { /* EXPR_INTERP_STRING */
            fprintf(out, "uint8_t %s[%" PRId64 "];\n",
                    e->interp_string.codegen_backing_name,
                    e->interp_string.backing_size + 1);
        }
    }
}

/* Set up hoisting for a function body, emit declarations, then tear down */
static void begin_hoisted_scope(Expr **body, int body_count, FILE *out) {
    g_hoisted_count = 0;
    g_fn_backing_count = 0;
    for (int i = 0; i < body_count; i++)
        collect_hoisted_bindings(body[i]);
    emit_hoisted_decls(out);
    emit_fn_backing_decls(out);
}

static void end_hoisted_scope(void) {
    g_hoisted_count = 0;
    g_fn_backing_count = 0;
}

/* Resolve a type variable to its concrete type under the active
   monomorphization substitution; identity for everything else. */
static Type *subst_resolve(Type *t) {
    if (g_subst && t && t->kind == TYPE_TYPE_VAR) {
        for (int i = 0; i < g_subst->count; i++)
            if (g_subst->var_names[i] == t->type_var.name)
                return g_subst->concrete[i];
    }
    return t;
}

/* Does the option inner type use null-sentinel optimization (bare pointer, NULL = none)? */
static bool is_null_sentinel(Type *opt_type) {
    opt_type = subst_resolve(opt_type);
    if (!opt_type || opt_type->kind != TYPE_OPTION) return false;
    Type *inner = subst_resolve(opt_type->option.inner);
    return inner && (inner->kind == TYPE_POINTER ||
                     inner->kind == TYPE_ANY_PTR);
}

/* Pointer-value null-status predicates (declared in ast.h, shared by pass2 and
 * codegen). A null-sentinel option (T*?, any*?, cstr?) represents none as a null
 * pointer, so some(p) over a null p is indistinguishable from none. pass2 uses
 * these to reject a provably-null payload and to gate const context; codegen uses
 * provably_nonnull to elide the runtime null-guard. Defined here, in one place,
 * so the guard/elide/reject decisions can never drift apart.
 *
 * Both are conservative toward emitting the guard: provably_nonnull returns true
 * ONLY when the value can never be null (so a guard would be dead), and
 * provably_null returns true ONLY when it is always null. Anything uncertain
 * (params, field reads, call results, extern returns) yields false from both →
 * a runtime guard. */
bool ptr_value_provably_nonnull(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_UNARY_PREFIX:
        /* &x / &fn — address of a binding/element/function is never null. */
        return e->unary_prefix.op == TOK_AMP;
    case EXPR_UNARY_POSTFIX:
        /* p! — unwrap yields the some-payload, which for a pointer option is
         * non-null (some(null) is itself rejected/guarded), and alloc(...)! is
         * malloc-checked. Only reached here when the result is pointer-typed. */
        return e->unary_postfix.op == TOK_BANG;
    case EXPR_CSTRING_LIT:
        /* c"..." points into static storage. */
        return true;
    case EXPR_CAST:
        /* (T*) <nonzero integer literal> — e.g. a fixed MMIO address. */
        if (e->cast.operand && e->cast.operand->kind == EXPR_INT_LIT)
            return e->cast.operand->int_lit.value != 0;
        /* A widening / const-add cast preserves the operand's null-status. */
        return ptr_value_provably_nonnull(e->cast.operand);
    default:
        return false;
    }
}

bool ptr_value_provably_null(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_DEFAULT:
        /* default(T*) / default(any*) zero-init to NULL. (default(T*?) is an
         * EXPR_DEFAULT with an option target — that is none, not a some, and
         * never reaches some().) */
        return e->default_expr.target &&
               (e->default_expr.target->kind == TYPE_POINTER ||
                e->default_expr.target->kind == TYPE_ANY_PTR);
    case EXPR_CAST:
        /* (T*) 0 — integer-literal zero cast to pointer. */
        if (e->cast.operand && e->cast.operand->kind == EXPR_INT_LIT)
            return e->cast.operand->int_lit.value == 0;
        return ptr_value_provably_null(e->cast.operand);
    default:
        return false;
    }
}

static void emit_indent(FILE *out) {
    for (int i = 0; i < indent_level; i++) fprintf(out, "    ");
}

/* Compute mangled name for a generic struct/union type under g_subst */
static const char *mangle_generic_with_subst(const char *base_name, Type *t) {
    const char **vars = NULL;
    int vc = 0, vcap = 0;
    type_collect_vars(t, &vars, &vc, &vcap);
    Type **concrete_args = arena_alloc(g_arena, sizeof(Type*) * (size_t)vc);
    for (int i = 0; i < vc; i++) {
        /* Look up this type var in g_subst */
        concrete_args[i] = NULL;
        for (int j = 0; j < g_subst->count; j++) {
            if (g_subst->var_names[j] == vars[i]) {
                concrete_args[i] = g_subst->concrete[j];
                break;
            }
        }
        if (!concrete_args[i]) {
            /* Not found — create a type var type */
            concrete_args[i] = type_type_var(g_arena, vars[i]);
        }
    }
    const char *mangled = mangle_generic_name(g_arena, g_intern,
        base_name, concrete_args, vc);
    free(vars);
    return mangled;
}

/* Canonical C name for a tuple type whose element types contain type variables,
 * resolved under the active g_subst. Substitutes the elements, then runs
 * mono_resolve_type_names (which re-derives tuple names and mangles any nested
 * generic struct/union instances) so the result matches the registered name. */
static const char *tuple_name_under_subst(Type *t) {
    Type *sub = type_substitute(g_arena, t, g_subst->var_names,
                                g_subst->concrete, g_subst->count);
    mono_resolve_type_names(g_mono, g_arena, g_intern, sub);
    return sub->struc.name;
}

/* Resolve a concrete type + property name to a C constant string.
 * Used for type-variable property access ('a.min etc.) during monomorphized emission. */
static const char *resolve_type_prop_codegen(Type *t, const char *prop) {
    TypeKind k = t->kind;
    bool is_int = type_is_integer(t);
    bool is_float = type_is_float(t);
    if (!is_int && !is_float) return NULL;

    if (strcmp(prop, "bits") == 0) {
        switch (k) {
        case TYPE_INT8: case TYPE_UINT8:   return "8";
        case TYPE_INT16: case TYPE_UINT16: return "16";
        case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: return "32";
        case TYPE_INT64: case TYPE_UINT64: case TYPE_FLOAT64: return "64";
        case TYPE_ISIZE: case TYPE_USIZE: return "((int32_t)(sizeof(ptrdiff_t)*8))";
        default: return NULL;
        }
    }
    if (is_int) {
        if (strcmp(prop, "min") == 0) {
            switch (k) {
            case TYPE_INT8:   return "INT8_MIN";
            case TYPE_INT16:  return "INT16_MIN";
            case TYPE_INT32:  return "INT32_MIN";
            case TYPE_INT64:  return "INT64_MIN";
            case TYPE_UINT8:  return "((uint8_t)0)";
            case TYPE_UINT16: return "((uint16_t)0)";
            case TYPE_UINT32: return "((uint32_t)0)";
            case TYPE_UINT64: return "((uint64_t)0)";
            case TYPE_ISIZE:  return "PTRDIFF_MIN";
            case TYPE_USIZE:  return "((size_t)0)";
            default: return NULL;
            }
        }
        if (strcmp(prop, "max") == 0) {
            switch (k) {
            case TYPE_INT8:   return "INT8_MAX";
            case TYPE_INT16:  return "INT16_MAX";
            case TYPE_INT32:  return "INT32_MAX";
            case TYPE_INT64:  return "INT64_MAX";
            case TYPE_UINT8:  return "UINT8_MAX";
            case TYPE_UINT16: return "UINT16_MAX";
            case TYPE_UINT32: return "UINT32_MAX";
            case TYPE_UINT64: return "UINT64_MAX";
            case TYPE_ISIZE:  return "PTRDIFF_MAX";
            case TYPE_USIZE:  return "SIZE_MAX";
            default: return NULL;
            }
        }
        return NULL;
    }
    /* Float properties */
    bool is_f32 = (k == TYPE_FLOAT32);
    if (strcmp(prop, "min") == 0) return is_f32 ? "FLT_MIN" : "DBL_MIN";
    if (strcmp(prop, "max") == 0) return is_f32 ? "FLT_MAX" : "DBL_MAX";
    if (strcmp(prop, "epsilon") == 0) return is_f32 ? "FLT_EPSILON" : "DBL_EPSILON";
    if (strcmp(prop, "nan") == 0) return is_f32 ? "((float)NAN)" : "((double)NAN)";
    if (strcmp(prop, "inf") == 0) return is_f32 ? "((float)INFINITY)" : "((double)INFINITY)";
    if (strcmp(prop, "neg_inf") == 0) return is_f32 ? "((float)(-INFINITY))" : "((double)(-INFINITY))";
    return NULL;
}

/* Emit the identifier portion of a function type typedef name */
static void emit_fn_type_suffix(Type *t, FILE *out);

static void emit_type_ident(Type *t, FILE *out);

static void emit_type(Type *t, FILE *out) {
    /* Handle type variable substitution during monomorphized emission */
    if (g_subst && t->kind == TYPE_TYPE_VAR) {
        for (int i = 0; i < g_subst->count; i++) {
            if (g_subst->var_names[i] == t->type_var.name) {
                emit_type(g_subst->concrete[i], out);
                return;
            }
        }
    }
    switch (t->kind) {
    case TYPE_INT8:    fprintf(out, "int8_t");    break;
    case TYPE_INT16:   fprintf(out, "int16_t");   break;
    case TYPE_INT32:   fprintf(out, "int32_t");   break;
    case TYPE_INT64:   fprintf(out, "int64_t");   break;
    case TYPE_UINT8:   fprintf(out, "uint8_t");   break;
    case TYPE_UINT16:  fprintf(out, "uint16_t");  break;
    case TYPE_UINT32:  fprintf(out, "uint32_t");  break;
    case TYPE_UINT64:  fprintf(out, "uint64_t");  break;
    case TYPE_ISIZE:   fprintf(out, "ptrdiff_t"); break;
    case TYPE_USIZE:   fprintf(out, "size_t");    break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_VOID:    fprintf(out, "void");      break;
    case TYPE_NEVER:   fprintf(out, "void");      break; /* defensive: never materialized */
    case TYPE_CHAR:    fprintf(out, "uint8_t");   break;
    case TYPE_POINTER:
        if (t->is_const) fprintf(out, "const ");
        emit_type(t->pointer.pointee, out);
        fprintf(out, "*");
        break;
    case TYPE_SLICE:
        if (is_str_type(t)) {
            fprintf(out, "fc_str");
        } else {
            fprintf(out, "fc_slice_");
            emit_type_ident(t->slice.elem, out);
        }
        break;
    case TYPE_OPTION: {
        Type *inner = subst_resolve(t->option.inner);
        if (inner && (inner->kind == TYPE_POINTER ||
            inner->kind == TYPE_ANY_PTR)) {
            /* Pointer-like? → plain pointer (null = none) */
            emit_type(inner, out);
        } else {
            fprintf(out, "fc_option_");
            if (inner) emit_type_ident(inner, out);
            else fprintf(out, "void");
        }
        break;
    }
    case TYPE_STRUCT:
        if (t->struc.is_tuple) {
            if (g_subst && type_contains_type_var(t))
                fprintf(out, "%s", tuple_name_under_subst(t));
            else
                fprintf(out, "%s", t->struc.name ? t->struc.name
                    : tuple_canonical_name(g_arena, g_intern, t->struc.fields, t->struc.field_count));
            break;
        }
        if (g_subst && type_contains_type_var(t)) {
            fprintf(out, "%s", mangle_generic_with_subst(t->struc.name, t));
            return;
        }
        if (t->struc.c_name) {
            fprintf(out, "%s %s", t->struc.is_c_union ? "union" : "struct",
                t->struc.c_name);
        } else {
            fprintf(out, "%s", t->struc.name);
        }
        break;
    case TYPE_UNION:
        if (g_subst && type_contains_type_var(t)) {
            fprintf(out, "%s", mangle_generic_with_subst(t->unio.name, t));
            return;
        }
        fprintf(out, "%s", t->unio.name);
        break;
    case TYPE_FUNC:
        fprintf(out, "fc_fn_");
        emit_fn_type_suffix(t, out);
        break;
    case TYPE_FIXED_ARRAY:
        /* Emitted as C array type — used by sizeof(T[N]) */
        emit_type(t->fixed_array.elem, out);
        fprintf(out, "[%lld]", (long long)t->fixed_array.size);
        break;
    case TYPE_ANY_PTR:
        if (t->is_const) fprintf(out, "const ");
        fprintf(out, "void*");
        break;
    case TYPE_STUB: {
        /* Resolve stub to the actual type and emit it */
        Type *resolved = resolve_struct_stub(t);
        if (resolved != t) {
            emit_type(resolved, out);
        } else {
            /* Fallback: use the stub name directly as the C type name */
            fprintf(out, "%s", t->stub.name);
        }
        break;
    }
    default:
        fprintf(out, "/* TODO: type %d */", t->kind);
        break;
    }
}

/* Emit a C type name suitable for use in identifiers (slice/option typedef names) */
static void emit_type_ident(Type *t, FILE *out) {
    /* Handle type variable substitution */
    if (g_subst && t->kind == TYPE_TYPE_VAR) {
        for (int i = 0; i < g_subst->count; i++) {
            if (g_subst->var_names[i] == t->type_var.name) {
                emit_type_ident(g_subst->concrete[i], out);
                return;
            }
        }
    }
    switch (t->kind) {
    case TYPE_INT8:    fprintf(out, "int8_t");    break;
    case TYPE_INT16:   fprintf(out, "int16_t");   break;
    case TYPE_INT32:   fprintf(out, "int32_t");   break;
    case TYPE_INT64:   fprintf(out, "int64_t");   break;
    case TYPE_UINT8:   fprintf(out, "uint8_t");   break;
    case TYPE_UINT16:  fprintf(out, "uint16_t");  break;
    case TYPE_UINT32:  fprintf(out, "uint32_t");  break;
    case TYPE_UINT64:  fprintf(out, "uint64_t");  break;
    case TYPE_ISIZE:   fprintf(out, "ptrdiff_t"); break;
    case TYPE_USIZE:   fprintf(out, "size_t");    break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_CHAR:    fprintf(out, "uint8_t");   break;
    case TYPE_ANY_PTR: fprintf(out, "void_ptr");  break;
    case TYPE_STRUCT:
        if (t->struc.is_tuple) {
            if (g_subst && type_contains_type_var(t))
                fprintf(out, "%s", tuple_name_under_subst(t));
            else
                fprintf(out, "%s", t->struc.name ? t->struc.name
                    : tuple_canonical_name(g_arena, g_intern, t->struc.fields, t->struc.field_count));
        }
        else if (g_subst && type_contains_type_var(t))
            fprintf(out, "%s", mangle_generic_with_subst(t->struc.name, t));
        else if (t->struc.c_name)
            fprintf(out, "%s", t->struc.c_name);
        else
            fprintf(out, "%s", t->struc.name);
        break;
    case TYPE_UNION:
        if (g_subst && type_contains_type_var(t))
            fprintf(out, "%s", mangle_generic_with_subst(t->unio.name, t));
        else
            fprintf(out, "%s", t->unio.name);
        break;
    case TYPE_SLICE:
        if (is_str_type(t)) {
            fprintf(out, "fc_str");
        } else {
            fprintf(out, "fc_slice_");
            emit_type_ident(t->slice.elem, out);
        }
        break;
    case TYPE_POINTER:
        emit_type_ident(t->pointer.pointee, out);
        fprintf(out, "_ptr");
        break;
    case TYPE_FUNC:
        fprintf(out, "fc_fn_");
        emit_fn_type_suffix(t, out);
        break;
    case TYPE_OPTION:
        fprintf(out, "fc_option_");
        if (t->option.inner) emit_type_ident(t->option.inner, out);
        else fprintf(out, "void");
        break;
    case TYPE_FIXED_ARRAY:
        fprintf(out, "fixarr%lld_", (long long)t->fixed_array.size);
        emit_type_ident(t->fixed_array.elem, out);
        break;
    case TYPE_VOID:
        fprintf(out, "void");
        break;
    case TYPE_STUB: {
        Type *resolved = resolve_struct_stub(t);
        if (resolved != t) {
            emit_type_ident(resolved, out);
        } else {
            fprintf(out, "%s", t->stub.name);
        }
        break;
    }
    default:           fprintf(out, "unknown");   break;
    }
}

/* Emit the identifier suffix for function type typedef names.
   E.g., for (int32, int32) -> bool: "int32_t_int32_t__bool" */
static void emit_fn_type_suffix(Type *t, FILE *out) {
    if (t->func.param_count == 0) {
        fprintf(out, "v");
    } else {
        for (int i = 0; i < t->func.param_count; i++) {
            if (i > 0) fprintf(out, "_");
            emit_type_ident(t->func.param_types[i], out);
        }
    }
    fprintf(out, "__");
    if (t->func.return_type->kind == TYPE_VOID) {
        fprintf(out, "void");
    } else {
        emit_type_ident(t->func.return_type, out);
    }
    /* Variadic and non-variadic function types are distinct (type_eq compares
     * is_variadic), so their typedef names must differ too — otherwise e.g.
     * `(const cstr, ...) -> int32` (printf) and `(const cstr) -> int32`
     * (stdlib's remove) mangle to the same name and emit a duplicate typedef. */
    if (t->func.is_variadic) {
        fprintf(out, "_vararg");
    }
}

static void emit_expr(Expr *e, FILE *out);
static void emit_eq_func_name(Type *t, FILE *out);

/* ---- Left-to-right evaluation-order sequencing ---------------------------
 *
 * C leaves the evaluation order of function-call arguments and of most binary
 * operands unspecified.  FC guarantees left-to-right, evaluate-once semantics
 * (see spec "Evaluation order").  Where an operand list contains a
 * side-effecting expression, codegen forces the order by evaluating the earlier
 * operands into temporaries — in source order — inside a statement-expression,
 * then rewriting those operand slots to read the temporaries.  Pure operands
 * (no calls/assignments/allocations) need no ordering and are emitted in place.
 */

/* Conservative "may produce an observable side effect" test: a call,
 * assignment, allocation, free, or atomic op anywhere in the tree.  Pure reads,
 * literals, and lambda values return false; control-flow operands default to
 * true (they may contain anything). */
static bool expr_has_side_effects(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT: case EXPR_STRING_LIT: case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT: case EXPR_IDENT: case EXPR_SIZEOF:
    case EXPR_ALIGNOF: case EXPR_TYPE_VAR_REF: case EXPR_DEFAULT:
    case EXPR_FUNC:
        return false;
    case EXPR_CALL: case EXPR_ASSIGN: case EXPR_ALLOC: case EXPR_FREE:
    case EXPR_ATOMIC_LOAD: case EXPR_ATOMIC_STORE:
        return true;
    case EXPR_BINARY:
        return expr_has_side_effects(e->binary.left) ||
               expr_has_side_effects(e->binary.right);
    case EXPR_UNARY_PREFIX:  return expr_has_side_effects(e->unary_prefix.operand);
    case EXPR_UNARY_POSTFIX: return expr_has_side_effects(e->unary_postfix.operand);
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        return expr_has_side_effects(e->field.object);
    case EXPR_INDEX:
        return expr_has_side_effects(e->index.object) ||
               expr_has_side_effects(e->index.index);
    case EXPR_SLICE:
        return expr_has_side_effects(e->slice.object) ||
               expr_has_side_effects(e->slice.lo) ||
               expr_has_side_effects(e->slice.hi);
    case EXPR_CAST: return expr_has_side_effects(e->cast.operand);
    case EXPR_SOME: return expr_has_side_effects(e->some_expr.value);
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++)
            if (!e->interp_string.segments[i].is_literal &&
                expr_has_side_effects(e->interp_string.segments[i].expr))
                return true;
        return false;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (expr_has_side_effects(e->struct_lit.fields[i].value)) return true;
        return false;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            if (expr_has_side_effects(e->tuple_lit.elems[i])) return true;
        return false;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            if (expr_has_side_effects(e->array_lit.elems[i])) return true;
        return false;
    case EXPR_SLICE_LIT:
        return expr_has_side_effects(e->slice_lit.ptr_expr) ||
               expr_has_side_effects(e->slice_lit.len_expr);
    default:
        /* if/match/loop/for/block/break/return/... may contain effects */
        return true;
    }
}

/* True for operands that read no mutable state and have no side effects, so
 * their position relative to a side-effecting sibling can never be observed —
 * leaving them in place avoids a pointless temporary. */
static bool seq_is_atom(Expr *e) {
    switch (e->kind) {
    case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT: case EXPR_STRING_LIT: case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT: case EXPR_SIZEOF: case EXPR_ALIGNOF:
    case EXPR_TYPE_VAR_REF: case EXPR_DEFAULT:
        return true;
    case EXPR_IDENT:
        /* a top-level function reference is a constant, not a variable read */
        return e->type && e->type->kind == TYPE_FUNC && !e->ident.is_local;
    case EXPR_FIELD:
        return e->field.is_type_property || e->field.is_extern_const;
    default:
        return false;
    }
}

/* A synthetic local-ident expr that emits as the bare temp name (is_local
 * suppresses the function-value fat-pointer wrapping in EXPR_IDENT). */
static Expr seq_make_ref(const char *name, Type *type) {
    Expr e;
    memset(&e, 0, sizeof e);
    e.kind = EXPR_IDENT;
    e.type = type;
    e.ident.name = name;
    e.ident.codegen_name = name;
    e.ident.is_local = true;
    return e;
}

/* Does any operand in this list have side effects?  (If not, C's order is
 * already unobservable and no sequencing temps are needed.) */
static bool seq_needed(Expr ***slots, int n) {
    for (int i = 0; i < n; i++)
        if (expr_has_side_effects(*slots[i])) return true;
    return false;
}

/* Evaluate operands [0, n-1) into temporaries in source order (skipping
 * side-effect-free atoms and any operand lacking a type), rewriting their slots
 * to read the temporaries.  The final operand is left in place — it is
 * evaluated last regardless.  Assumes a statement-expression `({ ` is already
 * open.  `scratch` (length >= n) backs the synthetic ident refs and must
 * outlive the body emit; `saved` (length >= n) records originals for restore. */
static void seq_hoist(Expr ***slots, int n, Expr *scratch, Expr **saved, FILE *out) {
    for (int i = 0; i < n - 1; i++) {
        Expr *op = *slots[i];
        saved[i] = op;
        if (!op->type || seq_is_atom(op)) continue;
        char *nm = arena_alloc(g_arena, 24);
        snprintf(nm, 24, "_sq%d", temp_counter++);
        emit_type(op->type, out);
        fprintf(out, " %s = ", nm);
        emit_expr(op, out);
        fprintf(out, "; ");
        scratch[i] = seq_make_ref(nm, op->type);
        *slots[i] = &scratch[i];
    }
}

/* Restore operand slots rewritten by seq_hoist. */
static void seq_restore(Expr ***slots, int n, Expr **saved) {
    for (int i = 0; i < n - 1; i++) *slots[i] = saved[i];
}

/* True when emitting `e` yields a plainly parenthesized `(...)` expression, so
 * a controlling-position `if ` may follow it without adding its own parens
 * (avoiding clang's -Wparentheses-equality on `if ((x == y))`).  A binary
 * operand list with side effects, or a division/modulo, instead emits a
 * statement-expression `({...})` and must be parenthesized normally. */
static bool emit_self_parens(Expr *e) {
    if (e->kind != EXPR_BINARY) return false;
    if (e->binary.op == TOK_SLASH || e->binary.op == TOK_PERCENT) return false;
    Expr **slots[2] = { &e->binary.left, &e->binary.right };
    bool seq = e->binary.op != TOK_AMPAMP && e->binary.op != TOK_PIPEPIPE &&
               !g_const_context && seq_needed(slots, 2);
    return !seq;
}

/* Emit a function call argument for an extern call, inserting casts at the
 * C boundary: cstr (uint8*) → const char*, cstr* (uint8**) → char**,
 * and any** (void**) → void* (C's void* converts to any T** implicitly,
 * but void** does not). */
static void emit_extern_arg(Expr *e, Type *param_type, FILE *out) {
    if (param_type && param_type->kind == TYPE_FUNC) {
        /* Function at C extern boundary — emit C-compatible trampoline name */
        if (e->kind == EXPR_IDENT && !e->ident.is_local) {
            const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
            fprintf(out, "_ctramp_%s", name);
            return;
        }
        if (e->kind == EXPR_FUNC && e->func.capture_count == 0 && e->func.lifted_name) {
            fprintf(out, "_ctramp_%s", e->func.lifted_name);
            return;
        }
        /* Capturing lambda at C boundary — fall through, will produce type error */
    }
    if (param_type && is_cstr_type(param_type)) {
        if (param_type->is_const)
            fprintf(out, "(const char*)");
        else
            fprintf(out, "(char*)");
    } else if (param_type && param_type->kind == TYPE_POINTER &&
               is_cstr_type(param_type->pointer.pointee)) {
        /* uint8** → char** at C boundary (e.g. strtol's char** out-param) */
        fprintf(out, "(char**)");
    } else if (param_type && param_type->kind == TYPE_POINTER &&
               param_type->pointer.pointee->kind == TYPE_ANY_PTR) {
        /* any** (void**) → void* at C boundary — void* converts to any T**
         * implicitly in C, but void** does not (e.g. sqlite3_open's sqlite3**) */
        fprintf(out, "(void*)");
    } else if (!param_type && e->type) {
        /* Variadic arg — apply C default argument promotions and boundary casts */
        Type *at = e->type;
        if (is_cstr_type(at)) {
            fprintf(out, "(const char*)");
        } else if (at->kind == TYPE_FLOAT32) {
            fprintf(out, "(double)");
        } else if (at->kind == TYPE_INT8 || at->kind == TYPE_INT16 ||
                   at->kind == TYPE_UINT8 || at->kind == TYPE_UINT16) {
            fprintf(out, "(int)");
        }
    }
    emit_expr(e, out);
}

/* Returns true if this pattern produces any condition predicate — false for
   wildcard/binding patterns and for struct/or patterns whose subpatterns are
   all wildcard-only. Used to decide whether an arm needs an `if` at all. */
static bool pattern_has_predicate(Pattern *pat) {
    switch (pat->kind) {
    case PAT_WILDCARD:
    case PAT_BINDING:
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
            if (pattern_has_predicate(pat->or_pat.alts[i])) return true;
        return false;
    default:
        return true;
    }
}

/* Emit predicates joined by ` && `, without any enclosing `if (`.
   `first` tracks whether any predicate has been emitted yet in the current
   conjunction chain — it flips to false on the first emission. */
static void emit_pat_predicate(Pattern *pat, const char *expr, Type *type, bool *first, FILE *out) {
    switch (pat->kind) {
    case PAT_BINDING:
    case PAT_WILDCARD:
        break;
    case PAT_INT_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        if (pat->int_lit.lit_type && (pat->int_lit.lit_type->kind == TYPE_UINT64 ||
            pat->int_lit.lit_type->kind == TYPE_UINT32 || pat->int_lit.lit_type->kind == TYPE_UINT16 ||
            pat->int_lit.lit_type->kind == TYPE_UINT8 || pat->int_lit.lit_type->kind == TYPE_USIZE))
            fprintf(out, "%s == %" PRIu64, expr, pat->int_lit.value);
        else
            fprintf(out, "%s == %" PRId64, expr, (int64_t)pat->int_lit.value);
        break;
    case PAT_BOOL_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "%s == %s", expr, pat->bool_lit.value ? "true" : "false");
        break;
    case PAT_CHAR_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "%s == '\\x%02x'", expr, pat->char_lit.value);
        break;
    case PAT_SOME: {
        if (!*first) fprintf(out, " && ");
        *first = false;
        bool is_ptr = is_null_sentinel(type);
        if (is_ptr) fprintf(out, "%s != NULL", expr);
        else fprintf(out, "%s.has_value", expr);
        if (pat->some_pat.inner) {
            char inner_expr[256];
            if (is_ptr) snprintf(inner_expr, sizeof(inner_expr), "%s", expr);
            else snprintf(inner_expr, sizeof(inner_expr), "%s.value", expr);
            emit_pat_predicate(pat->some_pat.inner, inner_expr, type->option.inner, first, out);
        }
        break;
    }
    case PAT_NONE:
        if (!*first) fprintf(out, " && ");
        *first = false;
        if (is_null_sentinel(type))
            fprintf(out, "%s == NULL", expr);
        else
            fprintf(out, "!%s.has_value", expr);
        break;
    case PAT_VARIANT: {
        if (!*first) fprintf(out, " && ");
        *first = false;
        const char *uname = type->unio.name;
        if (g_subst && type_contains_type_var(type)) {
            uname = mangle_generic_with_subst(uname, type);
        }
        fprintf(out, "%s.tag == %s_tag_%s", expr, uname, pat->variant.variant);
        if (pat->variant.payload) {
            char payload_expr[256];
            snprintf(payload_expr, sizeof(payload_expr), "%s.%s", expr,
                c_safe_ident(g_intern, pat->variant.variant));
            Type *payload_type = NULL;
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->variant.variant) {
                    payload_type = type->unio.variants[v].payload;
                    break;
                }
            }
            if (payload_type)
                emit_pat_predicate(pat->variant.payload, payload_expr, payload_type, first, out);
        }
        break;
    }
    case PAT_STRUCT:
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            char path[256];
            snprintf(path, sizeof(path), "%s.%s", expr,
                c_safe_ident(g_intern, pat->struc.fields[fi].name));
            emit_pat_predicate(pat->struc.fields[fi].pattern, path, pat->struc.fields[fi].resolved_type, first, out);
        }
        break;
    case PAT_STRING_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "fc_eq_fc_str(%s, (fc_str){(uint8_t*)\"%.*s\", %d})",
            expr, pat->string_lit.length, pat->string_lit.value, pat->string_lit.length);
        break;
    case PAT_OR: {
        if (!pattern_has_predicate(pat)) break;
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "(");
        for (int i = 0; i < pat->or_pat.alt_count; i++) {
            if (i > 0) fprintf(out, " || ");
            fprintf(out, "(");
            if (pattern_has_predicate(pat->or_pat.alts[i])) {
                bool alt_first = true;
                emit_pat_predicate(pat->or_pat.alts[i], expr, type, &alt_first, out);
            } else {
                fprintf(out, "1");
            }
            fprintf(out, ")");
        }
        fprintf(out, ")");
        break;
    }
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            char path[256];
            snprintf(path, sizeof(path), "%s.e%d", expr, i);
            emit_pat_predicate(pat->tuple_pat.patterns[i], path,
                pat->tuple_pat.resolved_types[i], first, out);
        }
        break;
    }
}

/* Recursively emit condition checks for any pattern.
   expr is the C expression for the value being matched.
   type is the FC type of the value.
   has_cond tracks whether "if (" has been emitted. */
static void emit_pat_conditions(Pattern *pat, const char *expr, Type *type, bool *has_cond, FILE *out) {
    if (!pattern_has_predicate(pat)) return;
    if (!*has_cond) {
        fprintf(out, "if (");
        *has_cond = true;
    } else {
        fprintf(out, " && ");
    }
    bool first = true;
    emit_pat_predicate(pat, expr, type, &first, out);
}

/* Recursively emit variable declarations for all bindings in a pattern.
   expr is the C expression for the value being matched.
   type is the FC type of the value. */
static void emit_pat_bindings(Pattern *pat, const char *expr, Type *type, FILE *out) {
    switch (pat->kind) {
    case PAT_BINDING:
        emit_indent(out);
        if (is_hoisted(pat->binding.name)) {
            fprintf(out, "%s = %s;\n", pat->binding.name, expr);
        } else {
            emit_type(type, out);
            fprintf(out, " %s = %s;\n", pat->binding.name, expr);
            emit_indent(out);
            fprintf(out, "(void)%s;\n", pat->binding.name);
        }
        break;
    case PAT_WILDCARD:
    case PAT_INT_LIT:
    case PAT_BOOL_LIT:
    case PAT_CHAR_LIT:
    case PAT_NONE:
    case PAT_STRING_LIT:
        break;
    case PAT_SOME: {
        if (pat->some_pat.inner) {
            Type *inner_type = type->option.inner;
            char inner_expr[256];
            if (is_null_sentinel(type))
                snprintf(inner_expr, sizeof(inner_expr), "%s", expr);
            else
                snprintf(inner_expr, sizeof(inner_expr), "%s.value", expr);
            emit_pat_bindings(pat->some_pat.inner, inner_expr, inner_type, out);
        }
        break;
    }
    case PAT_VARIANT: {
        if (pat->variant.payload) {
            /* Get variant payload type from the mono table's concrete_type if available,
             * since the pass2 expression type may have unsubstituted type variables
             * in variant payloads (pass2 renames but doesn't substitute them). */
            Type *source_union = type;
            if (type->kind == TYPE_UNION && g_mono) {
                MonoInstance *mi = mono_find(g_mono, type->unio.name);
                if (mi && mi->concrete_type && mi->concrete_type->kind == TYPE_UNION)
                    source_union = mi->concrete_type;
            }
            Type *payload_type = NULL;
            for (int v = 0; v < source_union->unio.variant_count; v++) {
                if (source_union->unio.variants[v].name == pat->variant.variant) {
                    payload_type = source_union->unio.variants[v].payload;
                    break;
                }
            }
            /* Substitute type variables when inside a monomorphized context */
            if (payload_type && g_subst) {
                payload_type = type_substitute(g_arena, payload_type,
                    g_subst->var_names, g_subst->concrete, g_subst->count);
            }
            if (payload_type) {
                char payload_expr[256];
                snprintf(payload_expr, sizeof(payload_expr), "%s.%s", expr,
                    c_safe_ident(g_intern, pat->variant.variant));
                emit_pat_bindings(pat->variant.payload, payload_expr, payload_type, out);
            }
        }
        break;
    }
    case PAT_STRUCT:
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            char path[256];
            snprintf(path, sizeof(path), "%s.%s", expr,
                c_safe_ident(g_intern, pat->struc.fields[fi].name));
            emit_pat_bindings(pat->struc.fields[fi].pattern, path, pat->struc.fields[fi].resolved_type, out);
        }
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            char path[256];
            snprintf(path, sizeof(path), "%s.e%d", expr, i);
            emit_pat_bindings(pat->tuple_pat.patterns[i], path, pat->tuple_pat.resolved_types[i], out);
        }
        break;
    case PAT_OR:
        /* Alternatives are binding-free in v1, so there's nothing to emit. */
        break;
    }
}

/* A type that materializes no C value: void, or never (return/break/continue).
   Used to decide whether an if/match needs a result temp and whether a tail
   expression should be wrapped in an implicit return. */
static bool type_valueless(Type *t) {
    return !t || t->kind == TYPE_VOID || t->kind == TYPE_NEVER;
}

static void emit_block_stmts(Expr **stmts, int count, FILE *out, bool as_return, bool discard_value) {
    /* Find last non-defer statement index (needed for block-value handling) */
    int last_real_idx = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (stmts[i]->kind != EXPR_DEFER) { last_real_idx = i; break; }
    }

    for (int i = 0; i < count; i++) {
        Expr *s = stmts[i];
        bool is_last = (i == count - 1);

        /* DEFER: record, don't emit */
        if (s->kind == EXPR_DEFER) {
            defer_scope_add(s->defer_expr.value);
            continue;
        }

        emit_indent(out);

        if (s->kind == EXPR_LET) {
            const char *vname = s->let_expr.codegen_name ? s->let_expr.codegen_name : s->let_expr.let_name;
            if (is_hoisted(vname)) {
                /* Hoisted: declaration already at function top, just assign */
                fprintf(out, "%s = ", vname);
            } else {
                emit_type(s->let_expr.let_type, out);
                fprintf(out, " %s = ", vname);
            }
            emit_expr(s->let_expr.let_init, out);
            fprintf(out, ";\n");
            emit_indent(out);
            fprintf(out, "(void)%s;\n", vname);
        } else if (s->kind == EXPR_LET_DESTRUCT) {
            /* Emit: struct_type _ds_N = rhs; then recursively emit field bindings */
            emit_type(s->let_destruct.init_type, out);
            fprintf(out, " %s = ", s->let_destruct.tmp_name);
            emit_expr(s->let_destruct.init, out);
            fprintf(out, ";\n");
            emit_pat_bindings(s->let_destruct.pattern, s->let_destruct.tmp_name, s->let_destruct.init_type, out);
            emit_indent(out);
            fprintf(out, "(void)%s;\n", s->let_destruct.tmp_name);
        } else if (s->kind == EXPR_RETURN) {
            /* Emit defers before return */
            if (s->return_expr.value && has_pending_defers()) {
                emit_type(s->return_expr.value->type, out);
                int tid = temp_counter++;
                fprintf(out, " _ret%d = ", tid);
                emit_expr(s->return_expr.value, out);
                fprintf(out, ";\n");
                emit_defers_to_func(out);
                emit_indent(out);
                fprintf(out, "return _ret%d;\n", tid);
            } else {
                if (has_pending_defers()) emit_defers_to_func(out);
                emit_indent(out);
                if (s->return_expr.value) {
                    fprintf(out, "return ");
                    emit_expr(s->return_expr.value, out);
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "return;\n");
                }
            }
        } else if (s->kind == EXPR_ASSIGN) {
            emit_expr(s, out);
            fprintf(out, ";\n");
        } else if (s->kind == EXPR_BREAK) {
            /* Emit defers before break */
            if (s->break_expr.value && has_pending_defers()) {
                emit_type(s->break_expr.value->type, out);
                int tid = temp_counter++;
                fprintf(out, " _brk%d = ", tid);
                emit_expr(s->break_expr.value, out);
                fprintf(out, ";\n");
                emit_defers_to_loop(out);
                emit_indent(out);
                fprintf(out, "_loop_result = _brk%d; break;\n", tid);
            } else {
                if (has_pending_defers()) emit_defers_to_loop(out);
                emit_indent(out);
                if (s->break_expr.value) {
                    fprintf(out, "_loop_result = ");
                    emit_expr(s->break_expr.value, out);
                    fprintf(out, "; break;\n");
                } else {
                    fprintf(out, "break;\n");
                }
            }
        } else if (s->kind == EXPR_CONTINUE) {
            if (has_pending_defers()) emit_defers_to_loop(out);
            emit_indent(out);
            fprintf(out, "continue;\n");
        } else if (s->kind == EXPR_IF && type_valueless(s->type)) {
            /* void- or never-typed if → emit as C if statement */
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (s->kind == EXPR_FOR) {
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (s->kind == EXPR_LOOP && type_valueless(s->type)) {
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (is_last && as_return && s->type && !type_valueless(s->type)) {
            /* Implicit return — emit defers before returning */
            if (has_pending_defers()) {
                emit_type(s->type, out);
                int tid = temp_counter++;
                fprintf(out, " _ret%d = ", tid);
                emit_expr(s, out);
                fprintf(out, ";\n");
                emit_defers_to_func(out);
                emit_indent(out);
                fprintf(out, "return _ret%d;\n", tid);
            } else {
                fprintf(out, "return ");
                emit_expr(s, out);
                fprintf(out, ";\n");
            }
        } else if (!as_return && i == last_real_idx && s->type &&
                   s->type->kind != TYPE_VOID &&
                   g_defer_scope && g_defer_scope->count > 0) {
            /* Last expression in a value-producing block with pending defers
             * in the current scope. Save to temp, emit defers, then produce
             * temp as block value. Only triggers when the current scope itself
             * has defers — parent scope defers are handled at their own level. */
            emit_type(s->type, out);
            int tid = temp_counter++;
            fprintf(out, " _blk%d = ", tid);
            emit_expr(s, out);
            fprintf(out, ";\n");
            emit_scope_defers(g_defer_scope, out);
            emit_indent(out);
            if (discard_value)
                fprintf(out, "(void)_blk%d;\n", tid);
            else
                fprintf(out, "_blk%d;\n", tid);
        } else {
            /* Cast non-void expressions to (void) when their value is
             * discarded (non-last statement, or last with discard_value).
             * Prevents -Wunused-value for GCC statement expressions like
             * non-void match/block used as statements. */
            bool is_last = (i == last_real_idx);
            bool void_cast = s->type && s->type->kind != TYPE_VOID &&
                             (!is_last || discard_value);
            if (void_cast) fprintf(out, "(void)(");
            emit_expr(s, out);
            if (void_cast) fprintf(out, ");\n");
            else fprintf(out, ";\n");
        }
    }
    /* Emit end-of-block defers for fall-through.
     * Skip when already handled: as_return with non-void last expr (defers
     * emitted with the return), or non-void block value (handled above). */
    if (g_defer_scope && g_defer_scope->count > 0) {
        bool already_handled = false;
        if (as_return && last_real_idx >= 0) {
            Expr *last_real = stmts[last_real_idx];
            already_handled = last_real->type &&
                last_real->type->kind != TYPE_VOID &&
                last_real->kind != EXPR_RETURN;
        }
        if (!as_return && last_real_idx >= 0) {
            Expr *last_real = stmts[last_real_idx];
            already_handled = last_real->type &&
                last_real->type->kind != TYPE_VOID;
        }
        if (!already_handled) {
            emit_scope_defers(g_defer_scope, out);
        }
    }
}

static void emit_if_stmt(Expr *e, FILE *out) {
    /* Emit if as a C statement (not expression).
     * Binary expressions emit their own outer parens, so use "if "
     * to avoid double-parens like if ((x == y)) which triggers
     * clang's -Wparentheses-equality. */
    if (emit_self_parens(e->if_expr.cond)) {
        fprintf(out, "if ");
        emit_expr(e->if_expr.cond, out);
        fprintf(out, " {\n");
    } else {
        fprintf(out, "if (");
        emit_expr(e->if_expr.cond, out);
        fprintf(out, ") {\n");
    }
    indent_level++;
    if (e->if_expr.then_body->kind == EXPR_BLOCK) {
        defer_scope_push(false);
        emit_block_stmts(e->if_expr.then_body->block.stmts,
            e->if_expr.then_body->block.count, out, false, true);
        defer_scope_pop();
    } else {
        emit_indent(out);
        emit_expr(e->if_expr.then_body, out);
        fprintf(out, ";\n");
    }
    indent_level--;
    emit_indent(out);
    fprintf(out, "}");
    if (e->if_expr.else_body) {
        if (e->if_expr.else_body->kind == EXPR_IF &&
            type_valueless(e->if_expr.else_body->type)) {
            fprintf(out, " else ");
            emit_if_stmt(e->if_expr.else_body, out);
        } else {
            fprintf(out, " else {\n");
            indent_level++;
            if (e->if_expr.else_body->kind == EXPR_BLOCK) {
                defer_scope_push(false);
                emit_block_stmts(e->if_expr.else_body->block.stmts,
                    e->if_expr.else_body->block.count, out, false, true);
                defer_scope_pop();
            } else {
                emit_indent(out);
                emit_expr(e->if_expr.else_body, out);
                fprintf(out, ";\n");
            }
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        }
    }
}

/* Emit one if-branch (a single expression, possibly an EXPR_BLOCK) inside a
   statement-expression. When the branch diverges (`never`: its tail is
   return/break/continue or an all-diverging if/match) it is emitted as plain
   statements — the control-flow exit fires and nothing is assigned. Otherwise the
   branch's value is assigned to res_var. Mirrors emit_if_stmt's block/non-block
   handling and emit_expr's value handling. */
static void emit_branch_into(Expr *branch, const char *res_var, FILE *out) {
    if (type_is_never(branch->type)) {
        if (branch->kind == EXPR_BLOCK) {
            defer_scope_push(false);
            emit_block_stmts(branch->block.stmts, branch->block.count, out, false, true);
            defer_scope_pop();
        } else {
            emit_indent(out);
            emit_expr(branch, out);
            fprintf(out, ";\n");
        }
    } else {
        emit_indent(out);
        fprintf(out, "%s = ", res_var);
        emit_expr(branch, out);
        fprintf(out, ";\n");
    }
}

/* Helper: C unsigned type name for a signed integer type */
static const char *unsigned_counterpart(Type *t) {
    switch (t->kind) {
    case TYPE_INT8:  case TYPE_UINT8:  return "uint8_t";
    case TYPE_INT16: case TYPE_UINT16: return "uint16_t";
    case TYPE_INT32: case TYPE_UINT32: return "uint32_t";
    case TYPE_INT64: case TYPE_UINT64: return "uint64_t";
    case TYPE_ISIZE: case TYPE_USIZE:  return "size_t";
    default: return NULL;
    }
}

/* Sub-int integer types (int8/uint8/int16/uint16). C's integer-promotion rules
 * widen these to `int` in arithmetic, so a plain `a + b` yields an un-truncated
 * value (200u8 + 100u8 observes as 300, not 44) and narrow signed multiply hits
 * signed-overflow UB (-1i16 * -1i16 promotes to int and 65535*65535 overflows).
 * Such results must be routed through `unsigned` and/or truncated back to the
 * FC type. Types at least as wide as int (int32+/uint32+) don't need this. */
static bool type_is_subint(Type *t) {
    return t->kind == TYPE_INT8  || t->kind == TYPE_UINT8 ||
           t->kind == TYPE_INT16 || t->kind == TYPE_UINT16;
}

/* Helper: shift mask for an integer type's bit width.
 * Returns -1 for platform-dependent types (isize/usize) — caller must handle. */
static int shift_mask_for(Type *t) {
    switch (t->kind) {
    case TYPE_INT8:  case TYPE_UINT8:  return 7;
    case TYPE_INT16: case TYPE_UINT16: return 15;
    case TYPE_INT32: case TYPE_UINT32: return 31;
    case TYPE_INT64: case TYPE_UINT64: return 63;
    case TYPE_ISIZE: case TYPE_USIZE:  return -1;
    default: return 0;
    }
}

/* Parse width and precision from an InterpSegment format spec string.
 * Sets *width to the explicit width (0 if absent).
 * Sets *precision to the explicit precision (-1 if absent, 0 for ".0"). */
static void parse_format_width_prec(const char *text,
                                     int *width, int *precision) {
    const char *fs = text;
    *width = 0;
    *precision = -1;
    /* Skip flags */
    while (*fs == '-' || *fs == '+' || *fs == '0' ||
           *fs == '#' || *fs == ' ') fs++;
    /* Width */
    while (*fs >= '0' && *fs <= '9') {
        *width = *width * 10 + (*fs - '0');
        fs++;
    }
    /* Precision */
    if (*fs == '.') {
        fs++;
        *precision = 0;
        while (*fs >= '0' && *fs <= '9') {
            *precision = *precision * 10 + (*fs - '0');
            fs++;
        }
    }
}

/* Bytes a literal segment contributes to the formatted output: each `%%` folds
 * to one `%`, and a backslash escape counts as the single byte it denotes. */
static int interp_literal_len(InterpSegment *seg) {
    int actual_len = 0;
    const char *s = seg->text;
    int slen = seg->text_length;
    for (int j = 0; j < slen; j++) {
        if (s[j] == '%' && j + 1 < slen && s[j+1] == '%') j++;
        else if (s[j] == '\\' && j + 1 < slen) {
            if (s[j+1] == 'x' && j + 3 < slen) j += 3;
            else j++;
        }
        actual_len++;
    }
    return actual_len;
}

/* Upper bound on the bytes a non-string conversion can emit, for buffer sizing.
 * A field width is a *minimum*, never a maximum, so it can only widen the bound;
 * flags may add a sign/space/`#`-prefix byte.  Shared by the buffer-size emitter
 * and the constant-size scan so the two can never disagree on the budget. */
static int interp_numeric_bound(char conv, Type *t, const char *flags_text,
                                int explicit_width, int explicit_prec) {
    int bound;
    switch (conv) {
    case 'd': case 'i': case 'u':
        switch (t ? t->kind : 0) {
        case TYPE_INT8: bound = 4; break;
        case TYPE_UINT8: bound = 3; break;
        case TYPE_INT16: bound = 6; break;
        case TYPE_UINT16: bound = 5; break;
        case TYPE_INT32: bound = 11; break;
        case TYPE_UINT32: bound = 10; break;
        case TYPE_INT64: bound = 20; break;
        case TYPE_UINT64: bound = 20; break;
        default: bound = 20; break;
        }
        break;
    case 'x': case 'X':
        switch (t ? t->kind : 0) {
        case TYPE_INT8: case TYPE_UINT8: bound = 2; break;
        case TYPE_INT16: case TYPE_UINT16: bound = 4; break;
        case TYPE_INT32: case TYPE_UINT32: bound = 8; break;
        case TYPE_INT64: case TYPE_UINT64: bound = 16; break;
        default: bound = 16; break;
        }
        break;
    case 'o':
        switch (t ? t->kind : 0) {
        case TYPE_INT8: case TYPE_UINT8: bound = 3; break;
        case TYPE_INT16: case TYPE_UINT16: bound = 6; break;
        case TYPE_INT32: case TYPE_UINT32: bound = 11; break;
        case TYPE_INT64: case TYPE_UINT64: bound = 22; break;
        default: bound = 22; break;
        }
        break;
    case 'f': {
        /* A %f field width is a *minimum*, never a maximum: the integer part can
         * be as wide as the type's largest exponent in digits (DBL_MAX ≈ 1.8e308
         * → 309 digits; FLT_MAX ≈ 3.4e38 → 39).  The budget must cover sign +
         * integer digits + '.' + fraction (default precision 6). */
        int int_digits = (t && t->kind == TYPE_FLOAT32) ? 39 : 309;
        int prec = explicit_prec >= 0 ? explicit_prec : 6;
        bound = 1 + int_digits + 1 + prec;
        break;
    }
    case 'e': case 'E': case 'g': case 'G': {
        /* Scientific/shortest forms are bounded by precision, not the exponent:
         * sign + leading digit + '.' + prec mantissa digits + 'e±ddd'. */
        int prec = explicit_prec >= 0 ? explicit_prec : 6;
        bound = 9 + prec;
        break;
    }
    case 'c': bound = 1; break;
    case 'p': bound = 18; break;
    default: bound = 24; break;
    }
    const char *flags = flags_text;
    while (*flags == '-' || *flags == '+' || *flags == '0' || *flags == '#' || *flags == ' ') {
        if (*flags == '+' || *flags == ' ') bound++;
        if (*flags == '#') bound += 2;
        flags++;
    }
    if (explicit_width > bound) bound = explicit_width;
    return bound;
}

/* If the interpolation buffer length is a compile-time constant — i.e. no
 * segment contributes a runtime length, the only such case being a %s (str or
 * cstr) without an explicit precision — compute that constant byte budget (the
 * exact value the emitted _flen sums to, since both go through the helpers
 * above) and return true.  Otherwise return false: the buffer stays
 * runtime-sized and keeps its alloca/malloc allocation. */
static bool interp_const_buffer_size(Expr *e, int64_t *out_size) {
    int seg_count = e->interp_string.segment_count;
    InterpSegment *segs = e->interp_string.segments;
    int64_t total = 0;
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) {
            total += interp_literal_len(&segs[i]);
            continue;
        }
        if (segs[i].conversion == 'T') {
            total += (int64_t)strlen(type_name(segs[i].expr->type));
            continue;
        }
        char conv = segs[i].conversion;
        int explicit_width = 0, explicit_prec = -1;
        parse_format_width_prec(segs[i].text, &explicit_width, &explicit_prec);
        Type *t = segs[i].expr->type;
        bool is_str_arg = (conv == 's' && t && is_str_type(t));
        bool is_cstr_arg = (conv == 's' && t && is_cstr_type(t));
        if (is_str_arg || is_cstr_arg) {
            if (explicit_prec >= 0) {
                int b = explicit_prec;
                if (explicit_width > b) b = explicit_width;
                total += b;
            } else {
                return false;  /* runtime string length */
            }
        } else {
            total += interp_numeric_bound(conv, t, segs[i].text,
                                          explicit_width, explicit_prec);
        }
    }
    *out_size = total;
    return true;
}

/* Exported predicate (declared in ast.h): an interpolation is runtime-sized iff
 * its buffer size is not a compile-time constant — the single source being a
 * %s/cstr segment without an explicit precision. Shares interp_const_buffer_size
 * so pass2's rejection and codegen's buffer emission agree exactly. */
bool interp_is_runtime_sized(const Expr *e) {
    int64_t dummy = 0;
    return !interp_const_buffer_size((Expr *)e, &dummy);
}

/* Emit interpolated string code.
 * alloc_opt_type: NULL → standalone (alloca), non-NULL → inside alloc() (malloc).
 * Handles both str and cstr interpolation (e->interp_string.is_cstr). */
static void emit_interp_string_impl(Expr *e, FILE *out, Type *alloc_opt_type) {
    bool use_heap = (alloc_opt_type != NULL);
    bool is_cstr = e->interp_string.is_cstr;
    int tid = temp_counter++;
    int seg_count = e->interp_string.segment_count;
    InterpSegment *segs = e->interp_string.segments;

    fprintf(out, "({ ");

    /* Pre-evaluate every non-literal, non-%T segment into a temp, in textual
     * order. This guarantees each interpolated expression is evaluated exactly
     * once (snprintf evaluates its arguments in unspecified order, and a cstr
     * %s would otherwise be evaluated twice — once for strlen, once as the
     * argument) and in source order. %T is excluded: its operand is never
     * evaluated, only its static type name is used. */
    for (int i = 0, k = 0; i < seg_count; i++) {
        if (segs[i].is_literal || segs[i].conversion == 'T') continue;
        emit_type(segs[i].expr->type, out);
        fprintf(out, " _sg%d_%d = ", tid, k);
        emit_expr(segs[i].expr, out);
        fprintf(out, "; ");
        k++;
    }

    /* Compute buffer size expression. The buffer must be guaranteed large
     * enough: a printf field width is a *minimum*, never a maximum, so it can
     * never tighten a bound — only widen it. String precision is the one true
     * maximum. */
    fprintf(out, "int64_t _flen%d = ", tid);
    bool first_term = true;
    int k = 0;
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) {
            int actual_len = interp_literal_len(&segs[i]);
            if (actual_len > 0) {
                if (!first_term) fprintf(out, " + ");
                fprintf(out, "%d", actual_len);
                first_term = false;
            }
            continue;
        }
        if (segs[i].conversion == 'T') {
            /* %T contributes the fixed length of the compile-time type name. */
            const char *tname = type_name(segs[i].expr->type);
            if (!first_term) fprintf(out, " + ");
            fprintf(out, "%d", (int)strlen(tname));
            first_term = false;
            continue;
        }

        char conv = segs[i].conversion;
        int explicit_width = 0, explicit_prec = -1;
        parse_format_width_prec(segs[i].text, &explicit_width, &explicit_prec);
        Type *t = segs[i].expr->type;
        bool is_str_arg = (conv == 's' && t && is_str_type(t));
        bool is_cstr_arg = (conv == 's' && t && is_cstr_type(t));

        if (!first_term) fprintf(out, " + ");
        first_term = false;

        if (is_str_arg || is_cstr_arg) {
            /* String-valued: precision is a hard maximum; width is a minimum
             * field. With a precision the bound is the constant max(prec,
             * width); otherwise it is the runtime length, floored at the
             * minimum field width (the str path was already correct; the cstr
             * path previously ignored the width and under-allocated). */
            if (explicit_prec >= 0) {
                int b = explicit_prec;
                if (explicit_width > b) b = explicit_width;
                fprintf(out, "%d", b);
            } else if (is_str_arg) {
                if (explicit_width > 0)
                    fprintf(out, "(%d > _sg%d_%d.len ? %d : _sg%d_%d.len)",
                        explicit_width, tid, k, explicit_width, tid, k);
                else
                    fprintf(out, "_sg%d_%d.len", tid, k);
            } else {
                if (explicit_width > 0)
                    fprintf(out, "((int64_t)%d > (int64_t)strlen((const char*)_sg%d_%d)"
                                 " ? (int64_t)%d : (int64_t)strlen((const char*)_sg%d_%d))",
                        explicit_width, tid, k, explicit_width, tid, k);
                else
                    fprintf(out, "(int64_t)strlen((const char*)_sg%d_%d)", tid, k);
            }
        } else {
            int bound = interp_numeric_bound(conv, t, segs[i].text,
                                             explicit_width, explicit_prec);
            fprintf(out, "%d", bound);
        }
        k++;
    }
    if (first_term) fprintf(out, "0");
    fprintf(out, "; ");

    /* Allocate buffer.  A constant-size buffer points at the fixed array hoisted
     * to function entry (codegen_backing_name) — its slot is reused on every loop
     * iteration, so stack use stays bounded.  A runtime-sized buffer (a %s/cstr
     * without an explicit precision) alloca's afresh each evaluation — which
     * grows the frame per loop iteration, documented as a known cost with
     * alloc(s)! as the heap-promoting escape hatch — or malloc's under alloc(s)!. */
    if (use_heap)
        fprintf(out, "uint8_t *_fbuf%d = (uint8_t*)malloc(fc_to_size(_flen%d + 1)); ", tid, tid);
    else if (e->interp_string.codegen_backing_name)
        fprintf(out, "uint8_t *_fbuf%d = %s; ", tid, e->interp_string.codegen_backing_name);
    else
        fprintf(out, "uint8_t *_fbuf%d = (uint8_t*)__builtin_alloca(fc_to_size(_flen%d + 1)); ", tid, tid);

    /* Build snprintf call */
    if (use_heap)
        fprintf(out, "int _fw%d = _fbuf%d ? snprintf((char*)_fbuf%d, fc_to_size(_flen%d + 1), \"",
            tid, tid, tid, tid);
    else
        fprintf(out, "int _fw%d = snprintf((char*)_fbuf%d, fc_to_size(_flen%d + 1), \"",
            tid, tid, tid);

    /* Emit format string */
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) {
            const char *s = segs[i].text;
            int slen = segs[i].text_length;
            for (int j = 0; j < slen; j++) {
                if (s[j] == '%') {
                    fprintf(out, "%%%%");
                    if (j + 1 < slen && s[j+1] == '%') j++;
                } else if (s[j] == '"') {
                    fprintf(out, "\\\"");
                } else {
                    fputc(s[j], out);
                }
            }
        } else if (segs[i].conversion == 'T') {
            const char *tname = type_name(segs[i].expr->type);
            fputs(tname, out);
        } else {
            bool is_str_arg2 = (segs[i].conversion == 's' &&
                               segs[i].expr->type && is_str_type(segs[i].expr->type));
            if (is_str_arg2) {
                fprintf(out, "%%");
                const char *sp = segs[i].text;
                int splen = segs[i].text_length;
                int j = 0;
                while (j < splen - 1 && (sp[j] == '-' || sp[j] == '+' ||
                       sp[j] == '0' || sp[j] == '#' || sp[j] == ' ')) {
                    fputc(sp[j], out);
                    j++;
                }
                while (j < splen - 1 && sp[j] >= '0' && sp[j] <= '9') {
                    fputc(sp[j], out);
                    j++;
                }
                if (j < splen - 1 && sp[j] == '.') {
                    j++;
                    while (j < splen - 1 && sp[j] >= '0' && sp[j] <= '9') j++;
                }
                fprintf(out, ".*s");
            } else {
                Type *t = segs[i].expr->type;
                char conv = segs[i].conversion;
                /* Every integer-number conversion is passed as (unsigned) long
                 * long (see the argument pass), so it always takes the `ll`
                 * length modifier — independent of the target's `int` width.
                 * %c (char/uint8) is passed as int and keeps no modifier. */
                bool int_number = t && type_is_integer(t) &&
                    (conv == 'd' || conv == 'i' || conv == 'u' ||
                     conv == 'x' || conv == 'X' || conv == 'o');
                const char *sp = segs[i].text;
                int splen = segs[i].text_length;
                fprintf(out, "%%");
                if (int_number) {
                    for (int j = 0; j < splen - 1; j++) fputc(sp[j], out);
                    fprintf(out, "ll%c", sp[splen - 1]);
                } else {
                    fwrite(sp, 1, (size_t)splen, out);
                }
            }
        }
    }
    fprintf(out, "\"");

    /* Emit arguments — every one reads its pre-evaluated temp (_sg<tid>_<k>),
     * so each expression is evaluated exactly once and in source order. */
    int ak = 0;
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) continue;
        if (segs[i].conversion == 'T') continue;
        fprintf(out, ", ");
        char conv = segs[i].conversion;
        Type *t = segs[i].expr->type;
        bool is_str_arg2 = (conv == 's' && t && is_str_type(t));
        if (is_str_arg2) {
            int prec_w = 0, prec_p = -1;
            parse_format_width_prec(segs[i].text, &prec_w, &prec_p);
            if (prec_p >= 0) {
                /* Compare in int64 to avoid truncating before the min; the
                 * fc_to_int branch is only taken when len < prec_p, so the
                 * narrowing assert is trivially satisfied. */
                fprintf(out, "(_sg%d_%d.len < (int64_t)%d ? fc_to_int(_sg%d_%d.len) : %d), _sg%d_%d.ptr",
                    tid, ak, prec_p, tid, ak, prec_p, tid, ak);
            } else {
                fprintf(out, "fc_to_int(_sg%d_%d.len), _sg%d_%d.ptr",
                    tid, ak, tid, ak);
            }
        } else {
            if (t && type_is_integer(t)) {
                if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                    /* Unsigned conversions print the operand's bit pattern.
                     * First reinterpret it at its *own* width via the unsigned
                     * counterpart — casting a signed narrow operand straight to
                     * a wider unsigned type sign-extends it (e.g. -16i8 →
                     * 0xFFFFFFF0, formatted "fffffff0" and overrunning the
                     * 2-digit budget). Then widen to unsigned long long so the
                     * `ll` length modifier is correct on every target: a plain
                     * (unsigned int) is only 16 bits where C's `int` is, which
                     * would truncate a uint32 value. */
                    const char *uw;
                    switch (t->kind) {
                    case TYPE_INT8:  case TYPE_UINT8:  uw = "uint8_t";  break;
                    case TYPE_INT16: case TYPE_UINT16: uw = "uint16_t"; break;
                    case TYPE_INT32: case TYPE_UINT32: uw = "uint32_t"; break;
                    default:                           uw = "uint64_t"; break;
                    }
                    fprintf(out, "(unsigned long long)(%s)", uw);
                } else if (conv == 'c') {
                    /* %c (uint8 operand) takes an int argument, no modifier. */
                    fprintf(out, "(int)");
                } else {
                    /* Signed decimal: widen to long long (matches the `ll`
                     * modifier) so int32 is not truncated on a 16-bit-`int`
                     * target, where int32_t is `long`, not `int`. */
                    fprintf(out, "(long long)");
                }
            } else if (t && t->kind == TYPE_FLOAT32) {
                fprintf(out, "(double)");
            } else if (t && is_cstr_type(t)) {
                if (conv == 'p')
                    fprintf(out, "(void*)");
                else
                    fprintf(out, "(const char*)");
            } else if (t && (t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR)) {
                fprintf(out, "(void*)");
            }
            fprintf(out, "_sg%d_%d", tid, ak);
        }
        ak++;
    }
    fprintf(out, ")");
    if (use_heap) fprintf(out, " : 0");  /* close the _fbuf ? snprintf(...) : 0 ternary */
    fprintf(out, "; ");
    if (is_cstr) fprintf(out, "(void)_fw%d; ", tid);  /* suppress -Wunused-variable for cstr paths */

    /* Construct result */
    if (use_heap && is_cstr) {
        /* cstr? = bare pointer (null sentinel) */
        fprintf(out, "_fbuf%d; })", tid);
    } else if (use_heap) {
        /* str? = fc_option_fc_str */
        fprintf(out, "_fbuf%d ? (", tid);
        emit_type(alloc_opt_type, out);
        fprintf(out, "){ .value = (fc_str){ .ptr = _fbuf%d, .len = _fw%d >= 0 && _fw%d <= _flen%d ? _fw%d : _flen%d }, .has_value = true } : (",
            tid, tid, tid, tid, tid, tid);
        emit_type(alloc_opt_type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else if (is_cstr) {
        /* standalone cstr = bare pointer */
        fprintf(out, "_fbuf%d; })", tid);
    } else {
        /* standalone str = fc_str */
        fprintf(out, "(fc_str){ .ptr = _fbuf%d, .len = _fw%d >= 0 && _fw%d <= _flen%d ? _fw%d : _flen%d }; })",
            tid, tid, tid, tid, tid, tid);
    }
}

/* Emit text as a C string literal body, escaping special chars */
static void emit_c_escaped(const char *text, int len, FILE *out) {
    for (int i = 0; i < len; i++) {
        switch (text[i]) {
        case '\\': fprintf(out, "\\\\"); break;
        case '"':  fprintf(out, "\\\""); break;
        case '\n': fprintf(out, "\\n"); break;
        case '\r': fprintf(out, "\\r"); break;
        case '\t': fprintf(out, "\\t"); break;
        case '%':  fprintf(out, "%%%%"); break;
        default:
            if ((unsigned char)text[i] < 0x20)
                fprintf(out, "\\x%02x", (unsigned char)text[i]);
            else
                fputc(text[i], out);
            break;
        }
    }
}

/* Per-integer-type data for FC's saturating float->int conversion (audit item
 * 16): NaN -> 0, clamp to [min,max] at the range edge, else truncate toward
 * zero. Single source of truth for the two emission sites in float_to_int_emit:
 * the runtime helpers `fc_f2*` (prelude) and the equivalent constant-expression
 * ternary. The guard constants are the exact power-of-two boundaries just
 * outside each type's range (all exactly representable as double), so the inner
 * cast never sees an unrepresentable value. `lo`/`hi` are the double-typed
 * saturation guards; `imin`/`imax` the integer results at/beyond them. Returns
 * false if `k` is not an integer type. */
typedef struct { const char *fn, *cty, *lo, *imin, *hi, *imax; } F2iInfo;

static bool float_to_int_info(TypeKind k, F2iInfo *o) {
    switch (k) {
    case TYPE_INT8:   *o = (F2iInfo){"fc_f2i8",  "int8_t",  "-128.0",                 "INT8_MIN",  "128.0",                 "INT8_MAX"};  return true;
    case TYPE_INT16:  *o = (F2iInfo){"fc_f2i16", "int16_t", "-32768.0",               "INT16_MIN", "32768.0",               "INT16_MAX"}; return true;
    case TYPE_INT32:  *o = (F2iInfo){"fc_f2i32", "int32_t", "-2147483648.0",          "INT32_MIN", "2147483648.0",          "INT32_MAX"}; return true;
    case TYPE_INT64:  *o = (F2iInfo){"fc_f2i64", "int64_t", "-9223372036854775808.0", "INT64_MIN", "9223372036854775808.0", "INT64_MAX"}; return true;
    case TYPE_UINT8:  *o = (F2iInfo){"fc_f2u8",  "uint8_t",  "0.0", "0", "256.0",                  "UINT8_MAX"};  return true;
    case TYPE_UINT16: *o = (F2iInfo){"fc_f2u16", "uint16_t", "0.0", "0", "65536.0",                "UINT16_MAX"}; return true;
    case TYPE_UINT32: *o = (F2iInfo){"fc_f2u32", "uint32_t", "0.0", "0", "4294967296.0",           "UINT32_MAX"}; return true;
    case TYPE_UINT64: *o = (F2iInfo){"fc_f2u64", "uint64_t", "0.0", "0", "18446744073709551616.0", "UINT64_MAX"}; return true;
    case TYPE_ISIZE:  *o = (F2iInfo){"fc_f2isize", "ptrdiff_t", "(double)PTRDIFF_MIN", "PTRDIFF_MIN", "-(double)PTRDIFF_MIN", "PTRDIFF_MAX"}; return true;
    case TYPE_USIZE:  *o = (F2iInfo){"fc_f2usize", "size_t", "0.0", "0", "(double)SIZE_MAX", "SIZE_MAX"}; return true;
    default: return false;
    }
}

/* Emit a saturating float->int conversion of `operand` to integer type `info`.
 * At runtime, call the single-evaluation helper. In const context (a C
 * file-scope initializer, where a function call is not a constant expression)
 * emit the equivalent ternary directly — the operand is a pure constant there,
 * so evaluating it up to four times is harmless, and deferring the float
 * arithmetic to the C compiler keeps it target-correct (FC does not fold
 * floats). Both forms produce identical results. */
static void float_to_int_emit(const F2iInfo *info, Expr *operand, FILE *out) {
    if (!g_const_context) {
        fprintf(out, "%s(", info->fn);
        emit_expr(operand, out);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "((");
    emit_expr(operand, out);
    fprintf(out, ") != (");
    emit_expr(operand, out);
    fprintf(out, ") ? 0 : ((");
    emit_expr(operand, out);
    fprintf(out, ") < %s ? %s : ((", info->lo, info->imin);
    emit_expr(operand, out);
    fprintf(out, ") >= %s ? %s : (%s)(", info->hi, info->imax, info->cty);
    emit_expr(operand, out);
    fprintf(out, "))))");
}

static void emit_expr(Expr *e, FILE *out) {
    switch (e->kind) {
    case EXPR_INT_LIT:
        if (e->int_lit.lit_type->kind == TYPE_INT64) {
            /* INT64_MIN cannot be written as a single literal (its magnitude
             * exceeds INT64_MAX); emit the portable (-MAX - 1) idiom. */
            if ((int64_t)e->int_lit.value == INT64_MIN)
                fprintf(out, "(-9223372036854775807LL - 1)");
            else
                fprintf(out, "INT64_C(%" PRId64 ")", (int64_t)e->int_lit.value);
        }
        else if (e->int_lit.lit_type->kind == TYPE_UINT64)
            fprintf(out, "UINT64_C(%" PRIu64 ")", e->int_lit.value);
        else if (e->int_lit.lit_type->kind == TYPE_ISIZE)
            fprintf(out, "((ptrdiff_t)%" PRId64 "LL)", (int64_t)e->int_lit.value);
        else if (e->int_lit.lit_type->kind == TYPE_USIZE)
            fprintf(out, "((size_t)%" PRIu64 "ULL)", e->int_lit.value);
        else
            fprintf(out, "%" PRId64, (int64_t)e->int_lit.value);
        break;

    case EXPR_FLOAT_LIT: {
        /* Use enough precision to round-trip IEEE 754 doubles (17 digits)
         * and floats (9 digits). %g may strip the decimal point (e.g.,
         * 0.0 → "0"), which makes "0f" invalid C. Ensure there's always one. */
        char fbuf[64];
        int prec = (e->float_lit.lit_type->kind == TYPE_FLOAT32) ? 9 : 17;
        snprintf(fbuf, sizeof(fbuf), "%.*g", prec, e->float_lit.value);
        bool has_dot = (strchr(fbuf, '.') || strchr(fbuf, 'e') || strchr(fbuf, 'E'));
        if (e->float_lit.lit_type->kind == TYPE_FLOAT32) {
            if (has_dot) fprintf(out, "%sf", fbuf);
            else fprintf(out, "%s.0f", fbuf);
        } else {
            if (has_dot) fprintf(out, "%s", fbuf);
            else fprintf(out, "%s.0", fbuf);
        }
        break;
    }

    case EXPR_BOOL_LIT:
        fprintf(out, "%s", e->bool_lit.value ? "true" : "false");
        break;

    case EXPR_VOID_LIT:
        fprintf(out, "((void)0)");
        break;

    case EXPR_CHAR_LIT:
        fprintf(out, "'\\x%02x'", e->char_lit.value);
        break;

    case EXPR_STRING_LIT: {
        /* Compute actual byte length after C escape processing */
        int actual_len = 0;
        const char *s = e->string_lit.value;
        int slen = e->string_lit.length;
        for (int j = 0; j < slen; j++) {
            if (s[j] == '%' && j + 1 < slen && s[j+1] == '%') j++;
            else if (s[j] == '\\' && j + 1 < slen) {
                if (s[j+1] == 'x' && j + 3 < slen) j += 3;
                else j++;
            }
            actual_len++;
        }
        fprintf(out, "((fc_str){(uint8_t*)\"%.*s\", %d})",
            e->string_lit.length, e->string_lit.value, actual_len);
        break;
    }

    case EXPR_CSTRING_LIT:
        fprintf(out, "(uint8_t*)\"%.*s\"", e->cstring_lit.length, e->cstring_lit.value);
        break;

    case EXPR_IDENT:
        /* Built-in globals: stdin, stdout, stderr */
        if (e->type && e->type->kind == TYPE_ANY_PTR) {
            const char *n = e->ident.name;
            if (strcmp(n, "stdin") == 0) { fprintf(out, "(void*)stdin"); break; }
            if (strcmp(n, "stdout") == 0) { fprintf(out, "(void*)stdout"); break; }
            if (strcmp(n, "stderr") == 0) { fprintf(out, "(void*)stderr"); break; }
        }
        if (e->type && e->type->kind == TYPE_FUNC &&
            !e->ident.is_local) {
            /* Top-level function used as a value — wrap in fat pointer */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
        } else {
            fprintf(out, "%s", e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
        }
        break;

    case EXPR_BINARY: {
        /* Force left-to-right operand evaluation when an operand has side
         * effects (C leaves binary-operand order unspecified).  /, %, &&, ||
         * are excluded: division self-sequences its temps below, and &&/|| are
         * already left-to-right and must not have their right operand hoisted
         * (it is conditionally evaluated). */
        Expr **_bslots[2] = { &e->binary.left, &e->binary.right };
        Expr _bscratch[2]; Expr *_bsaved[2];
        bool _bseq = e->binary.op != TOK_SLASH && e->binary.op != TOK_PERCENT &&
                     e->binary.op != TOK_AMPAMP && e->binary.op != TOK_PIPEPIPE &&
                     !g_const_context && seq_needed(_bslots, 2);
        if (_bseq) { fprintf(out, "({ "); seq_hoist(_bslots, 2, _bscratch, _bsaved, out); }
        /* Structural equality on complex types */
        if ((e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) && e->binary.left->type) {
            Type *cmp_type = e->binary.left->type;
            /* Resolve type vars through g_subst if active */
            if (g_subst && cmp_type->kind == TYPE_TYPE_VAR) {
                for (int i = 0; i < g_subst->count; i++) {
                    if (g_subst->var_names[i] == cmp_type->type_var.name) {
                        cmp_type = g_subst->concrete[i];
                        break;
                    }
                }
            }
            if (type_needs_eq_func(cmp_type)) {
                if (e->binary.op == TOK_BANGEQ) fprintf(out, "(!"); else fprintf(out, "(");
                emit_eq_func_name(cmp_type, out);
                fprintf(out, "(");
                emit_expr(e->binary.left, out);
                fprintf(out, ", ");
                emit_expr(e->binary.right, out);
                fprintf(out, "))");
                goto _binary_done;
            }
        }
        int op = e->binary.op;
        Type *rt = e->type;

        /* Pointer difference (ptr - ptr) → ptrdiff_t element count. Must emit a
         * plain subtraction: the signed-wrap path below would cast the pointers
         * to unsigned and compute a byte difference instead. */
        if (op == TOK_MINUS && e->binary.left->type &&
            e->binary.left->type->kind == TYPE_POINTER &&
            e->binary.right->type &&
            e->binary.right->type->kind == TYPE_POINTER) {
            fprintf(out, "(");
            emit_expr(e->binary.left, out);
            fprintf(out, " - ");
            emit_expr(e->binary.right, out);
            fprintf(out, ")");
            goto _binary_done;
        }

        /* Integer +, -, *: overflow is defined as modular wraparound.
         *
         * Sub-int result types (int8/uint8/int16/uint16) promote to `int`, which
         * leaves the value un-truncated in the surrounding expression
         * (200u8 + 100u8 would observe as 300, not 44) and causes signed-overflow
         * UB on multiply (-1i16 * -1i16 promotes to int and 65535*65535 overflows
         * int). Route through `unsigned` so the math is modular regardless of
         * int's width, then truncate back to the FC type:
         *   (int16_t)(uint16_t)((unsigned)(uint16_t)a * (unsigned)(uint16_t)b)
         *
         * Signed result types at least as wide as int (int32/int64/isize) cast
         * through the unsigned counterpart so overflow wraps instead of being UB:
         *   (int32_t)((uint32_t)a + (uint32_t)b)
         *
         * Unsigned result types at least as wide as int (uint32/uint64/usize) are
         * already modular in C — emit plain (fall through). */
        if ((op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR) &&
            rt && type_is_integer(rt)) {
            const char *op_c = op == TOK_PLUS ? "+" : op == TOK_MINUS ? "-" : "*";
            if (type_is_subint(rt)) {
                const char *ut = unsigned_counterpart(rt);
                fprintf(out, "(");
                emit_type(rt, out);
                fprintf(out, ")");
                /* well-defined truncation before reinterpreting as signed */
                if (type_is_signed(rt)) fprintf(out, "(%s)", ut);
                fprintf(out, "((unsigned)(%s)", ut);
                emit_expr(e->binary.left, out);
                fprintf(out, " %s (unsigned)(%s)", op_c, ut);
                emit_expr(e->binary.right, out);
                fprintf(out, ")");
                goto _binary_done;
            }
            if (type_is_signed(rt)) {
                const char *ut = unsigned_counterpart(rt);
                fprintf(out, "(");
                emit_type(rt, out);
                fprintf(out, ")(((%s)", ut);
                emit_expr(e->binary.left, out);
                fprintf(out, ") %s ((%s)", op_c, ut);
                emit_expr(e->binary.right, out);
                fprintf(out, "))");
                goto _binary_done;
            }
            /* unsigned, width >= int: modular in C — fall through to plain emit */
        }

        /* Shift masking: a << (b & 31) for 32-bit, etc. */
        if ((op == TOK_LTLT || op == TOK_GTGT) && rt && type_is_integer(rt)) {
            int mask = shift_mask_for(rt);
            /* For platform-dependent types, emit a computed mask */
            const char *mask_expr = NULL;
            char mask_buf[64];
            if (mask < 0) {
                snprintf(mask_buf, sizeof(mask_buf), "((int)(sizeof(size_t)*8)-1)");
                mask_expr = mask_buf;
            }
            if (op == TOK_LTLT && (type_is_signed(rt) || type_is_subint(rt))) {
                /* Left shift through the unsigned counterpart, then cast back to
                 * the result type: makes signed shift well-defined (no overflow
                 * UB) and truncates narrow results that would otherwise promote
                 * to int (200u8 << 1 observes as 144, not 400). Wide unsigned
                 * types fall to the plain branch below — already full-width. */
                const char *ut = unsigned_counterpart(rt);
                fprintf(out, "(");
                emit_type(rt, out);
                fprintf(out, ")(((%s)", ut);
                emit_expr(e->binary.left, out);
                fprintf(out, ") << (");
                emit_expr(e->binary.right, out);
                if (mask_expr) fprintf(out, " & %s))", mask_expr);
                else fprintf(out, " & %d))", mask);
            } else {
                fprintf(out, "(");
                emit_expr(e->binary.left, out);
                fprintf(out, " %s (", op == TOK_LTLT ? "<<" : ">>");
                emit_expr(e->binary.right, out);
                if (mask_expr) fprintf(out, " & %s))", mask_expr);
                else fprintf(out, " & %d))", mask);
            }
            goto _binary_done;
        }

        /* Integer division/modulo: guard the divisor.
         *  - divide-by-zero: hard abort (a defined failure, like bounds/unwrap).
         *  - signed `min / -1` and `min % -1`: two's-complement overflow. C
         *    leaves it undefined and x86 traps it (SIGFPE) exactly like /0, so a
         *    plain divide would be UB — and the trap precedes any result, so a
         *    cast-back cannot fix it. FC defines signed overflow as wrapping (see
         *    spec "Integer overflow"), and `a / -1 == -a`, `a % -1 == 0` under
         *    modular arithmetic, so we substitute the wrapped value instead of
         *    dividing. The quotient is a wrapping negation through the unsigned
         *    counterpart (correct at every width, including `min`, and
         *    int-width-agnostic). Unsigned division never overflows, so the
         *    `== -1` arm is signed-only. */
        if ((op == TOK_SLASH || op == TOK_PERCENT) && rt && type_is_integer(rt)) {
            int tid = temp_counter++;
            const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
            int fn_len = (int)strlen(fn);
            int line = e->loc.line;
            bool wrap = type_is_signed(rt);
            /* Evaluate the dividend (lhs) first, then the divisor (rhs):
             * left-to-right, each exactly once.  Both are hoisted regardless of
             * the wrap path so the guard and result reference them by name. */
            fprintf(out, "({ ");
            emit_type(rt, out);
            fprintf(out, " _nv%d = ", tid);
            emit_expr(e->binary.left, out);
            fprintf(out, "; ");
            emit_type(rt, out);
            fprintf(out, " _dv%d = ", tid);
            emit_expr(e->binary.right, out);
            fprintf(out, "; if (_dv%d == 0) { fprintf(stderr, \"", tid);
            emit_c_escaped(fn, fn_len, out);
            fprintf(out, ":%d: %s by zero\\n\"); FC_ABORT(); } ",
                    line, op == TOK_SLASH ? "divide" : "modulo");
            if (wrap) {
                /* signed min / -1 and min % -1 overflow: substitute the wrapped
                 * value (a / -1 == -a, a % -1 == 0) instead of trapping. */
                const char *ut = unsigned_counterpart(rt);
                fprintf(out, "(_dv%d == -1) ? (", tid);
                emit_type(rt, out);
                if (op == TOK_SLASH) fprintf(out, ")(-(%s)_nv%d) : (", ut, tid);
                else                 fprintf(out, ")0 : (");
                emit_type(rt, out);
                fprintf(out, ")(_nv%d %s _dv%d); })",
                        tid, op == TOK_SLASH ? "/" : "%", tid);
            } else {
                fprintf(out, "_nv%d %s _dv%d; })",
                        tid, op == TOK_SLASH ? "/" : "%", tid);
            }
            goto _binary_done;
        }

        const char *op_str;
        switch (op) {
        case TOK_PLUS:     op_str = "+";  break;
        case TOK_MINUS:    op_str = "-";  break;
        case TOK_STAR:     op_str = "*";  break;
        case TOK_SLASH:    op_str = "/";  break;
        case TOK_PERCENT:  op_str = "%"; break;
        case TOK_EQEQ:    op_str = "=="; break;
        case TOK_BANGEQ:  op_str = "!="; break;
        case TOK_LT:      op_str = "<";  break;
        case TOK_GT:      op_str = ">";  break;
        case TOK_LTEQ:    op_str = "<="; break;
        case TOK_GTEQ:    op_str = ">="; break;
        case TOK_AMPAMP:  op_str = "&&"; break;
        case TOK_PIPEPIPE: op_str = "||"; break;
        case TOK_AMP:     op_str = "&";  break;
        case TOK_PIPE:    op_str = "|";  break;
        case TOK_CARET:   op_str = "^";  break;
        case TOK_LTLT:    op_str = "<<"; break;
        case TOK_GTGT:    op_str = ">>"; break;
        default: op_str = "?"; break;
        }
        fprintf(out, "(");
        emit_expr(e->binary.left, out);
        fprintf(out, " %s ", op_str);
        emit_expr(e->binary.right, out);
        fprintf(out, ")");
    _binary_done:
        if (_bseq) { fprintf(out, "; })"); seq_restore(_bslots, 2, _bsaved); }
        break;
    }

    case EXPR_UNARY_PREFIX: {
        /* Signed negation wrapping: (int32_t)(-(uint32_t)x) */
        if (e->unary_prefix.op == TOK_MINUS && e->type && type_is_signed(e->type)) {
            const char *ut = unsigned_counterpart(e->type);
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, ")(-((%s)", ut);
            emit_expr(e->unary_prefix.operand, out);
            fprintf(out, "))");
            break;
        }
        /* Bitwise NOT on sub-int types: cast result back to prevent C promotion issues */
        if (e->unary_prefix.op == TOK_TILDE && e->type && type_is_integer(e->type) &&
            (e->type->kind == TYPE_INT8  || e->type->kind == TYPE_UINT8 ||
             e->type->kind == TYPE_INT16 || e->type->kind == TYPE_UINT16)) {
            fprintf(out, "((");
            emit_type(e->type, out);
            fprintf(out, ")(~");
            emit_expr(e->unary_prefix.operand, out);
            fprintf(out, "))");
            break;
        }
        /* &f on a top-level function or non-capturing lambda: emit the raw
         * C-boundary trampoline instead of the address of a fat-pointer literal.
         * This is the C-interop escape hatch documented in the spec. */
        if (e->unary_prefix.op == TOK_AMP) {
            Expr *operand = e->unary_prefix.operand;
            if (operand->kind == EXPR_IDENT && !operand->ident.is_local &&
                operand->type && operand->type->kind == TYPE_FUNC) {
                const char *name = operand->ident.codegen_name
                    ? operand->ident.codegen_name : operand->ident.name;
                fprintf(out, "_ctramp_%s", name);
                break;
            }
            if (operand->kind == EXPR_FIELD && operand->field.codegen_name &&
                operand->type && operand->type->kind == TYPE_FUNC) {
                fprintf(out, "_ctramp_%s", operand->field.codegen_name);
                break;
            }
            if (operand->kind == EXPR_FUNC && operand->func.capture_count == 0 &&
                operand->func.lifted_name && operand->type &&
                operand->type->kind == TYPE_FUNC) {
                fprintf(out, "_ctramp_%s", operand->func.lifted_name);
                break;
            }
        }
        const char *op_str;
        switch (e->unary_prefix.op) {
        case TOK_MINUS: op_str = "-"; break;
        case TOK_BANG:  op_str = "!"; break;
        case TOK_TILDE: op_str = "~"; break;
        case TOK_AMP:   op_str = "&"; break;
        case TOK_STAR:  op_str = "*"; break;
        default: op_str = "?"; break;
        }
        fprintf(out, "(%s", op_str);
        emit_expr(e->unary_prefix.operand, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_UNARY_POSTFIX: {
        if (e->unary_postfix.op == TOK_BANG) {
            /* Option unwrap: check has_value, abort with diagnostic if none */
            Type *opt_type = e->unary_postfix.operand->type;
            const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
            int fn_len = (int)strlen(fn);
            int line = e->loc.line;
            if (is_null_sentinel(opt_type)) {
                /* T*? → plain pointer, unwrap = null check */
                fprintf(out, "({ ");
                emit_type(opt_type->option.inner, out);
                int tid = temp_counter++;
                fprintf(out, " _uw%d = ", tid);
                emit_expr(e->unary_postfix.operand, out);
                fprintf(out, "; if (!_uw%d) { fprintf(stderr, \"", tid);
                emit_c_escaped(fn, fn_len, out);
                fprintf(out, ":%d: unwrap failed: ", line);
                if (e->unary_postfix.expr_text)
                    emit_c_escaped(e->unary_postfix.expr_text,
                                   e->unary_postfix.expr_text_len, out);
                fprintf(out, "\\n\"); FC_ABORT(); } _uw%d; })", tid);
            } else {
                /* Non-pointer option: check .has_value */
                fprintf(out, "({ ");
                emit_type(opt_type, out);
                int tid = temp_counter++;
                fprintf(out, " _uw%d = ", tid);
                emit_expr(e->unary_postfix.operand, out);
                fprintf(out, "; if (!_uw%d.has_value) { fprintf(stderr, \"", tid);
                emit_c_escaped(fn, fn_len, out);
                fprintf(out, ":%d: unwrap failed: ", line);
                if (e->unary_postfix.expr_text)
                    emit_c_escaped(e->unary_postfix.expr_text,
                                   e->unary_postfix.expr_text_len, out);
                fprintf(out, "\\n\"); FC_ABORT(); } _uw%d.value; })", tid);
            }
        }
        break;
    }

    case EXPR_CALL: {
        /* Check if this is a variant constructor: type is union and func is EXPR_FIELD */
        if (e->call.func->kind == EXPR_FIELD &&
            e->call.func->field.is_variant_constructor) {
            const char *union_name = e->type->unio.name;
            /* Under substitution, compute mangled name for generic unions */
            if (g_subst && type_contains_type_var(e->type)) {
                union_name = mangle_generic_with_subst(union_name, e->type);
            }
            const char *variant_name = e->call.func->field.name;
            fprintf(out, "(%s){ .tag = %s_tag_%s, .%s = ",
                union_name, union_name, variant_name,
                c_safe_ident(g_intern, variant_name));
            emit_expr(e->call.args[0], out);
            fprintf(out, " }");
            break;
        }

        /* Get callee function type for coercion */
        Type *call_ft = e->call.func->type;

        /* Force left-to-right argument evaluation when any argument has side
         * effects: evaluate the earlier arguments into temps in source order
         * (the callee is evaluated first, the final argument last). */
        Expr ***aslots = NULL; Expr *ascratch = NULL; Expr **asaved = NULL;
        bool aseq = !g_const_context && e->call.arg_count >= 2;
        if (aseq) {
            aslots = arena_alloc(g_arena, sizeof(Expr**) * (size_t)e->call.arg_count);
            for (int i = 0; i < e->call.arg_count; i++) aslots[i] = &e->call.args[i];
            aseq = seq_needed(aslots, e->call.arg_count);
        }
        if (aseq) {
            ascratch = arena_alloc(g_arena, sizeof(Expr) * (size_t)e->call.arg_count);
            asaved = arena_alloc(g_arena, sizeof(Expr*) * (size_t)e->call.arg_count);
        }

        if (e->call.is_indirect) {
            /* Indirect call through fat pointer */
            int tid = temp_counter++;
            fprintf(out, "({ ");
            emit_type(call_ft, out);
            fprintf(out, " _cf%d = ", tid);
            emit_expr(e->call.func, out);
            fprintf(out, "; ");
            if (aseq) seq_hoist(aslots, e->call.arg_count, ascratch, asaved, out);
            fprintf(out, "_cf%d.fn_ptr(", tid);
            for (int i = 0; i < e->call.arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_expr(e->call.args[i], out);
            }
            if (e->call.arg_count > 0) fprintf(out, ", ");
            fprintf(out, "_cf%d.ctx); })", tid);
            if (aseq) seq_restore(aslots, e->call.arg_count, asaved);
        } else {
            if (aseq) {
                fprintf(out, "({ ");
                seq_hoist(aslots, e->call.arg_count, ascratch, asaved, out);
            }
            /* Direct call — emit function name directly (not via emit_expr
               to avoid fat-pointer wrapping) and append NULL for _ctx */
            Expr *callee = e->call.func;
            const char *fn_name = e->call.mangled_name; /* monomorphized name if generic */

            /* Resolve deferred generic call under substitution context */
            if (!fn_name && g_subst && e->call.type_arg_count > 0) {
                Type **concrete_args = malloc(sizeof(Type*) * (size_t)e->call.type_arg_count);
                for (int i = 0; i < e->call.type_arg_count; i++) {
                    concrete_args[i] = type_substitute(g_arena, e->call.type_args[i],
                        g_subst->var_names, g_subst->concrete, g_subst->count);
                }
                /* Use resolved_callee from pass2 — always set for all call patterns
                   (single-level and multi-level qualified calls) */
                const char *base_name = NULL;
                Symbol *callee_sym = e->call.resolved_callee;
                if (callee_sym) {
                    base_name = (callee_sym->decl && callee_sym->decl->kind == DECL_LET
                                 && callee_sym->decl->let.codegen_name)
                                ? callee_sym->decl->let.codegen_name : callee_sym->name;
                }
                if (base_name && callee_sym) {
                    fn_name = mono_register(g_mono, g_arena, g_intern,
                        base_name, NULL, concrete_args, e->call.type_arg_count,
                        callee_sym->decl, DECL_LET,
                        callee_sym->type_params, callee_sym->type_param_count);
                }
                free(concrete_args);
            }

            if (!fn_name) {
                if (callee->kind == EXPR_IDENT) {
                    fn_name = callee->ident.codegen_name
                        ? callee->ident.codegen_name : callee->ident.name;
                } else if (callee->kind == EXPR_FIELD && callee->field.codegen_name) {
                    fn_name = callee->field.codegen_name;
                }
            }

            if (fn_name && !e->call.is_extern_call) {
                fprintf(out, "%s(", fn_name);
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    emit_expr(e->call.args[i], out);
                }
                if (e->call.arg_count > 0) fprintf(out, ", ");
                fprintf(out, "NULL)");
            } else if (fn_name) {
                /* Extern call: cast cstr-aliased params to const char* for C headers,
                 * and cast cstr/cstr? return values from char* back to uint8_t* */
                Type *ret_type = (call_ft && call_ft->kind == TYPE_FUNC)
                    ? call_ft->func.return_type : NULL;
                bool ret_is_cstr = ret_type && is_cstr_type(ret_type);
                bool ret_is_cstr_opt = ret_type && ret_type->kind == TYPE_OPTION &&
                    is_cstr_type(ret_type->option.inner);
                bool ret_is_cstr_ptr = ret_type && ret_type->kind == TYPE_POINTER &&
                    is_cstr_type(ret_type->pointer.pointee);
                if (ret_is_cstr || ret_is_cstr_opt)
                    fprintf(out, "(uint8_t*)");
                else if (ret_is_cstr_ptr)
                    fprintf(out, "(uint8_t**)");
                fprintf(out, "%s(", fn_name);
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    Type *pt = (call_ft && call_ft->kind == TYPE_FUNC && i < call_ft->func.param_count)
                        ? call_ft->func.param_types[i] : NULL;
                    emit_extern_arg(e->call.args[i], pt, out);
                }
                fprintf(out, ")");
            } else {
                /* Fallback: emit normally */
                emit_expr(e->call.func, out);
                fprintf(out, "(");
                for (int i = 0; i < e->call.arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    emit_expr(e->call.args[i], out);
                }
                fprintf(out, ")");
            }
            if (aseq) {
                fprintf(out, "; })");
                seq_restore(aslots, e->call.arg_count, asaved);
            }
        }
        break;
    }

    case EXPR_IF: {
        bool then_div = type_is_never(e->if_expr.then_body->type);
        bool else_div = e->if_expr.else_body && type_is_never(e->if_expr.else_body->type);
        if (type_valueless(e->type)) {
            /* Void- or never-typed if → C if statement. Any control-flow exits in
               the branches fire inside; nothing is produced as a value. */
            emit_if_stmt(e, out);
        } else if (e->if_expr.else_body && !then_div && !else_div) {
            /* Value if/then/else, both branches produce values → ternary */
            fprintf(out, "(");
            emit_expr(e->if_expr.cond, out);
            fprintf(out, " ? ");
            emit_expr(e->if_expr.then_body, out);
            fprintf(out, " : ");
            emit_expr(e->if_expr.else_body, out);
            fprintf(out, ")");
        } else {
            /* Value if where one branch diverges (return/break/continue): a ternary
               can't hold a statement, so use a statement-expression with a result
               temp. The value branch assigns it; the diverging branch emits as
               statements (its exit fires before the temp is read). */
            int res_id = temp_counter++;
            char res_var[32];
            snprintf(res_var, sizeof(res_var), "_ifres%d", res_id);
            fprintf(out, "({\n");
            indent_level++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " %s;\n", res_var);
            emit_indent(out);
            if (emit_self_parens(e->if_expr.cond)) {
                fprintf(out, "if ");
                emit_expr(e->if_expr.cond, out);
                fprintf(out, " {\n");
            } else {
                fprintf(out, "if (");
                emit_expr(e->if_expr.cond, out);
                fprintf(out, ") {\n");
            }
            indent_level++;
            emit_branch_into(e->if_expr.then_body, res_var, out);
            indent_level--;
            emit_indent(out);
            fprintf(out, "} else {\n");
            indent_level++;
            emit_branch_into(e->if_expr.else_body, res_var, out);
            indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
            emit_indent(out);
            fprintf(out, "%s;\n", res_var);
            indent_level--;
            emit_indent(out);
            fprintf(out, "})");
        }
        break;
    }

    case EXPR_BLOCK: {
        /* Statement expression */
        fprintf(out, "({\n");
        indent_level++;
        defer_scope_push(false);
        emit_block_stmts(e->block.stmts, e->block.count, out, false, false);
        defer_scope_pop();
        indent_level--;
        emit_indent(out);
        fprintf(out, "})");
        break;
    }

    case EXPR_CAST: {
        F2iInfo f2i_info;
        /* str -> cstr: stack copy with null terminator */
        if (e->cast.operand->type && is_str_type(e->cast.operand->type) &&
            is_cstr_type(e->cast.target)) {
            int tid = temp_counter++;
            if (e->cast.buffer_size > 0) {
                /* (cstr[N]) — truncating copy into a fixed N-byte backing array
                 * hoisted to function entry: bounded, loop-safe. Copy at most
                 * N-1 bytes, always NUL-terminate. */
                int n = e->cast.buffer_size;
                const char *bk = e->cast.codegen_backing_name;
                fprintf(out, "({ fc_str _sc%d = ", tid);
                emit_expr(e->cast.operand, out);
                fprintf(out, "; int64_t _cn%d = _sc%d.len < %d ? _sc%d.len : %d",
                        tid, tid, n - 1, tid, n - 1);
                fprintf(out, "; memcpy(%s, _sc%d.ptr, fc_to_size(_cn%d))", bk, tid, tid);
                fprintf(out, "; %s[_cn%d] = '\\0'; (uint8_t*)%s; })", bk, tid, bk);
            } else {
                /* Unbounded (cstr) — rejected in pass2; retained only as a fallback
                 * during staging. */
                fprintf(out, "({ fc_str _sc%d = ", tid);
                emit_expr(e->cast.operand, out);
                fprintf(out, "; uint8_t *_cb%d = (uint8_t*)__builtin_alloca(fc_to_size(_sc%d.len + 1))", tid, tid);
                fprintf(out, "; memcpy(_cb%d, _sc%d.ptr, fc_to_size(_sc%d.len))", tid, tid, tid);
                fprintf(out, "; _cb%d[_sc%d.len] = '\\0'; (uint8_t*)_cb%d; })", tid, tid, tid);
            }
        /* cstr -> str: wrap pointer with strlen-computed length */
        } else if (e->cast.operand->type && is_cstr_type(e->cast.operand->type) &&
                   is_str_type(e->cast.target)) {
            int tid = temp_counter++;
            bool src_const = e->cast.operand->type->is_const;
            fprintf(out, "({ %suint8_t *_cp%d = ", src_const ? "const " : "", tid);
            emit_expr(e->cast.operand, out);
            fprintf(out, "; (fc_str){ .ptr = %s_cp%d, .len = (int64_t)strlen((const char*)_cp%d) }; })",
                    src_const ? "(uint8_t*)" : "", tid, tid);
        } else if (e->cast.operand->type && type_is_float(e->cast.operand->type) &&
                   float_to_int_info(e->cast.target->kind, &f2i_info)) {
            /* float -> int: saturating conversion (NaN->0, clamp to range, else
             * truncate toward zero). A raw C cast here is UB out of range. */
            float_to_int_emit(&f2i_info, e->cast.operand, out);
        } else {
            fprintf(out, "((");
            emit_type(e->cast.target, out);
            fprintf(out, ")");
            emit_expr(e->cast.operand, out);
            fprintf(out, ")");
        }
        break;
    }

    case EXPR_FIELD: {
        /* Module member access: emit mangled name directly */
        if (e->field.codegen_name) {
            if (e->type && e->type->kind == TYPE_FUNC) {
                /* Module function used as a value — wrap in fat pointer */
                fprintf(out, "(");
                emit_type(e->type, out);
                fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                    e->field.codegen_name);
            } else if (is_cstr_type(e->type)) {
                /* C string #defines are char*; FC cstr is uint8_t* — cast at boundary.
                 * Outer parens are required so a following postfix (e.g. `c.s[0]`)
                 * binds to the cast result, not the raw #define literal. */
                fprintf(out, "((%s uint8_t*)(%s))",
                    (e->type->is_const ? "const" : ""), e->field.codegen_name);
            } else {
                fprintf(out, "%s", e->field.codegen_name);
            }
            break;
        }
        /* No-payload variant constructor: color.green → (color){ .tag = color_tag_green } */
        if (e->field.is_variant_constructor) {
            const char *union_name = e->type->unio.name;
            if (g_subst && type_contains_type_var(e->type)) {
                union_name = mangle_generic_with_subst(union_name, e->type);
            }
            fprintf(out, "(%s){ .tag = %s_tag_%s }",
                union_name, union_name, e->field.name);
            break;
        }
        /* Type variable property access: 'a.min → resolve via g_subst */
        if (e->field.object->kind == EXPR_TYPE_VAR_REF && g_subst) {
            const char *tv_name = e->field.object->type_var_ref.name;
            Type *concrete = NULL;
            for (int i = 0; i < g_subst->count; i++) {
                if (g_subst->var_names[i] == tv_name) {
                    concrete = g_subst->concrete[i];
                    break;
                }
            }
            if (concrete) {
                const char *cstr = resolve_type_prop_codegen(concrete, e->field.name);
                if (cstr) {
                    fprintf(out, "%s", cstr);
                    break;
                }
                diag_error(e->loc, "type '%s' has no property '%s'",
                    type_name(concrete), e->field.name);
                fprintf(out, "0 /* error */");
                break;
            }
        }
        /* Fixed-array field: create slice view */
        if (e->field.fixed_array_type) {
            Type *fat = e->field.fixed_array_type;
            /* Strip const from emitted slice type — FC enforces const at type-check level */
            Type *slice_type = e->type;
            if (slice_type->is_const) {
                slice_type = type_slice(g_arena, fat->fixed_array.elem);
            }
            fprintf(out, "(");
            emit_type(slice_type, out);
            fprintf(out, "){ .ptr = (");
            emit_type(fat->fixed_array.elem, out);
            fprintf(out, "*)");
            emit_expr(e->field.object, out);
            fprintf(out, ".%s, .len = %lld }", c_safe_ident(g_intern, e->field.name),
                    (long long)fat->fixed_array.size);
            break;
        }
        /* Option .is_some / .is_none synthetic fields */
        if (e->field.object->type && e->field.object->type->kind == TYPE_OPTION) {
            Type *opt_type = e->field.object->type;
            if (strcmp(e->field.name, "is_some") == 0) {
                if (is_null_sentinel(opt_type)) {
                    fprintf(out, "(");
                    emit_expr(e->field.object, out);
                    fprintf(out, " != NULL)");
                } else {
                    emit_expr(e->field.object, out);
                    fprintf(out, ".has_value");
                }
                break;
            }
            if (strcmp(e->field.name, "is_none") == 0) {
                if (is_null_sentinel(opt_type)) {
                    fprintf(out, "(");
                    emit_expr(e->field.object, out);
                    fprintf(out, " == NULL)");
                } else {
                    fprintf(out, "(!");
                    emit_expr(e->field.object, out);
                    fprintf(out, ".has_value)");
                }
                break;
            }
        }
        emit_expr(e->field.object, out);
        fprintf(out, ".%s", c_safe_ident(g_intern, e->field.name));
        break;
    }

    case EXPR_DEREF_FIELD: {
        if (e->field.fixed_array_type) {
            Type *fat = e->field.fixed_array_type;
            /* Strip const from emitted slice type — FC enforces const at type-check level */
            Type *slice_type = e->type;
            if (slice_type->is_const) {
                slice_type = type_slice(g_arena, fat->fixed_array.elem);
            }
            fprintf(out, "(");
            emit_type(slice_type, out);
            fprintf(out, "){ .ptr = (");
            emit_type(fat->fixed_array.elem, out);
            fprintf(out, "*)");
            emit_expr(e->field.object, out);
            fprintf(out, "->%s, .len = %lld }", c_safe_ident(g_intern, e->field.name),
                    (long long)fat->fixed_array.size);
            break;
        }
        emit_expr(e->field.object, out);
        fprintf(out, "->%s", c_safe_ident(g_intern, e->field.name));
        break;
    }

    case EXPR_INDEX: {
        /* Bounds check for slices — returns lvalue via pointer dereference.
         * Unsigned-compare fusion: casting a negative signed index to uint64_t
         * produces a huge value that compares greater than any valid len
         * (slice lengths are non-negative by invariant), so one compare
         * subsumes both the negative-index and past-end checks. */
        Type *obj_type = e->index.object->type;
        /* Tuple element access: object.e<const>. The index was proven to be a
         * literal in range during pass2, so no runtime bounds check is needed. */
        if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struc.is_tuple) {
            emit_expr(e->index.object, out);
            fprintf(out, ".e%llu", (unsigned long long)e->index.index->int_lit.value);
            break;
        }
        if (obj_type && obj_type->kind == TYPE_SLICE) {
            int tid = temp_counter++;
            const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
            int fn_len = (int)strlen(fn);
            int line = e->loc.line;
            fprintf(out, "(*({ ");
            emit_type(obj_type, out);
            fprintf(out, " _s%d = ", tid);
            emit_expr(e->index.object, out);
            fprintf(out, "; int64_t _i%d = (int64_t)", tid);
            emit_expr(e->index.index, out);
            fprintf(out, "; if (__builtin_expect((uint64_t)_i%d >= (uint64_t)_s%d.len, 0)) "
                         "fc_oob(\"", tid, tid);
            emit_c_escaped(fn, fn_len, out);
            fprintf(out, "\", %d, (long long)_i%d, (long long)_s%d.len); "
                         "_s%d.ptr + _i%d; }))",
                    line, tid, tid, tid, tid);
        } else {
            /* Pointer indexing — no bounds check */
            emit_expr(e->index.object, out);
            fprintf(out, "[");
            emit_expr(e->index.index, out);
            fprintf(out, "]");
        }
        break;
    }

    case EXPR_SLICE: {
        /* Subslice: s[lo..hi].  Unsigned-compare fusion covers negative lo/hi
         * and reversed ranges in a single pair of compares:
         *   (uint64_t)lo > (uint64_t)hi  catches lo>hi and (if both negative)
         *     any case where |lo| < |hi|.
         *   (uint64_t)hi > (uint64_t)s.len catches past-end and negative hi
         *     (negative cast to unsigned is huge). */
        int tid = temp_counter++;
        Type *obj_type = e->slice.object->type;
        const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
        int fn_len = (int)strlen(fn);
        int line = e->loc.line;
        fprintf(out, "({ ");
        emit_type(obj_type, out);
        fprintf(out, " _s%d = ", tid);
        emit_expr(e->slice.object, out);
        fprintf(out, "; int64_t _lo%d = ", tid);
        if (e->slice.lo) {
            fprintf(out, "(int64_t)");
            emit_expr(e->slice.lo, out);
        } else {
            fprintf(out, "0");
        }
        fprintf(out, "; int64_t _hi%d = ", tid);
        if (e->slice.hi) {
            fprintf(out, "(int64_t)");
            emit_expr(e->slice.hi, out);
        } else {
            fprintf(out, "_s%d.len", tid);
        }
        fprintf(out, "; if (__builtin_expect("
                     "(uint64_t)_lo%d > (uint64_t)_hi%d || "
                     "(uint64_t)_hi%d > (uint64_t)_s%d.len, 0)) "
                     "fc_oob_sub(\"",
            tid, tid, tid, tid);
        emit_c_escaped(fn, fn_len, out);
        fprintf(out, "\", %d, (long long)_lo%d, (long long)_hi%d, (long long)_s%d.len); ",
                line, tid, tid, tid);
        fprintf(out, "(");
        emit_type(obj_type, out);
        fprintf(out, "){ .ptr = _s%d.ptr + _lo%d, .len = _hi%d - _lo%d }; })",
            tid, tid, tid, tid);
        break;
    }

    case EXPR_SOME: {
        if (is_null_sentinel(e->type)) {
            /* T*?/any*?/cstr? → plain pointer, some(x) = x.  null would be
             * indistinguishable from none, so guard a not-provably-non-null
             * payload: evaluate once into a temp and abort if it is null.
             * Elided when the payload is provably non-null, and in const context
             * (a file-scope initializer cannot contain a statement-expression;
             * pass2 guarantees only provably-non-null payloads reach here). */
            Expr *val = e->some_expr.value;
            if (g_const_context || ptr_value_provably_nonnull(val)) {
                emit_expr(val, out);
            } else {
                int tid = temp_counter++;
                const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
                int fn_len = (int)strlen(fn);
                fprintf(out, "({ ");
                emit_type(e->type->option.inner, out);
                fprintf(out, " _sm%d = ", tid);
                emit_expr(val, out);
                fprintf(out, "; if (__builtin_expect(_sm%d == NULL, 0)) fc_null_some(\"",
                        tid);
                emit_c_escaped(fn, fn_len, out);
                fprintf(out, "\", %d); _sm%d; })", e->loc.line, tid);
            }
        } else {
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .value = ");
            emit_expr(e->some_expr.value, out);
            fprintf(out, ", .has_value = true }");
        }
        break;
    }

    case EXPR_ARRAY_LIT: {
        /* Module-scope const context: emit a slice header referencing the
         * static backing array that the pre-pass lifted to file scope.
         * Empty arrays have no backing (C rejects zero-length initializers);
         * emit a NULL/0 slice instead. */
        if (g_const_context) {
            fprintf(out, "(");
            emit_type(e->type, out);
            if (e->array_lit.elem_count == 0 || !e->array_lit.codegen_backing_name) {
                fprintf(out, "){ .ptr = 0, .len = ");
                emit_expr(e->array_lit.size_expr, out);
                fprintf(out, " }");
            } else {
                fprintf(out, "){ .ptr = %s, .len = ",
                        e->array_lit.codegen_backing_name);
                emit_expr(e->array_lit.size_expr, out);
                fprintf(out, " }");
            }
            break;
        }
        /* Stack array literal → slice over backing storage.
         *
         * The common case uses a fixed C array hoisted to function entry
         * (codegen_backing_name, set during the hoist pass): its single slot is
         * reused on every loop iteration, so stack use is bounded.  Zero-length
         * literals (and any not reached by the hoist pass) fall back to
         * __builtin_alloca.  Both give the backing function-frame lifetime — the
         * memory must outlive this statement-expression because the produced
         * slice escapes it (e.g. into a `let` binding used later). */
        int tid = temp_counter++;
        const char *bk = e->array_lit.codegen_backing_name;
        char arrname[24];
        const char *tgt;
        fprintf(out, "({ ");
        if (bk) {
            tgt = bk;
        } else {
            snprintf(arrname, sizeof arrname, "_arr%d", tid);
            tgt = arrname;
            emit_type(e->array_lit.elem_type, out);
            fprintf(out, " *%s = (", tgt);
            emit_type(e->array_lit.elem_type, out);
            fprintf(out, "*)__builtin_alloca(fc_to_size(");
            emit_expr(e->array_lit.size_expr, out);
            fprintf(out, ") * sizeof(");
            emit_type(e->array_lit.elem_type, out);
            fprintf(out, ")); ");
        }
        if (e->array_lit.elem_count == 0) {
            if (bk) {
                /* backing is a real array → sizeof yields its byte count */
                fprintf(out, "memset(%s, 0, sizeof %s); ", tgt, tgt);
            } else {
                fprintf(out, "memset(%s, 0, fc_to_size(", tgt);
                emit_expr(e->array_lit.size_expr, out);
                fprintf(out, ") * sizeof(");
                emit_type(e->array_lit.elem_type, out);
                fprintf(out, ")); ");
            }
        } else {
            for (int i = 0; i < e->array_lit.elem_count; i++) {
                fprintf(out, "%s[%d] = ", tgt, i);
                emit_expr(e->array_lit.elems[i], out);
                fprintf(out, "; ");
            }
        }
        fprintf(out, "(");
        emit_type(e->type, out);
        fprintf(out, "){ .ptr = %s, .len = ", tgt);
        emit_expr(e->array_lit.size_expr, out);
        fprintf(out, " }; })");
        break;
    }

    case EXPR_SLICE_LIT: {
        /* Slice construction from raw parts: (fc_slice_T){ .ptr = ptr, .len = len }
         *
         * A slice's length must be non-negative: the index/subslice bounds
         * checks fuse the negative-index test into an unsigned compare
         * ((uint64_t)idx >= (uint64_t)len), which a negative len silently
         * defeats.  When pass2 could not prove len >= 0, guard a runtime len so
         * a negative value aborts at construction instead of corrupting every
         * later access.  Skipped in const context (file-scope initializer: len
         * is a pass2-verified constant, and a function call is not a constant
         * expression) and when pass2 proved len non-negative. */
        bool guard = !g_const_context && !e->slice_lit.len_nonneg;
        Type *ptr_type = e->slice_lit.ptr_expr->type;
        bool const_ptr = ptr_type && ptr_type->kind == TYPE_POINTER && ptr_type->is_const;
        /* The guard hoists len into a temp; to keep ptr-before-len source order
         * (left-to-right) we then hoist ptr first.  Also sequence when both
         * operands have side effects even without the guard. */
        bool seq = guard || (!g_const_context &&
                             expr_has_side_effects(e->slice_lit.ptr_expr) &&
                             expr_has_side_effects(e->slice_lit.len_expr));
        if (seq) {
            int tid = temp_counter++;
            fprintf(out, "({ ");
            /* ptr first (left-to-right) */
            emit_type(e->slice_lit.elem_type, out);
            fprintf(out, " *_sp%d = ", tid);
            if (const_ptr) {
                fprintf(out, "(");
                emit_type(e->slice_lit.elem_type, out);
                fprintf(out, "*)");
            }
            emit_expr(e->slice_lit.ptr_expr, out);
            fprintf(out, "; int64_t _sll%d = (", tid);
            emit_expr(e->slice_lit.len_expr, out);
            fprintf(out, "); ");
            if (guard) {
                const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
                int fn_len = (int)strlen(fn);
                fprintf(out, "if (__builtin_expect(_sll%d < 0, 0)) fc_neg_len(\"", tid);
                emit_c_escaped(fn, fn_len, out);
                fprintf(out, "\", %d, (long long)_sll%d); ", e->loc.line, tid);
            }
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .ptr = _sp%d, .len = _sll%d }; })", tid, tid);
            break;
        }
        fprintf(out, "(");
        emit_type(e->type, out);
        fprintf(out, "){ .ptr = ");
        /* Cast away const if source pointer is const (FC tracks constness at slice level) */
        if (const_ptr) {
            fprintf(out, "(");
            emit_type(e->slice_lit.elem_type, out);
            fprintf(out, "*)");
        }
        emit_expr(e->slice_lit.ptr_expr, out);
        fprintf(out, ", .len = ");
        emit_expr(e->slice_lit.len_expr, out);
        fprintf(out, " }");
        break;
    }

    case EXPR_STRUCT_LIT: {
        /* Use the resolved type name (handles mangled module types) */
        const char *sname = (e->type && e->type->kind == TYPE_STRUCT) ?
            e->type->struc.name : e->struct_lit.type_name;
        /* Under substitution, compute mangled name for generic structs */
        if (g_subst && e->type && e->type->kind == TYPE_STRUCT &&
            type_contains_type_var(e->type)) {
            sname = mangle_generic_with_subst(sname, e->type);
        }
        /* Check if struct has any fixed-array fields — requires statement expression */
        bool has_fixed_array = false;
        Type *st = e->type;
        if (st && st->kind == TYPE_STRUCT) {
            for (int f = 0; f < st->struc.field_count; f++) {
                if (st->struc.fields[f].type->kind == TYPE_FIXED_ARRAY) {
                    has_fixed_array = true;
                    break;
                }
            }
        }
        /* Force left-to-right field evaluation when a field has side effects (C
         * leaves initializer-list order unspecified).  Only the plain
         * compound-literal path needs this: the fixed-array paths already
         * assign field-by-field in order, and const context has no effects. */
        int sln = e->struct_lit.field_count;
        Expr ***slslots = NULL; Expr *slscratch = NULL; Expr **slsaved = NULL;
        bool slseq = !has_fixed_array && !g_const_context && sln >= 2;
        if (slseq) {
            slslots = arena_alloc(g_arena, sizeof(Expr**) * (size_t)sln);
            for (int i = 0; i < sln; i++) slslots[i] = &e->struct_lit.fields[i].value;
            slseq = seq_needed(slslots, sln);
        }
        if (slseq) {
            slscratch = arena_alloc(g_arena, sizeof(Expr) * (size_t)sln);
            slsaved = arena_alloc(g_arena, sizeof(Expr*) * (size_t)sln);
            fprintf(out, "({ ");
            seq_hoist(slslots, sln, slscratch, slsaved, out);
        }
        if (has_fixed_array && g_const_context) {
            /* File-scope aggregate initializer: emit each field inline.  For
             * a fixed-array field we expect the value to be an EXPR_ARRAY_LIT
             * and we unwrap it to a bare { e0, e1, ... } C array initializer
             * — the fat-slice header form is invalid for initializing a raw
             * C array.  len overflow is checked at compile time here. */
            if (st->struc.c_name) {
                fprintf(out, "(%s %s){",
                    st->struc.is_c_union ? "union" : "struct", st->struc.c_name);
            } else {
                fprintf(out, "(%s){", sname);
            }
            bool first = true;
            for (int i = 0; i < e->struct_lit.field_count; i++) {
                const char *fname = e->struct_lit.fields[i].name;
                Type *field_type = NULL;
                for (int f = 0; f < st->struc.field_count; f++) {
                    if (st->struc.fields[f].name == fname) {
                        field_type = st->struc.fields[f].type;
                        break;
                    }
                }
                if (!first) fprintf(out, ", ");
                first = false;
                fprintf(out, ".%s = ", c_safe_ident(g_intern, fname));
                Expr *v = e->struct_lit.fields[i].value;
                if (field_type && field_type->kind == TYPE_FIXED_ARRAY &&
                    v && v->kind == EXPR_ARRAY_LIT) {
                    /* Bare aggregate — no slice header, no backing */
                    fprintf(out, "{");
                    if (v->array_lit.elem_count == 0) {
                        fprintf(out, "0");
                    } else {
                        for (int j = 0; j < v->array_lit.elem_count; j++) {
                            if (j > 0) fprintf(out, ", ");
                            emit_expr(v->array_lit.elems[j], out);
                        }
                    }
                    fprintf(out, "}");
                } else {
                    emit_expr(v, out);
                }
            }
            fprintf(out, "}");
            break;
        }
        if (has_fixed_array) {
            int tid = temp_counter++;
            fprintf(out, "({ ");
            if (st->struc.c_name) {
                fprintf(out, "%s %s", st->struc.is_c_union ? "union" : "struct",
                    st->struc.c_name);
            } else {
                fprintf(out, "%s", sname);
            }
            fprintf(out, " _sl%d = {0}; ", tid);
            for (int i = 0; i < e->struct_lit.field_count; i++) {
                const char *fname = e->struct_lit.fields[i].name;
                /* Find the field type in the struct */
                Type *field_type = NULL;
                for (int f = 0; f < st->struc.field_count; f++) {
                    if (st->struc.fields[f].name == fname) {
                        field_type = st->struc.fields[f].type;
                        break;
                    }
                }
                if (field_type && field_type->kind == TYPE_FIXED_ARRAY) {
                    int sid = temp_counter++;
                    SrcLoc vloc = e->struct_lit.fields[i].value->loc;
                    const char *vfn = vloc.filename ? vloc.filename : "<unknown>";
                    int vfn_len = (int)strlen(vfn);
                    emit_type(e->struct_lit.fields[i].value->type, out);
                    fprintf(out, " _fas%d = ", sid);
                    emit_expr(e->struct_lit.fields[i].value, out);
                    fprintf(out, "; if (_fas%d.len > %lld) { fprintf(stderr, \"", sid,
                            (long long)field_type->fixed_array.size);
                    emit_c_escaped(vfn, vfn_len, out);
                    fprintf(out, ":%d: fixed-array field '%s' overflow: "
                                 "len=%%lld capacity=%lld\\n\", "
                                 "(long long)_fas%d.len); FC_ABORT(); } ",
                            vloc.line, fname, (long long)field_type->fixed_array.size,
                            sid);
                    fprintf(out, "memcpy(_sl%d.%s, _fas%d.ptr, fc_to_size(_fas%d.len) * sizeof(",
                            tid, c_safe_ident(g_intern, fname), sid, sid);
                    emit_type(field_type->fixed_array.elem, out);
                    fprintf(out, ")); ");
                } else {
                    fprintf(out, "_sl%d.%s = ", tid, c_safe_ident(g_intern, fname));
                    emit_expr(e->struct_lit.fields[i].value, out);
                    fprintf(out, "; ");
                }
            }
            fprintf(out, "_sl%d; })", tid);
        } else {
            /* Multi-line when 2+ fields: each field on its own line so nested
             * statement-expressions (e.g. alloc(...)!) don't concatenate into
             * one huge line that trips gcc's column-tracking limit. */
            bool multiline = e->struct_lit.field_count >= 2;
            if (st && st->kind == TYPE_STRUCT && st->struc.c_name) {
                fprintf(out, "(%s %s){", st->struc.is_c_union ? "union" : "struct",
                    st->struc.c_name);
            } else {
                fprintf(out, "(%s){", sname);
            }
            if (multiline) {
                fprintf(out, "\n");
                indent_level++;
            } else {
                fprintf(out, " ");
            }
            for (int i = 0; i < e->struct_lit.field_count; i++) {
                if (i > 0) {
                    fprintf(out, ",");
                    if (multiline) fprintf(out, "\n");
                    else fprintf(out, " ");
                }
                if (multiline) emit_indent(out);
                fprintf(out, ".%s = ", c_safe_ident(g_intern, e->struct_lit.fields[i].name));
                emit_expr(e->struct_lit.fields[i].value, out);
            }
            if (multiline) {
                fprintf(out, "\n");
                indent_level--;
                emit_indent(out);
                fprintf(out, "}");
            } else {
                fprintf(out, " }");
            }
        }
        if (slseq) { fprintf(out, "; })"); seq_restore(slslots, sln, slsaved); }
        break;
    }

    case EXPR_TUPLE_LIT: {
        /* Anonymous tuple → C compound literal of the synthesized struct, with
         * positional fields e0, e1, ... Multi-line (always >= 2 elements) so
         * nested statement-expressions (alloc(...)!, interpolated strings) don't
         * collapse into one over-long line that trips gcc's column tracking. */
        bool multiline = e->tuple_lit.elem_count >= 2;
        /* Force left-to-right element evaluation when an element has side
         * effects (C leaves initializer-list order unspecified). */
        int tn = e->tuple_lit.elem_count;
        Expr ***tslots = NULL; Expr *tscratch = NULL; Expr **tsaved = NULL;
        bool tseq = !g_const_context && tn >= 2;
        if (tseq) {
            tslots = arena_alloc(g_arena, sizeof(Expr**) * (size_t)tn);
            for (int i = 0; i < tn; i++) tslots[i] = &e->tuple_lit.elems[i];
            tseq = seq_needed(tslots, tn);
        }
        if (tseq) {
            tscratch = arena_alloc(g_arena, sizeof(Expr) * (size_t)tn);
            tsaved = arena_alloc(g_arena, sizeof(Expr*) * (size_t)tn);
            fprintf(out, "({ ");
            seq_hoist(tslots, tn, tscratch, tsaved, out);
        }
        fprintf(out, "(");
        emit_type(e->type, out);
        fprintf(out, "){");
        if (multiline) { fprintf(out, "\n"); indent_level++; }
        else fprintf(out, " ");
        for (int i = 0; i < e->tuple_lit.elem_count; i++) {
            if (i > 0) {
                fprintf(out, ",");
                if (multiline) fprintf(out, "\n");
                else fprintf(out, " ");
            }
            if (multiline) emit_indent(out);
            fprintf(out, ".e%d = ", i);
            emit_expr(e->tuple_lit.elems[i], out);
        }
        if (multiline) {
            fprintf(out, "\n");
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        } else {
            fprintf(out, " }");
        }
        if (tseq) { fprintf(out, "; })"); seq_restore(tslots, tn, tsaved); }
        break;
    }

    case EXPR_LOOP: {
        if (e->type && e->type->kind != TYPE_VOID) {
            /* Loop as expression: produces value via break */
            fprintf(out, "({\n");
            indent_level++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " _loop_result;\n");
            emit_indent(out);
            fprintf(out, "while (1) {\n");
            indent_level++;
            defer_scope_push(true);
            emit_block_stmts(e->loop_expr.body, e->loop_expr.body_count, out, false, true);
            defer_scope_pop();
            indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
            emit_indent(out);
            fprintf(out, "_loop_result;\n");
            indent_level--;
            emit_indent(out);
            fprintf(out, "})");
        } else {
            /* Void loop */
            fprintf(out, "while (1) {\n");
            indent_level++;
            defer_scope_push(true);
            emit_block_stmts(e->loop_expr.body, e->loop_expr.body_count, out, false, true);
            defer_scope_pop();
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        }
        break;
    }

    case EXPR_FOR: {
        if (e->for_expr.range_end) {
            /* Range iteration: for i in lo..hi. Hoist the end bound into a
             * once-evaluated temp so a side-effecting end expression (e.g.
             * `for i in 0..get_end()`) runs exactly once, not every iteration.
             * pass2 has widened both endpoints to the loop variable's type, so
             * the start, end temp, `<` test, and `++` all share `var_type`. */
            int tid = temp_counter++;
            Type *var_type = e->for_expr.iter->type;
            emit_type(var_type, out);
            fprintf(out, " _fe%d = ", tid);
            emit_expr(e->for_expr.range_end, out);
            fprintf(out, ";\n");
            emit_indent(out);
            fprintf(out, "for (");
            emit_type(var_type, out);
            const char *rvar = c_safe_ident(g_intern, e->for_expr.var);
            fprintf(out, " %s = ", rvar);
            emit_expr(e->for_expr.iter, out);
            fprintf(out, "; %s < _fe%d; %s++) {\n", rvar, tid, rvar);
        } else {
            /* Collection iteration. Hoist the iterable into a once-evaluated
             * temp: it was previously re-emitted in both the `.len` bound and
             * the `.ptr[]` element read, so a slice-returning call (e.g.
             * `for x in get_slice()`) ran 2n+1 times. */
            int tid = temp_counter++;
            Type *iter_type = e->for_expr.iter->type;

            emit_type(iter_type, out);
            fprintf(out, " _fs%d = ", tid);
            emit_expr(e->for_expr.iter, out);
            fprintf(out, ";\n");
            emit_indent(out);
            fprintf(out, "for (int64_t _fi%d = 0; _fi%d < _fs%d.len; _fi%d++) {\n",
                tid, tid, tid, tid);
            indent_level++;

            /* Element binding — into a temp first when destructuring */
            emit_indent(out);
            Type *elem_type = iter_type->slice.elem;
            const char *elem_name = e->for_expr.var_pattern
                ? e->for_expr.elem_tmp : c_safe_ident(g_intern, e->for_expr.var);
            emit_type(elem_type, out);
            fprintf(out, " %s = _fs%d.ptr[_fi%d];\n", elem_name, tid, tid);
            if (e->for_expr.var_pattern) {
                emit_pat_bindings(e->for_expr.var_pattern, elem_name, elem_type, out);
                emit_indent(out);
                fprintf(out, "(void)%s;\n", elem_name);
            }

            /* Index binding if present */
            if (e->for_expr.index_var) {
                emit_indent(out);
                fprintf(out, "int64_t %s = _fi%d;\n",
                    c_safe_ident(g_intern, e->for_expr.index_var), tid);
            }

            /* Body (already indented by indent_level++) */
            defer_scope_push(true);
            emit_block_stmts(e->for_expr.body, e->for_expr.body_count, out, false, true);
            defer_scope_pop();
            indent_level--;
            emit_indent(out);
            fprintf(out, "}");
            break;
        }

        /* Body for range iteration */
        indent_level++;
        defer_scope_push(true);
        emit_block_stmts(e->for_expr.body, e->for_expr.body_count, out, false, true);
        defer_scope_pop();
        indent_level--;
        emit_indent(out);
        fprintf(out, "}");
        break;
    }

    case EXPR_MATCH: {
        /* Emit as statement expression. When no arm has a `when` guard we use
           the original if/else chain with an unconditional last arm (exhaustive
           by pass2). When at least one arm has a guard we use a done-flag so
           guard-false arms can fall through to subsequent arms. */
        /* No result temp when the match yields no value: void, or never (every arm
           diverges via return/break/continue). */
        bool match_is_void = type_valueless(e->type);

        bool has_any_guard = false;
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            if (e->match_expr.arms[i].guard) { has_any_guard = true; break; }
        }

        fprintf(out, "({\n");
        indent_level++;

        /* Emit subject into a temp variable */
        int subj_id = temp_counter++;
        emit_indent(out);
        emit_type(e->match_expr.subject->type, out);
        fprintf(out, " _subj%d = ", subj_id);
        emit_expr(e->match_expr.subject, out);
        fprintf(out, ";\n");
        emit_indent(out);
        fprintf(out, "(void)_subj%d;\n", subj_id);

        /* Emit result variable (skip for void matches) */
        int res_id = -1;
        if (!match_is_void) {
            res_id = temp_counter++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " _match%d;\n", res_id);
        }

        int done_id = -1;
        if (has_any_guard) {
            done_id = temp_counter++;
            emit_indent(out);
            fprintf(out, "int _matchdone%d = 0;\n", done_id);
        }

        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            Pattern *pat = arm->pattern;
            char subj_expr[64];
            snprintf(subj_expr, sizeof(subj_expr), "_subj%d", subj_id);

            bool is_last_arm = (i == e->match_expr.arm_count - 1);

            if (has_any_guard) {
                /* if (!_matchdoneN) { [if (pat_cond)] { bindings; [if (guard)] { body; _matchdoneN = 1; } } } */
                emit_indent(out);
                fprintf(out, "if (!_matchdone%d) {\n", done_id);
                indent_level++;

                emit_indent(out);
                bool has_cond = false;
                emit_pat_conditions(pat, subj_expr, e->match_expr.subject->type, &has_cond, out);
                if (has_cond) fprintf(out, ") {\n");
                else fprintf(out, "{\n");
                indent_level++;

                emit_pat_bindings(pat, subj_expr, e->match_expr.subject->type, out);

                bool has_guard = (arm->guard != NULL);
                if (has_guard) {
                    emit_indent(out);
                    /* Avoid double-parens like if ((x == y)) which triggers
                       clang's -Wparentheses-equality when the guard is a
                       binary expression (emit_expr wraps binaries in parens). */
                    if (emit_self_parens(arm->guard)) {
                        fprintf(out, "if ");
                        emit_expr(arm->guard, out);
                        fprintf(out, " {\n");
                    } else {
                        fprintf(out, "if (");
                        emit_expr(arm->guard, out);
                        fprintf(out, ") {\n");
                    }
                    indent_level++;
                }
            } else {
                emit_indent(out);
                if (i > 0) fprintf(out, "else ");
                bool has_cond = false;
                if (!is_last_arm) {
                    emit_pat_conditions(pat, subj_expr, e->match_expr.subject->type, &has_cond, out);
                }
                if (has_cond) fprintf(out, ") {\n");
                else fprintf(out, "{\n");
                indent_level++;
                emit_pat_bindings(pat, subj_expr, e->match_expr.subject->type, out);
            }

            /* Emit arm body */
            if (match_is_void) {
                defer_scope_push(false);
                emit_block_stmts(arm->body, arm->body_count, out, false, true);
                defer_scope_pop();
            } else if (arm->body_count == 1) {
                emit_indent(out);
                if (type_is_never(arm->body[0]->type)) {
                    /* Diverging arm (return/break/continue): emit as a statement,
                       no assignment — its exit fires before the temp is read. */
                    emit_expr(arm->body[0], out);
                } else {
                    fprintf(out, "_match%d = ", res_id);
                    emit_expr(arm->body[0], out);
                }
                fprintf(out, ";\n");
            } else {
                /* Multiple statements — emit all, last is the value */
                defer_scope_push(false);
                for (int s = 0; s < arm->body_count; s++) {
                    if (arm->body[s]->kind == EXPR_DEFER) {
                        defer_scope_add(arm->body[s]->defer_expr.value);
                        continue;
                    }
                    emit_indent(out);
                    if (s == arm->body_count - 1) {
                        if (type_is_never(arm->body[s]->type)) {
                            /* Diverging tail: emit as a statement, no assignment.
                               emit_expr handles its own defers (return/break). */
                            emit_expr(arm->body[s], out);
                            fprintf(out, ";\n");
                        } else if (has_pending_defers()) {
                            emit_type(arm->body[s]->type, out);
                            int tid = temp_counter++;
                            fprintf(out, " _mret%d = ", tid);
                            emit_expr(arm->body[s], out);
                            fprintf(out, ";\n");
                            emit_scope_defers(g_defer_scope, out);
                            emit_indent(out);
                            fprintf(out, "_match%d = _mret%d;\n", res_id, tid);
                        } else {
                            fprintf(out, "_match%d = ", res_id);
                            emit_expr(arm->body[s], out);
                            fprintf(out, ";\n");
                        }
                    } else {
                        emit_expr(arm->body[s], out);
                        fprintf(out, ";\n");
                    }
                }
                defer_scope_pop();
            }

            if (has_any_guard) {
                emit_indent(out);
                fprintf(out, "_matchdone%d = 1;\n", done_id);
                bool has_guard = (arm->guard != NULL);
                if (has_guard) {
                    indent_level--;
                    emit_indent(out);
                    fprintf(out, "}\n");
                }
                indent_level--;
                emit_indent(out);
                fprintf(out, "}\n");
                indent_level--;
                emit_indent(out);
                fprintf(out, "}\n");
            } else {
                indent_level--;
                emit_indent(out);
                fprintf(out, "}\n");
            }
        }

        if (has_any_guard) {
            /* Unreachable if pass2 exhaustiveness holds: at least one unguarded
               arm must have covered the value. Emitting abort() here both keeps
               _matchN provably initialized for -Wuninitialized and fails loudly
               if a bug ever produces an uncovered case at runtime. */
            emit_indent(out);
            fprintf(out, "if (!_matchdone%d) FC_ABORT();\n", done_id);
        }

        if (!match_is_void) {
            emit_indent(out);
            fprintf(out, "_match%d;\n", res_id);
        }

        indent_level--;
        emit_indent(out);
        fprintf(out, "})");
        break;
    }

    case EXPR_BREAK:
        if (has_pending_defers()) {
            fprintf(out, "({ ");
            if (e->break_expr.value) {
                emit_type(e->break_expr.value->type, out);
                int tid = temp_counter++;
                fprintf(out, " _brk%d = ", tid);
                emit_expr(e->break_expr.value, out);
                fprintf(out, "; ");
                emit_defers_to_loop(out);
                fprintf(out, "_loop_result = _brk%d; break; })", tid);
            } else {
                emit_defers_to_loop(out);
                fprintf(out, "break; })");
            }
        } else {
            if (e->break_expr.value) {
                fprintf(out, "_loop_result = ");
                emit_expr(e->break_expr.value, out);
                fprintf(out, "; break");
            } else {
                fprintf(out, "break");
            }
        }
        break;

    case EXPR_CONTINUE:
        if (has_pending_defers()) {
            fprintf(out, "({ ");
            emit_defers_to_loop(out);
            fprintf(out, "continue; })");
        } else {
            fprintf(out, "continue");
        }
        break;

    case EXPR_RETURN:
        if (has_pending_defers()) {
            fprintf(out, "({ ");
            if (e->return_expr.value) {
                emit_type(e->return_expr.value->type, out);
                int tid = temp_counter++;
                fprintf(out, " _ret%d = ", tid);
                emit_expr(e->return_expr.value, out);
                fprintf(out, "; ");
                emit_defers_to_func(out);
                fprintf(out, "return _ret%d; })", tid);
            } else {
                emit_defers_to_func(out);
                fprintf(out, "return; })");
            }
        } else {
            if (e->return_expr.value) {
                fprintf(out, "return ");
                emit_expr(e->return_expr.value, out);
            } else {
                fprintf(out, "return");
            }
        }
        break;

    case EXPR_DEFER:
        /* Handled in emit_block_stmts; should not reach here */
        break;

    case EXPR_SIZEOF: {
        fprintf(out, "(int64_t)sizeof(");
        emit_type(e->sizeof_expr.target, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_ALIGNOF: {
        fprintf(out, "(int64_t)_Alignof(");
        emit_type(e->alignof_expr.target, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_DEFAULT: {
        Type *t = e->default_expr.target;
        switch (t->kind) {
        case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        case TYPE_ISIZE: case TYPE_USIZE:
        case TYPE_CHAR:
            fprintf(out, "0");
            break;
        case TYPE_FLOAT32:
            fprintf(out, "0.0f");
            break;
        case TYPE_FLOAT64:
            fprintf(out, "0.0");
            break;
        case TYPE_BOOL:
            fprintf(out, "false");
            break;
        case TYPE_POINTER:
        case TYPE_ANY_PTR:
            fprintf(out, "NULL");
            break;
        case TYPE_OPTION:
            if (is_null_sentinel(t))
                fprintf(out, "NULL");
            else {
                fprintf(out, "(");
                emit_type(t, out);
                fprintf(out, "){ .has_value = false }");
            }
            break;
        default:
            /* Structs, unions, slices — compound literal with {0} */
            fprintf(out, "(");
            emit_type(t, out);
            fprintf(out, "){0}");
            break;
        }
        break;
    }

    case EXPR_FREE: {
        Type *ot = e->free_expr.operand->type;
        if (ot && ot->kind == TYPE_SLICE) {
            fprintf(out, "free((");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ").ptr)");
        } else {
            fprintf(out, "free(");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ")");
        }
        break;
    }

    case EXPR_ASSERT: {
        const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
        int fn_len = (int)strlen(fn);
        int line = e->loc.line;
        fprintf(out, "({ if (!");
        if (e->assert_expr.condition->kind != EXPR_BINARY) fprintf(out, "(");
        emit_expr(e->assert_expr.condition, out);
        if (e->assert_expr.condition->kind != EXPR_BINARY) fprintf(out, ")");
        fprintf(out, ") { ");
        if (e->assert_expr.message) {
            int tid = temp_counter++;
            fprintf(out, "fc_str _am%d = ", tid);
            emit_expr(e->assert_expr.message, out);
            fprintf(out, "; fprintf(stderr, \"");
            emit_c_escaped(fn, fn_len, out);
            fprintf(out, ":%d: assertion failed: ", line);
            emit_c_escaped(e->assert_expr.expr_text, e->assert_expr.expr_text_len, out);
            fprintf(out, ": %%.*s\\n\", fc_to_int(_am%d.len), (const char*)_am%d.ptr); ", tid, tid);
        } else {
            fprintf(out, "fprintf(stderr, \"");
            emit_c_escaped(fn, fn_len, out);
            fprintf(out, ":%d: assertion failed: ", line);
            emit_c_escaped(e->assert_expr.expr_text, e->assert_expr.expr_text_len, out);
            fprintf(out, "\\n\"); ");
        }
        fprintf(out, "FC_ABORT(); } })");
        break;
    }

    case EXPR_ATOMIC_LOAD: {
        Type *cell = e->atomic_load.ptr->type->pointer.pointee;
        fprintf(out, "({ _Static_assert(__atomic_always_lock_free(sizeof(");
        emit_type(cell, out);
        fprintf(out, "), 0), \"FC atomics require lock-free access for this type on the target\"); "
                     "__atomic_load_n((");
        emit_expr(e->atomic_load.ptr, out);
        fprintf(out, "), __ATOMIC_ACQUIRE); })");
        break;
    }

    case EXPR_ATOMIC_STORE: {
        Type *cell = e->atomic_store.ptr->type->pointer.pointee;
        fprintf(out, "({ _Static_assert(__atomic_always_lock_free(sizeof(");
        emit_type(cell, out);
        fprintf(out, "), 0), \"FC atomics require lock-free access for this type on the target\"); "
                     "__atomic_store_n((");
        emit_expr(e->atomic_store.ptr, out);
        fprintf(out, "), (");
        emit_expr(e->atomic_store.value, out);
        fprintf(out, "), __ATOMIC_RELEASE); })");
        break;
    }

    case EXPR_ALLOC: {
        if (e->alloc_expr.is_stack) {
            /* alloca(...) — dynamic stack, no option wrapper, no failure sentinel.
             * Reclaimed when the enclosing function returns. */
            if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
                /* alloca(T, N) → T* (raw buffer) */
                fprintf(out, "(");
                emit_type(e->alloc_expr.alloc_type, out);
                fprintf(out, "*)__builtin_alloca(fc_to_size(");
                emit_expr(e->alloc_expr.size_expr, out);
                fprintf(out, ") * sizeof(");
                emit_type(e->alloc_expr.alloc_type, out);
                fprintf(out, "))");
            } else if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr) {
                /* alloca(T[n] { }) → T[] (zero-initialized runtime-sized slice) */
                int tid = temp_counter++;
                fprintf(out, "({ int64_t _asz%d = (int64_t)", tid);
                emit_expr(e->alloc_expr.size_expr, out);
                fprintf(out, "; ");
                emit_type(e->alloc_expr.alloc_type, out);
                fprintf(out, "* _aptr%d = (", tid);
                emit_type(e->alloc_expr.alloc_type, out);
                fprintf(out, "*)__builtin_alloca(fc_to_size(_asz%d) * sizeof(", tid);
                emit_type(e->alloc_expr.alloc_type, out);
                fprintf(out, ")); memset(_aptr%d, 0, fc_to_size(_asz%d) * sizeof(", tid, tid);
                emit_type(e->alloc_expr.alloc_type, out);
                fprintf(out, ")); (");
                emit_type(e->type, out);
                fprintf(out, "){ .ptr = _aptr%d, .len = _asz%d }; })", tid, tid);
            } else if (e->alloc_expr.init_expr &&
                       e->alloc_expr.init_expr->kind == EXPR_INTERP_STRING) {
                /* alloca("interp %s{x}") / alloca(c"...") → str/cstr on the stack.
                 * The standalone (NULL) path emits __builtin_alloca. */
                emit_interp_string_impl(e->alloc_expr.init_expr, out, NULL);
            } else if (e->alloc_expr.init_expr &&
                       e->alloc_expr.init_expr->kind == EXPR_ARRAY_LIT) {
                /* alloca(T[N] { elems }) literal → reuse the array-literal stack
                 * emission (already a function-frame backing). */
                emit_expr(e->alloc_expr.init_expr, out);
            } else {
                emit_expr(e->alloc_expr.init_expr, out);
            }
            break;
        }
        if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
            /* alloc(T, N) → T*? (raw buffer, null sentinel) */
            fprintf(out, "(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)calloc(fc_to_size(");
            emit_expr(e->alloc_expr.size_expr, out);
            fprintf(out, "), sizeof(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "))");
        } else if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr) {
            /* alloc(T[N]) → T[]? */
            int tid = temp_counter++;
            fprintf(out, "({ int64_t _asz%d = (int64_t)", tid);
            emit_expr(e->alloc_expr.size_expr, out);
            fprintf(out, "; ");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "* _aptr%d = (", tid);
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)calloc(fc_to_size(_asz%d), sizeof(", tid);
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, ")); _aptr%d ? (", tid);
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            Type *slice_type = e->type->option.inner;
            emit_type(slice_type, out);
            fprintf(out, "){ .ptr = _aptr%d, .len = _asz%d }, .has_value = true } : (", tid, tid);
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }; })");
        } else if (e->alloc_expr.alloc_type) {
            /* alloc(T) → T*? (bare pointer, calloc returns NULL on failure) */
            fprintf(out, "(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)calloc(1, sizeof(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "))");
        } else if (e->alloc_expr.init_expr->kind == EXPR_STRING_LIT) {
            /* alloc("literal") → str? (direct to heap) */
            int tid = temp_counter++;
            Expr *ie = e->alloc_expr.init_expr;
            /* Compute actual byte length after escape processing */
            int actual_len = 0;
            const char *s = ie->string_lit.value;
            int slen = ie->string_lit.length;
            for (int j = 0; j < slen; j++) {
                if (s[j] == '%' && j + 1 < slen && s[j+1] == '%') j++;
                else if (s[j] == '\\' && j + 1 < slen) {
                    if (s[j+1] == 'x' && j + 3 < slen) j += 3;
                    else j++;
                }
                actual_len++;
            }
            fprintf(out, "({ uint8_t *_ap%d = (uint8_t*)malloc(%d); ", tid, actual_len);
            fprintf(out, "_ap%d ? (memcpy(_ap%d, (uint8_t*)\"%.*s\", %d), (",
                tid, tid, ie->string_lit.length, ie->string_lit.value, actual_len);
            emit_type(e->type, out);
            fprintf(out, "){ .value = (fc_str){ .ptr = _ap%d, .len = %d }, .has_value = true }) : (",
                tid, actual_len);
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }; })");
        } else if (e->alloc_expr.init_expr->kind == EXPR_CSTRING_LIT) {
            /* alloc(c"literal") → cstr? (direct to heap, null sentinel) */
            int tid = temp_counter++;
            Expr *ie = e->alloc_expr.init_expr;
            /* Compute actual byte length after escape processing */
            int actual_len = 0;
            const char *s = ie->cstring_lit.value;
            int slen = ie->cstring_lit.length;
            for (int j = 0; j < slen; j++) {
                if (s[j] == '\\' && j + 1 < slen) {
                    if (s[j+1] == 'x' && j + 3 < slen) j += 3;
                    else j++;
                }
                actual_len++;
            }
            fprintf(out, "({ uint8_t *_ap%d = (uint8_t*)malloc(%d + 1); ", tid, actual_len);
            fprintf(out, "if (_ap%d) memcpy(_ap%d, (uint8_t*)\"%.*s\", %d + 1); _ap%d; })",
                tid, tid, ie->cstring_lit.length, ie->cstring_lit.value, actual_len, tid);
        } else if (e->alloc_expr.init_expr->kind == EXPR_CAST &&
                   is_cstr_type(e->alloc_expr.init_expr->type)) {
            /* alloc((cstr) str) → cstr? — heap copy of str + NUL (null sentinel on
             * malloc failure). The cast's own (alloca) codegen is bypassed: the
             * str→cstr copy goes straight into the heap buffer. */
            int tid = temp_counter++;
            Expr *operand = e->alloc_expr.init_expr->cast.operand;
            fprintf(out, "({ fc_str _sc%d = ", tid);
            emit_expr(operand, out);
            fprintf(out, "; uint8_t *_ap%d = (uint8_t*)malloc(fc_to_size(_sc%d.len + 1)); ", tid, tid);
            fprintf(out, "if (_ap%d) { memcpy(_ap%d, _sc%d.ptr, fc_to_size(_sc%d.len)); "
                         "_ap%d[_sc%d.len] = '\\0'; } _ap%d; })",
                    tid, tid, tid, tid, tid, tid, tid);
        } else if (e->alloc_expr.init_expr->kind == EXPR_INTERP_STRING) {
            /* alloc("interp %d{x}") or alloc(c"interp %d{x}") → str?/cstr? */
            emit_interp_string_impl(e->alloc_expr.init_expr, out, e->type);
        } else if (e->alloc_expr.init_expr->kind == EXPR_ARRAY_LIT) {
            /* alloc(T[N] { elems }) → T[]? (direct to heap) */
            int tid = temp_counter++;
            Expr *ie = e->alloc_expr.init_expr;
            Type *elem_type = ie->array_lit.elem_type;
            int64_t size = ie->array_lit.size_expr->int_lit.value;
            int ec = ie->array_lit.elem_count;
            fprintf(out, "({ ");
            emit_type(elem_type, out);
            if (ec == 0) {
                fprintf(out, " *_ap%d = (", tid);
                emit_type(elem_type, out);
                fprintf(out, "*)calloc(%d, sizeof(", (int)size);
            } else {
                fprintf(out, " *_ap%d = (", tid);
                emit_type(elem_type, out);
                fprintf(out, "*)malloc(%d * sizeof(", (int)size);
            }
            emit_type(elem_type, out);
            fprintf(out, ")); ");
            fprintf(out, "_ap%d ? (", tid);
            for (int i = 0; i < ec; i++) {
                fprintf(out, "_ap%d[%d] = ", tid, i);
                emit_expr(ie->array_lit.elems[i], out);
                fprintf(out, ", ");
            }
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            /* inner slice type */
            Type *sl = e->type->option.inner;
            emit_type(sl, out);
            fprintf(out, "){ .ptr = _ap%d, .len = %d }, .has_value = true }) : (", tid, (int)size);
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }; })");
        } else if (e->alloc_expr.init_expr->kind == EXPR_STRUCT_LIT) {
            /* alloc(struct_lit) → T*? (malloc + compound literal, null sentinel).
             * Braces around the if body so multi-line struct literals don't
             * trigger clang's -Wmisleading-indentation. */
            int tid = temp_counter++;
            Type *val_type = e->alloc_expr.init_expr->type;
            fprintf(out, "({ ");
            emit_type(val_type, out);
            fprintf(out, "* _ap%d = (", tid);
            emit_type(val_type, out);
            fprintf(out, "*)malloc(sizeof(");
            emit_type(val_type, out);
            fprintf(out, ")); if (_ap%d) { *_ap%d = ", tid, tid);
            emit_expr(e->alloc_expr.init_expr, out);
            fprintf(out, "; } _ap%d; })", tid);
        } else if (e->alloc_expr.init_expr->type &&
                   e->alloc_expr.init_expr->type->kind == TYPE_UNION) {
            /* alloc(union_variant) → T*? (malloc + compound literal, null sentinel) */
            int tid = temp_counter++;
            Type *val_type = e->alloc_expr.init_expr->type;
            fprintf(out, "({ ");
            emit_type(val_type, out);
            fprintf(out, "* _ap%d = (", tid);
            emit_type(val_type, out);
            fprintf(out, "*)malloc(sizeof(");
            emit_type(val_type, out);
            fprintf(out, ")); if (_ap%d) { *_ap%d = ", tid, tid);
            emit_expr(e->alloc_expr.init_expr, out);
            fprintf(out, "; } _ap%d; })", tid);
        } else if (e->alloc_expr.init_expr->type &&
                   e->alloc_expr.init_expr->type->kind == TYPE_SLICE) {
            /* alloc(slice_expr) → T[]? (deep-copy slice data to heap) */
            int tid = temp_counter++;
            Type *st = e->alloc_expr.init_expr->type;
            Type *elem_type = st->slice.elem;
            fprintf(out, "({ ");
            emit_type(st, out);
            fprintf(out, " _as%d = ", tid);
            emit_expr(e->alloc_expr.init_expr, out);
            fprintf(out, "; ");
            emit_type(elem_type, out);
            fprintf(out, " *_ap%d = (", tid);
            emit_type(elem_type, out);
            fprintf(out, "*)malloc(fc_to_size(_as%d.len) * sizeof(", tid);
            emit_type(elem_type, out);
            fprintf(out, ")); ");
            fprintf(out, "_ap%d ? (memcpy(_ap%d, _as%d.ptr, fc_to_size(_as%d.len) * sizeof(",
                tid, tid, tid, tid);
            emit_type(elem_type, out);
            fprintf(out, ")), (");
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            emit_type(st, out);
            fprintf(out, "){ .ptr = _ap%d, .len = _as%d.len }, .has_value = true }) : (",
                tid, tid);
            emit_type(e->type, out);
            fprintf(out, "){ .has_value = false }; })");
        } else {
            /* Unreachable — pass2 rejects unsupported alloc(expr) forms */
            fprintf(out, "((void)0 /* unsupported alloc form */)");
        }
        break;
    }

    case EXPR_ASSIGN: {
        /* Special case: assignment to fixed-array field — bounded memcpy */
        Expr *target = e->assign.target;
        if ((target->kind == EXPR_FIELD || target->kind == EXPR_DEREF_FIELD) &&
            target->field.fixed_array_type) {
            Type *fat = target->field.fixed_array_type;
            int tid = temp_counter++;
            const char *fn = e->loc.filename ? e->loc.filename : "<unknown>";
            int fn_len = (int)strlen(fn);
            int line = e->loc.line;
            bool deref = target->kind == EXPR_DEREF_FIELD;
            fprintf(out, "({ ");
            /* Evaluate the destination object exactly once and before the
             * source (left-to-right): hoist a pointer to the containing struct.
             * For x->f the object is already a pointer; for x.f take its
             * address. */
            emit_type(target->field.object->type, out);
            if (deref) fprintf(out, " _ao%d = (", tid);
            else       fprintf(out, " *_ao%d = &(", tid);
            emit_expr(target->field.object, out);
            fprintf(out, "); ");
            /* Evaluate source slice */
            emit_type(e->assign.value->type, out);
            fprintf(out, " _fas%d = ", tid);
            emit_expr(e->assign.value, out);
            fprintf(out, "; if (_fas%d.len > %lld) { fprintf(stderr, \"", tid,
                    (long long)fat->fixed_array.size);
            emit_c_escaped(fn, fn_len, out);
            fprintf(out, ":%d: fixed-array field '%s' overflow: "
                         "len=%%lld capacity=%lld\\n\", "
                         "(long long)_fas%d.len); abort(); } ",
                    line, target->field.name,
                    (long long)fat->fixed_array.size, tid);
            /* memcpy the data */
            fprintf(out, "memcpy(_ao%d->%s, _fas%d.ptr, fc_to_size(_fas%d.len) * sizeof(",
                    tid, c_safe_ident(g_intern, target->field.name), tid, tid);
            emit_type(fat->fixed_array.elem, out);
            fprintf(out, ")); ");
            /* Zero-fill remainder */
            fprintf(out, "if (_fas%d.len < %lld) memset(_ao%d->%s + _fas%d.len, 0, "
                         "fc_to_size(%lld - _fas%d.len) * sizeof(",
                    tid, (long long)fat->fixed_array.size, tid,
                    c_safe_ident(g_intern, target->field.name),
                    tid, (long long)fat->fixed_array.size, tid);
            emit_type(fat->fixed_array.elem, out);
            fprintf(out, ")); })");
            break;
        }
        /* Slice element assignment handled generically — EXPR_INDEX now produces
         * an lvalue via pointer dereference, so no special case needed.
         *
         * When the target subexpressions or the value have side effects, force
         * left-to-right order (target's lvalue subexpressions, then the value):
         * take the target's address first, then evaluate the value, then store.
         * &(target) is well-defined for every assignment target (it is an
         * lvalue), and the index/field lowering inside it is already
         * left-to-right. */
        if (!g_const_context &&
            (expr_has_side_effects(e->assign.value) ||
             expr_has_side_effects(e->assign.target))) {
            int tid = temp_counter++;
            fprintf(out, "({ ");
            emit_type(e->assign.target->type, out);
            fprintf(out, " *_at%d = &(", tid);
            emit_expr(e->assign.target, out);
            fprintf(out, "); ");
            emit_type(e->assign.value->type, out);
            fprintf(out, " _av%d = ", tid);
            emit_expr(e->assign.value, out);
            fprintf(out, "; *_at%d = _av%d; })", tid, tid);
        } else {
            emit_expr(e->assign.target, out);
            fprintf(out, " = ");
            emit_expr(e->assign.value, out);
        }
        break;
    }

    case EXPR_LET: {
        const char *vname = e->let_expr.codegen_name ? e->let_expr.codegen_name : e->let_expr.let_name;
        if (is_hoisted(vname)) {
            fprintf(out, "%s = ", vname);
        } else {
            emit_type(e->let_expr.let_type, out);
            fprintf(out, " %s = ", vname);
        }
        emit_expr(e->let_expr.let_init, out);
        break;
    }

    case EXPR_FUNC: {
        /* Lambda in expression position — emit fat pointer */
        if (e->func.capture_count > 0) {
            /* Capturing lambda: use compound literal for context (block-scope lifetime) */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = &(_ctx_%s){ ",
                e->func.lifted_name, e->func.lifted_name);
            for (int i = 0; i < e->func.capture_count; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, ".%s = %s",
                    e->func.captures[i].codegen_name,
                    e->func.captures[i].codegen_name);
            }
            fprintf(out, " } }");
        } else {
            /* Non-capturing lambda: NULL context */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                e->func.lifted_name);
        }
        break;
    }

    case EXPR_INTERP_STRING:
        emit_interp_string_impl(e, out, NULL);
        break;


    default:
        fprintf(out, "/* TODO: expr kind %d */", e->kind);
        break;
    }
}

/* Check if a top-level decl is a function (its init expr is EXPR_FUNC) */
static bool is_func_decl(Decl *d) {
    return d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC;
}

static bool is_generic_decl(Decl *d) {
    if (d->kind == DECL_STRUCT) return d->struc.is_generic;
    if (d->kind == DECL_UNION) return d->unio.is_generic;
    if (d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC) {
        /* Check if any param contains type vars */
        Expr *fn = d->let.init;
        if (fn->func.explicit_type_var_count > 0) return true;
        for (int i = 0; i < fn->func.param_count; i++)
            if (type_contains_type_var(fn->func.params[i].type)) return true;
    }
    return false;
}

static void emit_func_decl(Decl *d, FILE *out) {
    Expr *fn = d->let.init;
    Type *ft = d->let.resolved_type;
    const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
    bool is_main = strcmp(d->let.name, "main") == 0;

    if (is_main) {
        /* Emit the FC main body as fc_main(str[] args).  Static: only the
         * int main(int, char**) wrapper (emitted below) calls it. */
        fprintf(out, "%sint32_t fc_main(", g_fn_attr);
        emit_type(fn->func.params[0].type, out);
        fprintf(out, " %s) {\n", c_safe_ident(g_intern, fn->func.params[0].name));
    } else {
        /* Emit return type.  Static: FC emits a single translation unit, so
         * nothing outside this TU calls these functions; static allows GCC
         * to inline more aggressively and DCE unused helpers. */
        fprintf(out, "%s", g_fn_attr);
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", cname);
        for (int i = 0; i < fn->func.param_count; i++) {
            if (i > 0) fprintf(out, ", ");
            emit_type(fn->func.params[i].type, out);
            fprintf(out, " %s", c_safe_ident(g_intern, fn->func.params[i].name));
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx) {\n");
        fprintf(out, "    (void)_ctx;\n");
    }

    indent_level = 1;
    begin_hoisted_scope(fn->func.body, fn->func.body_count, out);
    defer_scope_push(false);
    emit_block_stmts(fn->func.body, fn->func.body_count, out, true, true);
    defer_scope_pop();
    end_hoisted_scope();
    indent_level = 0;

    /* For main, if body's last expr doesn't return, add return 0 */
    if (is_main) {
        int last_idx = fn->func.body_count - 1;
        if (last_idx >= 0) {
            Expr *last = fn->func.body[last_idx];
            if (last->kind != EXPR_RETURN && last->type &&
                type_is_integer(last->type)) {
                /* Already emitted as return by emit_block_stmts */
            } else if (last->kind == EXPR_RETURN) {
                /* Already has return */
            } else {
                fprintf(out, "    return 0;\n");
            }
        }
    }

    fprintf(out, "}\n\n");

    /* Emit C main wrapper that converts argc/argv to str[] */
    if (is_main) {
        fprintf(out, "int main(int argc, char **argv) {\n");
        /* Hoisted file-level global initializations */
        for (int gi = 0; gi < g_file_global_count; gi++) {
            Decl *gd = g_file_globals[gi];
            fprintf(out, "    %s = ",
                gd->let.codegen_name ? gd->let.codegen_name : gd->let.name);
            emit_expr(gd->let.init, out);
            fprintf(out, ";\n");
        }
        fprintf(out, "    fc_str *_args = (fc_str*)__builtin_alloca((size_t)argc * sizeof(fc_str));\n");
        fprintf(out, "    for (int _i = 0; _i < argc; _i++) {\n");
        fprintf(out, "        _args[_i].ptr = (uint8_t*)argv[_i];\n");
        fprintf(out, "        _args[_i].len = (int64_t)strlen(argv[_i]);\n");
        fprintf(out, "    }\n");
        fprintf(out, "    return fc_main((");
        emit_type(fn->func.params[0].type, out);
        fprintf(out, "){ .ptr = _args, .len = (int64_t)argc });\n");
        fprintf(out, "}\n\n");
    }
}

static void emit_struct_forward(Decl *d, FILE *out) {
    fprintf(out, "typedef struct %s %s;\n", d->struc.name, d->struc.name);
}

static void emit_struct_field(Type *ft, const char *name, FILE *out) {
    if (ft->kind == TYPE_FIXED_ARRAY) {
        fprintf(out, " ");
        emit_type(ft->fixed_array.elem, out);
        fprintf(out, " %s[%lld];", name, (long long)ft->fixed_array.size);
    } else {
        fprintf(out, " ");
        emit_type(ft, out);
        fprintf(out, " %s;", name);
    }
}

static void emit_struct_def(Decl *d, FILE *out) {
    fprintf(out, "struct %s {", d->struc.name);
    for (int i = 0; i < d->struc.field_count; i++) {
        emit_struct_field(d->struc.fields[i].type,
            c_safe_ident(g_intern, d->struc.fields[i].name), out);
    }
    fprintf(out, " };\n");
}

static void emit_union_forward(Decl *d, FILE *out) {
    fprintf(out, "typedef struct %s %s;\n", d->unio.name, d->unio.name);
}

static void emit_union_tag_enum(Decl *d, FILE *out) {
    const char *name = d->unio.name;
    fprintf(out, "typedef enum {");
    for (int i = 0; i < d->unio.variant_count; i++) {
        if (i > 0) fprintf(out, ",");
        fprintf(out, " %s_tag_%s", name, d->unio.variants[i].name);
    }
    fprintf(out, " } %s_tag;\n", name);
}

static void emit_union_def(Decl *d, FILE *out) {
    const char *name = d->unio.name;
    /* Check if any variant has a payload */
    bool has_any_payload = false;
    for (int i = 0; i < d->unio.variant_count; i++) {
        if (d->unio.variants[i].payload) { has_any_payload = true; break; }
    }
    if (has_any_payload) {
        fprintf(out, "struct %s { %s_tag tag; union {", name, name);
        for (int i = 0; i < d->unio.variant_count; i++) {
            if (d->unio.variants[i].payload) {
                fprintf(out, " ");
                emit_type(d->unio.variants[i].payload, out);
                fprintf(out, " %s;", c_safe_ident(g_intern, d->unio.variants[i].name));
            }
        }
        fprintf(out, " }; };\n");
    } else {
        /* Tag-only union (enum-like) — no anonymous union needed */
        fprintf(out, "struct %s { %s_tag tag; };\n", name, name);
    }
}

/* ---- Collect used slice/option types for typedef generation ---- */

struct TypeSet {
    Type **types;
    int count;
    int cap;
};

static bool typeset_contains(TypeSet *ts, Type *t) {
    for (int i = 0; i < ts->count; i++) {
        if (type_eq_ignore_const(ts->types[i], t)) return true;
    }
    return false;
}

static void typeset_add(TypeSet *ts, Type *t) {
    if (!typeset_contains(ts, t)) {
        DA_APPEND(ts->types, ts->count, ts->cap, t);
    }
}

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options, TypeSet *fns);

static void collect_types_in_type(Type *t, TypeSet *slices, TypeSet *options, TypeSet *fns) {
    if (!t) return;
    /* Apply type variable substitution if available */
    if (g_subst && type_contains_type_var(t)) {
        t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
        if (!type_contains_type_var(t))
            mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    }
    if (type_contains_type_var(t)) return;  /* still has unresolved type vars */
    /* Resolve stubs before type classification */
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if (t->kind == TYPE_FIXED_ARRAY) {
        /* Fixed-array field: need slice typedef for the element type (field access returns slice) */
        collect_types_in_type(t->fixed_array.elem, slices, options, fns);
        Type *slice_t = type_slice(g_arena, t->fixed_array.elem);
        typeset_add(slices, slice_t);
    } else if (t->kind == TYPE_SLICE) {
        collect_types_in_type(t->slice.elem, slices, options, fns);
        typeset_add(slices, t);
    } else if (t->kind == TYPE_OPTION) {
        /* Canonicalize a stub inner (box<int32>?) to its monomorphized struct so
         * the stored option's inner carries the mangled name. Two things depend
         * on this: (1) the def-ordering interleave matches the option to its inner
         * def by name, and (2) an option collected from a concrete field type
         * (inner = base-name stub) dedupes against the same option collected from
         * a some()/value expression (inner = resolved struct) — otherwise both
         * emit `fc_option_box__5_int32`, a duplicate typedef. */
        Type *inner = t->option.inner;
        if (inner && inner->kind == TYPE_STUB) {
            Type *resolved = resolve_struct_stub(inner);
            if (resolved != inner) {
                t = type_option(g_arena, resolved);
                inner = resolved;
            }
        }
        /* Recurse into inner type FIRST so dependencies are emitted before this type */
        collect_types_in_type(inner, slices, options, fns);
        /* Only non-pointer options need typedefs */
        if (!inner || inner->kind != TYPE_POINTER) {
            typeset_add(options, t);
        }
    } else if (t->kind == TYPE_FUNC) {
        /* Recurse into param/return types FIRST so dependencies are emitted before this type */
        for (int i = 0; i < t->func.param_count; i++)
            collect_types_in_type(t->func.param_types[i], slices, options, fns);
        collect_types_in_type(t->func.return_type, slices, options, fns);
        typeset_add(fns, t);
    }
}

/* Resolve TYPE_STUB types (unresolved name-only references) to full definitions */
static Type *resolve_struct_stub(Type *t) {
    if (!g_symtab) return t;
    if (t->kind == TYPE_STUB) {
        /* A concrete generic instance (e.g. box<int32>) used as a by-value field
         * resolves to its monomorphized instance, not the generic template — the
         * template is never emitted as a complete type. The field stub keeps its
         * base name (the pass2 type is shared with the symbol table, so it must
         * not be mutated); the mangled name is computed here for the lookup. */
        if (t->stub.type_arg_count > 0 && !type_contains_type_var(t)) {
            const char *mangled = mangle_generic_name(g_arena, g_intern,
                t->stub.name, t->stub.type_args, t->stub.type_arg_count);
            if (g_mono) {
                for (int i = 0; i < g_mono->count; i++) {
                    if (g_mono->entries[i].mangled_name == mangled &&
                        g_mono->entries[i].concrete_type)
                        return g_mono->entries[i].concrete_type;
                }
            }
        }
        Symbol *sym = symtab_lookup(g_symtab, t->stub.name);
        if (sym && sym->type) {
            if ((sym->type->kind == TYPE_STRUCT && sym->type->struc.field_count > 0) ||
                (sym->type->kind == TYPE_UNION && sym->type->unio.variant_count > 0))
                return sym->type;
        }
        if (g_mono) {
            for (int i = 0; i < g_mono->count; i++) {
                if (g_mono->entries[i].mangled_name == t->stub.name &&
                    g_mono->entries[i].concrete_type)
                    return g_mono->entries[i].concrete_type;
            }
        }
    }
    return t;
}

/* Recursively collect types that need generated eq functions */
static void collect_eq_types(Type *t, TypeSet *eqs) {
    if (!t) return;
    /* Apply type variable substitution if available */
    if (g_subst && type_contains_type_var(t)) {
        t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
        if (!type_contains_type_var(t))
            mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    }
    if (type_contains_type_var(t)) return;
    /* Resolve stubs before checking if eq func is needed */
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    /* Fixed-array types: recurse into element, don't generate standalone eq func */
    if (t->kind == TYPE_FIXED_ARRAY) {
        collect_eq_types(t->fixed_array.elem, eqs);
        return;
    }
    if (!type_needs_eq_func(t)) return;
    t = resolve_struct_stub(t);
    /* Strip const for eq function collection — constness doesn't affect equality */
    if (t->is_const) {
        Type *nc = arena_alloc(g_arena, sizeof(Type));
        *nc = *t;
        nc->is_const = false;
        t = nc;
    }
    if (typeset_contains(eqs, t)) return;
    typeset_add(eqs, t);
    switch (t->kind) {
    case TYPE_STRUCT:
        if (!t->struc.is_c_union) {
            for (int i = 0; i < t->struc.field_count; i++)
                collect_eq_types(t->struc.fields[i].type, eqs);
        }
        break;
    case TYPE_UNION:
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload)
                collect_eq_types(t->unio.variants[i].payload, eqs);
        break;
    case TYPE_SLICE:
        collect_eq_types(t->slice.elem, eqs);
        break;
    case TYPE_OPTION:
        collect_eq_types(t->option.inner, eqs);
        break;
    default:
        break;
    }
}

/* Walk patterns looking for PAT_STRING_LIT to register str eq */
static void collect_eq_from_pattern(Pattern *pat, TypeSet *eqs) {
    if (!pat) return;
    switch (pat->kind) {
    case PAT_STRING_LIT:
        collect_eq_types(type_str(), eqs);
        break;
    case PAT_SOME:
        if (pat->some_pat.inner) collect_eq_from_pattern(pat->some_pat.inner, eqs);
        break;
    case PAT_VARIANT:
        if (pat->variant.payload) collect_eq_from_pattern(pat->variant.payload, eqs);
        break;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            collect_eq_from_pattern(pat->struc.fields[i].pattern, eqs);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            collect_eq_from_pattern(pat->tuple_pat.patterns[i], eqs);
        break;
    case PAT_OR:
        for (int i = 0; i < pat->or_pat.alt_count; i++)
            collect_eq_from_pattern(pat->or_pat.alts[i], eqs);
        break;
    default:
        break;
    }
}

static void collect_types_expr(Expr *e, TypeSet *slices, TypeSet *options, TypeSet *fns) {
    if (!e) return;
    collect_types_in_type(e->type, slices, options, fns);

    switch (e->kind) {
    case EXPR_BINARY:
        collect_types_expr(e->binary.left, slices, options, fns);
        collect_types_expr(e->binary.right, slices, options, fns);
        if (g_eq_set && (e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ)) {
            Type *cmp_type = e->binary.left->type;
            if (cmp_type) collect_eq_types(cmp_type, g_eq_set);
        }
        break;
    case EXPR_UNARY_PREFIX:
        collect_types_expr(e->unary_prefix.operand, slices, options, fns);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_types_expr(e->unary_postfix.operand, slices, options, fns);
        break;
    case EXPR_CALL:
        collect_types_expr(e->call.func, slices, options, fns);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_types_expr(e->call.args[i], slices, options, fns);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_types_expr(e->field.object, slices, options, fns);
        break;
    case EXPR_INDEX:
        collect_types_expr(e->index.object, slices, options, fns);
        collect_types_expr(e->index.index, slices, options, fns);
        break;
    case EXPR_SLICE:
        collect_types_expr(e->slice.object, slices, options, fns);
        if (e->slice.lo) collect_types_expr(e->slice.lo, slices, options, fns);
        if (e->slice.hi) collect_types_expr(e->slice.hi, slices, options, fns);
        break;
    case EXPR_IF:
        collect_types_expr(e->if_expr.cond, slices, options, fns);
        collect_types_expr(e->if_expr.then_body, slices, options, fns);
        if (e->if_expr.else_body) collect_types_expr(e->if_expr.else_body, slices, options, fns);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_types_expr(e->block.stmts[i], slices, options, fns);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_types_expr(e->func.body[i], slices, options, fns);
        /* Also check param types */
        for (int i = 0; i < e->func.param_count; i++)
            collect_types_in_type(e->func.params[i].type, slices, options, fns);
        break;
    case EXPR_LET:
        collect_types_in_type(e->let_expr.let_type, slices, options, fns);
        collect_types_expr(e->let_expr.let_init, slices, options, fns);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_types_expr(e->return_expr.value, slices, options, fns);
        break;
    case EXPR_ASSIGN:
        collect_types_expr(e->assign.target, slices, options, fns);
        collect_types_expr(e->assign.value, slices, options, fns);
        break;
    case EXPR_CAST:
        collect_types_in_type(e->cast.target, slices, options, fns);
        collect_types_expr(e->cast.operand, slices, options, fns);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_types_expr(e->struct_lit.fields[i].value, slices, options, fns);
        break;
    case EXPR_SOME:
        collect_types_expr(e->some_expr.value, slices, options, fns);
        break;
    case EXPR_ARRAY_LIT:
        collect_types_in_type(e->array_lit.elem_type, slices, options, fns);
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_types_expr(e->array_lit.elems[i], slices, options, fns);
        break;
    case EXPR_SLICE_LIT:
        collect_types_in_type(e->slice_lit.elem_type, slices, options, fns);
        collect_types_expr(e->slice_lit.ptr_expr, slices, options, fns);
        collect_types_expr(e->slice_lit.len_expr, slices, options, fns);
        break;
    case EXPR_MATCH:
        collect_types_expr(e->match_expr.subject, slices, options, fns);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            if (g_eq_set)
                collect_eq_from_pattern(e->match_expr.arms[i].pattern, g_eq_set);
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_types_expr(e->match_expr.arms[i].body[j], slices, options, fns);
        }
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_types_expr(e->loop_expr.body[i], slices, options, fns);
        break;
    case EXPR_FOR:
        collect_types_expr(e->for_expr.iter, slices, options, fns);
        if (e->for_expr.range_end) collect_types_expr(e->for_expr.range_end, slices, options, fns);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_types_expr(e->for_expr.body[i], slices, options, fns);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_types_expr(e->break_expr.value, slices, options, fns);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_types_expr(e->alloc_expr.size_expr, slices, options, fns);
        if (e->alloc_expr.init_expr) collect_types_expr(e->alloc_expr.init_expr, slices, options, fns);
        break;
    case EXPR_FREE:
        collect_types_expr(e->free_expr.operand, slices, options, fns);
        break;
    case EXPR_ATOMIC_LOAD:
        collect_types_expr(e->atomic_load.ptr, slices, options, fns);
        break;
    case EXPR_ATOMIC_STORE:
        collect_types_expr(e->atomic_store.ptr, slices, options, fns);
        collect_types_expr(e->atomic_store.value, slices, options, fns);
        break;
    case EXPR_ASSERT:
        collect_types_expr(e->assert_expr.condition, slices, options, fns);
        if (e->assert_expr.message)
            collect_types_expr(e->assert_expr.message, slices, options, fns);
        break;
    case EXPR_DEFER:
        collect_types_expr(e->defer_expr.value, slices, options, fns);
        break;
    case EXPR_SIZEOF:
        collect_types_in_type(e->sizeof_expr.target, slices, options, fns);
        break;
    case EXPR_ALIGNOF:
        collect_types_in_type(e->alignof_expr.target, slices, options, fns);
        break;
    case EXPR_DEFAULT:
        collect_types_in_type(e->default_expr.target, slices, options, fns);
        break;
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            if (!e->interp_string.segments[i].is_literal)
                collect_types_expr(e->interp_string.segments[i].expr, slices, options, fns);
        }
        break;
    default:
        break;
    }
}

/* ---- Lambda collection ---- */

typedef struct {
    Expr **exprs;
    int count;
    int cap;
} LambdaSet;

/* ---- Trampoline set ---- */
/* Tracks FC functions that need C-compatible trampolines for extern call boundaries.
 * A trampoline wraps an FC function (which has an extra void* _ctx param) into a
 * plain C function pointer compatible with the extern's expected signature. */

typedef struct {
    const char *name;   /* C function name (codegen_name or lifted_name) */
    Type *type;         /* TYPE_FUNC — the function's type */
} TrampolineEntry;

typedef struct {
    TrampolineEntry *entries;
    int count;
    int cap;
} TrampolineSet;

static bool trampolineset_contains(TrampolineSet *ts, const char *name) {
    for (int i = 0; i < ts->count; i++) {
        if (strcmp(ts->entries[i].name, name) == 0) return true;
    }
    return false;
}

static void trampolineset_add(TrampolineSet *ts, const char *name, Type *type) {
    if (!trampolineset_contains(ts, name)) {
        TrampolineEntry entry = { name, type };
        DA_APPEND(ts->entries, ts->count, ts->cap, entry);
    }
}

static void collect_trampolines_expr(Expr *e, TrampolineSet *ts);

/* Pre-pass for module-member initializers: assigns a unique backing-array
 * name to every EXPR_ARRAY_LIT inside a const-safe init tree, and appends
 * the node to g_const_backings so the file-scope emission pass can emit
 * `static T _fc_const_backing_N[] = {...};` before the module-member defs.
 *
 * Recurses only through expression kinds that is_const_expr accepts — other
 * kinds cannot appear here because pass2 has already gated the init.  A
 * special case: EXPR_STRUCT_LIT fields whose type is a fixed array take an
 * inline aggregate initializer at the use site rather than a backing array,
 * so we skip backing assignment for those specific children.
 */
static void collect_const_backings(Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_ARRAY_LIT: {
        /* Empty arrays don't need a backing — we emit a NULL/0 slice at
         * the use site.  C11 rejects zero-length aggregate initializers. */
        if (e->array_lit.elem_count > 0) {
            char buf[32];
            int n = snprintf(buf, sizeof buf, "_fc_const_backing_%d",
                             g_const_backing_counter++);
            e->array_lit.codegen_backing_name = arena_strdup(g_arena, buf, n);
            DA_APPEND(g_const_backings, g_const_backing_count,
                      g_const_backing_cap, e);
        }
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_const_backings(e->array_lit.elems[i]);
        break;
    }
    case EXPR_SLICE_LIT:
        collect_const_backings(e->slice_lit.ptr_expr);
        collect_const_backings(e->slice_lit.len_expr);
        break;
    case EXPR_STRUCT_LIT: {
        Type *st = e->type;
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            /* Fixed-array fields take an inline aggregate — the inner
             * array-lit is emitted raw, no backing needed.  Identify by
             * looking up the field type in the resolved struct. */
            bool is_fixed_field = false;
            if (st && st->kind == TYPE_STRUCT) {
                const char *fname = e->struct_lit.fields[i].name;
                for (int f = 0; f < st->struc.field_count; f++) {
                    if (st->struc.fields[f].name == fname) {
                        is_fixed_field = st->struc.fields[f].type->kind == TYPE_FIXED_ARRAY;
                        break;
                    }
                }
            }
            Expr *v = e->struct_lit.fields[i].value;
            if (is_fixed_field && v && v->kind == EXPR_ARRAY_LIT) {
                /* Recurse into elements but do not lift the outer array */
                for (int j = 0; j < v->array_lit.elem_count; j++)
                    collect_const_backings(v->array_lit.elems[j]);
            } else {
                collect_const_backings(v);
            }
        }
        break;
    }
    case EXPR_SOME:
        collect_const_backings(e->some_expr.value);
        break;
    case EXPR_CALL:
        if (e->call.func->kind == EXPR_FIELD &&
            e->call.func->field.is_variant_constructor) {
            for (int i = 0; i < e->call.arg_count; i++)
                collect_const_backings(e->call.args[i]);
        }
        break;
    case EXPR_UNARY_PREFIX:
        collect_const_backings(e->unary_prefix.operand);
        break;
    case EXPR_BINARY:
        collect_const_backings(e->binary.left);
        collect_const_backings(e->binary.right);
        break;
    case EXPR_CAST:
        collect_const_backings(e->cast.operand);
        break;
    default:
        /* Leaves: literals, extern-const/no-payload variant EXPR_FIELD, type
         * operators.  Nothing to lift. */
        break;
    }
}

static void collect_trampolines_expr(Expr *e, TrampolineSet *ts) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_CALL:
        /* Check if this is an extern call with function-type arguments */
        if (e->call.is_extern_call) {
            Type *call_ft = e->call.func->type;
            for (int i = 0; i < e->call.arg_count; i++) {
                Type *pt = (call_ft && call_ft->kind == TYPE_FUNC && i < call_ft->func.param_count)
                    ? call_ft->func.param_types[i] : NULL;
                if (!pt || pt->kind != TYPE_FUNC) continue;
                Expr *arg = e->call.args[i];
                if (arg->kind == EXPR_IDENT && !arg->ident.is_local && arg->type &&
                    arg->type->kind == TYPE_FUNC) {
                    /* Top-level function passed at extern boundary */
                    const char *fname = arg->ident.codegen_name
                        ? arg->ident.codegen_name : arg->ident.name;
                    trampolineset_add(ts, fname, arg->type);
                } else if (arg->kind == EXPR_FUNC && arg->func.capture_count == 0 &&
                           arg->func.lifted_name && arg->type &&
                           arg->type->kind == TYPE_FUNC) {
                    /* Non-capturing lambda passed at extern boundary */
                    trampolineset_add(ts, arg->func.lifted_name, arg->type);
                }
            }
        }
        collect_trampolines_expr(e->call.func, ts);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_trampolines_expr(e->call.args[i], ts);
        break;
    case EXPR_BINARY:
        collect_trampolines_expr(e->binary.left, ts);
        collect_trampolines_expr(e->binary.right, ts);
        break;
    case EXPR_UNARY_PREFIX:
        /* &f on a top-level function or non-capturing lambda yields a raw C
         * function pointer — record it so the trampoline is emitted. */
        if (e->unary_prefix.op == TOK_AMP) {
            Expr *operand = e->unary_prefix.operand;
            if (operand->kind == EXPR_IDENT && !operand->ident.is_local &&
                operand->type && operand->type->kind == TYPE_FUNC) {
                const char *name = operand->ident.codegen_name
                    ? operand->ident.codegen_name : operand->ident.name;
                trampolineset_add(ts, name, operand->type);
            } else if (operand->kind == EXPR_FIELD && operand->field.codegen_name &&
                       operand->type && operand->type->kind == TYPE_FUNC) {
                trampolineset_add(ts, operand->field.codegen_name, operand->type);
            } else if (operand->kind == EXPR_FUNC && operand->func.capture_count == 0 &&
                       operand->func.lifted_name && operand->type &&
                       operand->type->kind == TYPE_FUNC) {
                trampolineset_add(ts, operand->func.lifted_name, operand->type);
            }
        }
        collect_trampolines_expr(e->unary_prefix.operand, ts);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_trampolines_expr(e->unary_postfix.operand, ts);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_trampolines_expr(e->field.object, ts);
        break;
    case EXPR_INDEX:
        collect_trampolines_expr(e->index.object, ts);
        collect_trampolines_expr(e->index.index, ts);
        break;
    case EXPR_SLICE:
        collect_trampolines_expr(e->slice.object, ts);
        if (e->slice.lo) collect_trampolines_expr(e->slice.lo, ts);
        if (e->slice.hi) collect_trampolines_expr(e->slice.hi, ts);
        break;
    case EXPR_IF:
        collect_trampolines_expr(e->if_expr.cond, ts);
        collect_trampolines_expr(e->if_expr.then_body, ts);
        if (e->if_expr.else_body) collect_trampolines_expr(e->if_expr.else_body, ts);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_trampolines_expr(e->block.stmts[i], ts);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_trampolines_expr(e->func.body[i], ts);
        break;
    case EXPR_LET:
        collect_trampolines_expr(e->let_expr.let_init, ts);
        break;
    case EXPR_LET_DESTRUCT:
        collect_trampolines_expr(e->let_destruct.init, ts);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_trampolines_expr(e->return_expr.value, ts);
        break;
    case EXPR_ASSIGN:
        collect_trampolines_expr(e->assign.target, ts);
        collect_trampolines_expr(e->assign.value, ts);
        break;
    case EXPR_CAST:
        collect_trampolines_expr(e->cast.operand, ts);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_trampolines_expr(e->struct_lit.fields[i].value, ts);
        break;
    case EXPR_SOME:
        collect_trampolines_expr(e->some_expr.value, ts);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_trampolines_expr(e->array_lit.elems[i], ts);
        break;
    case EXPR_SLICE_LIT:
        collect_trampolines_expr(e->slice_lit.ptr_expr, ts);
        collect_trampolines_expr(e->slice_lit.len_expr, ts);
        break;
    case EXPR_MATCH:
        collect_trampolines_expr(e->match_expr.subject, ts);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_trampolines_expr(e->match_expr.arms[i].body[j], ts);
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_trampolines_expr(e->loop_expr.body[i], ts);
        break;
    case EXPR_FOR:
        collect_trampolines_expr(e->for_expr.iter, ts);
        if (e->for_expr.range_end) collect_trampolines_expr(e->for_expr.range_end, ts);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_trampolines_expr(e->for_expr.body[i], ts);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_trampolines_expr(e->break_expr.value, ts);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_trampolines_expr(e->alloc_expr.size_expr, ts);
        if (e->alloc_expr.init_expr) collect_trampolines_expr(e->alloc_expr.init_expr, ts);
        break;
    case EXPR_FREE:
        collect_trampolines_expr(e->free_expr.operand, ts);
        break;
    case EXPR_ATOMIC_LOAD:
        collect_trampolines_expr(e->atomic_load.ptr, ts);
        break;
    case EXPR_ATOMIC_STORE:
        collect_trampolines_expr(e->atomic_store.ptr, ts);
        collect_trampolines_expr(e->atomic_store.value, ts);
        break;
    case EXPR_ASSERT:
        collect_trampolines_expr(e->assert_expr.condition, ts);
        if (e->assert_expr.message)
            collect_trampolines_expr(e->assert_expr.message, ts);
        break;
    case EXPR_DEFER:
        collect_trampolines_expr(e->defer_expr.value, ts);
        break;
    default:
        break;
    }
}

static void collect_lambdas_expr(Expr *e, LambdaSet *ls) {
    if (!e) return;
    if (e->kind == EXPR_FUNC && e->func.lifted_name) {
        DA_APPEND(ls->exprs, ls->count, ls->cap, e);
    }
    switch (e->kind) {
    case EXPR_BINARY:
        collect_lambdas_expr(e->binary.left, ls);
        collect_lambdas_expr(e->binary.right, ls);
        break;
    case EXPR_UNARY_PREFIX:
        collect_lambdas_expr(e->unary_prefix.operand, ls);
        break;
    case EXPR_UNARY_POSTFIX:
        collect_lambdas_expr(e->unary_postfix.operand, ls);
        break;
    case EXPR_CALL:
        collect_lambdas_expr(e->call.func, ls);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_lambdas_expr(e->call.args[i], ls);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        collect_lambdas_expr(e->field.object, ls);
        break;
    case EXPR_INDEX:
        collect_lambdas_expr(e->index.object, ls);
        collect_lambdas_expr(e->index.index, ls);
        break;
    case EXPR_SLICE:
        collect_lambdas_expr(e->slice.object, ls);
        if (e->slice.lo) collect_lambdas_expr(e->slice.lo, ls);
        if (e->slice.hi) collect_lambdas_expr(e->slice.hi, ls);
        break;
    case EXPR_IF:
        collect_lambdas_expr(e->if_expr.cond, ls);
        collect_lambdas_expr(e->if_expr.then_body, ls);
        if (e->if_expr.else_body) collect_lambdas_expr(e->if_expr.else_body, ls);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            collect_lambdas_expr(e->block.stmts[i], ls);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            collect_lambdas_expr(e->func.body[i], ls);
        break;
    case EXPR_LET:
        collect_lambdas_expr(e->let_expr.let_init, ls);
        break;
    case EXPR_LET_DESTRUCT:
        collect_lambdas_expr(e->let_destruct.init, ls);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value) collect_lambdas_expr(e->return_expr.value, ls);
        break;
    case EXPR_ASSIGN:
        collect_lambdas_expr(e->assign.target, ls);
        collect_lambdas_expr(e->assign.value, ls);
        break;
    case EXPR_CAST:
        collect_lambdas_expr(e->cast.operand, ls);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            collect_lambdas_expr(e->struct_lit.fields[i].value, ls);
        break;
    case EXPR_SOME:
        collect_lambdas_expr(e->some_expr.value, ls);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_lambdas_expr(e->array_lit.elems[i], ls);
        break;
    case EXPR_SLICE_LIT:
        collect_lambdas_expr(e->slice_lit.ptr_expr, ls);
        collect_lambdas_expr(e->slice_lit.len_expr, ls);
        break;
    case EXPR_MATCH:
        collect_lambdas_expr(e->match_expr.subject, ls);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                collect_lambdas_expr(e->match_expr.arms[i].body[j], ls);
        }
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            collect_lambdas_expr(e->loop_expr.body[i], ls);
        break;
    case EXPR_FOR:
        collect_lambdas_expr(e->for_expr.iter, ls);
        if (e->for_expr.range_end) collect_lambdas_expr(e->for_expr.range_end, ls);
        for (int i = 0; i < e->for_expr.body_count; i++)
            collect_lambdas_expr(e->for_expr.body[i], ls);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value) collect_lambdas_expr(e->break_expr.value, ls);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.size_expr) collect_lambdas_expr(e->alloc_expr.size_expr, ls);
        if (e->alloc_expr.init_expr) collect_lambdas_expr(e->alloc_expr.init_expr, ls);
        break;
    case EXPR_FREE:
        collect_lambdas_expr(e->free_expr.operand, ls);
        break;
    case EXPR_ATOMIC_LOAD:
        collect_lambdas_expr(e->atomic_load.ptr, ls);
        break;
    case EXPR_ATOMIC_STORE:
        collect_lambdas_expr(e->atomic_store.ptr, ls);
        collect_lambdas_expr(e->atomic_store.value, ls);
        break;
    case EXPR_ASSERT:
        collect_lambdas_expr(e->assert_expr.condition, ls);
        if (e->assert_expr.message)
            collect_lambdas_expr(e->assert_expr.message, ls);
        break;
    case EXPR_DEFER:
        collect_lambdas_expr(e->defer_expr.value, ls);
        break;
    default:
        break;
    }
}

/* ---- Eq function generation ---- */

static void emit_eq_func_name(Type *t, FILE *out) {
    /* Strip const — equality semantics don't depend on constness */
    Type tmp;
    if (t->is_const) { tmp = *t; tmp.is_const = false; t = &tmp; }
    fprintf(out, "fc_eq_");
    emit_type_ident(t, out);
}

static void emit_eq_forward(Type *t, FILE *out) {
    fprintf(out, "static inline bool ");
    emit_eq_func_name(t, out);
    fprintf(out, "(");
    emit_type(t, out);
    fprintf(out, " a, ");
    emit_type(t, out);
    fprintf(out, " b);\n");
}

/* Emit the comparison expression for two values of type t.
   a_prefix/b_prefix are "a"/"b" (or "a.field"/"b.field"). */
static void emit_value_eq(Type *t, const char *a_expr, const char *b_expr, FILE *out) {
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if (type_needs_eq_func(t)) {
        emit_eq_func_name(t, out);
        fprintf(out, "(%s, %s)", a_expr, b_expr);
    } else {
        fprintf(out, "%s == %s", a_expr, b_expr);
    }
}

static void emit_eq_func(Type *t, FILE *out) {
    fprintf(out, "static inline bool ");
    emit_eq_func_name(t, out);
    fprintf(out, "(");
    emit_type(t, out);
    fprintf(out, " a, ");
    emit_type(t, out);
    fprintf(out, " b) {\n");

    t = resolve_struct_stub(t);
    switch (t->kind) {
    case TYPE_STRUCT: {
        if (t->struc.is_c_union) {
            /* Extern unions: byte-level comparison via memcmp */
            fprintf(out, "    return memcmp(&a, &b, sizeof(");
            emit_type(t, out);
            fprintf(out, ")) == 0;\n");
        } else {
            int fc = t->struc.field_count;
            if (fc == 0) {
                fprintf(out, "    (void)a; (void)b;\n    return true;\n");
            } else {
                fprintf(out, "    return ");
                for (int i = 0; i < fc; i++) {
                    if (i > 0) fprintf(out, " && ");
                    Type *ft = t->struc.fields[i].type;
                    const char *fname = c_safe_ident(g_intern, t->struc.fields[i].name);
                    if (ft->kind == TYPE_FIXED_ARRAY) {
                        Type *elem = ft->fixed_array.elem;
                        if (!type_needs_eq_func(elem) && !type_is_float(elem)) {
                            fprintf(out, "memcmp(a.%s, b.%s, sizeof(a.%s)) == 0",
                                    fname, fname, fname);
                        } else {
                            /* Element-wise comparison for complex types */
                            fprintf(out, "({ bool _eq = true; "
                                    "for (int _k = 0; _k < %lld; _k++) if (!",
                                    (long long)ft->fixed_array.size);
                            if (type_needs_eq_func(elem)) {
                                emit_eq_func_name(elem, out);
                                fprintf(out, "(a.%s[_k], b.%s[_k])", fname, fname);
                            } else {
                                fprintf(out, "(a.%s[_k] == b.%s[_k])", fname, fname);
                            }
                            fprintf(out, ") { _eq = false; break; } _eq; })");
                        }
                    } else {
                        char a_buf[256], b_buf[256];
                        snprintf(a_buf, sizeof(a_buf), "a.%s", fname);
                        snprintf(b_buf, sizeof(b_buf), "b.%s", fname);
                        emit_value_eq(ft, a_buf, b_buf, out);
                    }
                }
                fprintf(out, ";\n");
            }
        }
        break;
    }
    case TYPE_UNION: {
        /* Check if any variant has a payload */
        bool has_payload = false;
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload) { has_payload = true; break; }

        if (!has_payload) {
            /* Tag-only union — just compare tags */
            fprintf(out, "    return a.tag == b.tag;\n");
        } else {
            fprintf(out, "    if (a.tag != b.tag) return false;\n");
            fprintf(out, "    switch (a.tag) {\n");
            const char *uname = t->unio.name;
            for (int i = 0; i < t->unio.variant_count; i++) {
                fprintf(out, "    case %s_tag_%s: ", uname, t->unio.variants[i].name);
                if (t->unio.variants[i].payload) {
                    char a_buf[256], b_buf[256];
                    const char *vfield = c_safe_ident(g_intern, t->unio.variants[i].name);
                    snprintf(a_buf, sizeof(a_buf), "a.%s", vfield);
                    snprintf(b_buf, sizeof(b_buf), "b.%s", vfield);
                    fprintf(out, "return ");
                    emit_value_eq(t->unio.variants[i].payload, a_buf, b_buf, out);
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "return true;\n");
                }
            }
            fprintf(out, "    }\n");
            fprintf(out, "    return true;\n");
        }
        break;
    }
    case TYPE_SLICE: {
        Type *elem = t->slice.elem;
        fprintf(out, "    if (a.len != b.len) return false;\n");
        fprintf(out, "    if (a.len == 0) return true;\n");
        /* Use memcmp for non-float primitives that don't need eq funcs */
        if (!type_needs_eq_func(elem) && !type_is_float(elem)) {
            fprintf(out, "    return memcmp(a.ptr, b.ptr, fc_to_size(a.len) * sizeof(");
            emit_type(elem, out);
            fprintf(out, ")) == 0;\n");
        } else {
            fprintf(out, "    for (int64_t _i = 0; _i < a.len; _i++)\n");
            fprintf(out, "        if (!");
            if (type_needs_eq_func(elem)) {
                emit_eq_func_name(elem, out);
                fprintf(out, "(a.ptr[_i], b.ptr[_i])");
            } else {
                /* float — use C == */
                fprintf(out, "(a.ptr[_i] == b.ptr[_i])");
            }
            fprintf(out, ") return false;\n");
            fprintf(out, "    return true;\n");
        }
        break;
    }
    case TYPE_OPTION:
        fprintf(out, "    if (a.has_value != b.has_value) return false;\n");
        fprintf(out, "    if (!a.has_value) return true;\n");
        fprintf(out, "    return ");
        emit_value_eq(t->option.inner, "a.value", "b.value", out);
        fprintf(out, ";\n");
        break;
    case TYPE_FUNC:
        fprintf(out, "    return a.fn_ptr == b.fn_ptr && a.ctx == b.ctx;\n");
        break;
    default:
        fprintf(out, "    return false; /* unsupported type */\n");
        break;
    }

    fprintf(out, "}\n");
}

/* Collect all top-level decls plus module child decls into a flat array */
/* Recursively flatten module decls into a single array */
static void flatten_decls(Decl **decls, int decl_count, Decl ***out, int *count, int *cap) {
    for (int i = 0; i < decl_count; i++) {
        Decl *d = decls[i];
        if (d->kind == DECL_MODULE) {
            flatten_decls(d->module.decls, d->module.decl_count, out, count, cap);
        } else {
            DA_APPEND(*out, *count, *cap, d);
        }
    }
}

/* Find a struct/union name referenced by value in a type (not through
 * pointer/slice/option/func). Mirrors monomorph.c's find_by_value_dep but
 * operates on decl-level types (which may still contain stubs). */
static const char *find_by_value_dep_name(Type *type) {
    if (!type) return NULL;
    switch (type->kind) {
    case TYPE_STRUCT: return type->struc.name;
    case TYPE_UNION:  return type->unio.name;
    case TYPE_STUB:
        /* A concrete generic-instance field depends on its monomorphized
         * instance (box<int32> → box_int32), so emit it after that instance. */
        if (type->stub.type_arg_count > 0 && !type_contains_type_var(type))
            return mangle_generic_name(g_arena, g_intern, type->stub.name,
                                       type->stub.type_args, type->stub.type_arg_count);
        return type->stub.name;
    case TYPE_FIXED_ARRAY: return find_by_value_dep_name(type->fixed_array.elem);
    case TYPE_OPTION:
        /* A by-value option-of-struct/union/stub embeds its inner aggregate, so
         * the enclosing def must be ordered after the inner's def — the option
         * body (fc_option_<inner>) is emitted in the interleave immediately
         * after the inner def. Option-of-pointer/scalar/fn carries no by-value
         * aggregate dependency (inner recurses to NULL). */
        return find_by_value_dep_name(type->option.inner);
    default: return NULL;
    }
}

enum { DECL_TOPO_UNVISITED = 0, DECL_TOPO_VISITING = 1, DECL_TOPO_DONE = 2 };

static const char *decl_su_name(Decl *d) {
    if (d->kind == DECL_STRUCT) return d->struc.name;
    if (d->kind == DECL_UNION)  return d->unio.name;
    return NULL;
}

/* A struct/union definition to emit: either a top-level decl or a monomorphized
 * instance (which includes synthesized tuples). Both kinds participate in one
 * topological sort so by-value dependencies emit before dependents in EITHER
 * direction (a top-level struct holding a tuple, or a generic instance holding a
 * top-level struct). */
typedef struct {
    const char *name;
    Decl *decl;          /* non-NULL: top-level struct/union decl */
    MonoInstance *mi;    /* non-NULL: monomorphized struct/union/tuple instance */
} SuDef;

static void sudef_deps(SuDef *s, const char **deps, int *dep_count, int max) {
    *dep_count = 0;
    StructField *fields = NULL; int field_count = 0;
    UnionVariant *variants = NULL; int variant_count = 0;
    if (s->decl) {
        Decl *d = s->decl;
        if (d->kind == DECL_STRUCT && !d->struc.is_extern) {
            fields = d->struc.fields; field_count = d->struc.field_count;
        } else if (d->kind == DECL_UNION) {
            variants = d->unio.variants; variant_count = d->unio.variant_count;
        }
    } else if (s->mi && s->mi->concrete_type) {
        Type *ct = s->mi->concrete_type;
        if (ct->kind == TYPE_STRUCT) { fields = ct->struc.fields; field_count = ct->struc.field_count; }
        else if (ct->kind == TYPE_UNION) { variants = ct->unio.variants; variant_count = ct->unio.variant_count; }
    }
    for (int f = 0; f < field_count && *dep_count < max; f++) {
        const char *dep = find_by_value_dep_name(fields[f].type);
        if (dep) deps[(*dep_count)++] = dep;
    }
    for (int v = 0; v < variant_count && *dep_count < max; v++) {
        const char *dep = find_by_value_dep_name(variants[v].payload);
        if (dep) deps[(*dep_count)++] = dep;
    }
}

static void topo_visit_sudef(SuDef *items, int n, int idx, int *state,
                             int *order, int *order_count) {
    if (state[idx] != DECL_TOPO_UNVISITED) return;
    state[idx] = DECL_TOPO_VISITING;
    const char *deps[64];
    int dep_count = 0;
    sudef_deps(&items[idx], deps, &dep_count, 64);
    for (int di = 0; di < dep_count; di++) {
        for (int j = 0; j < n; j++) {
            if (j != idx && items[j].name == deps[di]) {
                topo_visit_sudef(items, n, j, state, order, order_count);
                break;
            }
        }
    }
    order[(*order_count)++] = idx;
    state[idx] = DECL_TOPO_DONE;
}

/* Recursively collect unique from_lib strings from module declarations */
static void collect_from_libs(Decl *d, const char **seen, int *count, int cap) {
    if (d->kind != DECL_MODULE) return;
    if (d->module.from_lib) {
        for (int i = 0; i < *count; i++)
            if (strcmp(seen[i], d->module.from_lib) == 0) return;
        if (*count < cap)
            seen[(*count)++] = d->module.from_lib;
    }
    for (int i = 0; i < d->module.decl_count; i++)
        collect_from_libs(d->module.decls[i], seen, count, cap);
}

/* ---- Define collection ---- */
/* Tracks #define macros from module `define` annotations */
typedef struct {
    const char *macro;
    const char *value;
} CDefine;

static void collect_defines(Decl *d, CDefine *defs, int *count, int cap) {
    if (d->kind != DECL_MODULE) return;
    if (d->module.define_macro) {
        for (int i = 0; i < *count; i++) {
            if (strcmp(defs[i].macro, d->module.define_macro) == 0) {
                if (strcmp(defs[i].value, d->module.define_value) != 0) {
                    diag_error(d->loc,
                        "conflicting define for '%s': '%s' vs '%s'",
                        d->module.define_macro, defs[i].value, d->module.define_value);
                }
                return;
            }
        }
        if (*count < cap) {
            defs[*count].macro = d->module.define_macro;
            defs[*count].value = d->module.define_value;
            (*count)++;
        }
    }
    for (int i = 0; i < d->module.decl_count; i++)
        collect_defines(d->module.decls[i], defs, count, cap);
}

/* ---- Feature detection ---- */
/* Lightweight AST scan to determine which feature-gated headers are needed */
static bool g_needs_stdio;
static bool g_needs_math;
static bool g_needs_float;
static bool g_backtraces;

/* Symbol map populated during codegen when g_backtraces is set; emitted as
 * _fc_symtab[] in the preamble so fc_dump_backtrace() can render FC-level names. */
typedef struct {
    const char *c_name;   /* mangled C identifier as emitted */
    const char *fc_name;  /* display name (e.g. "foo<int32>", "<lambda at f.fc:N>") */
    const char *file;     /* FC source file of the definition */
    int line;             /* FC source line of the definition */
} FcSymEntry;
static FcSymEntry *g_symmap = NULL;
static int g_symmap_count = 0;
static int g_symmap_cap = 0;

static void symmap_reset(void) {
    free(g_symmap);
    g_symmap = NULL;
    g_symmap_count = 0;
    g_symmap_cap = 0;
}

/* Add a function to the symbol table for --backtraces.  Pass fc_name=NULL to
 * mark an internal helper (fc_oob, fc_oob_sub) whose backtrace frames should
 * be skipped from the printed backtrace.  Otherwise fc_name is the FC display
 * name (e.g. "foo<int32>", "<lambda at f.fc:N>"). */
static void symmap_add(const char *c_name, const char *fc_name,
                       const char *file, int line) {
    if (!g_backtraces || !c_name) return;
    /* Dedup: a function can be emitted in multiple walks; skip if seen. */
    for (int i = 0; i < g_symmap_count; i++) {
        if (g_symmap[i].c_name && strcmp(g_symmap[i].c_name, c_name) == 0)
            return;
    }
    FcSymEntry e = { c_name, fc_name,
                     file ? file : "<unknown>", line };
    DA_APPEND(g_symmap, g_symmap_count, g_symmap_cap, e);
}

/* Build "template<T1, T2>" for a monomorphized instance into an arena
 * allocation. Returns a stable pointer suitable for storing in the symmap. */
static const char *fmt_mono_display(Arena *arena, const char *template_name,
                                    Type **type_args, int type_arg_count) {
    char buf[512];
    int off = snprintf(buf, sizeof buf, "%s<", template_name);
    for (int i = 0; i < type_arg_count && off < (int)sizeof buf; i++) {
        const char *tn = type_name(type_args[i]);
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%s%s",
                        i == 0 ? "" : ", ", tn ? tn : "?");
    }
    if (off < (int)sizeof buf) off += snprintf(buf + off, sizeof buf - (size_t)off, ">");
    if (off >= (int)sizeof buf) off = (int)sizeof buf - 1;
    return arena_strdup(arena, buf, off);
}

/* Lambda display name.  The location is rendered separately as
 * "defined at file:line" by fc_dump_backtrace, so the name itself just reads
 * "<lambda>" — duplicating the location in the name would be redundant. */
static const char *fmt_lambda_display(Arena *arena, const char *file, int line) {
    (void)arena; (void)file; (void)line;
    return "<lambda>";
}

static void detect_features_expr(Expr *e) {
    if (!e) return;
    if (g_needs_stdio && g_needs_math && g_needs_float) return; /* all found */

    switch (e->kind) {
    case EXPR_INTERP_STRING:
        g_needs_stdio = true;
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            if (e->interp_string.segments[i].expr)
                detect_features_expr(e->interp_string.segments[i].expr);
        }
        return;

    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        /* Direct float type properties resolved by pass2 */
        if (e->field.codegen_name) {
            const char *cn = e->field.codegen_name;
            if (strstr(cn, "NAN") || strstr(cn, "INFINITY"))
                g_needs_math = true;
            if (strstr(cn, "FLT_") || strstr(cn, "DBL_"))
                g_needs_float = true;
        }
        /* Type variable property access — conservatively check property name */
        if (e->field.object && e->field.object->kind == EXPR_TYPE_VAR_REF) {
            const char *prop = e->field.name;
            if (strcmp(prop, "nan") == 0 || strcmp(prop, "inf") == 0 ||
                strcmp(prop, "neg_inf") == 0)
                g_needs_math = true;
            if (strcmp(prop, "min") == 0 || strcmp(prop, "max") == 0 ||
                strcmp(prop, "epsilon") == 0)
                g_needs_float = true;
        }
        detect_features_expr(e->field.object);
        return;

    case EXPR_BINARY:
        /* Integer div/mod emits a by-zero abort with stderr message */
        if ((e->binary.op == TOK_SLASH || e->binary.op == TOK_PERCENT) &&
            e->type && type_is_integer(e->type))
            g_needs_stdio = true;
        detect_features_expr(e->binary.left);
        detect_features_expr(e->binary.right);
        return;
    case EXPR_UNARY_PREFIX:
        detect_features_expr(e->unary_prefix.operand);
        return;
    case EXPR_UNARY_POSTFIX:
        if (e->unary_postfix.op == TOK_BANG)
            g_needs_stdio = true;
        detect_features_expr(e->unary_postfix.operand);
        return;
    case EXPR_CALL:
        detect_features_expr(e->call.func);
        for (int i = 0; i < e->call.arg_count; i++)
            detect_features_expr(e->call.args[i]);
        return;
    case EXPR_CAST:
        detect_features_expr(e->cast.operand);
        return;
    case EXPR_IF:
        detect_features_expr(e->if_expr.cond);
        detect_features_expr(e->if_expr.then_body);
        detect_features_expr(e->if_expr.else_body);
        return;
    case EXPR_MATCH:
        detect_features_expr(e->match_expr.subject);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                detect_features_expr(e->match_expr.arms[i].body[j]);
        }
        return;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            detect_features_expr(e->block.stmts[i]);
        return;
    case EXPR_LET:
        detect_features_expr(e->let_expr.let_init);
        return;
    case EXPR_LET_DESTRUCT:
        detect_features_expr(e->let_destruct.init);
        return;
    case EXPR_ASSIGN:
        /* Assignment to fixed-array field emits an overflow abort with stderr */
        if (e->assign.target &&
            (e->assign.target->kind == EXPR_FIELD ||
             e->assign.target->kind == EXPR_DEREF_FIELD) &&
            e->assign.target->field.fixed_array_type)
            g_needs_stdio = true;
        detect_features_expr(e->assign.target);
        detect_features_expr(e->assign.value);
        return;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            detect_features_expr(e->loop_expr.body[i]);
        return;
    case EXPR_FOR:
        detect_features_expr(e->for_expr.iter);
        if (e->for_expr.range_end) detect_features_expr(e->for_expr.range_end);
        for (int i = 0; i < e->for_expr.body_count; i++)
            detect_features_expr(e->for_expr.body[i]);
        return;
    case EXPR_RETURN:
        detect_features_expr(e->return_expr.value);
        return;
    case EXPR_BREAK:
        detect_features_expr(e->break_expr.value);
        return;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            detect_features_expr(e->func.body[i]);
        return;
    case EXPR_SOME: {
        /* A null-sentinel some(p) over a not-provably-non-null pointer emits a
         * null-pointer abort via stderr (fc_null_some).  A type variable may
         * monomorphize to a pointer, so treat it as possibly guarded too — at
         * worst this pulls in stdio.h for a program that does not need it. */
        Type *inner = e->type && e->type->kind == TYPE_OPTION
                          ? e->type->option.inner : NULL;
        if (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_ANY_PTR ||
                      inner->kind == TYPE_TYPE_VAR) &&
            !ptr_value_provably_nonnull(e->some_expr.value))
            g_needs_stdio = true;
        detect_features_expr(e->some_expr.value);
        return;
    }
    case EXPR_ALLOC:
        detect_features_expr(e->alloc_expr.init_expr);
        detect_features_expr(e->alloc_expr.size_expr);
        return;
    case EXPR_FREE:
        detect_features_expr(e->free_expr.operand);
        return;
    case EXPR_ATOMIC_LOAD:
        detect_features_expr(e->atomic_load.ptr);
        return;
    case EXPR_ATOMIC_STORE:
        detect_features_expr(e->atomic_store.ptr);
        detect_features_expr(e->atomic_store.value);
        return;
    case EXPR_ASSERT:
        g_needs_stdio = true;
        detect_features_expr(e->assert_expr.condition);
        if (e->assert_expr.message)
            detect_features_expr(e->assert_expr.message);
        return;
    case EXPR_DEFER:
        detect_features_expr(e->defer_expr.value);
        return;
    case EXPR_INDEX:
        /* Slice indexing emits a bounds-check abort with stderr message */
        if (e->index.object && e->index.object->type &&
            e->index.object->type->kind == TYPE_SLICE)
            g_needs_stdio = true;
        detect_features_expr(e->index.object);
        detect_features_expr(e->index.index);
        return;
    case EXPR_SLICE:
        /* Subslice emits a bounds-check abort with stderr message */
        g_needs_stdio = true;
        detect_features_expr(e->slice.object);
        detect_features_expr(e->slice.lo);
        detect_features_expr(e->slice.hi);
        return;
    case EXPR_STRUCT_LIT:
        /* Struct literal with fixed-array field emits an overflow abort with stderr */
        if (e->type && e->type->kind == TYPE_STRUCT) {
            for (int f = 0; f < e->type->struc.field_count; f++) {
                if (e->type->struc.fields[f].type->kind == TYPE_FIXED_ARRAY) {
                    g_needs_stdio = true;
                    break;
                }
            }
        }
        for (int i = 0; i < e->struct_lit.field_count; i++)
            detect_features_expr(e->struct_lit.fields[i].value);
        return;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            detect_features_expr(e->array_lit.elems[i]);
        return;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            detect_features_expr(e->tuple_lit.elems[i]);
        return;
    case EXPR_SLICE_LIT:
        /* A runtime-len slice literal emits a negative-length abort via stderr
         * (fc_neg_len).  Provably non-negative lens skip the guard. */
        if (!e->slice_lit.len_nonneg)
            g_needs_stdio = true;
        detect_features_expr(e->slice_lit.ptr_expr);
        detect_features_expr(e->slice_lit.len_expr);
        return;
    case EXPR_IDENT:
        /* Built-in globals stdin/stdout/stderr require stdio.h */
        if (e->ident.name &&
            (strcmp(e->ident.name, "stdin") == 0 ||
             strcmp(e->ident.name, "stdout") == 0 ||
             strcmp(e->ident.name, "stderr") == 0))
            g_needs_stdio = true;
        return;
    case EXPR_SIZEOF:
    case EXPR_ALIGNOF:
    case EXPR_DEFAULT:
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_VOID_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_CONTINUE:
    case EXPR_TYPE_VAR_REF:
        return;
    }
}

static void detect_features_decl(Decl *d) {
    if (!d) return;
    switch (d->kind) {
    case DECL_LET: {
        Expr *fn = d->let.init;
        if (fn && fn->kind == EXPR_FUNC) {
            for (int i = 0; i < fn->func.body_count; i++)
                detect_features_expr(fn->func.body[i]);
        } else if (fn) {
            detect_features_expr(fn);
        }
        break;
    }
    case DECL_MODULE:
        for (int i = 0; i < d->module.decl_count; i++)
            detect_features_decl(d->module.decls[i]);
        break;
    default:
        break;
    }
}


static void collect_all_decls(Program *prog, Decl ***out_decls, int *out_count) {
    Decl **all = NULL;
    int count = 0, cap = 0;
    flatten_decls(prog->decls, prog->decl_count, &all, &count, &cap);
    *out_decls = all;
    *out_count = count;
}

void codegen_emit(Program *prog, FILE *out, MonoTable *mono,
                  Arena *arena, InternTable *intern_tbl, SymbolTable *symtab,
                  const CodegenOptions *opts) {
    g_mono = mono;
    g_arena = arena;
    g_intern = intern_tbl;
    g_symtab = symtab;
    g_backtraces = opts && opts->backtraces;
    g_fn_attr = g_backtraces ? "static __attribute__((unused, noinline)) "
                             : "static __attribute__((unused)) ";
    symmap_reset();

    /* Collect from_libs and defines from extern module declarations */
    const char *from_libs[64];
    int from_lib_count = 0;
    CDefine defines[64];
    int define_count = 0;
    for (int i = 0; i < prog->decl_count; i++) {
        collect_from_libs(prog->decls[i], from_libs, &from_lib_count, 64);
        collect_defines(prog->decls[i], defines, &define_count, 64);
    }

    /* Flatten module decls into a single array (needed for feature detection) */
    Decl **all_decls;
    int all_count;
    collect_all_decls(prog, &all_decls, &all_count);

    /* Detect which feature-gated headers are needed */
    g_needs_stdio = false;
    g_needs_math = false;
    g_needs_float = false;
    for (int i = 0; i < all_count; i++)
        detect_features_decl(all_decls[i]);
    /* Also scan monomorphized template bodies */
    for (int mi = 0; mi < mono->count; mi++) {
        if (mono->entries[mi].decl_kind == DECL_LET && mono->entries[mi].template_decl)
            detect_features_decl(mono->entries[mi].template_decl);
    }

    /* Preamble — emit defines before all includes */
    for (int i = 0; i < define_count; i++)
        fprintf(out, "#define %s %s\n", defines[i].macro, defines[i].value);

    /* Core headers — always emitted (FC's platform contract) */
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#include <stdbool.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <assert.h>\n");
    fprintf(out, "#include <limits.h>\n");

    /* On Windows, FC programs must link against UCRT — msvcrt lacks symbols
     * the emitted code relies on (e.g. _set_abort_behavior). Fail loudly here
     * rather than letting users discover the gap via confusing link errors.
     * The check sits *after* the core headers because _UCRT is defined in
     * MinGW-w64's <_mingw.h> (pulled in transitively by <stdint.h> et al.),
     * not by the compiler itself. */
    fprintf(out,
        "#if defined(_WIN32) && !defined(_UCRT)\n"
        "#error \"FC on Windows requires the UCRT runtime; msvcrt is not supported.\"\n"
        "#endif\n");

    /* Feature-gated headers — emitted only when needed.  --backtraces forces
     * <stdio.h> because the emitted fc_dump_backtrace helper uses fprintf/stderr
     * regardless of whether user code does any I/O. */
    if (g_needs_stdio || g_backtraces) fprintf(out, "#include <stdio.h>\n");
    if (g_needs_math)  fprintf(out, "#include <math.h>\n");
    if (g_needs_float) fprintf(out, "#include <float.h>\n");

    /* Emit #include for each unique from_lib in extern modules */
    for (int i = 0; i < from_lib_count; i++) {
        /* Skip headers already emitted in core or feature-gated preamble */
        if (strcmp(from_libs[i], "stdint.h") == 0 ||
            strcmp(from_libs[i], "stddef.h") == 0 ||
            strcmp(from_libs[i], "stdbool.h") == 0 ||
            strcmp(from_libs[i], "stdlib.h") == 0 ||
            strcmp(from_libs[i], "string.h") == 0)
            continue;
        if (g_needs_stdio && strcmp(from_libs[i], "stdio.h") == 0) continue;
        if (g_needs_math  && strcmp(from_libs[i], "math.h") == 0) continue;
        if (g_needs_float && strcmp(from_libs[i], "float.h") == 0) continue;
        fprintf(out, "#include <%s>\n", from_libs[i]);
    }
    fprintf(out, "\n");

    /* Windows: suppress the Watson/Windows-Error-Reporting dialog when
     * abort() fires (bounds checks, option unwrap, div-by-zero, assert).
     * Without this, CI and test runs on Windows hang on the error popup.
     * GCC constructor runs before main. UCRT is guaranteed by the prelude
     * #error above, so _set_abort_behavior is always available here. */
    fprintf(out,
        "#ifdef _WIN32\n"
        "__attribute__((unused, constructor))\n"
        "static void fc_win_abort_init(void) {\n"
        "    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);\n"
        "}\n"
        "#endif\n");

    /* Always emit fc_str (alias for uint8 slice) */
    fprintf(out, "typedef struct { uint8_t* ptr; int64_t len; } fc_str;\n");

    /* Narrowing-conversion guards. FC uses int64 for all slice/string lengths
     * and sizes; these helpers assert that a value fits in the target width
     * before narrowing. On 64-bit hosts SIZE_MAX == UINT64_MAX so the size_t
     * check is trivially true; on 32-bit (and smaller) hosts the assert fires
     * if user-derived math overflows size_t. Under -DNDEBUG the assert
     * compiles out and the helper reduces to a plain cast. */
    fprintf(out,
        "__attribute__((unused))\n"
        "static inline size_t fc_to_size(int64_t n) {\n"
        "    assert(n >= 0 && (uint64_t)n <= SIZE_MAX);\n"
        "    return (size_t)n;\n"
        "}\n"
        "__attribute__((unused))\n"
        "static inline int fc_to_int(int64_t n) {\n"
        "    assert(n >= 0 && n <= INT_MAX);\n"
        "    return (int)n;\n"
        "}\n");

    /* Saturating float->int conversion helpers (audit item 16). A raw C cast of
     * an out-of-range or NaN float to an integer is undefined behavior (and
     * observably divergent across -O levels), so FC defines `(intT) floatexpr` to
     * saturate. These are the runtime form (single operand evaluation); the
     * const-context form is an equivalent ternary emitted at the use site. Both
     * are driven by the one float_to_int_info table, so they cannot drift. Each
     * takes `double` — a float32 operand promotes losslessly, so the bounds and
     * truncation are identical regardless of source width. Fixed-width targets use
     * absolute constants (int-width-agnostic); isize/usize use PTRDIFF_MIN/SIZE_MAX. */
    {
        static const TypeKind f2i_kinds[] = {
            TYPE_INT8, TYPE_UINT8, TYPE_INT16, TYPE_UINT16, TYPE_INT32,
            TYPE_UINT32, TYPE_INT64, TYPE_UINT64, TYPE_ISIZE, TYPE_USIZE,
        };
        for (size_t i = 0; i < sizeof f2i_kinds / sizeof f2i_kinds[0]; i++) {
            F2iInfo fi;
            float_to_int_info(f2i_kinds[i], &fi);
            fprintf(out,
                "__attribute__((unused)) static inline %s %s(double f) { "
                "if (f!=f) return 0; if (f < %s) return %s; if (f >= %s) return %s; "
                "return (%s)f; }\n",
                fi.cty, fi.fn, fi.lo, fi.imin, fi.hi, fi.imax, fi.cty);
        }
    }

    /* Abort macro.  When --backtraces is on, every abort site dumps a
     * backtrace before bailing.  Forward-declare fc_dump_backtrace here so
     * callers can see it; body and the FC-symbol mapping table are emitted at
     * the end of the TU once all emitted function names are known. */
    if (g_backtraces) {
        fprintf(out, "__attribute__((cold, unused)) static void fc_dump_backtrace(void);\n");
        fprintf(out, "#define FC_ABORT() ({ fc_dump_backtrace(); abort(); })\n");
    } else {
        fprintf(out, "#define FC_ABORT() abort()\n");
    }

    /* Out-of-bounds failure helpers.  Cold+noreturn so the hot path stays
     * clean: the compiler can schedule these out of line, skip keeping
     * caller state live across the call, and is free to vectorize/reorder
     * loads around checked accesses.  Static so unused copies get DCE'd. */
    if (g_needs_stdio) {
        fprintf(out,
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_oob(const char *file, int line, long long idx, long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: slice index out of range: index=%%lld len=%%lld\\n\",\n"
            "            file, line, idx, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_oob_sub(const char *file, int line, long long lo, long long hi, long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: subslice out of range: lo=%%lld hi=%%lld len=%%lld\\n\",\n"
            "            file, line, lo, hi, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_neg_len(const char *file, int line, long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: slice literal length is negative: len=%%lld\\n\",\n"
            "            file, line, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_null_some(const char *file, int line) {\n"
            "    fprintf(stderr, \"%%s:%%d: some() of a null pointer "
            "(pointer options use null as the none sentinel)\\n\", file, line);\n"
            "    FC_ABORT();\n"
            "}\n");
        /* Register the helpers as skip-entries so their frames don't pollute
         * the user-visible backtrace when a bounds check fires. */
        symmap_add("fc_oob", NULL, "<runtime>", 0);
        symmap_add("fc_oob_sub", NULL, "<runtime>", 0);
        symmap_add("fc_neg_len", NULL, "<runtime>", 0);
        symmap_add("fc_null_some", NULL, "<runtime>", 0);
    }

    /* Collect all slice, option, function, and eq types used in the program */
    TypeSet slices = {0};
    TypeSet options = {0};
    TypeSet fns = {0};
    TypeSet eqs = {0};
    g_eq_set = &eqs;

    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_LET) {
            collect_types_in_type(d->let.resolved_type, &slices, &options, &fns);
            collect_types_expr(d->let.init, &slices, &options, &fns);
        }
        if (d->kind == DECL_STRUCT) {
            for (int j = 0; j < d->struc.field_count; j++)
                collect_types_in_type(d->struc.fields[j].type, &slices, &options, &fns);
        }
        if (d->kind == DECL_UNION) {
            for (int j = 0; j < d->unio.variant_count; j++)
                collect_types_in_type(d->unio.variants[j].payload, &slices, &options, &fns);
        }
    }
    /* Also collect types from monomorphized instances */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->concrete_type) {
            if (inst->concrete_type->kind == TYPE_STRUCT) {
                for (int f = 0; f < inst->concrete_type->struc.field_count; f++)
                    collect_types_in_type(inst->concrete_type->struc.fields[f].type, &slices, &options, &fns);
            } else if (inst->concrete_type->kind == TYPE_UNION) {
                for (int v = 0; v < inst->concrete_type->unio.variant_count; v++)
                    collect_types_in_type(inst->concrete_type->unio.variants[v].payload, &slices, &options, &fns);
            }
        }
    }

    /* Also collect types from monomorphized function bodies (with substitution) */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        for (int j = 0; j < fn->func.body_count; j++)
            collect_types_expr(fn->func.body[j], &slices, &options, &fns);
        g_subst = NULL;
    }
    g_eq_set = NULL;

    /* Emit forward declarations for all structs and unions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_STRUCT && !d->struc.is_extern) emit_struct_forward(d, out);
        else if (d->kind == DECL_UNION) emit_union_forward(d, out);
    }
    /* Forward declarations for monomorphized structs/unions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind == DECL_STRUCT) {
            fprintf(out, "typedef struct %s %s;\n", inst->mangled_name, inst->mangled_name);
        } else if (inst->decl_kind == DECL_UNION) {
            fprintf(out, "typedef struct %s %s;\n", inst->mangled_name, inst->mangled_name);
        }
    }

    /* Emit union tag enums (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_UNION) emit_union_tag_enum(d, out);
    }
    /* Tag enums for monomorphized unions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_UNION || !inst->concrete_type) continue;
        Type *ct = inst->concrete_type;
        fprintf(out, "typedef enum {");
        for (int v = 0; v < ct->unio.variant_count; v++) {
            if (v > 0) fprintf(out, ",");
            fprintf(out, " %s_tag_%s", inst->mangled_name, ct->unio.variants[v].name);
        }
        fprintf(out, " } %s_tag;\n", inst->mangled_name);
    }

    /* Forward-declare all option typedefs (as named struct tags). This lets slice
       typedefs reference fc_option_T* without requiring the option body to be
       emitted first, breaking the slice<option<T>> vs option<slice<T>> cycle. */
    for (int i = 0; i < options.count; i++) {
        Type *o = options.types[i];
        fprintf(out, "typedef struct fc_option_");
        if (o->option.inner) emit_type_ident(o->option.inner, out);
        else fprintf(out, "void");
        fprintf(out, " fc_option_");
        if (o->option.inner) emit_type_ident(o->option.inner, out);
        else fprintf(out, "void");
        fprintf(out, ";\n");
    }

    /* Slice typedefs are split into a forward declaration (named struct tag) and
       a body. The forward decls precede the function typedefs so a function type
       can take a slice by value as a parameter (an incomplete by-value param is
       fine in a function-pointer typedef). The bodies follow the fn typedefs so
       a slice OF a function type can name the now-declared fn typedef in its
       `elem* ptr` field. Both precede the scalar option bodies below, since an
       option-of-slice embeds the slice body by value. */
    for (int i = 0; i < slices.count; i++) {
        Type *s = slices.types[i];
        if (is_str_type(s)) continue; /* str already emitted */
        fprintf(out, "typedef struct fc_slice_");
        emit_type_ident(s->slice.elem, out);
        fprintf(out, "_s fc_slice_");
        emit_type_ident(s->slice.elem, out);
        fprintf(out, ";\n");
    }

    /* Function typedefs. A function-pointer typedef tolerates *incomplete*
       by-value struct/union/option/slice params and return (C only requires
       those complete at a call or definition, not at the pointer-type
       declaration), and every struct/union/option/slice is forward-declared
       above, so all fn typedefs can be emitted here in one pass — before
       struct/union defs. The `fns` set is dependency-ordered inner-first
       (collect_types_in_type recurses param/return before adding the outer fn),
       so a fn type used by value inside another fn's signature is declared first. */
    for (int i = 0; i < fns.count; i++) {
        Type *f = fns.types[i];
        fprintf(out, "typedef struct { ");
        emit_type(f->func.return_type, out);
        fprintf(out, " (*fn_ptr)(");
        for (int j = 0; j < f->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(f->func.param_types[j], out);
        }
        if (f->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void*); void* ctx; } ");
        emit_type(f, out);
        fprintf(out, ";\n");
    }

    /* Slice bodies — the fn typedefs above are now complete, so a slice of a
       function type can reference it by pointer. */
    for (int i = 0; i < slices.count; i++) {
        Type *s = slices.types[i];
        if (is_str_type(s)) continue;
        fprintf(out, "struct fc_slice_");
        emit_type_ident(s->slice.elem, out);
        fprintf(out, "_s { ");
        emit_type(s->slice.elem, out);
        fprintf(out, "* ptr; int64_t len; };\n");
    }

    /* Emit scalar option bodies (primitives, pointers, slices). Options wrapping
       structs/unions are deferred until after their definitions; options wrapping
       function types are deferred until just after the function typedefs (the
       body embeds the fn struct by value, so the typedef must precede it). */
    for (int i = 0; i < options.count; i++) {
        Type *o = options.types[i];
        if (o->option.inner &&
            (o->option.inner->kind == TYPE_STRUCT || o->option.inner->kind == TYPE_UNION ||
             o->option.inner->kind == TYPE_STUB || o->option.inner->kind == TYPE_FUNC))
            continue; /* deferred (see comment above) */
        fprintf(out, "struct fc_option_");
        emit_type_ident(o->option.inner, out);
        fprintf(out, " { ");
        emit_type(o->option.inner, out);
        fprintf(out, " value; bool has_value; };\n");
    }

    /* Option-of-function bodies, now that every fn typedef is complete. These
       embed the fn struct by value, so they must follow the typedefs but precede
       any struct that embeds the option by value (the struct defs come next). */
    for (int i = 0; i < options.count; i++) {
        Type *o = options.types[i];
        if (!o->option.inner || o->option.inner->kind != TYPE_FUNC) continue;
        fprintf(out, "struct fc_option_");
        emit_type_ident(o->option.inner, out);
        fprintf(out, " { ");
        emit_type(o->option.inner, out);
        fprintf(out, " value; bool has_value; };\n");
    }

    /* Combined topological sort of ALL struct/union definitions — top-level
       (non-generic) decls and monomorphized instances (including synthesized
       tuples) together — so by-value dependencies emit before their dependents
       no matter which kind references which (a top-level struct holding a tuple
       by value, or a generic instance holding a top-level struct by value). */
    SuDef *defs = NULL;
    int def_count = 0, def_cap = 0;
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_STRUCT || d->kind == DECL_UNION) {
            SuDef s = { decl_su_name(d), d, NULL };
            DA_APPEND(defs, def_count, def_cap, s);
        }
    }
    for (int m = 0; m < mono->count; m++) {
        MonoInstance *inst = &mono->entries[m];
        if (!inst->concrete_type) continue;
        if (inst->decl_kind == DECL_STRUCT || inst->decl_kind == DECL_UNION) {
            SuDef s = { inst->mangled_name, NULL, inst };
            DA_APPEND(defs, def_count, def_cap, s);
        }
    }
    int *def_state = calloc((size_t)(def_count > 0 ? def_count : 1), sizeof(int));
    int *def_order = malloc(sizeof(int) * (size_t)(def_count > 0 ? def_count : 1));
    int def_order_count = 0;
    for (int i = 0; i < def_count; i++)
        topo_visit_sudef(defs, def_count, i, def_state, def_order, &def_order_count);
    free(def_state);

    /* Emit definitions in dependency order, interleaving option typedefs that
       wrap each newly-defined struct/union/tuple. */
    bool *opt_emitted = calloc(options.count > 0 ? (size_t)options.count : 1, sizeof(bool));
    for (int oi = 0; oi < def_order_count; oi++) {
        SuDef *s = &defs[def_order[oi]];
        const char *def_name = s->name;
        if (s->decl) {
            Decl *d = s->decl;
            if (d->kind == DECL_STRUCT) {
                if (!d->struc.is_extern) emit_struct_def(d, out);
            } else if (d->kind == DECL_UNION) {
                emit_union_def(d, out);
            }
        } else if (s->mi) {
            MonoInstance *inst = s->mi;
            Type *ct = inst->concrete_type;
            if (inst->decl_kind == DECL_STRUCT) {
                fprintf(out, "struct %s {", inst->mangled_name);
                for (int f = 0; f < ct->struc.field_count; f++)
                    emit_struct_field(ct->struc.fields[f].type,
                        c_safe_ident(g_intern, ct->struc.fields[f].name), out);
                fprintf(out, " };\n");
            } else if (inst->decl_kind == DECL_UNION) {
                bool has_payload = false;
                for (int v = 0; v < ct->unio.variant_count; v++)
                    if (ct->unio.variants[v].payload) { has_payload = true; break; }
                if (has_payload) {
                    fprintf(out, "struct %s { %s_tag tag; union {", inst->mangled_name, inst->mangled_name);
                    for (int v = 0; v < ct->unio.variant_count; v++) {
                        if (ct->unio.variants[v].payload) {
                            fprintf(out, " ");
                            emit_type(ct->unio.variants[v].payload, out);
                            fprintf(out, " %s;", c_safe_ident(g_intern, ct->unio.variants[v].name));
                        }
                    }
                    fprintf(out, " }; };\n");
                } else {
                    fprintf(out, "struct %s { %s_tag tag; };\n", inst->mangled_name, inst->mangled_name);
                }
            }
        }
        if (!def_name) continue;
        for (int j = 0; j < options.count; j++) {
            if (opt_emitted[j]) continue;
            Type *o = options.types[j];
            if (!o->option.inner) continue;
            const char *inner_name = NULL;
            if (o->option.inner->kind == TYPE_STRUCT) inner_name = o->option.inner->struc.name;
            else if (o->option.inner->kind == TYPE_UNION) inner_name = o->option.inner->unio.name;
            else if (o->option.inner->kind == TYPE_STUB) inner_name = o->option.inner->stub.name;
            if (inner_name && inner_name == def_name) {
                fprintf(out, "struct fc_option_");
                emit_type_ident(o->option.inner, out);
                fprintf(out, " { ");
                emit_type(o->option.inner, out);
                fprintf(out, " value; bool has_value; };\n");
                opt_emitted[j] = true;
            }
        }
    }
    free(def_order);
    free(defs);
    free(opt_emitted);

    /* Emit eq function forward declarations and definitions */
    for (int i = 0; i < eqs.count; i++)
        emit_eq_forward(eqs.types[i], out);
    for (int i = 0; i < eqs.count; i++)
        emit_eq_func(eqs.types[i], out);

    free(slices.types);
    free(options.types);
    free(fns.types);
    free(eqs.types);

    fprintf(out, "\n");

    /* Check if we have a main function */
    bool has_main = false;
    bool has_non_func = false;
    for (int i = 0; i < all_count; i++) {
        if (is_func_decl(all_decls[i])) {
            if (strcmp(all_decls[i]->let.name, "main") == 0) has_main = true;
        } else if (all_decls[i]->kind == DECL_LET) {
            has_non_func = true;
        }
    }

    /* Collect file-level non-function globals for hoisted initialization in C main */
    g_file_global_count = 0;
    g_file_globals = NULL;
    if (has_main) {
        for (int i = 0; i < all_count; i++) {
            Decl *d = all_decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d) && !d->let.is_module_member)
                g_file_global_count++;
        }
        if (g_file_global_count > 0) {
            g_file_globals = malloc(sizeof(Decl*) * (size_t)g_file_global_count);
            int idx = 0;
            for (int i = 0; i < all_count; i++) {
                Decl *d = all_decls[i];
                if (d->kind == DECL_LET && !is_func_decl(d) && !d->let.is_module_member)
                    g_file_globals[idx++] = d;
            }
        }
    }

    /* Emit forward declarations for functions (with void* _ctx), skip generics.
     * main is emitted as fc_main with its str[] param (no _ctx). */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_func_decl(d) && strcmp(d->let.name, "main") == 0 && !is_generic_decl(d)) {
            Expr *fn = d->let.init;
            fprintf(out, "%sint32_t fc_main(", g_fn_attr);
            emit_type(fn->func.params[0].type, out);
            fprintf(out, " %s);\n", c_safe_ident(g_intern, fn->func.params[0].name));
            symmap_add("fc_main", "main", d->loc.filename, d->loc.line);
            continue;
        }
        if (is_func_decl(d) && strcmp(d->let.name, "main") != 0 && !is_generic_decl(d)) {
            const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            Type *ft = d->let.resolved_type;
            fprintf(out, "%s", g_fn_attr);
            emit_type(ft->func.return_type, out);
            fprintf(out, " %s(", cname);
            Expr *fn = d->let.init;
            for (int j = 0; j < fn->func.param_count; j++) {
                if (j > 0) fprintf(out, ", ");
                emit_type(fn->func.params[j].type, out);
                fprintf(out, " %s", c_safe_ident(g_intern, fn->func.params[j].name));
            }
            if (fn->func.param_count > 0) fprintf(out, ", ");
            fprintf(out, "void* _ctx);\n");
            symmap_add(cname, d->let.name, d->loc.filename, d->loc.line);
        }
    }
    /* Forward declarations for monomorphized functions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        /* Set up substitution context */
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        fprintf(out, "%s", g_fn_attr);
        emit_type(fn->type->func.return_type, out);
        fprintf(out, " %s(", inst->mangled_name);
        for (int j = 0; j < fn->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(fn->func.params[j].type, out);
            fprintf(out, " %s", c_safe_ident(g_intern, fn->func.params[j].name));
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx);\n");
        g_subst = NULL;
        if (g_backtraces) {
            const char *disp = fmt_mono_display(g_arena, tmpl->let.name,
                                                inst->type_args, inst->type_param_count);
            symmap_add(inst->mangled_name, disp, tmpl->loc.filename, tmpl->loc.line);
        }
    }
    fprintf(out, "\n");

    /* Collect lambdas from all declarations */
    LambdaSet lambdas = {0};
    for (int i = 0; i < all_count; i++) {
        if (all_decls[i]->kind == DECL_LET && all_decls[i]->let.init) {
            collect_lambdas_expr(all_decls[i]->let.init, &lambdas);
        }
    }
    /* Record lambdas in symmap (for --backtraces frames) */
    if (g_backtraces) {
        for (int i = 0; i < lambdas.count; i++) {
            Expr *lam = lambdas.exprs[i];
            if (!lam->func.lifted_name) continue;
            const char *disp = fmt_lambda_display(g_arena, lam->loc.filename, lam->loc.line);
            symmap_add(lam->func.lifted_name, disp, lam->loc.filename, lam->loc.line);
        }
    }

    /* Collect trampolines: FC functions passed at extern call boundaries.
     * Walk all non-generic bodies and all monomorphized bodies. */
    TrampolineSet trampolines = {0};
    for (int i = 0; i < all_count; i++) {
        if (all_decls[i]->kind == DECL_LET && all_decls[i]->let.init &&
            !is_generic_decl(all_decls[i])) {
            collect_trampolines_expr(all_decls[i]->let.init, &trampolines);
        }
    }
    /* Also walk monomorphized function bodies */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        for (int j = 0; j < fn->func.body_count; j++)
            collect_trampolines_expr(fn->func.body[j], &trampolines);
        g_subst = NULL;
    }

    /* Pre-pass: walk module-member inits and lift every EXPR_ARRAY_LIT into a
     * static backing array.  Nested array lits inside struct/variant/some
     * initializers are all collected; the use sites then emit slice headers
     * referencing these backings.  Only walks module members (codegen_name
     * set) — file-level lets are hoisted to main and can use the normal
     * alloca-based emission. */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_LET && !is_func_decl(d) && d->let.is_module_member &&
            d->let.init && d->let.init->kind != EXPR_FUNC) {
            collect_const_backings(d->let.init);
        }
    }

    /* Emit the static backing arrays themselves.  Each is `static T name[] =
     * { e0, e1, ... };` — elements are emitted in const context so nested
     * struct/variant/some stay on the aggregate-initializer path. */
    if (g_const_backing_count > 0) {
        g_const_context = true;
        for (int i = 0; i < g_const_backing_count; i++) {
            Expr *al = g_const_backings[i];
            fprintf(out, "static ");
            emit_type(al->array_lit.elem_type, out);
            fprintf(out, " %s[] = {", al->array_lit.codegen_backing_name);
            for (int j = 0; j < al->array_lit.elem_count; j++) {
                if (j > 0) fprintf(out, ", ");
                emit_expr(al->array_lit.elems[j], out);
            }
            fprintf(out, "};\n");
        }
        g_const_context = false;
        fprintf(out, "\n");
    }

    /* Emit non-function global variable definitions.
     * Must come before lifted lambdas so they can reference globals.
     * Module members are emitted with const-expr initializers at C file scope.
     * File-level globals are declared without initializers here; their
     * initialization is hoisted into the C main wrapper.  Both carry a mangled
     * codegen_name (fc__name); the two are told apart by is_module_member. */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_LET && !is_func_decl(d) &&
            (d->let.is_module_member || has_main)) {
            const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            emit_type(d->let.resolved_type, out);
            if (d->let.is_module_member) {
                /* Module member: emit with initializer (must be const expr).
                 * Flip const context so array/struct-with-fixed-array/etc.
                 * take the aggregate-initializer path. */
                fprintf(out, " %s = ", cname);
                g_const_context = true;
                emit_expr(d->let.init, out);
                g_const_context = false;
                fprintf(out, ";\n");
            } else {
                /* File-level global: declare only, init hoisted into C main */
                fprintf(out, " %s;\n", cname);
            }
        }
    }
    fprintf(out, "\n");

    /* Emit context structs for capturing lambdas */
    for (int i = 0; i < lambdas.count; i++) {
        Expr *lam = lambdas.exprs[i];
        if (lam->func.capture_count > 0) {
            fprintf(out, "typedef struct {");
            for (int j = 0; j < lam->func.capture_count; j++) {
                fprintf(out, " ");
                emit_type(lam->func.captures[j].type, out);
                fprintf(out, " %s;", lam->func.captures[j].codegen_name);
            }
            fprintf(out, " } _ctx_%s;\n", lam->func.lifted_name);
        }
    }

    /* Emit forward declarations for lifted lambdas */
    for (int i = 0; i < lambdas.count; i++) {
        Expr *lam = lambdas.exprs[i];
        Type *ft = lam->type;
        fprintf(out, "%s", g_fn_attr);
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", lam->func.lifted_name);
        for (int j = 0; j < lam->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(lam->func.params[j].type, out);
            fprintf(out, " %s", c_safe_ident(g_intern, lam->func.params[j].name));
        }
        if (lam->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx);\n");
    }
    /* Emit forward declarations for C-boundary trampolines */
    for (int i = 0; i < trampolines.count; i++) {
        TrampolineEntry *te = &trampolines.entries[i];
        Type *ft = te->type;
        fprintf(out, "%s", g_fn_attr);
        emit_type(ft->func.return_type, out);
        fprintf(out, " _ctramp_%s(", te->name);
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(ft->func.param_types[j], out);
            fprintf(out, " _p%d", j);
        }
        if (ft->func.param_count == 0) fprintf(out, "void");
        fprintf(out, ");\n");
    }
    fprintf(out, "\n");

    /* Emit lifted lambda function definitions */
    for (int i = 0; i < lambdas.count; i++) {
        Expr *lam = lambdas.exprs[i];
        Type *ft = lam->type;
        fprintf(out, "%s", g_fn_attr);
        emit_type(ft->func.return_type, out);
        fprintf(out, " %s(", lam->func.lifted_name);
        for (int j = 0; j < lam->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(lam->func.params[j].type, out);
            fprintf(out, " %s", c_safe_ident(g_intern, lam->func.params[j].name));
        }
        if (lam->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx) {\n");

        indent_level = 1;
        if (lam->func.capture_count > 0) {
            /* Extract captures from context struct */
            emit_indent(out);
            fprintf(out, "_ctx_%s* _c = (_ctx_%s*)_ctx;\n",
                lam->func.lifted_name, lam->func.lifted_name);
            for (int j = 0; j < lam->func.capture_count; j++) {
                emit_indent(out);
                emit_type(lam->func.captures[j].type, out);
                fprintf(out, " %s = _c->%s;\n",
                    lam->func.captures[j].codegen_name,
                    lam->func.captures[j].codegen_name);
            }
        } else {
            emit_indent(out);
            fprintf(out, "(void)_ctx;\n");
        }

        /* Self-recursion: materialize the binding as a local fat pointer so the body
           can call/use itself by name. Non-capturing passes a NULL context; capturing
           threads the same _ctx through recursive calls. Emitted only when the name was
           actually referenced, so non-recursive lambdas stay -Werror-clean. */
        if (lam->func.self_codegen_name && lam->func.self_referenced) {
            emit_indent(out);
            emit_type(lam->type, out);
            fprintf(out, " %s = { .fn_ptr = %s, .ctx = %s };\n",
                lam->func.self_codegen_name, lam->func.lifted_name,
                lam->func.capture_count > 0 ? "_ctx" : "NULL");
        }

        begin_hoisted_scope(lam->func.body, lam->func.body_count, out);
        defer_scope_push(false);
        emit_block_stmts(lam->func.body, lam->func.body_count, out, true, true);
        defer_scope_pop();
        end_hoisted_scope();
        indent_level = 0;
        fprintf(out, "}\n\n");
    }
    free(lambdas.exprs);

    /* Emit function definitions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        if (is_func_decl(all_decls[i]) && !is_generic_decl(all_decls[i])) {
            emit_func_decl(all_decls[i], out);
        }
    }

    /* Emit forward declarations for all monomorphized functions (including transitive) */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        fprintf(out, "%s", g_fn_attr);
        emit_type(fn->type->func.return_type, out);
        fprintf(out, " %s(", inst->mangled_name);
        for (int j = 0; j < fn->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(fn->func.params[j].type, out);
            fprintf(out, " %s", c_safe_ident(g_intern, fn->func.params[j].name));
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx);\n");
        g_subst = NULL;
    }
    fprintf(out, "\n");

    /* Emit monomorphized function definitions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst = { inst->type_param_names, inst->type_args, inst->type_param_count };
        g_subst = &subst;
        fprintf(out, "%s", g_fn_attr);
        emit_type(fn->type->func.return_type, out);
        fprintf(out, " %s(", inst->mangled_name);
        for (int j = 0; j < fn->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(fn->func.params[j].type, out);
            fprintf(out, " %s", c_safe_ident(g_intern, fn->func.params[j].name));
        }
        if (fn->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void* _ctx) {\n");
        fprintf(out, "    (void)_ctx;\n");
        indent_level = 1;
        begin_hoisted_scope(fn->func.body, fn->func.body_count, out);
        defer_scope_push(false);
        emit_block_stmts(fn->func.body, fn->func.body_count, out, true, true);
        defer_scope_pop();
        end_hoisted_scope();
        indent_level = 0;
        fprintf(out, "}\n\n");
        g_subst = NULL;
    }

    /* Emit C-boundary trampoline definitions */
    for (int i = 0; i < trampolines.count; i++) {
        TrampolineEntry *te = &trampolines.entries[i];
        Type *ft = te->type;
        fprintf(out, "%s", g_fn_attr);
        emit_type(ft->func.return_type, out);
        fprintf(out, " _ctramp_%s(", te->name);
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(ft->func.param_types[j], out);
            fprintf(out, " _p%d", j);
        }
        if (ft->func.param_count == 0) fprintf(out, "void");
        fprintf(out, ") {\n");
        if (ft->func.return_type->kind == TYPE_VOID) {
            fprintf(out, "    %s(", te->name);
        } else {
            fprintf(out, "    return %s(", te->name);
        }
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            fprintf(out, "_p%d", j);
        }
        if (ft->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "NULL);\n");
        fprintf(out, "}\n");
    }
    free(trampolines.entries);

    /* If no main function, wrap non-func decls in synthetic main */
    if (!has_main && has_non_func) {
        fprintf(out, "int main(void) {\n");
        indent_level = 1;
        for (int i = 0; i < all_count; i++) {
            Decl *d = all_decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d)) {
                const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
                emit_indent(out);
                emit_type(d->let.resolved_type, out);
                fprintf(out, " %s = ", cname);
                emit_expr(d->let.init, out);
                fprintf(out, ";\n");
            }
        }
        /* Return last binding's value */
        for (int i = all_count - 1; i >= 0; i--) {
            Decl *d = all_decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d)) {
                const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
                if (type_is_integer(d->let.resolved_type) || type_eq(d->let.resolved_type, type_bool())) {
                    emit_indent(out);
                    fprintf(out, "return (int)%s;\n", cname);
                } else {
                    emit_indent(out);
                    fprintf(out, "return 0;\n");
                }
                break;
            }
        }
        indent_level = 0;
        fprintf(out, "}\n");
    }

    /* Backtrace helper body + FC-symbol mapping table.
     * Emitted at the end of the TU so all collected names (top-level functions,
     * monomorphized instances, lifted lambdas) are present in g_symmap.
     *
     * The table is keyed by function address (taken with &name).  At program
     * startup a constructor sorts it by address; lookup is a binary search
     * for the largest entry with fn_addr <= return_addr.  No dependency on
     * the dynamic symbol table — `-rdynamic` is not required.  Entries with
     * fc_name == NULL mark internal helpers (fc_oob, fc_oob_sub) whose
     * frames are skipped from the printed backtrace.
     *
     * The body is gated on __linux__/__APPLE__ via #if; on other platforms
     * the helper is a no-op stub and abort() still fires.  The forward
     * declaration is in the preamble. */
    if (g_backtraces) {
        fprintf(out, "\n#if defined(__linux__) || defined(__APPLE__)\n");
        fprintf(out, "#include <execinfo.h>\n");
        fprintf(out, "#endif\n");
        fprintf(out, "typedef struct { void *fn_addr; const char *fc_name; const char *file; int line; } fc_sym_entry;\n");
        fprintf(out, "static fc_sym_entry _fc_symtab[] = {\n");
        for (int i = 0; i < g_symmap_count; i++) {
            FcSymEntry *e = &g_symmap[i];
            fprintf(out, "    { (void*)&%s, ", e->c_name);
            if (e->fc_name) {
                fprintf(out, "\"");
                emit_c_escaped(e->fc_name, (int)strlen(e->fc_name), out);
                fprintf(out, "\"");
            } else {
                fprintf(out, "(void*)0");
            }
            fprintf(out, ", \"");
            emit_c_escaped(e->file, (int)strlen(e->file), out);
            fprintf(out, "\", %d },\n", e->line);
        }
        fprintf(out, "};\n");
        fprintf(out, "static const int _fc_symtab_count = %d;\n", g_symmap_count);
        fprintf(out,
            "static int _fc_sym_cmp(const void *a, const void *b) {\n"
            "    void *pa = ((const fc_sym_entry*)a)->fn_addr;\n"
            "    void *pb = ((const fc_sym_entry*)b)->fn_addr;\n"
            "    if ((uintptr_t)pa < (uintptr_t)pb) return -1;\n"
            "    if ((uintptr_t)pa > (uintptr_t)pb) return  1;\n"
            "    return 0;\n"
            "}\n"
            "__attribute__((constructor)) static void _fc_symtab_init(void) {\n"
            "    qsort(_fc_symtab, (size_t)_fc_symtab_count, sizeof _fc_symtab[0], _fc_sym_cmp);\n"
            "}\n");
        fprintf(out,
            "__attribute__((cold, unused))\n"
            "static void fc_dump_backtrace(void) {\n"
            "#if defined(__linux__) || defined(__APPLE__)\n"
            "    void *_frames[64];\n"
            "    int _n = backtrace(_frames, 64);\n"
            "    if (_n < 2) return;\n"
            "    fprintf(stderr, \"backtrace:\\n\");\n"
            "    int _printed = 0;\n"
            "    for (int _i = 1; _i < _n; _i++) {\n"
            "        /* Binary search: largest entry with fn_addr <= frame addr. */\n"
            "        uintptr_t _a = (uintptr_t)_frames[_i];\n"
            "        int _lo = 0, _hi = _fc_symtab_count;\n"
            "        while (_lo < _hi) {\n"
            "            int _mid = _lo + (_hi - _lo) / 2;\n"
            "            if ((uintptr_t)_fc_symtab[_mid].fn_addr <= _a) _lo = _mid + 1;\n"
            "            else _hi = _mid;\n"
            "        }\n"
            "        if (_lo == 0) {\n"
            "            fprintf(stderr, \"  #%%d %%p\\n\", _printed++, _frames[_i]);\n"
            "            continue;\n"
            "        }\n"
            "        const fc_sym_entry *_e = &_fc_symtab[_lo - 1];\n"
            "        if (!_e->fc_name) continue;  /* internal helper: skip */\n"
            "        fprintf(stderr, \"  #%%d %%-24s defined at %%s:%%d\\n\",\n"
            "                _printed++, _e->fc_name, _e->file, _e->line);\n"
            "        if (strcmp(_e->fc_name, \"main\") == 0) break;\n"
            "    }\n"
            "#else\n"
            "    (void)_fc_symtab;\n"
            "    (void)_fc_symtab_count;\n"
            "#endif\n"
            "}\n");
    }

    symmap_reset();
    free(g_file_globals);
    g_file_globals = NULL;
    g_file_global_count = 0;
    free(all_decls);
}
