#pragma once
#include "ast.h"

/* Symbol collected by pass 1 */
typedef struct {
    const char *name;
    DeclKind kind;
    Decl *decl;
    Type *type;     /* NULL until pass2 resolves it */
} Symbol;

typedef struct {
    Symbol *symbols;
    int count;
    int capacity;
} SymbolTable;

void symtab_init(SymbolTable *t);
Symbol *symtab_lookup(SymbolTable *t, const char *name);
void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl);

/* Run pass 1: collect top-level declarations into symbol table */
void pass1_collect(Program *prog, SymbolTable *symtab);
