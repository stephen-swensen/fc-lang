#pragma once
#include "ast.h"
#include "common.h"

/* Symbol collected by pass 1 */
typedef struct SymbolTable SymbolTable;

typedef struct {
    const char *name;
    const char *ns_prefix;  /* namespace prefix (mangled), NULL = global */
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
Symbol *symtab_lookup_kind(SymbolTable *t, const char *name, DeclKind kind);
Symbol *symtab_lookup_module(SymbolTable *t, const char *name, const char *ns_prefix);
void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl);

/* Run pass 1: collect top-level declarations into symbol table */
void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern);
