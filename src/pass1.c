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
                   .type = NULL, .members = NULL, .imports = NULL, .is_private = false,
                   .is_generic = false, .type_params = NULL, .type_param_count = 0 };
    DA_APPEND(t->symbols, t->count, t->capacity, sym);
}

/* ---- Import table helpers ---- */

/* Add or replace (shadow) an import ref in an import table */
static void import_table_add(ImportTable *tbl, const char *local_name,
                              const char *source_name, DeclKind kind,
                              SymbolTable *source_members, Symbol *msym) {
    /* Check for existing entry with same local_name (shadowing: replace) */
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].local_name == local_name) {
            tbl->entries[i].source_name = source_name;
            tbl->entries[i].kind = kind;
            tbl->entries[i].source_members = source_members;
            tbl->entries[i].module_members = msym->members;
            tbl->entries[i].is_generic = msym->is_generic;
            tbl->entries[i].type_params = msym->type_params;
            tbl->entries[i].type_param_count = msym->type_param_count;
            tbl->entries[i].explicit_type_param_count = msym->explicit_type_param_count;
            return;
        }
    }
    ImportRef ref = {
        .local_name = local_name,
        .source_name = source_name,
        .kind = kind,
        .source_members = source_members,
        .module_members = msym->members,
        .is_generic = msym->is_generic,
        .type_params = msym->type_params,
        .type_param_count = msym->type_param_count,
        .explicit_type_param_count = msym->explicit_type_param_count,
    };
    DA_APPEND(tbl->entries, tbl->count, tbl->capacity, ref);
}

/* Add a whole-module import to an import table.
 * source_name = module's name in global symtab (for lookup).
 * module_members = the imported module's member table (for dotted access). */
static void import_table_add_module(ImportTable *tbl, const char *local_name,
                                     Symbol *mod_sym, SymbolTable *global_symtab) {
    for (int i = 0; i < tbl->count; i++) {
        if (tbl->entries[i].local_name == local_name) {
            tbl->entries[i].kind = DECL_MODULE;
            tbl->entries[i].source_members = global_symtab;
            tbl->entries[i].source_name = mod_sym->name;
            tbl->entries[i].ns_prefix = mod_sym->ns_prefix;
            tbl->entries[i].module_members = mod_sym->members;
            tbl->entries[i].is_generic = false;
            tbl->entries[i].type_params = NULL;
            tbl->entries[i].type_param_count = 0;
            tbl->entries[i].explicit_type_param_count = 0;
            return;
        }
    }
    ImportRef ref = {
        .local_name = local_name,
        .kind = DECL_MODULE,
        .source_members = global_symtab,
        .source_name = mod_sym->name,
        .ns_prefix = mod_sym->ns_prefix,
        .module_members = mod_sym->members,
    };
    DA_APPEND(tbl->entries, tbl->count, tbl->capacity, ref);
}

/* Find or create a per-file import scope */
static ImportTable *get_file_imports(FileImportScopes *scopes, const char *filename) {
    for (int i = 0; i < scopes->count; i++) {
        if (scopes->scopes[i].filename == filename)
            return &scopes->scopes[i].imports;
    }
    FileImportScope scope = { .filename = filename };
    memset(&scope.imports, 0, sizeof(ImportTable));
    DA_APPEND(scopes->scopes, scopes->count, scopes->capacity, scope);
    return &scopes->scopes[scopes->count - 1].imports;
}

/* Process a member import (wildcard or named) into an ImportTable.
 * Handles: import * from MODULE, import NAME [as ALIAS] from MODULE.
 * Does NOT handle whole-module imports (import MODULE [as ALIAS]). */
