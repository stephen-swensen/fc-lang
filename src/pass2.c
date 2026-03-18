#include "pass2.h"
#include "diag.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ---- Scope for local bindings ---- */

typedef struct {
    const char *name;
    const char *codegen_name;   /* unique C name for shadowing */
    Type *type;
    bool is_mut;
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
};

static Scope *scope_new(Arena *a, Scope *parent) {
    Scope *s = arena_alloc(a, sizeof(Scope));
    s->parent = parent;
    return s;
}

static int local_id_counter = 0;

static void scope_add(Scope *s, const char *name, const char *codegen_name, Type *type, bool is_mut) {
    LocalBinding b = { name, codegen_name, type, is_mut };
    DA_APPEND(s->locals, s->local_count, s->local_cap, b);
}

/* Scope lookup with lambda boundary crossing and mutability tracking.
   Boundary crossings are counted when moving FROM a boundary scope to its parent,
   so bindings within the boundary scope itself are not treated as captures. */
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
        /* Count boundary when leaving this scope to search parent */
        if (sc->is_lambda_boundary) crossings++;
    }
    if (out_codegen_name) *out_codegen_name = NULL;
    if (out_is_mut) *out_is_mut = false;
    if (out_crossings) *out_crossings = 0;
    if (out_is_global) *out_is_global = false;
    return NULL;
}

/* ---- Type checking ---- */

typedef struct {
    SymbolTable *symtab;
    Scope *scope;
    Arena *arena;
    Type **loop_break_type;  /* non-NULL when inside a loop; points to break value type */
    bool in_for;             /* true when inside a for loop (break value forbidden) */
    SymbolTable *module_symtab;  /* non-NULL when checking inside a module */
    const char *current_ns;      /* current namespace for namespace isolation */
    Type *recursive_ret;         /* non-NULL placeholder when resolving a recursive function */
    LambdaCtx *lambda_ctx;       /* capture tracking for lambdas, NULL outside lambdas */
    bool is_top_level_init;      /* true when checking the init of a top-level DECL_LET */
    MonoTable *mono_table;       /* global instantiation registry */
    InternTable *intern;         /* for name mangling */
} CheckCtx;

static Type *check_expr(CheckCtx *ctx, Expr *e);
static void check_decl_let(CheckCtx *ctx, Decl *d);

static Type *check_block(CheckCtx *ctx, Expr **stmts, int count) {
    if (count == 0) return type_void();
    Type *last = type_void();
    for (int i = 0; i < count; i++) {
        last = check_expr(ctx, stmts[i]);
    }
    return last;
}

/* Resolve a named type stub (TYPE_STRUCT with no fields) to the actual type from symtab */
static Type *resolve_type(CheckCtx *ctx, Type *t) {
    if (!t || t->kind == TYPE_ERROR) return t;

    /* Recurse into compound types */
    if (t->kind == TYPE_POINTER) {
        Type *inner = resolve_type(ctx, t->pointer.pointee);
        if (inner != t->pointer.pointee) return type_pointer(ctx->arena, inner);
        return t;
    }
    if (t->kind == TYPE_OPTION) {
        Type *inner = resolve_type(ctx, t->option.inner);
        if (inner != t->option.inner) return type_option(ctx->arena, inner);
        return t;
    }
    if (t->kind == TYPE_SLICE) {
        Type *inner = resolve_type(ctx, t->slice.elem);
        if (inner != t->slice.elem) return type_slice(ctx->arena, inner);
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
        return nf;
    }

    if (t->kind == TYPE_STRUCT && t->struc.field_count == 0 && t->struc.fields == NULL && t->struc.name) {
        /* Look up as struct/union first, then fall back to general lookup.
         * This handles the case where a module shares the same name as a type. */
        Symbol *sym = NULL;
        if (ctx->module_symtab) {
            sym = symtab_lookup_kind(ctx->module_symtab, t->struc.name, DECL_STRUCT);
            if (!sym)
                sym = symtab_lookup_kind(ctx->module_symtab, t->struc.name, DECL_UNION);
        }
        if (!sym) {
            sym = symtab_lookup_kind(ctx->symtab, t->struc.name, DECL_STRUCT);
            if (!sym)
                sym = symtab_lookup_kind(ctx->symtab, t->struc.name, DECL_UNION);
        }
        if (!sym) {
            if (ctx->module_symtab)
                sym = symtab_lookup(ctx->module_symtab, t->struc.name);
            if (!sym)
                sym = symtab_lookup(ctx->symtab, t->struc.name);
        }
        if (sym && sym->type) {
            /* If the stub has type args (e.g. box<int32>), instantiate the generic */
            if (t->struc.type_arg_count > 0 && sym->is_generic && sym->type_param_count > 0) {
                /* Resolve each type arg */
                int ntp = sym->type_param_count;
                int nta = t->struc.type_arg_count;
                Type **resolved_args = arena_alloc(ctx->arena, sizeof(Type*) * (size_t)nta);
                for (int i = 0; i < nta; i++) {
                    resolved_args[i] = resolve_type(ctx, t->struc.type_args[i]);
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
                    Type *copy = arena_alloc(ctx->arena, sizeof(Type));
                    *copy = *sym->type;
                    concrete = copy;
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
                    /* Register mono instance only with concrete types */
                    const char *ns_prefix = NULL;
                    DeclKind dk = sym->kind;
                    const char *mangled = mono_register(ctx->mono_table, ctx->arena,
                        ctx->intern, t->struc.name, ns_prefix,
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
                        Type *ct = arena_alloc(ctx->arena, sizeof(Type));
                        *ct = *concrete;
                        mono_resolve_type_names(ctx->mono_table, ctx->arena, ctx->intern, ct);
                        mi->concrete_type = ct;
                    }
                }
                return concrete;
            }
            return sym->type;
        }
    }
    return t;
}

