#include "monomorph.h"
#include "types.h"
#include "diag.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Monomorphization termination guards (audit item 14).
 *
 * A generic that instantiates itself with an ever-growing type argument produces
 * an infinite family of monomorphized copies — e.g. `f(some(x))` forces
 * f<'a> -> f<'a?> -> f<'a??> -> ..., and a non-uniform recursive type forces
 * node<int32> -> node<node<int32>> -> ... Both fixpoint loops below (function
 * discovery in mono_discover_transitive, nested-type discovery in
 * mono_finalize_types) would otherwise never converge (hang) or, when a name
 * collision happens to halt the loop, silently emit a truncated/dangling C type.
 *
 * The divergence always manifests as type arguments that nest one constructor
 * deeper each round, so a cap on the structural depth of an instance's type
 * arguments catches it regardless of shape. Because FC has no type-level
 * computation (array sizes, tuple arities, etc. are fixed by source, never
 * synthesized), a bounded type-argument depth admits only finitely many distinct
 * types — so the depth cap alone *guarantees* termination. The count cap is a
 * belt-and-suspenders backstop against any breadth-divergence shape not foreseen
 * here. Both limits are far above anything a finite program reaches (real generic
 * nesting is a handful of levels deep; whole programs have thousands, not
 * hundreds of thousands, of instances). */
#define MONO_MAX_INSTANTIATION_DEPTH 128
#define MONO_MAX_INSTANCES           200000

/* Structural nesting depth of a type's *type arguments*: the axis along which a
 * divergent instantiation grows. Recurses through wrapper constructors and into
 * generic type arguments, but NOT into struct/union fields (those are bounded by
 * the definition; only the args grow during divergence) — which also keeps this
 * walk finite on by-value-recursive concrete types. */
