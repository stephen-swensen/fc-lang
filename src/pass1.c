#include "pass1.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl) {
    Symbol sym = { .name = name, .kind = kind, .decl = decl, .type = NULL,
                   .members = NULL, .is_private = false };
    DA_APPEND(t->symbols, t->count, t->capacity, sym);
}

/* Build a mangled name: prefix_name */
static const char *make_mangled(InternTable *intern, const char *prefix, const char *name) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s_%s", prefix, name);
    return intern_cstr(intern, buf);
}

/* Register a struct type symbol and return the created type */
static Type *register_struct_sym(SymbolTable *tab, Decl *d) {
    symtab_add(tab, d->struc.name, DECL_STRUCT, d);
    Symbol *sym = symtab_lookup(tab, d->struc.name);
    Type *st = malloc(sizeof(Type));
    st->kind = TYPE_STRUCT;
    st->struc.name = d->struc.name;
    st->struc.fields = d->struc.fields;
    st->struc.field_count = d->struc.field_count;
    sym->type = st;
    return st;
}

/* Register a union type symbol and return the created type */
static Type *register_union_sym(SymbolTable *tab, Decl *d) {
    symtab_add(tab, d->unio.name, DECL_UNION, d);
    Symbol *sym = symtab_lookup(tab, d->unio.name);
    Type *ut = malloc(sizeof(Type));
    ut->kind = TYPE_UNION;
    ut->unio.name = d->unio.name;
    ut->unio.variants = d->unio.variants;
    ut->unio.variant_count = d->unio.variant_count;
    sym->type = ut;
    return ut;
}

void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern) {
    /* Determine namespace prefix (from first DECL_NAMESPACE, if any) */
    const char *ns_prefix = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == DECL_NAMESPACE) {
            ns_prefix = prog->decls[i]->ns.name;
            break;
        }
    }

    /* Phase 1: Register modules */
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind != DECL_MODULE) continue;

        const char *mod_name = d->module.name;

        /* Build mangling prefix: [namespace_]module */
        const char *mangle_prefix;
        if (ns_prefix) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s_%s", ns_prefix, mod_name);
            mangle_prefix = intern_cstr(intern, buf);
        } else {
            mangle_prefix = mod_name;
        }

        /* Check for duplicate module name */
        if (symtab_lookup(symtab, mod_name)) {
            diag_error(d->loc, "redefinition of module '%s'", mod_name);
            continue;
        }

        /* Register module in global symtab */
        symtab_add(symtab, mod_name, DECL_MODULE, d);
        Symbol *mod_sym = symtab_lookup(symtab, mod_name);
        mod_sym->is_private = d->is_private;

        /* Create sub-symbol table for module members */
        SymbolTable *members = malloc(sizeof(SymbolTable));
        symtab_init(members);
        mod_sym->members = members;

        /* Walk child decls, compute mangled names, register in members */
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
                }
                break;
            }
            case DECL_STRUCT: {
                const char *src_name = child->struc.name;
                const char *mangled = make_mangled(intern, mangle_prefix, src_name);
                /* Mangle the struct name in the Decl and Type */
                child->struc.name = mangled;
                if (symtab_lookup(members, src_name)) {
                    diag_error(child->loc, "redefinition of '%s' in module '%s'",
                        src_name, mod_name);
                } else {
                    symtab_add(members, src_name, DECL_STRUCT, child);
                    Symbol *msym = symtab_lookup(members, src_name);
                    msym->is_private = child->is_private;
                    /* Create type with mangled name */
                    Type *st = malloc(sizeof(Type));
                    st->kind = TYPE_STRUCT;
                    st->struc.name = mangled;
                    st->struc.fields = child->struc.fields;
                    st->struc.field_count = child->struc.field_count;
                    msym->type = st;
                }
                break;
            }
            case DECL_UNION: {
                const char *src_name = child->unio.name;
                const char *mangled = make_mangled(intern, mangle_prefix, src_name);
                /* Mangle the union name in the Decl */
                child->unio.name = mangled;
                if (symtab_lookup(members, src_name)) {
                    diag_error(child->loc, "redefinition of '%s' in module '%s'",
                        src_name, mod_name);
                } else {
                    symtab_add(members, src_name, DECL_UNION, child);
                    Symbol *msym = symtab_lookup(members, src_name);
                    msym->is_private = child->is_private;
                    /* Create type with mangled name */
                    Type *ut = malloc(sizeof(Type));
                    ut->kind = TYPE_UNION;
                    ut->unio.name = mangled;
                    ut->unio.variants = child->unio.variants;
                    ut->unio.variant_count = child->unio.variant_count;
                    msym->type = ut;
                }
                break;
            }
            default:
                break;
            }
        }
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
            break;
        }
        case DECL_STRUCT: {
            if (symtab_lookup(symtab, d->struc.name)) {
                diag_error(d->loc, "redefinition of '%s'", d->struc.name);
            } else {
                register_struct_sym(symtab, d);
            }
            break;
        }
        case DECL_UNION: {
            if (symtab_lookup(symtab, d->unio.name)) {
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
    for (int i = 0; i < prog->decl_count; i++) {
        Decl *d = prog->decls[i];
        if (d->kind != DECL_IMPORT) continue;

        const char *mod_name = d->import.from_module;

        if (!mod_name) {
            /* import MODULE (whole module import — no-op, module already accessible) */
            const char *name = d->import.name;
            Symbol *sym = symtab_lookup(symtab, name);
            if (!sym || sym->kind != DECL_MODULE)
                diag_error(d->loc, "unknown module '%s'", name);
            continue;
        }

        /* Look up the source module */
        Symbol *mod_sym = symtab_lookup(symtab, mod_name);
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
                    Symbol *new_sym = symtab_lookup(symtab, import_name);
                    new_sym->type = msym->type;
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
                Symbol *new_sym = symtab_lookup(symtab, import_name);
                new_sym->type = msym->type;
            }
        }
    }
}
