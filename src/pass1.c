#include "pass1.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Detect generics: scan fields/params for type variables */
static void detect_generic_struct(Decl *d, Symbol *sym) {
    const char **vars = NULL;
    int vcount = 0, vcap = 0;
    for (int i = 0; i < d->struc.field_count; i++)
        type_collect_vars(d->struc.fields[i].type, &vars, &vcount, &vcap);
    if (vcount > 0) {
        d->struc.is_generic = true;
        d->struc.type_params = vars;
        d->struc.type_param_count = vcount;
        sym->is_generic = true;
        sym->type_params = vars;
        sym->type_param_count = vcount;
    }
}

static void detect_generic_union(Decl *d, Symbol *sym) {
    const char **vars = NULL;
    int vcount = 0, vcap = 0;
    for (int i = 0; i < d->unio.variant_count; i++)
        type_collect_vars(d->unio.variants[i].payload, &vars, &vcount, &vcap);
    if (vcount > 0) {
        d->unio.is_generic = true;
        d->unio.type_params = vars;
        d->unio.type_param_count = vcount;
        sym->is_generic = true;
        sym->type_params = vars;
        sym->type_param_count = vcount;
    }
}

static void detect_generic_func(Decl *d, Symbol *sym) {
    if (!d->let.init || d->let.init->kind != EXPR_FUNC) return;
    Expr *fn = d->let.init;
    const char **vars = NULL;
    int vcount = 0, vcap = 0;

    /* Collect from explicit type vars */
    for (int i = 0; i < fn->func.explicit_type_var_count; i++)
        DA_APPEND(vars, vcount, vcap, fn->func.explicit_type_vars[i]);

    /* Collect from parameter types */
    for (int i = 0; i < fn->func.param_count; i++)
        type_collect_vars(fn->func.params[i].type, &vars, &vcount, &vcap);

    if (vcount > 0) {
        sym->is_generic = true;
        sym->type_params = vars;
        sym->type_param_count = vcount;
        sym->explicit_type_param_count = fn->func.explicit_type_var_count;
    }
}

void symtab_init(SymbolTable *t) {
    t->symbols = NULL;
    t->count = 0;
    t->capacity = 0;
}

Symbol *symtab_lookup(SymbolTable *t, const char *name) {
    /* Since names are interned, pointer comparison works */
    for (int i = 0; i < t->count; i++) {
        if (t->symbols[i].name == name) {
            return &t->symbols[i];
        }
    }
    return NULL;
}

Symbol *symtab_lookup_kind(SymbolTable *t, const char *name, DeclKind kind) {
    for (int i = 0; i < t->count; i++) {
        if (t->symbols[i].name == name && t->symbols[i].kind == kind) {
            return &t->symbols[i];
        }
    }
    return NULL;
}

void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl) {
    Symbol sym = { .name = name, .ns_prefix = NULL, .kind = kind, .decl = decl,
                   .type = NULL, .members = NULL, .is_private = false,
                   .is_generic = false, .type_params = NULL, .type_param_count = 0 };
    DA_APPEND(t->symbols, t->count, t->capacity, sym);
}

/* Find a module symbol by name and namespace prefix */
Symbol *symtab_lookup_module(SymbolTable *t, const char *name, const char *ns_prefix) {
    for (int i = 0; i < t->count; i++) {
        Symbol *s = &t->symbols[i];
        if (s->kind != DECL_MODULE) continue;
        if (s->name != name) continue;
        if (ns_prefix == s->ns_prefix) return s;  /* both NULL or same interned ptr */
    }
    return NULL;
}

/* Build a mangled name: prefix_name */
static const char *make_mangled(InternTable *intern, const char *prefix, const char *name) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s_%s", prefix, name);
    return intern_cstr(intern, buf);
}

/* Resolve struct type stubs in a type tree against a symbol table.
 * Replaces TYPE_STRUCT stubs (field_count == 0) with the actual type from the symtab.
 * Used to resolve extern function parameter types against sibling extern structs
 * within the same from-module. */