static int mono_type_arg_depth(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_POINTER:     return 1 + mono_type_arg_depth(t->pointer.pointee);
    case TYPE_SLICE:       return 1 + mono_type_arg_depth(t->slice.elem);
    case TYPE_OPTION:      return 1 + mono_type_arg_depth(t->option.inner);
    case TYPE_FIXED_ARRAY: return 1 + mono_type_arg_depth(t->fixed_array.elem);
    case TYPE_FUNC: {
        int m = mono_type_arg_depth(t->func.return_type);
        for (int i = 0; i < t->func.param_count; i++) {
            int d = mono_type_arg_depth(t->func.param_types[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    case TYPE_STRUCT: {
        int m = 0;
        for (int i = 0; i < t->struc.type_arg_count; i++) {
            int d = mono_type_arg_depth(t->struc.type_args[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    case TYPE_UNION: {
        int m = 0;
        for (int i = 0; i < t->unio.type_arg_count; i++) {
            int d = mono_type_arg_depth(t->unio.type_args[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    case TYPE_STUB: {
        int m = 0;
        for (int i = 0; i < t->stub.type_arg_count; i++) {
            int d = mono_type_arg_depth(t->stub.type_args[i]);
            if (d > m) m = d;
        }
        return 1 + m;
    }
    default: return 1; /* primitives, type vars, any* */
    }
}

const char *mono_register(MonoTable *t, Arena *a, InternTable *intern_tbl,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count) {
    /* Once an infinite instantiation has been reported, stop growing the table so
     * both discovery fixpoint loops converge (their guards key off t->count). The
     * single diagnostic below is thus emitted exactly once. */
    if (diag_error_count() > 0) return name;

    /* Build the base name for mangling */
    const char *base = name;
    if (ns_prefix) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s__%s", ns_prefix, name);
        base = intern_cstr(intern_tbl, buf);
    }
    const char *mangled = mangle_generic_name(a, intern_tbl, base, type_args, count);

    /* Dedup: check if already registered */
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].mangled_name == mangled)
            return mangled;
    }

    /* Termination guard: a new instance whose type arguments nest deeper than any
     * finite program would means a generic is instantiating itself with a growing
     * type argument (infinite monomorphization). Report once and stop. */
    int arg_depth = 0;
    for (int i = 0; i < count; i++) {
        int d = mono_type_arg_depth(type_args[i]);
        if (d > arg_depth) arg_depth = d;
    }
    if (arg_depth > MONO_MAX_INSTANTIATION_DEPTH || t->count >= MONO_MAX_INSTANCES) {
        /* Prefer the source-level name over the mangled C name for the message. */
        const char *disp = name;
        SrcLoc loc = {0};
        if (tmpl) {
            loc = tmpl->loc;
            /* DECL_LET keeps its source name; struct/union Decl names are mangled
             * by pass1, so those fall back to the mangled name (rare path: the
             * direct-self type case is already caught at the definition site). */
            if (tmpl->kind == DECL_LET && tmpl->let.name)
                disp = tmpl->let.name;
            else if (tmpl->kind == DECL_STRUCT)
                disp = tmpl->struc.name;
            else if (tmpl->kind == DECL_UNION)
                disp = tmpl->unio.name;
        }
        diag_error(loc,
            "infinite generic instantiation of '%s': it is instantiated with an "
            "unbounded family of ever-deeper type arguments (exceeded depth %d / "
            "%d instances). A generic function or type that instantiates itself "
            "with a growing type argument (e.g. f(some(x)), or a non-uniform "
            "recursive type) requires infinitely many monomorphized copies.",
            disp, MONO_MAX_INSTANTIATION_DEPTH, MONO_MAX_INSTANCES);
        return mangled;
    }

    /* Copy type_args into arena */
    Type **args_copy = arena_alloc(a, sizeof(Type*) * (size_t)count);
    memcpy(args_copy, type_args, sizeof(Type*) * (size_t)count);

    /* Copy type_params into arena */
    const char **params_copy = NULL;
    if (tp_count > 0) {
        params_copy = arena_alloc(a, sizeof(const char*) * (size_t)tp_count);
        memcpy(params_copy, type_params, sizeof(const char*) * (size_t)tp_count);
    }

    MonoInstance inst = {
        .generic_name = name,
        .mangled_name = mangled,
        .ns_prefix = ns_prefix,
        .type_args = args_copy,
        .type_arg_count = count,
        .template_decl = tmpl,
        .decl_kind = kind,
        .concrete_type = NULL,
        .type_param_names = params_copy,
        .type_param_count = tp_count,
    };
    DA_APPEND(t->entries, t->count, t->capacity, inst);
    return mangled;
}

MonoInstance *mono_find(MonoTable *t, const char *mangled_name) {
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].mangled_name == mangled_name)
            return &t->entries[i];
    }
    return NULL;
}

void mono_resolve_type_names(MonoTable *t, Arena *a, InternTable *intern, Type *type) {
    if (!type) return;
    switch (type->kind) {
    case TYPE_POINTER: mono_resolve_type_names(t, a, intern, type->pointer.pointee); return;
    case TYPE_SLICE:   mono_resolve_type_names(t, a, intern, type->slice.elem); return;
    case TYPE_OPTION:  mono_resolve_type_names(t, a, intern, type->option.inner); return;
    case TYPE_FIXED_ARRAY: mono_resolve_type_names(t, a, intern, type->fixed_array.elem); return;
    case TYPE_FUNC:
        for (int i = 0; i < type->func.param_count; i++)
            mono_resolve_type_names(t, a, intern, type->func.param_types[i]);
        mono_resolve_type_names(t, a, intern, type->func.return_type);
        return;
    case TYPE_STRUCT:
        /* Tuples carry their instantiation in fields (type_args is empty), so the
         * generic "base + type_args" rename doesn't apply — re-derive the canonical
         * name from the (now resolved) element types instead. Recurse fields first
         * so nested tuples/structs are named before this one. */
        if (type->struc.is_tuple) {
            for (int i = 0; i < type->struc.field_count; i++)
                mono_resolve_type_names(t, a, intern, type->struc.fields[i].type);
            if (!type_contains_type_var(type)) {
                const char *cn = tuple_canonical_name(a, intern,
                    type->struc.fields, type->struc.field_count);
                type->struc.name = cn;
                type->struc.qualified_name = cn;
            }
            return;
        }
        if (type->struc.type_arg_count > 0 && !type_contains_type_var(type)) {
            /* Canonicalize name via resolved_sym from pass1/pass2 */
            if (!mono_find(t, type->struc.name)) {
                Symbol *sym = type->struc.resolved_sym;
                if (sym && sym->type && sym->type->struc.name != type->struc.name) {
                    type->struc.name = sym->type->struc.name;
                }
            }
            if (!mono_find(t, type->struc.name)) {
                type->struc.name = mangle_generic_name(a, intern,
                    type->struc.name, type->struc.type_args, type->struc.type_arg_count);
            }
        }
        for (int i = 0; i < type->struc.field_count; i++)
            mono_resolve_type_names(t, a, intern, type->struc.fields[i].type);
        return;
    case TYPE_UNION:
        if (type->unio.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (!mono_find(t, type->unio.name)) {
                Symbol *sym = type->unio.resolved_sym;
                if (sym && sym->type && sym->type->unio.name != type->unio.name) {
                    type->unio.name = sym->type->unio.name;
                }
            }
            if (!mono_find(t, type->unio.name)) {
                type->unio.name = mangle_generic_name(a, intern,
                    type->unio.name, type->unio.type_args, type->unio.type_arg_count);
            }
        }
        for (int i = 0; i < type->unio.variant_count; i++)
            mono_resolve_type_names(t, a, intern, type->unio.variants[i].payload);
        return;
    case TYPE_STUB:
        if (type->stub.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (!mono_find(t, type->stub.name)) {
                type->stub.name = mangle_generic_name(a, intern,
                    type->stub.name, type->stub.type_args, type->stub.type_arg_count);
            }
        }
        return;
    default: return;
    }
}

/* Substitute type vars using a binding map, returning concrete types */
static Type **substitute_type_args(Arena *a, Type **type_args, int type_arg_count,
                                    const char **var_names, Type **concrete, int var_count) {
    Type **result = malloc(sizeof(Type*) * (size_t)type_arg_count);
    for (int i = 0; i < type_arg_count; i++)
        result[i] = type_substitute(a, type_args[i], var_names, concrete, var_count);
    return result;
}

static void discover_nested_types(Type *type, MonoTable *t, Arena *a,
                                  InternTable *intern, SymbolTable *symtab);

/* Substitute the type vars of an expression's *type operand* (e.g. the target of
 * sizeof/alignof/default/alloc, an array/slice-literal element type, or a variant
 * constructor's union result type) and register any generic struct/union instances
 * it transitively names. Such instances are otherwise never registered when they
 * appear only inside a generic body — pass2 sees the still-abstract template type,
 * and the expression walk below recurses into sub-expressions but not into the
 * types they carry. */
static void discover_in_type(Type *ty, MonoTable *t, Arena *a, InternTable *intern,
                             SymbolTable *symtab, const char **var_names,
                             Type **concrete, int var_count) {
    if (!ty) return;
    Type *ct = type_substitute(a, ty, var_names, concrete, var_count);
    if (type_contains_type_var(ct)) return;
    /* discover_nested_types rewrites struct/union/stub names in place; isolate a
     * private deep copy so it can't corrupt the live AST type or a template. */
    ct = type_deep_copy(a, ct);
    discover_nested_types(ct, t, a, intern, symtab);
}

/* Recursively walk an expression tree to discover transitive mono instances */
static void discover_in_expr(Expr *e, MonoTable *t, Arena *a, InternTable *intern,
                              SymbolTable *symtab,
                              const char **var_names, Type **concrete, int var_count) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_CALL:
        discover_in_expr(e->call.func, t, a, intern, symtab, var_names, concrete, var_count);
        for (int i = 0; i < e->call.arg_count; i++)
            discover_in_expr(e->call.args[i], t, a, intern, symtab, var_names, concrete, var_count);
        /* Resolve deferred generic call */
        if (!e->call.mangled_name && e->call.type_arg_count > 0) {
            Type **concrete_args = substitute_type_args(a, e->call.type_args,
                e->call.type_arg_count, var_names, concrete, var_count);
            /* Check all args are concrete */
            bool all_concrete = true;
            for (int i = 0; i < e->call.type_arg_count; i++) {
                if (type_contains_type_var(concrete_args[i])) {
                    all_concrete = false;
                    break;
                }
            }
            if (all_concrete) {
                /* Use resolved_callee from pass2 — always set for all call patterns
                 * (single-level and multi-level qualified calls) */
                Symbol *callee_sym = e->call.resolved_callee;
                if (callee_sym) {
                    const char *base_name = (callee_sym->decl && callee_sym->decl->kind == DECL_LET
                                             && callee_sym->decl->let.codegen_name)
                                            ? callee_sym->decl->let.codegen_name : callee_sym->name;
                    mono_register(t, a, intern, base_name, NULL,
                        concrete_args, e->call.type_arg_count,
                        callee_sym->decl, DECL_LET,
                        callee_sym->type_params, callee_sym->type_param_count);
                }
            }
            free(concrete_args);
        }
        return;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            discover_in_expr(e->struct_lit.fields[i].value, t, a, intern, symtab, var_names, concrete, var_count);
        /* Register generic struct instances created under substitution */
        if (e->type && e->type->kind == TYPE_STRUCT && type_contains_type_var(e->type)) {
            /* Use resolved_sym from pass2 — always set for struct literals */
            Symbol *struct_sym = e->struct_lit.resolved_sym;
            if (struct_sym && struct_sym->is_generic) {
                const char **vars = NULL;
                int vc = 0, vcap = 0;
                type_collect_vars(e->type, &vars, &vc, &vcap);
                Type **concrete_args = arena_alloc(a, sizeof(Type*) * (size_t)vc);
                for (int k = 0; k < vc; k++) {
                    concrete_args[k] = NULL;
                    for (int j = 0; j < var_count; j++) {
                        if (var_names[j] == vars[k]) {
                            concrete_args[k] = concrete[j];
                            break;
                        }
                    }
                    if (!concrete_args[k]) concrete_args[k] = type_type_var(a, vars[k]);
                }
                bool all_concrete = true;
                for (int k = 0; k < vc; k++) {
                    if (type_contains_type_var(concrete_args[k])) {
                        all_concrete = false;
                        break;
                    }
                }
                if (all_concrete) {
                    /* Use canonical C type name (already includes module/ns prefix) */
                    const char *mangled = mono_register(t, a, intern,
                        struct_sym->type->struc.name, NULL,
                        concrete_args, vc, struct_sym->decl,
                        DECL_STRUCT, struct_sym->type_params, struct_sym->type_param_count);
                    MonoInstance *mi = mono_find(t, mangled);
                    if (mi && !mi->concrete_type) {
                        int ntp = struct_sym->type_param_count < vc ? struct_sym->type_param_count : vc;
                        Type *ct = type_substitute(a, struct_sym->type,
                            struct_sym->type_params, concrete_args, ntp);
                        /* Deep copy: type_substitute shares unchanged field
                         * subtrees with the template, which the in-place name
                         * canonicalization below would otherwise corrupt. */
                        ct = type_deep_copy(a, ct);
                        ct->struc.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct);
                        mi->concrete_type = ct;
                    }
                }
                free(vars);
            }
        }
        return;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            discover_in_expr(e->array_lit.elems[i], t, a, intern, symtab, var_names, concrete, var_count);
        /* The element type may be a generic instance (box<'a>[N] { ... }) used only
         * inside a generic body — register it even when no element constructs it. */
        discover_in_type(e->array_lit.elem_type, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            discover_in_expr(e->tuple_lit.elems[i], t, a, intern, symtab, var_names, concrete, var_count);
        /* Register the concrete tuple instance produced under this substitution.
         * Concrete tuples were already registered in pass2; only generic ones
         * (type vars in the element types) need handling here. */
        if (e->type && e->type->kind == TYPE_STRUCT && e->type->struc.is_tuple &&
            type_contains_type_var(e->type)) {
            Type *ct = type_substitute(a, e->type, var_names, concrete, var_count);
            if (!type_contains_type_var(ct)) {
                ct = type_deep_copy(a, ct);  /* isolate before in-place name canonicalization */
                mono_resolve_type_names(t, a, intern, ct);  /* sets ct->struc.name canonically */
                Type *noargs[1] = {0};
                const char *mangled = mono_register(t, a, intern, ct->struc.name, NULL,
                    noargs, 0, NULL, DECL_STRUCT, NULL, 0);
                MonoInstance *mi = mono_find(t, mangled);
                if (mi && !mi->concrete_type)
                    mi->concrete_type = ct;
            }
        }
        return;
    case EXPR_BINARY:
        discover_in_expr(e->binary.left, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->binary.right, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_UNARY_PREFIX:
        discover_in_expr(e->unary_prefix.operand, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_UNARY_POSTFIX:
        discover_in_expr(e->unary_postfix.operand, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        discover_in_expr(e->field.object, t, a, intern, symtab, var_names, concrete, var_count);
        /* Generic-union variant construction (maybe<'a>.just(x) / maybe<'a>.nothing):
         * the result type is the union instance, otherwise unregistered when the
         * construction appears only inside a generic body. */
        if (e->field.is_variant_constructor)
            discover_in_type(e->type, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_INDEX:
        discover_in_expr(e->index.object, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->index.index, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_IF:
        discover_in_expr(e->if_expr.cond, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->if_expr.then_body, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->if_expr.else_body, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            discover_in_expr(e->block.stmts[i], t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            discover_in_expr(e->func.body[i], t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_LET:
        discover_in_expr(e->let_expr.let_init, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_ASSIGN:
        discover_in_expr(e->assign.target, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->assign.value, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_RETURN:
        discover_in_expr(e->return_expr.value, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_BREAK:
        discover_in_expr(e->break_expr.value, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            discover_in_expr(e->loop_expr.body[i], t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_FOR:
        discover_in_expr(e->for_expr.iter, t, a, intern, symtab, var_names, concrete, var_count);
        for (int i = 0; i < e->for_expr.body_count; i++)
            discover_in_expr(e->for_expr.body[i], t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_MATCH:
        discover_in_expr(e->match_expr.subject, t, a, intern, symtab, var_names, concrete, var_count);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                discover_in_expr(e->match_expr.arms[i].body[j], t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_CAST:
        discover_in_expr(e->cast.operand, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_type(e->cast.target, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_SOME:
        discover_in_expr(e->some_expr.value, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_SLICE:
        discover_in_expr(e->slice.object, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->slice.lo, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->slice.hi, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_ALLOC:
        discover_in_expr(e->alloc_expr.size_expr, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->alloc_expr.init_expr, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_type(e->alloc_expr.alloc_type, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_FREE:
        discover_in_expr(e->free_expr.operand, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_ATOMIC_LOAD:
        discover_in_expr(e->atomic_load.ptr, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_ATOMIC_STORE:
        discover_in_expr(e->atomic_store.ptr, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->atomic_store.value, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_ASSERT:
        discover_in_expr(e->assert_expr.condition, t, a, intern, symtab, var_names, concrete, var_count);
        if (e->assert_expr.message)
            discover_in_expr(e->assert_expr.message, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_DEFER:
        discover_in_expr(e->defer_expr.value, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            if (!e->interp_string.segments[i].is_literal)
                discover_in_expr(e->interp_string.segments[i].expr, t, a, intern, symtab, var_names, concrete, var_count);
        }
        return;
    case EXPR_SLICE_LIT:
        discover_in_expr(e->slice_lit.ptr_expr, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_expr(e->slice_lit.len_expr, t, a, intern, symtab, var_names, concrete, var_count);
        discover_in_type(e->slice_lit.elem_type, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_LET_DESTRUCT:
        discover_in_expr(e->let_destruct.init, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_SIZEOF:
        discover_in_type(e->sizeof_expr.target, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_ALIGNOF:
        discover_in_type(e->alignof_expr.target, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    case EXPR_DEFAULT:
        discover_in_type(e->default_expr.target, t, a, intern, symtab, var_names, concrete, var_count);
        return;
    default: return;
    }
}

/* Check if a type references a struct/union by VALUE (not through pointer/option/slice).
 * Returns the mangled name if found, NULL otherwise. */
static const char *find_by_value_dep(Type *type) {
    if (!type) return NULL;
    switch (type->kind) {
    case TYPE_STRUCT:
        /* A struct embedded by value is a direct dependency */
        return type->struc.name;
    case TYPE_UNION:
        /* A union embedded by value is a direct dependency */
        return type->unio.name;
    case TYPE_STUB:
        /* An unresolved stub by value is a direct dependency */
        return type->stub.name;
    case TYPE_FIXED_ARRAY:
        /* Fixed arrays of structs are by-value */
        return find_by_value_dep(type->fixed_array.elem);
    default:
        /* Pointers, slices, options, functions — NOT by-value dependencies */
        return NULL;
    }
}

/* Topological sort state for DFS */
enum { TOPO_UNVISITED = 0, TOPO_VISITING = 1, TOPO_DONE = 2 };

static void topo_visit(MonoTable *t, int idx, int *state, int *order, int *order_count) {
    if (state[idx] != TOPO_UNVISITED) return;
    state[idx] = TOPO_VISITING;
    MonoInstance *inst = &t->entries[idx];
    if (inst->concrete_type && (inst->decl_kind == DECL_STRUCT || inst->decl_kind == DECL_UNION)) {
        Type *ct = inst->concrete_type;
        /* Collect by-value dependencies from struct fields and union variant payloads */
        const char **deps = NULL;
        int dep_count = 0, dep_cap = 0;
        if (ct->kind == TYPE_STRUCT) {
            for (int f = 0; f < ct->struc.field_count; f++) {
                const char *d = find_by_value_dep(ct->struc.fields[f].type);
                if (d) DA_APPEND(deps, dep_count, dep_cap, d);
            }
        } else if (ct->kind == TYPE_UNION) {
            for (int v = 0; v < ct->unio.variant_count; v++) {
                const char *d = find_by_value_dep(ct->unio.variants[v].payload);
                if (d) DA_APPEND(deps, dep_count, dep_cap, d);
            }
        }
        for (int d = 0; d < dep_count; d++) {
            for (int j = 0; j < t->count; j++) {
                if (j != idx && t->entries[j].mangled_name == deps[d]) {
                    topo_visit(t, j, state, order, order_count);
                    break;
                }
            }
        }
        free(deps);
    }
    state[idx] = TOPO_DONE;
    order[(*order_count)++] = idx;
}

/* Recursively walk a type tree and register any concrete generic struct/union
 * references that don't have a MonoInstance yet. This handles structs referenced
 * only as field types (never directly constructed via struct literals). */
static void discover_nested_types(Type *type, MonoTable *t, Arena *a,
                                   InternTable *intern, SymbolTable *symtab) {
    if (!type) return;
    switch (type->kind) {
    case TYPE_POINTER: discover_nested_types(type->pointer.pointee, t, a, intern, symtab); return;
    case TYPE_SLICE:   discover_nested_types(type->slice.elem, t, a, intern, symtab); return;
    case TYPE_OPTION:  discover_nested_types(type->option.inner, t, a, intern, symtab); return;
    case TYPE_FIXED_ARRAY: discover_nested_types(type->fixed_array.elem, t, a, intern, symtab); return;
    case TYPE_FUNC:
        for (int i = 0; i < type->func.param_count; i++)
            discover_nested_types(type->func.param_types[i], t, a, intern, symtab);
        discover_nested_types(type->func.return_type, t, a, intern, symtab);
        return;
    case TYPE_STRUCT:
        /* Tuple appearing only as a field type of another mono entry: register it
         * so its typedef/eq/default emit. type_args is empty, so it bypasses the
         * generic-struct path below. Recurse fields first to name nested elements. */
        if (type->struc.is_tuple) {
            for (int i = 0; i < type->struc.field_count; i++)
                discover_nested_types(type->struc.fields[i].type, t, a, intern, symtab);
            if (!type_contains_type_var(type)) {
                const char *cn = tuple_canonical_name(a, intern,
                    type->struc.fields, type->struc.field_count);
                type->struc.name = cn;
                type->struc.qualified_name = cn;
                if (!mono_find(t, cn)) {
                    Type *noargs[1] = {0};
                    mono_register(t, a, intern, cn, NULL, noargs, 0, NULL, DECL_STRUCT, NULL, 0);
                    MonoInstance *mi = mono_find(t, cn);
                    if (mi && !mi->concrete_type)
                        mi->concrete_type = type_deep_copy(a, type);
                }
            }
            return;
        }
        if (type->struc.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (mono_find(t, type->struc.name)) {
                for (int i = 0; i < type->struc.field_count; i++)
                    discover_nested_types(type->struc.fields[i].type, t, a, intern, symtab);
                return;
            }
            /* Use resolved_sym from pass1/pass2 for the canonical name. A type
             * coming straight from resolve_type (e.g. a sizeof/default/alloc
             * operand) may carry no resolved_sym — fall back to a name lookup,
             * as the STUB branch below already does. */
            Symbol *sym = type->struc.resolved_sym;
            if (!sym && symtab)
                sym = symtab_lookup_kind(symtab, type->struc.name, DECL_STRUCT);
            const char *canon = (sym && sym->type) ? sym->type->struc.name : type->struc.name;
            const char *mangled = mangle_generic_name(a, intern,
                canon, type->struc.type_args, type->struc.type_arg_count);
            type->struc.name = mangled;
            if (!mono_find(t, mangled)) {
                if (sym && sym->is_generic && sym->decl) {
                    mono_register(t, a, intern, sym->type->struc.name, NULL,
                        type->struc.type_args, type->struc.type_arg_count,
                        sym->decl, DECL_STRUCT, sym->type_params, sym->type_param_count);
                    MonoInstance *mi = mono_find(t, mangled);
                    if (mi && !mi->concrete_type) {
                        int ntp = sym->type_param_count < type->struc.type_arg_count
                                  ? sym->type_param_count : type->struc.type_arg_count;
                        Type *ct = type_substitute(a, sym->type,
                            sym->type_params, type->struc.type_args, ntp);
                        ct = type_deep_copy(a, ct);  /* isolate before in-place canonicalization */
                        ct->struc.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct);
                        mi->concrete_type = ct;
                    }
                }
            }
        }
        for (int i = 0; i < type->struc.field_count; i++)
            discover_nested_types(type->struc.fields[i].type, t, a, intern, symtab);
        return;
    case TYPE_UNION:
        if (type->unio.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (mono_find(t, type->unio.name)) {
                for (int i = 0; i < type->unio.variant_count; i++)
                    discover_nested_types(type->unio.variants[i].payload, t, a, intern, symtab);
                return;
            }
            Symbol *sym = type->unio.resolved_sym;
            if (!sym && symtab)
                sym = symtab_lookup_kind(symtab, type->unio.name, DECL_UNION);
            const char *canon = (sym && sym->type) ? sym->type->unio.name : type->unio.name;
            const char *mangled = mangle_generic_name(a, intern,
                canon, type->unio.type_args, type->unio.type_arg_count);
            type->unio.name = mangled;
            if (!mono_find(t, mangled)) {
                if (sym && sym->is_generic && sym->decl) {
                    mono_register(t, a, intern, sym->type->unio.name, NULL,
                        type->unio.type_args, type->unio.type_arg_count,
                        sym->decl, DECL_UNION, sym->type_params, sym->type_param_count);
                    MonoInstance *mi = mono_find(t, mangled);
                    if (mi && !mi->concrete_type) {
                        int ntp = sym->type_param_count < type->unio.type_arg_count
                                  ? sym->type_param_count : type->unio.type_arg_count;
                        Type *ct = type_substitute(a, sym->type,
                            sym->type_params, type->unio.type_args, ntp);
                        ct = type_deep_copy(a, ct);  /* isolate before in-place canonicalization */
                        ct->unio.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct);
                        mi->concrete_type = ct;
                    }
                }
            }
        }
        for (int i = 0; i < type->unio.variant_count; i++)
            discover_nested_types(type->unio.variants[i].payload, t, a, intern, symtab);
        return;
    case TYPE_STUB:
        if (type->stub.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (mono_find(t, type->stub.name)) return;
            const char *base_name = type->stub.name;
            const char *mangled = mangle_generic_name(a, intern,
                base_name, type->stub.type_args, type->stub.type_arg_count);
            type->stub.name = mangled;
            if (!mono_find(t, mangled) && symtab) {
                Symbol *sym = symtab_lookup_kind(symtab, base_name, DECL_STRUCT);
                if (!sym) sym = symtab_lookup_kind(symtab, base_name, DECL_UNION);
                if (sym && sym->is_generic && sym->decl && sym->type) {
                    DeclKind dk = sym->kind;
                    mono_register(t, a, intern, base_name, NULL,
                        type->stub.type_args, type->stub.type_arg_count,
                        sym->decl, dk, sym->type_params, sym->type_param_count);
                    MonoInstance *mi = mono_find(t, mangled);
                    if (mi && !mi->concrete_type) {
                        int ntp = sym->type_param_count < type->stub.type_arg_count
                                  ? sym->type_param_count : type->stub.type_arg_count;
                        Type *ct = type_substitute(a, sym->type,
                            sym->type_params, type->stub.type_args, ntp);
                        ct = type_deep_copy(a, ct);  /* isolate before in-place canonicalization */
                        if (ct->kind == TYPE_STRUCT) ct->struc.name = mangled;
                        else if (ct->kind == TYPE_UNION) ct->unio.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct);
                        mi->concrete_type = ct;
                    }
                }
            }
        }
        return;
    default: return;
    }
}

/* Completeness backstop (audit item 14): walk a finalized concrete type and, if
 * it references a generic struct/union instance that was never registered (so
 * codegen would emit a dangling C typedef name), report an infinite-instantiation
 * error once. A missing instance is the fingerprint of a truncated infinite
 * family — the mutually/indirectly non-uniform recursive types that the
 * definition-site self-check (pass1) and the depth cap don't catch, because the
 * buggy discovery fixpoint halts itself (via a name collision) below the depth
 * limit instead of growing without bound. Recurses through wrapper constructors
 * and into type arguments, but not into a referenced instance's own fields (those
 * are validated when its own table entry is visited), so this terminates. */
static void check_dangling_instance(MonoTable *t, Type *ty, Decl *site, bool *reported) {
    if (!ty || *reported) return;
    /* A type that still contains a type variable is template residue, not a
     * concrete instance codegen emits, so it is never dangling. */
    if (type_contains_type_var(ty)) return;
    switch (ty->kind) {
    /* Recurse only through wrapper constructors and function signatures — the
     * shapes that carry an emitted *field* type. NOT into a generic instance's
     * type arguments: those are mangling inputs, not emitted member types, and
     * mono_resolve_type_names deliberately leaves them at their base name. Every
     * emitted instance is reached as some entry's field anyway, so heads are
     * fully covered without descending into args. */
    case TYPE_POINTER:     check_dangling_instance(t, ty->pointer.pointee, site, reported); return;
    case TYPE_SLICE:       check_dangling_instance(t, ty->slice.elem, site, reported); return;
    case TYPE_OPTION:      check_dangling_instance(t, ty->option.inner, site, reported); return;
    case TYPE_FIXED_ARRAY: check_dangling_instance(t, ty->fixed_array.elem, site, reported); return;
    case TYPE_FUNC:
        for (int i = 0; i < ty->func.param_count; i++)
            check_dangling_instance(t, ty->func.param_types[i], site, reported);
        check_dangling_instance(t, ty->func.return_type, site, reported);
        return;
    case TYPE_STRUCT:
        if (ty->struc.type_arg_count > 0 && !mono_find(t, ty->struc.name)) goto dangling;
        return;
    case TYPE_UNION:
        if (ty->unio.type_arg_count > 0 && !mono_find(t, ty->unio.name)) goto dangling;
        return;
    case TYPE_STUB:
        /* A stub with args should have been resolved+registered by now; if not,
         * it is the same dangling-reference condition. */
        if (ty->stub.type_arg_count > 0 && !mono_find(t, ty->stub.name)) goto dangling;
        return;
    default: return;
    }
dangling:
    *reported = true;
    diag_error(site ? site->loc : (SrcLoc){0},
        "infinite generic instantiation: a generic type transitively references an "
        "unbounded family of instances (a mutually or indirectly non-uniform "
        "recursive type). A generic type may only refer to other generic types with "
        "concrete or parameter-preserving type arguments around any recursion cycle.");
}

void mono_finalize_types(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab) {
    if (t->count == 0) return;

    /* Isolate every concrete_type as a private deep copy before the in-place name
     * canonicalization done below (discover_nested_types and mono_resolve_type_names
     * both rewrite struct/union/stub names in place). Several pass2 sites store the
     * concrete_type as the very expression type they inferred (a shallow copy that
     * shares field/variant subtrees with a pass2-live type or a generic template);
     * mangling those shared nodes would corrupt them (item 7). Deep-copying here
     * makes the finalize phase's mutations local. Instances created *during* the
     * discovery loop below already deep-copy at their build sites. */
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].concrete_type)
            t->entries[i].concrete_type = type_deep_copy(a, t->entries[i].concrete_type);
    }

    /* Discover any concrete generic structs/unions referenced in field types
     * that don't have their own MonoInstance yet (e.g., entry<int32> used only
     * as a field type of table<int32>, never directly constructed). */
    int prev_count;
    do {
        prev_count = t->count;
        for (int i = 0; i < prev_count; i++) {
            MonoInstance *inst = &t->entries[i];
            if (inst->concrete_type && (inst->decl_kind == DECL_STRUCT || inst->decl_kind == DECL_UNION)) {
                Type *ct = inst->concrete_type;
                if (ct->kind == TYPE_STRUCT) {
                    for (int f = 0; f < ct->struc.field_count; f++)
                        discover_nested_types(ct->struc.fields[f].type, t, a, intern, symtab);
                } else if (ct->kind == TYPE_UNION) {
                    for (int v = 0; v < ct->unio.variant_count; v++)
                        discover_nested_types(ct->unio.variants[v].payload, t, a, intern, symtab);
                }
            }
        }
    } while (t->count > prev_count);  /* Repeat until fixpoint */

    /* Resolve all type names in concrete_types. This is the single centralized
     * pass that converts canonical struct names (e.g., "m__entry") to mangled
     * C identifiers (e.g., "m__entry_int32_int32"). Done AFTER discovery so
     * all mono instances are registered and mono_find can prevent double-mangling. */
    for (int i = 0; i < t->count; i++) {
        MonoInstance *inst = &t->entries[i];
        if (inst->concrete_type)
            mono_resolve_type_names(t, a, intern, inst->concrete_type);
    }

    /* Completeness backstop: reject any concrete type that references an
     * unregistered generic instance (a truncated infinite family — see
     * check_dangling_instance). Done before the topo sort so an incomplete table
     * is never handed to codegen. */
    bool dangling_reported = false;
    for (int i = 0; i < t->count && !dangling_reported; i++) {
        MonoInstance *inst = &t->entries[i];
        if (!inst->concrete_type) continue;
        Type *ct = inst->concrete_type;
        if (ct->kind == TYPE_STRUCT) {
            for (int f = 0; f < ct->struc.field_count && !dangling_reported; f++)
                check_dangling_instance(t, ct->struc.fields[f].type, inst->template_decl, &dangling_reported);
        } else if (ct->kind == TYPE_UNION) {
            for (int v = 0; v < ct->unio.variant_count && !dangling_reported; v++)
                check_dangling_instance(t, ct->unio.variants[v].payload, inst->template_decl, &dangling_reported);
        }
    }
    if (dangling_reported) return;  /* main gates codegen on the error count */

    /* Topologically sort struct/union entries so by-value dependencies come first.
     * Function entries are left in their original order at the end. */
    assert(t->count >= 0);  /* invariant: count only grows from 0 via DA_APPEND — tells GCC LTO the (size_t) cast below can't become a huge value */
    int *state = calloc((size_t)t->count, sizeof(int));
    int *order = malloc(sizeof(int) * (size_t)t->count);
    int order_count = 0;

    /* Visit struct/union entries first (DFS-based topological sort) */
    for (int i = 0; i < t->count; i++) {
        if (t->entries[i].decl_kind == DECL_STRUCT || t->entries[i].decl_kind == DECL_UNION)
            topo_visit(t, i, state, order, &order_count);
    }
    /* Append remaining entries (functions) in original order */
    for (int i = 0; i < t->count; i++) {
        if (state[i] == TOPO_UNVISITED)
            order[order_count++] = i;
    }

    /* Reorder entries according to topological order */
    MonoInstance *sorted = malloc(sizeof(MonoInstance) * (size_t)t->count);
    for (int i = 0; i < t->count; i++)
        sorted[i] = t->entries[order[i]];
    memcpy(t->entries, sorted, sizeof(MonoInstance) * (size_t)t->count);
    free(sorted);
    free(state);
    free(order);
}

void mono_discover_transitive(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab) {
    int discovered = 0;
    while (discovered < t->count) {
        int batch_end = t->count;
        for (int i = discovered; i < batch_end; i++) {
            MonoInstance *inst = &t->entries[i];
            if (inst->decl_kind != DECL_LET) continue;
            Decl *tmpl = inst->template_decl;
            if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
            Expr *fn = tmpl->let.init;
            for (int j = 0; j < fn->func.body_count; j++) {
                discover_in_expr(fn->func.body[j], t, a, intern, symtab,
                    inst->type_param_names, inst->type_args, inst->type_param_count);
            }
        }
        discovered = batch_end;
    }
}
