#pragma once
#include "ast.h"
#include "common.h"

/* Symbol collected by pass 1 */
typedef struct SymbolTable SymbolTable;

typedef struct {
    const char *name;
    DeclKind kind;
    Decl *decl;
    Type *type;             /* NULL until pass2 resolves it */
    SymbolTable *members;   /* non-NULL for DECL_MODULE */
    bool is_private;
} Symbol;

struct SymbolTable {
    Symbol *symbols;
    int count;
    int capacity;
};

void symtab_init(SymbolTable *t);
Symbol *symtab_lookup(SymbolTable *t, const char *name);
void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl);

/* Run pass 1: collect top-level declarations into symbol table */
void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern);