/* Check that an integer literal value fits in its target type */
static void check_int_literal_range(int64_t value, Type *type, SrcLoc loc) {
    int64_t lo, hi;
    switch (type->kind) {
    case TYPE_INT8:   lo = -128;        hi = 127;        break;
    case TYPE_INT16:  lo = -32768;      hi = 32767;      break;
    case TYPE_INT32:  lo = -2147483648LL; hi = 2147483647; break;
    case TYPE_INT64:  return; /* always fits in int64_t storage */
    case TYPE_UINT8:  lo = 0; hi = 255;        break;
    case TYPE_UINT16: lo = 0; hi = 65535;      break;
    case TYPE_UINT32: lo = 0; hi = 4294967295LL; break;
    case TYPE_UINT64:
        if (value < 0)
            diag_error(loc, "integer literal %" PRId64 " out of range for uint64 (0..18446744073709551615)", value);
        return;
    default: return;
    }
    if (value < lo || value > hi)
        diag_error(loc, "integer literal %" PRId64 " out of range for %s (%" PRId64 "..%" PRId64 ")",
                   value, type_name(type), lo, hi);
}

/* Wrap an expression in an implicit widening cast */
static Expr *wrap_widen(Arena *a, Expr *e, Type *target) {
    Expr *cast = arena_alloc(a, sizeof(Expr));
    cast->kind = EXPR_CAST;
    cast->loc = e->loc;
    cast->type = target;
    cast->cast.target = target;
    cast->cast.operand = e;
    return cast;
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
        /* Handle struct/union stub mismatch */
        if ((param_type->kind == TYPE_STRUCT && arg_type->kind == TYPE_UNION) ||
            (param_type->kind == TYPE_UNION && arg_type->kind == TYPE_STRUCT)) {
            const char *na = param_type->kind == TYPE_STRUCT ? param_type->struc.name : param_type->unio.name;
            const char *nb = arg_type->kind == TYPE_STRUCT ? arg_type->struc.name : arg_type->unio.name;
            return na == nb;
        }
        return false;
    }

    switch (param_type->kind) {
    case TYPE_POINTER:
        return unify(param_type->pointer.pointee, arg_type->pointer.pointee,
                     var_names, bindings, var_count);
    case TYPE_SLICE:
        return unify(param_type->slice.elem, arg_type->slice.elem,
                     var_names, bindings, var_count);
    case TYPE_OPTION:
        return unify(param_type->option.inner, arg_type->option.inner,
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
    default:
        return type_eq(param_type, arg_type);
    }
}

/* Look up a module symbol, trying namespace-qualified, then global, then module_symtab */
static Symbol *lookup_module(CheckCtx *ctx, const char *name) {
    Symbol *mod = symtab_lookup_module(ctx->symtab, name, ctx->current_ns);
    if (!mod && ctx->current_ns)
        mod = symtab_lookup_module(ctx->symtab, name, NULL);
    if (!mod && ctx->module_symtab)
        mod = symtab_lookup_kind(ctx->module_symtab, name, DECL_MODULE);
    if (!mod)
        mod = symtab_lookup_kind(ctx->symtab, name, DECL_MODULE);
    return mod;
}

/* Check if any type binding still contains unresolved type variables */
static bool bindings_contain_type_vars(Type **bindings, int count) {
    for (int i = 0; i < count; i++)
        if (type_contains_type_var(bindings[i])) return true;
    return false;
}

/* Find the Symbol for an EXPR_CALL's callee, looking up global/module symbols */
static Symbol *find_callee_symbol(CheckCtx *ctx, Expr *callee) {
    if (callee->kind == EXPR_IDENT) {
        Symbol *sym = NULL;
        if (ctx->module_symtab)
            sym = symtab_lookup(ctx->module_symtab, callee->ident.name);
        if (!sym)
            sym = symtab_lookup(ctx->symtab, callee->ident.name);
        return sym;
    }
    if (callee->kind == EXPR_FIELD && callee->field.object->kind == EXPR_IDENT) {
        Symbol *mod = lookup_module(ctx, callee->field.object->ident.name);
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
        if (inner != t->pointer.pointee) return type_pointer(ctx->arena, inner);
        return t;
    }
    case TYPE_OPTION: {
        Type *inner = resolve_generic_types_in_ret(ctx, t->option.inner);
        if (inner != t->option.inner) return type_option(ctx->arena, inner);
        return t;
    }
    case TYPE_SLICE: {
        Type *inner = resolve_generic_types_in_ret(ctx, t->slice.elem);
        if (inner != t->slice.elem) return type_slice(ctx->arena, inner);
        return t;
    }
    case TYPE_STRUCT:
    case TYPE_UNION: {
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

        Type *result = arena_alloc(ctx->arena, sizeof(Type));
        *result = *t;
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
                Type *cp = arena_alloc(ctx->arena, sizeof(Type));
                *cp = *ct;
                ct = cp;
            }
            if (ct->kind == TYPE_STRUCT) ct->struc.name = type_mangled;
            else ct->unio.name = type_mangled;
            mono_resolve_type_names(ctx->mono_table, ctx->arena, ctx->intern, ct);
            mi->concrete_type = ct;
        }
        return result;
    }
    default:
        return t;
    }
}

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
            diag_error(loc, "struct '%s' has no field '%s'", struct_type->struc.name, fname);
            continue;
        }

        field_type = resolve_type(ctx, field_type);
        pat->struc.fields[i].resolved_type = field_type;

        if (inner->kind == PAT_BINDING) {
            const char *orig_name = inner->binding.name;
            int id = local_id_counter++;
            char buf[128];
            snprintf(buf, sizeof(buf), "_l_%s_%d", orig_name, id);
            char *cg = arena_alloc(ctx->arena, strlen(buf) + 1);
            memcpy(cg, buf, strlen(buf) + 1);
            inner->binding.name = cg;  /* overwrite with codegen name */
            scope_add(ctx->scope, orig_name, cg, field_type, is_mut);
        } else if (inner->kind == PAT_WILDCARD) {
            /* skip this field */
        } else if (inner->kind == PAT_STRUCT) {
            check_destruct_pattern(ctx, inner, field_type, is_mut, loc);
        } else {
            diag_error(inner->loc, "unsupported pattern in let destructuring");
            return;
        }
    }
}