static void process_member_import(Decl *d, ImportTable *target,
                                   SymbolTable *symtab, InternTable *intern  __attribute__((unused)),
                                   const char *current_ns) {
    const char *mod_name = d->import.from_module;
    const char *from_ns = d->import.from_namespace;

    /* Look up the source module: global symtab first, then import table
     * (a prior whole-module import in the same scope may have brought it in) */
    const char *lookup_ns = from_ns ? from_ns : current_ns;
    Symbol *mod_sym = symtab_lookup_module(symtab, mod_name, lookup_ns);
    if (!mod_sym) mod_sym = symtab_lookup_module(symtab, mod_name, NULL);
    if (!mod_sym || mod_sym->kind != DECL_MODULE) {
        /* Check the target import table for a whole-module import */
        for (int k = 0; k < target->count; k++) {
            ImportRef *ref = &target->entries[k];
            if (ref->local_name == mod_name && ref->kind == DECL_MODULE && ref->module_members) {
                /* Found via import — resolve using namespace-aware lookup */
                mod_sym = ref->ns_prefix
                    ? symtab_lookup_module(ref->source_members, ref->source_name, ref->ns_prefix)
                    : symtab_lookup(ref->source_members, ref->source_name);
                break;
            }
        }
    }
    if (!mod_sym || mod_sym->kind != DECL_MODULE) {
        diag_error(d->loc, "unknown module '%s'", mod_name);
        return;
    }

    if (d->import.is_wildcard) {
        /* import * from MODULE: add all non-private members */
        SymbolTable *members = mod_sym->members;
        for (int j = 0; j < members->count; j++) {
            Symbol *msym = &members->symbols[j];
            if (msym->is_private) continue;
            import_table_add(target, msym->name, msym->name, msym->kind,
                             members, msym);
        }
    } else {
        /* import NAME [as ALIAS] from MODULE */
        Symbol *msym = symtab_lookup(mod_sym->members, d->import.name);
        if (!msym) {
            diag_error(d->loc, "module '%s' has no member '%s'",
                mod_name, d->import.name);
            return;
        }
        if (msym->is_private) {
            diag_error(d->loc, "cannot import private member '%s' from module '%s'",
                d->import.name, mod_name);
            return;
        }
        const char *import_name = d->import.alias ? d->import.alias : d->import.name;
        import_table_add(target, import_name, d->import.name, msym->kind,
                         mod_sym->members, msym);
        /* Type-associated module: if importing a type, also import its
         * associated module under the same name. */
        if (msym->kind == DECL_STRUCT || msym->kind == DECL_UNION) {
            Symbol *assoc_mod = symtab_lookup_kind(mod_sym->members,
                d->import.name, DECL_MODULE);
            if (assoc_mod && !assoc_mod->is_private) {
                import_table_add(target, import_name, d->import.name,
                                 DECL_MODULE, mod_sym->members, assoc_mod);
            }
        }
    }
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

/* Build a mangled name: prefix__name
 * Uses __ (double underscore) to separate namespace/module hierarchy levels.
 * This avoids collisions between e.g. namespace foo:: module bar (foo__bar)
 * and global module foo_bar (foo_bar). */
static const char *make_mangled(InternTable *intern, const char *prefix, const char *name) {
    int needed = snprintf(NULL, 0, "%s__%s", prefix, name) + 1;
    char *buf = malloc((size_t)needed);
    snprintf(buf, (size_t)needed, "%s__%s", prefix, name);
    const char *result = intern_cstr(intern, buf);
    free(buf);
    return result;
}

/* Canonicalize generic struct/union stub names in a type tree.
 * Walks through compound type wrappers and, for TYPE_STRUCT/TYPE_UNION stubs
 * (field_count/variant_count == 0) with type_arg_count > 0, updates the stub's
 * name to the canonical mangled form from the module's symbol table.
 * This is a NAME-ONLY update: preserves type_args, doesn't replace with full type,
 * doesn't create circular references. Mutates the type in place. */
static void canonicalize_stub_names(Type *t, SymbolTable *members) {
    if (!t) return;
    switch (t->kind) {
    case TYPE_POINTER: canonicalize_stub_names(t->pointer.pointee, members); return;
    case TYPE_SLICE:   canonicalize_stub_names(t->slice.elem, members); return;
    case TYPE_OPTION:  canonicalize_stub_names(t->option.inner, members); return;
    case TYPE_FIXED_ARRAY: canonicalize_stub_names(t->fixed_array.elem, members); return;
    case TYPE_FUNC:
        for (int i = 0; i < t->func.param_count; i++)
            canonicalize_stub_names(t->func.param_types[i], members);
        canonicalize_stub_names(t->func.return_type, members);
        return;
    case TYPE_STRUCT:
        if (t->struc.field_count == 0 && t->struc.type_arg_count > 0 && t->struc.name) {
            Symbol *sym = symtab_lookup_kind(members, t->struc.name, DECL_STRUCT);
            if (sym && sym->type && sym->type->struc.name != t->struc.name) {
                t->struc.base_name = t->struc.name;
                t->struc.name = sym->type->struc.name;
                if (sym->type->struc.qualified_name)
                    t->struc.qualified_name = sym->type->struc.qualified_name;
            }
        }
        /* Don't recurse into fields — stubs have field_count=0 */
        return;
    case TYPE_UNION:
        if (t->unio.variant_count == 0 && t->unio.type_arg_count > 0 && t->unio.name) {
            Symbol *sym = symtab_lookup_kind(members, t->unio.name, DECL_UNION);
            if (sym && sym->type && sym->type->unio.name != t->unio.name) {
                t->unio.base_name = t->unio.name;
                t->unio.name = sym->type->unio.name;
                if (sym->type->unio.qualified_name)
                    t->unio.qualified_name = sym->type->unio.qualified_name;
            }
        }
        return;
    default: return;
    }
}

/* Resolve type stubs in a type tree against a symbol table.
 * Recursively walks the type tree (through pointers, slices, options, fixed arrays,
 * and function types) and replaces TYPE_STRUCT stubs (field_count == 0) with the
 * actual type from the symtab. Used to resolve references to sibling extern types
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
    if (t->kind == TYPE_FIXED_ARRAY) {
        Type *inner = resolve_type_stubs(t->fixed_array.elem, members);
        if (inner != t->fixed_array.elem) {
            Type *r = malloc(sizeof(Type));
            *r = *t;
            r->fixed_array.elem = inner;
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
    if (t->kind == TYPE_STRUCT && t->struc.field_count == 0 && t->struc.name
        && t->struc.type_arg_count == 0) {
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
    st->struc.is_c_union = d->struc.is_c_union;
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
/* Build a qualified display name: prefix.name */
static const char *make_qualified(InternTable *intern, const char *prefix, const char *name) {
    int needed = snprintf(NULL, 0, "%s.%s", prefix, name) + 1;
    char *buf = malloc((size_t)needed);
    snprintf(buf, (size_t)needed, "%s.%s", prefix, name);
    const char *result = intern_cstr(intern, buf);
    free(buf);
    return result;
}

static void register_module_members(Decl *d, const char *mangle_prefix,
                                    const char *display_prefix,
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
                "extern %s in module '%s' requires a 'from' clause on the module",
                child->struc.is_c_union ? "union" : "struct", mod_name);
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
                st->struc.qualified_name = make_qualified(intern, display_prefix, src_name);
                st->struc.c_name = child->struc.c_name;
                st->struc.is_c_union = child->struc.is_c_union;
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
                ut->unio.qualified_name = make_qualified(intern, display_prefix, src_name);
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
            /* Validate types for extern constants (non-function externs).
             * Function-type externs are extern function declarations.
             * Constants may only have scalar or pointer types — types that
             * map directly to C #define constant values. */
            Type *et = child->ext.type;
            if (et && et->kind != TYPE_FUNC) {
                const char *reason = NULL;
                switch (et->kind) {
                case TYPE_SLICE:
                case TYPE_FIXED_ARRAY:
                    reason = "slice"; break;
                case TYPE_OPTION:
                    reason = "option"; break;
                case TYPE_STRUCT:
                    reason = "struct"; break;
                case TYPE_UNION:
                    reason = "union"; break;
                case TYPE_VOID:
                    reason = "void"; break;
                default: break;
                }
                if (reason) {
                    diag_error(child->loc,
                        "extern constant '%s' cannot have %s type '%s'",
                        child->ext.name, reason, type_name(et));
                }
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
                register_module_members(child, sub_prefix, make_qualified(intern, display_prefix, sub_name), sub_members, intern, global_symtab);
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
            register_module_members(child, sub_prefix, make_qualified(intern, display_prefix, sub_name), sub_members, intern, global_symtab);
            break;
        }
        default:
            break;
        }
    }

    /* Resolve type stubs in all symbols against sibling types within the module.
     * A single comprehensive pass that walks every type tree in every symbol,
     * covering extern function signatures, struct field types, and any other
     * position where a type stub may appear. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if (!msym->type) continue;

        /* Resolve the symbol's top-level type (e.g. extern function signatures
         * whose param/return types reference sibling extern structs/unions) */
        Type *resolved = resolve_type_stubs(msym->type, members);
        if (resolved != msym->type) {
            msym->type = resolved;
            if (msym->kind == DECL_EXTERN && msym->decl)
                msym->decl->ext.type = resolved;
        }

        /* Resolve stubs within struct field types */
        if (msym->type->kind == TYPE_STRUCT) {
            Type *st = msym->type;
            for (int k = 0; k < st->struc.field_count; k++) {
                Type *fr = resolve_type_stubs(st->struc.fields[k].type, members);
                if (fr != st->struc.fields[k].type)
                    st->struc.fields[k].type = fr;
            }
        }

    }

    /* Canonicalize generic stub names in struct/union field types.
     * Updates parser names (e.g., "inner") to canonical mangled names (e.g., "m__inner")
     * so downstream code doesn't need module context to resolve them. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if (!msym->type) continue;
        if (msym->type->kind == TYPE_STRUCT) {
            for (int k = 0; k < msym->type->struc.field_count; k++)
                canonicalize_stub_names(msym->type->struc.fields[k].type, members);
        }
        if (msym->type->kind == TYPE_UNION) {
            for (int k = 0; k < msym->type->unio.variant_count; k++)
                canonicalize_stub_names(msym->type->unio.variants[k].payload, members);
        }
    }

    /* Register module-scoped struct/union types in the global symbol table under
     * their mangled names, so resolve_type can find them from any context. */
    for (int j = 0; j < members->count; j++) {
        Symbol *msym = &members->symbols[j];
        if ((msym->kind == DECL_STRUCT || msym->kind == DECL_UNION) && msym->type) {
            const char *mangled_name = (msym->kind == DECL_STRUCT)
                ? msym->type->struc.name : msym->type->unio.name;
            symtab_add(global_symtab, mangled_name, msym->kind, msym->decl);
            Symbol *gsym = &global_symtab->symbols[global_symtab->count - 1];
            gsym->type = msym->type;
            gsym->is_generic = msym->is_generic;
            gsym->type_params = msym->type_params;
            gsym->type_param_count = msym->type_param_count;
            gsym->explicit_type_param_count = msym->explicit_type_param_count;
        }
    }
}

