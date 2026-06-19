#include "pass2.h"
#include "diag.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---- Scope for local bindings ---- */

typedef struct {
    const char *name;
    const char *codegen_name;   /* unique C name for shadowing */
    Type *type;
    bool is_mut;
    bool is_capturing;          /* true if bound to a capturing lambda */
    Provenance prov;            /* provenance of the bound value */
} LocalBinding;

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    LocalBinding *locals;
    int local_count;
    int local_cap;
    bool is_lambda_boundary;
    bool is_global;          /* true for root scope — bindings here are not captures */
};

typedef struct LambdaCtx LambdaCtx;
struct LambdaCtx {
    LambdaCtx *parent;
    Capture *entries;
    int count;
    int cap;
    /* Return statements collected during this function's body check, so they can
       be validated against the inferred return type once the body's tail type is
       known. Only the innermost lambda collects its own returns — nested EXPR_FUNC
       pushes its own ctx, so returns inside a nested lambda go to that lambda. */
    struct Expr **returns;
    int return_count;
    int return_cap;
    /* Self-recursion: when this lambda is the init of a `let f = <lambda>` binding,
       self_name/self_codegen_name identify the binding visible within the body, and
       self_referenced records whether it was actually used (gates codegen of the
       materialized self fat pointer so unused locals don't trip -Werror). */
    const char *self_name;
    const char *self_codegen_name;
    bool self_referenced;
};

static Scope *scope_new(Arena *a, Scope *parent) {
    Scope *s = arena_alloc(a, sizeof(Scope));
    s->parent = parent;
    return s;
}

static int local_id_counter = 0;

/* Generate a unique codegen name like "_l_varname_42" with no fixed buffer limit. */
static const char *make_local_name(Arena *a, const char *prefix, const char *name, int id) {
    int needed = snprintf(NULL, 0, "%s%s_%d", prefix, name, id) + 1;
    char *buf = arena_alloc(a, (size_t)needed);
    snprintf(buf, (size_t)needed, "%s%s_%d", prefix, name, id);
    return buf;
}

static void scope_add_prov(Scope *s, const char *name, const char *codegen_name, Type *type, bool is_mut, Provenance prov) {
    LocalBinding b = { name, codegen_name, type, is_mut, false, prov };
    DA_APPEND(s->locals, s->local_count, s->local_cap, b);
}

static void scope_add(Scope *s, const char *name, const char *codegen_name, Type *type, bool is_mut) {
    scope_add_prov(s, name, codegen_name, type, is_mut, PROV_UNKNOWN);
}

/* Scope lookup with lambda boundary crossing and mutability tracking.
   Boundary crossings are counted when moving FROM a boundary scope to its parent,
   so bindings within the boundary scope itself are not treated as captures.
   Stops at the first is_global scope (current module boundary) — parent module
   bindings are resolved by the interleaved parent/import loop in EXPR_IDENT. */
static Type *scope_lookup_capture(Scope *s, const char *name,
    const char **out_codegen_name, bool *out_is_mut, int *out_crossings,
    bool *out_is_global)
{
    int crossings = 0;
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (int i = sc->local_count - 1; i >= 0; i--) {
            if (sc->locals[i].name == name) {
                if (out_codegen_name) *out_codegen_name = sc->locals[i].codegen_name;
                if (out_is_mut) *out_is_mut = sc->locals[i].is_mut;
                /* Global scope bindings are never captures */
                if (out_crossings) *out_crossings = sc->is_global ? 0 : crossings;
                if (out_is_global) *out_is_global = sc->is_global;
                return sc->locals[i].type;
            }
        }
        /* Stop after searching the current module's global scope — don't walk
         * into parent module scopes.  The interleaved resolution loop handles
         * parent members and imports in the correct order. */
        if (sc->is_global) break;
        /* Count boundary when leaving this scope to search parent */
        if (sc->is_lambda_boundary) crossings++;
    }
    if (out_codegen_name) *out_codegen_name = NULL;
    if (out_is_mut) *out_is_mut = false;
    if (out_crossings) *out_crossings = 0;
    if (out_is_global) *out_is_global = false;
    return NULL;
}

/* Look up whether a binding holds a capturing lambda */
static bool scope_lookup_is_capturing(Scope *s, const char *name) {
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (int i = sc->local_count - 1; i >= 0; i--) {
            if (sc->locals[i].name == name)
                return sc->locals[i].is_capturing;
        }
    }
    return false;
}

/* Look up a binding's provenance by name */
static Provenance scope_lookup_prov(Scope *s, const char *name) {
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (int i = sc->local_count - 1; i >= 0; i--) {
            if (sc->locals[i].name == name)
                return sc->locals[i].prov;
        }
    }
    return PROV_UNKNOWN;
}

/* Conservative merge: if any branch is STACK, result is STACK */
static Provenance merge_prov(Provenance a, Provenance b) {
    if (a == b) return a;
    if (a == PROV_STACK || b == PROV_STACK) return PROV_STACK;
    return PROV_UNKNOWN;
}

/* Merge a freshly-assigned value's provenance into a mut binding (identified by
 * its unique codegen name). Reassignment of a `let mut` binding can change what
 * it holds, but escape analysis is flow-insensitive: once a binding has held a
 * stack value on any checked path, every later read must treat it as stack so
 * the return/global/heap sinks fire. merge_prov is monotone toward STACK, so the
 * taint is conservative (it can only add rejections, never remove them) and
 * branch-safe (a stack assignment in one arm taints the binding for the join). */
static void scope_taint_prov(Scope *s, const char *codegen_name, Provenance prov) {
    if (!codegen_name) return;
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (int i = sc->local_count - 1; i >= 0; i--) {
            if (sc->locals[i].codegen_name &&
                strcmp(sc->locals[i].codegen_name, codegen_name) == 0) {
                sc->locals[i].prov = merge_prov(sc->locals[i].prov, prov);
                return;
            }
        }
    }
}

/* Unify two if-branch / match-arm result types. A `never` (return/break/continue)
   branch carries no value, so it is absorbed by its sibling — the result is the
   other branch's type. When both diverge, the result is `never`. Returns the
   unified type, or NULL if the two concrete types are incompatible (the caller
   emits the appropriate "different types" diagnostic). */
static Type *unify_branch(Type *a, Type *b) {
    /* The unresolved recursion marker carries no value of its own (a branch that
       is only a self-recursive call): it is absorbed by its sibling, exactly like
       `never`. When both sides are markers the result stays a marker — every
       branch recurses, so the function never returns (resolved to `never` later). */
    if (type_is_unresolved(a)) return b;
    if (type_is_unresolved(b)) return a;
    if (type_is_never(a)) return b;
    if (type_is_never(b)) return a;
    if (type_eq(a, b)) return a;
    return NULL;
}

/* Does this type carry provenance (pointer-like, slice-like, or closure-like)?
 * A struct/union carries provenance if any of its fields/variants does — so that
 * a stack-allocated struct with a stack-pointer field can be rejected at the
 * same return/escape sites as a bare stack pointer. */
static bool type_has_provenance(Type *t) {
    if (!t) return false;
    if (t->kind == TYPE_POINTER || t->kind == TYPE_SLICE ||
        t->kind == TYPE_ANY_PTR) return true;
    /* Capturing closures have a stack-allocated context struct */
    if (t->kind == TYPE_FUNC) return true;
    /* Option wrapping a pointer/slice also carries provenance */
    if (t->kind == TYPE_OPTION) return type_has_provenance(t->option.inner);
    if (t->kind == TYPE_STRUCT) {
        for (int i = 0; i < t->struc.field_count; i++) {
            if (type_has_provenance(t->struc.fields[i].type)) return true;
        }
        return false;
    }
    if (t->kind == TYPE_UNION) {
        for (int i = 0; i < t->unio.variant_count; i++) {
            if (t->unio.variants[i].payload &&
                type_has_provenance(t->unio.variants[i].payload)) return true;
        }
        return false;
    }
    /* Conservative: unresolved stubs might refer to a struct with pointer fields */
    if (t->kind == TYPE_STUB) return true;
    return false;
}

/* ---- Loop-carried escape pre-taint ----
 * Escape analysis is flow-insensitive and runs in a single textual pass, so a
 * read of a `let mut` binding that is textually BEFORE its stack-tainting
 * reassignment is checked while the binding still looks safe — yet in a loop
 * that read executes AFTER the assignment on a later iteration:
 *     let mut p = ...
 *     for _ in 0..2
 *         saved = p      // iteration 2 observes &x from iteration 1
 *         let mut x = 5
 *         p = &x
 * Before a loop body is checked we therefore pre-taint: any in-scope mut binding
 * that is assigned a (transitively) stack-derived value anywhere in the body is
 * marked PROV_STACK up front, so every read inside and after the loop sees it.
 * This mirrors the conservative over-rejection the assignment merge already
 * applies across branches; the taint is monotone (never cleared), so the pass
 * can only add rejections, never remove a sound one. */

/* Conservative syntactic predicate: could evaluating `e` produce a value with
 * stack/local provenance? Used only by the pre-taint, so it must not depend on
 * type info pass2 has not computed yet. Call results are excluded — a function
 * can never return a stack pointer (the return sink rejects that); alloc is
 * heap; literals/arithmetic are not pointers. */
static bool expr_may_yield_stack(Scope *scope, Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_AMP) {
            /* &x is a stack address — except &fn, a static C function pointer. */
            Expr *operand = e->unary_prefix.operand;
            if (operand->kind == EXPR_IDENT) {
                Type *ot = scope_lookup_capture(scope, operand->ident.name,
                    NULL, NULL, NULL, NULL);
                if (ot && ot->kind == TYPE_FUNC) return false;
            }
            return true;
        }
        if (e->unary_prefix.op == TOK_STAR)
            return expr_may_yield_stack(scope, e->unary_prefix.operand);
        return false;
    case EXPR_ARRAY_LIT:
    case EXPR_INTERP_STRING:
    case EXPR_SLICE_LIT:
        return true;   /* stack/alloca-backed temporaries */
    case EXPR_IDENT:
        return scope_lookup_prov(scope, e->ident.name) == PROV_STACK;
    case EXPR_CAST:
        return expr_may_yield_stack(scope, e->cast.operand);
    case EXPR_SOME:
        return expr_may_yield_stack(scope, e->some_expr.value);
    case EXPR_UNARY_POSTFIX:           /* option unwrap x! preserves provenance */
        return expr_may_yield_stack(scope, e->unary_postfix.operand);
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        return expr_may_yield_stack(scope, e->field.object);
    case EXPR_INDEX:
        return expr_may_yield_stack(scope, e->index.object);
    case EXPR_SLICE:
        return expr_may_yield_stack(scope, e->slice.object);
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (expr_may_yield_stack(scope, e->struct_lit.fields[i].value)) return true;
        return false;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            if (expr_may_yield_stack(scope, e->tuple_lit.elems[i])) return true;
        return false;
    case EXPR_IF:
        return expr_may_yield_stack(scope, e->if_expr.then_body) ||
               expr_may_yield_stack(scope, e->if_expr.else_body);
    case EXPR_MATCH:
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            int bc = e->match_expr.arms[i].body_count;
            if (bc > 0 && expr_may_yield_stack(scope, e->match_expr.arms[i].body[bc - 1]))
                return true;
        }
        return false;
    case EXPR_BLOCK:
        return e->block.count > 0 &&
               expr_may_yield_stack(scope, e->block.stmts[e->block.count - 1]);
    default:
        return false;
    }
}

/* Find the nearest in-scope binding with this (interned) source name. */
static LocalBinding *scope_find_binding(Scope *s, const char *name) {
    for (Scope *sc = s; sc; sc = sc->parent)
        for (int i = sc->local_count - 1; i >= 0; i--)
            if (sc->locals[i].name == name)
                return &sc->locals[i];
    return NULL;
}

/* One pre-taint sweep over a loop-body expression tree. Taints any in-scope mut
 * binding assigned a may-be-stack value; sets *changed when a binding newly
 * becomes stack so the caller can iterate to a fixpoint (covering p = q; q = &x
 * chains regardless of textual order). Does not descend into nested lambdas:
 * mut bindings cannot be captured, so an assignment there cannot target an outer
 * mut binding. */
static void pretaint_walk(Scope *scope, Expr *e, bool *changed) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_ASSIGN:
        if (e->assign.target->kind == EXPR_IDENT &&
            expr_may_yield_stack(scope, e->assign.value)) {
            LocalBinding *b = scope_find_binding(scope, e->assign.target->ident.name);
            if (b && b->is_mut && b->prov != PROV_STACK && type_has_provenance(b->type)) {
                b->prov = PROV_STACK;
                *changed = true;
            }
        }
        pretaint_walk(scope, e->assign.target, changed);
        pretaint_walk(scope, e->assign.value, changed);
        break;
    case EXPR_FUNC:
        break;   /* nested lambda: own scope, cannot reach outer mut bindings */
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            pretaint_walk(scope, e->block.stmts[i], changed);
        break;
    case EXPR_IF:
        pretaint_walk(scope, e->if_expr.cond, changed);
        pretaint_walk(scope, e->if_expr.then_body, changed);
        pretaint_walk(scope, e->if_expr.else_body, changed);
        break;
    case EXPR_MATCH:
        pretaint_walk(scope, e->match_expr.subject, changed);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                pretaint_walk(scope, e->match_expr.arms[i].body[j], changed);
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            pretaint_walk(scope, e->loop_expr.body[i], changed);
        break;
    case EXPR_FOR:
        pretaint_walk(scope, e->for_expr.iter, changed);
        if (e->for_expr.range_end) pretaint_walk(scope, e->for_expr.range_end, changed);
        for (int i = 0; i < e->for_expr.body_count; i++)
            pretaint_walk(scope, e->for_expr.body[i], changed);
        break;
    case EXPR_LET:
        pretaint_walk(scope, e->let_expr.let_init, changed);
        break;
    case EXPR_LET_DESTRUCT:
        pretaint_walk(scope, e->let_destruct.init, changed);
        break;
    case EXPR_RETURN:
        pretaint_walk(scope, e->return_expr.value, changed);
        break;
    case EXPR_BREAK:
        pretaint_walk(scope, e->break_expr.value, changed);
        break;
    case EXPR_DEFER:
        pretaint_walk(scope, e->defer_expr.value, changed);
        break;
    case EXPR_BINARY:
        pretaint_walk(scope, e->binary.left, changed);
        pretaint_walk(scope, e->binary.right, changed);
        break;
    case EXPR_UNARY_PREFIX:
        pretaint_walk(scope, e->unary_prefix.operand, changed);
        break;
    case EXPR_UNARY_POSTFIX:
        pretaint_walk(scope, e->unary_postfix.operand, changed);
        break;
    case EXPR_CALL:
        pretaint_walk(scope, e->call.func, changed);
        for (int i = 0; i < e->call.arg_count; i++)
            pretaint_walk(scope, e->call.args[i], changed);
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        pretaint_walk(scope, e->field.object, changed);
        break;
    case EXPR_INDEX:
        pretaint_walk(scope, e->index.object, changed);
        pretaint_walk(scope, e->index.index, changed);
        break;
    case EXPR_SLICE:
        pretaint_walk(scope, e->slice.object, changed);
        pretaint_walk(scope, e->slice.lo, changed);
        pretaint_walk(scope, e->slice.hi, changed);
        break;
    case EXPR_CAST:
        pretaint_walk(scope, e->cast.operand, changed);
        break;
    case EXPR_SOME:
        pretaint_walk(scope, e->some_expr.value, changed);
        break;
    case EXPR_ASSERT:
        pretaint_walk(scope, e->assert_expr.condition, changed);
        pretaint_walk(scope, e->assert_expr.message, changed);
        break;
    case EXPR_ALLOC:
        pretaint_walk(scope, e->alloc_expr.init_expr, changed);
        pretaint_walk(scope, e->alloc_expr.size_expr, changed);
        break;
    case EXPR_FREE:
        pretaint_walk(scope, e->free_expr.operand, changed);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            pretaint_walk(scope, e->struct_lit.fields[i].value, changed);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            pretaint_walk(scope, e->array_lit.elems[i], changed);
        break;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            pretaint_walk(scope, e->tuple_lit.elems[i], changed);
        break;
    case EXPR_SLICE_LIT:
        pretaint_walk(scope, e->slice_lit.ptr_expr, changed);
        pretaint_walk(scope, e->slice_lit.len_expr, changed);
        break;
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++)
            if (!e->interp_string.segments[i].is_literal)
                pretaint_walk(scope, e->interp_string.segments[i].expr, changed);
        break;
    case EXPR_ATOMIC_LOAD:
        pretaint_walk(scope, e->atomic_load.ptr, changed);
        break;
    case EXPR_ATOMIC_STORE:
        pretaint_walk(scope, e->atomic_store.ptr, changed);
        pretaint_walk(scope, e->atomic_store.value, changed);
        break;
    default:
        break;
    }
}

/* Pre-taint a loop body to a fixpoint before it is type-checked. */
static void pretaint_loop_body(Scope *scope, Expr **body, int count) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < count; i++)
            pretaint_walk(scope, body[i], &changed);
    }
}

/* ---- Type checking ---- */

typedef struct ImportScope {
    ImportTable *table;
    struct ImportScope *parent;
} ImportScope;

/* On-demand type-check visited set — stack-allocated linked list to detect cycles */
typedef struct OnDemandVisited {
    Decl *decl;
    struct OnDemandVisited *next;
} OnDemandVisited;

/* Linked list of parent module symbol tables for arbitrary nesting depth.
 * Enables child modules to see symbols from all ancestor modules. */
typedef struct ModuleScopeChain {
    SymbolTable *members;
    struct ImportScope *import_scope; /* import scope at this module level */
    struct ModuleScopeChain *parent;
} ModuleScopeChain;

typedef struct {
    SymbolTable *symtab;
    Scope *scope;
    Arena *arena;
    Type **loop_break_type;  /* non-NULL when inside a loop; points to break value type */
    bool in_for;             /* true when inside a for loop (break value forbidden) */
    SymbolTable *module_symtab;  /* non-NULL when checking inside a module */
    ModuleScopeChain *parent_modules;  /* chain of ancestor module symtabs (nearest first) */
    const char *current_ns;      /* current namespace for namespace isolation */
    Type *recursive_ret;         /* TYPE_UNRESOLVED placeholder while resolving a recursive
                                    function's body; scoped to that body by EXPR_FUNC */
    const char *recursive_self_name; /* interned name of the function being resolved, so the
                                    if/match handlers can order base-case branches before
                                    branches that consume a self-recursive call's result */
    /* One-shot channel to the recursive function's own EXPR_FUNC (set by the enclosing
       EXPR_LET / check_decl_let, consumed and cleared by EXPR_FUNC). Keeps the placeholder
       from leaking into nested or anonymous lambda bodies defined inside the recursive one. */
    Type *pending_recursive_ret;
    const char *pending_recursive_self;
    /* One-shot channel from EXPR_LET to the EXPR_FUNC it wraps, so a `let f = <lambda>`
       binding becomes visible inside its own body (self-recursion). Set by EXPR_LET,
       consumed and cleared by EXPR_FUNC before checking the body so nested lambdas
       don't inherit it. */
    const char *pending_self_name;
    const char *pending_self_codegen;
    Type *pending_self_type;     /* partial function type with mutable placeholder return */
    LambdaCtx *lambda_ctx;       /* capture tracking for lambdas, NULL outside lambdas */
    bool is_top_level_init;      /* true when checking the init of a top-level DECL_LET */
    MonoTable *mono_table;       /* global instantiation registry */
    InternTable *intern;         /* for name mangling */
    ImportScope *import_scope;   /* lexically scoped import chain */
    OnDemandVisited *on_demand_visited;  /* cycle detection for on-demand type checking */
    FileImportScopes *file_scopes;  /* per-file import tables — needed to rebuild scope on on-demand checks */
    /* One-shot flag: set true by EXPR_CALL only while checking its direct callee,
       so EXPR_IDENT can allow a generic function in call position but reject it in
       value position (a generic function has no single concrete type until
       instantiated). Consumed (cleared) the moment an EXPR_IDENT reads it. */
    bool in_callee_position;
    /* One-shot flag: set true while checking a `%T` interpolation segment. `%T`
       is compile-time type reflection — it prints the expression's type and
       never emits it as a runtime value, so a generic function is permitted here
       even though it is rejected in ordinary value position. */
    bool in_reflection_position;
    /* Best-effort source location for type-resolution diagnostics (resolve_type
       has no loc of its own). Set broadly at each expression and precisely at
       function-parameter resolution; used to attribute "unknown type name". */
    SrcLoc type_loc;
} CheckCtx;

static Type *check_expr(CheckCtx *ctx, Expr *e);
static Type *check_expr_inner(CheckCtx *ctx, Expr *e);
static void check_decl_let(CheckCtx *ctx, Decl *d);

/* ---- Recursive return-type inference: branch ordering ----
 * A recursive function's return type is inferred from its base cases. The base
 * case may sit in any branch, but a self-recursive call cannot be typed until
 * that base case has anchored the return type. So when checking an if/match
 * inside a recursive body we check anchor-able (base-case) branches first, then
 * the branches that consume a self-recursive result. These helpers decide the
 * order; they only ever change the order of checking, never the resulting types,
 * so a misjudgment can at worst forgo an ordering improvement. */

/* Does e syntactically reference the recursive function `self` (called or used
   as a value)? Does not descend into nested lambda bodies — a self-reference
   there is deferred (the lambda is a value, not evaluated as part of e), so it
   does not make e's own value depend on the not-yet-inferred return type. */
static bool expr_refs_self(Expr *e, const char *self) {
    if (!e || !self) return false;
    switch (e->kind) {
    case EXPR_IDENT:        return e->ident.name == self;
    case EXPR_BINARY:       return expr_refs_self(e->binary.left, self) ||
                                   expr_refs_self(e->binary.right, self);
    case EXPR_UNARY_PREFIX: return expr_refs_self(e->unary_prefix.operand, self);
    case EXPR_UNARY_POSTFIX:return expr_refs_self(e->unary_postfix.operand, self);
    case EXPR_CALL:
        if (expr_refs_self(e->call.func, self)) return true;
        for (int i = 0; i < e->call.arg_count; i++)
            if (expr_refs_self(e->call.args[i], self)) return true;
        return false;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:  return expr_refs_self(e->field.object, self);
    case EXPR_INDEX:        return expr_refs_self(e->index.object, self) ||
                                   expr_refs_self(e->index.index, self);
    case EXPR_SLICE:        return expr_refs_self(e->slice.object, self) ||
                                   (e->slice.lo && expr_refs_self(e->slice.lo, self)) ||
                                   (e->slice.hi && expr_refs_self(e->slice.hi, self));
    case EXPR_CAST:         return expr_refs_self(e->cast.operand, self);
    case EXPR_SOME:         return expr_refs_self(e->some_expr.value, self);
    case EXPR_ASSERT:       return expr_refs_self(e->assert_expr.condition, self);
    case EXPR_IF:           return expr_refs_self(e->if_expr.cond, self) ||
                                   expr_refs_self(e->if_expr.then_body, self) ||
                                   (e->if_expr.else_body && expr_refs_self(e->if_expr.else_body, self));
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            if (expr_refs_self(e->block.stmts[i], self)) return true;
        return false;
    case EXPR_MATCH:
        if (expr_refs_self(e->match_expr.subject, self)) return true;
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            for (int j = 0; j < arm->body_count; j++)
                if (expr_refs_self(arm->body[j], self)) return true;
        }
        return false;
    case EXPR_RETURN:       return e->return_expr.value && expr_refs_self(e->return_expr.value, self);
    case EXPR_BREAK:        return e->break_expr.value && expr_refs_self(e->break_expr.value, self);
    /* Other forms (literals, nested EXPR_FUNC, struct/tuple/slice literals, …) do
       not contribute a self-recursive call to a value position we order against;
       treat them as self-free. */
    default:                return false;
    }
}

/* Can checking this branch anchor the recursive return type — i.e. does it have a
   tail value (reachable through if/match/block control flow) that does not consume
   a self-recursive call? Such branches are checked before recursive branches. */
static bool branch_can_anchor(Expr *e, const char *self) {
    if (!e) return true;   /* a missing/void tail consumes no self-call */
    switch (e->kind) {
    case EXPR_IF:
        return branch_can_anchor(e->if_expr.then_body, self) ||
               (e->if_expr.else_body ? branch_can_anchor(e->if_expr.else_body, self) : true);
    case EXPR_MATCH:
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            Expr *tail = arm->body_count > 0 ? arm->body[arm->body_count - 1] : NULL;
            if (branch_can_anchor(tail, self)) return true;
        }
        return false;
    case EXPR_BLOCK:
        return e->block.count > 0
            ? branch_can_anchor(e->block.stmts[e->block.count - 1], self)
            : true;
    default:
        return !expr_refs_self(e, self);
    }
}

/* --- Unconditional infinite self-recursion detection ---------------------------
 *
 * A function whose every control-flow path reaches a self-recursive call before it
 * can return or fall off the end never returns. Its generated C is rejected by the
 * C compiler (gcc's -Winfinite-recursion, which is in -Wall -Werror), so we reject
 * it here with a clear FC diagnostic instead. An intentional infinite *loop* is
 * written `loop ...` (a breakless loop never makes a self-call, so it is never
 * flagged — it compiles to `while (1)`).
 *
 * The analysis abstracts each expression to the set of ways control can leave it:
 * complete normally, return from the function, make a self-recursive call, or
 * break/continue an enclosing loop. A function is rejected exactly when its body
 * can ONLY leave via a self-recursive call — no completing path, no return, and no
 * loop exit. This is sound (it only flags when every path provably self-recurses),
 * so a real base case — reachable on any path — always keeps it accepted. */
enum {
    SR_COMPLETE = 1 << 0,   /* falls off the end normally */
    SR_RETURN   = 1 << 1,   /* a `return [value]` leaves the function */
    SR_SELFREC  = 1 << 2,   /* a self-recursive call is reached */
    SR_BREAK    = 1 << 3,   /* a `break` leaves the enclosing loop */
    SR_CONTINUE = 1 << 4,   /* a `continue` restarts the enclosing loop */
};

/* A *direct* self-recursive call: invoked by the function's own name (so a call to a
   mutually-recursive sibling is excluded — gcc only flags direct self-recursion) AND
   carrying the function's own return placeholder as its result type. The placeholder
   match — pointer-identical whether or not a base case later anchored that cell —
   excludes a same-named inner binding that shadows the function (it resolves to a
   different type). Together they pin exactly the calls that recurse into THIS body. */
static bool sr_is_self_call(Expr *e, const char *self, Type *placeholder) {
    return e->kind == EXPR_CALL &&
           e->call.func->kind == EXPR_IDENT &&
           e->call.func->ident.name == self &&
           e->type == placeholder;
}

static int sr_flow_block(Expr **stmts, int count, const char *self, Type *ph);

/* Outcome set of evaluating one expression in statement/tail position. */
static int sr_flow(Expr *e, const char *self, Type *ph) {
    if (!e) return SR_COMPLETE;
    switch (e->kind) {
    case EXPR_CALL:
        return sr_is_self_call(e, self, ph) ? SR_SELFREC : SR_COMPLETE;
    case EXPR_RETURN: {
        /* A self-call in the returned value recurses before control transfers out. */
        if (e->return_expr.value) {
            int vf = sr_flow(e->return_expr.value, self, ph);
            if ((vf & SR_SELFREC) && !(vf & SR_COMPLETE)) return SR_SELFREC;
        }
        return SR_RETURN;
    }
    case EXPR_BREAK:    return SR_BREAK;
    case EXPR_CONTINUE: return SR_CONTINUE;
    case EXPR_BLOCK:    return sr_flow_block(e->block.stmts, e->block.count, self, ph);
    case EXPR_IF: {
        /* The condition is evaluated unconditionally but a self-call there is
           consumed as a value and rejected elsewhere; the branch bodies decide
           whether the construct can complete. A missing `else` completes. */
        int tf = sr_flow(e->if_expr.then_body, self, ph);
        int ef = e->if_expr.else_body ? sr_flow(e->if_expr.else_body, self, ph) : SR_COMPLETE;
        return tf | ef;
    }
    case EXPR_MATCH: {
        /* `match` is exhaustive, so its outcome is the union of its arms — there is
           no implicit completing path. */
        int acc = 0;
        for (int i = 0; i < e->match_expr.arm_count; i++)
            acc |= sr_flow_block(e->match_expr.arms[i].body,
                                 e->match_expr.arms[i].body_count, self, ph);
        return acc ? acc : SR_COMPLETE;
    }
    case EXPR_LOOP: {
        /* A breakless loop never completes or continues outward on its own; only a
           `break` lets it exit normally. A self-call reached on every iteration path
           makes the loop self-recurse. */
        int f = sr_flow_block(e->loop_expr.body, e->loop_expr.body_count, self, ph);
        int out = 0;
        if (f & SR_RETURN)  out |= SR_RETURN;
        if (f & SR_SELFREC) out |= SR_SELFREC;
        if (f & SR_BREAK)   out |= SR_COMPLETE;
        return out;   /* empty set = clean infinite loop: never returns, no self-call */
    }
    case EXPR_FOR:
        /* A `for` may iterate zero times, so it can always complete; a body self-call
           is therefore conditional, never on every path. A `return` in the body is
           still reachable; break/continue stay within the loop. */
        return SR_COMPLETE |
               (sr_flow_block(e->for_expr.body, e->for_expr.body_count, self, ph) & SR_RETURN);
    default:
        /* Everything else (value expressions, let bindings, lambda definitions,
           assignments) completes for the purpose of this analysis; a self-call buried
           in such a position is consumed as a value and rejected at that site. */
        return SR_COMPLETE;
    }
}

/* Outcome set of a statement sequence: walk until a statement cannot complete, then
   stop (later statements are unreachable). The block completes only if control can
   thread normal completion through every statement. */
static int sr_flow_block(Expr **stmts, int count, const char *self, Type *ph) {
    int acc = 0;
    bool reachable = true;
    for (int i = 0; i < count; i++) {
        int f = sr_flow(stmts[i], self, ph);
        acc |= (f & (SR_RETURN | SR_SELFREC | SR_BREAK | SR_CONTINUE));
        if (!(f & SR_COMPLETE)) { reachable = false; break; }
    }
    if (reachable) acc |= SR_COMPLETE;
    return acc;
}

/* The function body provably never returns because every path self-recurses: a
   self-call is reachable and no path completes, returns, or exits a loop. */
static bool body_always_self_recurses(Expr **body, int count, const char *self,
                                      Type *placeholder) {
    if (!self || !placeholder) return false;
    int out = sr_flow_block(body, count, self, placeholder);
    return (out & SR_SELFREC) &&
           !(out & (SR_COMPLETE | SR_RETURN | SR_BREAK | SR_CONTINUE));
}

/* A self-recursive call's result reaching a value-consuming position (a binary
   operand, call argument, cast, index, …) while the return type is still the
   unresolved placeholder means the recursion has no base case to anchor a return
   type before the result is used — genuine non-termination, the same fault as a
   body that always recurses. Report it with the same message instead of leaking the
   internal `<unresolved>` placeholder into a downstream type-mismatch diagnostic,
   and poison the type so nothing else complains. Sound by construction: in a valid
   recursive function a reachable base case anchors the placeholder (via the if/match
   branch reordering) before any operand is consumed, so this never misfires. Returns
   true when it fired (the operand was an unresolved recursive result). */
static bool reject_unresolved_recursive_value(Expr *e) {
    if (e && e->type && e->type->kind == TYPE_UNRESOLVED) {
        diag_error(e->loc, "this function never returns: it calls itself on every "
            "path with no base case; use 'loop' for an intentional infinite loop");
        e->type = type_error();
        return true;
    }
    return false;
}

/* True while a recursive function's return type is still being inferred. */
static bool resolving_recursion(CheckCtx *ctx) {
    return ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_UNRESOLVED &&
           ctx->recursive_self_name;
}

/* Anchor an unresolved recursive return type to the first concrete branch type
   seen (skipping void/never/error/unresolved), so a sibling branch's recursive
   call observes the inferred type. */
static void maybe_anchor_recursive(CheckCtx *ctx, Type *t) {
    if (ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_UNRESOLVED &&
        t && t->kind != TYPE_VOID && t->kind != TYPE_NEVER &&
        t->kind != TYPE_UNRESOLVED && !type_is_error(t))
        *ctx->recursive_ret = *t;
}

/* Saved CheckCtx fields for save/restore around on-demand scope switches. */
typedef struct {
    SymbolTable *module_symtab;
    ModuleScopeChain *parent_modules;
    ImportScope *import_scope;
    const char *current_ns;
    Scope *scope;
} SavedCtxScope;

/* Reconstruct ctx.module_symtab, parent_modules, import_scope, and current_ns
 * from a module Symbol's parent chain (set by pass1). Used when on-demand
 * type-checking must enter a module's scope outside the natural
 * check_module_members walk — e.g., when a user module calls into a function
 * declared in a stdlib module before the stdlib module has been checked.
 * Without this reconstruction, bare stub names in the target module's
 * signatures (like `pcg_random*` inside `std::random.pcg_random.next_uint32`)
 * would fail to resolve, leaving the parameter types as unresolved stubs. */
static void enter_module_scope_on_demand(CheckCtx *ctx, Symbol *mod_sym,
                                          SavedCtxScope *saved) {
    saved->module_symtab = ctx->module_symtab;
    saved->parent_modules = ctx->parent_modules;
    saved->import_scope = ctx->import_scope;
    saved->current_ns = ctx->current_ns;
    saved->scope = ctx->scope;

    enum { MAX_DEPTH = 64 };
    Symbol *chain[MAX_DEPTH];
    int depth = 0;
    for (Symbol *s = mod_sym; s && depth < MAX_DEPTH; s = s->parent) {
        chain[depth++] = s;
    }
    /* chain[0] = mod_sym (innermost); chain[depth-1] = outermost ancestor */

    ImportTable *file_tbl = NULL;
    const char *fn = mod_sym->decl ? mod_sym->decl->loc.filename : NULL;
    if (ctx->file_scopes && fn) {
        for (int fi = 0; fi < ctx->file_scopes->count; fi++) {
            if (ctx->file_scopes->scopes[fi].filename == fn) {
                file_tbl = &ctx->file_scopes->scopes[fi].imports;
                break;
            }
        }
    }

    ImportScope *scope = NULL;
    if (file_tbl) {
        ImportScope *fs = arena_alloc(ctx->arena, sizeof(ImportScope));
        fs->table = file_tbl;
        fs->parent = NULL;
        scope = fs;
    }

    /* Build import scope bottom-up (outermost ancestor first). After the
     * i-th iteration, scope = imports visible while checking chain[i]. */
    ImportScope *scope_at_level[MAX_DEPTH];
    for (int i = depth - 1; i >= 0; i--) {
        if (chain[i]->imports) {
            ImportScope *is = arena_alloc(ctx->arena, sizeof(ImportScope));
            is->table = chain[i]->imports;
            is->parent = scope;
            scope = is;
        }
        scope_at_level[i] = scope;
    }

    /* Build parent_modules chain: innermost parent first. chain[0] is the
     * current module, so parents are chain[1..depth-1]. */
    ModuleScopeChain *pc = NULL;
    for (int i = depth - 1; i >= 1; i--) {
        ModuleScopeChain *pcn = arena_alloc(ctx->arena, sizeof(ModuleScopeChain));
        pcn->members = chain[i]->members;
        pcn->import_scope = scope_at_level[i];
        pcn->parent = pc;
        pc = pcn;
    }

    ctx->module_symtab = mod_sym->members;
    ctx->parent_modules = pc;
    ctx->import_scope = scope_at_level[0];
    ctx->current_ns = mod_sym->ns_prefix;
    ctx->scope = scope_new(ctx->arena, NULL);
    ctx->scope->is_global = true;
}

static void restore_scope(CheckCtx *ctx, SavedCtxScope *saved) {
    ctx->module_symtab = saved->module_symtab;
    ctx->parent_modules = saved->parent_modules;
    ctx->import_scope = saved->import_scope;
    ctx->current_ns = saved->current_ns;
    ctx->scope = saved->scope;
}

/* Look up a name in the import scope chain (innermost first = shadowing).
 * Searches from `scope` up to (but not including) `stop`.
 * Pass stop=NULL to search the entire chain. */
static Symbol *import_scope_lookup_until(ImportScope *scope, const char *name,
                                         ImportScope *stop) {
    for (ImportScope *s = scope; s && s != stop; s = s->parent) {
        if (s->table) {
            for (int i = s->table->count - 1; i >= 0; i--) {
                ImportRef *ref = &s->table->entries[i];
                if (ref->local_name == name) {
                    /* Module imports need namespace-aware lookup to avoid
                     * finding the wrong module when two namespaces define
                     * modules with the same source name. */
                    if (ref->kind == DECL_MODULE)
                        return symtab_lookup_module(ref->source_members, ref->source_name, ref->ns_prefix);
                    return symtab_lookup(ref->source_members, ref->source_name);
                }
            }
        }
    }
    return NULL;
}

static Symbol *import_scope_lookup_kind_until(ImportScope *scope, const char *name,
                                              DeclKind kind, ImportScope *stop) {
    for (ImportScope *s = scope; s && s != stop; s = s->parent) {
        if (s->table) {
            for (int i = s->table->count - 1; i >= 0; i--) {
                ImportRef *ref = &s->table->entries[i];
                if (ref->local_name == name && ref->kind == kind) {
                    if (kind == DECL_MODULE)
                        return symtab_lookup_module(ref->source_members, ref->source_name, ref->ns_prefix);
                    /* Namespace-aware: a struct/union imported by alias must
                     * resolve to the type in ITS source namespace, not the
                     * first same-named type in compilation order. ref->ns_prefix
                     * mirrors the imported symbol's namespace. */
                    return symtab_lookup_kind_ns(ref->source_members, ref->source_name, kind, ref->ns_prefix);
                }
            }
        }
    }
    return NULL;
}

/* Look up ImportRef metadata within a bounded import scope range */
static ImportRef *import_scope_find_ref_until(ImportScope *scope, const char *name,
                                              ImportScope *stop) {
    for (ImportScope *s = scope; s && s != stop; s = s->parent) {
        if (s->table) {
            for (int i = s->table->count - 1; i >= 0; i--) {
                ImportRef *ref = &s->table->entries[i];
                if (ref->local_name == name) return ref;
            }
        }
    }
    return NULL;
}



/* Namespace-aware global symtab lookup for non-module symbols.
 * Top-level declarations are registered with ns_prefix set to their enclosing
 * namespace (or NULL for global::); lookup filters to only return entries
 * whose ns_prefix matches current_ns. Multiple top-level declarations with
 * the same name may coexist across different namespaces, so the scan must
 * walk all entries rather than stopping at the first name match.
 *
 * Module-scoped types are registered under mangled names (e.g. "m__entry"),
 * so they don't collide with top-level user-visible names. */
static Symbol *global_lookup(SymbolTable *symtab, const char *name, const char *current_ns) {
    for (int i = 0; i < symtab->count; i++) {
        Symbol *s = &symtab->symbols[i];
        if (s->name == name && s->ns_prefix == current_ns) return s;
    }
    return NULL;
}

static Symbol *global_lookup_kind(SymbolTable *symtab, const char *name, DeclKind kind,
                                  const char *current_ns) {
    return symtab_lookup_kind_ns(symtab, name, kind, current_ns);
}

/* Interleaved symbol resolution: at each module level, check members then
 * that level's imports before moving to the parent.  This ensures a child's
 * import can shadow a parent's member — consistent with "imports follow the
 * same lexical scoping rules as let bindings."
 *
 * Order: module_symtab → current imports → parent[0] members → parent[0]
 *        imports → … → remaining imports → global. */
static Symbol *resolve_symbol(CheckCtx *ctx, const char *name) {
    /* 1. Current module members */
    if (ctx->module_symtab) {
        Symbol *sym = symtab_lookup(ctx->module_symtab, name);
        if (sym) return sym;
    }
    /* 2. Interleaved: current imports → parent members → parent imports → … */
    ImportScope *imp = ctx->import_scope;
    for (ModuleScopeChain *p = ctx->parent_modules; ; p = p->parent) {
        ImportScope *stop = p ? p->import_scope : NULL;
        Symbol *sym = import_scope_lookup_until(imp, name, stop);
        if (sym) return sym;
        if (!p) break;
        sym = symtab_lookup(p->members, name);
        if (sym) return sym;
        imp = p->import_scope;
    }
    /* 3. Global */
    return global_lookup(ctx->symtab, name, ctx->current_ns);
}

static Symbol *resolve_symbol_kind(CheckCtx *ctx, const char *name, DeclKind kind) {
    /* 1. Current module members */
    if (ctx->module_symtab) {
        Symbol *sym = symtab_lookup_kind(ctx->module_symtab, name, kind);
        if (sym) return sym;
    }
    /* 2. Interleaved: current imports → parent members → parent imports → … */
    ImportScope *imp = ctx->import_scope;
    for (ModuleScopeChain *p = ctx->parent_modules; ; p = p->parent) {
        ImportScope *stop = p ? p->import_scope : NULL;
        Symbol *sym = import_scope_lookup_kind_until(imp, name, kind, stop);
        if (sym) return sym;
        if (!p) break;
        sym = symtab_lookup_kind(p->members, name, kind);
        if (sym) return sym;
        imp = p->import_scope;
    }
    /* 3. Global */
    if (kind == DECL_MODULE)
        return symtab_lookup_module(ctx->symtab, name, ctx->current_ns);
    return global_lookup_kind(ctx->symtab, name, kind, ctx->current_ns);
}

static Type *check_block(CheckCtx *ctx, Expr **stmts, int count) {
    if (count == 0) return type_void();
    Type *last = type_void();
    for (int i = 0; i < count; i++) {
        last = check_expr(ctx, stmts[i]);
    }
    return last;
}

/* Look up a dotted name (e.g., "module.type" or "a.b.type") by walking the module chain.
 * Returns the symbol for the final member, or NULL if any segment is not found. */
static Symbol *resolve_dotted_name_ex(CheckCtx *ctx, const char *dotted_name,
    SymbolTable **out_owner_members) {
    const char *dot = strchr(dotted_name, '.');
    if (!dot) return NULL;

    const char *path = dotted_name;
    Symbol *mod_sym = NULL;
    while (dot) {
        int seg_len = (int)(dot - path);
        const char *seg_name = intern(ctx->intern, path, seg_len);
        if (!mod_sym) {
            mod_sym = resolve_symbol_kind(ctx, seg_name, DECL_MODULE);
        } else {
            mod_sym = mod_sym->members ?
                symtab_lookup_kind(mod_sym->members, seg_name, DECL_MODULE) : NULL;
        }
        if (!mod_sym) return NULL;
        path = dot + 1;
        dot = strchr(path, '.');
    }
    if (!mod_sym || !mod_sym->members) return NULL;
    if (out_owner_members) *out_owner_members = mod_sym->members;
    const char *member_name = intern_cstr(ctx->intern, path);
    Symbol *sym = symtab_lookup_kind(mod_sym->members, member_name, DECL_STRUCT);
    if (!sym) sym = symtab_lookup_kind(mod_sym->members, member_name, DECL_UNION);
    if (!sym) sym = symtab_lookup(mod_sym->members, member_name);
    return sym;
}

static Symbol *resolve_dotted_name(CheckCtx *ctx, const char *dotted_name) {
    return resolve_dotted_name_ex(ctx, dotted_name, NULL);
}

/* Walk a type tree and rewrite TYPE_STUB names to their canonical mangled
 * form. For dotted names (e.g., "a.foo") resolve via the module chain; for
 * bare names (e.g., "point" from a member import) resolve via the normal
 * scope chain. Mutates in place. Codegen's resolve_struct_stub then finds
 * the type via the global symtab.
 *
 * This is the field-type counterpart to the resolve_type path used for
 * function parameters and expressions. We keep the node as a TYPE_STUB (not
 * the full struct type) to avoid creating cycles for self-referential
 * structs like `node { next: node*? }`. */
static void register_concrete_tuple(CheckCtx *ctx, Type *tup);

static void canonicalize_field_stubs(CheckCtx *ctx, Type *t) {
    if (!t) return;
    switch (t->kind) {
    case TYPE_POINTER: canonicalize_field_stubs(ctx, t->pointer.pointee); return;
    case TYPE_OPTION:  canonicalize_field_stubs(ctx, t->option.inner); return;
    case TYPE_SLICE:   canonicalize_field_stubs(ctx, t->slice.elem); return;
    case TYPE_FIXED_ARRAY: canonicalize_field_stubs(ctx, t->fixed_array.elem); return;
    case TYPE_STRUCT:
        /* A tuple field type: canonicalize element stub names, then name+register
         * this tuple *in place* so the struct decl's own field-type object carries
         * the mangled name. Codegen's by-value dependency sort reads that name to
         * order the tuple's typedef before this struct. (Named struct field types
         * are kept as stubs, so a real TYPE_STRUCT here is always a tuple.) */
        if (t->struc.is_tuple) {
            for (int i = 0; i < t->struc.field_count; i++)
                canonicalize_field_stubs(ctx, t->struc.fields[i].type);
            if (!type_contains_type_var(t))
                register_concrete_tuple(ctx, t);
        }
        return;
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            canonicalize_field_stubs(ctx, t->func.param_types[i]);
        canonicalize_field_stubs(ctx, t->func.return_type);
        return;
    case TYPE_STUB:
        for (int i = 0; i < t->stub.type_arg_count; i++)
            canonicalize_field_stubs(ctx, t->stub.type_args[i]);
        if (t->stub.name) {
            Symbol *sym = NULL;
            if (strchr(t->stub.name, '.'))
                sym = resolve_dotted_name(ctx, t->stub.name);
            if (!sym)
                sym = resolve_symbol_kind(ctx, t->stub.name, DECL_STRUCT);
            if (!sym)
                sym = resolve_symbol_kind(ctx, t->stub.name, DECL_UNION);
            if (sym && sym->type) {
                const char *canon = NULL;
                if (sym->type->kind == TYPE_STRUCT) canon = sym->type->struc.name;
                else if (sym->type->kind == TYPE_UNION) canon = sym->type->unio.name;
                if (canon && canon != t->stub.name)
                    t->stub.name = canon;
            }
        }
        return;
    default: return;
    }
}

/* Canonicalize stub names in all field/payload types of a struct/union decl. */
static void canonicalize_decl_field_stubs(CheckCtx *ctx, Decl *d) {
    if (d->kind == DECL_STRUCT) {
        for (int i = 0; i < d->struc.field_count; i++)
            canonicalize_field_stubs(ctx, d->struc.fields[i].type);
    } else if (d->kind == DECL_UNION) {
        for (int i = 0; i < d->unio.variant_count; i++)
            canonicalize_field_stubs(ctx, d->unio.variants[i].payload);
    }
}

/* Resolve a TYPE_STUB to the actual type from symtab */
/* Register a fully-concrete tuple struct so codegen emits its typedef, generated
 * == function, and default. Idempotent via the mono table's dedup-by-name. Sets
 * the tuple's canonical interned name. Element types must already be resolved and
 * fully concrete (no type variables). */
static void register_concrete_tuple(CheckCtx *ctx, Type *tup) {
    const char *name = tuple_canonical_name(ctx->arena, ctx->intern,
                                            tup->struc.fields, tup->struc.field_count);
    tup->struc.name = name;
    tup->struc.qualified_name = name;
    Type *noargs[1] = {0};   /* non-NULL placeholder; count 0 means it's never read */
    const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                                        name, NULL, noargs, 0, NULL, DECL_STRUCT,
                                        NULL, 0);
    MonoInstance *mi = mono_find(ctx->mono_table, mangled);
    if (mi && !mi->concrete_type) {
        /* Deep copy so the in-place name canonicalization below cannot mutate a
         * field subtree still shared with the live tuple expression type. */
        Type *ct = type_deep_copy(ctx->arena, tup);
        mono_resolve_type_names(ctx->mono_table, ctx->arena, ctx->intern, ct);
        mi->concrete_type = ct;
    }
}

static Type *resolve_type(CheckCtx *ctx, Type *t) {
    if (!t || t->kind == TYPE_ERROR) return t;

    /* Tuple type: resolve each element, then either register it as a concrete
     * synthesized struct (for codegen) or, if any element is a type variable,
     * leave it for monomorphization. type_eq compares tuples structurally, so a
     * not-yet-named generic tuple still unifies correctly. */
    if (t->kind == TYPE_STRUCT && t->struc.is_tuple) {
        int n = t->struc.field_count;
        bool changed = false, generic = false;
        Type **elems = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) {
            elems[i] = resolve_type(ctx, t->struc.fields[i].type);
            if (elems[i] != t->struc.fields[i].type) changed = true;
            if (type_contains_type_var(elems[i])) generic = true;
        }
        Type *tup = changed ? type_tuple(ctx->arena, elems, n) : t;
        if (generic) {
            tup->struc.name = tuple_canonical_name(ctx->arena, ctx->intern,
                                                   tup->struc.fields, n);
            tup->struc.qualified_name = tup->struc.name;
            return tup;
        }
        register_concrete_tuple(ctx, tup);
        return tup;
    }

    /* Recurse into compound types */
    if (t->kind == TYPE_POINTER) {
        Type *inner = resolve_type(ctx, t->pointer.pointee);
        if (inner != t->pointer.pointee) {
            Type *r = type_pointer(ctx->arena, inner);
            r->is_const = t->is_const;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_OPTION) {
        Type *inner = resolve_type(ctx, t->option.inner);
        if (inner != t->option.inner) return type_option(ctx->arena, inner);
        return t;
    }
    if (t->kind == TYPE_SLICE) {
        Type *inner = resolve_type(ctx, t->slice.elem);
        if (inner != t->slice.elem) {
            Type *r = type_slice(ctx->arena, inner);
            r->is_const = t->is_const;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_FIXED_ARRAY) {
        Type *inner = resolve_type(ctx, t->fixed_array.elem);
        if (inner != t->fixed_array.elem)
            return type_fixed_array(ctx->arena, inner, t->fixed_array.size);
        return t;
    }
    if (t->kind == TYPE_FUNC) {
        bool changed = false;
        Type **params = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)t->func.param_count);
        for (int i = 0; i < t->func.param_count; i++) {
            params[i] = resolve_type(ctx, t->func.param_types[i]);
            if (params[i] != t->func.param_types[i]) changed = true;
        }
        Type *ret = resolve_type(ctx, t->func.return_type);
        if (ret != t->func.return_type) changed = true;
        if (!changed) return t;
        Type *nf = arena_alloc(ctx->arena, sizeof(Type));
        nf->kind = TYPE_FUNC;
        nf->func.param_types = params;
        nf->func.param_count = t->func.param_count;
        nf->func.return_type = ret;
        nf->func.type_params = t->func.type_params;
        nf->func.type_param_count = t->func.type_param_count;
        return nf;
    }

    if (t->kind == TYPE_STUB && t->stub.name) {
        /* Look up as struct or union using the standard scope chain.
         * Kind-filtering avoids returning a module in the companion pattern. */
        Symbol *sym = NULL;

        if (strchr(t->stub.name, '.'))
            sym = resolve_dotted_name(ctx, t->stub.name);
        if (!sym)
            sym = resolve_symbol_kind(ctx, t->stub.name, DECL_STRUCT);
        if (!sym)
            sym = resolve_symbol_kind(ctx, t->stub.name, DECL_UNION);
        if (!sym)
            sym = resolve_symbol(ctx, t->stub.name);
        if (sym && sym->type) {
            /* If the stub has type args (e.g. box<int32>), instantiate the generic */
            if (t->stub.type_arg_count > 0 && sym->is_generic && sym->type_param_count > 0) {
                /* Resolve each type arg */
                int ntp = sym->type_param_count;
                int nta = t->stub.type_arg_count;
                Type **resolved_args = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)nta);
                for (int i = 0; i < nta; i++) {
                    resolved_args[i] = resolve_type(ctx, t->stub.type_args[i]);
                }

                /* Check if any resolved arg contains type vars */
                bool has_tv = false;
                for (int i = 0; i < nta; i++) {
                    if (type_contains_type_var(resolved_args[i])) {
                        has_tv = true;
                        break;
                    }
                }

                /* Substitute type params with concrete types */
                Type *concrete = type_substitute(ctx->arena, sym->type,
                    sym->type_params, resolved_args,
                    ntp < nta ? ntp : nta);

                /* Ensure we don't mutate the original type */
                if (concrete == sym->type) {
                    concrete = type_copy(ctx->arena, sym->type);
                }

                /* Preserve resolved type_args on the concrete type for unification */
                if (concrete->kind == TYPE_STRUCT) {
                    concrete->struc.type_args = resolved_args;
                    concrete->struc.type_arg_count = nta;
                } else if (concrete->kind == TYPE_UNION) {
                    concrete->unio.type_args = resolved_args;
                    concrete->unio.type_arg_count = nta;
                }

                if (!has_tv) {
                    /* Register mono instance only with concrete types.
                     * Use canonical name from sym->type (already mangled by pass1),
                     * not the stub name which may contain dots. */
                    DeclKind dk = sym->kind;
                    const char *canon_name = (sym->type->kind == TYPE_STRUCT)
                        ? sym->type->struc.name : sym->type->unio.name;
                    const char *mangled = mono_register(ctx->mono_table, ctx->arena,
                        ctx->intern, canon_name, NULL,
                        resolved_args, nta, sym->decl, dk,
                        sym->type_params, ntp);
                    /* Update the concrete type's name to the mangled name */
                    if (concrete->kind == TYPE_STRUCT) {
                        concrete->struc.name = mangled;
                    } else if (concrete->kind == TYPE_UNION) {
                        concrete->unio.name = mangled;
                    }
                    /* Build resolved concrete_type for codegen (separate copy
                     * so stub resolution doesn't strip type_args from the
                     * pass2 type still needed for unification) */
                    MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                    if (mi && !mi->concrete_type) {
                        Type *ct = type_copy(ctx->arena, concrete);
                        mi->concrete_type = ct;
                    }
                }
                return concrete;
            }
            return sym->type;
        }
        /* Stub matched no struct, union, or other symbol — genuinely unknown.
         * A concrete unknown name (no type variables) would otherwise leak
         * verbatim into the generated C and fail the C compile; report a clean
         * FC diagnostic instead. Stubs that still contain type variables are
         * generic templates resolved later at monomorphization, so leave them. */
        if (!type_contains_type_var(t)) {
            diag_error(ctx->type_loc, "unknown type name '%s'", t->stub.name);
            return type_error();
        }
    }
    return t;
}

/* Check that an integer literal value fits in its target type.
 * The value is stored as uint64_t; for signed types we check the
 * bit pattern against the type's range. Negative literals are
 * represented as the two's-complement uint64_t (e.g. -1 → 0xFFFF...).
 * Unsigned negation is caught separately in negation folding.
 * If the parser flagged the literal as out-of-range (strtoull saturated
 * to ULLONG_MAX), report that first since the stored value is meaningless. */
static void check_int_literal_range(uint64_t value, Type *type, SrcLoc loc,
                                    bool out_of_range, bool negative) {
    if (out_of_range) {
        diag_error(loc, "integer literal exceeds 64-bit unsigned range (max 18446744073709551615)");
        return;
    }
    /* A literal written with a leading minus can never fit an unsigned type.
     * `value` holds its two's-complement bit pattern, so report the original
     * negative value rather than the (huge, misleading) bit pattern. */
    if (negative && type_is_unsigned(type)) {
        diag_error(loc, "integer literal -%" PRIu64 " out of range for %s",
                   (uint64_t)(-(int64_t)value), type_name(type));
        return;
    }
    switch (type->kind) {
    case TYPE_INT8:
        if (value > 127 && value < (uint64_t)(int64_t)-128)
            diag_error(loc, "integer literal %" PRId64 " out of range for int8 (-128..127)", (int64_t)value);
        return;
    case TYPE_INT16:
        if (value > 32767 && value < (uint64_t)(int64_t)-32768)
            diag_error(loc, "integer literal %" PRId64 " out of range for int16 (-32768..32767)", (int64_t)value);
        return;
    case TYPE_INT32:
        if (value > 2147483647ULL && value < (uint64_t)(int64_t)-2147483648LL)
            diag_error(loc, "integer literal %" PRId64 " out of range for int32 (-2147483648..2147483647)", (int64_t)value);
        return;
    case TYPE_INT64:
        if (value > 9223372036854775807ULL && value < (uint64_t)INT64_MIN)
            diag_error(loc, "integer literal out of range for int64");
        return;
    case TYPE_UINT8:
        if (value > 255)
            diag_error(loc, "integer literal %" PRIu64 " out of range for uint8 (0..255)", value);
        return;
    case TYPE_UINT16:
        if (value > 65535)
            diag_error(loc, "integer literal %" PRIu64 " out of range for uint16 (0..65535)", value);
        return;
    case TYPE_UINT32:
        if (value > 4294967295ULL)
            diag_error(loc, "integer literal %" PRIu64 " out of range for uint32 (0..4294967295)", value);
        return;
    case TYPE_UINT64:
        return; /* always fits in uint64_t storage */
    default: return;
    }
}

/* Check a float literal for overflow/underflow detected by the parser.
 * Overflow → ±inf in the target type; underflow → 0 from a nonzero source.
 * Subnormals are accepted (C accepts them silently, so do we). */
static void check_float_literal_range(Type *type, bool out_of_range, bool underflow, SrcLoc loc) {
    if (out_of_range) {
        if (type->kind == TYPE_FLOAT32)
            diag_error(loc, "float literal out of range for float32 (max ~3.4e38)");
        else
            diag_error(loc, "float literal out of range for float64 (max ~1.8e308)");
        return;
    }
    if (underflow) {
        if (type->kind == TYPE_FLOAT32)
            diag_error(loc, "float literal underflows to zero in float32");
        else
            diag_error(loc, "float literal underflows to zero in float64");
    }
}

/* Wrap an expression in an implicit widening cast.
 *
 * Provenance must carry through: a widen either produces a fresh scalar
 * (numeric widening, no provenance to speak of) or a pointer/slice that aliases
 * the *same* storage as its operand (const-add, slice const-change). In every
 * case the operand's provenance is the wrapper's provenance, so we copy it.
 * Forgetting this silently launders a stack value's PROV_STACK into the
 * arena-zeroed default (PROV_UNKNOWN) and defeats EVERY escape sink that reads
 * the post-widen value's provenance — return, global store, heap-field store,
 * option payload, struct-literal field. (e.g. assigning a stack `(cstr[N]) s`
 * into a `const cstr` global widens cstr→const cstr and used to leak.) */
static Expr *wrap_widen(Arena *a, Expr *e, Type *target) {
    Expr *cast = arena_alloc(a, sizeof(Expr));
    cast->kind = EXPR_CAST;
    cast->loc = e->loc;
    cast->type = target;
    cast->cast.target = target;
    cast->cast.operand = e;
    cast->prov = e->prov;
    return cast;
}

/* Validate the operand of an atomic builtin: must be a pointer to an integer
 * type or bool (the only types with guaranteed lock-free, tear-free access).
 * Emits a diagnostic and returns false on violation. */
static bool atomic_pointee_ok(Type *pt, SrcLoc loc, const char *op_name) {
    if (pt->kind != TYPE_POINTER) {
        diag_error(loc, "%s requires a pointer operand, got %s", op_name, type_name(pt));
        return false;
    }
    Type *cell = pt->pointer.pointee;
    if (cell->kind == TYPE_TYPE_VAR) {
        diag_error(loc, "%s requires a pointer to a concrete integer or bool type; "
            "type variable %s is not supported", op_name, type_name(cell));
        return false;
    }
    if (!type_is_integer(cell) && cell->kind != TYPE_BOOL) {
        diag_error(loc, "%s requires a pointer to an integer or bool type, got %s",
            op_name, type_name(pt));
        return false;
    }
    return true;
}

static Type *check_match(CheckCtx *ctx, Expr *e);

/* ---- Generic unification ---- */

/* Unify a (possibly generic) parameter type against a concrete argument type.
 * Binds type variables in var_names/bindings. Returns true on success. */
static bool unify(Type *param_type, Type *arg_type,
                  const char **var_names, Type **bindings, int var_count) {
    if (!param_type || !arg_type) return param_type == arg_type;
    if (arg_type->kind == TYPE_ERROR) return true;

    if (param_type->kind == TYPE_TYPE_VAR) {
        /* Find this variable */
        for (int i = 0; i < var_count; i++) {
            if (var_names[i] == param_type->type_var.name) {
                if (bindings[i]) {
                    /* Already bound — check consistency */
                    return type_eq(bindings[i], arg_type);
                }
                bindings[i] = arg_type;
                return true;
            }
        }
        return false; /* unknown type var */
    }

    if (param_type->kind != arg_type->kind) {
        /* Handle kind mismatches between TYPE_STUB, TYPE_STRUCT, and TYPE_UNION.
         * Stubs are kind-agnostic type references; unify their type_args so
         * generic type variables get bound correctly. */
        bool p_is_udt = param_type->kind == TYPE_STRUCT || param_type->kind == TYPE_UNION || param_type->kind == TYPE_STUB;
        bool a_is_udt = arg_type->kind == TYPE_STRUCT || arg_type->kind == TYPE_UNION || arg_type->kind == TYPE_STUB;
        if (p_is_udt && a_is_udt) {
            int pa = (param_type->kind == TYPE_STRUCT) ? param_type->struc.type_arg_count
                   : (param_type->kind == TYPE_UNION) ? param_type->unio.type_arg_count
                   : param_type->stub.type_arg_count;
            int aa = (arg_type->kind == TYPE_STRUCT) ? arg_type->struc.type_arg_count
                   : (arg_type->kind == TYPE_UNION) ? arg_type->unio.type_arg_count
                   : arg_type->stub.type_arg_count;
            Type **pt_args = (param_type->kind == TYPE_STRUCT) ? param_type->struc.type_args
                           : (param_type->kind == TYPE_UNION) ? param_type->unio.type_args
                           : param_type->stub.type_args;
            Type **at_args = (arg_type->kind == TYPE_STRUCT) ? arg_type->struc.type_args
                           : (arg_type->kind == TYPE_UNION) ? arg_type->unio.type_args
                           : arg_type->stub.type_args;
            if (pa > 0 && pa == aa) {
                for (int i = 0; i < pa; i++)
                    if (!unify(pt_args[i], at_args[i], var_names, bindings, var_count))
                        return false;
                return true;
            }
            const char *na = (param_type->kind == TYPE_STRUCT) ? param_type->struc.name
                           : (param_type->kind == TYPE_UNION) ? param_type->unio.name
                           : param_type->stub.name;
            const char *nb = (arg_type->kind == TYPE_STRUCT) ? arg_type->struc.name
                           : (arg_type->kind == TYPE_UNION) ? arg_type->unio.name
                           : arg_type->stub.name;
            return na == nb;
        }
        /* Fixed-array field accepts slice of matching element type */
        if (param_type->kind == TYPE_FIXED_ARRAY && arg_type->kind == TYPE_SLICE)
            return unify(param_type->fixed_array.elem, arg_type->slice.elem,
                         var_names, bindings, var_count);
        return false;
    }

    switch (param_type->kind) {
    case TYPE_POINTER:
        if (param_type->is_const != arg_type->is_const) {
            if (param_type->is_const && !arg_type->is_const) { /* non-const→const ok */ }
            else return false;
        }
        return unify(param_type->pointer.pointee, arg_type->pointer.pointee,
                     var_names, bindings, var_count);
    case TYPE_SLICE:
        if (param_type->is_const != arg_type->is_const) {
            if (param_type->is_const && !arg_type->is_const) { /* non-const→const ok */ }
            else return false;
        }
        return unify(param_type->slice.elem, arg_type->slice.elem,
                     var_names, bindings, var_count);
    case TYPE_OPTION:
        return unify(param_type->option.inner, arg_type->option.inner,
                     var_names, bindings, var_count);
    case TYPE_FIXED_ARRAY:
        if (param_type->fixed_array.size != arg_type->fixed_array.size) return false;
        return unify(param_type->fixed_array.elem, arg_type->fixed_array.elem,
                     var_names, bindings, var_count);
    case TYPE_FUNC:
        if (param_type->func.param_count != arg_type->func.param_count) return false;
        for (int i = 0; i < param_type->func.param_count; i++)
            if (!unify(param_type->func.param_types[i], arg_type->func.param_types[i],
                       var_names, bindings, var_count))
                return false;
        return unify(param_type->func.return_type, arg_type->func.return_type,
                     var_names, bindings, var_count);
    case TYPE_STRUCT:
        /* If both have type_args, unify them (covers same-name and different-name cases) */
        if (param_type->struc.type_arg_count > 0 &&
            arg_type->struc.type_arg_count == param_type->struc.type_arg_count) {
            for (int i = 0; i < param_type->struc.type_arg_count; i++) {
                if (!unify(param_type->struc.type_args[i], arg_type->struc.type_args[i],
                           var_names, bindings, var_count))
                    return false;
            }
            return true;
        }
        if (param_type->struc.name == arg_type->struc.name)
            return true;
        /* If param has type-var fields, try unifying field-by-field */
        if (param_type->struc.field_count > 0 &&
            param_type->struc.field_count == arg_type->struc.field_count) {
            for (int i = 0; i < param_type->struc.field_count; i++) {
                if (!unify(param_type->struc.fields[i].type, arg_type->struc.fields[i].type,
                           var_names, bindings, var_count))
                    return false;
            }
            return true;
        }
        return false;
    case TYPE_UNION:
        /* If both have type_args, unify them */
        if (param_type->unio.type_arg_count > 0 &&
            arg_type->unio.type_arg_count == param_type->unio.type_arg_count) {
            for (int i = 0; i < param_type->unio.type_arg_count; i++) {
                if (!unify(param_type->unio.type_args[i], arg_type->unio.type_args[i],
                           var_names, bindings, var_count))
                    return false;
            }
            return true;
        }
        if (param_type->unio.name == arg_type->unio.name)
            return true;
        /* If param has type-var variants, try unifying variant-by-variant */
        if (param_type->unio.variant_count > 0 &&
            param_type->unio.variant_count == arg_type->unio.variant_count) {
            for (int i = 0; i < param_type->unio.variant_count; i++) {
                if (!unify(param_type->unio.variants[i].payload, arg_type->unio.variants[i].payload,
                           var_names, bindings, var_count))
                    return false;
            }
            return true;
        }
        return false;
    case TYPE_STUB:
        /* Unify type_args if both stubs have them */
        if (param_type->stub.type_arg_count > 0 &&
            arg_type->stub.type_arg_count == param_type->stub.type_arg_count) {
            for (int i = 0; i < param_type->stub.type_arg_count; i++) {
                if (!unify(param_type->stub.type_args[i], arg_type->stub.type_args[i],
                           var_names, bindings, var_count))
                    return false;
            }
            return true;
        }
        return param_type->stub.name == arg_type->stub.name;
    default:
        return type_eq(param_type, arg_type);
    }
}


/* Check if any type binding still contains unresolved type variables */
static bool bindings_contain_type_vars(Type **bindings, int count) {
    for (int i = 0; i < count; i++)
        if (type_contains_type_var(bindings[i])) return true;
    return false;
}

/* Find the Symbol for an EXPR_CALL's callee, looking up global/module symbols */
/* Find the callee symbol for an EXPR_CALL.  Uses resolved_sym / companion_module
 * from EXPR_IDENT — no re-resolution needed.  For qualified calls (mod.func),
 * walks the EXPR_FIELD chain using the stored resolved symbols. */
static Symbol *find_callee_symbol(CheckCtx *ctx, Expr *callee) {
    (void)ctx;
    if (callee->kind == EXPR_IDENT)
        return callee->ident.resolved_sym;
    if (callee->kind == EXPR_FIELD) {
        Expr *obj = callee->field.object;
        Symbol *mod = NULL;
        if (obj->kind == EXPR_IDENT) {
            if (obj->ident.resolved_sym && obj->ident.resolved_sym->kind == DECL_MODULE)
                mod = obj->ident.resolved_sym;
            else if (obj->ident.companion_module)
                mod = obj->ident.companion_module;
        } else if (obj->kind == EXPR_FIELD) {
            /* Multi-level: walk EXPR_FIELD chain from root using resolved_sym */
            Expr *cur = obj;
            while (cur->kind == EXPR_FIELD) cur = cur->field.object;
            if (cur->kind == EXPR_IDENT && cur->ident.resolved_sym) {
                Symbol *root = cur->ident.resolved_sym;
                if (root->kind != DECL_MODULE && cur->ident.companion_module)
                    root = cur->ident.companion_module;
                if (root->kind == DECL_MODULE && root->members) {
                    mod = root;
                    Expr **segs = NULL;
                    int depth = 0, seg_cap = 0;
                    for (Expr *e = obj; e->kind == EXPR_FIELD; e = e->field.object)
                        DA_APPEND(segs, depth, seg_cap, e);
                    for (int k = depth - 1; k >= 0; k--) {
                        Symbol *next = symtab_lookup_kind(mod->members,
                            segs[k]->field.name, DECL_MODULE);
                        if (!next || !next->members) { mod = NULL; break; }
                        mod = next;
                    }
                    free(segs);
                }
            }
        }
        if (mod && mod->members)
            return symtab_lookup(mod->members, callee->field.name);
    }
    return NULL;
}

/* Recursively walk a return type and register/mangle any generic structs/unions.
 * Returns the (possibly modified) type with mangled names. */
static Type *resolve_generic_types_in_ret(CheckCtx *ctx, Type *t) {
    if (!t) return t;
    switch (t->kind) {
    case TYPE_POINTER: {
        Type *inner = resolve_generic_types_in_ret(ctx, t->pointer.pointee);
        if (inner != t->pointer.pointee) {
            Type *r = type_pointer(ctx->arena, inner);
            r->is_const = t->is_const;
            return r;
        }
        return t;
    }
    case TYPE_OPTION: {
        Type *inner = resolve_generic_types_in_ret(ctx, t->option.inner);
        if (inner != t->option.inner) return type_option(ctx->arena, inner);
        return t;
    }
    case TYPE_SLICE: {
        Type *inner = resolve_generic_types_in_ret(ctx, t->slice.elem);
        if (inner != t->slice.elem) {
            Type *r = type_slice(ctx->arena, inner);
            r->is_const = t->is_const;
            return r;
        }
        return t;
    }
    case TYPE_STRUCT:
    case TYPE_UNION: {
        /* A tuple in the return type carries its instantiation in fields, not in a
         * symtab template — re-canonicalize and register it directly. */
        if (t->kind == TYPE_STRUCT && t->struc.is_tuple) {
            int n = t->struc.field_count;
            bool changed = false, generic = false;
            Type **elems = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)(n > 0 ? n : 1));
            for (int i = 0; i < n; i++) {
                elems[i] = resolve_generic_types_in_ret(ctx, t->struc.fields[i].type);
                if (elems[i] != t->struc.fields[i].type) changed = true;
                if (type_contains_type_var(elems[i])) generic = true;
            }
            Type *tup = changed ? type_tuple(ctx->arena, elems, n) : t;
            if (generic) return tup;
            register_concrete_tuple(ctx, tup);
            return tup;
        }
        const char *type_base_name = (t->kind == TYPE_STRUCT) ? t->struc.name : t->unio.name;
        /* Look up the original type symbol */
        Symbol *type_sym = symtab_lookup_kind(ctx->symtab, type_base_name,
            t->kind == TYPE_STRUCT ? DECL_STRUCT : DECL_UNION);
        if (!type_sym) {
            /* Search module member symtabs */
            for (int si = 0; si < ctx->symtab->count && !type_sym; si++) {
                Symbol *s = &ctx->symtab->symbols[si];
                if (s->kind != DECL_MODULE || !s->members) continue;
                for (int mi2 = 0; mi2 < s->members->count; mi2++) {
                    Symbol *ms = &s->members->symbols[mi2];
                    if (!ms->type) continue;
                    if ((ms->type->kind == TYPE_STRUCT && ms->type->struc.name == type_base_name) ||
                        (ms->type->kind == TYPE_UNION && ms->type->unio.name == type_base_name)) {
                        type_sym = ms; break;
                    }
                }
            }
        }
        if (!type_sym || !type_sym->is_generic) return t;

        int tntp = type_sym->type_param_count;
        Type **type_bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)tntp);
        memset(type_bindings, 0, sizeof(Type*) * (size_t)tntp);

        /* Use type_args if available (preferred), otherwise unify fields/variants */
        if (t->kind == TYPE_STRUCT && t->struc.type_arg_count == tntp) {
            for (int i = 0; i < tntp; i++)
                type_bindings[i] = t->struc.type_args[i];
        } else if (t->kind == TYPE_UNION && t->unio.type_arg_count == tntp) {
            for (int i = 0; i < tntp; i++)
                type_bindings[i] = t->unio.type_args[i];
        } else {
            Type *tmpl_type = type_sym->type;
            if (t->kind == TYPE_STRUCT && tmpl_type->kind == TYPE_STRUCT) {
                for (int fi = 0; fi < tmpl_type->struc.field_count &&
                                 fi < t->struc.field_count; fi++) {
                    unify(tmpl_type->struc.fields[fi].type, t->struc.fields[fi].type,
                          type_sym->type_params, type_bindings, tntp);
                }
            } else if (t->kind == TYPE_UNION && tmpl_type->kind == TYPE_UNION) {
                for (int vi = 0; vi < tmpl_type->unio.variant_count &&
                                 vi < t->unio.variant_count; vi++) {
                    if (tmpl_type->unio.variants[vi].payload && t->unio.variants[vi].payload) {
                        unify(tmpl_type->unio.variants[vi].payload, t->unio.variants[vi].payload,
                              type_sym->type_params, type_bindings, tntp);
                    }
                }
            }
        }

        /* Check all bindings resolved */
        for (int i = 0; i < tntp; i++) {
            if (!type_bindings[i]) return t;
        }

        const char *type_mangled = mono_register(ctx->mono_table, ctx->arena,
            ctx->intern, type_base_name, type_sym->ns_prefix,
            type_bindings, tntp, type_sym->decl,
            type_sym->kind, type_sym->type_params, tntp);

        Type *result = type_copy(ctx->arena, t);
        if (result->kind == TYPE_STRUCT) {
            result->struc.name = type_mangled;
            result->struc.type_args = type_bindings;
            result->struc.type_arg_count = tntp;
        } else {
            result->unio.name = type_mangled;
            result->unio.type_args = type_bindings;
            result->unio.type_arg_count = tntp;
        }

        MonoInstance *mi = mono_find(ctx->mono_table, type_mangled);
        if (mi && !mi->concrete_type) {
            Type *ct = type_substitute(ctx->arena, type_sym->type,
                type_sym->type_params, type_bindings, tntp);
            if (ct == type_sym->type) {
                ct = type_copy(ctx->arena, ct);
            }
            if (ct->kind == TYPE_STRUCT) {
                ct->struc.name = type_mangled;
            } else {
                ct->unio.name = type_mangled;
            }
            mi->concrete_type = ct;
        }
        return result;
    }
    default:
        return t;
    }
}

static void check_tuple_destruct(CheckCtx *ctx, Pattern *pat, Type *tup, bool is_mut, SrcLoc loc);

/* Recursively check a struct destructuring pattern, adding bindings to scope */
static void check_destruct_pattern(CheckCtx *ctx, Pattern *pat, Type *struct_type, bool is_mut, SrcLoc loc) {
    if (type_is_error(struct_type)) return;
    if (pat->kind != PAT_STRUCT) {
        diag_error(pat->loc, "expected struct destructuring pattern");
        return;
    }
    if (struct_type->kind != TYPE_STRUCT) {
        diag_error(loc, "cannot destructure non-struct type %s", type_name(struct_type));
        return;
    }
    if (struct_type->struc.is_tuple) {
        diag_error(loc, "use positional destructuring '{ a, b }' for tuple type %s",
            type_name(struct_type));
        return;
    }
    if (struct_type->struc.is_c_union) {
        diag_error(loc, "cannot destructure extern union type '%s'", type_name(struct_type));
        return;
    }

    for (int i = 0; i < pat->struc.field_count; i++) {
        const char *fname = pat->struc.fields[i].name;
        Pattern *inner = pat->struc.fields[i].pattern;

        /* Find the field type */
        Type *field_type = NULL;
        for (int j = 0; j < struct_type->struc.field_count; j++) {
            if (struct_type->struc.fields[j].name == fname) {
                field_type = struct_type->struc.fields[j].type;
                break;
            }
        }
        if (!field_type) {
            diag_error(loc, "struct '%s' has no field '%s'", type_name(struct_type), fname);
            continue;
        }

        field_type = resolve_type(ctx, field_type);
        pat->struc.fields[i].resolved_type = field_type;

        if (inner->kind == PAT_BINDING) {
            const char *orig_name = inner->binding.name;
            int id = local_id_counter++;
            const char *cg = make_local_name(ctx->arena, "_l_", orig_name, id);
            inner->binding.name = cg;  /* overwrite with codegen name */
            scope_add(ctx->scope, orig_name, cg, field_type, is_mut);
        } else if (inner->kind == PAT_WILDCARD) {
            /* skip this field */
        } else if (inner->kind == PAT_STRUCT) {
            check_destruct_pattern(ctx, inner, field_type, is_mut, loc);
        } else if (inner->kind == PAT_TUPLE) {
            check_tuple_destruct(ctx, inner, field_type, is_mut, loc);
        } else if (inner->kind == PAT_OR) {
            diag_error(inner->loc, "or-patterns are not allowed in let destructuring");
            return;
        } else {
            diag_error(inner->loc, "unsupported pattern in let destructuring");
            return;
        }
    }
}

/* Recursively check a positional tuple destructuring pattern, binding each
 * element in order. Mirrors check_destruct_pattern but matches by position
 * against the tuple's element types rather than by field name. */
static void check_tuple_destruct(CheckCtx *ctx, Pattern *pat, Type *tup, bool is_mut, SrcLoc loc) {
    if (type_is_error(tup)) return;
    if (pat->kind != PAT_TUPLE) {
        diag_error(pat->loc, "expected tuple destructuring pattern");
        return;
    }
    if (tup->kind != TYPE_STRUCT || !tup->struc.is_tuple) {
        diag_error(loc, "positional destructuring '{ a, b }' requires a tuple, got %s",
            type_name(tup));
        return;
    }
    if (pat->tuple_pat.pattern_count != tup->struc.field_count) {
        diag_error(loc, "tuple %s has %d elements but the pattern binds %d",
            type_name(tup), tup->struc.field_count, pat->tuple_pat.pattern_count);
        return;
    }
    pat->tuple_pat.resolved_types = arena_alloc(ctx->arena,
        sizeof(Type*) * (size_t)(pat->tuple_pat.pattern_count > 0 ? pat->tuple_pat.pattern_count : 1));

    for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
        Type *elem_type = resolve_type(ctx, tup->struc.fields[i].type);
        pat->tuple_pat.resolved_types[i] = elem_type;
        Pattern *inner = pat->tuple_pat.patterns[i];

        if (inner->kind == PAT_BINDING) {
            const char *orig_name = inner->binding.name;
            int id = local_id_counter++;
            const char *cg = make_local_name(ctx->arena, "_l_", orig_name, id);
            inner->binding.name = cg;  /* overwrite with codegen name */
            scope_add(ctx->scope, orig_name, cg, elem_type, is_mut);
        } else if (inner->kind == PAT_WILDCARD) {
            /* skip this element */
        } else if (inner->kind == PAT_STRUCT) {
            check_destruct_pattern(ctx, inner, elem_type, is_mut, loc);
        } else if (inner->kind == PAT_TUPLE) {
            check_tuple_destruct(ctx, inner, elem_type, is_mut, loc);
        } else if (inner->kind == PAT_OR) {
            diag_error(inner->loc, "or-patterns are not allowed in let destructuring");
            return;
        } else {
            diag_error(inner->loc, "unsupported pattern in let destructuring");
            return;
        }
    }
}

/* Bind every name in a for-loop element pattern with the error type, so the
 * loop body still type-checks (avoiding cascading "undefined name" errors)
 * when the element type couldn't be determined. */
static void for_pattern_bind_error(CheckCtx *ctx, Pattern *pat) {
    switch (pat->kind) {
    case PAT_BINDING:
        scope_add(ctx->scope, pat->binding.name, pat->binding.name, type_error(), false);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            for_pattern_bind_error(ctx, pat->tuple_pat.patterns[i]);
        break;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            for_pattern_bind_error(ctx, pat->struc.fields[i].pattern);
        break;
    default:
        break;
    }
}

/* Bind a for-loop's element to the loop scope. With a destructure pattern,
 * generate the element temp name and check the pattern against elem_type
 * (reusing the let-destructuring checkers); otherwise bind the plain name.
 * For-loop element bindings are always immutable (a fresh copy per iteration). */
static void bind_for_element(CheckCtx *ctx, Expr *e, Type *elem_type) {
    if (e->for_expr.var_pattern) {
        int tmp_id = local_id_counter++;
        int n = snprintf(NULL, 0, "_fe_%d", tmp_id) + 1;
        char *tmp = arena_alloc(ctx->arena, (size_t)n);
        snprintf(tmp, (size_t)n, "_fe_%d", tmp_id);
        e->for_expr.elem_tmp = tmp;
        if (e->for_expr.var_pattern->kind == PAT_TUPLE)
            check_tuple_destruct(ctx, e->for_expr.var_pattern, elem_type, false, e->loc);
        else
            check_destruct_pattern(ctx, e->for_expr.var_pattern, elem_type, false, e->loc);
    } else {
        scope_add(ctx->scope, e->for_expr.var,
            c_safe_ident(ctx->intern, e->for_expr.var), elem_type, false);
    }
}

/* Returns the result type if type_name.prop is a valid static type property,
 * NULL if type_name is not a primitive type name (fall through to normal resolution).
 * Returns (Type*)-1 sentinel if it IS a type name but the property is invalid.
 * Sets *codegen_out to the C constant string to emit on success. */
static Type *resolve_type_property(const char *type_name, const char *prop,
                                   const char **codegen_out) {
    int len = (int)strlen(type_name);
    Type *t = type_from_name(type_name, len);
    if (!t) return NULL;  /* not a type name at all — fall through */

    TypeKind kind = t->kind;
    bool is_int = type_is_integer(t);
    bool is_float = type_is_float(t);

    /* Only integer and float types have static properties */
    if (!is_int && !is_float)
        return (Type *)-1;  /* e.g. bool.min — type name but no properties */

    /* .bits — always int32 */
    if (strcmp(prop, "bits") == 0) {
        switch (kind) {
            case TYPE_INT8:  case TYPE_UINT8:  *codegen_out = "8";  break;
            case TYPE_INT16: case TYPE_UINT16: *codegen_out = "16"; break;
            case TYPE_INT32: case TYPE_UINT32: case TYPE_FLOAT32: *codegen_out = "32"; break;
            case TYPE_INT64: case TYPE_UINT64: case TYPE_FLOAT64: *codegen_out = "64"; break;
            case TYPE_ISIZE: case TYPE_USIZE: *codegen_out = "((int32_t)(sizeof(ptrdiff_t)*8))"; break;
            default: return (Type *)-1;
        }
        return type_int32();
    }

    /* Integer properties */
    if (is_int) {
        if (strcmp(prop, "min") == 0) {
            switch (kind) {
                case TYPE_INT8:   *codegen_out = "INT8_MIN";       break;
                case TYPE_INT16:  *codegen_out = "INT16_MIN";      break;
                case TYPE_INT32:  *codegen_out = "INT32_MIN";      break;
                case TYPE_INT64:  *codegen_out = "INT64_MIN";      break;
                case TYPE_UINT8:  *codegen_out = "((uint8_t)0)";   break;
                case TYPE_UINT16: *codegen_out = "((uint16_t)0)";  break;
                case TYPE_UINT32: *codegen_out = "((uint32_t)0)";  break;
                case TYPE_UINT64: *codegen_out = "((uint64_t)0)";  break;
                case TYPE_ISIZE:  *codegen_out = "PTRDIFF_MIN";      break;
                case TYPE_USIZE:  *codegen_out = "((size_t)0)";     break;
                default: return (Type *)-1;
            }
            return t;
        }
        if (strcmp(prop, "max") == 0) {
            switch (kind) {
                case TYPE_INT8:   *codegen_out = "INT8_MAX";   break;
                case TYPE_INT16:  *codegen_out = "INT16_MAX";  break;
                case TYPE_INT32:  *codegen_out = "INT32_MAX";  break;
                case TYPE_INT64:  *codegen_out = "INT64_MAX";  break;
                case TYPE_UINT8:  *codegen_out = "UINT8_MAX";  break;
                case TYPE_UINT16: *codegen_out = "UINT16_MAX"; break;
                case TYPE_UINT32: *codegen_out = "UINT32_MAX"; break;
                case TYPE_UINT64: *codegen_out = "UINT64_MAX"; break;
                case TYPE_ISIZE:  *codegen_out = "PTRDIFF_MAX"; break;
                case TYPE_USIZE:  *codegen_out = "SIZE_MAX";    break;
                default: return (Type *)-1;
            }
            return t;
        }
        return (Type *)-1;  /* valid type, invalid property */
    }

    /* Float properties */
    bool is_f32 = (kind == TYPE_FLOAT32);
    if (strcmp(prop, "min") == 0) {
        *codegen_out = is_f32 ? "FLT_MIN" : "DBL_MIN";
        return t;
    }
    if (strcmp(prop, "max") == 0) {
        *codegen_out = is_f32 ? "FLT_MAX" : "DBL_MAX";
        return t;
    }
    if (strcmp(prop, "epsilon") == 0) {
        *codegen_out = is_f32 ? "FLT_EPSILON" : "DBL_EPSILON";
        return t;
    }
    if (strcmp(prop, "nan") == 0) {
        *codegen_out = is_f32 ? "((float)NAN)" : "((double)NAN)";
        return t;
    }
    if (strcmp(prop, "inf") == 0) {
        *codegen_out = is_f32 ? "((float)INFINITY)" : "((double)INFINITY)";
        return t;
    }
    if (strcmp(prop, "neg_inf") == 0) {
        *codegen_out = is_f32 ? "((float)(-INFINITY))" : "((double)(-INFINITY))";
        return t;
    }
    return (Type *)-1;  /* valid type, invalid property */
}

static bool is_write_through_const(Expr *target) {
    if (!target) return false;
    switch (target->kind) {
    case EXPR_UNARY_PREFIX:
        if (target->unary_prefix.op == TOK_STAR)
            return target->unary_prefix.operand->type &&
                   target->unary_prefix.operand->type->is_const;
        return false;
    case EXPR_DEREF_FIELD:
        return (target->field.object->type &&
                target->field.object->type->is_const) ||
               is_write_through_const(target->field.object);
    case EXPR_FIELD:
        return is_write_through_const(target->field.object);
    case EXPR_INDEX:
        return (target->index.object->type &&
                target->index.object->type->is_const) ||
               is_write_through_const(target->index.object);
    default:
        return false;
    }
}

/* Provenance of the storage that a write to this lvalue lands in. Walks the
 * lvalue path (field / index / deref / deref-field) down to the root so the
 * escape check can ask one question of any target shape — "does this store
 * reach memory that outlives the stack frame?" — instead of pattern-matching a
 * single shape. Reads the propagated `prov` already computed on each sub-expr:
 *   *p          -> where p points       (p's prov)
 *   p->field    -> the struct p points at (p's prov)
 *   obj.field   -> obj's own storage     (recurse)
 *   obj[i]      -> the buffer obj views  (obj's prov: slice/pointer pointee)
 *   ident       -> stack for a local, static for a global
 * A loaded-through-unknown pointer (e.g. **pp, or *param) yields PROV_UNKNOWN,
 * which the caller treats leniently — the same out-param latitude the analysis
 * already grants writes through unknown-provenance pointers. */
static Provenance assign_dest_prov(Expr *target) {
    if (!target) return PROV_UNKNOWN;
    switch (target->kind) {
    case EXPR_IDENT:
        return target->ident.is_local ? PROV_STACK : PROV_STATIC;
    case EXPR_UNARY_PREFIX:
        if (target->unary_prefix.op == TOK_STAR)
            return target->unary_prefix.operand->prov;
        return PROV_UNKNOWN;
    case EXPR_DEREF_FIELD:
        return target->field.object->prov;
    case EXPR_FIELD:
        return assign_dest_prov(target->field.object);
    case EXPR_INDEX:
        return target->index.object->prov;
    default:
        return PROV_UNKNOWN;
    }
}

/* ---- Generic body validation after type variable resolution ---- */

/* Format "func_name(param_type1, param_type2)" for generic validation error messages.
 * Shows concrete parameter types (after substitution), not type variable bindings. */
static const char *fmt_generic_inst(const char *func_name, Arena *arena,
    Type *func_type, const char **type_params, Type **bindings, int ntp)
{
    static char buf[512];
    const int cap = (int)sizeof(buf);
    int pos = 0;
    /* Clamp pos to [0, cap] after every append so the remaining-size argument
     * never underflows when a deeply nested instantiation's name overflows the
     * buffer (the descriptor just truncates — it is depth-keyed for dedup). */
    #define FGI_APPEND(...) do { \
        if (pos < cap) { \
            int _n = snprintf(buf + pos, (size_t)(cap - pos), __VA_ARGS__); \
            if (_n > 0) pos += _n; \
            if (pos > cap) pos = cap; \
        } \
    } while (0)
    FGI_APPEND("%s(", func_name);
    if (func_type && func_type->kind == TYPE_FUNC) {
        for (int i = 0; i < func_type->func.param_count; i++) {
            if (i > 0) FGI_APPEND(", ");
            Type *pt = type_substitute(arena, func_type->func.param_types[i],
                type_params, bindings, ntp);
            FGI_APPEND("%s", type_name(pt));
        }
    }
    FGI_APPEND(")");
    #undef FGI_APPEND
    return buf;
}

/* Bounds transitive (cross-function-body) generic validation so a pathological
 * type-growing recursion can't loop forever. This is the absolute recursion net;
 * GEN_INST_DEPTH_MAX (the depth backstop below) and the instantiation memo halt
 * real divergence well before it. Set above GEN_INST_DEPTH_MAX so the backstop
 * fires (with a precise diagnostic) before this silent cutoff. */
#define GEN_XBODY_DEPTH_MAX 192
static int g_gen_xbody_depth = 0;

/* Depth backstop for infinite generic instantiation (audit item 14). A divergent
 * generic function instantiates itself with an ever-deeper type argument —
 * f(wrap{v=x}) forces f<wrap<'a>> -> f<wrap<wrap<'a>>> -> ..., naming an infinite
 * family of monomorphized copies. Unlike option-wrapped divergence (caught later
 * by monomorph.c's cap), a user struct/union wrapper produces a recursive call
 * inside a *function body*; monomorphization either never registers it (the
 * inferred call carries no explicit type args) or truncates it on a mangling
 * collision, emitting dangling C. We catch it here, at the cross-body descent
 * that already walks these growing instantiations, the moment a binding's
 * structural depth exceeds anything a finite program reaches. Reuses the same
 * "infinite generic instantiation" wording as the monomorph.c guards. */
#define GEN_INST_DEPTH_MAX 64
static bool g_gen_inf_reported = false;

/* Structural nesting depth of a concrete instantiation's type argument — the axis
 * a divergent generic grows along. Mirrors monomorph.c's mono_type_arg_depth:
 * recurses through wrapper constructors and into generic type arguments, never
 * into struct/union *fields* (bounded by the definition; only args grow), so it
 * stays finite on by-value-recursive concrete types. */
static int gen_inst_type_depth(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_POINTER:     return 1 + gen_inst_type_depth(t->pointer.pointee);
    case TYPE_SLICE:       return 1 + gen_inst_type_depth(t->slice.elem);
    case TYPE_OPTION:      return 1 + gen_inst_type_depth(t->option.inner);
    case TYPE_FIXED_ARRAY: return 1 + gen_inst_type_depth(t->fixed_array.elem);
    case TYPE_FUNC: {
        int m = gen_inst_type_depth(t->func.return_type);
        for (int i = 0; i < t->func.param_count; i++) {
            int d = gen_inst_type_depth(t->func.param_types[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    case TYPE_STRUCT: {
        int m = 0;
        for (int i = 0; i < t->struc.type_arg_count; i++) {
            int d = gen_inst_type_depth(t->struc.type_args[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    case TYPE_UNION: {
        int m = 0;
        for (int i = 0; i < t->unio.type_arg_count; i++) {
            int d = gen_inst_type_depth(t->unio.type_args[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    case TYPE_STUB: {
        int m = 0;
        for (int i = 0; i < t->stub.type_arg_count; i++) {
            int d = gen_inst_type_depth(t->stub.type_args[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    default: return 1;
    }
}

/* Memo of generic instantiations already validated in the current top-level
 * pass. A self- or mutually-recursive generic (or a diamond of generic calls)
 * resolves to a finite set of distinct instantiations; validating each once
 * bounds the total work. Without it, a body with two self-calls re-descends
 * 2^depth times and exhausts memory (a single such test OOM-kills the process).
 * Keyed on (callee symbol, concrete signature) so same-named generics in
 * different modules never alias. fmt_generic_inst() returns a shared static
 * buffer, so descriptors are stored as strcmp-comparable arena copies. The memo
 * is reset at each independent top-level entry. */
typedef struct { const Symbol *sym; const char *desc; } GenSeen;
static GenSeen *g_gen_seen = NULL;
static int g_gen_seen_n = 0, g_gen_seen_cap = 0;

static void gen_seen_reset(void) { g_gen_seen_n = 0; g_gen_inf_reported = false; }

/* Record (sym, desc) as validated. Returns true if newly added (caller should
 * descend), false if this instantiation was already validated this pass. */
static bool gen_seen_add(Arena *arena, const Symbol *sym, const char *desc) {
    for (int i = 0; i < g_gen_seen_n; i++)
        if (g_gen_seen[i].sym == sym && strcmp(g_gen_seen[i].desc, desc) == 0)
            return false;
    GenSeen ent = { sym, arena_strdup(arena, desc, (int) strlen(desc)) };
    DA_APPEND(g_gen_seen, g_gen_seen_n, g_gen_seen_cap, ent);
    return true;
}

/* One frame of a generic-instantiation chain: a concrete instantiation and the
 * call site that required it. Linked outward via `parent` (NULL at the entry
 * call). Frames are stack-allocated during the validate_generic_body recursion,
 * so their lifetime is exactly the descent that built them. */
typedef struct InstFrame {
    const char *desc;               /* "inner(int32)" — fmt_generic_inst output */
    SrcLoc site;                    /* the call expression that triggered this instantiation */
    const struct InstFrame *parent; /* enclosing instantiation, NULL at the entry */
} InstFrame;

/* The entry (outermost) frame — its site is the user's actionable call. */
static const InstFrame *inst_frame_root(const InstFrame *f) {
    while (f->parent) f = f->parent;
    return f;
}

/* Emit a diagnostic for an error found while validating a generic instantiation.
 * The head line keeps the long-standing one-line form
 *   "in <innermost> at <err>: <message>"
 * (so single-level diagnostics are byte-identical to before); when the failing
 * instantiation was reached transitively, every enclosing instantiation and the
 * site that required it is listed on its own continuation line — so the full
 * chain, previously collapsed to just the innermost frame, is visible. The
 * primary location is the outermost (entry) call site, the user's actionable code. */
static void gen_inst_diag(const InstFrame *frame, SrcLoc err_loc, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SrcLoc primary = inst_frame_root(frame)->site;

    if (!frame->parent) {
        /* Single-level: identical to the original format. */
        diag_error(primary, "in %s at %d:%d: %s",
                   frame->desc, err_loc.line, err_loc.col, msg);
        return;
    }

    /* Multi-level: head line + one continuation per frame (innermost first). */
    char buf[4096];
    int pos = 0;
    const int cap = (int) sizeof(buf);
    #define GID_APPEND(...) do { \
        if (pos < cap) { \
            int _n = snprintf(buf + pos, (size_t)(cap - pos), __VA_ARGS__); \
            if (_n > 0) pos += _n; \
            if (pos > cap) pos = cap; \
        } \
    } while (0)
    GID_APPEND("in %s at %d:%d: %s", frame->desc, err_loc.line, err_loc.col, msg);
    for (const InstFrame *f = frame; f; f = f->parent) {
        const char *fn = f->site.filename ? f->site.filename : diag_filename();
        GID_APPEND("\n    %s instantiated at %s:%d:%d",
                   f->desc, fn, f->site.line, f->site.col);
    }
    #undef GID_APPEND
    diag_error(primary, "%s", buf);
}

/* Walk a generic function body with concrete type bindings and validate
 * operations that were deferred during template type-checking (i.e. binary
 * operations on type variables and type property access).
 * Returns true if validation passed. */
static bool validate_generic_body(Expr *e, Arena *arena,
    const char **type_params, Type **bindings, int ntp,
    const InstFrame *frame)
{
    if (!e) return true;
    bool ok = true;

    switch (e->kind) {
    case EXPR_BINARY: {
        /* Recurse into children first */
        ok &= validate_generic_body(e->binary.left, arena, type_params, bindings, ntp, frame);
        ok &= validate_generic_body(e->binary.right, arena, type_params, bindings, ntp, frame);

        Type *lt_raw = e->binary.left->type;
        Type *rt_raw = e->binary.right->type;
        if (!lt_raw || !rt_raw) break;

        /* Only check operations that were deferred (involve type vars) */
        if (!type_contains_type_var(lt_raw) && !type_contains_type_var(rt_raw)) break;

        Type *lt = type_substitute(arena, lt_raw, type_params, bindings, ntp);
        Type *rt = type_substitute(arena, rt_raw, type_params, bindings, ntp);
        TokenKind op = e->binary.op;

        if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR ||
            op == TOK_SLASH || op == TOK_PERCENT) {
            if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
                gen_inst_diag(frame, e->loc, "arithmetic requires numeric operands, got %s and %s",
                    type_name(lt), type_name(rt));
                ok = false;
            } else if (!type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                gen_inst_diag(frame, e->loc, "type mismatch: %s vs %s",
                    type_name(lt), type_name(rt));
                ok = false;
            }
        } else if (op == TOK_EQEQ || op == TOK_BANGEQ) {
            if (!type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                gen_inst_diag(frame, e->loc, "comparison type mismatch: %s vs %s",
                    type_name(lt), type_name(rt));
                ok = false;
            }
        } else if (op == TOK_LT || op == TOK_GT || op == TOK_LTEQ || op == TOK_GTEQ) {
            if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
                gen_inst_diag(frame, e->loc, "ordering comparison requires numeric or pointer types, got %s and %s",
                    type_name(lt), type_name(rt));
                ok = false;
            } else if (!type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                gen_inst_diag(frame, e->loc, "comparison type mismatch: %s vs %s",
                    type_name(lt), type_name(rt));
                ok = false;
            }
        } else if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
            if (!type_eq(lt, type_bool()) || !type_eq(rt, type_bool())) {
                gen_inst_diag(frame, e->loc, "logical operator requires bool operands");
                ok = false;
            }
        } else if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET ||
                   op == TOK_LTLT || op == TOK_GTGT) {
            if (!type_is_integer(lt) || !type_is_integer(rt)) {
                gen_inst_diag(frame, e->loc, "bitwise/shift operator requires integer operands");
                ok = false;
            } else if (op != TOK_LTLT && op != TOK_GTGT &&
                       !type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                gen_inst_diag(frame, e->loc, "type mismatch: %s vs %s",
                    type_name(lt), type_name(rt));
                ok = false;
            }
        }
        break;
    }

    /* Type variable property access: 'a.nan, 'a.min, etc. */
    case EXPR_FIELD: case EXPR_DEREF_FIELD: {
        ok &= validate_generic_body(e->field.object, arena, type_params, bindings, ntp, frame);
        if (e->field.object->kind == EXPR_TYPE_VAR_REF) {
            const char *tv_name = e->field.object->type_var_ref.name;
            const char *prop = e->field.name;
            /* Resolve the type variable to its concrete type */
            Type *concrete = NULL;
            for (int i = 0; i < ntp; i++) {
                if (type_params[i] == tv_name || strcmp(type_params[i], tv_name) == 0) {
                    concrete = bindings[i];
                    break;
                }
            }
            if (concrete) {
                bool is_int = type_is_integer(concrete);
                bool is_float = type_is_float(concrete);
                bool valid = false;
                if (strcmp(prop, "bits") == 0) {
                    valid = is_int || is_float;
                } else if (strcmp(prop, "min") == 0 || strcmp(prop, "max") == 0) {
                    valid = is_int || is_float;
                } else if (strcmp(prop, "nan") == 0 || strcmp(prop, "inf") == 0 ||
                           strcmp(prop, "neg_inf") == 0 || strcmp(prop, "epsilon") == 0) {
                    valid = is_float;
                }
                if (!valid) {
                    gen_inst_diag(frame, e->loc, "type '%s' has no property '%s'",
                        type_name(concrete), prop);
                    ok = false;
                }
            }
        }
        break;
    }

    /* Recurse into all sub-expressions */
    case EXPR_UNARY_PREFIX: {
        ok &= validate_generic_body(e->unary_prefix.operand, arena, type_params, bindings, ntp, frame);
        /* Validate deferred unary ops on type variables */
        Type *ot_raw = e->unary_prefix.operand->type;
        if (ot_raw && type_contains_type_var(ot_raw)) {
            Type *ot = type_substitute(arena, ot_raw, type_params, bindings, ntp);
            if (e->unary_prefix.op == TOK_MINUS) {
                /* Same rule as the concrete path: signed/float only, never unsigned. */
                if (!type_is_signed(ot) && !type_is_float(ot)) {
                    gen_inst_diag(frame, e->loc, "unary minus requires a signed integer or float operand, got %s",
                        type_name(ot));
                    ok = false;
                }
            } else if (e->unary_prefix.op == TOK_TILDE) {
                if (!type_is_integer(ot)) {
                    gen_inst_diag(frame, e->loc, "bitwise not requires integer operand, got %s",
                        type_name(ot));
                    ok = false;
                }
            }
        }
        break;
    }
    case EXPR_UNARY_POSTFIX:
        ok &= validate_generic_body(e->unary_postfix.operand, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_CALL: {
        ok &= validate_generic_body(e->call.func, arena, type_params, bindings, ntp, frame);
        for (int i = 0; i < e->call.arg_count; i++)
            ok &= validate_generic_body(e->call.args[i], arena, type_params, bindings, ntp, frame);
        /* Transitive validation: if this call targets another generic function,
         * propagate the concrete bindings into the callee's body so that an
         * unsupported operation on the instantiated type is reported here at the
         * originating call site — not leaked to the C compiler. (Without this,
         * a generic that only fails for some types would type-check at its own
         * definition but emit invalid C when reached through a wrapper.) */
        Symbol *callee = e->call.resolved_callee;
        Type *cft = e->call.func ? e->call.func->type : NULL;
        if (callee && callee->is_generic && callee->type_param_count > 0 &&
            callee->decl && callee->decl->kind == DECL_LET && callee->decl->let.init &&
            callee->decl->let.init->kind == EXPR_FUNC &&
            cft && cft->kind == TYPE_FUNC && cft->func.param_count == e->call.arg_count &&
            g_gen_xbody_depth < GEN_XBODY_DEPTH_MAX) {
            int cntp = callee->type_param_count;
            Type **cbind = arena_alloc(arena, sizeof(Type*) * (size_t) cntp);
            memset(cbind, 0, sizeof(Type*) * (size_t) cntp);
            bool unified = true;
            for (int i = 0; i < e->call.arg_count && unified; i++) {
                Type *araw = e->call.args[i]->type;
                if (!araw) { unified = false; break; }
                /* Resolve the argument's type under the CURRENT instantiation. */
                Type *aconc = type_substitute(arena, araw, type_params, bindings, ntp);
                if (type_contains_type_var(aconc) ||
                    !unify(cft->func.param_types[i], aconc, callee->type_params, cbind, cntp))
                    unified = false;
            }
            if (unified) {
                for (int i = 0; i < cntp; i++)
                    if (!cbind[i] || type_contains_type_var(cbind[i])) { unified = false; break; }
            }
            /* Depth backstop: a binding nesting deeper than any finite program
             * would means this generic instantiates itself with an ever-growing
             * type argument — an infinite monomorphized family. Report once (the
             * pass2 error gate then stops compilation before codegen emits the
             * dangling/truncated C such a family produces) and don't descend. */
            if (unified) {
                int maxd = 0;
                for (int i = 0; i < cntp; i++) {
                    int d = gen_inst_type_depth(cbind[i]);
                    if (d > maxd) maxd = d;
                }
                if (maxd > GEN_INST_DEPTH_MAX) {
                    if (!g_gen_inf_reported) {
                        g_gen_inf_reported = true;
                        diag_error(inst_frame_root(frame)->site,
                            "infinite generic instantiation of '%s': it is instantiated "
                            "with an unbounded family of ever-deeper type arguments "
                            "(exceeded depth %d). A generic function that calls itself "
                            "with a growing type argument (e.g. f(wrap{ v = x }), where "
                            "each call wraps the argument in another generic layer) "
                            "requires infinitely many monomorphized copies.",
                            callee->name, GEN_INST_DEPTH_MAX);
                    }
                    ok = false;
                } else {
                    /* fmt_generic_inst returns a shared static buffer; copy into the
                     * arena so the descriptor stays valid across the recursive
                     * descent (which calls fmt_generic_inst again). cdesc is the
                     * human-readable substitution context threaded into diagnostics.
                     * Validate each distinct instantiation once — recursive or diamond
                     * generic calls otherwise re-descend exponentially. */
                    const char *tmp = fmt_generic_inst(callee->name, arena, cft,
                        callee->type_params, cbind, cntp);
                    const char *cdesc = arena_strdup(arena, tmp, (int) strlen(tmp));
                    /* Memo key is depth-prefixed (kept separate from the display
                     * descriptor): fmt_generic_inst truncates in its fixed buffer, so
                     * two different-depth instantiations of a long-named generic can
                     * format identically; without the depth in the key such a collision
                     * would dedup them and silently halt a divergent descent before the
                     * backstop above fires. */
                    char keybuf[512];
                    int klen = snprintf(keybuf, sizeof(keybuf), "%d:%s", maxd, tmp);
                    if (klen < 0) klen = 0;
                    if (klen >= (int)sizeof(keybuf)) klen = (int)sizeof(keybuf) - 1;
                    const char *ckey = arena_strdup(arena, keybuf, klen);
                    if (gen_seen_add(arena, callee, ckey)) {
                        Expr *cfn = callee->decl->let.init;
                        /* Push a chain frame: this callee instantiation, required
                         * by the call expression `e`. Stack-allocated — its
                         * lifetime is exactly this descent. */
                        InstFrame child = { cdesc, e->loc, frame };
                        g_gen_xbody_depth++;
                        for (int i = 0; i < cfn->func.body_count; i++)
                            ok &= validate_generic_body(cfn->func.body[i], arena,
                                callee->type_params, cbind, cntp, &child);
                        g_gen_xbody_depth--;
                    }
                }
            }
        }
        break;
    }
    case EXPR_INDEX:
        ok &= validate_generic_body(e->index.object, arena, type_params, bindings, ntp, frame);
        ok &= validate_generic_body(e->index.index, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_SLICE:
        ok &= validate_generic_body(e->slice.object, arena, type_params, bindings, ntp, frame);
        if (e->slice.lo) ok &= validate_generic_body(e->slice.lo, arena, type_params, bindings, ntp, frame);
        if (e->slice.hi) ok &= validate_generic_body(e->slice.hi, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_CAST:
        ok &= validate_generic_body(e->cast.operand, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_IF:
        ok &= validate_generic_body(e->if_expr.cond, arena, type_params, bindings, ntp, frame);
        ok &= validate_generic_body(e->if_expr.then_body, arena, type_params, bindings, ntp, frame);
        if (e->if_expr.else_body)
            ok &= validate_generic_body(e->if_expr.else_body, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            ok &= validate_generic_body(e->loop_expr.body[i], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_FOR:
        ok &= validate_generic_body(e->for_expr.iter, arena, type_params, bindings, ntp, frame);
        if (e->for_expr.range_end)
            ok &= validate_generic_body(e->for_expr.range_end, arena, type_params, bindings, ntp, frame);
        for (int i = 0; i < e->for_expr.body_count; i++)
            ok &= validate_generic_body(e->for_expr.body[i], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value)
            ok &= validate_generic_body(e->break_expr.value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value)
            ok &= validate_generic_body(e->return_expr.value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            ok &= validate_generic_body(e->block.stmts[i], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            ok &= validate_generic_body(e->func.body[i], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.init_expr)
            ok &= validate_generic_body(e->alloc_expr.init_expr, arena, type_params, bindings, ntp, frame);
        if (e->alloc_expr.size_expr)
            ok &= validate_generic_body(e->alloc_expr.size_expr, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_FREE:
        ok &= validate_generic_body(e->free_expr.operand, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_ATOMIC_LOAD:
        ok &= validate_generic_body(e->atomic_load.ptr, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_ATOMIC_STORE:
        ok &= validate_generic_body(e->atomic_store.ptr, arena, type_params, bindings, ntp, frame);
        ok &= validate_generic_body(e->atomic_store.value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_ASSERT:
        ok &= validate_generic_body(e->assert_expr.condition, arena, type_params, bindings, ntp, frame);
        if (e->assert_expr.message)
            ok &= validate_generic_body(e->assert_expr.message, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_DEFER:
        ok &= validate_generic_body(e->defer_expr.value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_SOME:
        ok &= validate_generic_body(e->some_expr.value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_ASSIGN:
        ok &= validate_generic_body(e->assign.target, arena, type_params, bindings, ntp, frame);
        ok &= validate_generic_body(e->assign.value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            ok &= validate_generic_body(e->struct_lit.fields[i].value, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            ok &= validate_generic_body(e->array_lit.elems[i], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            ok &= validate_generic_body(e->tuple_lit.elems[i], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_SLICE_LIT:
        ok &= validate_generic_body(e->slice_lit.ptr_expr, arena, type_params, bindings, ntp, frame);
        ok &= validate_generic_body(e->slice_lit.len_expr, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++)
            if (!e->interp_string.segments[i].is_literal && e->interp_string.segments[i].expr)
                ok &= validate_generic_body(e->interp_string.segments[i].expr, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_MATCH:
        ok &= validate_generic_body(e->match_expr.subject, arena, type_params, bindings, ntp, frame);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                ok &= validate_generic_body(e->match_expr.arms[i].body[j], arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_LET:
        if (e->let_expr.let_init)
            ok &= validate_generic_body(e->let_expr.let_init, arena, type_params, bindings, ntp, frame);
        break;
    case EXPR_LET_DESTRUCT:
        ok &= validate_generic_body(e->let_destruct.init, arena, type_params, bindings, ntp, frame);
        break;

    /* Leaf nodes — no children to walk */
    default:
        break;
    }
    return ok;
}

/* Check if an expression is an lvalue (has addressable storage that outlives the expression).
 * Used to prevent fixed-array field access on temporaries. */
static bool is_lvalue_expr(Expr *e) {
    switch (e->kind) {
    case EXPR_IDENT:       return true;   /* named binding */
    case EXPR_FIELD:       return is_lvalue_expr(e->field.object);   /* chain: s.inner.data */
    case EXPR_DEREF_FIELD: return true;   /* p->field: pointee has own lifetime */
    case EXPR_INDEX:       return true;   /* arr[i] is an lvalue */
    default:               return false;  /* function calls, literals, etc. */
    }
}

/* Check if an expression tree contains return/break/continue (forbidden inside defer).
 * Does NOT recurse into nested EXPR_FUNC (lambdas have their own scope). */
static bool expr_contains_control_flow(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_RETURN: case EXPR_BREAK: case EXPR_CONTINUE:
        return true;
    case EXPR_FUNC:
        return false;  /* lambdas have their own scope */
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            if (expr_contains_control_flow(e->block.stmts[i])) return true;
        return false;
    case EXPR_IF:
        return expr_contains_control_flow(e->if_expr.cond) ||
               expr_contains_control_flow(e->if_expr.then_body) ||
               expr_contains_control_flow(e->if_expr.else_body);
    case EXPR_CALL:
        for (int i = 0; i < e->call.arg_count; i++)
            if (expr_contains_control_flow(e->call.args[i])) return true;
        return expr_contains_control_flow(e->call.func);
    case EXPR_BINARY:
        return expr_contains_control_flow(e->binary.left) ||
               expr_contains_control_flow(e->binary.right);
    case EXPR_UNARY_PREFIX:
        return expr_contains_control_flow(e->unary_prefix.operand);
    case EXPR_UNARY_POSTFIX:
        return expr_contains_control_flow(e->unary_postfix.operand);
    case EXPR_ATOMIC_LOAD:
        return expr_contains_control_flow(e->atomic_load.ptr);
    case EXPR_ATOMIC_STORE:
        return expr_contains_control_flow(e->atomic_store.ptr) ||
               expr_contains_control_flow(e->atomic_store.value);
    case EXPR_MATCH:
        if (expr_contains_control_flow(e->match_expr.subject)) return true;
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                if (expr_contains_control_flow(e->match_expr.arms[i].body[j])) return true;
        return false;
    case EXPR_LOOP:
        /* break/continue inside a loop are scoped to that loop, not to defer */
        return false;
    case EXPR_FOR:
        /* Same — break/continue inside for are scoped to the for loop */
        return false;
    case EXPR_ASSIGN:
        return expr_contains_control_flow(e->assign.target) ||
               expr_contains_control_flow(e->assign.value);
    case EXPR_INDEX:
        return expr_contains_control_flow(e->index.object) ||
               expr_contains_control_flow(e->index.index);
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        return expr_contains_control_flow(e->field.object);
    case EXPR_CAST:
        return expr_contains_control_flow(e->cast.operand);
    case EXPR_SOME:
        return expr_contains_control_flow(e->some_expr.value);
    case EXPR_DEFER:
        return expr_contains_control_flow(e->defer_expr.value);
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            if (expr_contains_control_flow(e->tuple_lit.elems[i])) return true;
        return false;
    default:
        return false;
    }
}

/* Wrapper around the per-kind type checker. Consumes the one-shot
 * `in_callee_position` / `in_reflection_position` flags and rejects a generic
 * function used as a value (anywhere other than directly in call position or a
 * `%T` reflection slot): a generic function has no single concrete type until
 * instantiated, so passing/returning/binding it would otherwise emit
 * uncompilable C. The idiomatic pattern is a wrapper lambda that instantiates
 * it at the required type. */
static Type *check_expr(CheckCtx *ctx, Expr *e) {
    bool allow_generic = ctx->in_callee_position || ctx->in_reflection_position;
    ctx->in_callee_position = false;
    ctx->in_reflection_position = false;
    Type *t = check_expr_inner(ctx, e);
    /* Reject a *generic function declaration* used as a value. The signal is the
     * resolved symbol's is_generic flag (set only on generic top-level/module
     * function and type declarations) plus a function type — NOT merely a type
     * that mentions type variables, since a function-typed parameter like
     * `f: ('a) -> 'b` legitimately carries the enclosing generic's type vars and
     * is concrete at each instantiation. */
    if (!allow_generic && t && t->kind == TYPE_FUNC && e->kind == EXPR_IDENT &&
        e->ident.resolved_sym && e->ident.resolved_sym->is_generic) {
        diag_error(e->loc,
            "generic function '%s' cannot be used as a value; wrap it in a lambda "
            "that instantiates it, e.g. (x) -> %s(x)", e->ident.name, e->ident.name);
        e->type = type_error();
        return e->type;
    }
    return t;
}

static Type *check_expr_inner(CheckCtx *ctx, Expr *e) {
    ctx->type_loc = e->loc;  /* best-effort loc for any type resolution in this expr */
    switch (e->kind) {
    case EXPR_INT_LIT:
        e->type = e->int_lit.lit_type;
        /* Expression literals are never negative here — negation is a separate
         * EXPR_UNARY_PREFIX folded earlier — so pass negative=false. */
        check_int_literal_range(e->int_lit.value, e->type, e->loc, e->int_lit.out_of_range, false);
        return e->type;

    case EXPR_FLOAT_LIT:
        e->type = e->float_lit.lit_type;
        check_float_literal_range(e->type, e->float_lit.out_of_range,
                                  e->float_lit.underflow, e->loc);
        return e->type;

    case EXPR_BOOL_LIT:
        e->type = type_bool();
        return e->type;

    case EXPR_CHAR_LIT:
        e->type = type_char();
        return e->type;

    case EXPR_VOID_LIT:
        e->type = type_void();
        return e->type;

    case EXPR_STRING_LIT:
        e->type = type_const_str();
        e->prov = PROV_STATIC;
        return e->type;

    case EXPR_CSTRING_LIT:
        e->type = type_const_cstr();
        e->prov = PROV_STATIC;
        return e->type;

    case EXPR_IDENT: {
        /* 1. Check local scope (stops at current module boundary) */
        const char *cg_name = NULL;
        bool is_mut = false;
        int boundary_crossings = 0;
        bool is_global_binding = false;
        Type *t = scope_lookup_capture(ctx->scope, e->ident.name,
            &cg_name, &is_mut, &boundary_crossings, &is_global_binding);
        if (t) {
            if (boundary_crossings > 0) {
                if (is_mut) {
                    diag_error(e->loc, "cannot capture mutable binding '%s'",
                        e->ident.name);
                    e->type = type_error();
                    return e->type;
                }
                /* Add capture to each lambda_ctx level */
                LambdaCtx *lc = ctx->lambda_ctx;
                for (int bc = 0; bc < boundary_crossings && lc; bc++) {
                    bool found = false;
                    for (int j = 0; j < lc->count; j++) {
                        if (lc->entries[j].codegen_name == cg_name) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        Capture cap = { .name = e->ident.name,
                                        .codegen_name = cg_name,
                                        .type = t };
                        DA_APPEND(lc->entries, lc->count, lc->cap, cap);
                    }
                    lc = lc->parent;
                }
            }
            /* Self-recursion: if this resolves to the self-binding of one of the
               enclosing lambdas (the one at depth boundary_crossings), record that the
               name was used so codegen materializes the self fat pointer. Covers a
               direct self-call (bc == 0) and a self-reference from a nested lambda
               (bc > 0, which is also a normal capture handled above). */
            {
                LambdaCtx *owner = ctx->lambda_ctx;
                for (int bc = 0; bc < boundary_crossings && owner; bc++)
                    owner = owner->parent;
                if (owner && owner->self_codegen_name == cg_name)
                    owner->self_referenced = true;
            }
            e->ident.codegen_name = cg_name;
            e->ident.is_local = !is_global_binding;
            e->ident.is_mut = is_mut;
            /* For global-scope bindings (module-level or top-level lets),
             * look up the Symbol so EXPR_FIELD and EXPR_CALL can use it
             * without re-resolving.  Local bindings (parameters, block-scoped
             * lets) have resolved_sym = NULL. */
            if (is_global_binding) {
                /* Use kind-aware lookup: scope found a let binding, so look
                 * for the DECL_LET symbol specifically.  This avoids returning
                 * a struct symbol if a companion struct shares the name. */
                if (ctx->module_symtab)
                    e->ident.resolved_sym = symtab_lookup_kind(ctx->module_symtab, e->ident.name, DECL_LET);
                if (!e->ident.resolved_sym)
                    e->ident.resolved_sym = global_lookup_kind(ctx->symtab, e->ident.name, DECL_LET, ctx->current_ns);
            }
            e->type = t;
            e->prov = scope_lookup_prov(ctx->scope, e->ident.name);
            return t;
        }
        /* 2. Check module symtab (for within-module sibling/forward references) */
        if (ctx->module_symtab) {
            Symbol *msym = symtab_lookup(ctx->module_symtab, e->ident.name);
            if (msym) {
                if (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION) {
                    e->ident.resolved_sym = msym;
                    e->ident.companion_module = symtab_lookup_kind(ctx->module_symtab, e->ident.name, DECL_MODULE);
                    e->type = msym->type;
                    return e->type;
                }
                if (msym->kind == DECL_MODULE) {
                    e->ident.resolved_sym = msym;
                    e->type = type_void();  /* placeholder; resolved by EXPR_FIELD */
                    return e->type;
                }
                if (!msym->type && msym->decl && msym->decl->kind == DECL_LET) {
                    /* On-demand type check for module sibling with cycle detection */
                    bool cycle = false;
                    for (OnDemandVisited *v = ctx->on_demand_visited; v; v = v->next) {
                        if (v->decl == msym->decl) { cycle = true; break; }
                    }
                    if (cycle) {
                        diag_error(e->loc, "circular dependency: '%s' depends on itself",
                            e->ident.name);
                        e->type = type_error();
                        return e->type;
                    }
                    OnDemandVisited vis = { .decl = msym->decl, .next = ctx->on_demand_visited };
                    ctx->on_demand_visited = &vis;
                    Scope *saved_scope = ctx->scope;
                    ctx->scope = scope_new(ctx->arena, NULL);
                    ctx->scope->is_global = true;
                    check_decl_let(ctx, msym->decl);
                    ctx->scope = saved_scope;
                    ctx->on_demand_visited = vis.next;
                }
                if (msym->decl && msym->decl->kind == DECL_LET && msym->decl->let.codegen_name)
                    e->ident.codegen_name = msym->decl->let.codegen_name;
                if (msym->decl && msym->decl->kind == DECL_LET)
                    e->ident.is_mut = msym->decl->let.is_mut;
                if (!msym->type) {
                    diag_error(e->loc, "use of '%s' before its type is resolved", e->ident.name);
                    e->type = type_error();
                    return e->type;
                }
                e->ident.resolved_sym = msym;
                e->type = msym->type;
                return e->type;
            }
        }
        /* 3. Interleaved import/parent resolution: at each module level, check
         * that level's imports before moving to the parent's members.  This
         * ensures a child's import shadows a parent's member. */
        {
            ImportScope *imp = ctx->import_scope;
            ModuleScopeChain *p = ctx->parent_modules;
            while (true) {
                ImportScope *stop = p ? p->import_scope : NULL;

                /* Check imports at this level */
                Symbol *isym = import_scope_lookup_until(imp, e->ident.name, stop);
                if (isym) {
                    if (isym->kind == DECL_STRUCT || isym->kind == DECL_UNION) {
                        e->ident.resolved_sym = isym;
                        e->ident.companion_module = import_scope_lookup_kind_until(imp, e->ident.name, DECL_MODULE, stop);
                        e->type = isym->type;
                        return e->type;
                    }
                    if (isym->kind == DECL_MODULE) {
                        e->ident.resolved_sym = isym;
                        e->type = type_void();
                        return e->type;
                    }
                    if (!isym->type && isym->decl && isym->decl->kind == DECL_LET) {
                        bool cycle = false;
                        for (OnDemandVisited *v = ctx->on_demand_visited; v; v = v->next) {
                            if (v->decl == isym->decl) { cycle = true; break; }
                        }
                        if (cycle) {
                            diag_error(e->loc, "circular dependency: '%s' depends on itself through imports",
                                e->ident.name);
                            e->type = type_error();
                            return e->type;
                        }
                        ImportRef *ref = import_scope_find_ref_until(imp, e->ident.name, stop);
                        if (ref) {
                            OnDemandVisited vis = { .decl = isym->decl, .next = ctx->on_demand_visited };
                            ctx->on_demand_visited = &vis;
                            SymbolTable *saved_mod = ctx->module_symtab;
                            Scope *saved_scope = ctx->scope;
                            ctx->module_symtab = ref->source_members;
                            ctx->scope = scope_new(ctx->arena, NULL);
                            ctx->scope->is_global = true;
                            check_decl_let(ctx, isym->decl);
                            ctx->scope = saved_scope;
                            ctx->module_symtab = saved_mod;
                            ctx->on_demand_visited = vis.next;
                        }
                    }
                    if (isym->decl && isym->decl->kind == DECL_LET && isym->decl->let.codegen_name)
                        e->ident.codegen_name = isym->decl->let.codegen_name;
                    if (isym->decl && isym->decl->kind == DECL_LET)
                        e->ident.is_mut = isym->decl->let.is_mut;
                    if (!isym->type) {
                        diag_error(e->loc, "use of '%s' before its type is resolved", e->ident.name);
                        e->type = type_error();
                        return e->type;
                    }
                    e->ident.resolved_sym = isym;
                    e->type = isym->type;
                    return e->type;
                }

                if (!p) break;

                /* Check parent members at this level */
                Symbol *psym = symtab_lookup(p->members, e->ident.name);
                if (psym) {
                    if (psym->kind == DECL_STRUCT || psym->kind == DECL_UNION) {
                        e->ident.resolved_sym = psym;
                        e->ident.companion_module = symtab_lookup_kind(p->members, e->ident.name, DECL_MODULE);
                        e->type = psym->type;
                        return e->type;
                    }
                    if (psym->kind == DECL_MODULE) {
                        e->ident.resolved_sym = psym;
                        e->type = type_void();
                        return e->type;
                    }
                    if (!psym->type && psym->decl && psym->decl->kind == DECL_LET) {
                        bool cycle = false;
                        for (OnDemandVisited *v = ctx->on_demand_visited; v; v = v->next) {
                            if (v->decl == psym->decl) { cycle = true; break; }
                        }
                        if (cycle) {
                            diag_error(e->loc, "circular dependency: '%s' depends on itself",
                                e->ident.name);
                            e->type = type_error();
                            return e->type;
                        }
                        OnDemandVisited vis = { .decl = psym->decl, .next = ctx->on_demand_visited };
                        ctx->on_demand_visited = &vis;
                        SymbolTable *saved_mod = ctx->module_symtab;
                        Scope *saved_scope = ctx->scope;
                        ImportScope *saved_imports = ctx->import_scope;
                        ctx->module_symtab = p->members;
                        ctx->import_scope = p->import_scope;
                        ctx->scope = scope_new(ctx->arena, NULL);
                        ctx->scope->is_global = true;
                        check_decl_let(ctx, psym->decl);
                        ctx->scope = saved_scope;
                        ctx->module_symtab = saved_mod;
                        ctx->import_scope = saved_imports;
                        ctx->on_demand_visited = vis.next;
                    }
                    if (psym->decl && psym->decl->kind == DECL_LET && psym->decl->let.codegen_name)
                        e->ident.codegen_name = psym->decl->let.codegen_name;
                    if (psym->decl && psym->decl->kind == DECL_LET)
                        e->ident.is_mut = psym->decl->let.is_mut;
                    if (!psym->type) {
                        diag_error(e->loc, "use of '%s' before its type is resolved", e->ident.name);
                        e->type = type_error();
                        return e->type;
                    }
                    e->ident.resolved_sym = psym;
                    e->type = psym->type;
                    return e->type;
                }

                imp = p->import_scope;
                p = p->parent;
            }
        }
        /* 4. Check global symbol table (namespace-aware) */
        Symbol *sym = global_lookup(ctx->symtab, e->ident.name, ctx->current_ns);
        /* Modules use namespace-aware lookup with error messaging */
        if (!sym) sym = symtab_lookup_module(ctx->symtab, e->ident.name, ctx->current_ns);
        if (!sym) {
            /* Built-in globals: stdin, stdout, stderr */
            const char *n = e->ident.name;
            if (n == intern_cstr(ctx->intern, "stdin") ||
                n == intern_cstr(ctx->intern, "stdout") ||
                n == intern_cstr(ctx->intern, "stderr")) {
                e->type = type_any_ptr();
                return e->type;
            }
            /* Check if a module with this name exists in a different namespace */
            Symbol *other_ns = symtab_lookup_kind(ctx->symtab, e->ident.name, DECL_MODULE);
            if (other_ns) {
                diag_error(e->loc, "module '%s' is in a different namespace; use 'import' to access it",
                    e->ident.name);
            } else if (type_from_name(e->ident.name, (int)strlen(e->ident.name))) {
                /* A type name used where a value is expected — e.g. writing a tuple
                 * type {int32, str} in expression position instead of a value, or
                 * a slice literal element type without the [N]{ ... } body. */
                diag_error(e->loc, "'%s' is a type, not a value", e->ident.name);
            } else {
                diag_error(e->loc, "undefined name '%s'", e->ident.name);
            }
            e->type = type_error();
            return e->type;
        }
        /* For struct/union type names used in expressions (e.g., variant construction),
         * return the type itself */
        if (sym->kind == DECL_STRUCT || sym->kind == DECL_UNION) {
            e->ident.resolved_sym = sym;
            e->ident.companion_module = symtab_lookup_module(ctx->symtab, e->ident.name, ctx->current_ns);
            e->type = sym->type;
            return e->type;
        }
        /* Module names resolve to a sentinel — handled in EXPR_FIELD.
         * Use namespace-aware lookup: prefer same-namespace entry. */
        if (sym->kind == DECL_MODULE) {
            Symbol *ns_mod = symtab_lookup_module(ctx->symtab, e->ident.name, ctx->current_ns);
            if (!ns_mod) {
                diag_error(e->loc, "module '%s' is in a different namespace; use 'import' to access it",
                    e->ident.name);
                e->type = type_error();
                return e->type;
            }
            e->ident.resolved_sym = ns_mod;
            e->type = type_void();  /* placeholder; real type determined by EXPR_FIELD */
            return e->type;
        }
        if (!sym->type && sym->decl && sym->decl->kind == DECL_LET) {
            /* On-demand type check for global symbol with cycle detection */
            bool cycle = false;
            for (OnDemandVisited *v = ctx->on_demand_visited; v; v = v->next) {
                if (v->decl == sym->decl) { cycle = true; break; }
            }
            if (cycle) {
                diag_error(e->loc, "circular dependency: '%s' depends on itself",
                    e->ident.name);
                e->type = type_error();
                return e->type;
            }
            OnDemandVisited vis = { .decl = sym->decl, .next = ctx->on_demand_visited };
            ctx->on_demand_visited = &vis;
            SymbolTable *saved_mod = ctx->module_symtab;
            Scope *saved_scope = ctx->scope;
            ImportScope *saved_imports = ctx->import_scope;
            ctx->module_symtab = NULL;
            ctx->scope = scope_new(ctx->arena, NULL);
            ctx->scope->is_global = true;
            check_decl_let(ctx, sym->decl);
            ctx->scope = saved_scope;
            ctx->module_symtab = saved_mod;
            ctx->import_scope = saved_imports;
            ctx->on_demand_visited = vis.next;
        }
        if (!sym->type) {
            diag_error(e->loc, "use of '%s' before its type is resolved", e->ident.name);
            e->type = type_error();
            return e->type;
        }
        /* Propagate codegen_name from imported/module symbols */
        if (sym->decl && sym->decl->kind == DECL_LET && sym->decl->let.codegen_name) {
            e->ident.codegen_name = sym->decl->let.codegen_name;
        }
        if (sym->decl && sym->decl->kind == DECL_LET)
            e->ident.is_mut = sym->decl->let.is_mut;
        e->ident.resolved_sym = sym;
        e->type = sym->type;
        return e->type;
    }

    case EXPR_BINARY: {
        Type *lt = check_expr(ctx, e->binary.left);
        Type *rt = check_expr(ctx, e->binary.right);
        if (reject_unresolved_recursive_value(e->binary.left) ||
            reject_unresolved_recursive_value(e->binary.right)) {
            e->type = type_error(); return e->type;
        }
        if (type_is_error(lt) || type_is_error(rt)) { e->type = type_error(); return e->type; }
        TokenKind op = e->binary.op;

        /* Allow operations on type variables — defer validation to monomorphization.
         * Reject binary ops on different type variables ('a op 'b): the result
         * type cannot be soundly determined when widening is involved.
         * Same type var ('a op 'a) and concrete op typevar are fine. */
        if (lt->kind == TYPE_TYPE_VAR || rt->kind == TYPE_TYPE_VAR) {
            if (lt->kind == TYPE_TYPE_VAR && rt->kind == TYPE_TYPE_VAR &&
                lt->type_var.name != rt->type_var.name) {
                diag_error(e->loc,
                    "binary operator on different type variables %s and %s",
                    type_name(lt), type_name(rt));
                e->type = type_error();
                return e->type;
            }
            /* Comparison/logical always returns bool */
            if (op == TOK_EQEQ || op == TOK_BANGEQ || op == TOK_LT ||
                op == TOK_GT || op == TOK_LTEQ || op == TOK_GTEQ ||
                op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
                e->type = type_bool();
            } else {
                /* Arithmetic/bitwise: result is the type var type */
                e->type = (lt->kind == TYPE_TYPE_VAR) ? lt : rt;
            }
            return e->type;
        }

        if (op == TOK_PLUS || op == TOK_MINUS) {
            /* Pointer arithmetic: ptr ± int → same pointer type (offset by N elements). */
            if (lt->kind == TYPE_POINTER && type_is_integer(rt)) {
                e->type = lt;
                e->prov = e->binary.left->prov;
                return e->type;
            }
            /* Commutative int + ptr → pointer type (addition only). */
            if (op == TOK_PLUS && type_is_integer(lt) && rt->kind == TYPE_POINTER) {
                e->type = rt;
                e->prov = e->binary.right->prov;
                return e->type;
            }
            /* Pointer difference: ptr - ptr (matching types) → isize element count
             * (C ptrdiff_t semantics). Unchecked — same-buffer is the programmer's. */
            if (op == TOK_MINUS && lt->kind == TYPE_POINTER && rt->kind == TYPE_POINTER) {
                if (!type_eq(lt, rt)) {
                    diag_error(e->loc, "pointer difference requires matching pointer types, got %s and %s",
                        type_name(lt), type_name(rt));
                    e->type = type_error();
                    return e->type;
                }
                e->type = type_isize();
                return e->type;
            }
            if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
                diag_error(e->loc, "arithmetic requires numeric operands, got %s and %s",
                    type_name(lt), type_name(rt));
                e->type = type_error();
                return e->type;
            }
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common) {
                    diag_error(e->loc, "type mismatch: %s vs %s", type_name(lt), type_name(rt));
                    e->type = type_error();
                    return e->type;
                }
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                lt = rt = common;
            }
            e->type = lt;
            return e->type;
        }

        if (op == TOK_STAR || op == TOK_SLASH || op == TOK_PERCENT) {
            if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
                diag_error(e->loc, "arithmetic requires numeric operands, got %s and %s",
                    type_name(lt), type_name(rt));
                e->type = type_error();
                return e->type;
            }
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common) {
                    diag_error(e->loc, "type mismatch: %s vs %s", type_name(lt), type_name(rt));
                    e->type = type_error();
                    return e->type;
                }
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                lt = rt = common;
            }
            e->type = lt;
            return e->type;
        }

        /* Structural equality: == and != work on all types */
        if (op == TOK_EQEQ || op == TOK_BANGEQ) {
            if (type_eq(lt, rt)) {
                e->type = type_bool();
                return e->type;
            }
            /* Try numeric widening for mismatched numeric types */
            Type *common = type_common_numeric(lt, rt);
            if (common) {
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                e->type = type_bool();
                return e->type;
            }
            diag_error(e->loc, "comparison type mismatch: %s vs %s", type_name(lt), type_name(rt));
            e->type = type_error();
            return e->type;
        }

        /* Ordering: < > <= >= require numeric or pointer types */
        if (op == TOK_LT || op == TOK_GT || op == TOK_LTEQ || op == TOK_GTEQ) {
            /* Pointer ordering: both must be same pointer type */
            if (lt->kind == TYPE_POINTER && rt->kind == TYPE_POINTER) {
                if (!type_eq(lt, rt)) {
                    diag_error(e->loc, "comparison type mismatch: %s vs %s", type_name(lt), type_name(rt));
                    e->type = type_error();
                    return e->type;
                }
                e->type = type_bool();
                return e->type;
            }
            if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
                diag_error(e->loc, "ordering comparison requires numeric or pointer types, got %s and %s", type_name(lt), type_name(rt));
                e->type = type_error();
                return e->type;
            }
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common) {
                    diag_error(e->loc, "comparison type mismatch: %s vs %s", type_name(lt), type_name(rt));
                    e->type = type_error();
                    return e->type;
                }
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
            }
            e->type = type_bool();
            return e->type;
        }

        if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
            if (!type_eq(lt, type_bool()) || !type_eq(rt, type_bool())) {
                diag_error(e->loc, "logical operator requires bool operands");
                e->type = type_error();
                return e->type;
            }
            e->type = type_bool();
            return e->type;
        }

        if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET) {
            if (!type_is_integer(lt) || !type_is_integer(rt)) {
                diag_error(e->loc, "bitwise operator requires integer operands");
                e->type = type_error();
                return e->type;
            }
            if (!type_eq(lt, rt)) {
                Type *common = type_common_numeric(lt, rt);
                if (!common) {
                    diag_error(e->loc, "type mismatch: %s vs %s", type_name(lt), type_name(rt));
                    e->type = type_error();
                    return e->type;
                }
                if (!type_eq(lt, common)) e->binary.left = wrap_widen(ctx->arena, e->binary.left, common);
                if (!type_eq(rt, common)) e->binary.right = wrap_widen(ctx->arena, e->binary.right, common);
                lt = rt = common;
            }
            e->type = lt;
            return e->type;
        }

        if (op == TOK_LTLT || op == TOK_GTGT) {
            if (!type_is_integer(lt) || !type_is_integer(rt)) {
                diag_error(e->loc, "shift requires integer operands");
                e->type = type_error();
                return e->type;
            }
            e->type = lt;
            return e->type;
        }

        diag_error(e->loc, "unsupported binary operator");
        e->type = type_error();
        return e->type;
    }

    case EXPR_UNARY_PREFIX: {
        /* Fold -literal before recursing so range check sees final value */
        if (e->unary_prefix.op == TOK_MINUS) {
            Expr *operand = e->unary_prefix.operand;
            if (operand->kind == EXPR_INT_LIT) {
                Type *lt = operand->int_lit.lit_type;
                /* Reject negation of unsigned types */
                if (lt->kind == TYPE_UINT8) {
                    diag_error(e->loc, "integer literal -%" PRIu64 " out of range for uint8 (0..255)", operand->int_lit.value);
                    e->type = type_error(); return e->type;
                }
                if (lt->kind == TYPE_UINT16) {
                    diag_error(e->loc, "integer literal -%" PRIu64 " out of range for uint16 (0..65535)", operand->int_lit.value);
                    e->type = type_error(); return e->type;
                }
                if (lt->kind == TYPE_UINT32) {
                    diag_error(e->loc, "integer literal -%" PRIu64 " out of range for uint32 (0..4294967295)", operand->int_lit.value);
                    e->type = type_error(); return e->type;
                }
                if (lt->kind == TYPE_UINT64) {
                    diag_error(e->loc, "integer literal -%" PRIu64 " out of range for uint64 (0..18446744073709551615)", operand->int_lit.value);
                    e->type = type_error(); return e->type;
                }
                if (lt->kind == TYPE_USIZE) {
                    diag_error(e->loc, "integer literal -%" PRIu64 " out of range for usize", operand->int_lit.value);
                    e->type = type_error(); return e->type;
                }
                /* Negate via two's complement: -(uint64_t)v */
                uint64_t val = -operand->int_lit.value;
                bool oor = operand->int_lit.out_of_range;
                e->kind = EXPR_INT_LIT;
                e->int_lit.value = val;
                e->int_lit.lit_type = lt;
                e->int_lit.out_of_range = oor;
                return check_expr(ctx, e);
            }
            if (operand->kind == EXPR_FLOAT_LIT) {
                double val = -operand->float_lit.value;
                Type *lt = operand->float_lit.lit_type;
                bool oor = operand->float_lit.out_of_range;
                bool uf  = operand->float_lit.underflow;
                e->kind = EXPR_FLOAT_LIT;
                e->float_lit.value = val;
                e->float_lit.lit_type = lt;
                e->float_lit.out_of_range = oor;
                e->float_lit.underflow = uf;
                return check_expr(ctx, e);
            }
        }
        Type *ot = check_expr(ctx, e->unary_prefix.operand);
        if (reject_unresolved_recursive_value(e->unary_prefix.operand)) { e->type = type_error(); return e->type; }
        if (type_is_error(ot)) { e->type = type_error(); return e->type; }
        TokenKind op = e->unary_prefix.op;
        /* Defer unary minus and bitwise not on type variables to monomorphization */
        if (ot->kind == TYPE_TYPE_VAR && (op == TOK_MINUS || op == TOK_TILDE)) {
            e->type = ot;
            return e->type;
        }
        if (op == TOK_MINUS) {
            /* Negation is defined only for signed integers and floats; an
             * unsigned operand has no representable negative and would silently
             * wrap (the literal path already rejects `-5u32`). */
            if (!type_is_signed(ot) && !type_is_float(ot)) {
                diag_error(e->loc, "unary minus requires a signed integer or float operand, got %s", type_name(ot));
                e->type = type_error();
                return e->type;
            }
            e->type = ot;
        } else if (op == TOK_BANG) {
            if (!type_eq(ot, type_bool())) {
                diag_error(e->loc, "unary ! requires bool operand, got %s", type_name(ot));
                e->type = type_error();
                return e->type;
            }
            e->type = type_bool();
        } else if (op == TOK_TILDE) {
            if (!type_is_integer(ot)) {
                diag_error(e->loc, "bitwise not requires integer operand");
                e->type = type_error();
                return e->type;
            }
            e->type = ot;
        } else if (op == TOK_AMP) {
            /* Address-of: only allowed on let mut bindings.
             * Exception: &f on a top-level (non-capturing) function gives a
             * raw C function pointer. */
            Expr *operand = e->unary_prefix.operand;
            /* Cannot take address of inline array field — use .ptr instead */
            if ((operand->kind == EXPR_FIELD || operand->kind == EXPR_DEREF_FIELD) &&
                operand->field.fixed_array_type) {
                diag_error(e->loc,
                    "cannot take address of inline array field; use .ptr for the underlying pointer");
                e->type = type_error();
                return e->type;
            }
            if (operand->kind == EXPR_IDENT && operand->ident.is_local) {
                bool op_is_mut = false;
                scope_lookup_capture(ctx->scope, operand->ident.name,
                    NULL, &op_is_mut, NULL, NULL);
                if (!op_is_mut) {
                    diag_error(e->loc, "address-of requires mutable binding");
                    e->type = type_error();
                    return e->type;
                }
                /* &f on a capturing lambda is an error — only non-capturing
                 * function bindings can yield a raw C function pointer */
                if (ot->kind == TYPE_FUNC &&
                    scope_lookup_is_capturing(ctx->scope, operand->ident.name)) {
                    diag_error(e->loc, "cannot take address of capturing closure");
                    e->type = type_error();
                    return e->type;
                }
            }
            /* &(inline lambda): reject if it captures */
            if (operand->kind == EXPR_FUNC && operand->func.capture_count > 0) {
                diag_error(e->loc, "cannot take address of capturing closure");
                e->type = type_error();
                return e->type;
            }
            if (is_write_through_const(operand)) {
                diag_error(e->loc, "cannot take mutable address through const pointer");
                e->type = type_error();
                return e->type;
            }
            /* &f on a function value yields a raw C function pointer — typed
             * as any* (opaque) because it is strictly a C-interop handle,
             * not an FC pointer that can be dereferenced or called. */
            if (ot->kind == TYPE_FUNC) {
                e->type = type_any_ptr();
                e->prov = PROV_STATIC;
            } else {
                e->type = type_pointer(ctx->arena, ot);
                e->prov = PROV_STACK;
            }
        } else if (op == TOK_STAR) {
            /* Dereference: operand must be pointer */
            if (ot->kind != TYPE_POINTER) {
                diag_error(e->loc, "dereference requires pointer operand, got %s", type_name(ot));
                e->type = type_error();
                return e->type;
            }
            e->type = ot->pointer.pointee;
        } else {
            diag_error(e->loc, "unsupported unary operator");
            e->type = type_error();
            return e->type;
        }
        return e->type;
    }

    case EXPR_UNARY_POSTFIX: {
        Type *ot = check_expr(ctx, e->unary_postfix.operand);
        if (type_is_error(ot)) { e->type = type_error(); return e->type; }
        if (e->unary_postfix.op == TOK_BANG) {
            /* Option unwrap: T? -> T */
            if (ot->kind != TYPE_OPTION) {
                diag_error(e->loc, "unwrap (!) requires option type, got %s", type_name(ot));
                e->type = type_error();
                return e->type;
            }
            e->type = ot->option.inner;
            e->prov = e->unary_postfix.operand->prov;
            return e->type;
        }
        diag_error(e->loc, "unsupported postfix operator");
        e->type = type_error();
        return e->type;
    }

    case EXPR_FUNC: {
        /* If this function was already type-checked (e.g., during on-demand
         * checking of a forward-referenced function), return the cached result.
         * Re-checking would assign different codegen names from a new scope. */
        if (e->type) return e->type;
        bool is_top = ctx->is_top_level_init;
        ctx->is_top_level_init = false;

        /* Create function type from params */
        int pc = e->func.param_count;
        Type **ptypes = NULL;
        if (pc > 0) {
            ptypes = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)pc);
        }

        /* Validate explicit type vars: must not appear in any parameter type */
        for (int i = 0; i < e->func.explicit_type_var_count; i++) {
            const char *tv = e->func.explicit_type_vars[i];
            for (int j = 0; j < pc; j++) {
                if (type_contains_type_var(e->func.params[j].type)) {
                    const char **vars = NULL;
                    int vc = 0, vcap = 0;
                    type_collect_vars(e->func.params[j].type, &vars, &vc, &vcap);
                    for (int k = 0; k < vc; k++) {
                        if (vars[k] == tv || strcmp(vars[k], tv) == 0) {
                            diag_error(e->loc,
                                "type variable %s appears in parameter and in explicit <> declaration",
                                tv);
                            free(vars);
                            goto done_explicit_check;
                        }
                    }
                    free(vars);
                }
            }
            done_explicit_check:;
        }

        /* Create inner scope for function body with lambda boundary */
        Scope *inner = scope_new(ctx->arena, ctx->scope);
        inner->is_lambda_boundary = true;
        for (int i = 0; i < pc; i++) {
            ctx->type_loc = e->func.params[i].loc;  /* precise loc for unknown-type errors */
            ptypes[i] = resolve_type(ctx, e->func.params[i].type);
            if (ptypes[i]->kind == TYPE_FIXED_ARRAY) {
                diag_error(e->func.params[i].loc,
                    "fixed-size array types are only valid in struct field declarations");
                ptypes[i] = type_error();
            }
            e->func.params[i].type = ptypes[i];
            /* Codegen name escapes C reserved words (e.g. a parameter named
             * `register`); the FC lookup name stays raw so source references
             * still resolve. Decl emission applies the same escape. */
            scope_add(inner, e->func.params[i].name,
                c_safe_ident(ctx->intern, e->func.params[i].name), ptypes[i], false);
        }

        /* Consume the pending-self channel set by an enclosing EXPR_LET so this
           lambda's own binding name is visible within its body (self-recursion).
           Added to the body's own inner scope (not the enclosing scope) so references
           resolve as a local — not a capture. Cleared immediately so a nested lambda
           defined inside this body does not inherit it. */
        const char *self_name = ctx->pending_self_name;
        const char *self_cg = ctx->pending_self_codegen;
        Type *self_type = ctx->pending_self_type;
        ctx->pending_self_name = NULL;
        ctx->pending_self_codegen = NULL;
        ctx->pending_self_type = NULL;
        if (self_name)
            scope_add(inner, self_name, self_cg, self_type, false);

        /* Consume the recursion channel set by the enclosing let/decl when this is
           the recursive binding's own initializer. Clearing it here scopes the
           return-type placeholder to this body alone — a nested or anonymous lambda
           checked within finds it empty, so it cannot wrongly anchor the enclosing
           function's return type on its own branches. */
        Type *my_recursive_ret = ctx->pending_recursive_ret;
        const char *my_recursive_self = ctx->pending_recursive_self;
        ctx->pending_recursive_ret = NULL;
        ctx->pending_recursive_self = NULL;

        /* Push lambda context for capture tracking */
        LambdaCtx lctx = { .parent = ctx->lambda_ctx };
        if (self_name) {
            lctx.self_name = self_name;
            lctx.self_codegen_name = self_cg;
        }
        LambdaCtx *saved_lambda = ctx->lambda_ctx;
        ctx->lambda_ctx = &lctx;

        /* Type-check body in inner scope. recursive_ret/self_name are scoped to
           exactly this body (see channel consumption above): NULL for an ordinary
           lambda, the placeholder for the recursive function being resolved. */
        Type *saved_recursive_ret = ctx->recursive_ret;
        const char *saved_recursive_self = ctx->recursive_self_name;
        ctx->recursive_ret = my_recursive_ret;
        ctx->recursive_self_name = my_recursive_self;
        Scope *saved = ctx->scope;
        ctx->scope = inner;
        Type *ret = check_block(ctx, e->func.body, e->func.body_count);
        ctx->scope = saved;
        ctx->recursive_ret = saved_recursive_ret;
        ctx->recursive_self_name = saved_recursive_self;

        /* The body's tail yields no value of its own — it either diverges (`never`:
           a trailing `return value`, or an exhaustive `match` whose every arm
           returns) or every path recurses (the unresolved recursion marker: the
           function's tail is a self-recursive call). Derive the return type from the
           function's `return` statements: the first valued return wins, else a bare
           `return` makes it void. With no returns at all, a `never` tail must have
           come from a return elsewhere → void, whereas an unresolved tail is genuine
           non-terminating recursion → never. The validation loop below then enforces
           agreement across all returns. Patching the placeholder cell (further down)
           propagates the resolved type to the recursive call sites that read it. */
        bool own_marker = ret->kind == TYPE_UNRESOLVED && ret == my_recursive_ret;
        if (ret->kind == TYPE_NEVER || own_marker) {
            Type *derived = NULL;
            for (int i = 0; i < lctx.return_count; i++) {
                Expr *re = lctx.returns[i];
                if (re->return_expr.value && re->return_expr.value->type &&
                    !type_is_error(re->return_expr.value->type)) {
                    derived = re->return_expr.value->type;
                    break;
                }
                if (!re->return_expr.value && !derived) derived = type_void();
            }
            if (!derived)
                derived = own_marker ? type_never() : type_void();
            ret = derived;
        }
        /* An UNRESOLVED tail that is NOT this function's own placeholder is a
           forward reference to another recursive function still being resolved
           (e.g. a nested lambda that calls its enclosing function). Leave ret as
           that function's placeholder cell — patched in place when it resolves —
           exactly as a directly-returned recursive call is handled. */

        /* Reject unconditional infinite self-recursion. If every path through the
           body reaches a *direct* self-recursive call before it can return or
           complete, the function never returns and its generated C trips the C
           compiler's -Winfinite-recursion (in -Wall -Werror) — surface a clear FC
           error instead. The flow analysis covers a tail self-call, recursion in
           every branch of an if/match, a self-call buried in a non-tail statement,
           and recursion inside a breakless loop body. Mutual recursion (a→b→a) is
           deliberately not flagged — gcc doesn't flag it either, and the name match
           in sr_is_self_call excludes the sibling call. An intentional infinite loop
           is written with `loop`, which makes no self-call and is never flagged.
           Poison the return type so dependents (e.g. a binding of the result) don't
           also error. */
        if (body_always_self_recurses(e->func.body, e->func.body_count,
                                      my_recursive_self, my_recursive_ret)) {
            diag_error(e->loc, "this function never returns: it calls itself on every "
                "path with no base case; use 'loop' for an intentional infinite loop");
            ret = type_error();
        }

        /* Validate every explicit `return [value]` against the inferred return type.
           A bare `return` requires a void-returning function; `return value` requires
           strict type equality with the function's inferred return type. No widening
           here — `return` joins the body's tail expression as a symmetric contributor
           to the inferred return type, matching the rule used for `if`/`else` branches,
           `match` arms, and `loop break` values (see spec §Implicit Widening). Users
           who need to widen write an explicit cast: `return (int64) x`. Skip when the
           inferred return type is poisoned to avoid cascading false positives. */
        if (!type_is_error(ret)) {
            for (int i = 0; i < lctx.return_count; i++) {
                Expr *re = lctx.returns[i];
                if (re->return_expr.value) {
                    Type *vt = re->return_expr.value->type;
                    if (!vt || type_is_error(vt)) continue;
                    if (!type_eq(vt, ret)) {
                        diag_error(re->loc,
                            "return type mismatch: expected %s, got %s",
                            type_name(ret), type_name(vt));
                    }
                } else {
                    /* bare `return` */
                    if (ret->kind != TYPE_VOID) {
                        diag_error(re->loc,
                            "return type mismatch: expected %s, got void",
                            type_name(ret));
                    }
                }
            }
        }

        /* Pop lambda context */
        ctx->lambda_ctx = saved_lambda;

        /* Transfer captures to AST node */
        e->func.captures = lctx.entries;
        e->func.capture_count = lctx.count;

        /* Record self-recursion result for codegen: materialize the self fat pointer
           only when the name was actually referenced, keeping generated C -Werror-clean. */
        if (self_name) {
            e->func.self_codegen_name = self_cg;
            e->func.self_referenced = lctx.self_referenced;
        }

        /* Capturing closures have stack-allocated context (compound literal) */
        if (lctx.count > 0)
            e->prov = PROV_STACK;

        /* Generate lifted_name for lambdas (non-top-level functions) */
        if (!is_top) {
            int id = local_id_counter++;
            char buf[64];
            snprintf(buf, sizeof(buf), "_fn_%d", id);
            char *ln = arena_alloc(ctx->arena, strlen(buf) + 1);
            memcpy(ln, buf, strlen(buf) + 1);
            e->func.lifted_name = ln;
        }

        /* Check if implicitly returning stack-derived values */
        if (e->func.body_count > 0) {
            Expr *last = e->func.body[e->func.body_count - 1];
            if (last->prov == PROV_STACK && type_has_provenance(last->type)) {
                if (last->type->kind == TYPE_FUNC)
                    diag_error(last->loc, "cannot return a capturing closure");
                else
                    diag_error(last->loc, "cannot return stack-allocated %s from function",
                        type_name(last->type));
            }
        }

        /* Build function type */
        Type *ft = arena_alloc(ctx->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_types = ptypes;
        ft->func.param_count = pc;
        ft->func.return_type = ret;
        ft->func.type_params = e->func.explicit_type_vars;
        ft->func.type_param_count = e->func.explicit_type_var_count;
        e->type = ft;
        return e->type;
    }

    case EXPR_CALL: {
        /* If this call was already type-checked (e.g., during on-demand checking
         * of a forward-referenced function), return the cached result. Without this,
         * inferred type_args from the first check are misinterpreted as explicit
         * type args on the second check, causing spurious errors. */
        if (e->type) return e->type;
        ctx->in_callee_position = true;
        Type *ft = check_expr(ctx, e->call.func);
        ctx->in_callee_position = false;

        /* `name<Types>` written in value position with no call (parser-marked).
         * Explicit type arguments are only meaningful on a function call (or
         * `.variant` construction), so this is always an error. We resolved the
         * callee in callee position above, suppressing check_expr's generic-as-
         * value guard, so we can emit a message tailored to what `name` is. */
        if (e->call.bare_inst) {
            if (!type_is_error(ft)) {
                Symbol *sym = find_callee_symbol(ctx, e->call.func);
                const char *nm = e->call.func->kind == EXPR_IDENT ? e->call.func->ident.name
                               : e->call.func->kind == EXPR_FIELD ? e->call.func->field.name
                               : "?";
                if (sym && sym->is_generic && sym->kind == DECL_LET) {
                    diag_error(e->loc,
                        "a generic function cannot be used as a value; call it with "
                        "arguments, e.g. %s(...), or wrap it in a lambda", nm);
                } else if (sym && sym->is_generic) {
                    diag_error(e->loc,
                        "generic type '%s' cannot be used as a value", nm);
                } else {
                    diag_error(e->loc,
                        "explicit type arguments require a function call: write '%s(...)'", nm);
                }
            }
            e->type = type_error();
            return e->type;
        }

        if (type_is_error(ft)) { e->type = type_error(); return e->type; }

        /* Check if this is a union variant constructor: union_name.variant(payload) */
        if (ft->kind == TYPE_UNION && e->call.func->kind == EXPR_FIELD) {
            Type *union_type = ft;
            const char *variant_name = e->call.func->field.name;

            /* Look up the union symbol for generic instantiation.
             * Use resolved_sym from EXPR_IDENT — no re-resolution. */
            Symbol *union_sym = NULL;
            if (e->call.func->field.object->kind == EXPR_IDENT) {
                union_sym = e->call.func->field.object->ident.resolved_sym;
            } else if (e->call.func->field.object->kind == EXPR_FIELD) {
                /* Module-qualified path: walk EXPR_FIELD chain to find module, then union */
                Expr *cur = e->call.func->field.object;
                while (cur->kind == EXPR_FIELD) cur = cur->field.object;
                if (cur->kind == EXPR_IDENT) {
                    Symbol *root = cur->ident.resolved_sym;
                    if (root && root->kind != DECL_MODULE && cur->ident.companion_module)
                        root = cur->ident.companion_module;
                    if (root && root->members) {
                        /* Walk intermediate fields to find the innermost module */
                        Symbol *walk = root;
                        Expr *p = e->call.func->field.object;
                        /* Collect intermediate segments (between root and the union name) */
                        Expr **segs = NULL;
                        int nseg = 0, seg_cap = 0;
                        while (p->kind == EXPR_FIELD && p->field.object != cur) {
                            DA_APPEND(segs, nseg, seg_cap, p);
                            p = p->field.object;
                        }
                        /* p is now the EXPR_FIELD whose object is cur (the root ident) */
                        /* Walk from the root module through any nested submodules */
                        /* The last segment's name is the union type name */
                        if (p->kind == EXPR_FIELD) {
                            /* p->field.name is a member of root — look it up */
                            Symbol *member = symtab_lookup_kind(walk->members, p->field.name, DECL_UNION);
                            if (member) {
                                union_sym = member;
                            } else {
                                /* It might be a submodule — walk deeper */
                                Symbol *sub = symtab_lookup_kind(walk->members, p->field.name, DECL_MODULE);
                                if (sub && sub->members) {
                                    walk = sub;
                                    for (int si = nseg - 1; si >= 0; si--) {
                                        Symbol *next = symtab_lookup_kind(walk->members, segs[si]->field.name, DECL_MODULE);
                                        if (!next) {
                                            /* Last segment: must be the union */
                                            union_sym = symtab_lookup_kind(walk->members, segs[si]->field.name, DECL_UNION);
                                            break;
                                        }
                                        walk = next;
                                    }
                                }
                            }
                        }
                        free(segs);
                    }
                }
            }

            /* Instantiate generic union if type args are present */
            int ta_count = e->call.func->field.type_arg_count;
            if (ta_count == 0) ta_count = e->call.type_arg_count;
            Type **ta_types = e->call.func->field.type_args;
            if (!ta_types) ta_types = e->call.type_args;

            if (union_sym && union_sym->is_generic && ta_count > 0) {
                int ntp = union_sym->type_param_count;
                if (ta_count != ntp) {
                    diag_error(e->loc, "expected %d type argument(s), got %d", ntp, ta_count);
                    e->type = type_error();
                    return e->type;
                }
                Type **bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
                for (int k = 0; k < ntp; k++)
                    bindings[k] = resolve_type(ctx, ta_types[k]);

                Type *concrete = type_substitute(ctx->arena, union_sym->type,
                    union_sym->type_params, bindings, ntp);
                if (concrete == union_sym->type) {
                    concrete = type_copy(ctx->arena, union_sym->type);
                }
                if (concrete->unio.type_arg_count == 0) {
                    concrete->unio.type_args = bindings;
                    concrete->unio.type_arg_count = ntp;
                }
                if (!bindings_contain_type_vars(bindings, ntp)) {
                    const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                        union_sym->type->unio.name, NULL,
                        bindings, ntp, union_sym->decl,
                        DECL_UNION, union_sym->type_params, ntp);
                    concrete->unio.name = mangled;
                    MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                    if (mi) mi->concrete_type = concrete;
                }
                union_type = concrete;
            }

            /* Find the variant */
            for (int v = 0; v < union_type->unio.variant_count; v++) {
                if (union_type->unio.variants[v].name == variant_name) {
                    Type *payload_type = resolve_type(ctx, union_type->unio.variants[v].payload);
                    if (!payload_type) {
                        diag_error(e->loc, "variant '%s' takes no payload", variant_name);
                        e->type = type_error();
                        return e->type;
                    }
                    if (e->call.arg_count != 1) {
                        diag_error(e->loc, "variant constructor takes exactly 1 argument");
                        e->type = type_error();
                        return e->type;
                    }
                    Type *arg_type = check_expr(ctx, e->call.args[0]);
                    if (type_is_error(arg_type)) { e->type = type_error(); return e->type; }

                    if (!type_eq(arg_type, payload_type) && !type_contains_type_var(payload_type)) {
                        if (type_can_widen(arg_type, payload_type)) {
                            e->call.args[0] = wrap_widen(ctx->arena, e->call.args[0], payload_type);
                        } else {
                            diag_error(e->call.args[0]->loc,
                                "variant '%s': expected %s, got %s",
                                variant_name, type_name(payload_type), type_name(arg_type));
                            e->type = type_error();
                            return e->type;
                        }
                    }
                    e->type = union_type;
                    /* Propagate stack provenance so a variant carrying a stack
                       pointer taints the union value. */
                    if (e->call.args[0]->prov == PROV_STACK &&
                        type_has_provenance(e->call.args[0]->type))
                        e->prov = PROV_STACK;
                    return e->type;
                }
            }
            diag_error(e->loc, "union '%s' has no variant '%s'",
                type_name(union_type), variant_name);
            e->type = type_error();
            return e->type;
        }

        if (ft->kind != TYPE_FUNC) {
            diag_error(e->loc, "cannot call non-function type %s", type_name(ft));
            e->type = type_error();
            return e->type;
        }

        /* Check for generic function call — either type contains type vars
         * or the callee symbol is marked generic (for explicit-only type vars
         * that don't appear in parameter/return types, e.g. sizeof('a)) */
        Symbol *callee_sym = find_callee_symbol(ctx, e->call.func);
        e->call.resolved_callee = callee_sym;
        bool callee_is_generic = callee_sym && callee_sym->is_generic;
        /* A local function value whose type contains type vars (e.g. a closure
         * parameter 'f: ('a) -> 'b') is NOT a generic call — skip resolution */
        bool is_local_fn_value = (e->call.func->kind == EXPR_IDENT
                                  && e->call.func->ident.is_local)
                                 || ((e->call.func->kind == EXPR_FIELD
                                      || e->call.func->kind == EXPR_DEREF_FIELD)
                                     && !callee_sym);
        if ((type_contains_type_var(ft) || callee_is_generic) && !is_local_fn_value) {
            if (!callee_sym || !callee_sym->is_generic) {
                diag_error(e->loc, "cannot resolve generic function '%s'",
                    e->call.func->kind == EXPR_IDENT ? e->call.func->ident.name : "?");
                e->type = type_error();
                return e->type;
            }

            int ntp = callee_sym->type_param_count;
            Type **bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
            memset(bindings, 0, sizeof(Type*) * (size_t)ntp);

            /* Fill from explicit type args if provided */
            if (e->call.type_arg_count > 0) {
                int n_explicit = callee_sym->explicit_type_param_count;
                if (n_explicit == 0) {
                    diag_error(e->loc,
                        "function '%s' has no explicit type parameters; "
                        "type arguments are inferred from call arguments",
                        callee_sym->name);
                    e->type = type_error();
                    return e->type;
                }
                if (e->call.type_arg_count != n_explicit) {
                    diag_error(e->loc,
                        "expected %d explicit type argument(s), got %d",
                        n_explicit, e->call.type_arg_count);
                    e->type = type_error();
                    return e->type;
                }
                for (int i = 0; i < n_explicit; i++) {
                    bindings[i] = resolve_type(ctx, e->call.type_args[i]);
                }
            }

            /* Check arg count */
            if (e->call.arg_count != ft->func.param_count) {
                diag_error(e->loc, "expected %d arguments, got %d",
                    ft->func.param_count, e->call.arg_count);
                e->type = type_error();
                return e->type;
            }

            /* Type-check args and unify */
            bool arg_err = false;
            for (int i = 0; i < e->call.arg_count; i++) {
                Type *at = check_expr(ctx, e->call.args[i]);
                if (type_is_error(at)) { arg_err = true; continue; }
                if (!unify(ft->func.param_types[i], at,
                           callee_sym->type_params, bindings, ntp)) {
                    /* Unify failed — try implicit widening for concrete params */
                    Type *pt = ft->func.param_types[i];
                    if (!type_contains_type_var(pt) && type_can_widen(at, pt)) {
                        e->call.args[i] = wrap_widen(ctx->arena, e->call.args[i], pt);
                    } else {
                        diag_error(e->call.args[i]->loc,
                            "argument %d: type mismatch", i + 1);
                        arg_err = true;
                    }
                }
            }
            if (arg_err) { e->type = type_error(); return e->type; }

            /* Check all type vars resolved */
            for (int i = 0; i < ntp; i++) {
                if (!bindings[i]) {
                    diag_error(e->loc, "could not infer type variable %s",
                        callee_sym->type_params[i]);
                    e->type = type_error();
                    return e->type;
                }
            }

            /* Validate generic body with concrete types.
             * Skip if the template itself has errors (TYPE_ERROR in return type)
             * to avoid cascading call-site noise from template-level problems. */
            if (!bindings_contain_type_vars(bindings, ntp)) {
                Decl *tmpl = callee_sym->decl;
                if (tmpl && tmpl->kind == DECL_LET && tmpl->let.init &&
                    tmpl->let.init->kind == EXPR_FUNC) {
                    Expr *func_expr = tmpl->let.init;
                    bool tmpl_has_errors = false;
                    if (func_expr->func.body_count > 0) {
                        Expr *last = func_expr->func.body[func_expr->func.body_count - 1];
                        tmpl_has_errors = last->type && type_is_error(last->type);
                    }
                    if (!tmpl_has_errors) {
                        const char *tmp = fmt_generic_inst(callee_sym->name, ctx->arena,
                            ft, callee_sym->type_params, bindings, ntp);
                        const char *inst_desc = arena_strdup(ctx->arena, tmp, (int) strlen(tmp));
                        /* Fresh memo for this top-level validation; seed it with the
                         * entry instantiation so its own self-calls don't re-descend
                         * into an identical body. The memo key is depth-prefixed to
                         * match the keys the cross-body descent builds (kept separate
                         * from inst_desc, which is the display descriptor). */
                        int seed_d = 0;
                        for (int i = 0; i < ntp; i++) {
                            int d = gen_inst_type_depth(bindings[i]);
                            if (d > seed_d) seed_d = d;
                        }
                        char seedbuf[512];
                        int slen = snprintf(seedbuf, sizeof(seedbuf), "%d:%s", seed_d, tmp);
                        if (slen < 0) slen = 0;
                        if (slen >= (int)sizeof(seedbuf)) slen = (int)sizeof(seedbuf) - 1;
                        gen_seen_reset();
                        gen_seen_add(ctx->arena, callee_sym, arena_strdup(ctx->arena, seedbuf, slen));
                        /* Entry (root) chain frame: this instantiation, required by
                         * the user's call `e`. Transitive descents push children. */
                        InstFrame root = { inst_desc, e->loc, NULL };
                        for (int i = 0; i < func_expr->func.body_count; i++) {
                            if (!validate_generic_body(func_expr->func.body[i], ctx->arena,
                                    callee_sym->type_params, bindings, ntp, &root)) {
                                e->type = type_error();
                                return e->type;
                            }
                        }
                    }
                }
            }

            /* Substitute to get concrete return type */
            Type *concrete_ret = type_substitute(ctx->arena, ft->func.return_type,
                callee_sym->type_params, bindings, ntp);

            /* Register mono instances for any generic struct/union in the return type */
            if (concrete_ret && !type_contains_type_var(concrete_ret)) {
                concrete_ret = resolve_generic_types_in_ret(ctx, concrete_ret);
            }

            /* Store the inferred type args on the call for codegen */
            e->call.type_args = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
            memcpy(e->call.type_args, bindings, sizeof(Type*) * (size_t)ntp);
            e->call.type_arg_count = ntp;

            /* If any binding still has type vars, defer monomorphization to codegen */
            if (!bindings_contain_type_vars(bindings, ntp)) {
                const char *base_name = callee_sym->name;
                Decl *tmpl_decl = callee_sym->decl;
                if (tmpl_decl && tmpl_decl->kind == DECL_LET && tmpl_decl->let.codegen_name) {
                    base_name = tmpl_decl->let.codegen_name;
                }
                const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                    base_name, NULL, bindings, ntp, tmpl_decl,
                    DECL_LET, callee_sym->type_params, ntp);
                e->call.mangled_name = mangled;
            }

            e->call.is_indirect = false;
            e->type = concrete_ret;
            return e->type;
        }

        /* Normal (non-generic) call */
        if (ft->func.is_variadic) {
            if (e->call.arg_count < ft->func.param_count) {
                diag_error(e->loc, "expected at least %d arguments, got %d",
                    ft->func.param_count, e->call.arg_count);
                e->type = type_error();
                return e->type;
            }
        } else if (e->call.arg_count != ft->func.param_count) {
            diag_error(e->loc, "expected %d arguments, got %d",
                ft->func.param_count, e->call.arg_count);
            e->type = type_error();
            return e->type;
        }
        bool arg_err = false;
        for (int i = 0; i < e->call.arg_count; i++) {
            Type *at = check_expr(ctx, e->call.args[i]);
            if (reject_unresolved_recursive_value(e->call.args[i])) { arg_err = true; continue; }
            if (type_is_error(at)) { arg_err = true; continue; }
            /* Variadic args beyond fixed params: type-check the expr but skip param matching */
            if (i >= ft->func.param_count) continue;
            /* Skip strict type check if either side contains type vars
             * (inside a generic template — checked at monomorphization) */
            if (type_contains_type_var(at) || type_contains_type_var(ft->func.param_types[i]))
                continue;
            if (!type_eq(at, ft->func.param_types[i])) {
                if (type_can_widen(at, ft->func.param_types[i])) {
                    e->call.args[i] = wrap_widen(ctx->arena, e->call.args[i], ft->func.param_types[i]);
                } else {
                    diag_error(e->call.args[i]->loc, "argument %d: expected %s, got %s",
                        i + 1, type_name(ft->func.param_types[i]), type_name(at));
                    arg_err = true;
                }
            }
        }
        if (arg_err) { e->type = type_error(); return e->type; }

        /* Determine call mode: direct vs indirect */
        e->call.is_indirect = true;  /* default to indirect for function-type callees */
        if (e->call.func->kind == EXPR_IDENT && !e->call.func->ident.is_local) {
            e->call.is_indirect = false;  /* global function → direct */
        } else if (e->call.func->kind == EXPR_FIELD && e->call.func->field.codegen_name) {
            e->call.is_indirect = false;  /* module function → direct */
        }

        /* Detect extern calls — these skip the _ctx parameter */
        if (callee_sym && callee_sym->kind == DECL_EXTERN) {
            e->call.is_extern_call = true;
            /* Validate function-type args: only top-level functions and
             * non-capturing lambdas can be passed as C function pointers. */
            for (int i = 0; i < e->call.arg_count && i < ft->func.param_count; i++) {
                if (ft->func.param_types[i]->kind != TYPE_FUNC) continue;
                Expr *arg = e->call.args[i];
                if (arg->kind == EXPR_IDENT && !arg->ident.is_local) continue;
                if (arg->kind == EXPR_FUNC && arg->func.capture_count == 0) continue;
                if (arg->kind == EXPR_FUNC && arg->func.capture_count > 0)
                    diag_error(arg->loc, "cannot pass capturing closure to extern — "
                        "C function pointers cannot represent closures");
                else
                    diag_error(arg->loc, "cannot pass function value to extern — "
                        "only top-level functions and non-capturing lambdas "
                        "can be used as C function pointers");
            }
        }

        e->type = ft->func.return_type;
        return e->type;
    }

    case EXPR_IF: {
        Type *ct = check_expr(ctx, e->if_expr.cond);
        if (reject_unresolved_recursive_value(e->if_expr.cond)) ct = type_error();
        if (type_is_error(ct)) {
            /* Still check branches for more errors */
            Type *tt = check_expr(ctx, e->if_expr.then_body);
            if (e->if_expr.else_body) check_expr(ctx, e->if_expr.else_body);
            (void)tt;
            e->type = type_error();
            return e->type;
        }
        if (!type_eq(ct, type_bool())) {
            diag_error(e->loc, "if condition must be bool, got %s", type_name(ct));
            Type *tt = check_expr(ctx, e->if_expr.then_body);
            if (e->if_expr.else_body) check_expr(ctx, e->if_expr.else_body);
            (void)tt;
            e->type = type_error();
            return e->type;
        }
        /* While inferring a recursive function's return type, check a base-case
         * branch before a branch that consumes a self-recursive call's result, so
         * the placeholder is anchored first (order-independent: the base case may
         * be in either branch). This only reorders checking; types are unchanged. */
        Type *tt, *et = NULL;
        bool if_reorder = resolving_recursion(ctx) && e->if_expr.else_body &&
            !branch_can_anchor(e->if_expr.then_body, ctx->recursive_self_name) &&
            branch_can_anchor(e->if_expr.else_body, ctx->recursive_self_name);
        if (if_reorder) {
            et = check_expr(ctx, e->if_expr.else_body);
            maybe_anchor_recursive(ctx, et);
            tt = check_expr(ctx, e->if_expr.then_body);
        } else {
            tt = check_expr(ctx, e->if_expr.then_body);
            maybe_anchor_recursive(ctx, tt);
            if (e->if_expr.else_body) et = check_expr(ctx, e->if_expr.else_body);
        }
        if (e->if_expr.else_body) {
            if (type_is_error(tt) || type_is_error(et)) {
                /* Use whichever is non-error, or error if both */
                e->type = type_is_error(tt) ? et : tt;
                if (type_is_error(e->type)) e->type = type_error();
                return e->type;
            }
            Type *unified = unify_branch(tt, et);
            if (!unified) {
                diag_error(e->loc, "if branches have different types: %s vs %s",
                    type_name(tt), type_name(et));
                e->type = type_error();
                return e->type;
            }
            e->type = unified;
            /* Provenance comes from the value-producing branch(es); a diverging
               (never) branch yields no value and contributes none. */
            if (type_is_never(tt))
                e->prov = e->if_expr.else_body->prov;
            else if (type_is_never(et))
                e->prov = e->if_expr.then_body->prov;
            else
                e->prov = merge_prov(e->if_expr.then_body->prov, e->if_expr.else_body->prov);
        } else {
            /* No else → void */
            e->type = type_void();
        }
        return e->type;
    }

    case EXPR_BLOCK: {
        Scope *inner = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = inner;
        e->type = check_block(ctx, e->block.stmts, e->block.count);
        if (e->block.count > 0)
            e->prov = e->block.stmts[e->block.count - 1]->prov;
        ctx->scope = saved;
        return e->type;
    }

    case EXPR_LET: {
        if (e->type) return e->type;

        /* Assign the codegen name up front. A `let f = <lambda>` binding must have its
           name fixed before the init is checked so the lambda body can refer to it for
           self-recursion. */
        int id = local_id_counter++;
        const char *cg = make_local_name(ctx->arena, "_l_", e->let_expr.let_name, id);
        e->let_expr.codegen_name = cg;

        /* Self-recursion setup: when the init is a direct lambda and the binding is
           immutable, pre-register a partial function type (params known, return type a
           mutable placeholder cell) and hand it to the wrapped EXPR_FUNC via the
           pending-self channel so the body can call itself. The placeholder is patched
           in place after the body is checked, mirroring top-level recursion in
           check_decl_let. `let mut` lambdas are excluded — a mutable binding is neither
           capturable nor stable enough to refer to itself. */
        Type *self_placeholder = NULL;
        const char *saved_psn = ctx->pending_self_name;
        const char *saved_psc = ctx->pending_self_codegen;
        Type *saved_pst = ctx->pending_self_type;
        Type *saved_prr = ctx->pending_recursive_ret;
        const char *saved_prs = ctx->pending_recursive_self;
        if (e->let_expr.let_init->kind == EXPR_FUNC && !e->let_expr.let_is_mut) {
            Expr *fn = e->let_expr.let_init;
            int pc = fn->func.param_count;
            Type **ptypes = NULL;
            if (pc > 0) ptypes = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)pc);
            for (int i = 0; i < pc; i++)
                ptypes[i] = resolve_type(ctx, fn->func.params[i].type);

            self_placeholder = malloc(sizeof(Type));
            memset(self_placeholder, 0, sizeof(Type));
            self_placeholder->kind = TYPE_UNRESOLVED;  /* patched after body check */

            Type *ft = arena_alloc(ctx->arena, sizeof(Type));
            ft->kind = TYPE_FUNC;
            ft->func.param_types = ptypes;
            ft->func.param_count = pc;
            ft->func.return_type = self_placeholder;
            ft->func.type_params = fn->func.explicit_type_vars;
            ft->func.type_param_count = fn->func.explicit_type_var_count;

            ctx->pending_self_name = e->let_expr.let_name;
            ctx->pending_self_codegen = cg;
            ctx->pending_self_type = ft;
            ctx->pending_recursive_ret = self_placeholder;
            ctx->pending_recursive_self = e->let_expr.let_name;
        }

        Type *t = check_expr(ctx, e->let_expr.let_init);

        /* Restore the channels. EXPR_FUNC clears them on consumption, but restore
           unconditionally so error paths and non-lambda inits leave them clean. */
        ctx->pending_self_name = saved_psn;
        ctx->pending_self_codegen = saved_psc;
        ctx->pending_self_type = saved_pst;
        ctx->pending_recursive_ret = saved_prr;
        ctx->pending_recursive_self = saved_prs;

        if (type_is_error(t)) {
            /* Add binding with error type so subsequent uses don't cascade "undefined" */
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        if (t->kind == TYPE_VOID) {
            diag_error(e->loc, "cannot bind void expression to '%s'", e->let_expr.let_name);
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        if (t->kind == TYPE_NEVER) {
            diag_error(e->loc, "cannot bind '%s': every path through this expression "
                "returns, so it has no value", e->let_expr.let_name);
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        if (t->kind == TYPE_UNRESOLVED) {
            /* The value is (or derives from) a self-recursive call made before any
               base case anchors the return type — i.e. unconditional recursion. */
            diag_error(e->loc, "cannot bind '%s': it depends on a recursive call made "
                "before a base case establishes the return type", e->let_expr.let_name);
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }

        /* Patch the placeholder return type in place so all recursive call sites (which
           captured the placeholder cell) observe the final inferred return type. */
        if (self_placeholder) {
            Type *actual_ret = t->kind == TYPE_FUNC ? t->func.return_type : t;
            *self_placeholder = *actual_ret;
        }

        e->let_expr.let_type = t;
        scope_add_prov(ctx->scope, e->let_expr.let_name, cg, t, e->let_expr.let_is_mut,
                        e->let_expr.let_init->prov);
        /* Mark binding as capturing if init is a lambda with captures */
        if (e->let_expr.let_init->kind == EXPR_FUNC &&
            e->let_expr.let_init->func.capture_count > 0) {
            Scope *sc = ctx->scope;
            for (int ci = sc->local_count - 1; ci >= 0; ci--) {
                if (sc->locals[ci].codegen_name == cg) {
                    sc->locals[ci].is_capturing = true;
                    break;
                }
            }
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_LET_DESTRUCT: {
        Type *t = check_expr(ctx, e->let_destruct.init);
        if (type_is_error(t)) {
            e->type = type_void();
            return e->type;
        }
        if (t->kind == TYPE_NEVER) {
            diag_error(e->loc, "cannot destructure: every path through this "
                "expression returns, so it has no value");
            e->type = type_void();
            return e->type;
        }
        if (t->kind != TYPE_STRUCT) {
            diag_error(e->loc, "cannot destructure non-struct type %s", type_name(t));
            e->type = type_void();
            return e->type;
        }
        e->let_destruct.init_type = t;

        /* Generate temp name for the RHS struct value */
        int tmp_id = local_id_counter++;
        int ds_len = snprintf(NULL, 0, "_ds_%d", tmp_id) + 1;
        char *tmp_name = arena_alloc(ctx->arena, (size_t)ds_len);
        snprintf(tmp_name, (size_t)ds_len, "_ds_%d", tmp_id);
        e->let_destruct.tmp_name = tmp_name;

        if (e->let_destruct.pattern->kind == PAT_TUPLE)
            check_tuple_destruct(ctx, e->let_destruct.pattern, t, e->let_destruct.is_mut, e->loc);
        else
            check_destruct_pattern(ctx, e->let_destruct.pattern, t, e->let_destruct.is_mut, e->loc);

        e->type = type_void();
        return e->type;
    }

    case EXPR_RETURN: {
        if (e->return_expr.value) {
            check_expr(ctx, e->return_expr.value);
            /* Check if returning stack-derived values */
            if (e->return_expr.value->prov == PROV_STACK &&
                type_has_provenance(e->return_expr.value->type)) {
                if (e->return_expr.value->type->kind == TYPE_FUNC)
                    diag_error(e->loc, "cannot return a capturing closure");
                else
                    diag_error(e->loc, "cannot return stack-allocated %s from function",
                        type_name(e->return_expr.value->type));
            }
            /* An early `return value` is a base case too: anchor the recursive
               return type from it, so a self-recursive call appearing later in the
               body (the common guard-clause idiom `if base then return v; … f(…) …`)
               observes the inferred type. The final return-type check below still
               enforces agreement across every return. */
            maybe_anchor_recursive(ctx, e->return_expr.value->type);
        }
        /* Register with the enclosing function so its value can be checked against
           the inferred return type once the body's tail type is known. */
        if (ctx->lambda_ctx) {
            LambdaCtx *lc = ctx->lambda_ctx;
            DA_APPEND(lc->returns, lc->return_count, lc->return_cap, e);
        }
        e->type = type_never();
        return e->type;
    }

    case EXPR_ASSIGN: {
        Type *lt = check_expr(ctx, e->assign.target);
        Type *vt = check_expr(ctx, e->assign.value);
        if (type_is_error(lt) || type_is_error(vt)) {
            e->type = type_void();
            return e->type;
        }
        /* Reject reassignment of immutable (let) bindings */
        if (e->assign.target->kind == EXPR_IDENT && !e->assign.target->ident.is_mut) {
            diag_error(e->loc, "cannot assign to immutable binding '%s'",
                e->assign.target->ident.name);
        }
        /* Reject self-assignment (x = x) — it's always a no-op */
        if (e->assign.target->kind == EXPR_IDENT && e->assign.value->kind == EXPR_IDENT &&
            e->assign.target->ident.name == e->assign.value->ident.name) {
            diag_error(e->loc, "self-assignment of '%s' has no effect", e->assign.target->ident.name);
        }
        if (!type_eq(lt, vt)) {
            if (type_can_widen(vt, lt)) {
                e->assign.value = wrap_widen(ctx->arena, e->assign.value, lt);
            } else {
                diag_error(e->loc, "assignment type mismatch: %s vs %s", type_name(lt), type_name(vt));
            }
        }
        if (is_write_through_const(e->assign.target)) {
            diag_error(e->loc, "cannot assign through const pointer/slice");
        }
        /* Reject assignment to slice .len and .ptr fields */
        if (e->assign.target->kind == EXPR_FIELD) {
            Expr *obj = e->assign.target->field.object;
            Type *ot = obj->type;
            if (ot && ot->kind == TYPE_SLICE &&
                (strcmp(e->assign.target->field.name, "len") == 0 ||
                 strcmp(e->assign.target->field.name, "ptr") == 0)) {
                diag_error(e->loc, "cannot assign to slice .%s field", e->assign.target->field.name);
            }
            if (ot && ot->kind == TYPE_OPTION &&
                (strcmp(e->assign.target->field.name, "is_some") == 0 ||
                 strcmp(e->assign.target->field.name, "is_none") == 0)) {
                diag_error(e->loc, "cannot assign to option .%s field", e->assign.target->field.name);
            }
        }
        /* Escape check: storing stack-allocated values where they outlive the stack frame. */
        if (e->assign.value->prov == PROV_STACK && type_has_provenance(vt)) {
            Expr *target = e->assign.target;
            if (target->kind == EXPR_IDENT && !target->ident.is_local) {
                /* Direct store to a global binding. */
                diag_error(e->loc, "cannot store stack-allocated %s in global '%s'",
                    type_name(vt), target->ident.name);
            } else if (target->kind != EXPR_IDENT) {
                /* Store through a field / index / deref path: reject when the
                 * destination storage outlives the frame (heap or static). A
                 * direct local ident is exempt — its own storage is the frame,
                 * and the taint below tracks the binding instead. */
                Provenance dest = assign_dest_prov(target);
                if (dest == PROV_HEAP || dest == PROV_STATIC) {
                    bool is_field = (target->kind == EXPR_DEREF_FIELD ||
                                     target->kind == EXPR_FIELD);
                    diag_error(e->loc,
                        "cannot store stack-allocated %s in heap-allocated %s",
                        type_name(vt), is_field ? "struct field" : "memory");
                }
            }
        }
        /* Reassignment of a mut local: merge the value's provenance into the
         * binding so subsequent reads (return/global/heap sinks) see a stack
         * taint that was assigned after the original binding. */
        if (e->assign.target->kind == EXPR_IDENT && e->assign.target->ident.is_local &&
            type_has_provenance(vt)) {
            scope_taint_prov(ctx->scope, e->assign.target->ident.codegen_name,
                e->assign.value->prov);
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_CAST: {
        Type *from = check_expr(ctx, e->cast.operand);
        if (reject_unresolved_recursive_value(e->cast.operand)) { e->type = type_error(); return e->type; }
        if (type_is_error(from)) { e->type = type_error(); return e->type; }
        Type *to = resolve_type(ctx, e->cast.target);
        e->cast.target = to;
        bool from_num = type_is_numeric(from);
        bool to_num = type_is_numeric(to);
        bool from_ptr = (from->kind == TYPE_POINTER || from->kind == TYPE_ANY_PTR);
        bool to_ptr = (to->kind == TYPE_POINTER || to->kind == TYPE_ANY_PTR);
        bool from_int = type_is_integer(from);
        bool to_int = type_is_integer(to);
        /* Pointer<->integer conversions are restricted to the pointer-width
         * integer types (usize/isize). A fixed-width int (int32/uint64/...)
         * does not match the target's pointer width, so the cast is both
         * lossy on some targets and a -Wpointer-to-int-cast /
         * -Wint-to-pointer-cast failure in C. This mirrors FC's refusal to
         * implicitly widen to/from usize/isize (their width is target-defined). */
        bool from_ptrwidth_int = (from->kind == TYPE_USIZE || from->kind == TYPE_ISIZE);
        bool to_ptrwidth_int = (to->kind == TYPE_USIZE || to->kind == TYPE_ISIZE);
        bool bool_to_num = (from->kind == TYPE_BOOL && to_num);
        bool str_to_cstr = (is_str_type(from) && is_cstr_type(to));
        bool cstr_to_str = (is_cstr_type(from) && is_str_type(to));
        bool const_change_slice = (from->kind == TYPE_SLICE && to->kind == TYPE_SLICE &&
            type_eq_ignore_const(from, to));
        /* Identity cast (same type) is a no-op. Allow it uniformly for the scalar
         * types C can actually cast to (numeric, pointer, bool, char), matching
         * numeric identity casts like (int32) int32. Aggregate types
         * (struct/union/slice) cannot be cast in C, so they stay rejected. */
        bool same_type = type_eq(from, to) &&
            (from_num || from_ptr || from->kind == TYPE_BOOL || from->kind == TYPE_CHAR);
        /* (T!) opts out of the saturating float→int helper. It is the *only* cast
         * that emits a runtime check, so the unchecked `!` is meaningful only there;
         * anywhere else it would be silently inert, so reject it (mirrors rejecting
         * `x!` on a non-option). */
        if (e->cast.unchecked && !(type_is_float(from) && to_int)) {
            diag_error(e->loc,
                "redundant '!': this cast inserts no runtime check; '!' is allowed "
                "only on a float-to-integer cast");
            e->type = type_error();
            return e->type;
        }
        /* A pointer<->integer cast through a fixed-width int gets a targeted
         * diagnostic pointing to the pointer-width types, rather than the
         * generic "invalid cast" below. */
        if ((from_ptr && to_int && !to_ptrwidth_int) ||
            (from_int && !from_ptrwidth_int && to_ptr)) {
            diag_error(e->loc,
                "pointer<->integer cast must go through usize/isize (the "
                "pointer-width integer types), not %s; chain through "
                "(usize)/(isize) to convert width",
                type_name(from_ptr ? to : from));
            e->type = type_error();
            return e->type;
        }
        /* Allowed: identity, numeric <-> numeric, bool -> numeric (0/1),
         * pointer <-> pointer, pointer <-> usize/isize, str <-> cstr, slice
         * const cast. NOT allowed: numeric -> bool (ambiguous: !=0 vs 0/1?). */
        if (!(same_type || (from_num && to_num) || bool_to_num || (from_ptr && to_ptr) ||
              (from_ptr && to_ptrwidth_int) || (from_ptrwidth_int && to_ptr) ||
              str_to_cstr || cstr_to_str || const_change_slice)) {
            diag_error(e->loc, "invalid cast from %s to %s", type_name(from), type_name(to));
            e->type = type_error();
            return e->type;
        }
        /* (cstr[N]) buffer size is only meaningful for a str→cstr cast. */
        if (e->cast.buffer_size > 0 && !str_to_cstr) {
            diag_error(e->loc,
                "[N] buffer size applies only to a (cstr) cast of a str");
            e->type = type_error();
            return e->type;
        }
        /* An unbounded str→cstr cast is rejected: its stack copy is runtime-sized
         * and would grow the frame per loop iteration. Force an explicit home —
         * unless this cast is the direct init of alloc(...)/alloca(...), which gives
         * it one (the `licensed` flag, set by the parser). */
        if (str_to_cstr && e->cast.buffer_size == 0 && !e->cast.licensed) {
            if (e->cast.operand->kind == EXPR_STRING_LIT)
                diag_error(e->loc,
                    "use a c\"...\" literal for a null-terminated string constant "
                    "instead of casting with (cstr)");
            else
                diag_error(e->loc,
                    "unbounded (cstr) cast of a runtime-length str; use (cstr[N]) for a "
                    "fixed N-byte stack buffer (truncating), alloc((cstr) ...)! for the "
                    "heap, or alloca((cstr) ...) for dynamic stack");
            e->type = type_error();
            return e->type;
        }
        e->type = to;
        /* str→cstr creates a stack copy; cstr→str wraps with strlen on stack */
        if (str_to_cstr)
            e->prov = PROV_STACK;
        else if (cstr_to_str) {
            e->prov = e->cast.operand->prov;  /* preserves source provenance */
            if (from->is_const && is_str_type(to)) {
                Type *ct = type_make_const(ctx->arena, to);
                e->type = ct;
            }
        } else if (type_has_provenance(to))
            e->prov = e->cast.operand->prov;   /* pointer casts preserve provenance */
        return e->type;
    }

    case EXPR_STRUCT_LIT: {
        if (e->type) return e->type;
        /* Look up the struct type */
        Symbol *sym = NULL;

        /* Check for module-qualified name: "module.type", "a.b.type", etc. */
        if (strchr(e->struct_lit.type_name, '.')) {
            SymbolTable *owner_members = NULL;
            sym = resolve_dotted_name_ex(ctx, e->struct_lit.type_name, &owner_members);
            if (sym && sym->is_private && ctx->module_symtab != owner_members) {
                diag_error(e->loc, "cannot access private type '%s'",
                    e->struct_lit.type_name);
                e->type = type_error();
                return e->type;
            }
        }

        if (!sym)
            sym = resolve_symbol_kind(ctx, e->struct_lit.type_name, DECL_STRUCT);
        /* Fallback: try general lookup (for within-module struct references) */
        if (!sym)
            sym = resolve_symbol(ctx, e->struct_lit.type_name);
        if (!sym) {
            diag_error(e->loc, "unknown type '%s'", e->struct_lit.type_name);
            e->type = type_error();
            return e->type;
        }
        e->struct_lit.resolved_sym = sym;
        if (sym->kind != DECL_STRUCT || !sym->type || sym->type->kind != TYPE_STRUCT) {
            diag_error(e->loc, "'%s' is not a struct type", e->struct_lit.type_name);
            e->type = type_error();
            return e->type;
        }
        Type *st = sym->type;

        /* Reject duplicate field names. Silent last-wins emits override-init C
         * that fails -Wextra and almost always indicates a typo. */
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            for (int j = 0; j < i; j++) {
                if (e->struct_lit.fields[i].name == e->struct_lit.fields[j].name) {
                    diag_error(e->loc, "duplicate field '%s' in struct literal '%s'",
                        e->struct_lit.fields[i].name, e->struct_lit.type_name);
                    e->type = type_error();
                    return e->type;
                }
            }
        }

        /* Type-check each field init */
        bool field_error = false;
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            FieldInit *fi = &e->struct_lit.fields[i];
            bool found = false;
            for (int j = 0; j < st->struc.field_count; j++) {
                if (st->struc.fields[j].name == fi->name) {
                    Type *fval = check_expr(ctx, fi->value);
                    if (type_is_error(fval)) { field_error = true; found = true; break; }
                    Type *expected = resolve_type(ctx, st->struc.fields[j].type);
                    /* Fixed-array fields accept slice of matching element type */
                    Type *check_type = expected;
                    if (expected->kind == TYPE_FIXED_ARRAY)
                        check_type = type_slice(ctx->arena, expected->fixed_array.elem);
                    if (!type_eq(fval, check_type) && !type_contains_type_var(check_type)) {
                        if (type_can_widen(fval, check_type)) {
                            fi->value = wrap_widen(ctx->arena, fi->value, check_type);
                        } else {
                            diag_error(fi->value->loc, "field '%s': expected %s, got %s",
                                fi->name, type_name(check_type), type_name(fval));
                            field_error = true;
                        }
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                diag_error(e->loc, "struct '%s' has no field '%s'",
                    e->struct_lit.type_name, fi->name);
                field_error = true;
            }
        }
        if (field_error) { e->type = type_error(); return e->type; }

        /* Generic struct instantiation: unify field types with provided values */
        if (sym->is_generic) {
            int ntp = sym->type_param_count;
            Type **bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
            memset(bindings, 0, sizeof(Type*) * (size_t)ntp);
            bool unify_err = false;
            for (int i = 0; i < e->struct_lit.field_count; i++) {
                FieldInit *fi = &e->struct_lit.fields[i];
                for (int j = 0; j < st->struc.field_count; j++) {
                    if (st->struc.fields[j].name == fi->name) {
                        if (!unify(st->struc.fields[j].type, fi->value->type,
                                   sym->type_params, bindings, ntp)) {
                            diag_error(fi->value->loc, "field '%s': type mismatch in generic struct",
                                fi->name);
                            unify_err = true;
                        }
                        break;
                    }
                }
            }
            if (unify_err) { e->type = type_error(); return e->type; }
            for (int i = 0; i < ntp; i++) {
                if (!bindings[i]) {
                    diag_error(e->loc, "could not infer type variable %s", sym->type_params[i]);
                    e->type = type_error();
                    return e->type;
                }
            }
            Type *concrete = type_substitute(ctx->arena, st,
                sym->type_params, bindings, ntp);
            if (concrete == st) {
                concrete = type_copy(ctx->arena, st);
            }
            /* Preserve type_args (bindings) on the concrete type for unification */
            concrete->struc.type_args = bindings;
            concrete->struc.type_arg_count = ntp;

            if (!bindings_contain_type_vars(bindings, ntp)) {
                /* Register and build concrete type */
                /* Use canonical C type name (already includes module/ns prefix) */
                const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                    st->struc.name, NULL,
                    bindings, ntp, sym->decl,
                    DECL_STRUCT, sym->type_params, ntp);
                concrete->struc.name = mangled;
                MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                if (mi && !mi->concrete_type) {
                    /* Deep copy: canonicalizing nested generic field names in place
                     * must not corrupt the live struct-literal type (e->type), whose
                     * fields a shallow copy would alias (item 7). */
                    Type *ct = type_deep_copy(ctx->arena, concrete);
                    mono_resolve_type_names(ctx->mono_table, ctx->arena, ctx->intern, ct);
                    mi->concrete_type = ct;
                }
            }
            e->type = concrete;
            /* Propagate stack provenance from any stack-pointer field so that
               returning/escaping the struct triggers the return-check rules. */
            for (int i = 0; i < e->struct_lit.field_count; i++) {
                FieldInit *fi = &e->struct_lit.fields[i];
                if (fi->value->prov == PROV_STACK &&
                    type_has_provenance(fi->value->type)) {
                    e->prov = PROV_STACK;
                    break;
                }
            }
            return e->type;
        }

        e->type = st;
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            FieldInit *fi = &e->struct_lit.fields[i];
            if (fi->value->prov == PROV_STACK &&
                type_has_provenance(fi->value->type)) {
                e->prov = PROV_STACK;
                break;
            }
        }
        return e->type;
    }

    case EXPR_TUPLE_LIT: {
        if (e->type) return e->type;
        int n = e->tuple_lit.elem_count;
        Type **elems = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)(n > 0 ? n : 1));
        bool err = false;
        for (int i = 0; i < n; i++) {
            Type *et = check_expr(ctx, e->tuple_lit.elems[i]);
            if (type_is_error(et)) {
                err = true;
            } else if (et->kind == TYPE_VOID || et->kind == TYPE_NEVER) {
                diag_error(e->tuple_lit.elems[i]->loc,
                    "tuple element %d has no value (type %s)", i, type_name(et));
                err = true;
            }
            elems[i] = et;
        }
        if (err) { e->type = type_error(); return e->type; }

        /* Build the synthesized tuple struct and canonicalize/register it.
         * resolve_type handles both concrete tuples (registered for codegen) and
         * generic tuples inside a generic body (registered later by monomorph). */
        Type *tup = type_tuple(ctx->arena, elems, n);
        tup = resolve_type(ctx, tup);
        e->type = tup;
        /* Propagate stack provenance from any stack-pointer element, mirroring
         * struct literals, so a tuple holding a stack pointer can't escape. */
        for (int i = 0; i < n; i++) {
            if (e->tuple_lit.elems[i]->prov == PROV_STACK &&
                type_has_provenance(e->tuple_lit.elems[i]->type)) {
                e->prov = PROV_STACK;
                break;
            }
        }
        return e->type;
    }

    case EXPR_TYPE_VAR_REF: {
        /* 'a used standalone (not as 'a.property) — error */
        diag_error(e->loc, "type variable '%s' cannot be used as a value",
            e->type_var_ref.name);
        e->type = type_error();
        return e->type;
    }

    case EXPR_FIELD: {
        /* Static type properties: int32.min, float64.nan, etc. */
        if (e->field.object->kind == EXPR_IDENT) {
            const char *codegen_cstr = NULL;
            Type *prop_type = resolve_type_property(
                e->field.object->ident.name, e->field.name, &codegen_cstr);
            if (prop_type == (Type *)-1) {
                /* Valid type name but unsupported property */
                diag_error(e->loc, "type '%s' has no property '%s'",
                    e->field.object->ident.name, e->field.name);
                e->type = type_error();
                return e->type;
            }
            if (prop_type) {
                e->field.codegen_name = codegen_cstr;
                e->field.is_type_property = true;
                e->type = prop_type;
                return e->type;
            }
        }

        /* Type variable property access: 'a.min, 'a.max, etc.
         * Defer resolution to monomorphization — validate property name now,
         * resolve concrete value in codegen with g_subst. */
        if (e->field.object->kind == EXPR_TYPE_VAR_REF) {
            const char *prop = e->field.name;
            const char *tv_name = e->field.object->type_var_ref.name;
            bool is_bits = (strcmp(prop, "bits") == 0);
            bool is_value_prop = (strcmp(prop, "min") == 0 ||
                                  strcmp(prop, "max") == 0 ||
                                  strcmp(prop, "nan") == 0 ||
                                  strcmp(prop, "inf") == 0 ||
                                  strcmp(prop, "neg_inf") == 0 ||
                                  strcmp(prop, "epsilon") == 0);
            if (!is_bits && !is_value_prop) {
                diag_error(e->loc, "unknown type property '%s'", prop);
                e->type = type_error();
                return e->type;
            }
            e->type = is_bits ? type_int32() : type_type_var(ctx->arena, tv_name);
            return e->type;
        }

        Type *obj_type = check_expr(ctx, e->field.object);
        if (type_is_error(obj_type)) { e->type = type_error(); return e->type; }

        /* Resolve the object as a module reference using resolved_sym from EXPR_IDENT.
         * EXPR_IDENT already determined what the name refers to — we never re-resolve.
         *
         * Three cases:
         * (a) EXPR_IDENT resolved to a module (resolved_sym->kind == DECL_MODULE)
         * (b) EXPR_IDENT resolved to a struct/union with a companion module
         * (c) EXPR_FIELD chain (a.b.member) — walk using root IDENT's resolved_sym
         *
         * If resolved_sym is NULL, the object was a local binding (parameter, let
         * variable) — skip module lookup entirely and go to field access. */
        Symbol *mod_sym = NULL;
        SymbolTable *mod_owner = NULL; /* members table containing mod_sym (for chain companion lookup) */
        if (e->field.object->kind == EXPR_IDENT) {
            Symbol *rsym = e->field.object->ident.resolved_sym;
            if (rsym && rsym->kind == DECL_MODULE)
                mod_sym = rsym;
            else if (e->field.object->ident.companion_module)
                mod_sym = e->field.object->ident.companion_module;
        }

        /* For nested module chains (a.b.member), walk from the root IDENT's
         * resolved_sym through submodules.  Only enter if root has a resolved_sym. */
        if (!mod_sym && e->field.object->kind == EXPR_FIELD) {
            Expr **chain = NULL;
            int depth = 0, chain_cap = 0;
            Expr *cur = e->field.object;
            while (cur->kind == EXPR_FIELD) {
                DA_APPEND(chain, depth, chain_cap, cur);
                cur = cur->field.object;
            }
            if (cur->kind == EXPR_IDENT && cur->ident.resolved_sym) {
                Symbol *root = cur->ident.resolved_sym;
                /* For companion pattern: root might be a struct/union, use its companion */
                if (root->kind != DECL_MODULE && cur->ident.companion_module)
                    root = cur->ident.companion_module;
                if (root->kind == DECL_MODULE && root->members) {
                    Symbol *walk = root;
                    SymbolTable *owner = NULL;
                    for (int k = depth - 1; k >= 0; k--) {
                        Symbol *next = symtab_lookup_kind(walk->members, chain[k]->field.name, DECL_MODULE);
                        if (!next) { walk = NULL; break; }
                        owner = walk->members;
                        walk = next;
                    }
                    if (walk) { mod_sym = walk; mod_owner = owner; }
                }
            }
            free(chain);
        }

        if (mod_sym && mod_sym->members) {
            Symbol *member = symtab_lookup(mod_sym->members, e->field.name);
            if (!member) {
                /* Companion union fallback: if a union shares the module's name,
                 * check if the field matches a variant of that union. This allows
                 * shape.circle(r) where "shape" is both a module and a union.
                 * Use the IDENT's resolved_sym (the union) rather than re-resolving. */
                Symbol *companion = NULL;
                if (e->field.object->kind == EXPR_IDENT &&
                    e->field.object->ident.resolved_sym &&
                    (e->field.object->ident.resolved_sym->kind == DECL_UNION ||
                     e->field.object->ident.resolved_sym->kind == DECL_STRUCT))
                    companion = e->field.object->ident.resolved_sym;
                /* For EXPR_FIELD chains (outer.shape.variant), the companion type
                 * is a sibling of the module in the parent's members table */
                if (!companion && mod_owner)
                    companion = symtab_lookup_kind(mod_owner, mod_sym->name, DECL_UNION);
                if (!companion && mod_owner)
                    companion = symtab_lookup_kind(mod_owner, mod_sym->name, DECL_STRUCT);
                if (companion && companion->type && companion->type->kind == TYPE_UNION) {
                    Type *ut = companion->type;
                    for (int v = 0; v < ut->unio.variant_count; v++) {
                        if (ut->unio.variants[v].name == e->field.name) {
                            e->type = ut;
                            e->field.is_variant_constructor = true;
                            return e->type;
                        }
                    }
                }
                diag_error(e->loc, "module '%s' has no member '%s'",
                    mod_sym->name, e->field.name);
                e->type = type_error();
                return e->type;
            }
            if (member->is_private && ctx->module_symtab != mod_sym->members) {
                diag_error(e->loc, "cannot access private member '%s' of module '%s'",
                    e->field.name, mod_sym->name);
                e->type = type_error();
                return e->type;
            }
            /* Submodule access: return void sentinel for further chaining */
            if (member->kind == DECL_MODULE) {
                e->type = type_void();
                return e->type;
            }
            /* Struct/union type member — handle generic instantiation if type args present */
            if (member->kind == DECL_STRUCT || member->kind == DECL_UNION) {
                if (member->is_generic && e->field.type_arg_count > 0) {
                    int ntp = member->type_param_count;
                    if (e->field.type_arg_count != ntp) {
                        diag_error(e->loc, "expected %d type argument(s), got %d",
                            ntp, e->field.type_arg_count);
                        e->type = type_error();
                        return e->type;
                    }
                    Type **bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
                    for (int k = 0; k < ntp; k++)
                        bindings[k] = resolve_type(ctx, e->field.type_args[k]);

                    Type *concrete = type_substitute(ctx->arena, member->type,
                        member->type_params, bindings, ntp);
                    if (concrete == member->type) {
                        concrete = type_copy(ctx->arena, member->type);
                    }
                    /* Set type_args for diagnostics */
                    if (member->kind == DECL_UNION) {
                        if (concrete->unio.type_arg_count == 0) {
                            concrete->unio.type_args = bindings;
                            concrete->unio.type_arg_count = ntp;
                        }
                    } else {
                        if (concrete->struc.type_arg_count == 0) {
                            concrete->struc.type_args = bindings;
                            concrete->struc.type_arg_count = ntp;
                        }
                    }
                    if (!bindings_contain_type_vars(bindings, ntp)) {
                        /* Use canonical C type name (already includes module/ns prefix) */
                        DeclKind dk = member->kind;
                        const char *canon_name = (dk == DECL_UNION)
                            ? member->type->unio.name : member->type->struc.name;
                        const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                            canon_name, NULL,
                            bindings, ntp, member->decl,
                            dk, member->type_params, ntp);
                        if (dk == DECL_UNION) {
                            concrete->unio.name = mangled;
                        } else {
                            concrete->struc.name = mangled;
                        }
                        MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                        if (mi) mi->concrete_type = concrete;
                    }
                    e->type = concrete;
                    return e->type;
                }
                e->type = member->type;
                return e->type;
            }
            /* Extern member: use raw C name */
            if (member->kind == DECL_EXTERN) {
                e->field.codegen_name = member->decl->ext.name;
                e->field.is_extern_const = (member->type && member->type->kind != TYPE_FUNC);
                e->type = member->type;
                return e->type;
            }
            /* Let member: set codegen_name */
            if (member->decl && member->decl->kind == DECL_LET) {
                e->field.codegen_name = member->decl->let.codegen_name;
            }
            if (!member->type && member->decl && member->decl->kind == DECL_LET) {
                /* On-demand type-check with cycle detection */
                for (OnDemandVisited *v = ctx->on_demand_visited; v; v = v->next) {
                    if (v->decl == member->decl) {
                        diag_error(e->loc, "circular dependency: '%s.%s' depends on itself through imports",
                            mod_sym->name, e->field.name);
                        e->type = type_error();
                        return e->type;
                    }
                }
                OnDemandVisited vis = { .decl = member->decl, .next = ctx->on_demand_visited };
                ctx->on_demand_visited = &vis;
                /* Reconstruct the full module scope chain so stub names in the
                 * callee's signature (e.g. `pcg_random*` inside
                 * std::random.pcg_random.next_uint32) resolve against the
                 * callee's declaration context, not the caller's. */
                SavedCtxScope saved;
                enter_module_scope_on_demand(ctx, mod_sym, &saved);
                check_decl_let(ctx, member->decl);
                restore_scope(ctx, &saved);
                ctx->on_demand_visited = vis.next;
            }
            if (!member->type) {
                diag_error(e->loc, "use of '%s.%s' before its type is resolved",
                    mod_sym->name, e->field.name);
                e->type = type_error();
                return e->type;
            }
            e->type = member->type;
            return e->type;
        }

        /* If the object is an IDENT referencing a union type, this is variant construction.
         * Use resolved_sym from EXPR_IDENT — no re-resolution needed. */
        if (e->field.object->kind == EXPR_IDENT && obj_type->kind == TYPE_UNION) {
            Symbol *sym = e->field.object->ident.resolved_sym;
            if (sym && sym->kind == DECL_UNION && sym->type && sym->type->kind == TYPE_UNION) {
                e->field.is_variant_constructor = true;
                if (sym->is_generic) {
                    /* Generic union no-payload variant: require explicit type args */
                    int ntp = sym->type_param_count;
                    if (e->field.type_arg_count == 0) {
                        diag_error(e->loc,
                            "generic union '%s' requires explicit type arguments: %s<...>.%s",
                            sym->name, sym->name, e->field.name);
                        e->type = type_error();
                        return e->type;
                    }
                    if (e->field.type_arg_count != ntp) {
                        diag_error(e->loc,
                            "expected %d type argument(s), got %d",
                            ntp, e->field.type_arg_count);
                        e->type = type_error();
                        return e->type;
                    }
                    Type **bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
                    for (int k = 0; k < ntp; k++)
                        bindings[k] = resolve_type(ctx, e->field.type_args[k]);

                    Type *concrete = type_substitute(ctx->arena, sym->type,
                        sym->type_params, bindings, ntp);
                    if (concrete == sym->type) {
                        concrete = type_copy(ctx->arena, sym->type);
                    }
                    /* Ensure type_args are set for diagnostics (template may lack them) */
                    if (concrete->unio.type_arg_count == 0) {
                        concrete->unio.type_args = bindings;
                        concrete->unio.type_arg_count = ntp;
                    }

                    if (!bindings_contain_type_vars(bindings, ntp)) {
                        /* Use canonical C type name (already includes module/ns prefix) */
                        const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                            sym->type->unio.name, NULL,
                            bindings, ntp, sym->decl,
                            DECL_UNION, sym->type_params, ntp);
                        concrete->unio.name = mangled;
                        MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                        if (mi) mi->concrete_type = concrete;
                    }
                    e->type = concrete;
                    return e->type;
                }
                e->type = sym->type;
                return e->type;
            }
        }

        /* If the object resolved to a union type (e.g., module.UnionType.Variant) */
        if (obj_type->kind == TYPE_UNION) {
            e->field.is_variant_constructor = true;
            /* For module-qualified generic variants (m.union_name<Types>.variant),
             * the parser puts type args on the outer FIELD node.  Instantiate
             * the generic union if type args are present. */
            if (e->field.type_arg_count > 0 && obj_type->unio.variant_count > 0) {
                /* Find the symbol for this union to get type params */
                Symbol *usym = resolve_symbol_kind(ctx, obj_type->unio.name, DECL_UNION);
                if (!usym) usym = resolve_symbol(ctx, obj_type->unio.name);
                if (usym && usym->is_generic) {
                    int ntp = usym->type_param_count;
                    if (e->field.type_arg_count != ntp) {
                        diag_error(e->loc, "expected %d type argument(s), got %d",
                            ntp, e->field.type_arg_count);
                        e->type = type_error();
                        return e->type;
                    }
                    Type **bindings = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)ntp);
                    for (int k = 0; k < ntp; k++)
                        bindings[k] = resolve_type(ctx, e->field.type_args[k]);
                    Type *concrete = type_substitute(ctx->arena, usym->type,
                        usym->type_params, bindings, ntp);
                    if (concrete == usym->type)
                        concrete = type_copy(ctx->arena, usym->type);
                    if (concrete->unio.type_arg_count == 0) {
                        concrete->unio.type_args = bindings;
                        concrete->unio.type_arg_count = ntp;
                    }
                    if (!bindings_contain_type_vars(bindings, ntp)) {
                        const char *mangled = mono_register(ctx->mono_table, ctx->arena,
                            ctx->intern, usym->type->unio.name, NULL,
                            bindings, ntp, usym->decl,
                            DECL_UNION, usym->type_params, ntp);
                        concrete->unio.name = mangled;
                        MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                        if (mi && !mi->concrete_type) mi->concrete_type = concrete;
                    }
                    e->type = concrete;
                    return e->type;
                }
            }
            e->type = obj_type;
            return e->type;
        }

        obj_type = resolve_type(ctx, obj_type);

        /* Slice .len and .ptr fields */
        if (obj_type->kind == TYPE_SLICE) {
            if (strcmp(e->field.name, "len") == 0) {
                e->type = type_int64();
                return e->type;
            }
            if (strcmp(e->field.name, "ptr") == 0) {
                Type *ptr_type = type_pointer(ctx->arena, obj_type->slice.elem);
                if (obj_type->is_const) ptr_type->is_const = true;
                e->type = ptr_type;
                e->prov = e->field.object->prov;
                return e->type;
            }
        }

        /* Option .is_some and .is_none fields */
        if (obj_type->kind == TYPE_OPTION) {
            if (strcmp(e->field.name, "is_some") == 0 || strcmp(e->field.name, "is_none") == 0) {
                e->type = type_bool();
                return e->type;
            }
            diag_error(e->loc, "option type has no field '%s'", e->field.name);
            e->type = type_error();
            return e->type;
        }

        /* Normal struct field access */
        if (obj_type->kind != TYPE_STRUCT) {
            diag_error(e->loc, "field access on non-struct type %s", type_name(obj_type));
            e->type = type_error();
            return e->type;
        }
        if (obj_type->struc.is_tuple) {
            diag_error(e->loc, "tuple elements are accessed by index (e.g. t[0]), "
                "not by field name");
            e->type = type_error();
            return e->type;
        }
        for (int i = 0; i < obj_type->struc.field_count; i++) {
            if (obj_type->struc.fields[i].name == e->field.name) {
                Type *ft = resolve_type(ctx, obj_type->struc.fields[i].type);
                if (ft->kind == TYPE_FIXED_ARRAY) {
                    /* Lvalue check: object must have stable storage */
                    if (!is_lvalue_expr(e->field.object)) {
                        diag_error(e->loc,
                            "cannot access inline array field '%s' on a temporary; "
                            "bind the struct to a variable first",
                            e->field.name);
                        e->type = type_error();
                        return e->type;
                    }
                    e->field.fixed_array_type = ft;
                    e->type = type_slice(ctx->arena, ft->fixed_array.elem);
                    /* Slice view of fixed-array on stack struct → PROV_STACK */
                    e->prov = e->field.object->prov;
                    if (e->prov == PROV_UNKNOWN)
                        e->prov = PROV_STACK;
                } else {
                    e->type = ft;
                    /* Propagate provenance from the struct so reading a pointer
                       field out of a stack-provenance struct stays tainted. */
                    if (type_has_provenance(ft))
                        e->prov = e->field.object->prov;
                }
                return e->type;
            }
        }
        diag_error(e->loc, "struct '%s' has no field '%s'",
            type_name(obj_type), e->field.name);
        e->type = type_error();
        return e->type;
    }

    case EXPR_DEREF_FIELD: {
        Type *obj_type = check_expr(ctx, e->field.object);
        if (type_is_error(obj_type)) { e->type = type_error(); return e->type; }
        bool through_const = obj_type->is_const;
        if (obj_type->kind != TYPE_POINTER) {
            diag_error(e->loc, "-> requires pointer type, got %s", type_name(obj_type));
            e->type = type_error();
            return e->type;
        }
        Type *pointee = resolve_type(ctx, obj_type->pointer.pointee);
        if (pointee->kind != TYPE_STRUCT) {
            diag_error(e->loc, "-> requires pointer to struct, got pointer to %s", type_name(pointee));
            e->type = type_error();
            return e->type;
        }
        for (int i = 0; i < pointee->struc.field_count; i++) {
            if (pointee->struc.fields[i].name == e->field.name) {
                Type *ft = resolve_type(ctx, pointee->struc.fields[i].type);
                if (ft->kind == TYPE_FIXED_ARRAY) {
                    e->field.fixed_array_type = ft;
                    e->type = type_slice(ctx->arena, ft->fixed_array.elem);
                    if (through_const) e->type = type_make_const(ctx->arena, e->type);
                    e->prov = e->field.object->prov;
                } else {
                    e->type = ft;
                    if (through_const) e->type = type_make_const(ctx->arena, e->type);
                    /* Propagate provenance from the pointed-to struct. */
                    if (type_has_provenance(ft))
                        e->prov = e->field.object->prov;
                }
                return e->type;
            }
        }
        diag_error(e->loc, "struct '%s' has no field '%s'",
            type_name(pointee), e->field.name);
        e->type = type_error();
        return e->type;
    }

    case EXPR_INDEX: {
        Type *obj_type = check_expr(ctx, e->index.object);
        Type *idx_type = check_expr(ctx, e->index.index);
        if (reject_unresolved_recursive_value(e->index.object) ||
            reject_unresolved_recursive_value(e->index.index)) { e->type = type_error(); return e->type; }
        if (type_is_error(obj_type) || type_is_error(idx_type)) { e->type = type_error(); return e->type; }

        /* Tuple indexing: the index must be a non-negative integer literal in
         * [0, N). The element type is selected at compile time — heterogeneous
         * elements mean a runtime index has no single type — so there is no
         * bounds check emitted (the bound is proven here). */
        if (obj_type->kind == TYPE_STRUCT && obj_type->struc.is_tuple) {
            Expr *ix = e->index.index;
            if (ix->kind != EXPR_INT_LIT) {
                diag_error(e->loc, "tuple index must be an integer literal");
                e->type = type_error();
                return e->type;
            }
            /* A negated literal (e.g. -1) folds to a large unsigned value; report
             * it as negative rather than as a huge out-of-range index. */
            if (ix->int_lit.lit_type && type_is_signed(ix->int_lit.lit_type) &&
                (int64_t)ix->int_lit.value < 0) {
                diag_error(e->loc, "tuple index cannot be negative");
                e->type = type_error();
                return e->type;
            }
            uint64_t idx = ix->int_lit.value;
            if (ix->int_lit.out_of_range ||
                idx >= (uint64_t)obj_type->struc.field_count) {
                diag_error(e->loc, "tuple index %llu is out of range for %s (has %d elements)",
                    (unsigned long long)idx, type_name(obj_type), obj_type->struc.field_count);
                e->type = type_error();
                return e->type;
            }
            e->type = obj_type->struc.fields[(int)idx].type;
            if (type_has_provenance(e->type))
                e->prov = e->index.object->prov;
            return e->type;
        }

        if (!type_is_integer(idx_type)) {
            diag_error(e->loc, "index must be integer, got %s", type_name(idx_type));
            e->type = type_error();
            return e->type;
        }

        if (obj_type->kind == TYPE_SLICE) {
            e->type = obj_type->slice.elem;
            if (type_has_provenance(e->type))
                e->prov = e->index.object->prov;
            return e->type;
        }
        if (obj_type->kind == TYPE_POINTER) {
            e->type = obj_type->pointer.pointee;
            if (type_has_provenance(e->type))
                e->prov = e->index.object->prov;
            return e->type;
        }
        diag_error(e->loc, "indexing requires slice or pointer, got %s", type_name(obj_type));
        e->type = type_error();
        return e->type;
    }

    case EXPR_SLICE: {
        Type *obj_type = check_expr(ctx, e->slice.object);
        if (type_is_error(obj_type)) { e->type = type_error(); return e->type; }
        if (e->slice.lo) {
            Type *lo_type = check_expr(ctx, e->slice.lo);
            if (!type_is_error(lo_type) && !type_is_integer(lo_type)) {
                diag_error(e->loc, "slice index must be integer");
                e->type = type_error();
                return e->type;
            }
        }
        if (e->slice.hi) {
            Type *hi_type = check_expr(ctx, e->slice.hi);
            if (!type_is_error(hi_type) && !type_is_integer(hi_type)) {
                diag_error(e->loc, "slice index must be integer");
                e->type = type_error();
                return e->type;
            }
        }
        if (obj_type->kind != TYPE_SLICE) {
            diag_error(e->loc, "subslice requires slice type, got %s", type_name(obj_type));
            e->type = type_error();
            return e->type;
        }
        e->type = obj_type;
        e->prov = e->slice.object->prov;
        return e->type;
    }

    case EXPR_ARRAY_LIT: {
        /* Slice literal: type[length] { elems... } → creates a slice (the
         * EXPR_ARRAY_LIT node name reflects the backing-array mechanism). */
        /* The length expression must be an integer */
        Type *size_type = check_expr(ctx, e->array_lit.size_expr);
        if (!type_is_error(size_type) && !type_is_integer(size_type)) {
            diag_error(e->loc, "slice literal length must be integer, got %s", type_name(size_type));
            e->type = type_error();
            return e->type;
        }
        /* Length must be a compile-time constant (integer literal) */
        if (e->array_lit.size_expr->kind != EXPR_INT_LIT) {
            diag_error(e->array_lit.size_expr->loc,
                "slice literal length must be a compile-time constant");
            e->type = type_error();
            return e->type;
        }
        /* Element count must match the declared length exactly. The empty
         * form `{ }` (elem_count == 0) zero-initializes all elements and is
         * always allowed; an explicit element list must be exhaustive. Without
         * this check, a short list leaves elements uninitialized and an
         * over-long list writes past the alloca/malloc backing buffer. */
        uint64_t declared_size = e->array_lit.size_expr->int_lit.value;
        if (e->array_lit.elem_count > 0 &&
            (uint64_t) e->array_lit.elem_count != declared_size) {
            diag_error(e->loc,
                "slice literal has %d element%s but declared length is %" PRIu64
                "; the element list must be exhaustive (or use `{ }` to zero-initialize)",
                e->array_lit.elem_count,
                e->array_lit.elem_count == 1 ? "" : "s",
                declared_size);
            e->type = type_error();
            return e->type;
        }
        /* Type-check elements */
        Type *elem_type = resolve_type(ctx, e->array_lit.elem_type);
        e->array_lit.elem_type = elem_type;
        bool elem_error = false;
        for (int i = 0; i < e->array_lit.elem_count; i++) {
            Type *et = check_expr(ctx, e->array_lit.elems[i]);
            if (type_is_error(et)) { elem_error = true; continue; }
            if (!type_eq(et, elem_type)) {
                diag_error(e->array_lit.elems[i]->loc,
                    "slice literal element type mismatch: expected %s, got %s",
                    type_name(elem_type), type_name(et));
                elem_error = true;
            }
        }
        if (elem_error) { e->type = type_error(); return e->type; }
        e->type = type_slice(ctx->arena, elem_type);
        e->prov = PROV_STACK;
        return e->type;
    }

    case EXPR_SLICE_LIT: {
        /* Slice literal: T[] { ptr = expr, len = expr } */
        Type *elem_type = resolve_type(ctx, e->slice_lit.elem_type);
        e->slice_lit.elem_type = elem_type;
        Type *pt = check_expr(ctx, e->slice_lit.ptr_expr);
        Type *lt = check_expr(ctx, e->slice_lit.len_expr);
        if (type_is_error(pt) || type_is_error(lt)) {
            e->type = type_error();
            return e->type;
        }
        /* ptr must be T* (pointer to element type) */
        Type *expected_ptr = type_pointer(ctx->arena, elem_type);
        if (!type_eq(pt, expected_ptr) && !(pt->kind == TYPE_POINTER && type_eq(pt->pointer.pointee, elem_type))) {
            /* Also allow const T* — result will be const slice */
            if (!(pt->kind == TYPE_POINTER && pt->is_const && type_eq(pt->pointer.pointee, elem_type))) {
                diag_error(e->slice_lit.ptr_expr->loc,
                    "slice ptr field: expected %s*, got %s",
                    type_name(elem_type), type_name(pt));
            }
        }
        /* len must be int64 (or widenable to int64) */
        Type *len_type = type_int64();
        Expr *orig_len = e->slice_lit.len_expr;  /* before any widening wrap */
        bool len_type_ok = true;
        if (!type_eq(lt, len_type)) {
            if (type_can_widen(lt, len_type)) {
                e->slice_lit.len_expr = wrap_widen(ctx->arena, e->slice_lit.len_expr, len_type);
            } else {
                diag_error(e->slice_lit.len_expr->loc,
                    "slice len field: expected int64, got %s", type_name(lt));
                len_type_ok = false;
            }
        }
        /* A slice's length must be non-negative.  The index/subslice bounds
         * checks fuse the negative-index test into an unsigned compare
         * ((uint64_t)idx >= (uint64_t)len), which a negative len defeats — it
         * casts to an enormous value, so every later access slips through.
         * Reject a statically-negative literal here; flag a provably
         * non-negative len so codegen can skip the runtime guard.  An unsigned
         * source is always >= 0; a non-constant signed len is guarded at
         * construction in codegen; a const-context computed negative is caught
         * in const_fold_expr (no runtime guard there). */
        if (len_type_ok && type_is_integer(lt)) {
            if (!type_is_signed(lt)) {
                e->slice_lit.len_nonneg = true;
            } else if (orig_len->kind == EXPR_INT_LIT && !orig_len->int_lit.out_of_range) {
                if ((int64_t)orig_len->int_lit.value < 0)
                    diag_error(orig_len->loc, "slice literal length cannot be negative");
                else
                    e->slice_lit.len_nonneg = true;
            }
        }
        Type *slice_type = type_slice(ctx->arena, elem_type);
        /* If ptr is const, result is const slice */
        if (pt->is_const) {
            slice_type = type_make_const(ctx->arena, slice_type);
        }
        e->type = slice_type;
        e->prov = e->slice_lit.ptr_expr->prov;
        return e->type;
    }

    case EXPR_SOME: {
        Type *inner = check_expr(ctx, e->some_expr.value);
        if (reject_unresolved_recursive_value(e->some_expr.value)) { e->type = type_error(); return e->type; }
        if (type_is_error(inner)) { e->type = type_error(); return e->type; }
        e->type = type_option(ctx->arena, inner);
        e->prov = e->some_expr.value->prov;
        /* Null-sentinel options (T*?, any*?, cstr?) represent none as a null
         * pointer, so some(p) over a null p is indistinguishable from none.
         * Reject a provably-null payload outright; a not-provably-non-null
         * payload is guarded at construction in codegen (a generic 'a? whose
         * concrete type is a pointer is likewise guarded there). */
        if (inner->kind == TYPE_POINTER || inner->kind == TYPE_ANY_PTR) {
            if (ptr_value_provably_null(e->some_expr.value))
                diag_error(e->loc,
                    "some() of a null pointer is indistinguishable from none "
                    "(pointer options use null as the none sentinel)");
        }
        return e->type;
    }

    case EXPR_LOOP: {
        Scope *inner = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = inner;

        /* Set up break type tracking */
        Type *break_type = NULL;
        Type **saved_break = ctx->loop_break_type;
        bool saved_in_for = ctx->in_for;
        ctx->loop_break_type = &break_type;
        ctx->in_for = false;

        pretaint_loop_body(ctx->scope, e->loop_expr.body, e->loop_expr.body_count);
        check_block(ctx, e->loop_expr.body, e->loop_expr.body_count);

        ctx->scope = saved;
        ctx->loop_break_type = saved_break;
        ctx->in_for = saved_in_for;

        /* Loop type comes from break values; void if no break-with-value */
        e->type = break_type ? break_type : type_void();
        return e->type;
    }

    case EXPR_FOR: {
        /* Type check the iterator/range */
        Type *iter_type = check_expr(ctx, e->for_expr.iter);

        Scope *inner = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = inner;

        if (type_is_error(iter_type)) {
            /* Add loop binding(s) with error type so body can still be checked */
            if (e->for_expr.var_pattern)
                for_pattern_bind_error(ctx, e->for_expr.var_pattern);
            else
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            if (e->for_expr.index_var)
                scope_add(ctx->scope, e->for_expr.index_var, e->for_expr.index_var, type_int64(), false);
        } else if (e->for_expr.range_end) {
            /* Range iteration: for i in lo..hi */
            Type *end_type = check_expr(ctx, e->for_expr.range_end);
            if (e->for_expr.var_pattern) {
                /* A range produces integers; there is nothing to destructure. */
                diag_error(e->loc, "cannot destructure a range element — ranges produce integers");
                for_pattern_bind_error(ctx, e->for_expr.var_pattern);
            } else if (type_is_error(end_type)) {
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            } else if (!type_is_integer(iter_type) || !type_is_integer(end_type)) {
                diag_error(e->loc, "range bounds must be integer types");
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            } else {
                /* Unify the endpoints by widening rules — not by TypeKind
                 * ordinal. The common type becomes the loop variable's type,
                 * and both endpoints are widened to it so the emitted C loop
                 * variable, `<` test, and `++` all share one consistent type.
                 * Mixed pairs with no common type (e.g. uint32..int32, or any
                 * implicit isize/usize mix) are rejected here, the same as
                 * `+`/`<` would reject them. */
                Type *var_type = type_common_numeric(iter_type, end_type);
                if (!var_type) {
                    diag_error(e->loc,
                        "range endpoints have incompatible types: %s and %s",
                        type_name(iter_type), type_name(end_type));
                    var_type = type_error();
                } else {
                    if (!type_eq(iter_type, var_type))
                        e->for_expr.iter = wrap_widen(ctx->arena, e->for_expr.iter, var_type);
                    if (!type_eq(end_type, var_type))
                        e->for_expr.range_end = wrap_widen(ctx->arena, e->for_expr.range_end, var_type);
                }
                scope_add(ctx->scope, e->for_expr.var,
                    c_safe_ident(ctx->intern, e->for_expr.var), var_type, false);
            }
        } else {
            /* Collection iteration: for x in slice */
            if (iter_type->kind == TYPE_SLICE) {
                bind_for_element(ctx, e, iter_type->slice.elem);
                if (e->for_expr.index_var) {
                    scope_add(ctx->scope, e->for_expr.index_var,
                        c_safe_ident(ctx->intern, e->for_expr.index_var), type_int64(), false);
                }
            } else {
                diag_error(e->loc, "for-in requires slice or range, got %s", type_name(iter_type));
                if (e->for_expr.var_pattern)
                    for_pattern_bind_error(ctx, e->for_expr.var_pattern);
                else
                    scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            }
        }

        /* Save/set loop context for break checking */
        Type **saved_break = ctx->loop_break_type;
        bool saved_in_for = ctx->in_for;
        Type *break_type = NULL;
        ctx->loop_break_type = &break_type;
        ctx->in_for = true;

        pretaint_loop_body(ctx->scope, e->for_expr.body, e->for_expr.body_count);
        check_block(ctx, e->for_expr.body, e->for_expr.body_count);

        ctx->scope = saved;
        ctx->loop_break_type = saved_break;
        ctx->in_for = saved_in_for;

        e->type = type_void();
        return e->type;
    }

    case EXPR_BREAK: {
        if (!ctx->loop_break_type) {
            diag_error(e->loc, "break outside of loop");
            e->type = type_void();
            return e->type;
        }
        if (e->break_expr.value) {
            if (ctx->in_for) {
                diag_error(e->loc, "break with value is not allowed in for loops");
                e->type = type_void();
                return e->type;
            }
            Type *vt = check_expr(ctx, e->break_expr.value);
            if (!type_is_error(vt)) {
                if (*ctx->loop_break_type == NULL) {
                    *ctx->loop_break_type = vt;
                } else if (!type_eq(*ctx->loop_break_type, vt)) {
                    diag_error(e->loc, "break type mismatch: expected %s, got %s",
                        type_name(*ctx->loop_break_type), type_name(vt));
                }
            }
        }
        e->type = type_never();
        return e->type;
    }

    case EXPR_CONTINUE: {
        if (!ctx->loop_break_type) {
            diag_error(e->loc, "continue outside of loop");
        }
        e->type = type_never();
        return e->type;
    }

    case EXPR_MATCH:
        return check_match(ctx, e);

    case EXPR_SIZEOF: {
        Type *ty = resolve_type(ctx, e->sizeof_expr.target);
        e->sizeof_expr.target = ty;
        e->type = type_int64();
        return e->type;
    }

    case EXPR_ALIGNOF: {
        Type *ty = resolve_type(ctx, e->alignof_expr.target);
        e->alignof_expr.target = ty;
        e->type = type_int64();
        return e->type;
    }

    case EXPR_DEFAULT: {
        Type *ty = resolve_type(ctx, e->default_expr.target);
        e->default_expr.target = ty;
        e->type = ty;
        return e->type;
    }

    case EXPR_FREE: {
        Type *ot = check_expr(ctx, e->free_expr.operand);
        if (type_is_error(ot)) {
            e->type = type_void();
            return e->type;
        }
        if (ot->kind != TYPE_POINTER && ot->kind != TYPE_SLICE &&
            ot->kind != TYPE_ANY_PTR) {
            diag_error(e->loc, "free requires pointer or slice, got %s", type_name(ot));
        }
        /* Escape analysis: reject free on non-heap memory */
        if (e->free_expr.operand->prov == PROV_STATIC) {
            diag_error(e->loc, "cannot free static memory (string literal)");
        } else if (e->free_expr.operand->prov == PROV_STACK) {
            diag_error(e->loc, "cannot free stack-allocated memory");
        }
        if (ot->is_const) {
            diag_error(e->loc, "cannot free const pointer/slice");
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_ASSERT: {
        Type *ct = check_expr(ctx, e->assert_expr.condition);
        if (reject_unresolved_recursive_value(e->assert_expr.condition)) { e->type = type_void(); return e->type; }
        if (!type_is_error(ct) && ct->kind != TYPE_BOOL) {
            diag_error(e->loc, "assert condition must be bool, got %s", type_name(ct));
        }
        if (e->assert_expr.message) {
            Type *mt = check_expr(ctx, e->assert_expr.message);
            if (!type_is_error(mt)) {
                if (mt->kind != TYPE_SLICE || mt->slice.elem->kind != TYPE_UINT8) {
                    diag_error(e->loc, "assert message must be str, got %s", type_name(mt));
                }
            }
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_ATOMIC_LOAD: {
        Type *pt = check_expr(ctx, e->atomic_load.ptr);
        if (type_is_error(pt)) { e->type = type_error(); return e->type; }
        if (!atomic_pointee_ok(pt, e->loc, "atomic_load_acquire")) {
            e->type = type_error();
            return e->type;
        }
        e->type = pt->pointer.pointee;
        return e->type;
    }

    case EXPR_ATOMIC_STORE: {
        Type *pt = check_expr(ctx, e->atomic_store.ptr);
        Type *vt = check_expr(ctx, e->atomic_store.value);
        e->type = type_void();
        if (type_is_error(pt) || type_is_error(vt)) return e->type;
        if (!atomic_pointee_ok(pt, e->loc, "atomic_store_release")) return e->type;
        if (pt->is_const) {
            diag_error(e->loc, "cannot atomic_store_release through const pointer");
            return e->type;
        }
        Type *cell = pt->pointer.pointee;
        if (!type_eq(cell, vt)) {
            if (type_can_widen(vt, cell)) {
                e->atomic_store.value = wrap_widen(ctx->arena, e->atomic_store.value, cell);
            } else {
                diag_error(e->loc, "atomic_store_release value type mismatch: %s vs %s",
                    type_name(cell), type_name(vt));
            }
        }
        return e->type;
    }

    case EXPR_DEFER: {
        check_expr(ctx, e->defer_expr.value);
        if (expr_contains_control_flow(e->defer_expr.value)) {
            diag_error(e->loc, "deferred expression must not contain return, break, or continue");
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_ALLOC: {
        if (e->alloc_expr.is_stack) {
            /* alloca(...) — dynamic stack. Same shapes as alloc but the result is
             * the value directly (no option, no failure sentinel) and it is tagged
             * PROV_STACK so escape analysis forbids returning it or storing it in
             * heap/static memory, exactly like today's implicit stack temporaries. */
            if (e->alloc_expr.alloc_type) {
                Type *ty = resolve_type(ctx, e->alloc_expr.alloc_type);
                e->alloc_expr.alloc_type = ty;
                if (e->alloc_expr.size_expr) {
                    Type *st = check_expr(ctx, e->alloc_expr.size_expr);
                    if (type_is_error(st)) { e->type = type_error(); return e->type; }
                    if (!type_is_integer(st)) {
                        diag_error(e->loc, "alloca buffer size must be integer, got %s",
                                   type_name(st));
                        e->type = type_error();
                        return e->type;
                    }
                    e->type = e->alloc_expr.alloc_raw
                        ? type_pointer(ctx->arena, ty)   /* alloca(T, N) → T* */
                        : type_slice(ctx->arena, ty);    /* alloca(T[n] { }) → T[] */
                } else {
                    e->type = type_pointer(ctx->arena, ty);  /* alloca(T) → T* */
                }
                e->prov = PROV_STACK;
                return e->type;
            }
            /* alloca(expr) — only an interpolated string (str/cstr) or slice literal
             * makes sense as a runtime-sized stack temporary. */
            Expr *ie = e->alloc_expr.init_expr;
            Type *t = check_expr(ctx, ie);
            if (type_is_error(t)) { e->type = type_error(); return e->type; }
            if (ie->kind == EXPR_INTERP_STRING || ie->kind == EXPR_ARRAY_LIT ||
                t->kind == TYPE_SLICE || is_cstr_type(t)) {
                Type *rt = t;
                if (rt->is_const) {
                    rt = arena_alloc(ctx->arena, sizeof(Type));
                    *rt = *t;
                    rt->is_const = false;
                }
                e->type = rt;
                e->prov = PROV_STACK;
                return e->type;
            }
            diag_error(e->loc,
                "alloca(expr) requires an interpolated string or slice literal; "
                "use alloca(T, n) or alloca(T[n] {}) for an uninitialized buffer");
            e->type = type_error();
            return e->type;
        }
        if (e->alloc_expr.alloc_type) {
            Type *ty = resolve_type(ctx, e->alloc_expr.alloc_type);
            e->alloc_expr.alloc_type = ty;

            if (e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
                /* alloc(T, N) → T*? (raw buffer) */
                Type *st = check_expr(ctx, e->alloc_expr.size_expr);
                if (type_is_error(st)) { e->type = type_error(); return e->type; }
                if (!type_is_integer(st)) {
                    diag_error(e->loc, "alloc buffer size must be integer, got %s", type_name(st));
                    e->type = type_error();
                    return e->type;
                }
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, ty));
                e->prov = PROV_HEAP;
            } else if (e->alloc_expr.size_expr) {
                /* alloc(T[N]) → T[]? */
                Type *st = check_expr(ctx, e->alloc_expr.size_expr);
                if (type_is_error(st)) { e->type = type_error(); return e->type; }
                if (!type_is_integer(st)) {
                    diag_error(e->loc, "alloc slice length must be integer, got %s", type_name(st));
                    e->type = type_error();
                    return e->type;
                }
                e->type = type_option(ctx->arena, type_slice(ctx->arena, ty));
                e->prov = PROV_HEAP;
            } else {
                /* alloc(T) → T*? */
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, ty));
                e->prov = PROV_HEAP;
            }
        } else {
            /* alloc(expr) — a bare identifier may be a type name rather than
             * a variable.  Check the variable scope first; if not found, try
             * resolving as a type so that alloc(my_struct) works. */
            if (e->alloc_expr.init_expr->kind == EXPR_IDENT) {
                const char *name = e->alloc_expr.init_expr->ident.name;
                Type *var_type = scope_lookup_capture(ctx->scope, name,
                    NULL, NULL, NULL, NULL);
                if (!var_type) {
                    Symbol *sym = global_lookup(ctx->symtab, name, ctx->current_ns);
                    if (sym && sym->kind == DECL_LET) var_type = sym->type;
                }
                if (!var_type) {
                    /* Not a variable — try to resolve as a type name */
                    Type *stub = arena_alloc(ctx->arena, sizeof(Type));
                    memset(stub, 0, sizeof(Type));
                    stub->kind = TYPE_STUB;
                    stub->stub.name = name;
                    Type *ty = resolve_type(ctx, stub);
                    if (ty != stub) {
                        /* Resolved as type — treat as alloc(T) */
                        e->alloc_expr.alloc_type = ty;
                        e->alloc_expr.init_expr = NULL;
                        e->type = type_option(ctx->arena, type_pointer(ctx->arena, ty));
                        e->prov = PROV_HEAP;
                        return e->type;
                    }
                    /* Neither variable nor type — fall through to check_expr
                     * which will report the undeclared identifier error */
                }
            }
            /* alloc(expr) — only specific literal/slice forms allowed */
            Type *t = check_expr(ctx, e->alloc_expr.init_expr);
            if (type_is_error(t)) { e->type = type_error(); return e->type; }

            Expr *ie = e->alloc_expr.init_expr;

            /* alloc(c"literal") → cstr? */
            if (ie->kind == EXPR_CSTRING_LIT) {
                Type *rt = type_pointer(ctx->arena, type_uint8());
                e->type = type_option(ctx->arena, rt);
                e->prov = PROV_HEAP;
                return e->type;
            }
            /* alloc(c"interp %d{x}") → cstr? */
            if (ie->kind == EXPR_INTERP_STRING && ie->interp_string.is_cstr) {
                Type *rt = type_pointer(ctx->arena, type_uint8());
                e->type = type_option(ctx->arena, rt);
                e->prov = PROV_HEAP;
                return e->type;
            }
            /* alloc((cstr) str) → cstr? — heap copy of a str→cstr conversion (the
             * heap home for an unbounded (cstr) cast). The bounded (cstr[N]) form
             * already has its own home, so only the unbounded cast qualifies. */
            if (ie->kind == EXPR_CAST && ie->cast.buffer_size == 0 && is_cstr_type(t)) {
                Type *rt = type_pointer(ctx->arena, type_uint8());
                e->type = type_option(ctx->arena, rt);
                e->prov = PROV_HEAP;
                return e->type;
            }

            /* Escape analysis: check for stack pointers in heap-allocated struct */
            if (ie->kind == EXPR_STRUCT_LIT) {
                Type *st_type = ie->type;
                for (int i = 0; i < ie->struct_lit.field_count; i++) {
                    FieldInit *fi = &ie->struct_lit.fields[i];
                    if (fi->value->prov == PROV_STACK && type_has_provenance(fi->value->type)) {
                        /* Fixed-array fields copy data inline — stack provenance is safe */
                        bool is_fixed = false;
                        if (st_type && st_type->kind == TYPE_STRUCT) {
                            for (int f = 0; f < st_type->struc.field_count; f++) {
                                if (st_type->struc.fields[f].name == fi->name &&
                                    st_type->struc.fields[f].type->kind == TYPE_FIXED_ARRAY) {
                                    is_fixed = true;
                                    break;
                                }
                            }
                        }
                        if (!is_fixed) {
                            diag_error(fi->value->loc,
                                "cannot store stack-allocated %s in heap-allocated struct",
                                type_name(fi->value->type));
                        }
                    }
                }
            }

            if (t->kind == TYPE_SLICE) {
                /* alloc(slice_expr) → T[]? (deep-copy to heap) */
                /* Also handles alloc("str_lit"), alloc("interp %d{x}"), alloc(slice_var) */
                Type *rt = t;
                if (rt->is_const) {
                    rt = arena_alloc(ctx->arena, sizeof(Type));
                    *rt = *t;
                    rt->is_const = false;
                }
                e->type = type_option(ctx->arena, rt);
            } else if (ie->kind == EXPR_STRUCT_LIT) {
                /* alloc(struct_literal) → T*? */
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, t));
            } else if (t->kind == TYPE_UNION) {
                /* alloc(union_variant) → T*? */
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, t));
            } else {
                diag_error(e->loc,
                    "alloc(expr) requires a literal or slice expression; "
                    "use alloc(%s) for uninitialized heap allocation",
                    type_name(t));
                e->type = type_error();
                return e->type;
            }
            e->prov = PROV_HEAP;
        }
        return e->type;
    }

    case EXPR_INTERP_STRING: {
        bool any_seg_err = false;
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            InterpSegment *seg = &e->interp_string.segments[i];
            if (seg->is_literal) continue;

            /* `%T` reflects the expression's type at compile time and never emits
             * it as a value, so allow a generic function name here. */
            if (seg->conversion == 'T') ctx->in_reflection_position = true;
            Type *et = check_expr(ctx, seg->expr);
            if (type_is_error(et)) { any_seg_err = true; continue; }
            et = resolve_type(ctx, et);

            char conv = seg->conversion;
            bool ok = false;
            switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
                ok = type_is_integer(et);
                if (!ok) diag_error(seg->expr->loc,
                    "format specifier %%%c expects integer type, got %s",
                    conv, type_name(et));
                break;
            case 'f': case 'e': case 'E': case 'g': case 'G':
                ok = (et->kind == TYPE_FLOAT32 || et->kind == TYPE_FLOAT64);
                if (!ok) diag_error(seg->expr->loc,
                    "format specifier %%%c expects float type, got %s",
                    conv, type_name(et));
                break;
            case 's':
                ok = (is_str_type(et) || is_cstr_type(et));
                if (!ok) diag_error(seg->expr->loc,
                    "format specifier %%s expects str or cstr, got %s",
                    type_name(et));
                break;
            case 'c':
                ok = (et->kind == TYPE_CHAR || et->kind == TYPE_UINT8);
                if (!ok) diag_error(seg->expr->loc,
                    "format specifier %%c expects char, got %s",
                    type_name(et));
                break;
            case 'p':
                ok = (et->kind == TYPE_POINTER || et->kind == TYPE_ANY_PTR);
                if (!ok) diag_error(seg->expr->loc,
                    "format specifier %%p expects pointer type, got %s",
                    type_name(et));
                break;
            case 'T':
                /* %T accepts any type — emits compile-time type name */
                ok = true;
                break;
            default:
                diag_error(seg->expr->loc,
                    "unknown format specifier %%%c", conv);
                break;
            }
        }
        /* A runtime-sized interpolation (a %s/cstr segment with no precision) has a
         * buffer whose size isn't known until execution; evaluating it in a loop
         * would grow the frame each iteration. Reject it unless given an explicit
         * home (a precision makes it constant; alloc/alloca wrap it). */
        if (!any_seg_err && !e->interp_string.wrapped && interp_is_runtime_sized(e)) {
            diag_error(e->loc,
                "unbounded string interpolation: a %%s segment has no compile-time "
                "size; add a precision (e.g. %%.64s), or wrap the string in "
                "alloc(...)! (heap) or alloca(...) (dynamic stack)");
        }
        if (e->interp_string.is_cstr) {
            e->type = type_pointer(ctx->arena, type_uint8());
            e->prov = PROV_STACK;
        } else {
            e->type = type_str();
            e->prov = PROV_STACK;
        }
        return e->type;
    }

    default:
        diag_fatal(e->loc, "unsupported expression kind in type checker (kind=%d)", e->kind);
    }
}

/* Recursively check any pattern in a match arm, resolving types and adding bindings.
   When reject_bindings is true, any surviving PAT_BINDING (i.e. not converted to
   PAT_VARIANT for no-payload variants) is an error — used inside or-pattern
   alternatives in v1, which must be binding-free. */
static void check_match_pattern(CheckCtx *ctx, Pattern *pat, Type *type, bool reject_bindings) {
    if (type_is_error(type)) return;
    type = resolve_type(ctx, type);
    switch (pat->kind) {
    case PAT_WILDCARD:
        break;
    case PAT_BINDING:
        /* Check if binding name is a no-payload union variant */
        if (type->kind == TYPE_UNION) {
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->binding.name &&
                    type->unio.variants[v].payload == NULL) {
                    pat->kind = PAT_VARIANT;
                    pat->variant.variant = pat->binding.name;
                    pat->variant.payload = NULL;
                    return;
                }
            }
        }
        if (reject_bindings) {
            diag_error(pat->loc, "or-pattern alternatives cannot bind variables: '%s'",
                pat->binding.name);
            return;
        }
        scope_add(ctx->scope, pat->binding.name, pat->binding.name, type, false);
        break;
    case PAT_INT_LIT:
        if (!type_is_integer(type)) {
            diag_error(pat->loc, "integer pattern on non-integer type %s", type_name(type));
            return;
        }
        check_int_literal_range(pat->int_lit.value, type, pat->loc,
                                pat->int_lit.out_of_range, pat->int_lit.negative);
        break;
    case PAT_BOOL_LIT:
        if (!type_eq(type, type_bool())) {
            diag_error(pat->loc, "bool pattern on non-bool type %s", type_name(type));
            return;
        }
        break;
    case PAT_CHAR_LIT:
        if (!type_eq(type, type_char())) {
            diag_error(pat->loc, "char pattern on non-char type %s", type_name(type));
            return;
        }
        break;
    case PAT_STRING_LIT:
        if (!is_str_type(type)) {
            diag_error(pat->loc, "string pattern on non-str type %s", type_name(type));
            return;
        }
        break;
    case PAT_SOME:
        if (type->kind != TYPE_OPTION) {
            diag_error(pat->loc, "some pattern on non-option type %s", type_name(type));
            return;
        }
        if (pat->some_pat.inner)
            check_match_pattern(ctx, pat->some_pat.inner, type->option.inner, reject_bindings);
        break;
    case PAT_NONE:
        if (type->kind != TYPE_OPTION) {
            diag_error(pat->loc, "none pattern on non-option type %s", type_name(type));
            return;
        }
        break;
    case PAT_VARIANT: {
        if (type->kind != TYPE_UNION) {
            diag_error(pat->loc, "variant pattern on non-union type %s", type_name(type));
            return;
        }
        bool found = false;
        for (int v = 0; v < type->unio.variant_count; v++) {
            if (type->unio.variants[v].name == pat->variant.variant) {
                found = true;
                if (pat->variant.payload && type->unio.variants[v].payload) {
                    Type *payload_type = resolve_type(ctx, type->unio.variants[v].payload);
                    check_match_pattern(ctx, pat->variant.payload, payload_type, reject_bindings);
                }
                break;
            }
        }
        if (!found) {
            diag_error(pat->loc, "union '%s' has no variant '%s'",
                type_name(type), pat->variant.variant);
            return;
        }
        break;
    }
    case PAT_TUPLE: {
        if (type->kind != TYPE_STRUCT || !type->struc.is_tuple) {
            diag_error(pat->loc, "tuple pattern on non-tuple type %s", type_name(type));
            return;
        }
        if (pat->tuple_pat.pattern_count != type->struc.field_count) {
            diag_error(pat->loc, "tuple %s has %d elements but the pattern matches %d",
                type_name(type), type->struc.field_count, pat->tuple_pat.pattern_count);
            return;
        }
        pat->tuple_pat.resolved_types = arena_alloc(ctx->arena,
            sizeof(Type*) * (size_t)(pat->tuple_pat.pattern_count > 0 ? pat->tuple_pat.pattern_count : 1));
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            Type *elem_type = resolve_type(ctx, type->struc.fields[i].type);
            pat->tuple_pat.resolved_types[i] = elem_type;
            check_match_pattern(ctx, pat->tuple_pat.patterns[i], elem_type, reject_bindings);
        }
        return;
    }
    case PAT_STRUCT: {
        if (type->kind != TYPE_STRUCT) {
            diag_error(pat->loc, "struct pattern on non-struct type %s", type_name(type));
            return;
        }
        if (type->struc.is_tuple) {
            diag_error(pat->loc, "use a positional pattern '{ a, b }' to match tuple type %s",
                type_name(type));
            return;
        }
        if (type->struc.is_c_union) {
            diag_error(pat->loc, "cannot pattern match on extern union type '%s'",
                type_name(type));
            return;
        }
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            const char *fname = pat->struc.fields[fi].name;
            Type *field_type = NULL;
            for (int fj = 0; fj < type->struc.field_count; fj++) {
                if (type->struc.fields[fj].name == fname) {
                    field_type = resolve_type(ctx, type->struc.fields[fj].type);
                    break;
                }
            }
            if (!field_type) {
                diag_error(pat->loc, "struct '%s' has no field '%s'", type_name(type), fname);
                continue;
            }
            pat->struc.fields[fi].resolved_type = field_type;
            check_match_pattern(ctx, pat->struc.fields[fi].pattern, field_type, reject_bindings);
        }
        break;
    }
    case PAT_OR:
        for (int i = 0; i < pat->or_pat.alt_count; i++)
            check_match_pattern(ctx, pat->or_pat.alts[i], type, /*reject_bindings=*/true);
        break;
    }
}

/* ---- Maranget exhaustiveness checking ---- */

/* Constructor representation for the pattern matrix */
typedef enum {
    CTOR_TRUE, CTOR_FALSE,
    CTOR_SOME, CTOR_NONE,
    CTOR_VARIANT,
    CTOR_STRUCT,
    CTOR_INT_LIT, CTOR_CHAR_LIT, CTOR_STRING_LIT,
} CtorKind;

typedef struct {
    CtorKind kind;
    const char *name;   /* variant name (CTOR_VARIANT) or struct name */
    int arity;          /* number of sub-patterns */
    uint64_t int_val;   /* for CTOR_INT_LIT */
    uint8_t char_val;   /* for CTOR_CHAR_LIT */
    const char *str_val; /* for CTOR_STRING_LIT */
} Ctor;

typedef struct MatPat MatPat;
struct MatPat {
    bool is_wildcard;
    Ctor ctor;
    MatPat *sub;        /* array of arity sub-patterns */
};

typedef struct { MatPat *elems; int len; } PatRow;
typedef struct { PatRow *rows; int row_count; int col_count; } PatMatrix;
typedef struct { Type **elems; int len; } TypeRow;

static bool ctor_eq(Ctor *a, Ctor *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case CTOR_VARIANT: return a->name == b->name;
    case CTOR_INT_LIT: return a->int_val == b->int_val;
    case CTOR_CHAR_LIT: return a->char_val == b->char_val;
    case CTOR_STRING_LIT: return strcmp(a->str_val, b->str_val) == 0;
    default: return true;
    }
}

static MatPat matpat_wild(void) {
    MatPat m = { .is_wildcard = true };
    return m;
}

/* Convert AST Pattern to MatPat */
static MatPat pat_to_matpat(CheckCtx *ctx, Pattern *pat, Type *type) {
    Arena *a = ctx->arena;
    MatPat m = {0};
    type = resolve_type(ctx, type);
    switch (pat->kind) {
    case PAT_WILDCARD:
    case PAT_BINDING:
        m.is_wildcard = true;
        return m;
    case PAT_BOOL_LIT:
        m.ctor.kind = pat->bool_lit.value ? CTOR_TRUE : CTOR_FALSE;
        m.ctor.arity = 0;
        return m;
    case PAT_SOME:
        m.ctor.kind = CTOR_SOME;
        m.ctor.arity = 1;
        m.sub = arena_alloc(a, sizeof(MatPat));
        if (pat->some_pat.inner) {
            Type *inner = (type && type->kind == TYPE_OPTION) ? type->option.inner : NULL;
            m.sub[0] = pat_to_matpat(ctx, pat->some_pat.inner, inner);
        } else {
            m.sub[0] = matpat_wild();
        }
        return m;
    case PAT_NONE:
        m.ctor.kind = CTOR_NONE;
        m.ctor.arity = 0;
        return m;
    case PAT_VARIANT: {
        m.ctor.kind = CTOR_VARIANT;
        m.ctor.name = pat->variant.variant;
        if (pat->variant.payload) {
            m.ctor.arity = 1;
            m.sub = arena_alloc(a, sizeof(MatPat));
            /* Find payload type from union */
            Type *pay_type = NULL;
            if (type && type->kind == TYPE_UNION) {
                for (int i = 0; i < type->unio.variant_count; i++) {
                    if (type->unio.variants[i].name == pat->variant.variant) {
                        pay_type = type->unio.variants[i].payload;
                        break;
                    }
                }
            }
            m.sub[0] = pat_to_matpat(ctx, pat->variant.payload, pay_type);
        } else {
            m.ctor.arity = 0;
        }
        return m;
    }
    case PAT_STRUCT: {
        /* Struct patterns expand to one sub-pattern per field in definition order.
         * Omitted fields become wildcards. */
        int nfields = 0;
        StructField *fields = NULL;
        if (type && type->kind == TYPE_STRUCT) {
            nfields = type->struc.field_count;
            fields = type->struc.fields;
        }
        m.ctor.kind = CTOR_STRUCT;
        m.ctor.name = type ? type->struc.name : NULL;
        m.ctor.arity = nfields;
        m.sub = arena_alloc(a, nfields * sizeof(MatPat));
        for (int i = 0; i < nfields; i++)
            m.sub[i] = matpat_wild();
        /* Fill in explicitly mentioned fields */
        for (int pi = 0; pi < pat->struc.field_count; pi++) {
            for (int fi = 0; fi < nfields; fi++) {
                if (fields[fi].name == pat->struc.fields[pi].name) {
                    m.sub[fi] = pat_to_matpat(ctx, pat->struc.fields[pi].pattern,
                                              fields[fi].type);
                    break;
                }
            }
        }
        return m;
    }
    case PAT_INT_LIT:
        m.ctor.kind = CTOR_INT_LIT;
        m.ctor.int_val = pat->int_lit.value;
        m.ctor.arity = 0;
        return m;
    case PAT_CHAR_LIT:
        m.ctor.kind = CTOR_CHAR_LIT;
        m.ctor.char_val = pat->char_lit.value;
        m.ctor.arity = 0;
        return m;
    case PAT_STRING_LIT:
        m.ctor.kind = CTOR_STRING_LIT;
        m.ctor.str_val = pat->string_lit.value;
        m.ctor.arity = 0;
        return m;
    case PAT_OR:
        /* flatten_or_pattern should eliminate PAT_OR before reaching here;
           fall through to wildcard as a safe default. */
        m.is_wildcard = true;
        return m;
    case PAT_TUPLE: {
        /* A tuple has a single (struct-like) constructor; sub-patterns are
         * positional, one per element, all present (no omitted positions). */
        int nfields = (type && type->kind == TYPE_STRUCT) ? type->struc.field_count : 0;
        m.ctor.kind = CTOR_STRUCT;
        m.ctor.name = type ? type->struc.name : NULL;
        m.ctor.arity = nfields;
        m.sub = arena_alloc(a, (nfields > 0 ? nfields : 1) * sizeof(MatPat));
        for (int i = 0; i < nfields; i++) {
            if (i < pat->tuple_pat.pattern_count)
                m.sub[i] = pat_to_matpat(ctx, pat->tuple_pat.patterns[i],
                                         type->struc.fields[i].type);
            else
                m.sub[i] = matpat_wild();
        }
        return m;
    }
    }
    m.is_wildcard = true;
    return m;
}

#define MAX_OR_EXPANSION 1024

/* Flatten a pattern into an array of or-free Pattern* via cartesian product over
   all PAT_OR positions. Leaf and or-free subtrees are shared with the input.
   Returns the number of flattened patterns (>=1) and sets *out, or returns -1
   and emits a diagnostic if the expansion exceeds MAX_OR_EXPANSION. */
static int flatten_or_pattern(CheckCtx *ctx, Pattern *pat, Pattern ***out, SrcLoc match_loc) {
    Arena *a = ctx->arena;

    switch (pat->kind) {
    case PAT_WILDCARD:
    case PAT_BINDING:
    case PAT_INT_LIT:
    case PAT_CHAR_LIT:
    case PAT_BOOL_LIT:
    case PAT_STRING_LIT:
    case PAT_NONE:
        *out = arena_alloc(a, sizeof(Pattern *));
        (*out)[0] = pat;
        return 1;
    case PAT_OR: {
        Pattern **tmp = NULL;
        int count = 0, cap = 0;
        for (int i = 0; i < pat->or_pat.alt_count; i++) {
            Pattern **sub;
            int sc = flatten_or_pattern(ctx, pat->or_pat.alts[i], &sub, match_loc);
            if (sc < 0) { free(tmp); return -1; }
            for (int j = 0; j < sc; j++) {
                if (count >= MAX_OR_EXPANSION) {
                    diag_error(match_loc,
                        "or-pattern expands to too many combinations — simplify");
                    free(tmp);
                    return -1;
                }
                DA_APPEND(tmp, count, cap, sub[j]);
            }
        }
        *out = arena_alloc(a, sizeof(Pattern *) * (size_t)count);
        memcpy(*out, tmp, sizeof(Pattern *) * (size_t)count);
        free(tmp);
        return count;
    }
    case PAT_SOME: {
        if (!pat->some_pat.inner) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        Pattern **inners;
        int ic = flatten_or_pattern(ctx, pat->some_pat.inner, &inners, match_loc);
        if (ic < 0) return -1;
        if (ic == 1 && inners[0] == pat->some_pat.inner) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        *out = arena_alloc(a, sizeof(Pattern *) * (size_t)ic);
        for (int i = 0; i < ic; i++) {
            Pattern *np = arena_alloc(a, sizeof(Pattern));
            np->kind = PAT_SOME;
            np->loc = pat->loc;
            np->some_pat.inner = inners[i];
            (*out)[i] = np;
        }
        return ic;
    }
    case PAT_VARIANT: {
        if (!pat->variant.payload) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        Pattern **inners;
        int ic = flatten_or_pattern(ctx, pat->variant.payload, &inners, match_loc);
        if (ic < 0) return -1;
        if (ic == 1 && inners[0] == pat->variant.payload) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        *out = arena_alloc(a, sizeof(Pattern *) * (size_t)ic);
        for (int i = 0; i < ic; i++) {
            Pattern *np = arena_alloc(a, sizeof(Pattern));
            np->kind = PAT_VARIANT;
            np->loc = pat->loc;
            np->variant.variant = pat->variant.variant;
            np->variant.payload = inners[i];
            (*out)[i] = np;
        }
        return ic;
    }
    case PAT_STRUCT: {
        int nf = pat->struc.field_count;
        if (nf == 0) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        Pattern ***field_exp = arena_alloc(a, sizeof(Pattern **) * (size_t)nf);
        int *field_counts = arena_alloc(a, sizeof(int) * (size_t)nf);
        bool any_expanded = false;
        for (int i = 0; i < nf; i++) {
            int fc = flatten_or_pattern(ctx, pat->struc.fields[i].pattern,
                                        &field_exp[i], match_loc);
            if (fc < 0) return -1;
            field_counts[i] = fc;
            if (fc > 1 || field_exp[i][0] != pat->struc.fields[i].pattern)
                any_expanded = true;
        }
        if (!any_expanded) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        long long total = 1;
        for (int i = 0; i < nf; i++) {
            total *= field_counts[i];
            if (total > MAX_OR_EXPANSION) {
                diag_error(match_loc,
                    "or-pattern expands to too many combinations — simplify");
                return -1;
            }
        }
        Pattern **result = arena_alloc(a, sizeof(Pattern *) * (size_t)total);
        int *idx = arena_alloc(a, sizeof(int) * (size_t)nf);
        memset(idx, 0, sizeof(int) * (size_t)nf);
        for (int n = 0; n < total; n++) {
            FieldPattern *new_fields = arena_alloc(a, sizeof(FieldPattern) * (size_t)nf);
            for (int i = 0; i < nf; i++) {
                new_fields[i].name = pat->struc.fields[i].name;
                new_fields[i].pattern = field_exp[i][idx[i]];
                new_fields[i].resolved_type = pat->struc.fields[i].resolved_type;
            }
            Pattern *np = arena_alloc(a, sizeof(Pattern));
            np->kind = PAT_STRUCT;
            np->loc = pat->loc;
            np->struc.fields = new_fields;
            np->struc.field_count = nf;
            result[n] = np;
            for (int i = nf - 1; i >= 0; i--) {
                idx[i]++;
                if (idx[i] < field_counts[i]) break;
                idx[i] = 0;
            }
        }
        *out = result;
        return (int)total;
    }
    case PAT_TUPLE: {
        int nf = pat->tuple_pat.pattern_count;
        Pattern ***elem_exp = arena_alloc(a, sizeof(Pattern **) * (size_t)(nf > 0 ? nf : 1));
        int *elem_counts = arena_alloc(a, sizeof(int) * (size_t)(nf > 0 ? nf : 1));
        bool any_expanded = false;
        for (int i = 0; i < nf; i++) {
            int ec = flatten_or_pattern(ctx, pat->tuple_pat.patterns[i],
                                        &elem_exp[i], match_loc);
            if (ec < 0) return -1;
            elem_counts[i] = ec;
            if (ec > 1 || elem_exp[i][0] != pat->tuple_pat.patterns[i])
                any_expanded = true;
        }
        if (!any_expanded) {
            *out = arena_alloc(a, sizeof(Pattern *));
            (*out)[0] = pat;
            return 1;
        }
        long long total = 1;
        for (int i = 0; i < nf; i++) {
            total *= elem_counts[i];
            if (total > MAX_OR_EXPANSION) {
                diag_error(match_loc,
                    "or-pattern expands to too many combinations — simplify");
                return -1;
            }
        }
        Pattern **result = arena_alloc(a, sizeof(Pattern *) * (size_t)total);
        int *idx = arena_alloc(a, sizeof(int) * (size_t)(nf > 0 ? nf : 1));
        memset(idx, 0, sizeof(int) * (size_t)(nf > 0 ? nf : 1));
        for (int n = 0; n < total; n++) {
            Pattern **new_pats = arena_alloc(a, sizeof(Pattern *) * (size_t)(nf > 0 ? nf : 1));
            for (int i = 0; i < nf; i++)
                new_pats[i] = elem_exp[i][idx[i]];
            Pattern *np = arena_alloc(a, sizeof(Pattern));
            np->kind = PAT_TUPLE;
            np->loc = pat->loc;
            np->tuple_pat.patterns = new_pats;
            np->tuple_pat.pattern_count = nf;
            np->tuple_pat.resolved_types = pat->tuple_pat.resolved_types;
            result[n] = np;
            for (int i = nf - 1; i >= 0; i--) {
                idx[i]++;
                if (idx[i] < elem_counts[i]) break;
                idx[i] = 0;
            }
        }
        *out = result;
        return (int)total;
    }
    }
    *out = arena_alloc(a, sizeof(Pattern *));
    (*out)[0] = pat;
    return 1;
}

/* Enumerate all constructors for a type. Returns count, fills ctors array.
 * Returns -1 if the type has infinite/non-enumerable constructors. */
static int type_ctors_list(CheckCtx *ctx, Type *type, Ctor **out) {
    Arena *a = ctx->arena;
    type = resolve_type(ctx, type);
    if (!type) { *out = NULL; return -1; }
    if (type->kind == TYPE_BOOL) {
        *out = arena_alloc(a, 2 * sizeof(Ctor));
        (*out)[0] = (Ctor){ .kind = CTOR_TRUE, .arity = 0 };
        (*out)[1] = (Ctor){ .kind = CTOR_FALSE, .arity = 0 };
        return 2;
    }
    if (type->kind == TYPE_OPTION) {
        *out = arena_alloc(a, 2 * sizeof(Ctor));
        (*out)[0] = (Ctor){ .kind = CTOR_SOME, .arity = 1 };
        (*out)[1] = (Ctor){ .kind = CTOR_NONE, .arity = 0 };
        return 2;
    }
    if (type->kind == TYPE_UNION) {
        int n = type->unio.variant_count;
        *out = arena_alloc(a, n * sizeof(Ctor));
        for (int i = 0; i < n; i++) {
            (*out)[i] = (Ctor){
                .kind = CTOR_VARIANT,
                .name = type->unio.variants[i].name,
                .arity = type->unio.variants[i].payload ? 1 : 0,
            };
        }
        return n;
    }
    if (type->kind == TYPE_STRUCT) {
        *out = arena_alloc(a, sizeof(Ctor));
        (*out)[0] = (Ctor){
            .kind = CTOR_STRUCT,
            .name = type->struc.name,
            .arity = type->struc.field_count,
        };
        return 1;
    }
    *out = NULL;
    return -1;
}

/* Get the sub-types for a constructor applied to a type.
 * Uses CheckCtx to resolve stub types (struct field types may be unresolved). */
static TypeRow ctor_sub_types(CheckCtx *ctx, Ctor *ctor, Type *type) {
    TypeRow r = {0};
    switch (ctor->kind) {
    case CTOR_SOME:
        if (type && type->kind == TYPE_OPTION) {
            r.len = 1;
            r.elems = arena_alloc(ctx->arena, sizeof(Type*));
            r.elems[0] = resolve_type(ctx, type->option.inner);
        }
        break;
    case CTOR_NONE:
    case CTOR_TRUE:
    case CTOR_FALSE:
    case CTOR_INT_LIT:
    case CTOR_CHAR_LIT:
    case CTOR_STRING_LIT:
        break;
    case CTOR_VARIANT:
        if (type && type->kind == TYPE_UNION) {
            for (int i = 0; i < type->unio.variant_count; i++) {
                if (type->unio.variants[i].name == ctor->name) {
                    if (type->unio.variants[i].payload) {
                        r.len = 1;
                        r.elems = arena_alloc(ctx->arena, sizeof(Type*));
                        r.elems[0] = resolve_type(ctx, type->unio.variants[i].payload);
                    }
                    break;
                }
            }
        }
        break;
    case CTOR_STRUCT:
        if (type && type->kind == TYPE_STRUCT) {
            r.len = type->struc.field_count;
            r.elems = arena_alloc(ctx->arena, r.len * sizeof(Type*));
            for (int i = 0; i < r.len; i++)
                r.elems[i] = resolve_type(ctx, type->struc.fields[i].type);
        }
        break;
    }
    return r;
}

/* Build a new TypeRow: ctor_sub_types ++ types[1..] */
static TypeRow types_specialize(CheckCtx *ctx, TypeRow *types, Ctor *ctor, Type *col_type) {
    TypeRow sub = ctor_sub_types(ctx, ctor, col_type);
    int new_len = sub.len + types->len - 1;
    TypeRow r;
    r.len = new_len;
    r.elems = arena_alloc(ctx->arena, new_len * sizeof(Type*));
    for (int i = 0; i < sub.len; i++)
        r.elems[i] = sub.elems[i];
    for (int i = 1; i < types->len; i++)
        r.elems[sub.len + i - 1] = types->elems[i];
    return r;
}

/* Specialize the matrix by constructor c */
static PatMatrix specialize(Arena *a, PatMatrix *mat, Ctor *c) {
    /* Count matching rows first */
    int cap = mat->row_count;
    PatRow *rows = arena_alloc(a, cap * sizeof(PatRow));
    int count = 0;
    int new_cols = c->arity + mat->col_count - 1;

    for (int r = 0; r < mat->row_count; r++) {
        MatPat *first = &mat->rows[r].elems[0];
        bool match = false;
        MatPat *subs = NULL;
        int sub_count = 0;

        if (first->is_wildcard) {
            match = true;
            /* Expand wildcard into arity wildcards */
            sub_count = c->arity;
            subs = arena_alloc(a, sub_count * sizeof(MatPat));
            for (int i = 0; i < sub_count; i++)
                subs[i] = matpat_wild();
        } else if (ctor_eq(&first->ctor, c)) {
            match = true;
            sub_count = first->ctor.arity;
            subs = first->sub;
        }

        if (match) {
            PatRow *row = &rows[count++];
            row->len = new_cols;
            row->elems = arena_alloc(a, new_cols * sizeof(MatPat));
            for (int i = 0; i < sub_count; i++)
                row->elems[i] = subs[i];
            for (int i = 1; i < mat->col_count; i++)
                row->elems[sub_count + i - 1] = mat->rows[r].elems[i];
        }
    }

    return (PatMatrix){ .rows = rows, .row_count = count, .col_count = new_cols };
}

/* Default matrix: rows with wildcard in first column, minus first column */
static PatMatrix default_matrix(Arena *a, PatMatrix *mat) {
    int cap = mat->row_count;
    PatRow *rows = arena_alloc(a, cap * sizeof(PatRow));
    int count = 0;
    int new_cols = mat->col_count - 1;

    for (int r = 0; r < mat->row_count; r++) {
        if (mat->rows[r].elems[0].is_wildcard) {
            PatRow *row = &rows[count++];
            row->len = new_cols;
            row->elems = arena_alloc(a, new_cols * sizeof(MatPat));
            for (int i = 0; i < new_cols; i++)
                row->elems[i] = mat->rows[r].elems[i + 1];
        }
    }

    return (PatMatrix){ .rows = rows, .row_count = count, .col_count = new_cols };
}

/* Collect the set of constructors appearing in column 0 of the matrix */
static int collect_head_ctors(Arena *a, PatMatrix *mat, Ctor **out) {
    int count = 0, cap = 0;
    *out = NULL;
    for (int r = 0; r < mat->row_count; r++) {
        MatPat *first = &mat->rows[r].elems[0];
        if (first->is_wildcard) continue;
        /* Check if already in the set */
        bool found = false;
        for (int i = 0; i < count; i++) {
            if (ctor_eq(&(*out)[i], &first->ctor)) { found = true; break; }
        }
        if (!found) {
            DA_APPEND(*out, count, cap, first->ctor);
        }
    }
    /* Allocate into arena and copy if needed */
    (void)a;
    return count;
}

/* Find witness: returns NULL if exhaustive, or a witness PatRow if not */
static PatRow *find_witness(CheckCtx *ctx, PatMatrix *mat, TypeRow *types) {
    Arena *a = ctx->arena;
    if (types->len == 0) {
        if (mat->row_count > 0) return NULL; /* exhaustive */
        /* Non-exhaustive: empty witness */
        PatRow *w = arena_alloc(a, sizeof(PatRow));
        w->len = 0;
        w->elems = NULL;
        return w;
    }

    Type *col_type = types->elems[0];

    /* Collect constructors in column 0 */
    Ctor *head_ctors = NULL;
    int head_count = collect_head_ctors(a, mat, &head_ctors);

    /* Get all constructors for this type */
    Ctor *all_ctors = NULL;
    int all_count = type_ctors_list(ctx, col_type, &all_ctors);

    /* Check if head constructors form a complete signature */
    bool complete = false;
    if (all_count >= 0) {
        complete = true;
        for (int i = 0; i < all_count; i++) {
            bool found = false;
            for (int j = 0; j < head_count; j++) {
                if (ctor_eq(&all_ctors[i], &head_ctors[j])) { found = true; break; }
            }
            if (!found) { complete = false; break; }
        }
    }

    if (complete) {
        /* Complete signature: check each constructor */
        for (int ci = 0; ci < all_count; ci++) {
            Ctor *c = &all_ctors[ci];
            PatMatrix sm = specialize(a, mat, c);
            TypeRow st = types_specialize(ctx, types, c, col_type);
            PatRow *w = find_witness(ctx, &sm, &st);
            if (w) {
                /* Reconstruct: wrap first `arity` elements in c, prepend to rest */
                int arity = c->arity;
                PatRow *result = arena_alloc(a, sizeof(PatRow));
                result->len = 1 + (w->len - arity);
                result->elems = arena_alloc(a, result->len * sizeof(MatPat));
                /* Build the constructor pattern from witness sub-patterns */
                result->elems[0].is_wildcard = false;
                result->elems[0].ctor = *c;
                if (arity > 0) {
                    result->elems[0].sub = arena_alloc(a, arity * sizeof(MatPat));
                    for (int i = 0; i < arity; i++)
                        result->elems[0].sub[i] = w->elems[i];
                } else {
                    result->elems[0].sub = NULL;
                }
                /* Copy remaining */
                for (int i = arity; i < w->len; i++)
                    result->elems[i - arity + 1] = w->elems[i];
                return result;
            }
        }
        return NULL; /* all constructors exhaustive */
    } else {
        /* Incomplete signature: check default matrix */
        PatMatrix dm = default_matrix(a, mat);
        TypeRow dt;
        dt.len = types->len - 1;
        dt.elems = types->elems + 1;
        PatRow *w = find_witness(ctx, &dm, &dt);
        if (w) {
            PatRow *result = arena_alloc(a, sizeof(PatRow));
            result->len = 1 + w->len;
            result->elems = arena_alloc(a, result->len * sizeof(MatPat));
            /* Copy rest */
            for (int i = 0; i < w->len; i++)
                result->elems[i + 1] = w->elems[i];

            if (all_count < 0) {
                /* Infinite type: wildcard */
                result->elems[0] = matpat_wild();
            } else {
                /* Find a missing constructor */
                Ctor *missing = &all_ctors[0]; /* default */
                for (int i = 0; i < all_count; i++) {
                    bool found = false;
                    for (int j = 0; j < head_count; j++) {
                        if (ctor_eq(&all_ctors[i], &head_ctors[j])) { found = true; break; }
                    }
                    if (!found) { missing = &all_ctors[i]; break; }
                }
                result->elems[0].is_wildcard = false;
                result->elems[0].ctor = *missing;
                if (missing->arity > 0) {
                    result->elems[0].sub = arena_alloc(a, missing->arity * sizeof(MatPat));
                    for (int i = 0; i < missing->arity; i++)
                        result->elems[0].sub[i] = matpat_wild();
                } else {
                    result->elems[0].sub = NULL;
                }
            }
            return result;
        }
        return NULL;
    }
}

/* Find the deepest interesting witness pattern — dig through structs (single
 * constructor) and option/variant wrappers to find the leaf that the user
 * actually needs to handle. */
static MatPat *find_interesting_witness(CheckCtx *ctx, MatPat *w, Type *type, Type **out_type) {
    type = resolve_type(ctx, type);
    if (w->is_wildcard) { *out_type = type; return w; }
    if (w->ctor.kind == CTOR_STRUCT && type && type->kind == TYPE_STRUCT) {
        /* Look inside struct for the interesting non-wildcard sub-pattern */
        for (int i = 0; i < w->ctor.arity; i++) {
            if (!w->sub[i].is_wildcard) {
                Type *field_type = resolve_type(ctx, type->struc.fields[i].type);
                return find_interesting_witness(ctx, &w->sub[i], field_type, out_type);
            }
        }
    }
    /* Dig through some(inner) — the interesting part is what's inside */
    if (w->ctor.kind == CTOR_SOME && w->ctor.arity == 1 && !w->sub[0].is_wildcard) {
        Type *inner = (type && type->kind == TYPE_OPTION) ? type->option.inner : NULL;
        return find_interesting_witness(ctx, &w->sub[0], inner, out_type);
    }
    /* Dig through variant(payload) — the interesting part is what's inside */
    if (w->ctor.kind == CTOR_VARIANT && w->ctor.arity == 1 && !w->sub[0].is_wildcard) {
        Type *pay = NULL;
        if (type && type->kind == TYPE_UNION) {
            for (int i = 0; i < type->unio.variant_count; i++) {
                if (type->unio.variants[i].name == w->ctor.name) {
                    pay = type->unio.variants[i].payload;
                    break;
                }
            }
        }
        return find_interesting_witness(ctx, &w->sub[0], pay, out_type);
    }
    *out_type = type;
    return w;
}

/* Report a non-exhaustive match based on witness */
static void report_witness(CheckCtx *ctx, SrcLoc loc, MatPat *witness, Type *subj_type) {
    /* For struct witnesses, dig into sub-patterns for a meaningful error */
    Type *witness_type = subj_type;
    MatPat *interesting = find_interesting_witness(ctx, witness, subj_type, &witness_type);

    if (interesting->is_wildcard) {
        diag_error(loc,
            "non-exhaustive match: add a wildcard '_' pattern or binding to cover all cases");
        return;
    }
    switch (interesting->ctor.kind) {
    case CTOR_TRUE:
        diag_error(loc, "non-exhaustive match: missing 'true' case for bool");
        break;
    case CTOR_FALSE:
        diag_error(loc, "non-exhaustive match: missing 'false' case for bool");
        break;
    case CTOR_SOME:
        diag_error(loc, "non-exhaustive match: missing 'some' case for option type");
        break;
    case CTOR_NONE:
        diag_error(loc, "non-exhaustive match: missing 'none' case for option type");
        break;
    case CTOR_VARIANT:
        if (witness_type && witness_type->kind == TYPE_UNION)
            diag_error(loc, "non-exhaustive match: missing variant '%s' of union '%s'",
                interesting->ctor.name, type_name(witness_type));
        else
            diag_error(loc, "non-exhaustive match: missing variant '%s'",
                interesting->ctor.name);
        break;
    case CTOR_STRUCT:
    case CTOR_INT_LIT:
    case CTOR_CHAR_LIT:
    case CTOR_STRING_LIT:
        diag_error(loc,
            "non-exhaustive match: add a wildcard '_' pattern or binding to cover all cases");
        break;
    }
}

static void check_match_exhaustiveness(CheckCtx *ctx, Expr *e, Type *subj_type) {
    /* Build the pattern matrix. Each arm may contribute multiple rows if its
       pattern contains any PAT_OR — flatten_or_pattern produces the cartesian
       product of or-free patterns.

       Arms with a `when` guard are NOT treated as covering their shape: the
       guard may evaluate false at runtime, letting control fall through to
       the next arm. Skipping guarded arms here forces the remaining unguarded
       arms (or a wildcard) to be exhaustive on their own. */
    int arm_count = e->match_expr.arm_count;
    PatRow *tmp_rows = NULL;
    int row_count = 0, row_cap = 0;
    for (int i = 0; i < arm_count; i++) {
        if (e->match_expr.arms[i].guard) continue;
        Pattern **flats;
        int fc = flatten_or_pattern(ctx, e->match_expr.arms[i].pattern, &flats, e->loc);
        if (fc < 0) { free(tmp_rows); return; }
        for (int j = 0; j < fc; j++) {
            PatRow row;
            row.len = 1;
            row.elems = arena_alloc(ctx->arena, sizeof(MatPat));
            row.elems[0] = pat_to_matpat(ctx, flats[j], subj_type);
            DA_APPEND(tmp_rows, row_count, row_cap, row);
        }
    }
    PatRow *rows = arena_alloc(ctx->arena, row_count * sizeof(PatRow));
    memcpy(rows, tmp_rows, row_count * sizeof(PatRow));
    free(tmp_rows);
    PatMatrix mat = { .rows = rows, .row_count = row_count, .col_count = 1 };

    TypeRow types;
    types.len = 1;
    types.elems = arena_alloc(ctx->arena, sizeof(Type*));
    types.elems[0] = subj_type;

    PatRow *witness = find_witness(ctx, &mat, &types);
    if (witness && witness->len > 0) {
        report_witness(ctx, e->loc, &witness->elems[0], subj_type);
    } else if (witness) {
        /* Empty witness — shouldn't happen with len > 0, but guard */
        diag_error(e->loc,
            "non-exhaustive match: add a wildcard '_' pattern or binding to cover all cases");
    }
}

static Type *check_match(CheckCtx *ctx, Expr *e) {
    Type *subj_type = check_expr(ctx, e->match_expr.subject);
    if (reject_unresolved_recursive_value(e->match_expr.subject)) { e->type = type_error(); return e->type; }
    subj_type = resolve_type(ctx, subj_type);
    /* Update the subject's type to the resolved type so codegen can access it */
    e->match_expr.subject->type = subj_type;

    if (e->match_expr.arm_count == 0) {
        diag_error(e->loc, "match expression has no arms");
        e->type = type_error();
        return e->type;
    }

    Type *result_type = NULL;
    Provenance result_prov = PROV_UNKNOWN;

    /* While inferring a recursive function's return type, check base-case arms
       before arms that consume a self-recursive call's result, so the placeholder
       is anchored first. This reorders only the checking pass; the arms array (used
       by codegen and the exhaustiveness check below) stays in source order. */
    int arm_count = e->match_expr.arm_count;
    int *order = arena_alloc(ctx->arena, sizeof(int) * (size_t)arm_count);
    if (resolving_recursion(ctx)) {
        int oc = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < arm_count; i++) {
                MatchArm *a = &e->match_expr.arms[i];
                Expr *tail = a->body_count > 0 ? a->body[a->body_count - 1] : NULL;
                if (branch_can_anchor(tail, ctx->recursive_self_name) == (pass == 0))
                    order[oc++] = i;
            }
    } else {
        for (int i = 0; i < arm_count; i++) order[i] = i;
    }

    for (int k = 0; k < arm_count; k++) {
        int i = order[k];
        MatchArm *arm = &e->match_expr.arms[i];
        Pattern *pat = arm->pattern;

        /* Create a new scope for pattern bindings */
        Scope *arm_scope = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = arm_scope;

        /* Check pattern and introduce bindings */
        check_match_pattern(ctx, pat, subj_type, /*reject_bindings=*/false);

        /* Type-check the optional `when` guard in the arm scope, so
           destructured pattern bindings are visible. Guard must be bool. */
        if (arm->guard) {
            Type *guard_type = check_expr(ctx, arm->guard);
            if (!type_is_error(guard_type) && guard_type->kind != TYPE_BOOL) {
                diag_error(arm->guard->loc,
                    "'when' guard must be bool, got %s",
                    type_name(guard_type));
            }
        }

        /* Type-check arm body */
        Type *arm_type = check_block(ctx, arm->body, arm->body_count);
        Provenance arm_prov = PROV_UNKNOWN;
        if (arm->body_count > 0)
            arm_prov = arm->body[arm->body_count - 1]->prov;
        ctx->scope = saved;

        if (type_is_error(arm_type)) continue;

        if (!result_type || type_is_error(result_type)) {
            /* First arm (may be `never` if it diverges; a concrete arm overtakes
               it on a later iteration via unify_branch). */
            result_type = arm_type;
            result_prov = arm_prov;
        } else {
            Type *unified = unify_branch(result_type, arm_type);
            if (!unified) {
                diag_error(arm->loc, "match arms have different types: %s vs %s",
                    type_name(result_type), type_name(arm_type));
            } else {
                /* A diverging (never) arm carries no value: when it overtakes a
                   prior never result, adopt its provenance; otherwise merge only
                   value-producing arms. */
                if (type_is_never(result_type) && !type_is_never(arm_type))
                    result_prov = arm_prov;
                else if (!type_is_never(arm_type))
                    result_prov = merge_prov(result_prov, arm_prov);
                result_type = unified;
            }
        }
        /* Anchor the recursive return type from the first concrete arm (base case),
           so a later arm's self-recursive call observes the inferred type. */
        maybe_anchor_recursive(ctx, result_type);
    }

    /* ---- Exhaustiveness check ---- */
    if (!type_is_error(subj_type)) {
        check_match_exhaustiveness(ctx, e, subj_type);
    }

    e->type = result_type ? result_type : type_error();
    e->prov = result_prov;
    return e->type;
}

/* ---- Const-expr fold for module-level let initializers ----
 *
 * When a module-level let's init contains EXPR_IDENT references to other
 * module-level const-expr lets, fold substitutes the referenced values so
 * codegen sees only literal forms.  On success the caller overwrites
 * d->let.init with the folded tree.
 *
 * The per-decl state machine is UNVISITED -> (VISITING ->) DONE | FAILED:
 * DONE caches the folded value for later refs; FAILED short-circuits
 * retries when the target's init isn't a const-expr.  VISITING exists
 * only as an infinite-recursion guard — any actual cycle in FC source
 * is caught earlier by pass2's on-demand type-check cycle detector
 * (`circular dependency: 'X' depends on itself`), which type-checks the
 * same ident paths fold would walk.  If fold ever observes VISITING in
 * practice we've hit an internal inconsistency. */

enum {
    CONST_FOLD_UNVISITED = 0,
    CONST_FOLD_VISITING  = 1,
    CONST_FOLD_DONE      = 2,
    CONST_FOLD_FAILED    = 3,
};

static bool is_const_expr(Expr *e);
static Expr *const_fold_expr(CheckCtx *ctx, Expr *e);

/* Clone an Expr subtree for const-expr substitution.  Aggregate nodes are
 * freshly allocated so mutable per-node codegen state (EXPR_ARRAY_LIT's
 * codegen_backing_name) is not aliased across substitution sites. */
static Expr *const_clone_expr(CheckCtx *ctx, Expr *src) {
    if (!src) return NULL;
    switch (src->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT:
    case EXPR_SIZEOF:
    case EXPR_ALIGNOF:
    case EXPR_DEFAULT:
    case EXPR_FIELD:  /* extern-const or no-payload variant ctor */
        return src;
    case EXPR_UNARY_PREFIX: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        n->unary_prefix.operand = const_clone_expr(ctx, src->unary_prefix.operand);
        return n;
    }
    case EXPR_BINARY: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        n->binary.left  = const_clone_expr(ctx, src->binary.left);
        n->binary.right = const_clone_expr(ctx, src->binary.right);
        return n;
    }
    case EXPR_CAST: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        n->cast.operand = const_clone_expr(ctx, src->cast.operand);
        return n;
    }
    case EXPR_STRUCT_LIT: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        int fc = src->struct_lit.field_count;
        if (fc > 0) {
            FieldInit *fields = arena_alloc(ctx->arena, sizeof(FieldInit) * (size_t)fc);
            for (int i = 0; i < fc; i++) {
                fields[i] = src->struct_lit.fields[i];
                fields[i].value = const_clone_expr(ctx, src->struct_lit.fields[i].value);
            }
            n->struct_lit.fields = fields;
        }
        return n;
    }
    case EXPR_TUPLE_LIT: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        int ec = src->tuple_lit.elem_count;
        if (ec > 0) {
            Expr **elems = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)ec);
            for (int i = 0; i < ec; i++)
                elems[i] = const_clone_expr(ctx, src->tuple_lit.elems[i]);
            n->tuple_lit.elems = elems;
        }
        return n;
    }
    case EXPR_ARRAY_LIT: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        n->array_lit.codegen_backing_name = NULL;  /* fresh backing per clone */
        int ec = src->array_lit.elem_count;
        if (ec > 0) {
            Expr **elems = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)ec);
            for (int i = 0; i < ec; i++)
                elems[i] = const_clone_expr(ctx, src->array_lit.elems[i]);
            n->array_lit.elems = elems;
        }
        n->array_lit.size_expr = const_clone_expr(ctx, src->array_lit.size_expr);
        return n;
    }
    case EXPR_SLICE_LIT: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        n->slice_lit.ptr_expr = const_clone_expr(ctx, src->slice_lit.ptr_expr);
        n->slice_lit.len_expr = const_clone_expr(ctx, src->slice_lit.len_expr);
        return n;
    }
    case EXPR_SOME: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        n->some_expr.value = const_clone_expr(ctx, src->some_expr.value);
        return n;
    }
    case EXPR_CALL: {
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *src;
        int ac = src->call.arg_count;
        if (ac > 0) {
            Expr **args = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)ac);
            for (int i = 0; i < ac; i++)
                args[i] = const_clone_expr(ctx, src->call.args[i]);
            n->call.args = args;
        }
        return n;
    }
    default:
        return src;
    }
}

/* ---- Compile-time evaluation of fixed-width scalar const expressions ----
 *
 * Evaluates a folded integer/bool subexpression to a single literal node so
 * module-member initializers (which must be valid C file-scope constants) can
 * use operations that would otherwise emit runtime checks — notably integer
 * division/modulo, whose codegen is a by-zero abort statement-expression that
 * is not a constant initializer.  Only fixed-width types (int8..uint64, bool)
 * are evaluated: isize/usize widths are target-defined and float folding is
 * left to the C compiler, so those are deferred (return NULL → tree unchanged).
 * Semantics mirror codegen exactly: 2's-complement wrap, shift-amount masking,
 * arithmetic vs. logical right shift, and the signed INT_MIN/-1 case. */

/* Bit width of a fixed-width integer type, or 0 for isize/usize/non-integer. */
static int const_int_width(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_INT8:  case TYPE_UINT8:  return 8;
    case TYPE_INT16: case TYPE_UINT16: return 16;
    case TYPE_INT32: case TYPE_UINT32: return 32;
    case TYPE_INT64: case TYPE_UINT64: return 64;
    default: return 0;  /* isize/usize (target-defined) and non-integers */
    }
}

/* Mask a 64-bit value to `width` bits, sign-extending back to 64 if signed. */
static uint64_t const_mask_extend(uint64_t v, int width, bool is_signed) {
    if (width >= 64) return v;
    uint64_t mask = ((uint64_t)1 << width) - 1;
    v &= mask;
    if (is_signed && (v & ((uint64_t)1 << (width - 1))))
        v |= ~mask;
    return v;
}

/* A scalar integer value read from a literal, normalized to 64 bits. */
typedef struct { uint64_t val; int width; bool is_signed; } ConstScalar;

/* Read a folded expr as a fixed-width int/bool/char literal value, or fail
 * (isize/usize, float, or non-literal operands are not host-evaluable). */
static bool const_read_scalar(Expr *e, ConstScalar *out) {
    switch (e->kind) {
    case EXPR_INT_LIT: {
        int w = const_int_width(e->int_lit.lit_type);
        if (w == 0) return false;  /* isize/usize: defer to target compiler */
        bool s = type_is_signed(e->int_lit.lit_type);
        out->val = const_mask_extend(e->int_lit.value, w, s);
        out->width = w;
        out->is_signed = s;
        return true;
    }
    case EXPR_BOOL_LIT:
        out->val = e->bool_lit.value ? 1 : 0;
        out->width = 8;
        out->is_signed = false;
        return true;
    case EXPR_CHAR_LIT:
        out->val = e->char_lit.value;
        out->width = 8;
        out->is_signed = false;
        return true;
    default:
        return false;
    }
}

/* True when a slice literal's len is a (possibly implicitly-widened) integer
 * literal — the form check_expr already validated for negativity, so
 * const_fold_expr skips it to avoid a duplicate diagnostic. */
static bool slicelit_len_is_literal(Expr *len) {
    if (!len) return false;
    if (len->kind == EXPR_INT_LIT) return true;
    if (len->kind == EXPR_CAST && len->cast.operand &&
        len->cast.operand->kind == EXPR_INT_LIT)
        return true;
    return false;
}

static Expr *const_make_int(CheckCtx *ctx, Type *t, uint64_t v, SrcLoc loc) {
    Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
    n->kind = EXPR_INT_LIT;
    n->loc = loc;
    n->type = t;
    n->int_lit.value = v;
    n->int_lit.lit_type = t;
    n->int_lit.out_of_range = false;
    return n;
}

static Expr *const_make_bool(CheckCtx *ctx, Type *t, bool v, SrcLoc loc) {
    Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
    n->kind = EXPR_BOOL_LIT;
    n->loc = loc;
    n->type = t;
    n->bool_lit.value = v;
    return n;
}

/* Fold a static integer type property (int32.min, uint8.max, int16.bits, ...)
 * to a host literal so it can participate in compile-time evaluation — integer
 * division/modulo, comparisons, and reuse from other const initializers.
 * Returns NULL for properties whose value is not a fixed-width host constant:
 * float properties (min/max/nan/... — deferred to the C compiler) and the
 * target-defined isize/usize width.  Those still satisfy the const-expr gate and
 * emit their C macro unchanged. */
static Expr *const_fold_type_property(CheckCtx *ctx, Expr *e) {
    if (!e->field.is_type_property || e->field.object->kind != EXPR_IDENT)
        return NULL;
    Type *t = type_from_name(e->field.object->ident.name,
                             (int)strlen(e->field.object->ident.name));
    if (!t) return NULL;
    int w = const_int_width(t);  /* 0 for floats and isize/usize */
    const char *prop = e->field.name;

    if (strcmp(prop, "bits") == 0) {
        if (w == 0) return NULL;  /* isize/usize width is target-defined */
        return const_make_int(ctx, type_int32(), (uint64_t)w, e->loc);
    }
    if (w == 0) return NULL;  /* float min/max/etc. and isize/usize min/max */
    bool s = type_is_signed(t);
    uint64_t v;
    if (strcmp(prop, "min") == 0)
        v = s ? ((uint64_t)1 << (w - 1)) : 0;
    else if (strcmp(prop, "max") == 0)
        v = s ? (((uint64_t)1 << (w - 1)) - 1) : ~(uint64_t)0;
    else
        return NULL;
    return const_make_int(ctx, t, const_mask_extend(v, w, s), e->loc);
}

/* Try to evaluate a node whose children have already been const-folded to a
 * single literal.  Returns the literal, or NULL if the node is not evaluable
 * (caller keeps the folded tree).  Emits a diagnostic for compile-time
 * division by zero. */
static Expr *try_eval_const(CheckCtx *ctx, Expr *e) {
    if (!e || !e->type) return NULL;
    switch (e->kind) {
    case EXPR_UNARY_PREFIX: {
        TokenKind op = e->unary_prefix.op;
        ConstScalar a;
        if (!const_read_scalar(e->unary_prefix.operand, &a)) return NULL;
        if (op == TOK_BANG) {
            if (e->type->kind != TYPE_BOOL) return NULL;
            return const_make_bool(ctx, e->type, a.val == 0, e->loc);
        }
        int w = const_int_width(e->type);
        if (w == 0) return NULL;
        bool s = type_is_signed(e->type);
        uint64_t r;
        if (op == TOK_MINUS)      r = (uint64_t)0 - a.val;
        else if (op == TOK_TILDE) r = ~a.val;
        else return NULL;
        return const_make_int(ctx, e->type, const_mask_extend(r, w, s), e->loc);
    }
    case EXPR_CAST: {
        Type *tgt = e->cast.target;
        int w = const_int_width(tgt);
        if (w == 0) return NULL;  /* non-fixed-int target: bool/char/float/isize/usize */
        ConstScalar a;
        if (!const_read_scalar(e->cast.operand, &a)) return NULL;
        bool s = type_is_signed(tgt);
        return const_make_int(ctx, tgt, const_mask_extend(a.val, w, s), e->loc);
    }
    case EXPR_BINARY: {
        TokenKind op = e->binary.op;
        ConstScalar a, b;
        if (!const_read_scalar(e->binary.left, &a)) return NULL;
        if (!const_read_scalar(e->binary.right, &b)) return NULL;

        switch (op) {
        case TOK_EQEQ: case TOK_BANGEQ:
        case TOK_LT: case TOK_GT: case TOK_LTEQ: case TOK_GTEQ: {
            if (e->type->kind != TYPE_BOOL) return NULL;
            /* Operands share a common type post-widening; use left's signedness. */
            bool res;
            if (a.is_signed) {
                int64_t la = (int64_t)a.val, lb = (int64_t)b.val;
                switch (op) {
                case TOK_EQEQ:  res = la == lb; break;
                case TOK_BANGEQ:res = la != lb; break;
                case TOK_LT:    res = la <  lb; break;
                case TOK_GT:    res = la >  lb; break;
                case TOK_LTEQ:  res = la <= lb; break;
                default:        res = la >= lb; break;
                }
            } else {
                uint64_t la = a.val, lb = b.val;
                switch (op) {
                case TOK_EQEQ:  res = la == lb; break;
                case TOK_BANGEQ:res = la != lb; break;
                case TOK_LT:    res = la <  lb; break;
                case TOK_GT:    res = la >  lb; break;
                case TOK_LTEQ:  res = la <= lb; break;
                default:        res = la >= lb; break;
                }
            }
            return const_make_bool(ctx, e->type, res, e->loc);
        }
        case TOK_AMPAMP:
            return const_make_bool(ctx, e->type, (a.val != 0) && (b.val != 0), e->loc);
        case TOK_PIPEPIPE:
            return const_make_bool(ctx, e->type, (a.val != 0) || (b.val != 0), e->loc);
        default: break;
        }

        int w = const_int_width(e->type);
        if (w == 0) return NULL;
        bool s = type_is_signed(e->type);
        uint64_t lv = const_mask_extend(a.val, w, s);
        uint64_t rv = const_mask_extend(b.val, w, s);
        uint64_t shamt = (uint64_t)(w - 1);
        uint64_t r;
        switch (op) {
        case TOK_PLUS:  r = lv + rv; break;
        case TOK_MINUS: r = lv - rv; break;
        case TOK_STAR:  r = lv * rv; break;
        case TOK_AMP:   r = lv & rv; break;
        case TOK_PIPE:  r = lv | rv; break;
        case TOK_CARET: r = lv ^ rv; break;
        case TOK_LTLT:  r = lv << (rv & shamt); break;
        case TOK_GTGT:
            if (s) r = (uint64_t)((int64_t)lv >> (rv & shamt));
            else   r = lv >> (rv & shamt);
            break;
        case TOK_SLASH:
        case TOK_PERCENT:
            if (rv == 0) {
                diag_error(e->loc, "division by zero in constant expression");
                return NULL;
            }
            if (s) {
                int64_t la = (int64_t)lv, ra = (int64_t)rv;
                if (ra == -1)  /* INT_MIN/-1 and x%-1: avoid UB in fcc itself */
                    r = (op == TOK_SLASH) ? ((uint64_t)0 - lv) : 0;
                else
                    r = (op == TOK_SLASH) ? (uint64_t)(la / ra) : (uint64_t)(la % ra);
            } else {
                r = (op == TOK_SLASH) ? (lv / rv) : (lv % rv);
            }
            break;
        default:
            return NULL;
        }
        return const_make_int(ctx, e->type, const_mask_extend(r, w, s), e->loc);
    }
    default:
        return NULL;
    }
}

/* Recursively fold an Expr in const-expr position.  Returns the folded tree
 * (same pointer if no substitution) or NULL on failure.  NULL is silent for
 * most kinds (the outer gate emits the generic "must be a constant
 * expression"); a specific diagnostic is emitted only for the mut-ref and
 * division-by-zero cases. */
static Expr *const_fold_expr(CheckCtx *ctx, Expr *e) {
    if (!e) return e;
    switch (e->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT:
    case EXPR_SIZEOF:
    case EXPR_ALIGNOF:
    case EXPR_DEFAULT:
        return e;
    case EXPR_IDENT: {
        Symbol *s = e->ident.resolved_sym;
        if (!s || !s->decl || s->decl->kind != DECL_LET)
            return NULL;
        if (!s->decl->let.is_module_member)
            return NULL;
        if (s->decl->let.is_mut) {
            diag_error(e->loc,
                "cannot reference mutable binding '%s' in constant expression",
                e->ident.name);
            return NULL;
        }
        Decl *d = s->decl;
        if (d->let.const_fold_state == CONST_FOLD_DONE)
            return const_clone_expr(ctx, d->let.const_fold_value);
        if (d->let.const_fold_state == CONST_FOLD_VISITING) {
            /* Unreachable in valid pipelines — any cycle would have been
             * caught earlier by on-demand type-check cycle detection. */
            diag_fatal(e->loc,
                "internal: const-fold reentered let '%s' (missed type-level cycle)",
                d->let.name);
        }
        if (d->let.const_fold_state == CONST_FOLD_FAILED)
            return NULL;
        /* UNVISITED — recurse into the target's init */
        d->let.const_fold_state = CONST_FOLD_VISITING;
        Expr *folded = const_fold_expr(ctx, d->let.init);
        if (!folded || !is_const_expr(folded)) {
            d->let.const_fold_state = CONST_FOLD_FAILED;
            return NULL;
        }
        d->let.init = folded;
        d->let.const_fold_value = folded;
        d->let.const_fold_state = CONST_FOLD_DONE;
        return const_clone_expr(ctx, folded);
    }
    case EXPR_UNARY_PREFIX: {
        Expr *op = const_fold_expr(ctx, e->unary_prefix.operand);
        if (!op) return NULL;
        Expr *node;
        if (op == e->unary_prefix.operand) {
            node = e;
        } else {
            node = arena_alloc(ctx->arena, sizeof(Expr));
            *node = *e;
            node->unary_prefix.operand = op;
        }
        Expr *v = try_eval_const(ctx, node);
        return v ? v : node;
    }
    case EXPR_BINARY: {
        Expr *l = const_fold_expr(ctx, e->binary.left);
        Expr *r = const_fold_expr(ctx, e->binary.right);
        if (!l || !r) return NULL;
        Expr *node;
        if (l == e->binary.left && r == e->binary.right) {
            node = e;
        } else {
            node = arena_alloc(ctx->arena, sizeof(Expr));
            *node = *e;
            node->binary.left = l;
            node->binary.right = r;
        }
        Expr *v = try_eval_const(ctx, node);
        return v ? v : node;
    }
    case EXPR_CAST: {
        Expr *op = const_fold_expr(ctx, e->cast.operand);
        if (!op) return NULL;
        Expr *node;
        if (op == e->cast.operand) {
            node = e;
        } else {
            node = arena_alloc(ctx->arena, sizeof(Expr));
            *node = *e;
            node->cast.operand = op;
        }
        Expr *v = try_eval_const(ctx, node);
        return v ? v : node;
    }
    case EXPR_FIELD:
        if (e->field.is_type_property) {
            Expr *v = const_fold_type_property(ctx, e);
            return v ? v : e;  /* host-foldable → literal; else keep the macro node */
        }
        if (e->field.is_extern_const || e->field.is_variant_constructor)
            return e;
        return NULL;
    case EXPR_STRUCT_LIT: {
        bool changed = false;
        int fc = e->struct_lit.field_count;
        Expr **new_vals = NULL;
        for (int i = 0; i < fc; i++) {
            Expr *nv = const_fold_expr(ctx, e->struct_lit.fields[i].value);
            if (!nv) return NULL;
            if (nv != e->struct_lit.fields[i].value) {
                if (!new_vals) {
                    new_vals = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)fc);
                    for (int k = 0; k < i; k++)
                        new_vals[k] = e->struct_lit.fields[k].value;
                }
                new_vals[i] = nv;
                changed = true;
            } else if (new_vals) {
                new_vals[i] = nv;
            }
        }
        if (!changed) return e;
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *e;
        FieldInit *fields = arena_alloc(ctx->arena, sizeof(FieldInit) * (size_t)fc);
        for (int i = 0; i < fc; i++) {
            fields[i] = e->struct_lit.fields[i];
            fields[i].value = new_vals[i];
        }
        n->struct_lit.fields = fields;
        return n;
    }
    case EXPR_TUPLE_LIT: {
        bool changed = false;
        int ec = e->tuple_lit.elem_count;
        Expr **new_elems = NULL;
        for (int i = 0; i < ec; i++) {
            Expr *nv = const_fold_expr(ctx, e->tuple_lit.elems[i]);
            if (!nv) return NULL;
            if (nv != e->tuple_lit.elems[i] && !new_elems) {
                new_elems = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)ec);
                for (int k = 0; k < i; k++) new_elems[k] = e->tuple_lit.elems[k];
            }
            if (new_elems) new_elems[i] = nv;
            if (nv != e->tuple_lit.elems[i]) changed = true;
        }
        if (!changed) return e;
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *e;
        n->tuple_lit.elems = new_elems;
        return n;
    }
    case EXPR_ARRAY_LIT: {
        bool changed = false;
        int ec = e->array_lit.elem_count;
        Expr **new_elems = NULL;
        for (int i = 0; i < ec; i++) {
            Expr *ne = const_fold_expr(ctx, e->array_lit.elems[i]);
            if (!ne) return NULL;
            if (ne != e->array_lit.elems[i]) {
                if (!new_elems) {
                    new_elems = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)ec);
                    for (int k = 0; k < i; k++)
                        new_elems[k] = e->array_lit.elems[k];
                }
                new_elems[i] = ne;
                changed = true;
            } else if (new_elems) {
                new_elems[i] = ne;
            }
        }
        Expr *new_size = e->array_lit.size_expr
            ? const_fold_expr(ctx, e->array_lit.size_expr)
            : NULL;
        if (e->array_lit.size_expr && !new_size) return NULL;
        if (!changed && new_size == e->array_lit.size_expr) return e;
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *e;
        n->array_lit.codegen_backing_name = NULL;
        if (new_elems) n->array_lit.elems = new_elems;
        n->array_lit.size_expr = new_size;
        return n;
    }
    case EXPR_SLICE_LIT: {
        Expr *p = const_fold_expr(ctx, e->slice_lit.ptr_expr);
        Expr *l = const_fold_expr(ctx, e->slice_lit.len_expr);
        if (!p || !l) return NULL;
        /* Const-context slice literals are emitted as C file-scope initializers
         * with no runtime guard, so a negative len must be caught statically.
         * A plain (possibly widened) integer literal was already validated in
         * check_expr; only report here when folding a non-literal const
         * expression (e.g. `0 - 1`, or a reference to a negative const) yields a
         * negative value, to avoid a duplicate diagnostic. */
        if (!slicelit_len_is_literal(e->slice_lit.len_expr)) {
            ConstScalar cs;
            if (const_read_scalar(l, &cs) && cs.is_signed && (int64_t)cs.val < 0)
                diag_error(e->slice_lit.len_expr->loc,
                    "slice literal length cannot be negative");
        }
        if (p == e->slice_lit.ptr_expr && l == e->slice_lit.len_expr) return e;
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *e;
        n->slice_lit.ptr_expr = p;
        n->slice_lit.len_expr = l;
        return n;
    }
    case EXPR_SOME: {
        Expr *v = const_fold_expr(ctx, e->some_expr.value);
        if (!v) return NULL;
        /* A null-sentinel some(p) compiles to a runtime null-guard
         * (statement-expression), which is not a valid C file-scope constant
         * initializer.  A provably-non-null payload emits the bare pointer (a
         * valid constant) and is fine; reject anything else in const context
         * with a targeted message (a provably-null payload already errored in
         * check_expr). */
        if (e->type && e->type->kind == TYPE_OPTION && e->type->option.inner &&
            (e->type->option.inner->kind == TYPE_POINTER ||
             e->type->option.inner->kind == TYPE_ANY_PTR) &&
            !ptr_value_provably_nonnull(v)) {
            diag_error(e->loc,
                "some() of a possibly-null pointer is not allowed in a constant "
                "initializer; wrap a provably non-null pointer or initialize at "
                "runtime");
            return NULL;
        }
        if (v == e->some_expr.value) return e;
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *e;
        n->some_expr.value = v;
        return n;
    }
    case EXPR_CALL: {
        if (e->call.func->kind != EXPR_FIELD ||
            !e->call.func->field.is_variant_constructor)
            return NULL;
        bool changed = false;
        int ac = e->call.arg_count;
        Expr **new_args = NULL;
        for (int i = 0; i < ac; i++) {
            Expr *na = const_fold_expr(ctx, e->call.args[i]);
            if (!na) return NULL;
            if (na != e->call.args[i]) {
                if (!new_args) {
                    new_args = arena_alloc(ctx->arena, sizeof(Expr*) * (size_t)ac);
                    for (int k = 0; k < i; k++)
                        new_args[k] = e->call.args[k];
                }
                new_args[i] = na;
                changed = true;
            } else if (new_args) {
                new_args[i] = na;
            }
        }
        if (!changed) return e;
        Expr *n = arena_alloc(ctx->arena, sizeof(Expr));
        *n = *e;
        n->call.args = new_args;
        return n;
    }
    default:
        return NULL;
    }
}

/* Check if an expression is a compile-time constant (no variable refs or calls) */
static bool is_const_expr(Expr *e) {
    if (!e) return true;
    switch (e->kind) {
    /* Literals — always valid C constants */
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT:
        return true;
    /* Type operators — compile-time constants in C */
    case EXPR_SIZEOF:
    case EXPR_ALIGNOF:
    case EXPR_DEFAULT:
        return true;
    /* Unary prefix — only negate and boolean-not emit simple C infix */
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_MINUS || e->unary_prefix.op == TOK_BANG)
            return is_const_expr(e->unary_prefix.operand);
        return false;
    /* Binary — safelist operators that emit simple C infix at file scope.
     * Integer div/mod emit zero-check statement expressions.
     * Equality on aggregate types emits generated comparison function calls. */
    case EXPR_BINARY:
        switch (e->binary.op) {
        case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
        case TOK_LTLT: case TOK_GTGT:
        case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        case TOK_AMPAMP: case TOK_PIPEPIPE:
        case TOK_LT: case TOK_GT: case TOK_LTEQ: case TOK_GTEQ:
            return is_const_expr(e->binary.left) &&
                   is_const_expr(e->binary.right);
        case TOK_SLASH: case TOK_PERCENT:
            /* Float div/mod emits simple infix; integer emits zero-check */
            if (e->type && type_is_integer(e->type)) return false;
            return is_const_expr(e->binary.left) &&
                   is_const_expr(e->binary.right);
        case TOK_EQEQ: case TOK_BANGEQ:
            /* Primitive equality is simple infix; aggregate types emit
             * generated comparison function calls */
            if (e->binary.left->type &&
                type_needs_eq_func(e->binary.left->type))
                return false;
            return is_const_expr(e->binary.left) &&
                   is_const_expr(e->binary.right);
        default:
            return false;
        }
    /* Cast — simple type casts are fine; str↔cstr emit statement exprs */
    case EXPR_CAST:
        if (e->cast.operand->type &&
            ((is_str_type(e->cast.operand->type) && is_cstr_type(e->cast.target)) ||
             (is_cstr_type(e->cast.operand->type) && is_str_type(e->cast.target))))
            return false;
        return is_const_expr(e->cast.operand);
    /* Extern constants — C macros/enums are compile-time constants.
     * Static type properties (int32.min, float64.nan, ...) emit C macros or
     * folded literals — all valid C constant expressions.
     * No-payload variant constructors also emit plain compound literals. */
    case EXPR_FIELD:
        return e->field.is_extern_const || e->field.is_variant_constructor ||
               e->field.is_type_property;
    /* Struct literal — valid if all field values are const */
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (!is_const_expr(e->struct_lit.fields[i].value)) return false;
        return true;
    /* Tuple literal — valid if all elements are const (a plain compound literal) */
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            if (!is_const_expr(e->tuple_lit.elems[i])) return false;
        return true;
    /* Array literal — valid if all elements and size are const.  Codegen
     * lifts the backing array to file scope in const context. */
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            if (!is_const_expr(e->array_lit.elems[i])) return false;
        return is_const_expr(e->array_lit.size_expr);
    /* Slice literal — valid if both ptr and len are const.  In practice
     * ptr_expr is always an EXPR_ARRAY_LIT at module scope, which is handled
     * above; other ptr forms (ident/&expr) are rejected by their own cases. */
    case EXPR_SLICE_LIT:
        return is_const_expr(e->slice_lit.ptr_expr) &&
               is_const_expr(e->slice_lit.len_expr);
    /* some(x) — valid if payload is const.  Emits a plain compound literal. */
    case EXPR_SOME:
        return is_const_expr(e->some_expr.value);
    /* Union variant constructor with payload — valid if all args are const. */
    case EXPR_CALL:
        if (e->call.func->kind == EXPR_FIELD &&
            e->call.func->field.is_variant_constructor) {
            for (int i = 0; i < e->call.arg_count; i++)
                if (!is_const_expr(e->call.args[i])) return false;
            return true;
        }
        return false;
    /* Everything else is rejected by default */
    default:
        return false;
    }
}

/* Check if an expression is valid in a file-level initializer.
 * More permissive than is_const_expr: allows alloc, some, unwrap, array/slice
 * literals, and union variant constructors, but still disallows FC function
 * calls and variable references.  Called after type-checking so e->type is set. */
static bool is_file_init_expr(Expr *e) {
    if (!e) return true;
    switch (e->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT:
    case EXPR_SIZEOF:
    case EXPR_ALIGNOF:
    case EXPR_DEFAULT:
        return true;
    case EXPR_UNARY_PREFIX:
        /* Only negate (-) and boolean not (!) are safe; deref (*) and
         * address-of (&) are not valid in init context. */
        if (e->unary_prefix.op != TOK_MINUS && e->unary_prefix.op != TOK_BANG)
            return false;
        return is_file_init_expr(e->unary_prefix.operand);
    case EXPR_UNARY_POSTFIX:
        return is_file_init_expr(e->unary_postfix.operand);
    case EXPR_BINARY:
        return is_file_init_expr(e->binary.left) && is_file_init_expr(e->binary.right);
    case EXPR_CAST:
        return is_file_init_expr(e->cast.operand);
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (!is_file_init_expr(e->struct_lit.fields[i].value)) return false;
        return true;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            if (!is_file_init_expr(e->tuple_lit.elems[i])) return false;
        return true;
    case EXPR_ALLOC:
        return is_file_init_expr(e->alloc_expr.size_expr) &&
               is_file_init_expr(e->alloc_expr.init_expr);
    case EXPR_SOME:
        return is_file_init_expr(e->some_expr.value);
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            if (!is_file_init_expr(e->array_lit.elems[i])) return false;
        return is_file_init_expr(e->array_lit.size_expr);
    case EXPR_SLICE_LIT:
        return is_file_init_expr(e->slice_lit.ptr_expr) &&
               is_file_init_expr(e->slice_lit.len_expr);
    case EXPR_CALL:
        /* Allow union variant constructors only */
        if (e->type && e->type->kind == TYPE_UNION &&
            e->call.func->kind == EXPR_FIELD) {
            for (int i = 0; i < e->call.arg_count; i++)
                if (!is_file_init_expr(e->call.args[i])) return false;
            return true;
        }
        return false;
    case EXPR_FIELD:
        /* Allow no-payload union variant constructors */
        if (e->type && e->type->kind == TYPE_UNION)
            return true;
        /* Allow extern constants (C macros/enums) and static type properties
         * (int32.min, float64.nan, ...) */
        if (e->field.is_extern_const || e->field.is_type_property)
            return true;
        return false;
    default:
        return false;
    }
}

static void check_decl_let(CheckCtx *ctx, Decl *d) {
    /* For function declarations, pre-register a partial function type
     * so the body can make recursive calls. */
    const char *lookup_name = d->let.name;
    Symbol *sym = resolve_symbol(ctx, lookup_name);

    Type *recursive_ret = NULL;
    if (d->let.init && d->let.init->kind == EXPR_FUNC && sym && !sym->type) {
        /* Build a partial function type with params known, return type placeholder.
         * Allocate the return type as a mutable cell; after body checking we
         * overwrite it in-place so all references (including recursive call sites)
         * see the resolved return type. */
        Expr *fn = d->let.init;
        int pc = fn->func.param_count;
        Type **ptypes = NULL;
        if (pc > 0)
            ptypes = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)pc);
        for (int i = 0; i < pc; i++)
            ptypes[i] = resolve_type(ctx, fn->func.params[i].type);

        recursive_ret = malloc(sizeof(Type));
        memset(recursive_ret, 0, sizeof(Type));
        recursive_ret->kind = TYPE_UNRESOLVED;  /* placeholder */

        Type *ft = arena_alloc(ctx->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_types = ptypes;
        ft->func.param_count = pc;
        ft->func.return_type = recursive_ret;
        sym->type = ft;
    }

    bool saved_top = ctx->is_top_level_init;
    if (d->let.init && d->let.init->kind == EXPR_FUNC)
        ctx->is_top_level_init = true;

    /* Hand the placeholder to the function's own EXPR_FUNC via the recursion
       channel (consumed there, which scopes it to that body — see EXPR_FUNC). */
    Type *saved_prr = ctx->pending_recursive_ret;
    const char *saved_prs = ctx->pending_recursive_self;
    ctx->pending_recursive_ret = recursive_ret;
    ctx->pending_recursive_self = recursive_ret ? d->let.name : NULL;
    Type *t = check_expr(ctx, d->let.init);
    ctx->pending_recursive_ret = saved_prr;
    ctx->pending_recursive_self = saved_prs;
    ctx->is_top_level_init = saved_top;

    /* If we pre-registered a recursive function type, patch the return type */
    if (recursive_ret) {
        Type *actual_ret = t->kind == TYPE_FUNC ? t->func.return_type : t;
        *recursive_ret = *actual_ret;
    }

    d->let.resolved_type = t;
    if (sym) sym->type = t;
    /* Add to scope so later decls can reference it */
    const char *cg_name = d->let.codegen_name ? d->let.codegen_name : d->let.name;
    scope_add(ctx->scope, d->let.name, cg_name, t, d->let.is_mut);
}

/* Recursively type-check module members, including arbitrarily nested submodules.
 * parent_members is the symbol table to look up submodule symbols in. */
static void check_module_members(CheckCtx *ctx, Decl *mod_decl,
                                 SymbolTable *parent_members) {
    for (int i = 0; i < mod_decl->module.decl_count; i++) {
        Decl *child = mod_decl->module.decls[i];
        if (child->kind == DECL_LET) {
            check_decl_let(ctx, child);
            /* Skip the const-expr gate if type-checking already errored
             * (e.g. type-level cycle, undefined name); otherwise fold would
             * emit a second, redundant diagnostic. */
            bool type_ok = child->let.resolved_type &&
                           child->let.resolved_type->kind != TYPE_ERROR;
            if (type_ok &&
                child->let.init && child->let.init->kind != EXPR_FUNC &&
                child->let.const_fold_state != CONST_FOLD_DONE &&
                child->let.const_fold_state != CONST_FOLD_FAILED) {
                child->let.const_fold_state = CONST_FOLD_VISITING;
                int errs_before = diag_error_count();
                Expr *folded = const_fold_expr(ctx, child->let.init);
                int new_errs = diag_error_count() - errs_before;
                if (folded && is_const_expr(folded)) {
                    child->let.init = folded;
                    child->let.const_fold_value = folded;
                    child->let.const_fold_state = CONST_FOLD_DONE;
                } else {
                    child->let.const_fold_state = CONST_FOLD_FAILED;
                    if (new_errs == 0) {
                        diag_error(child->loc,
                            "top-level initializer for '%s' must be a constant expression",
                            child->let.name);
                    }
                }
            }
        } else if (child->kind == DECL_STRUCT || child->kind == DECL_UNION) {
            canonicalize_decl_field_stubs(ctx, child);
        } else if (child->kind == DECL_MODULE) {
            Symbol *sub_sym = symtab_lookup_kind(parent_members,
                child->module.name, DECL_MODULE);
            if (sub_sym && sub_sym->members) {
                SymbolTable *saved_symtab = ctx->module_symtab;
                ModuleScopeChain *saved_parents = ctx->parent_modules;
                Scope *saved_scope = ctx->scope;
                ImportScope *saved_imports = ctx->import_scope;

                /* Push sub-module's imports onto chain */
                ImportScope sub_import_scope = { .table = sub_sym->imports, .parent = ctx->import_scope };
                if (sub_sym->imports) ctx->import_scope = &sub_import_scope;

                /* Push current module onto parent chain (use saved_imports, not
                 * ctx->import_scope which already has the child's imports pushed) */
                ModuleScopeChain parent_link = { .members = ctx->module_symtab, .import_scope = saved_imports, .parent = ctx->parent_modules };
                if (ctx->module_symtab) ctx->parent_modules = &parent_link;
                ctx->module_symtab = sub_sym->members;
                ctx->scope = scope_new(ctx->arena, ctx->scope);
                ctx->scope->is_global = true;
                check_module_members(ctx, child, sub_sym->members);
                ctx->scope = saved_scope;
                ctx->module_symtab = saved_symtab;
                ctx->parent_modules = saved_parents;
                ctx->import_scope = saved_imports;
            }
        }
    }
}

/* ---- Infinite-size (by-value recursive type) detection ----
 *
 * A struct or union that contains itself by value — directly or through a chain
 * of other by-value types — has no finite size and cannot be emitted as valid C.
 * The spec mandates a compile error for this. We build the by-value containment
 * graph over all (non-extern) struct/union declarations and report a cycle.
 *
 * Pointers and slices break the cycle (they are fixed-size indirections), so the
 * common `next: node*?` / `children: node[]` recursive patterns are fine. Fixed
 * inline arrays and options of a value type DO propagate by-value containment. */

/* Name of the struct/union a type embeds BY VALUE, or NULL (pointers, slices,
 * primitives, functions, and options-of-pointer carry no by-value UDT). */
static const char *a5_byval_name(Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
    case TYPE_STUB:        return t->stub.name;
    case TYPE_STRUCT:      return t->struc.name;
    case TYPE_UNION:       return t->unio.name;
    case TYPE_FIXED_ARRAY: return a5_byval_name(t->fixed_array.elem);
    case TYPE_OPTION:      return a5_byval_name(t->option.inner);
    default:               return NULL;  /* pointer, slice, func, primitives */
    }
}

static void a5_collect(Decl **decls, int count, Decl ***list, int *n, int *cap) {
    for (int i = 0; i < count; i++) {
        Decl *d = decls[i];
        if (d->kind == DECL_STRUCT) { if (!d->struc.is_extern) DA_APPEND(*list, *n, *cap, d); }
        else if (d->kind == DECL_UNION) DA_APPEND(*list, *n, *cap, d);
        else if (d->kind == DECL_MODULE) a5_collect(d->module.decls, d->module.decl_count, list, n, cap);
    }
}

/* Resolve a by-value reference name to a unique UDT index. Returns -1 if the
 * name matches no UDT or more than one (ambiguous across modules/namespaces —
 * skipped conservatively so we never reject a valid program). */
static int a5_find(Decl **udts, int n, const char *name) {
    int found = -1;
    for (int i = 0; i < n; i++) {
        const char *un = udts[i]->kind == DECL_STRUCT ? udts[i]->struc.name : udts[i]->unio.name;
        if (un == name) { if (found >= 0) return -1; found = i; }
    }
    return found;
}

/* DFS over by-value edges; on a back-edge to a node on the current stack, report
 * an infinite-size cycle. state: 0=unvisited, 1=on-stack, 2=done. */
static void a5_visit(Decl **udts, int n, int *state, int idx) {
    state[idx] = 1;
    Decl *d = udts[idx];
    if (d->kind == DECL_STRUCT) {
        for (int f = 0; f < d->struc.field_count; f++) {
            const char *tn = a5_byval_name(d->struc.fields[f].type);
            if (!tn) continue;
            int j = a5_find(udts, n, tn);
            if (j < 0) continue;
            if (state[j] == 1) {
                diag_error(d->loc, "type '%s' has infinite size: field '%s' contains "
                    "'%s' by value, forming a cycle; use a pointer or slice to break it",
                    d->struc.name, d->struc.fields[f].name, tn);
            } else if (state[j] == 0) {
                a5_visit(udts, n, state, j);
            }
        }
    } else { /* DECL_UNION */
        for (int v = 0; v < d->unio.variant_count; v++) {
            const char *tn = a5_byval_name(d->unio.variants[v].payload);
            if (!tn) continue;
            int j = a5_find(udts, n, tn);
            if (j < 0) continue;
            if (state[j] == 1) {
                diag_error(d->loc, "type '%s' has infinite size: variant '%s' contains "
                    "'%s' by value, forming a cycle; use a pointer or slice to break it",
                    d->unio.name, d->unio.variants[v].name, tn);
            } else if (state[j] == 0) {
                a5_visit(udts, n, state, j);
            }
        }
    }
    state[idx] = 2;
}

static void check_infinite_size(Program *prog) {
    Decl **udts = NULL;
    int n = 0, cap = 0;
    a5_collect(prog->decls, prog->decl_count, &udts, &n, &cap);
    if (n == 0) { free(udts); return; }
    int *state = calloc((size_t) n, sizeof(int));
    for (int i = 0; i < n; i++)
        if (state[i] == 0) a5_visit(udts, n, state, i);
    free(state);
    free(udts);
}

void pass2_check(Program *prog, SymbolTable *symtab, InternTable *intern_tbl, MonoTable *mono,
                 FileImportScopes *file_scopes) {
    Arena arena;
    arena_init(&arena);

    Scope *root_scope = scope_new(&arena, NULL);
    root_scope->is_global = true;

    CheckCtx ctx = {
        .symtab = symtab,
        .scope = root_scope,
        .arena = &arena,
        .loop_break_type = NULL,
        .in_for = false,
        .module_symtab = NULL,
        .current_ns = NULL,
        .recursive_ret = NULL,
        .lambda_ctx = NULL,
        .is_top_level_init = false,
        .mono_table = mono,
        .intern = intern_tbl,
        .import_scope = NULL,
        .on_demand_visited = NULL,
        .file_scopes = file_scopes,
    };

    /* First pass: type-check all module member decls (including nested submodules) */
    const char *ns_tracker = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            ns_tracker = d->ns.name;
            continue;
        }
        if (d->kind != DECL_MODULE) continue;
        Symbol *mod_sym = symtab_lookup_module(symtab, d->module.name,
            d->module.ns_prefix ? d->module.ns_prefix : ns_tracker);
        if (!mod_sym || !mod_sym->members) continue;
        ctx.current_ns = mod_sym->ns_prefix;
        SymbolTable *saved_mod = ctx.module_symtab;
        Scope *saved_scope = ctx.scope;
        ImportScope *saved_imports = ctx.import_scope;

        /* Build import scope chain: module imports -> file imports */
        ImportTable *file_tbl = NULL;
        if (file_scopes) {
            const char *fn = d->loc.filename;
            for (int fi = 0; fi < file_scopes->count; fi++) {
                if (file_scopes->scopes[fi].filename == fn) {
                    file_tbl = &file_scopes->scopes[fi].imports;
                    break;
                }
            }
        }
        ImportScope file_import_scope = { .table = file_tbl, .parent = NULL };
        ImportScope mod_import_scope = { .table = mod_sym->imports, .parent = &file_import_scope };
        ctx.import_scope = mod_sym->imports ? &mod_import_scope : (file_tbl ? &file_import_scope : NULL);

        ctx.module_symtab = mod_sym->members;
        ctx.scope = scope_new(&arena, NULL);
        ctx.scope->is_global = true;
        check_module_members(&ctx, d, mod_sym->members);
        ctx.scope = saved_scope;
        ctx.module_symtab = saved_mod;
        ctx.import_scope = saved_imports;
    }

    /* Update imported symbols' types from resolved module members */
    for (int i = 0; i < symtab->count; i++) {
        Symbol *gsym = &symtab->symbols[i];
        /* Skip non-imported symbols (those with no decl or that aren't DECL_LET from a module) */
        if (!gsym->decl) continue;
        if (gsym->kind == DECL_LET && !gsym->type && gsym->decl->let.codegen_name) {
            /* This is likely an imported let that needs its type resolved */
            /* Find it in any module's member table */
            for (int j = 0; j < symtab->count; j++) {
                Symbol *mod = &symtab->symbols[j];
                if (mod->kind != DECL_MODULE || !mod->members) continue;
                for (int k = 0; k < mod->members->count; k++) {
                    Symbol *msym = &mod->members->symbols[k];
                    if (msym->decl == gsym->decl && msym->type) {
                        gsym->type = msym->type;
                        goto next_sym;
                    }
                }
            }
            next_sym:;
        }
    }

    /* Second pass: type-check top-level (non-module) decls.
     *
     * Top-level lets are treated like module members: they take priority over
     * file-level imports, consistent with the uniform rule that members beat
     * imports at every level. */
    ctx.current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            ctx.current_ns = d->ns.name;
            continue;
        }
        if (d->kind == DECL_LET || d->kind == DECL_STRUCT || d->kind == DECL_UNION) {
            /* Set up file-level import scope for this decl's file */
            const char *fn = d->loc.filename;
            ImportTable *file_tbl = NULL;
            if (file_scopes) {
                for (int fi = 0; fi < file_scopes->count; fi++) {
                    if (file_scopes->scopes[fi].filename == fn) {
                        file_tbl = &file_scopes->scopes[fi].imports;
                        break;
                    }
                }
            }
            ImportScope file_import_scope = { .table = file_tbl, .parent = NULL };
            ctx.import_scope = file_tbl ? &file_import_scope : NULL;

            if (d->kind == DECL_LET) {
                check_decl_let(&ctx, d);
                if (d->let.init && d->let.init->kind != EXPR_FUNC &&
                    !is_file_init_expr(d->let.init)) {
                    diag_error(d->loc,
                        "file-level initializer for '%s' must not contain "
                        "function calls or variable references",
                        d->let.name);
                }
            } else {
                canonicalize_decl_field_stubs(&ctx, d);
            }
            ctx.import_scope = NULL;
        }
    }

    /* Validate main function signature: must take str[] and return int32 */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind != DECL_LET || !d->let.init) continue;
        if (strcmp(d->let.name, "main") != 0) continue;
        if (d->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = d->let.init;
        Type *ft = d->let.resolved_type;
        if (!ft || ft->kind != TYPE_FUNC) break;
        if (type_is_error(ft)) break;
        /* Check return type is int32 */
        if (ft->func.return_type && !type_is_error(ft->func.return_type) &&
            !type_eq(ft->func.return_type, type_int32())) {
            diag_error(d->loc, "main must return int32");
        }
        /* Check exactly one parameter of type str[] (slice of str = slice of uint8[]) */
        if (fn->func.param_count != 1) {
            diag_error(d->loc, "main must take exactly one parameter of type str[]");
        } else {
            Type *pt = fn->func.params[0].type;
            bool is_str_slice = pt && pt->kind == TYPE_SLICE &&
                                is_str_type(pt->slice.elem);
            if (!is_str_slice) {
                diag_error(d->loc, "main parameter must be str[], got %s",
                    type_name(pt));
            }
        }
        break;
    }

    check_infinite_size(prog);

    /* Don't free arena — types are referenced from AST */
}