static Type *resolve_type_stubs(Type *t, SymbolTable *members) {
    if (!t) return t;
    if (t->kind == TYPE_POINTER) {
        Type *inner = resolve_type_stubs(t->pointer.pointee, members);
        if (inner != t->pointer.pointee) {
            Type *r = malloc(sizeof(Type));
            *r = *t;
            r->pointer.pointee = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_SLICE) {
        Type *inner = resolve_type_stubs(t->slice.elem, members);
        if (inner != t->slice.elem) {
            Type *r = malloc(sizeof(Type));
            *r = *t;
            r->slice.elem = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_OPTION) {
        Type *inner = resolve_type_stubs(t->option.inner, members);
        if (inner != t->option.inner) {
            Type *r = malloc(sizeof(Type));
            *r = *t;
            r->option.inner = inner;
            return r;
        }
        return t;
    }
    if (t->kind == TYPE_FUNC) {
        bool changed = false;
        Type **params = malloc(sizeof(Type*) * (size_t)t->func.param_count);
        for (int i = 0; i < t->func.param_count; i++) {
            params[i] = resolve_type_stubs(t->func.param_types[i], members);
            if (params[i] != t->func.param_types[i]) changed = true;
        }
        Type *ret = resolve_type_stubs(t->func.return_type, members);
        if (ret != t->func.return_type) changed = true;
        if (!changed) { free(params); return t; }
        Type *r = malloc(sizeof(Type));
        *r = *t;
        r->func.param_types = params;
        r->func.return_type = ret;
        return r;
    }
    if (t->kind == TYPE_STRUCT && t->struc.field_count == 0 && t->struc.name) {
        Symbol *sym = symtab_lookup_kind(members, t->struc.name, DECL_STRUCT);
        if (sym && sym->type) return sym->type;
        /* Also check for union stubs */
        sym = symtab_lookup_kind(members, t->struc.name, DECL_UNION);
        if (sym && sym->type) return sym->type;
    }
    return t;
}

/* Register a struct type symbol and return the created type */
static Type *register_struct_sym(SymbolTable *tab, Decl *d) {
    symtab_add(tab, d->struc.name, DECL_STRUCT, d);
    /* Use the last added entry (not symtab_lookup which may find a module with same name) */
    Symbol *sym = &tab->symbols[tab->count - 1];
    Type *st = malloc(sizeof(Type));
    memset(st, 0, sizeof(Type));
    st->kind = TYPE_STRUCT;
    st->struc.name = d->struc.name;
    st->struc.base_name = d->struc.name;
    st->struc.c_name = d->struc.c_name;
    st->struc.fields = d->struc.fields;
    st->struc.field_count = d->struc.field_count;
    st->struc.type_args = NULL;
    st->struc.type_arg_count = 0;
    sym->type = st;
    detect_generic_struct(d, sym);
    return st;
}

/* Register a union type symbol and return the created type */
static Type *register_union_sym(SymbolTable *tab, Decl *d) {
    symtab_add(tab, d->unio.name, DECL_UNION, d);
    /* Use the last added entry (not symtab_lookup which may find a module with same name) */
    Symbol *sym = &tab->symbols[tab->count - 1];
    Type *ut = malloc(sizeof(Type));
    memset(ut, 0, sizeof(Type));
    ut->kind = TYPE_UNION;
    ut->unio.name = d->unio.name;
    ut->unio.base_name = d->unio.name;
    ut->unio.variants = d->unio.variants;
    ut->unio.variant_count = d->unio.variant_count;
    ut->unio.type_args = NULL;
    ut->unio.type_arg_count = 0;
    sym->type = ut;
    detect_generic_union(d, sym);
    return ut;
}

/* Register module members: compute mangled names, populate sub-symtab */
static void register_module_members(Decl *d, const char *mangle_prefix,
                                    SymbolTable *members, InternTable *intern,
                                    SymbolTable *global_symtab) {
    const char *mod_name = d->module.name;
    /* Validate: from-modules may only contain extern declarations,
     * and non-from modules may not contain extern declarations. */
    for (int j = 0; j < d->module.decl_count; j++) {
        Decl *child = d->module.decls[j];
        if (d->module.from_lib && child->kind != DECL_EXTERN &&
            !(child->kind == DECL_STRUCT && child->struc.is_extern)) {
            diag_error(child->loc,
                "module '%s' has a 'from' clause — only extern declarations are allowed",
                mod_name);
        }
        if (!d->module.from_lib && child->kind == DECL_EXTERN) {
            diag_error(child->loc,
                "extern declaration in module '%s' requires a 'from' clause on the module",
                mod_name);
        }
        if (!d->module.from_lib && child->kind == DECL_STRUCT && child->struc.is_extern) {
            diag_error(child->loc,
                "extern struct in module '%s' requires a 'from' clause on the module",
                mod_name);
        }
    }
    for (int j = 0; j < d->module.decl_count; j++) {
        Decl *child = d->module.decls[j];
        switch (child->kind) {
        case DECL_LET: {
            const char *src_name = child->let.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->let.codegen_name = mangled;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_LET, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                detect_generic_func(child, msym);
            }
            break;
        }
        case DECL_STRUCT: {
            const char *src_name = child->struc.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->struc.name = mangled;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_STRUCT, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                Type *st = malloc(sizeof(Type));
                memset(st, 0, sizeof(Type));
                st->kind = TYPE_STRUCT;
                st->struc.name = mangled;
                st->struc.base_name = src_name;
                st->struc.c_name = child->struc.c_name;
                st->struc.fields = child->struc.fields;
                st->struc.field_count = child->struc.field_count;
                st->struc.type_args = NULL;
                st->struc.type_arg_count = 0;
                msym->type = st;
                detect_generic_struct(child, msym);
            }
            break;
        }
        case DECL_UNION: {
            const char *src_name = child->unio.name;
            const char *mangled = make_mangled(intern, mangle_prefix, src_name);
            child->unio.name = mangled;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_UNION, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->is_private = child->is_private;
                Type *ut = malloc(sizeof(Type));
                memset(ut, 0, sizeof(Type));
                ut->kind = TYPE_UNION;
                ut->unio.name = mangled;
                ut->unio.base_name = src_name;
                ut->unio.variants = child->unio.variants;
                ut->unio.variant_count = child->unio.variant_count;
                ut->unio.type_args = NULL;
                ut->unio.type_arg_count = 0;
                msym->type = ut;
                detect_generic_union(child, msym);
            }
            break;
        }
        case DECL_EXTERN: {
            const char *src_name = child->ext.alias ? child->ext.alias : child->ext.name;
            if (symtab_lookup(members, src_name)) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    src_name, mod_name);
            } else {
                symtab_add(members, src_name, DECL_EXTERN, child);
                Symbol *msym = symtab_lookup(members, src_name);
                msym->type = child->ext.type;
                msym->is_private = child->is_private;
            }
            break;
        }
        case DECL_MODULE: {
            /* Nested submodule: register as a module member */
            const char *sub_name = child->module.name;
            const char *sub_prefix = make_mangled(intern, mangle_prefix, sub_name);

            Symbol *existing = symtab_lookup(members, sub_name);
            if (existing && (existing->kind == DECL_STRUCT || existing->kind == DECL_UNION)) {
                /* Type-associated module: struct/union with same name already registered.
                 * Add a second entry with kind=DECL_MODULE under the same name.
                 * Use symtab_lookup_kind to distinguish. */
                symtab_add(members, sub_name, DECL_MODULE, child);
                Symbol *sub_sym = &members->symbols[members->count - 1];
                sub_sym->is_private = child->is_private;
                SymbolTable *sub_members = malloc(sizeof(SymbolTable));
                symtab_init(sub_members);
                sub_sym->members = sub_members;
                register_module_members(child, sub_prefix, sub_members, intern, global_symtab);
                break;
            }
            if (existing) {
                diag_error(child->loc, "redefinition of '%s' in module '%s'",
                    sub_name, mod_name);
                break;
            }
            symtab_add(members, sub_name, DECL_MODULE, child);
            Symbol *sub_sym = &members->symbols[members->count - 1];
            sub_sym->is_private = child->is_private;
            SymbolTable *sub_members = malloc(sizeof(SymbolTable));
            symtab_init(sub_members);
            sub_sym->members = sub_members;
            register_module_members(child, sub_prefix, sub_members, intern, global_symtab);
            break;
        }
        default:
            break;
        }
    }

    /* Resolve struct type stubs in extern function signatures against sibling
     * extern structs within the same module. This allows e.g.:
     *   extern struct timespec = ...
     *   extern clock_gettime: (int32, timespec*) -> int32
     * where timespec* in the function type resolves to the extern struct. */
    for (int j = 0; j < d->module.decl_count; j++) {
        Decl *child = d->module.decls[j];
        if (child->kind != DECL_EXTERN) continue;
        Type *resolved = resolve_type_stubs(child->ext.type, members);
        if (resolved != child->ext.type) {
            child->ext.type = resolved;
            /* Update the symbol table entry too */
            const char *src_name = child->ext.alias ? child->ext.alias : child->ext.name;
            Symbol *msym = symtab_lookup(members, src_name);
            if (msym) msym->type = resolved;
        }
    }
}

