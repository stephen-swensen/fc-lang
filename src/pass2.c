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

/* Does this type carry provenance (pointer-like, slice-like, or closure-like)? */
static bool type_has_provenance(Type *t) {
    if (!t) return false;
    if (t->kind == TYPE_POINTER || t->kind == TYPE_SLICE ||
        t->kind == TYPE_ANY_PTR) return true;
    /* Capturing closures have a stack-allocated context struct */
    if (t->kind == TYPE_FUNC) return true;
    /* Option wrapping a pointer/slice also carries provenance */
    if (t->kind == TYPE_OPTION) return type_has_provenance(t->option.inner);
    return false;
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
    Type *recursive_ret;         /* non-NULL placeholder when resolving a recursive function */
    LambdaCtx *lambda_ctx;       /* capture tracking for lambdas, NULL outside lambdas */
    bool is_top_level_init;      /* true when checking the init of a top-level DECL_LET */
    MonoTable *mono_table;       /* global instantiation registry */
    InternTable *intern;         /* for name mangling */
    ImportScope *import_scope;   /* lexically scoped import chain */
    OnDemandVisited *on_demand_visited;  /* cycle detection for on-demand type checking */
} CheckCtx;

static Type *check_expr(CheckCtx *ctx, Expr *e);
static void check_decl_let(CheckCtx *ctx, Decl *d);

/* Look up a name in the import scope chain (innermost first = shadowing).
 * Searches from `scope` up to (but not including) `stop`.
 * Pass stop=NULL to search the entire chain. */
