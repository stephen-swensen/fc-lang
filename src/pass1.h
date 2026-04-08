#pragma once
#include "ast.h"
#include "common.h"

/* Symbol collected by pass 1 */
typedef struct SymbolTable SymbolTable;

typedef struct Symbol {
    const char *name;
    const char *ns_prefix;  /* namespace prefix (mangled), NULL = global */
    DeclKind kind;
    Decl *decl;
    Type *type;             /* NULL until pass2 resolves it */
    SymbolTable *members;   /* non-NULL for DECL_MODULE */
    struct ImportTable *imports; /* non-NULL for modules with internal imports */
    bool is_private;
    bool is_generic;
    const char **type_params;    /* ["'a", "'b"] — explicit vars first, then implicit */
    int type_param_count;
    int explicit_type_param_count;  /* how many of type_params are from <> decl */
} Symbol;

struct SymbolTable {
    Symbol *symbols;
    int count;
    int capacity;
};

/* Import reference: transparent alias pointing to a source module's member */
typedef struct ImportRef {
    const char *local_name;        /* interned — name visible in importing scope */
    const char *source_name;       /* interned — name in source module's members */
    DeclKind kind;
    SymbolTable *source_members;   /* source module's member table (stable pointer) */
    SymbolTable *module_members;   /* for DECL_MODULE imports: the imported module's members */
    const char *ns_prefix;         /* for DECL_MODULE imports: source module's namespace */
    bool is_generic;
    const char **type_params;
    int type_param_count;
    int explicit_type_param_count;
} ImportRef;

typedef struct ImportTable {
    ImportRef *entries;
    int count;
    int capacity;
} ImportTable;

/* Per-file import scope */
typedef struct FileImportScope {
    const char *filename;          /* interned, or NULL for single-file mode */
    ImportTable imports;
} FileImportScope;

typedef struct FileImportScopes {
    FileImportScope *scopes;
    int count;
    int capacity;
} FileImportScopes;

void symtab_init(SymbolTable *t);
Symbol *symtab_lookup(SymbolTable *t, const char *name);
Symbol *symtab_lookup_kind(SymbolTable *t, const char *name, DeclKind kind);
Symbol *symtab_lookup_module(SymbolTable *t, const char *name, const char *ns_prefix);
void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl);

/* Run pass 1: collect top-level declarations into symbol table */
void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern,
                   FileImportScopes *file_scopes);