void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern) {
    /* Phase 1: Register modules.
     * Track current namespace as we iterate — DECL_NAMESPACE resets it. */
    const char *current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];

        if (d->kind == DECL_NAMESPACE) {
            current_ns = d->ns.name;
            continue;
        }

        if (d->kind != DECL_MODULE) continue;

        const char *mod_name = d->module.name;
        const char *ns_prefix = d->module.ns_prefix ? d->module.ns_prefix : current_ns;
        d->module.ns_prefix = ns_prefix;

        /* Build mangling prefix: [namespace_]module */
        const char *mangle_prefix;
        if (ns_prefix) {
            mangle_prefix = make_mangled(intern, ns_prefix, mod_name);
        } else {
            mangle_prefix = mod_name;
        }

        /* Check for duplicate module name within the same namespace */
        Symbol *existing = symtab_lookup_module(symtab, mod_name, ns_prefix);
        if (existing) {
            /* Type-associated module: allow struct/union + module with same name */
            if (existing->kind == DECL_STRUCT || existing->kind == DECL_UNION) {
                /* Register the module alongside the type.
                 * The type is already in the symtab. We need a separate symbol
                 * for the module. Use a special mangled key for the symtab,
                 * but the user accesses it by the original name (context-dependent). */
                /* Store as a secondary entry with kind DECL_MODULE */
                symtab_add(symtab, mod_name, DECL_MODULE, d);
                Symbol *mod_sym = symtab_lookup(symtab, mod_name);
                /* symtab_lookup returns the first match (the struct/union).
                 * We need the new one — find it at the end. */
                mod_sym = &symtab->symbols[symtab->count - 1];
                mod_sym->is_private = d->is_private;
                mod_sym->ns_prefix = ns_prefix;
                SymbolTable *members = malloc(sizeof(SymbolTable));
                symtab_init(members);
                mod_sym->members = members;
                register_module_members(d, mangle_prefix, members, intern, symtab);
                continue;
            }
            if (existing->kind == DECL_MODULE) {
                diag_error(d->loc, "redefinition of module '%s'", mod_name);
                continue;
            }
        }

        /* Register module in global symtab */
        symtab_add(symtab, mod_name, DECL_MODULE, d);
        Symbol *mod_sym = &symtab->symbols[symtab->count - 1];
        mod_sym->is_private = d->is_private;
        mod_sym->ns_prefix = ns_prefix;

        /* Create sub-symbol table for module members */
        SymbolTable *members = malloc(sizeof(SymbolTable));
        symtab_init(members);
        mod_sym->members = members;

        /* Walk child decls, compute mangled names, register in members */
        register_module_members(d, mangle_prefix, members, intern, symtab);
    }

    /* Phase 2: Register top-level (non-module) decls */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        switch (d->kind) {
        case DECL_LET: {
            if (symtab_lookup(symtab, d->let.name)) {
                diag_error(d->loc, "redefinition of '%s'", d->let.name);
            } else {
                symtab_add(symtab, d->let.name, DECL_LET, d);
            }
            /* Mangle non-main top-level function names to avoid C namespace collisions */
            if (strcmp(d->let.name, "main") != 0 && !d->let.codegen_name &&
                d->let.init && d->let.init->kind == EXPR_FUNC) {
                d->let.codegen_name = make_mangled(intern, "fc", d->let.name);
            }
            /* Detect generic functions */
            Symbol *let_sym = symtab_lookup(symtab, d->let.name);
            if (let_sym) detect_generic_func(d, let_sym);
            break;
        }
        case DECL_STRUCT: {
            if (d->struc.is_extern) {
                diag_error(d->loc, "extern struct must be inside a module with a 'from' clause");
                break;
            }
            Symbol *existing = symtab_lookup(symtab, d->struc.name);
            if (existing && existing->kind != DECL_MODULE) {
                diag_error(d->loc, "redefinition of '%s'", d->struc.name);
            } else {
                register_struct_sym(symtab, d);
            }
            break;
        }
        case DECL_UNION: {
            Symbol *existing = symtab_lookup(symtab, d->unio.name);
            if (existing && existing->kind != DECL_MODULE) {
                diag_error(d->loc, "redefinition of '%s'", d->unio.name);
            } else {
                register_union_sym(symtab, d);
            }
            break;
        }
        default:
            break;
        }
    }

    /* Phase 3: Process imports */
    current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];

        if (d->kind == DECL_NAMESPACE) {
            current_ns = d->ns.name;
            continue;
        }

        if (d->kind != DECL_IMPORT) continue;

        const char *mod_name = d->import.from_module;
        const char *from_ns = d->import.from_namespace;

        if (!mod_name && !from_ns) {
            /* import MODULE [as ALIAS] — whole module import */
            const char *name = d->import.name;
            /* Look up module, trying current namespace first, then global */
            Symbol *sym = symtab_lookup_module(symtab, name, current_ns);
            if (!sym) sym = symtab_lookup_module(symtab, name, NULL);
            if (!sym || sym->kind != DECL_MODULE) {
                diag_error(d->loc, "unknown module '%s'", name);
                continue;
            }
            if (d->import.alias) {
                /* import MODULE as ALIAS: add alias to symtab */
                const char *alias = d->import.alias;
                if (symtab_lookup(symtab, alias)) {
                    diag_error(d->loc, "import '%s' conflicts with existing name", alias);
                } else {
                    symtab_add(symtab, alias, DECL_MODULE, sym->decl);
                    Symbol *alias_sym = &symtab->symbols[symtab->count - 1];
                    alias_sym->members = sym->members;
                    alias_sym->type = sym->type;
                    alias_sym->ns_prefix = current_ns;
                }
            }
            continue;
        }

        if (from_ns && !mod_name) {
            /* Wildcard import from bare namespace is invalid per spec */
            if (d->import.is_wildcard) {
                diag_error(d->loc, "cannot wildcard-import from a namespace; import * works on modules only");
                continue;
            }
            /* import MODULE from namespace:: — cross-namespace whole module import */
            const char *name = d->import.name;
            Symbol *sym = symtab_lookup_module(symtab, name, from_ns);
            if (!sym || sym->kind != DECL_MODULE) {
                diag_error(d->loc, "unknown module '%s' in namespace '%s'", name, from_ns);
                continue;
            }
            const char *import_name = d->import.alias ? d->import.alias : name;
            /* Check for existing entry in importer's namespace */
            Symbol *existing = symtab_lookup_module(symtab, import_name, current_ns);
            if (existing) {
                if (existing->decl != sym->decl) {
                    diag_error(d->loc, "import '%s' conflicts with existing name", import_name);
                }
            } else {
                /* Add entry accessible from importer's namespace */
                symtab_add(symtab, import_name, DECL_MODULE, sym->decl);
                Symbol *new_sym = &symtab->symbols[symtab->count - 1];
                new_sym->members = sym->members;
                new_sym->type = sym->type;
                new_sym->ns_prefix = current_ns;
            }
            continue;
        }

        /* Have a from_module — look up the source module */
        const char *lookup_ns = from_ns ? from_ns : current_ns;
        Symbol *mod_sym = symtab_lookup_module(symtab, mod_name, lookup_ns);
        if (!mod_sym) mod_sym = symtab_lookup_module(symtab, mod_name, NULL);
        if (!mod_sym || mod_sym->kind != DECL_MODULE) {
            diag_error(d->loc, "unknown module '%s'", mod_name);
            continue;
        }

        if (d->import.is_wildcard) {
            /* import * from MODULE: add all non-private members to global symtab */
            SymbolTable *members = mod_sym->members;
            for (int j = 0; j < members->count; j++) {
                Symbol *msym = &members->symbols[j];
                if (msym->is_private) continue;
                const char *import_name = msym->name;
                if (symtab_lookup(symtab, import_name)) {
                    diag_error(d->loc, "import '%s' conflicts with existing name", import_name);
                } else {
                    symtab_add(symtab, import_name, msym->kind, msym->decl);
                    Symbol *new_sym = &symtab->symbols[symtab->count - 1];
                    new_sym->type = msym->type;
                    new_sym->members = msym->members;
                }
            }
        } else {
            /* import NAME [as ALIAS] from MODULE */
            Symbol *msym = symtab_lookup(mod_sym->members, d->import.name);
            if (!msym) {
                diag_error(d->loc, "module '%s' has no member '%s'",
                    mod_name, d->import.name);
                continue;
            }
            if (msym->is_private) {
                diag_error(d->loc, "cannot import private member '%s' from module '%s'",
                    d->import.name, mod_name);
                continue;
            }
            const char *import_name = d->import.alias ? d->import.alias : d->import.name;
            if (symtab_lookup(symtab, import_name)) {
                diag_error(d->loc, "import '%s' conflicts with existing name", import_name);
            } else {
                symtab_add(symtab, import_name, msym->kind, msym->decl);
                Symbol *new_sym = &symtab->symbols[symtab->count - 1];
                /* If importing a type with an alias, shallow-copy the Type so
                 * the alias field doesn't mutate the original shared type. */
                if (d->import.alias && msym->type &&
                    (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION)) {
                    Type *copy = malloc(sizeof(Type));
                    *copy = *msym->type;
                    copy->alias = d->import.alias;
                    new_sym->type = copy;
                } else {
                    new_sym->type = msym->type;
                }
                new_sym->members = msym->members;
            }
            /* Type-associated module: if importing a type, also import the
             * associated module (if any) under the same name.
             * Per spec: "import T from M brings both the type T and the module T
             * (if it exists) into scope together." */
            if (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION) {
                Symbol *assoc_mod = symtab_lookup_kind(mod_sym->members, d->import.name, DECL_MODULE);
                if (assoc_mod && !assoc_mod->is_private) {
                    symtab_add(symtab, import_name, DECL_MODULE, assoc_mod->decl);
                    Symbol *mod_entry = &symtab->symbols[symtab->count - 1];
                    mod_entry->members = assoc_mod->members;
                    mod_entry->type = assoc_mod->type;
                }
            }
        }
    }

    /* Phase 4: Detect circular references between modules.
     * Scan module member expressions for EXPR_FIELD on other modules,
     * build a dependency graph, and detect cycles with DFS. */
    int mod_count = 0;
    for (int i = 0; i < symtab->count; i++)
        if (symtab->symbols[i].kind == DECL_MODULE) mod_count++;

    if (mod_count >= 2) {
        /* Collect module decl pointers */
        Decl **mods = malloc(sizeof(Decl*) * (size_t)mod_count);
        int mi = 0;
        for (int i = 0; i < symtab->count; i++)
            if (symtab->symbols[i].kind == DECL_MODULE)
                mods[mi++] = symtab->symbols[i].decl;

        /* Build adjacency: deps[i*mod_count + j] = true if mod i references mod j */
        bool *deps = calloc((size_t)(mod_count * mod_count), sizeof(bool));

        for (int i = 0; i < mod_count; i++) {
            /* Scan all expressions in module i for IDENT references to other modules */
            for (int d = 0; d < mods[i]->module.decl_count; d++) {
                Decl *child = mods[i]->module.decls[d];
                if (child->kind != DECL_LET || !child->let.init) continue;
                /* Walk expression tree looking for EXPR_IDENT matching other module names */
                Expr **stack = NULL;
                int sp = 0, stack_cap = 0;
                DA_APPEND(stack, sp, stack_cap, child->let.init);
                while (sp > 0) {
                    Expr *ex = stack[--sp];
                    if (!ex) continue;
                    if (ex->kind == EXPR_IDENT) {
                        /* Check if this ident refers to another module */
                        for (int j = 0; j < mod_count; j++) {
                            if (j == i) continue;
                            if (ex->ident.name == mods[j]->module.name) {
                                deps[i * mod_count + j] = true;
                            }
                        }
                    }
                    /* Push children to stack */
                    #define PUSH(e) DA_APPEND(stack, sp, stack_cap, (e))
                    switch (ex->kind) {
                    case EXPR_BINARY: PUSH(ex->binary.left); PUSH(ex->binary.right); break;
                    case EXPR_UNARY_PREFIX: PUSH(ex->unary_prefix.operand); break;
                    case EXPR_UNARY_POSTFIX: PUSH(ex->unary_postfix.operand); break;
                    case EXPR_CALL:
                        PUSH(ex->call.func);
                        for (int a = 0; a < ex->call.arg_count; a++) PUSH(ex->call.args[a]);
                        break;
                    case EXPR_FIELD: case EXPR_DEREF_FIELD: PUSH(ex->field.object); break;
                    case EXPR_INDEX: PUSH(ex->index.object); PUSH(ex->index.index); break;
                    case EXPR_IF:
                        PUSH(ex->if_expr.cond);
                        PUSH(ex->if_expr.then_body);
                        if (ex->if_expr.else_body) { PUSH(ex->if_expr.else_body); }
                        break;
                    case EXPR_BLOCK:
                        for (int s = 0; s < ex->block.count; s++) { PUSH(ex->block.stmts[s]); }
                        break;
                    case EXPR_FUNC:
                        for (int s = 0; s < ex->func.body_count; s++) { PUSH(ex->func.body[s]); }
                        break;
                    case EXPR_LET: PUSH(ex->let_expr.let_init); break;
                    case EXPR_ASSIGN: PUSH(ex->assign.target); PUSH(ex->assign.value); break;
                    case EXPR_RETURN:
                        if (ex->return_expr.value) { PUSH(ex->return_expr.value); }
                        break;
                    case EXPR_BREAK:
                        if (ex->break_expr.value) { PUSH(ex->break_expr.value); }
                        break;
                    case EXPR_LOOP:
                        for (int s = 0; s < ex->loop_expr.body_count; s++) { PUSH(ex->loop_expr.body[s]); }
                        break;
                    case EXPR_FOR:
                        PUSH(ex->for_expr.iter);
                        for (int s = 0; s < ex->for_expr.body_count; s++) { PUSH(ex->for_expr.body[s]); }
                        break;
                    case EXPR_MATCH:
                        PUSH(ex->match_expr.subject);
                        for (int a = 0; a < ex->match_expr.arm_count; a++) {
                            for (int s = 0; s < ex->match_expr.arms[a].body_count; s++) {
                                PUSH(ex->match_expr.arms[a].body[s]);
                            }
                        }
                        break;
                    case EXPR_CAST: PUSH(ex->cast.operand); break;
                    case EXPR_SOME: PUSH(ex->some_expr.value); break;
                    case EXPR_STRUCT_LIT:
                        for (int f = 0; f < ex->struct_lit.field_count; f++) { PUSH(ex->struct_lit.fields[f].value); }
                        break;
                    case EXPR_SLICE:
                        PUSH(ex->slice.object);
                        if (ex->slice.lo) { PUSH(ex->slice.lo); }
                        if (ex->slice.hi) { PUSH(ex->slice.hi); }
                        break;
                    case EXPR_ALLOC:
                        if (ex->alloc_expr.size_expr) { PUSH(ex->alloc_expr.size_expr); }
                        if (ex->alloc_expr.init_expr) { PUSH(ex->alloc_expr.init_expr); }
                        break;
                    case EXPR_FREE: PUSH(ex->free_expr.operand); break;
                    default: break;
                    }
                    #undef PUSH
                }
                free(stack);
            }
        }

        /* DFS cycle detection */
        int *color = calloc((size_t)mod_count, sizeof(int)); /* 0=white, 1=gray, 2=black */
        int *dfs_stack = malloc((size_t)mod_count * sizeof(int));
        bool found_cycle = false;
        for (int start = 0; start < mod_count && !found_cycle; start++) {
            if (color[start] != 0) continue;
            int dsp = 0;
            dfs_stack[dsp++] = start;
            color[start] = 1;
            while (dsp > 0 && !found_cycle) {
                int u = dfs_stack[dsp - 1];
                bool pushed = false;
                for (int v = 0; v < mod_count; v++) {
                    if (!deps[u * mod_count + v]) continue;
                    if (color[v] == 1) {
                        diag_error(mods[u]->loc,
                            "circular reference between modules '%s' and '%s'",
                            mods[u]->module.name, mods[v]->module.name);
                        found_cycle = true;
                        break;
                    }
                    if (color[v] == 0) {
                        color[v] = 1;
                        dfs_stack[dsp++] = v;
                        pushed = true;
                        break;
                    }
                }
                if (!pushed && !found_cycle) {
                    color[u] = 2;
                    dsp--;
                }
            }
        }

        free(dfs_stack);
        free(mods);
        free(deps);
        free(color);
    }
}