static Type *check_expr(CheckCtx *ctx, Expr *e) {
    switch (e->kind) {
    case EXPR_INT_LIT:
        e->type = e->int_lit.lit_type;
        check_int_literal_range(e->int_lit.value, e->type, e->loc);
        return e->type;

    case EXPR_FLOAT_LIT:
        e->type = e->float_lit.lit_type;
        return e->type;

    case EXPR_BOOL_LIT:
        e->type = type_bool();
        return e->type;

    case EXPR_CHAR_LIT:
        e->type = type_char();
        return e->type;

    case EXPR_STRING_LIT:
        e->type = type_str();
        return e->type;

    case EXPR_CSTRING_LIT:
        e->type = type_cstr();
        return e->type;

    case EXPR_IDENT: {
        /* Check local scope first */
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
            e->ident.codegen_name = cg_name;
            e->ident.is_local = !is_global_binding;
            e->type = t;
            return t;
        }
        /* Check module symtab (for within-module sibling references) */
        if (ctx->module_symtab) {
            Symbol *msym = symtab_lookup(ctx->module_symtab, e->ident.name);
            if (msym) {
                if (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION) {
                    e->type = msym->type;
                    return e->type;
                }
                if (msym->kind == DECL_MODULE) {
                    e->type = type_void();  /* placeholder; resolved by EXPR_FIELD */
                    return e->type;
                }
                /* Use the mangled codegen_name */
                if (msym->decl && msym->decl->kind == DECL_LET && msym->decl->let.codegen_name) {
                    e->ident.codegen_name = msym->decl->let.codegen_name;
                }
                if (!msym->type) {
                    diag_error(e->loc, "use of '%s' before its type is resolved", e->ident.name);
                    e->type = type_error();
                    return e->type;
                }
                e->type = msym->type;
                return e->type;
            }
        }
        /* Check global symbol table */
        Symbol *sym = symtab_lookup(ctx->symtab, e->ident.name);
        if (!sym) {
            /* Built-in globals: stdin, stdout, stderr */
            const char *n = e->ident.name;
            if (n == intern_cstr(ctx->intern, "stdin") ||
                n == intern_cstr(ctx->intern, "stdout") ||
                n == intern_cstr(ctx->intern, "stderr")) {
                e->type = type_any_ptr();
                return e->type;
            }
            diag_error(e->loc, "undefined name '%s'", e->ident.name);
            e->type = type_error();
            return e->type;
        }
        /* For struct/union type names used in expressions (e.g., variant construction),
         * return the type itself */
        if (sym->kind == DECL_STRUCT || sym->kind == DECL_UNION) {
            e->type = sym->type;
            return e->type;
        }
        /* Module names resolve to a sentinel — handled in EXPR_FIELD.
         * Use namespace-aware lookup: prefer same-namespace entry. */
        if (sym->kind == DECL_MODULE) {
            Symbol *ns_mod = symtab_lookup_module(ctx->symtab, e->ident.name, ctx->current_ns);
            if (!ns_mod && ctx->current_ns) {
                ns_mod = symtab_lookup_module(ctx->symtab, e->ident.name, NULL);
            }
            if (!ns_mod) {
                diag_error(e->loc, "module '%s' is in a different namespace; use 'import' to access it",
                    e->ident.name);
                e->type = type_error();
                return e->type;
            }
            e->type = type_void();  /* placeholder; real type determined by EXPR_FIELD */
            return e->type;
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
        e->type = sym->type;
        return e->type;
    }

    case EXPR_BINARY: {
        Type *lt = check_expr(ctx, e->binary.left);
        Type *rt = check_expr(ctx, e->binary.right);
        if (type_is_error(lt) || type_is_error(rt)) { e->type = type_error(); return e->type; }
        TokenKind op = e->binary.op;

        /* Allow operations on type variables — defer validation to monomorphization */
        if (lt->kind == TYPE_TYPE_VAR || rt->kind == TYPE_TYPE_VAR) {
            /* Comparison always returns bool */
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
            /* Allow pointer arithmetic: ptr + int, ptr - int */
            if (lt->kind == TYPE_POINTER && type_is_integer(rt)) {
                e->type = lt;
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
                int64_t val = -operand->int_lit.value;
                Type *lt = operand->int_lit.lit_type;
                e->kind = EXPR_INT_LIT;
                e->int_lit.value = val;
                e->int_lit.lit_type = lt;
                return check_expr(ctx, e);
            }
            if (operand->kind == EXPR_FLOAT_LIT) {
                double val = -operand->float_lit.value;
                Type *lt = operand->float_lit.lit_type;
                e->kind = EXPR_FLOAT_LIT;
                e->float_lit.value = val;
                e->float_lit.lit_type = lt;
                return check_expr(ctx, e);
            }
        }
        Type *ot = check_expr(ctx, e->unary_prefix.operand);
        if (type_is_error(ot)) { e->type = type_error(); return e->type; }
        TokenKind op = e->unary_prefix.op;
        if (op == TOK_MINUS) {
            if (!type_is_numeric(ot)) {
                diag_error(e->loc, "unary minus requires numeric operand, got %s", type_name(ot));
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
            if (operand->kind == EXPR_IDENT && operand->ident.is_local) {
                bool op_is_mut = false;
                scope_lookup_capture(ctx->scope, operand->ident.name,
                    NULL, &op_is_mut, NULL, NULL);
                if (!op_is_mut) {
                    diag_error(e->loc, "address-of requires mutable binding");
                    e->type = type_error();
                    return e->type;
                }
            }
            e->type = type_pointer(ctx->arena, ot);
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
            return e->type;
        }
        diag_error(e->loc, "unsupported postfix operator");
        e->type = type_error();
        return e->type;
    }

    case EXPR_FUNC: {
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
            ptypes[i] = resolve_type(ctx, e->func.params[i].type);
            e->func.params[i].type = ptypes[i];
            scope_add(inner, e->func.params[i].name, e->func.params[i].name, ptypes[i], false);
        }

        /* Push lambda context for capture tracking */
        LambdaCtx lctx = { .parent = ctx->lambda_ctx };
        LambdaCtx *saved_lambda = ctx->lambda_ctx;
        ctx->lambda_ctx = &lctx;

        /* Type-check body in inner scope */
        Scope *saved = ctx->scope;
        ctx->scope = inner;
        Type *ret = check_block(ctx, e->func.body, e->func.body_count);
        ctx->scope = saved;

        /* Pop lambda context */
        ctx->lambda_ctx = saved_lambda;

        /* Transfer captures to AST node */
        e->func.captures = lctx.entries;
        e->func.capture_count = lctx.count;

        /* Generate lifted_name for lambdas (non-top-level functions) */
        if (!is_top) {
            int id = local_id_counter++;
            char buf[64];
            snprintf(buf, sizeof(buf), "_fn_%d", id);
            char *ln = arena_alloc(ctx->arena, strlen(buf) + 1);
            memcpy(ln, buf, strlen(buf) + 1);
            e->func.lifted_name = ln;
        }

        /* Check if last expression returns a capturing closure */
        if (e->func.body_count > 0) {
            Expr *last = e->func.body[e->func.body_count - 1];
            if (last->kind == EXPR_FUNC && last->func.capture_count > 0) {
                diag_error(last->loc, "cannot return a capturing closure");
            }
        }

        /* Build function type */
        Type *ft = arena_alloc(ctx->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_types = ptypes;
        ft->func.param_count = pc;
        ft->func.return_type = ret;
        e->type = ft;
        return e->type;
    }

    case EXPR_CALL: {
        Type *ft = check_expr(ctx, e->call.func);
        if (type_is_error(ft)) { e->type = type_error(); return e->type; }

        /* Check if this is a union variant constructor: union_name.variant(payload) */
        if (ft->kind == TYPE_UNION && e->call.func->kind == EXPR_FIELD) {
            Type *union_type = ft;
            const char *variant_name = e->call.func->field.name;

            /* Check if the union is generic — need to unify and instantiate */
            Symbol *union_sym = NULL;
            if (e->call.func->field.object->kind == EXPR_IDENT) {
                union_sym = symtab_lookup(ctx->symtab, e->call.func->field.object->ident.name);
                if (!union_sym && ctx->module_symtab)
                    union_sym = symtab_lookup(ctx->module_symtab, e->call.func->field.object->ident.name);
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
                    return e->type;
                }
            }
            diag_error(e->loc, "union '%s' has no variant '%s'",
                union_type->unio.name, variant_name);
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
        bool callee_is_generic = callee_sym && callee_sym->is_generic;
        /* A local function value whose type contains type vars (e.g. a closure
         * parameter 'f: ('a) -> 'b') is NOT a generic call — skip resolution */
        bool is_local_fn_value = e->call.func->kind == EXPR_IDENT
                                 && e->call.func->ident.is_local;
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
        if (e->call.arg_count != ft->func.param_count) {
            diag_error(e->loc, "expected %d arguments, got %d",
                ft->func.param_count, e->call.arg_count);
            e->type = type_error();
            return e->type;
        }
        bool arg_err = false;
        for (int i = 0; i < e->call.arg_count; i++) {
            Type *at = check_expr(ctx, e->call.args[i]);
            if (type_is_error(at)) { arg_err = true; continue; }
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
        }

        e->type = ft->func.return_type;
        return e->type;
    }

    case EXPR_IF: {
        Type *ct = check_expr(ctx, e->if_expr.cond);
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
        Type *tt = check_expr(ctx, e->if_expr.then_body);
        /* If resolving a recursive function, fill in the return type from the
         * base case (then-branch) before checking the recursive branch (else). */
        if (ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_VOID &&
            tt->kind != TYPE_VOID && !type_is_error(tt)) {
            *ctx->recursive_ret = *tt;
        }
        if (e->if_expr.else_body) {
            Type *et = check_expr(ctx, e->if_expr.else_body);
            if (type_is_error(tt) || type_is_error(et)) {
                /* Use whichever is non-error, or error if both */
                e->type = type_is_error(tt) ? et : tt;
                if (type_is_error(e->type)) e->type = type_error();
                return e->type;
            }
            if (!type_eq(tt, et)) {
                diag_error(e->loc, "if branches have different types: %s vs %s",
                    type_name(tt), type_name(et));
                e->type = type_error();
                return e->type;
            }
            e->type = tt;
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
        ctx->scope = saved;
        return e->type;
    }

    case EXPR_LET: {
        Type *t = check_expr(ctx, e->let_expr.let_init);
        if (type_is_error(t)) {
            /* Add binding with error type so subsequent uses don't cascade "undefined" */
            int id = local_id_counter++;
            char buf[128];
            snprintf(buf, sizeof(buf), "_l_%s_%d", e->let_expr.let_name, id);
            char *cg = arena_alloc(ctx->arena, strlen(buf) + 1);
            memcpy(cg, buf, strlen(buf) + 1);
            e->let_expr.codegen_name = cg;
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        if (t->kind == TYPE_VOID) {
            diag_error(e->loc, "cannot bind void expression to '%s'", e->let_expr.let_name);
            int id = local_id_counter++;
            char buf[128];
            snprintf(buf, sizeof(buf), "_l_%s_%d", e->let_expr.let_name, id);
            char *cg = arena_alloc(ctx->arena, strlen(buf) + 1);
            memcpy(cg, buf, strlen(buf) + 1);
            e->let_expr.codegen_name = cg;
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        e->let_expr.let_type = t;
        int id = local_id_counter++;
        char buf[128];
        snprintf(buf, sizeof(buf), "_l_%s_%d", e->let_expr.let_name, id);
        char *cg = arena_alloc(ctx->arena, strlen(buf) + 1);
        memcpy(cg, buf, strlen(buf) + 1);
        e->let_expr.codegen_name = cg;
        scope_add(ctx->scope, e->let_expr.let_name, cg, t, e->let_expr.let_is_mut);
        e->type = type_void();
        return e->type;
    }

    case EXPR_LET_DESTRUCT: {
        Type *t = check_expr(ctx, e->let_destruct.init);
        if (type_is_error(t)) {
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
        char tmp_buf[128];
        snprintf(tmp_buf, sizeof(tmp_buf), "_ds_%d", tmp_id);
        char *tmp_name = arena_alloc(ctx->arena, strlen(tmp_buf) + 1);
        memcpy(tmp_name, tmp_buf, strlen(tmp_buf) + 1);
        e->let_destruct.tmp_name = tmp_name;

        check_destruct_pattern(ctx, e->let_destruct.pattern, t, e->let_destruct.is_mut, e->loc);

        e->type = type_void();
        return e->type;
    }

    case EXPR_RETURN: {
        if (e->return_expr.value) {
            check_expr(ctx, e->return_expr.value);
            /* Check if returning a capturing closure */
            if (e->return_expr.value->kind == EXPR_FUNC &&
                e->return_expr.value->func.capture_count > 0) {
                diag_error(e->loc, "cannot return a capturing closure");
            }
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_ASSIGN: {
        Type *lt = check_expr(ctx, e->assign.target);
        Type *vt = check_expr(ctx, e->assign.value);
        if (type_is_error(lt) || type_is_error(vt)) {
            e->type = type_void();
            return e->type;
        }
        if (!type_eq(lt, vt)) {
            if (type_can_widen(vt, lt)) {
                e->assign.value = wrap_widen(ctx->arena, e->assign.value, lt);
            } else {
                diag_error(e->loc, "assignment type mismatch: %s vs %s", type_name(lt), type_name(vt));
            }
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_CAST: {
        Type *from = check_expr(ctx, e->cast.operand);
        if (type_is_error(from)) { e->type = type_error(); return e->type; }
        Type *to = e->cast.target;
        bool from_num = type_is_numeric(from);
        bool to_num = type_is_numeric(to);
        bool from_ptr = (from->kind == TYPE_POINTER || from->kind == TYPE_ANY_PTR);
        bool to_ptr = (to->kind == TYPE_POINTER || to->kind == TYPE_ANY_PTR);
        bool from_int = type_is_integer(from);
        bool to_int = type_is_integer(to);
        bool str_to_cstr = (is_str_type(from) && is_cstr_type(to));
        bool cstr_to_str = (is_cstr_type(from) && is_str_type(to));
        /* Allowed: numeric <-> numeric, pointer <-> pointer, pointer <-> integer, str <-> cstr */
        if (!((from_num && to_num) || (from_ptr && to_ptr) ||
              (from_ptr && to_int) || (from_int && to_ptr) || str_to_cstr || cstr_to_str)) {
            diag_error(e->loc, "invalid cast from %s to %s", type_name(from), type_name(to));
            e->type = type_error();
            return e->type;
        }
        e->type = to;
        return e->type;
    }

    case EXPR_STRUCT_LIT: {
        /* Look up the struct type */
        Symbol *sym = NULL;
        if (ctx->module_symtab)
            sym = symtab_lookup_kind(ctx->module_symtab, e->struct_lit.type_name, DECL_STRUCT);
        if (!sym)
            sym = symtab_lookup_kind(ctx->symtab, e->struct_lit.type_name, DECL_STRUCT);
        /* Fallback: try general lookup (for within-module struct references) */
        if (!sym) {
            if (ctx->module_symtab)
                sym = symtab_lookup(ctx->module_symtab, e->struct_lit.type_name);
            if (!sym)
                sym = symtab_lookup(ctx->symtab, e->struct_lit.type_name);
        }
        if (!sym) {
            diag_error(e->loc, "unknown type '%s'", e->struct_lit.type_name);
            e->type = type_error();
            return e->type;
        }
        if (sym->kind != DECL_STRUCT || !sym->type || sym->type->kind != TYPE_STRUCT) {
            diag_error(e->loc, "'%s' is not a struct type", e->struct_lit.type_name);
            e->type = type_error();
            return e->type;
        }
        Type *st = sym->type;

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
                    if (!type_eq(fval, expected) && !type_contains_type_var(expected)) {
                        if (type_can_widen(fval, expected)) {
                            fi->value = wrap_widen(ctx->arena, fi->value, expected);
                        } else {
                            diag_error(fi->value->loc, "field '%s': expected %s, got %s",
                                fi->name, type_name(expected), type_name(fval));
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
                Type *copy = arena_alloc(ctx->arena, sizeof(Type));
                *copy = *st;
                concrete = copy;
            }
            /* Preserve type_args (bindings) on the concrete type for unification */
            concrete->struc.type_args = bindings;
            concrete->struc.type_arg_count = ntp;

            if (!bindings_contain_type_vars(bindings, ntp)) {
                /* Register and build concrete type */
                const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                    sym->name, sym->ns_prefix,
                    bindings, ntp, sym->decl,
                    DECL_STRUCT, sym->type_params, ntp);
                concrete->struc.name = mangled;
                MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                if (mi && !mi->concrete_type) {
                    Type *ct = arena_alloc(ctx->arena, sizeof(Type));
                    *ct = *concrete;
                    mono_resolve_type_names(ctx->mono_table, ctx->arena, ctx->intern, ct);
                    mi->concrete_type = ct;
                }
            }
            e->type = concrete;
            return e->type;
        }

        e->type = st;
        return e->type;
    }

    case EXPR_FIELD: {
        Type *obj_type = check_expr(ctx, e->field.object);
        if (type_is_error(obj_type)) { e->type = type_error(); return e->type; }

        /* Try to resolve the object as a module reference (handles nested chains).
         * For EXPR_IDENT "math", find module "math".
         * For EXPR_FIELD "geometry.shapes", recursively find submodule "shapes" in "geometry".
         * Also handles type-associated modules (same name as struct/union). */
        Symbol *mod_sym = NULL;
        if (e->field.object->kind == EXPR_IDENT) {
            const char *name = e->field.object->ident.name;
            /* Namespace-aware module lookup: try current namespace first, then global */
            mod_sym = symtab_lookup_module(ctx->symtab, name, ctx->current_ns);
            if (!mod_sym && ctx->current_ns)
                mod_sym = symtab_lookup_module(ctx->symtab, name, NULL);
            /* Also check module symtab for within-module references */
            if (!mod_sym && ctx->module_symtab)
                mod_sym = symtab_lookup_kind(ctx->module_symtab, name, DECL_MODULE);
        }

        /* If object's EXPR_FIELD resolved to a module (for nested chains like a.b.member),
         * try to find the module by walking the EXPR_FIELD chain. */
        if (!mod_sym && e->field.object->kind == EXPR_FIELD) {
            /* Walk up the nested EXPR_FIELD chain to find the deepest module. */
            /* We need to re-resolve: the object is already type-checked and set to void.
             * Walk the chain from root to find the module. */
            Expr *chain[32];
            int depth = 0;
            Expr *cur = e->field.object;
            while (cur->kind == EXPR_FIELD && depth < 31) {
                chain[depth++] = cur;
                cur = cur->field.object;
            }
            if (cur->kind == EXPR_IDENT) {
                Symbol *root = symtab_lookup_module(ctx->symtab, cur->ident.name, ctx->current_ns);
                if (!root && ctx->current_ns)
                    root = symtab_lookup_module(ctx->symtab, cur->ident.name, NULL);
                if (!root && ctx->module_symtab)
                    root = symtab_lookup_kind(ctx->module_symtab, cur->ident.name, DECL_MODULE);
                if (root && root->members) {
                    Symbol *walk = root;
                    for (int k = depth - 1; k >= 0; k--) {
                        Symbol *next = symtab_lookup_kind(walk->members, chain[k]->field.name, DECL_MODULE);
                        if (!next) { walk = NULL; break; }
                        walk = next;
                    }
                    if (walk) mod_sym = walk;
                }
            }
        }

        if (mod_sym && mod_sym->members) {
            Symbol *member = symtab_lookup(mod_sym->members, e->field.name);
            if (!member) {
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
            /* Struct/union type member */
            if (member->kind == DECL_STRUCT || member->kind == DECL_UNION) {
                e->type = member->type;
                return e->type;
            }
            /* Extern member: use raw C name */
            if (member->kind == DECL_EXTERN) {
                e->field.codegen_name = member->decl->ext.name;
                e->type = member->type;
                return e->type;
            }
            /* Let member: set codegen_name */
            if (member->decl && member->decl->kind == DECL_LET) {
                e->field.codegen_name = member->decl->let.codegen_name;
            }
            if (!member->type && member->decl && member->decl->kind == DECL_LET) {
                /* On-demand type-check: target module member hasn't been processed yet.
                 * This is safe because pass1 phase 4 already detected circular deps. */
                SymbolTable *saved_mod = ctx->module_symtab;
                Scope *saved_scope = ctx->scope;
                ctx->module_symtab = mod_sym->members;
                ctx->scope = scope_new(ctx->arena, NULL);
                check_decl_let(ctx, member->decl);
                ctx->scope = saved_scope;
                ctx->module_symtab = saved_mod;
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

        /* If the object is an IDENT referencing a union type, this is variant construction */
        if (e->field.object->kind == EXPR_IDENT) {
            Symbol *sym = symtab_lookup(ctx->symtab, e->field.object->ident.name);
            if (!sym && ctx->module_symtab)
                sym = symtab_lookup(ctx->module_symtab, e->field.object->ident.name);
            if (sym && sym->kind == DECL_UNION && sym->type && sym->type->kind == TYPE_UNION) {
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
                        Type *copy = arena_alloc(ctx->arena, sizeof(Type));
                        *copy = *sym->type;
                        concrete = copy;
                    }

                    if (!bindings_contain_type_vars(bindings, ntp)) {
                        const char *mangled = mono_register(ctx->mono_table, ctx->arena, ctx->intern,
                            sym->name, sym->ns_prefix,
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
                e->type = type_pointer(ctx->arena, obj_type->slice.elem);
                return e->type;
            }
        }

        /* Normal struct field access */
        if (obj_type->kind != TYPE_STRUCT) {
            diag_error(e->loc, "field access on non-struct type %s", type_name(obj_type));
            e->type = type_error();
            return e->type;
        }
        for (int i = 0; i < obj_type->struc.field_count; i++) {
            if (obj_type->struc.fields[i].name == e->field.name) {
                e->type = resolve_type(ctx, obj_type->struc.fields[i].type);
                return e->type;
            }
        }
        diag_error(e->loc, "struct '%s' has no field '%s'",
            obj_type->struc.name, e->field.name);
        e->type = type_error();
        return e->type;
    }

    case EXPR_DEREF_FIELD: {
        Type *obj_type = check_expr(ctx, e->field.object);
        if (type_is_error(obj_type)) { e->type = type_error(); return e->type; }
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
                e->type = resolve_type(ctx, pointee->struc.fields[i].type);
                return e->type;
            }
        }
        diag_error(e->loc, "struct '%s' has no field '%s'",
            pointee->struc.name, e->field.name);
        e->type = type_error();
        return e->type;
    }

    case EXPR_INDEX: {
        Type *obj_type = check_expr(ctx, e->index.object);
        Type *idx_type = check_expr(ctx, e->index.index);
        if (type_is_error(obj_type) || type_is_error(idx_type)) { e->type = type_error(); return e->type; }
        if (!type_is_integer(idx_type)) {
            diag_error(e->loc, "index must be integer, got %s", type_name(idx_type));
            e->type = type_error();
            return e->type;
        }

        if (obj_type->kind == TYPE_SLICE) {
            e->type = obj_type->slice.elem;
            return e->type;
        }
        if (obj_type->kind == TYPE_POINTER) {
            e->type = obj_type->pointer.pointee;
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
        return e->type;
    }

    case EXPR_ARRAY_LIT: {
        /* Array literal: type[size] { elems... } → creates a slice */
        /* The size expression must be an integer */
        Type *size_type = check_expr(ctx, e->array_lit.size_expr);
        if (!type_is_error(size_type) && !type_is_integer(size_type)) {
            diag_error(e->loc, "array size must be integer, got %s", type_name(size_type));
            e->type = type_error();
            return e->type;
        }
        /* Type-check elements */
        Type *elem_type = resolve_type(ctx, e->array_lit.elem_type);
        bool elem_error = false;
        for (int i = 0; i < e->array_lit.elem_count; i++) {
            Type *et = check_expr(ctx, e->array_lit.elems[i]);
            if (type_is_error(et)) { elem_error = true; continue; }
            if (!type_eq(et, elem_type)) {
                diag_error(e->array_lit.elems[i]->loc,
                    "array element type mismatch: expected %s, got %s",
                    type_name(elem_type), type_name(et));
                elem_error = true;
            }
        }
        if (elem_error) { e->type = type_error(); return e->type; }
        e->type = type_slice(ctx->arena, elem_type);
        return e->type;
    }

    case EXPR_SOME: {
        Type *inner = check_expr(ctx, e->some_expr.value);
        if (type_is_error(inner)) { e->type = type_error(); return e->type; }
        e->type = type_option(ctx->arena, inner);
        return e->type;
    }

    case EXPR_NONE: {
        /* none(type) — resolve type stubs in the option's inner type */
        e->type = resolve_type(ctx, e->none_expr.target);
        if (e->type->kind == TYPE_OPTION)
            e->type->option.inner = resolve_type(ctx, e->type->option.inner);
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
            /* Add loop var with error type so body can still be checked */
            scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            if (e->for_expr.index_var)
                scope_add(ctx->scope, e->for_expr.index_var, e->for_expr.index_var, type_int64(), false);
        } else if (e->for_expr.range_end) {
            /* Range iteration: for i in lo..hi */
            Type *end_type = check_expr(ctx, e->for_expr.range_end);
            if (type_is_error(end_type)) {
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            } else if (!type_is_integer(iter_type) || !type_is_integer(end_type)) {
                diag_error(e->loc, "range bounds must be integer types");
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            } else {
                /* Use the wider type; if both same, use that */
                Type *var_type = iter_type;
                if (!type_eq(iter_type, end_type)) {
                    /* Allow int32..int64 widening */
                    if (iter_type->kind < end_type->kind) var_type = end_type;
                    else var_type = iter_type;
                }
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, var_type, false);
            }
        } else {
            /* Collection iteration: for x in slice */
            if (iter_type->kind == TYPE_SLICE) {
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, iter_type->slice.elem, false);
                if (e->for_expr.index_var) {
                    scope_add(ctx->scope, e->for_expr.index_var, e->for_expr.index_var, type_int64(), false);
                }
            } else {
                diag_error(e->loc, "for-in requires slice or range, got %s", type_name(iter_type));
                scope_add(ctx->scope, e->for_expr.var, e->for_expr.var, type_error(), false);
            }
        }

        /* Save/set loop context for break checking */
        Type **saved_break = ctx->loop_break_type;
        bool saved_in_for = ctx->in_for;
        Type *break_type = NULL;
        ctx->loop_break_type = &break_type;
        ctx->in_for = true;

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
        e->type = type_void();
        return e->type;
    }

    case EXPR_CONTINUE: {
        if (!ctx->loop_break_type) {
            diag_error(e->loc, "continue outside of loop");
        }
        e->type = type_void();
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
        e->type = type_void();
        return e->type;
    }

    case EXPR_ALLOC: {
        if (e->alloc_expr.alloc_type) {
            Type *ty = resolve_type(ctx, e->alloc_expr.alloc_type);
            e->alloc_expr.alloc_type = ty;

            if (e->alloc_expr.size_expr) {
                /* alloc(T[N]) → T[]? */
                Type *st = check_expr(ctx, e->alloc_expr.size_expr);
                if (type_is_error(st)) { e->type = type_error(); return e->type; }
                if (!type_is_integer(st)) {
                    diag_error(e->loc, "alloc array size must be integer, got %s", type_name(st));
                    e->type = type_error();
                    return e->type;
                }
                e->type = type_option(ctx->arena, type_slice(ctx->arena, ty));
            } else {
                /* alloc(T) → T*? */
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, ty));
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
                    Symbol *sym = symtab_lookup(ctx->symtab, name);
                    if (sym && sym->kind == DECL_LET) var_type = sym->type;
                }
                if (!var_type) {
                    /* Not a variable — try to resolve as a type name */
                    Type *stub = arena_alloc(ctx->arena, sizeof(Type));
                    memset(stub, 0, sizeof(Type));
                    stub->kind = TYPE_STRUCT;
                    stub->struc.name = name;
                    Type *ty = resolve_type(ctx, stub);
                    if (ty != stub) {
                        /* Resolved as type — treat as alloc(T) */
                        e->alloc_expr.alloc_type = ty;
                        e->alloc_expr.init_expr = NULL;
                        e->type = type_option(ctx->arena, type_pointer(ctx->arena, ty));
                        return e->type;
                    }
                    /* Neither variable nor type — fall through to check_expr
                     * which will report the undeclared identifier error */
                }
            }
            /* alloc(expr) → T*? where T is the type of expr */
            Type *t = check_expr(ctx, e->alloc_expr.init_expr);
            if (type_is_error(t)) { e->type = type_error(); return e->type; }
            if (t->kind == TYPE_SLICE) {
                /* alloc(slice_expr) → T[]? (deep-copy slice data to heap) */
                e->type = type_option(ctx->arena, t);
            } else {
                e->type = type_option(ctx->arena, type_pointer(ctx->arena, t));
            }
        }
        return e->type;
    }

    case EXPR_INTERP_STRING: {
        for (int i = 0; i < e->interp_string.segment_count; i++) {
            InterpSegment *seg = &e->interp_string.segments[i];
            if (seg->is_literal) continue;

            Type *et = check_expr(ctx, seg->expr);
            if (type_is_error(et)) continue;
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
                /* For %f, check that explicit width is specified */
                if (ok && conv == 'f') {
                    /* Parse the format spec to check for width */
                    bool has_width = false;
                    const char *s = seg->text;
                    /* Skip flags */
                    while (*s == '-' || *s == '+' || *s == '0' || *s == '#' || *s == ' ') s++;
                    /* Check for width digits */
                    if (*s >= '1' && *s <= '9') has_width = true;
                    if (!has_width) {
                        diag_error(seg->expr->loc,
                            "%%f format requires explicit width (e.g. %%8.2f)");
                    }
                }
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
            default:
                diag_error(seg->expr->loc,
                    "unknown format specifier %%%c", conv);
                break;
            }
        }
        e->type = type_str();
        return e->type;
    }

    default:
        diag_fatal(e->loc, "unsupported expression kind in type checker (kind=%d)", e->kind);
    }
}

/* Recursively check any pattern in a match arm, resolving types and adding bindings */
static void check_match_pattern(CheckCtx *ctx, Pattern *pat, Type *type) {
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
        scope_add(ctx->scope, pat->binding.name, pat->binding.name, type, false);
        break;
    case PAT_INT_LIT:
        if (!type_is_integer(type)) {
            diag_error(pat->loc, "integer pattern on non-integer type %s", type_name(type));
            return;
        }
        check_int_literal_range(pat->int_lit.value, type, pat->loc);
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
        if (!type_eq(type, type_str())) {
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
            check_match_pattern(ctx, pat->some_pat.inner, type->option.inner);
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
                    check_match_pattern(ctx, pat->variant.payload, payload_type);
                }
                break;
            }
        }
        if (!found) {
            diag_error(pat->loc, "union '%s' has no variant '%s'",
                type->unio.name, pat->variant.variant);
            return;
        }
        break;
    }
    case PAT_STRUCT: {
        if (type->kind != TYPE_STRUCT) {
            diag_error(pat->loc, "struct pattern on non-struct type %s", type_name(type));
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
                diag_error(pat->loc, "struct '%s' has no field '%s'", type->struc.name, fname);
                continue;
            }
            pat->struc.fields[fi].resolved_type = field_type;
            check_match_pattern(ctx, pat->struc.fields[fi].pattern, field_type);
        }
        break;
    }
    }
}

