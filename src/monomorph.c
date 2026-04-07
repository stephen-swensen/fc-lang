#include "monomorph.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *mono_register(MonoTable *t, Arena *a, InternTable *intern_tbl,
                          const char *name, const char *ns_prefix,
                          Type **type_args, int count,
                          Decl *tmpl, DeclKind kind,
                          const char **type_params, int tp_count) {
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

void mono_resolve_type_names(MonoTable *t, Arena *a, InternTable *intern, Type *type,
                              SymbolTable *symtab) {
    if (!type) return;
    switch (type->kind) {
    case TYPE_POINTER: mono_resolve_type_names(t, a, intern, type->pointer.pointee, symtab); return;
    case TYPE_SLICE:   mono_resolve_type_names(t, a, intern, type->slice.elem, symtab); return;
    case TYPE_OPTION:  mono_resolve_type_names(t, a, intern, type->option.inner, symtab); return;
    case TYPE_FIXED_ARRAY: mono_resolve_type_names(t, a, intern, type->fixed_array.elem, symtab); return;
    case TYPE_FUNC:
        for (int i = 0; i < type->func.param_count; i++)
            mono_resolve_type_names(t, a, intern, type->func.param_types[i], symtab);
        mono_resolve_type_names(t, a, intern, type->func.return_type, symtab);
        return;
    case TYPE_STRUCT:
        if (type->struc.type_arg_count > 0 && !type_contains_type_var(type)) {
            /* Canonicalize name via symtab before mangling */
            if (symtab && !mono_find(t, type->struc.name)) {
                const char *lookup = type->struc.base_name ? type->struc.base_name : type->struc.name;
                Symbol *sym = symtab_lookup_kind(symtab, lookup, DECL_STRUCT);
                if (!sym) sym = symtab_lookup_kind(symtab, type->struc.name, DECL_STRUCT);
                if (sym && sym->type && sym->type->struc.name != type->struc.name) {
                    if (!type->struc.base_name) type->struc.base_name = type->struc.name;
                    type->struc.name = sym->type->struc.name;
                }
            }
            if (!mono_find(t, type->struc.name)) {
                if (!type->struc.base_name) type->struc.base_name = type->struc.name;
                type->struc.name = mangle_generic_name(a, intern,
                    type->struc.name, type->struc.type_args, type->struc.type_arg_count);
            }
        }
        for (int i = 0; i < type->struc.field_count; i++)
            mono_resolve_type_names(t, a, intern, type->struc.fields[i].type, symtab);
        return;
    case TYPE_UNION:
        if (type->unio.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (symtab && !mono_find(t, type->unio.name)) {
                const char *lookup = type->unio.base_name ? type->unio.base_name : type->unio.name;
                Symbol *sym = symtab_lookup_kind(symtab, lookup, DECL_UNION);
                if (!sym) sym = symtab_lookup_kind(symtab, type->unio.name, DECL_UNION);
                if (sym && sym->type && sym->type->unio.name != type->unio.name) {
                    if (!type->unio.base_name) type->unio.base_name = type->unio.name;
                    type->unio.name = sym->type->unio.name;
                }
            }
            if (!mono_find(t, type->unio.name)) {
                if (!type->unio.base_name) type->unio.base_name = type->unio.name;
                type->unio.name = mangle_generic_name(a, intern,
                    type->unio.name, type->unio.type_args, type->unio.type_arg_count);
            }
        }
        for (int i = 0; i < type->unio.variant_count; i++)
            mono_resolve_type_names(t, a, intern, type->unio.variants[i].payload, symtab);
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
                Symbol *callee_sym = (Symbol *)e->call.resolved_callee;
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
            /* Prefer resolved_sym from pass2 (handles module-scoped structs) */
            Symbol *struct_sym = (Symbol *)e->struct_lit.resolved_sym;
            if (!struct_sym) {
                struct_sym = symtab_lookup_kind(symtab, e->struct_lit.type_name, DECL_STRUCT);
                if (!struct_sym)
                    struct_sym = symtab_lookup(symtab, e->struct_lit.type_name);
            }
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
                        if (ct == struct_sym->type) {
                            ct = type_copy(a, ct);
                        }
                        if (!ct->struc.base_name) ct->struc.base_name = ct->struc.name;
                        ct->struc.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct, symtab);
                        mi->concrete_type = ct;
                    }
                }
                free(vars);
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
        return;
    case EXPR_FREE:
        discover_in_expr(e->free_expr.operand, t, a, intern, symtab, var_names, concrete, var_count);
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
        int fc = (ct->kind == TYPE_STRUCT) ? ct->struc.field_count : 0;
        StructField *fields = (ct->kind == TYPE_STRUCT) ? ct->struc.fields : NULL;
        for (int f = 0; f < fc; f++) {
            const char *dep = find_by_value_dep(fields[f].type);
            if (!dep) continue;
            /* Find the dependency in the mono table */
            for (int j = 0; j < t->count; j++) {
                if (j != idx && t->entries[j].mangled_name == dep) {
                    topo_visit(t, j, state, order, order_count);
                    break;
                }
            }
        }
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
        if (type->struc.type_arg_count > 0 && !type_contains_type_var(type)) {
            /* Check if already fully mangled and registered */
            if (mono_find(t, type->struc.name)) {
                /* Recurse into fields only */
                for (int i = 0; i < type->struc.field_count; i++)
                    discover_nested_types(type->struc.fields[i].type, t, a, intern, symtab);
                return;
            }
            /* Find the template symbol to get the canonical base name for mangling.
             * Try base_name first (parser name), then name (may be canonical from pass1). */
            const char *lookup = type->struc.base_name ? type->struc.base_name : type->struc.name;
            Symbol *sym = symtab_lookup_kind(symtab, lookup, DECL_STRUCT);
            if (!sym) sym = symtab_lookup_kind(symtab, type->struc.name, DECL_STRUCT);
            /* Use the symbol's canonical name for mangling */
            const char *canon = (sym && sym->type) ? sym->type->struc.name : type->struc.name;
            const char *mangled = mangle_generic_name(a, intern,
                canon, type->struc.type_args, type->struc.type_arg_count);
            if (!type->struc.base_name) type->struc.base_name = type->struc.name;
            type->struc.name = mangled;
            if (!mono_find(t, mangled)) {
                if (sym && sym->is_generic && sym->decl) {
                    mono_register(t, a, intern, sym->type->struc.name, NULL,
                        type->struc.type_args, type->struc.type_arg_count,
                        sym->decl, DECL_STRUCT, sym->type_params, sym->type_param_count);
                    /* Build concrete type */
                    MonoInstance *mi = mono_find(t, mangled);
                    if (mi && !mi->concrete_type) {
                        int ntp = sym->type_param_count < type->struc.type_arg_count
                                  ? sym->type_param_count : type->struc.type_arg_count;
                        Type *ct = type_substitute(a, sym->type,
                            sym->type_params, type->struc.type_args, ntp);
                        if (ct == sym->type) ct = type_copy(a, ct);
                        if (!ct->struc.base_name) ct->struc.base_name = ct->struc.name;
                        ct->struc.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct, symtab);
                        mi->concrete_type = ct;
                    }
                }
            }
        }
        /* Recurse into fields (for by-value nested structs) */
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
            const char *lookup = type->unio.base_name ? type->unio.base_name : type->unio.name;
            Symbol *sym = symtab_lookup_kind(symtab, lookup, DECL_UNION);
            if (!sym) sym = symtab_lookup_kind(symtab, type->unio.name, DECL_UNION);
            const char *canon = (sym && sym->type) ? sym->type->unio.name : type->unio.name;
            const char *mangled = mangle_generic_name(a, intern,
                canon, type->unio.type_args, type->unio.type_arg_count);
            if (!type->unio.base_name) type->unio.base_name = type->unio.name;
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
                        if (ct == sym->type) ct = type_copy(a, ct);
                        if (!ct->unio.base_name) ct->unio.base_name = ct->unio.name;
                        ct->unio.name = mangled;
                        mono_resolve_type_names(t, a, intern, ct, symtab);
                        mi->concrete_type = ct;
                    }
                }
            }
        }
        for (int i = 0; i < type->unio.variant_count; i++)
            discover_nested_types(type->unio.variants[i].payload, t, a, intern, symtab);
        return;
    default: return;
    }
}

void mono_finalize_types(MonoTable *t, Arena *a, InternTable *intern, SymbolTable *symtab) {
    if (t->count == 0) return;

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
            mono_resolve_type_names(t, a, intern, inst->concrete_type, symtab);
    }

    /* Topologically sort struct/union entries so by-value dependencies come first.
     * Function entries are left in their original order at the end. */
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

void mono_discover_transitive(MonoTable *t, Arena *a, InternTable *intern,
                              SymbolTable *symtab) {
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