static Symbol *import_scope_lookup_until(ImportScope *scope, const char *name,
                                         ImportScope *stop) {
    for (ImportScope *s = scope; s && s != stop; s = s->parent) {
        if (s->table) {
            for (int i = s->table->count - 1; i >= 0; i--) {
                ImportRef *ref = &s->table->entries[i];
                if (ref->local_name == name)
                    return symtab_lookup(ref->source_members, ref->source_name);
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
                    return symtab_lookup_kind(ref->source_members, ref->source_name, kind);
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
 * Top-level structs/unions/lets (ns_prefix=NULL) are only visible to global:: code.
 * Module-scoped types registered with mangled names are always accessible
 * (they're found by mangled name, not user-facing name). */
static Symbol *global_lookup(SymbolTable *symtab, const char *name, const char *current_ns) {
    Symbol *sym = symtab_lookup(symtab, name);
    if (sym && sym->ns_prefix != current_ns) return NULL;
    return sym;
}

static Symbol *global_lookup_kind(SymbolTable *symtab, const char *name, DeclKind kind,
                                  const char *current_ns) {
    Symbol *sym = symtab_lookup_kind(symtab, name, kind);
    if (sym && sym->ns_prefix != current_ns) return NULL;
    return sym;
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

/* Resolve a named type stub (TYPE_STRUCT with no fields) to the actual type from symtab */
static Type *resolve_type(CheckCtx *ctx, Type *t) {
    if (!t || t->kind == TYPE_ERROR) return t;

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

    if (t->kind == TYPE_STRUCT && t->struc.field_count == 0 && t->struc.name) {
        /* Look up as struct or union using the standard scope chain.
         * Kind-filtering avoids returning a module in the companion pattern. */
        Symbol *sym = NULL;

        if (strchr(t->struc.name, '.'))
            sym = resolve_dotted_name(ctx, t->struc.name);
        if (!sym)
            sym = resolve_symbol_kind(ctx, t->struc.name, DECL_STRUCT);
        if (!sym)
            sym = resolve_symbol_kind(ctx, t->struc.name, DECL_UNION);
        if (!sym)
            sym = resolve_symbol(ctx, t->struc.name);
        if (sym && sym->type) {
            /* Store resolved symbol on the type for mono to use */
            t->struc.resolved_sym = sym;
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
                     * not the stub name t->struc.name which may contain dots. */
                    DeclKind dk = sym->kind;
                    const char *canon_name = (sym->type->kind == TYPE_STRUCT)
                        ? sym->type->struc.name : sym->type->unio.name;
                    const char *mangled = mono_register(ctx->mono_table, ctx->arena,
                        ctx->intern, canon_name, NULL,
                        resolved_args, nta, sym->decl, dk,
                        sym->type_params, ntp);
                    /* Update the concrete type's name to the mangled name */
                    if (concrete->kind == TYPE_STRUCT) {
                        if (!concrete->struc.base_name) concrete->struc.base_name = concrete->struc.name;
                        concrete->struc.name = mangled;
                    } else if (concrete->kind == TYPE_UNION) {
                        if (!concrete->unio.base_name) concrete->unio.base_name = concrete->unio.name;
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
    }
    return t;
}

/* Check that an integer literal value fits in its target type.
 * The value is stored as uint64_t; for signed types we check the
 * bit pattern against the type's range. Negative literals are
 * represented as the two's-complement uint64_t (e.g. -1 → 0xFFFF...).
 * Unsigned negation is caught separately in negation folding. */
static void check_int_literal_range(uint64_t value, Type *type, SrcLoc loc) {
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
        /* Handle struct/union stub mismatch: the parser creates all type refs as
         * TYPE_STRUCT stubs, but the resolved type may be TYPE_UNION.  Unify their
         * type_args so generic type variables get bound correctly. */
        if ((param_type->kind == TYPE_STRUCT && arg_type->kind == TYPE_UNION) ||
            (param_type->kind == TYPE_UNION && arg_type->kind == TYPE_STRUCT)) {
            int pa = (param_type->kind == TYPE_STRUCT) ? param_type->struc.type_arg_count : param_type->unio.type_arg_count;
            int aa = (arg_type->kind == TYPE_STRUCT) ? arg_type->struc.type_arg_count : arg_type->unio.type_arg_count;
            Type **pt_args = (param_type->kind == TYPE_STRUCT) ? param_type->struc.type_args : param_type->unio.type_args;
            Type **at_args = (arg_type->kind == TYPE_STRUCT) ? arg_type->struc.type_args : arg_type->unio.type_args;
            if (pa > 0 && pa == aa) {
                for (int i = 0; i < pa; i++)
                    if (!unify(pt_args[i], at_args[i], var_names, bindings, var_count))
                        return false;
                return true;
            }
            const char *na = param_type->kind == TYPE_STRUCT ? param_type->struc.name : param_type->unio.name;
            const char *nb = arg_type->kind == TYPE_STRUCT ? arg_type->struc.name : arg_type->unio.name;
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
    default:
        return type_eq(param_type, arg_type);
    }
}

/* Look up a module symbol: inner scope first, then imports, then global */
static Symbol *lookup_module(CheckCtx *ctx, const char *name) {
    return resolve_symbol_kind(ctx, name, DECL_MODULE);
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
        return resolve_symbol(ctx, callee->ident.name);
    }
    if (callee->kind == EXPR_FIELD) {
        /* Resolve the module chain for qualified calls (mod.func or mod1.mod2.func) */
        Expr *obj = callee->field.object;
        Symbol *mod = NULL;
        if (obj->kind == EXPR_IDENT) {
            mod = lookup_module(ctx, obj->ident.name);
        } else if (obj->kind == EXPR_FIELD) {
            /* Multi-level: walk EXPR_FIELD chain to find root EXPR_IDENT,
             * then resolve each intermediate module segment */
            Expr *cur = obj;
            while (cur->kind == EXPR_FIELD) cur = cur->field.object;
            if (cur->kind == EXPR_IDENT) {
                mod = lookup_module(ctx, cur->ident.name);
                if (mod && mod->members) {
                    /* Walk intermediate module segments */
                    Expr **segs = NULL;
                    int depth = 0, seg_cap = 0;
                    for (Expr *e = obj; e->kind == EXPR_FIELD; e = e->field.object)
                        DA_APPEND(segs, depth, seg_cap, e);
                    /* segs is in reverse order (innermost first); walk from depth-1 to 0 */
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
            if (!result->struc.base_name) result->struc.base_name = result->struc.name;
            result->struc.name = type_mangled;
            result->struc.type_args = type_bindings;
            result->struc.type_arg_count = tntp;
        } else {
            if (!result->unio.base_name) result->unio.base_name = result->unio.name;
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
                if (!ct->struc.base_name) ct->struc.base_name = ct->struc.name;
                ct->struc.name = type_mangled;
            } else {
                if (!ct->unio.base_name) ct->unio.base_name = ct->unio.name;
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
            diag_error(loc, "struct '%s' has no field '%s'", struct_type->struc.name, fname);
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
        } else {
            diag_error(inner->loc, "unsupported pattern in let destructuring");
            return;
        }
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

/* ---- Generic body validation after type variable resolution ---- */

/* Format "func_name(param_type1, param_type2)" for generic validation error messages.
 * Shows concrete parameter types (after substitution), not type variable bindings. */
static const char *fmt_generic_inst(const char *func_name, Arena *arena,
    Type *func_type, const char **type_params, Type **bindings, int ntp)
{
    static char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s(", func_name);
    if (func_type && func_type->kind == TYPE_FUNC) {
        for (int i = 0; i < func_type->func.param_count; i++) {
            if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ", ");
            Type *pt = type_substitute(arena, func_type->func.param_types[i],
                type_params, bindings, ntp);
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s", type_name(pt));
        }
    }
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, ")");
    return buf;
}

/* Walk a generic function body with concrete type bindings and validate
 * operations that were deferred during template type-checking (i.e. binary
 * operations on type variables and type property access).
 * Returns true if validation passed. */
static bool validate_generic_body(Expr *e, Arena *arena,
    const char **type_params, Type **bindings, int ntp,
    SrcLoc call_loc, const char *inst_desc)
{
    if (!e) return true;
    bool ok = true;

    switch (e->kind) {
    case EXPR_BINARY: {
        /* Recurse into children first */
        ok &= validate_generic_body(e->binary.left, arena, type_params, bindings, ntp, call_loc, inst_desc);
        ok &= validate_generic_body(e->binary.right, arena, type_params, bindings, ntp, call_loc, inst_desc);

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
                diag_error(call_loc, "in %s at %d:%d: arithmetic requires numeric operands, got %s and %s",
                    inst_desc, e->loc.line, e->loc.col, type_name(lt), type_name(rt));
                ok = false;
            } else if (!type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                diag_error(call_loc, "in %s at %d:%d: type mismatch: %s vs %s",
                    inst_desc, e->loc.line, e->loc.col, type_name(lt), type_name(rt));
                ok = false;
            }
        } else if (op == TOK_EQEQ || op == TOK_BANGEQ) {
            if (!type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                diag_error(call_loc, "in %s at %d:%d: comparison type mismatch: %s vs %s",
                    inst_desc, e->loc.line, e->loc.col, type_name(lt), type_name(rt));
                ok = false;
            }
        } else if (op == TOK_LT || op == TOK_GT || op == TOK_LTEQ || op == TOK_GTEQ) {
            if (!type_is_numeric(lt) || !type_is_numeric(rt)) {
                diag_error(call_loc, "in %s at %d:%d: ordering comparison requires numeric or pointer types, got %s and %s",
                    inst_desc, e->loc.line, e->loc.col, type_name(lt), type_name(rt));
                ok = false;
            } else if (!type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                diag_error(call_loc, "in %s at %d:%d: comparison type mismatch: %s vs %s",
                    inst_desc, e->loc.line, e->loc.col, type_name(lt), type_name(rt));
                ok = false;
            }
        } else if (op == TOK_AMPAMP || op == TOK_PIPEPIPE) {
            if (!type_eq(lt, type_bool()) || !type_eq(rt, type_bool())) {
                diag_error(call_loc, "in %s at %d:%d: logical operator requires bool operands",
                    inst_desc, e->loc.line, e->loc.col);
                ok = false;
            }
        } else if (op == TOK_AMP || op == TOK_PIPE || op == TOK_CARET ||
                   op == TOK_LTLT || op == TOK_GTGT) {
            if (!type_is_integer(lt) || !type_is_integer(rt)) {
                diag_error(call_loc, "in %s at %d:%d: bitwise/shift operator requires integer operands",
                    inst_desc, e->loc.line, e->loc.col);
                ok = false;
            } else if (op != TOK_LTLT && op != TOK_GTGT &&
                       !type_eq(lt, rt) && !type_common_numeric(lt, rt)) {
                diag_error(call_loc, "in %s at %d:%d: type mismatch: %s vs %s",
                    inst_desc, e->loc.line, e->loc.col, type_name(lt), type_name(rt));
                ok = false;
            }
        }
        break;
    }

    /* Type variable property access: 'a.nan, 'a.min, etc. */
    case EXPR_FIELD: case EXPR_DEREF_FIELD: {
        ok &= validate_generic_body(e->field.object, arena, type_params, bindings, ntp, call_loc, inst_desc);
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
                    diag_error(call_loc, "in %s at %d:%d: type '%s' has no property '%s'",
                        inst_desc, e->loc.line, e->loc.col, type_name(concrete), prop);
                    ok = false;
                }
            }
        }
        break;
    }

    /* Recurse into all sub-expressions */
    case EXPR_UNARY_PREFIX: {
        ok &= validate_generic_body(e->unary_prefix.operand, arena, type_params, bindings, ntp, call_loc, inst_desc);
        /* Validate deferred unary ops on type variables */
        Type *ot_raw = e->unary_prefix.operand->type;
        if (ot_raw && type_contains_type_var(ot_raw)) {
            Type *ot = type_substitute(arena, ot_raw, type_params, bindings, ntp);
            if (e->unary_prefix.op == TOK_MINUS) {
                if (!type_is_numeric(ot)) {
                    diag_error(call_loc, "in %s at %d:%d: unary minus requires numeric operand, got %s",
                        inst_desc, e->loc.line, e->loc.col, type_name(ot));
                    ok = false;
                }
            } else if (e->unary_prefix.op == TOK_TILDE) {
                if (!type_is_integer(ot)) {
                    diag_error(call_loc, "in %s at %d:%d: bitwise not requires integer operand, got %s",
                        inst_desc, e->loc.line, e->loc.col, type_name(ot));
                    ok = false;
                }
            }
        }
        break;
    }
    case EXPR_UNARY_POSTFIX:
        ok &= validate_generic_body(e->unary_postfix.operand, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_CALL:
        ok &= validate_generic_body(e->call.func, arena, type_params, bindings, ntp, call_loc, inst_desc);
        for (int i = 0; i < e->call.arg_count; i++)
            ok &= validate_generic_body(e->call.args[i], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_INDEX:
        ok &= validate_generic_body(e->index.object, arena, type_params, bindings, ntp, call_loc, inst_desc);
        ok &= validate_generic_body(e->index.index, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_SLICE:
        ok &= validate_generic_body(e->slice.object, arena, type_params, bindings, ntp, call_loc, inst_desc);
        if (e->slice.lo) ok &= validate_generic_body(e->slice.lo, arena, type_params, bindings, ntp, call_loc, inst_desc);
        if (e->slice.hi) ok &= validate_generic_body(e->slice.hi, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_CAST:
        ok &= validate_generic_body(e->cast.operand, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_IF:
        ok &= validate_generic_body(e->if_expr.cond, arena, type_params, bindings, ntp, call_loc, inst_desc);
        ok &= validate_generic_body(e->if_expr.then_body, arena, type_params, bindings, ntp, call_loc, inst_desc);
        if (e->if_expr.else_body)
            ok &= validate_generic_body(e->if_expr.else_body, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_LOOP:
        for (int i = 0; i < e->loop_expr.body_count; i++)
            ok &= validate_generic_body(e->loop_expr.body[i], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_FOR:
        ok &= validate_generic_body(e->for_expr.iter, arena, type_params, bindings, ntp, call_loc, inst_desc);
        if (e->for_expr.range_end)
            ok &= validate_generic_body(e->for_expr.range_end, arena, type_params, bindings, ntp, call_loc, inst_desc);
        for (int i = 0; i < e->for_expr.body_count; i++)
            ok &= validate_generic_body(e->for_expr.body[i], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_BREAK:
        if (e->break_expr.value)
            ok &= validate_generic_body(e->break_expr.value, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_RETURN:
        if (e->return_expr.value)
            ok &= validate_generic_body(e->return_expr.value, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_BLOCK:
        for (int i = 0; i < e->block.count; i++)
            ok &= validate_generic_body(e->block.stmts[i], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_FUNC:
        for (int i = 0; i < e->func.body_count; i++)
            ok &= validate_generic_body(e->func.body[i], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_ALLOC:
        if (e->alloc_expr.init_expr)
            ok &= validate_generic_body(e->alloc_expr.init_expr, arena, type_params, bindings, ntp, call_loc, inst_desc);
        if (e->alloc_expr.size_expr)
            ok &= validate_generic_body(e->alloc_expr.size_expr, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_FREE:
        ok &= validate_generic_body(e->free_expr.operand, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_ASSERT:
        ok &= validate_generic_body(e->assert_expr.condition, arena, type_params, bindings, ntp, call_loc, inst_desc);
        if (e->assert_expr.message)
            ok &= validate_generic_body(e->assert_expr.message, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_DEFER:
        ok &= validate_generic_body(e->defer_expr.value, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_SOME:
        ok &= validate_generic_body(e->some_expr.value, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_ASSIGN:
        ok &= validate_generic_body(e->assign.target, arena, type_params, bindings, ntp, call_loc, inst_desc);
        ok &= validate_generic_body(e->assign.value, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            ok &= validate_generic_body(e->struct_lit.fields[i].value, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            ok &= validate_generic_body(e->array_lit.elems[i], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_SLICE_LIT:
        ok &= validate_generic_body(e->slice_lit.ptr_expr, arena, type_params, bindings, ntp, call_loc, inst_desc);
        ok &= validate_generic_body(e->slice_lit.len_expr, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++)
            if (!e->interp_string.segments[i].is_literal && e->interp_string.segments[i].expr)
                ok &= validate_generic_body(e->interp_string.segments[i].expr, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_MATCH:
        ok &= validate_generic_body(e->match_expr.subject, arena, type_params, bindings, ntp, call_loc, inst_desc);
        for (int i = 0; i < e->match_expr.arm_count; i++)
            for (int j = 0; j < e->match_expr.arms[i].body_count; j++)
                ok &= validate_generic_body(e->match_expr.arms[i].body[j], arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_LET:
        if (e->let_expr.let_init)
            ok &= validate_generic_body(e->let_expr.let_init, arena, type_params, bindings, ntp, call_loc, inst_desc);
        break;
    case EXPR_LET_DESTRUCT:
        ok &= validate_generic_body(e->let_destruct.init, arena, type_params, bindings, ntp, call_loc, inst_desc);
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
    default:
        return false;
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
            e->ident.codegen_name = cg_name;
            e->ident.is_local = !is_global_binding;
            e->ident.is_mut = is_mut;
            e->type = t;
            e->prov = scope_lookup_prov(ctx->scope, e->ident.name);
            return t;
        }
        /* 2. Check module symtab (for within-module sibling/forward references) */
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
                        e->type = isym->type;
                        return e->type;
                    }
                    if (isym->kind == DECL_MODULE) {
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
                    e->type = isym->type;
                    return e->type;
                }

                if (!p) break;

                /* Check parent members at this level */
                Symbol *psym = symtab_lookup(p->members, e->ident.name);
                if (psym) {
                    if (psym->kind == DECL_STRUCT || psym->kind == DECL_UNION) {
                        e->type = psym->type;
                        return e->type;
                    }
                    if (psym->kind == DECL_MODULE) {
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
            } else {
                diag_error(e->loc, "undefined name '%s'", e->ident.name);
            }
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
            if (!ns_mod) {
                diag_error(e->loc, "module '%s' is in a different namespace; use 'import' to access it",
                    e->ident.name);
                e->type = type_error();
                return e->type;
            }
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
        e->type = sym->type;
        return e->type;
    }

    case EXPR_BINARY: {
        Type *lt = check_expr(ctx, e->binary.left);
        Type *rt = check_expr(ctx, e->binary.right);
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
            /* Allow pointer arithmetic: ptr + int, ptr - int */
            if (lt->kind == TYPE_POINTER && type_is_integer(rt)) {
                e->type = lt;
                e->prov = e->binary.left->prov;
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
        /* Defer unary minus and bitwise not on type variables to monomorphization */
        if (ot->kind == TYPE_TYPE_VAR && (op == TOK_MINUS || op == TOK_TILDE)) {
            e->type = ot;
            return e->type;
        }
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
            if (is_write_through_const(operand)) {
                diag_error(e->loc, "cannot take mutable address through const pointer");
                e->type = type_error();
                return e->type;
            }
            e->type = type_pointer(ctx->arena, ot);
            e->prov = PROV_STACK;
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
            ptypes[i] = resolve_type(ctx, e->func.params[i].type);
            if (ptypes[i]->kind == TYPE_FIXED_ARRAY) {
                diag_error(e->func.params[i].loc,
                    "fixed-size array types are only valid in struct field declarations");
                ptypes[i] = type_error();
            }
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
        Type *ft = check_expr(ctx, e->call.func);
        if (type_is_error(ft)) { e->type = type_error(); return e->type; }

        /* Check if this is a union variant constructor: union_name.variant(payload) */
        if (ft->kind == TYPE_UNION && e->call.func->kind == EXPR_FIELD) {
            Type *union_type = ft;
            const char *variant_name = e->call.func->field.name;

            /* Look up the union symbol for generic instantiation */
            Symbol *union_sym = NULL;
            if (e->call.func->field.object->kind == EXPR_IDENT) {
                const char *uname = e->call.func->field.object->ident.name;
                union_sym = resolve_symbol(ctx, uname);
            } else if (e->call.func->field.object->kind == EXPR_FIELD) {
                /* Module-qualified path: walk EXPR_FIELD chain to find module, then union */
                Expr *cur = e->call.func->field.object;
                while (cur->kind == EXPR_FIELD) cur = cur->field.object;
                if (cur->kind == EXPR_IDENT) {
                    Symbol *root = resolve_symbol_kind(ctx, cur->ident.name, DECL_MODULE);
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
                    if (!concrete->unio.base_name) concrete->unio.base_name = concrete->unio.name;
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
                        const char *inst_desc = fmt_generic_inst(callee_sym->name, ctx->arena,
                            ft, callee_sym->type_params, bindings, ntp);
                        for (int i = 0; i < func_expr->func.body_count; i++) {
                            if (!validate_generic_body(func_expr->func.body[i], ctx->arena,
                                    callee_sym->type_params, bindings, ntp,
                                    e->loc, inst_desc)) {
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
        Type *t = check_expr(ctx, e->let_expr.let_init);
        if (type_is_error(t)) {
            /* Add binding with error type so subsequent uses don't cascade "undefined" */
            int id = local_id_counter++;
            const char *cg = make_local_name(ctx->arena, "_l_", e->let_expr.let_name, id);
            e->let_expr.codegen_name = cg;
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        if (t->kind == TYPE_VOID) {
            diag_error(e->loc, "cannot bind void expression to '%s'", e->let_expr.let_name);
            int id = local_id_counter++;
            const char *cg = make_local_name(ctx->arena, "_l_", e->let_expr.let_name, id);
            e->let_expr.codegen_name = cg;
            e->let_expr.let_type = type_error();
            scope_add(ctx->scope, e->let_expr.let_name, cg, type_error(), e->let_expr.let_is_mut);
            e->type = type_void();
            return e->type;
        }
        e->let_expr.let_type = t;
        int id = local_id_counter++;
        const char *cg = make_local_name(ctx->arena, "_l_", e->let_expr.let_name, id);
        e->let_expr.codegen_name = cg;
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
        e->type = type_void();
        return e->type;
    }

    case EXPR_CAST: {
        Type *from = check_expr(ctx, e->cast.operand);
        if (type_is_error(from)) { e->type = type_error(); return e->type; }
        Type *to = resolve_type(ctx, e->cast.target);
        e->cast.target = to;
        bool from_num = type_is_numeric(from);
        bool to_num = type_is_numeric(to);
        bool from_ptr = (from->kind == TYPE_POINTER || from->kind == TYPE_ANY_PTR);
        bool to_ptr = (to->kind == TYPE_POINTER || to->kind == TYPE_ANY_PTR);
        bool from_int = type_is_integer(from);
        bool to_int = type_is_integer(to);
        bool str_to_cstr = (is_str_type(from) && is_cstr_type(to));
        bool cstr_to_str = (is_cstr_type(from) && is_str_type(to));
        bool const_change_slice = (from->kind == TYPE_SLICE && to->kind == TYPE_SLICE &&
            type_eq_ignore_const(from, to));
        /* Allowed: numeric <-> numeric, pointer <-> pointer, pointer <-> integer, str <-> cstr, slice const cast */
        if (!((from_num && to_num) || (from_ptr && to_ptr) ||
              (from_ptr && to_int) || (from_int && to_ptr) || str_to_cstr || cstr_to_str || const_change_slice)) {
            diag_error(e->loc, "invalid cast from %s to %s", type_name(from), type_name(to));
            e->type = type_error();
            return e->type;
        }
        e->type = to;
        /* str→cstr creates alloca copy; cstr→str wraps with strlen on stack */
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
                if (!concrete->struc.base_name) concrete->struc.base_name = concrete->struc.name;
                concrete->struc.name = mangled;
                MonoInstance *mi = mono_find(ctx->mono_table, mangled);
                if (mi && !mi->concrete_type) {
                    Type *ct = type_copy(ctx->arena, concrete);
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

        /* Try to resolve the object as a module reference (handles nested chains).
         * For EXPR_IDENT "math", find module "math".
         * For EXPR_FIELD "geometry.shapes", recursively find submodule "shapes" in "geometry".
         * Also handles type-associated modules (same name as struct/union). */
        Symbol *mod_sym = NULL;
        SymbolTable *mod_parent_members = NULL; /* members table where mod_sym was found */
        if (e->field.object->kind == EXPR_IDENT) {
            const char *name = e->field.object->ident.name;
            mod_sym = resolve_symbol_kind(ctx, name, DECL_MODULE);
        }

        /* If object's EXPR_FIELD resolved to a module (for nested chains like a.b.member),
         * try to find the module by walking the EXPR_FIELD chain. */
        if (!mod_sym && e->field.object->kind == EXPR_FIELD) {
            /* Walk up the nested EXPR_FIELD chain to find the deepest module. */
            /* We need to re-resolve: the object is already type-checked and set to void.
             * Walk the chain from root to find the module. */
            Expr **chain = NULL;
            int depth = 0, chain_cap = 0;
            Expr *cur = e->field.object;
            while (cur->kind == EXPR_FIELD) {
                DA_APPEND(chain, depth, chain_cap, cur);
                cur = cur->field.object;
            }
            if (cur->kind == EXPR_IDENT) {
                Symbol *root = resolve_symbol_kind(ctx, cur->ident.name, DECL_MODULE);
                if (root && root->members) {
                    Symbol *walk = root;
                    for (int k = depth - 1; k >= 0; k--) {
                        Symbol *next = symtab_lookup_kind(walk->members, chain[k]->field.name, DECL_MODULE);
                        if (!next) { walk = NULL; break; }
                        mod_parent_members = walk->members;
                        walk = next;
                    }
                    if (walk) mod_sym = walk;
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
                 * Search current scope first, then the parent module where mod_sym was found. */
                Symbol *companion = resolve_symbol_kind(ctx, mod_sym->name, DECL_UNION);
                if (!companion && mod_parent_members)
                    companion = symtab_lookup_kind(mod_parent_members, mod_sym->name, DECL_UNION);
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
                            if (!concrete->unio.base_name) concrete->unio.base_name = concrete->unio.name;
                            concrete->unio.name = mangled;
                        } else {
                            if (!concrete->struc.base_name) concrete->struc.base_name = concrete->struc.name;
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
                SymbolTable *saved_mod = ctx->module_symtab;
                Scope *saved_scope = ctx->scope;
                ctx->module_symtab = mod_sym->members;
                ctx->scope = scope_new(ctx->arena, NULL);
                check_decl_let(ctx, member->decl);
                ctx->scope = saved_scope;
                ctx->module_symtab = saved_mod;
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

        /* If the object is an IDENT referencing a union type, this is variant construction */
        if (e->field.object->kind == EXPR_IDENT) {
            Symbol *sym = resolve_symbol(ctx, e->field.object->ident.name);
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
                        if (!concrete->unio.base_name) concrete->unio.base_name = concrete->unio.name;
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
                        if (!concrete->unio.base_name) concrete->unio.base_name = concrete->unio.name;
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
                }
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
                }
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
        e->prov = e->slice.object->prov;
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
        /* Size must be a compile-time constant (integer literal) */
        if (e->array_lit.size_expr->kind != EXPR_INT_LIT) {
            diag_error(e->array_lit.size_expr->loc,
                "array size must be a compile-time constant");
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
                    "array element type mismatch: expected %s, got %s",
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
        if (!type_eq(lt, len_type)) {
            if (type_can_widen(lt, len_type)) {
                e->slice_lit.len_expr = wrap_widen(ctx->arena, e->slice_lit.len_expr, len_type);
            } else {
                diag_error(e->slice_lit.len_expr->loc,
                    "slice len field: expected int64, got %s", type_name(lt));
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
        if (type_is_error(inner)) { e->type = type_error(); return e->type; }
        e->type = type_option(ctx->arena, inner);
        e->prov = e->some_expr.value->prov;
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

    case EXPR_DEFER: {
        check_expr(ctx, e->defer_expr.value);
        if (expr_contains_control_flow(e->defer_expr.value)) {
            diag_error(e->loc, "deferred expression must not contain return, break, or continue");
        }
        e->type = type_void();
        return e->type;
    }

    case EXPR_ALLOC: {
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
                    diag_error(e->loc, "alloc array size must be integer, got %s", type_name(st));
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
                    stub->kind = TYPE_STRUCT;
                    stub->struc.name = name;
                    stub->struc.base_name = name;
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
    }
    m.is_wildcard = true;
    return m;
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
                interesting->ctor.name, witness_type->unio.name);
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
    /* Build the pattern matrix (1 column per arm pattern) */
    int arm_count = e->match_expr.arm_count;
    PatRow *rows = arena_alloc(ctx->arena, arm_count * sizeof(PatRow));
    for (int i = 0; i < arm_count; i++) {
        rows[i].len = 1;
        rows[i].elems = arena_alloc(ctx->arena, sizeof(MatPat));
        rows[i].elems[0] = pat_to_matpat(ctx, e->match_expr.arms[i].pattern, subj_type);
    }
    PatMatrix mat = { .rows = rows, .row_count = arm_count, .col_count = 1 };

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
        Provenance arm_prov = PROV_UNKNOWN;
        if (arm->body_count > 0)
            arm_prov = arm->body[arm->body_count - 1]->prov;
        ctx->scope = saved;

        if (type_is_error(arm_type)) continue;

        if (!result_type || type_is_error(result_type)) {
            result_type = arm_type;
            result_prov = arm_prov;
            /* Fill recursive return type from first concrete arm (base case) */
            if (ctx->recursive_ret && ctx->recursive_ret->kind == TYPE_VOID &&
                arm_type->kind != TYPE_VOID) {
                *ctx->recursive_ret = *arm_type;
            }
        } else if (!type_eq(result_type, arm_type)) {
            diag_error(arm->loc, "match arms have different types: %s vs %s",
                type_name(result_type), type_name(arm_type));
        } else {
            result_prov = merge_prov(result_prov, arm_prov);
        }
    }

    /* ---- Exhaustiveness check ---- */
    if (!type_is_error(subj_type)) {
        check_match_exhaustiveness(ctx, e, subj_type);
    }

    e->type = result_type ? result_type : type_error();
    e->prov = result_prov;
    return e->type;
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
    /* Extern constants — C macros/enums are compile-time constants */
    case EXPR_FIELD:
        return e->field.is_extern_const;
    /* Struct literal — valid if all field values are const */
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (!is_const_expr(e->struct_lit.fields[i].value)) return false;
        return true;
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
        /* Allow extern constants (C macros/enums) */
        if (e->field.is_extern_const)
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

/* Recursively type-check module members, including arbitrarily nested submodules.
 * parent_members is the symbol table to look up submodule symbols in. */
static void check_module_members(CheckCtx *ctx, Decl *mod_decl,
                                 SymbolTable *parent_members) {
    for (int i = 0; i < mod_decl->module.decl_count; i++) {
        Decl *child = mod_decl->module.decls[i];
        if (child->kind == DECL_LET) {
            check_decl_let(ctx, child);
            if (child->let.init && child->let.init->kind != EXPR_FUNC &&
                !is_const_expr(child->let.init)) {
                diag_error(child->loc,
                    "top-level initializer for '%s' must be a constant expression",
                    child->let.name);
            }
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
        if (d->kind == DECL_LET) {
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

            check_decl_let(&ctx, d);
            if (d->let.init && d->let.init->kind != EXPR_FUNC &&
                !is_file_init_expr(d->let.init)) {
                diag_error(d->loc,
                    "file-level initializer for '%s' must not contain "
                    "function calls or variable references",
                    d->let.name);
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

    /* Don't free arena — types are referenced from AST */
}