/* Process imports for a single module symbol */
static void process_module_level_imports(Symbol *ms, SymbolTable *global_symtab,
                                          InternTable *intern) {
    Decl *mod_decl = ms->decl;
    if (!mod_decl) return;

    bool has_imports = false;
    for (int j = 0; j < mod_decl->module.decl_count; j++) {
        if (mod_decl->module.decls[j]->kind == DECL_IMPORT) {
            has_imports = true;
            break;
        }
    }
    if (!has_imports) return;

    ImportTable *imports = malloc(sizeof(ImportTable));
    memset(imports, 0, sizeof(ImportTable));
    ms->imports = imports;

    const char *mod_ns = ms->ns_prefix;
    for (int j = 0; j < mod_decl->module.decl_count; j++) {
        Decl *d = mod_decl->module.decls[j];
        if (d->kind != DECL_IMPORT) continue;

        const char *imp_mod = d->import.from_module;
        const char *imp_ns = d->import.from_namespace;

        if (imp_mod) {
            /* import X from MODULE or import * from MODULE */
            process_member_import(d, imports, global_symtab, intern, mod_ns);
        } else if (imp_ns && !imp_mod) {
            /* import MODULE from NS:: */
            if (d->import.is_wildcard) {
                diag_error(d->loc, "cannot wildcard-import from a namespace");
                continue;
            }
            const char *name = d->import.name;
            Symbol *src = symtab_lookup_module(global_symtab, name, imp_ns);
            if (!src || src->kind != DECL_MODULE) {
                diag_error(d->loc, "unknown module '%s' in namespace '%s'", name, imp_ns);
                continue;
            }
            const char *import_name = d->import.alias ? d->import.alias : name;
            import_table_add_module(imports, import_name, src, global_symtab);
        } else {
            /* import MODULE [as ALIAS] */
            const char *name = d->import.name;
            Symbol *src = symtab_lookup_module(global_symtab, name, mod_ns);
            if (!src) src = symtab_lookup_module(global_symtab, name, NULL);
            if (!src || src->kind != DECL_MODULE) {
                diag_error(d->loc, "unknown module '%s'", name);
                continue;
            }
            const char *import_name = d->import.alias ? d->import.alias : name;
            import_table_add_module(imports, import_name, src, global_symtab);
        }
    }
}