static Type *check_match(CheckCtx *ctx, Expr *e) {
    Type *subj_type = check_expr(ctx, e->match_expr.subject);
    subj_type = resolve_type(ctx, subj_type);
    /* Update the subject's type to the resolved type so codegen can access it */
    e->match_expr.subject->type = subj_type;

    if (e->match_expr.arm_count == 0) {
        diag_error(e->loc, "match expression has no arms");
        e->type = type_error();
        return e->type;
    }

    Type *result_type = NULL;

    for (int i = 0; i < e->match_expr.arm_count; i++) {
        MatchArm *arm = &e->match_expr.arms[i];
        Pattern *pat = arm->pattern;

        /* Create a new scope for pattern bindings */
        Scope *arm_scope = scope_new(ctx->arena, ctx->scope);
        Scope *saved = ctx->scope;
        ctx->scope = arm_scope;

        /* Check pattern and introduce bindings */
        check_match_pattern(ctx, pat, subj_type);

        /* Type-check arm body */
        Type *arm_type = check_block(ctx, arm->body, arm->body_count);
        ctx->scope = saved;

        if (type_is_error(arm_type)) continue;

        if (!result_type || type_is_error(result_type)) {
            result_type = arm_type;
            /* Fill recursive return type from first concrete arm (base case) */
            if (ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_VOID &&
                arm_type->kind != TYPE_VOID) {
                *ctx->recursive_ret = *arm_type;
            }
        } else if (!type_eq(result_type, arm_type)) {
            diag_error(arm->loc, "match arms have different types: %s vs %s",
                type_name(result_type), type_name(arm_type));
        }
    }

    e->type = result_type ? result_type : type_error();
    return e->type;
}

