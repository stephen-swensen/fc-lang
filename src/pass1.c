#include "pass1.h"
#include "diag.h"
#include <stdlib.h>
#include <string.h>

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
    Symbol sym = { .name = name, .kind = kind, .decl = decl, .type = NULL };
    DA_APPEND(t->symbols, t->count, t->capacity, sym);
}

void pass1_collect(Program *prog, SymbolTable *symtab) {
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
                symtab_add(symtab, d->struc.name, DECL_STRUCT, d);
                /* Set the symbol's type to the struct type */
                Symbol *sym = symtab_lookup(symtab, d->struc.name);
                Type *st = malloc(sizeof(Type));
                st->kind = TYPE_STRUCT;
                st->struc.name = d->struc.name;
                st->struc.fields = d->struc.fields;
                st->struc.field_count = d->struc.field_count;
                sym->type = st;
            }
            break;
        }
        case DECL_UNION: {
            if (symtab_lookup(symtab, d->unio.name)) {
                diag_error(d->loc, "redefinition of '%s'", d->unio.name);
            } else {
                symtab_add(symtab, d->unio.name, DECL_UNION, d);
                /* Set the symbol's type to the union type */
                Symbol *sym = symtab_lookup(symtab, d->unio.name);
                Type *ut = malloc(sizeof(Type));
                ut->kind = TYPE_UNION;
                ut->unio.name = d->unio.name;
                ut->unio.variants = d->unio.variants;
                ut->unio.variant_count = d->unio.variant_count;
                sym->type = ut;
            }
            break;
        }
        default:
            break;
        }
    }
}
