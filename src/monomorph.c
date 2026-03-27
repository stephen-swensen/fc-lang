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

void mono_resolve_type_names(MonoTable *t, Arena *a, InternTable *intern, Type *type) {
    if (!type) return;
    switch (type->kind) {
    case TYPE_POINTER: mono_resolve_type_names(t, a, intern, type->pointer.pointee); return;
    case TYPE_SLICE:   mono_resolve_type_names(t, a, intern, type->slice.elem); return;
    case TYPE_OPTION:  mono_resolve_type_names(t, a, intern, type->option.inner); return;
    case TYPE_FUNC:
        for (int i = 0; i < type->func.param_count; i++)
            mono_resolve_type_names(t, a, intern, type->func.param_types[i]);
        mono_resolve_type_names(t, a, intern, type->func.return_type);
        return;
    case TYPE_STRUCT:
        if (type->struc.type_arg_count > 0 && !type_contains_type_var(type)) {
            if (!mono_find(t, type->struc.name)) {
                if (!type->struc.base_name) type->struc.base_name = type->struc.name;
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
                if (!type->unio.base_name) type->unio.base_name = type->unio.name;
                type->unio.name = mangle_generic_name(a, intern,
                    type->unio.name, type->unio.type_args, type->unio.type_arg_count);
            }
        }
        for (int i = 0; i < type->unio.variant_count; i++)
            mono_resolve_type_names(t, a, intern, type->unio.variants[i].payload);
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
                /* Find callee symbol */
                Expr *callee = e->call.func;
                Symbol *callee_sym = NULL;
                if (callee->kind == EXPR_IDENT) {
                    callee_sym = symtab_lookup(symtab, callee->ident.name);
                } else if (callee->kind == EXPR_FIELD && callee->field.object->kind == EXPR_IDENT) {
                    Symbol *mod = symtab_lookup_module(symtab, callee->field.object->ident.name, NULL);
                    if (!mod) mod = symtab_lookup_kind(symtab, callee->field.object->ident.name, DECL_MODULE);
                    if (mod && mod->members)
                        callee_sym = symtab_lookup(mod->members, callee->field.name);
                }
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
            Symbol *struct_sym = symtab_lookup_kind(symtab, e->struct_lit.type_name, DECL_STRUCT);
            if (!struct_sym)
                struct_sym = symtab_lookup(symtab, e->struct_lit.type_name);
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
                        mono_resolve_type_names(t, a, intern, ct);
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
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            if (!e->interp_string.segments[i].is_literal)
                discover_in_expr(e->interp_string.segments[i].expr, t, a, intern, symtab, var_names, concrete, var_count);
        }
        return;
    default: return;
    }
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