/* Check if an expression is a compile-time constant (no variable refs or calls) */
static bool is_const_expr(Expr *e) {
    if (!e) return true;
    switch (e->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT:
    case EXPR_STRING_LIT:
    case EXPR_CSTRING_LIT:
    case EXPR_SIZEOF:
    case EXPR_DEFAULT:
    case EXPR_NONE:
        return true;
    case EXPR_UNARY_PREFIX:
        return is_const_expr(e->unary_prefix.operand);
    case EXPR_BINARY:
        return is_const_expr(e->binary.left) && is_const_expr(e->binary.right);
    case EXPR_CAST:
        return is_const_expr(e->cast.operand);
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (!is_const_expr(e->struct_lit.fields[i].value)) return false;
        return true;
    default:
        return false;
    }
}

static void check_decl_let(CheckCtx *ctx, Decl *d) {
    /* For function declarations, pre-register a partial function type
     * so the body can make recursive calls. */
    const char *lookup_name = d->let.name;
    Symbol *sym = NULL;
    if (ctx->module_symtab) {
        sym = symtab_lookup(ctx->module_symtab, lookup_name);
    }
    if (!sym) {
        sym = symtab_lookup(ctx->symtab, lookup_name);
    }

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
        recursive_ret->kind = TYPE_VOID;  /* placeholder */

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

    Type *saved_recursive_ret = ctx->recursive_ret;
    ctx->recursive_ret = recursive_ret;
    Type *t = check_expr(ctx, d->let.init);
    ctx->recursive_ret = saved_recursive_ret;
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

void pass2_check(Program *prog, SymbolTable *symtab, InternTable *intern_tbl, MonoTable *mono) {
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
        ctx.module_symtab = mod_sym->members;
        /* Type-check all let decls in this module, recursing into submodules */
        for (int j = 0; j < d->module.decl_count; j++) {
            Decl *child = d->module.decls[j];
            if (child->kind == DECL_LET) {
                check_decl_let(&ctx, child);
                if (child->let.init && child->let.init->kind != EXPR_FUNC &&
                    !is_const_expr(child->let.init)) {
                    diag_error(child->loc,
                        "top-level initializer for '%s' must be a constant expression",
                        child->let.name);
                }
            } else if (child->kind == DECL_MODULE) {
                /* Nested submodule: type-check with submodule's symtab */
                Symbol *sub_sym = symtab_lookup_kind(mod_sym->members, child->module.name, DECL_MODULE);
                if (sub_sym && sub_sym->members) {
                    SymbolTable *saved_inner = ctx.module_symtab;
                    ctx.module_symtab = sub_sym->members;
                    for (int k = 0; k < child->module.decl_count; k++) {
                        Decl *sub_child = child->module.decls[k];
                        if (sub_child->kind == DECL_LET) {
                            check_decl_let(&ctx, sub_child);
                            if (sub_child->let.init && sub_child->let.init->kind != EXPR_FUNC &&
                                !is_const_expr(sub_child->let.init)) {
                                diag_error(sub_child->loc,
                                    "top-level initializer for '%s' must be a constant expression",
                                    sub_child->let.name);
                            }
                        }
                    }
                    ctx.module_symtab = saved_inner;
                }
            }
        }
        ctx.module_symtab = saved_mod;
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

    /* Second pass: type-check top-level (non-module) decls */
    ctx.current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            ctx.current_ns = d->ns.name;
            continue;
        }
        if (d->kind == DECL_LET) {
            check_decl_let(&ctx, d);
            if (d->let.init && d->let.init->kind != EXPR_FUNC &&
                !is_const_expr(d->let.init)) {
                diag_error(d->loc,
                    "top-level initializer for '%s' must be a constant expression",
                    d->let.name);
            }
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

    /* Don't free arena — types are referenced from AST */
}