/* Process imports for all top-level modules in global symtab */
static void resolve_module_imports(SymbolTable *symtab, InternTable *intern) {
    for (int i = 0; i < symtab->count; i++) {
        Symbol *ms = &symtab->symbols[i];
        if (ms->kind != DECL_MODULE) continue;
        process_module_level_imports(ms, symtab, intern);
    }
}

/* Recursively process imports for nested submodules */
static void resolve_nested_module_imports(SymbolTable *members,
                                           SymbolTable *global_symtab,
                                           InternTable *intern,
                                           const char *ns_prefix __attribute__((unused))) {
    for (int i = 0; i < members->count; i++) {
        Symbol *ms = &members->symbols[i];
        if (ms->kind != DECL_MODULE || !ms->members) continue;
        process_module_level_imports(ms, global_symtab, intern);
        resolve_nested_module_imports(ms->members, global_symtab, intern, ms->ns_prefix);
    }
}

void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern,
                   FileImportScopes *file_scopes) {
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

        /* Build display prefix for qualified names: [namespace::]module */
        const char *display_prefix;
        if (ns_prefix) {
            int needed = snprintf(NULL, 0, "%s::%s", ns_prefix, mod_name) + 1;
            char *buf = malloc((size_t)needed);
            snprintf(buf, (size_t)needed, "%s::%s", ns_prefix, mod_name);
            display_prefix = intern_cstr(intern, buf);
            free(buf);
        } else {
            display_prefix = mod_name;
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
                register_module_members(d, mangle_prefix, display_prefix, members, intern, symtab);
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
        register_module_members(d, mangle_prefix, display_prefix, members, intern, symtab);
    }

    /* Phase 2: Register top-level (non-module) decls.
     *
     * Restrictions:
     * - Non-global namespaces: only modules allowed at top level.
     * - Global namespace: non-module top-level decls (let, struct, union)
     *   must all be in a single file (the entry-point file with let main).
     *   This prevents cross-file ordering dependencies. */

    /* Pre-scan: find the entry-point file (the one containing let main).
     * Only that file may have non-module top-level declarations. */
    const char *entry_file = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_LET && strcmp(d->let.name, "main") == 0) {
            entry_file = d->loc.filename;
            break;
        }
    }
    if (!entry_file) {
        diag_error((SrcLoc){0}, "no entry point: program must contain 'let main'");
    }
    current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) { current_ns = d->ns.name; continue; }
        if (current_ns) continue; /* non-global namespace — checked below */
        if (d->kind == DECL_LET || d->kind == DECL_STRUCT || d->kind == DECL_UNION) {
            if (d->loc.filename != entry_file) {
                diag_error(d->loc,
                    "top-level %s '%s' not allowed here — "
                    "non-module top-level declarations are only allowed "
                    "in the entry-point file (the file containing let main)",
                    d->kind == DECL_LET ? "let" :
                    d->kind == DECL_STRUCT ? "struct" : "union",
                    d->kind == DECL_LET ? d->let.name :
                    d->kind == DECL_STRUCT ? d->struc.name : d->unio.name);
            }
        }
    }

    current_ns = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind == DECL_NAMESPACE) {
            current_ns = d->ns.name;
            continue;
        }
        /* Non-global namespaces may only contain module declarations at top level */
        if (current_ns && (d->kind == DECL_LET || d->kind == DECL_STRUCT || d->kind == DECL_UNION)) {
            diag_error(d->loc,
                "top-level %s not allowed in namespace '%s::' — "
                "wrap it in a module or move it to a global:: file",
                d->kind == DECL_LET ? "let" :
                d->kind == DECL_STRUCT ? "struct" : "union",
                current_ns);
            continue;
        }
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
                diag_error(d->loc, "extern %s must be inside a module with a 'from' clause",
                    d->struc.is_c_union ? "union" : "struct");
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

    /* Phase 3: Process imports.
     * Imports are lexically scoped: module-level imports are scoped to the
     * module (visible to children), file-level member imports go into per-file
     * tables. Whole-module imports (import MODULE [as ALIAS]) still go to the
     * global symtab for dotted-name resolution. */

    /* Phase 3a: Module-level imports — process recursively (including nested submodules) */
    resolve_module_imports(symtab, intern);
    /* Also process nested submodules within each top-level module */
    for (int i = 0; i < symtab->count; i++) {
        Symbol *ms = &symtab->symbols[i];
        if (ms->kind != DECL_MODULE || !ms->members) continue;
        resolve_nested_module_imports(ms->members, symtab, intern, ms->ns_prefix);
    }

    /* Phase 3b: File-level imports */
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

        /* All imports go to the file's ImportTable */
        ImportTable *file_tbl = get_file_imports(file_scopes, d->loc.filename);

        if (!mod_name && !from_ns) {
            /* import MODULE [as ALIAS] — whole module import */
            const char *name = d->import.name;
            Symbol *sym = symtab_lookup_module(symtab, name, current_ns);
            if (!sym) sym = symtab_lookup_module(symtab, name, NULL);
            if (!sym || sym->kind != DECL_MODULE) {
                diag_error(d->loc, "unknown module '%s'", name);
                continue;
            }
            const char *import_name = d->import.alias ? d->import.alias : name;
            import_table_add_module(file_tbl, import_name, sym, symtab);
            continue;
        }

        if (from_ns && !mod_name) {
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
            import_table_add_module(file_tbl, import_name, sym, symtab);
            continue;
        }

        /* Member import (import X from M / import * from M) */
        process_member_import(d, file_tbl, symtab, intern, current_ns);
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
                    case EXPR_ASSERT:
                        PUSH(ex->assert_expr.condition);
                        if (ex->assert_expr.message) { PUSH(ex->assert_expr.message); }
                        break;
                    case EXPR_DEFER: PUSH(ex->defer_expr.value); break;
                    default: break;
                    }
                    #undef PUSH
                }
                free(stack);
            }
        }

        /* Also add edges from import statements: if module i imports from module j,
         * record a dependency.  This catches cycles through imports that the
         * expression-based scan above misses (import names are not module names). */
        for (int i = 0; i < mod_count; i++) {
            for (int d = 0; d < mods[i]->module.decl_count; d++) {
                Decl *child = mods[i]->module.decls[d];
                if (child->kind != DECL_IMPORT) continue;
                const char *from = child->import.from_module;
                if (!from) continue;
                for (int j = 0; j < mod_count; j++) {
                    if (j == i) continue;
                    if (from == mods[j]->module.name)
                        deps[i * mod_count + j] = true;
                }
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
