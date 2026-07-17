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
    struct Symbol *parent;  /* enclosing module's Symbol (for every member kind — modules, lets, types); NULL for top-level. Set after pass1. */
    bool is_private;
    bool is_generic;
    const char **type_params;    /* ["'a", "'b"] — explicit vars first, then implicit */
    int type_param_count;
    int explicit_type_param_count;  /* how many of type_params are from <> decl */
    uint8_t *param_kinds;        /* GenParamKind per type_params entry; NULL = all GP_TYPE.
                                    Mutable: pass1's fixpoint and pass2's lazy body
                                    inference refine GP_UNKNOWN entries in place. */
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
    uint8_t *param_kinds;          /* mirrors the source Symbol's param_kinds */
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

/* ---- Declared error codes (`error` groups) ----
 * pass1_collect assigns every declared error constant a deterministic code:
 * fully-qualified names are sorted and numbered sequentially from
 * FC_ERROR_CODE_BASE (so a declared code is provably non-zero and the table
 * is diffable across builds). [1, 65535] is reserved platform passthrough
 * (errno / Win32 / WSA); 0 is the ok tag. The registry is rebuilt on every
 * pass1_collect (the LSP re-runs it per edit) and read by codegen (the
 * error_name table / --backtraces aborts) and the CLI (--emit-error-codes). */
#define FC_ERROR_CODE_BASE 65536

typedef struct ErrorCodeInfo {
    const char *qualified;  /* fully-qualified display name, e.g. "io.file_io.not_found" */
    SrcLoc loc;             /* declaration site of the member */
} ErrorCodeInfo;

int error_code_count(void);
ErrorCodeInfo error_code_info(int idx);  /* code = FC_ERROR_CODE_BASE + idx */

void symtab_init(SymbolTable *t);
Symbol *symtab_lookup(SymbolTable *t, const char *name);
Symbol *symtab_lookup_kind(SymbolTable *t, const char *name, DeclKind kind);
Symbol *symtab_lookup_kind_ns(SymbolTable *t, const char *name, DeclKind kind,
                               const char *ns_prefix);
Symbol *symtab_lookup_module(SymbolTable *t, const char *name, const char *ns_prefix);
void symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl);

/* Free the malloc'd tables hanging off a symtab's module symbols: each module
 * Symbol owns exactly one members table and at most one imports table
 * (ImportRefs only *reference* other tables), so a recursive walk frees each
 * exactly once. Does NOT free t->symbols itself — the caller owns that. */
void symtab_free_nested(SymbolTable *t);

/* Run pass 1: collect top-level declarations into symbol table.
 *
 * `require_main` gates the entry-point requirement: when true (the CLI), a
 * program with no `let main` is an error. When false (the in-process LSP, which
 * analyzes library code like the stdlib that has no entry point), the missing
 * `main` is tolerated and the entry-point-file restriction on top-level `let`
 * is skipped. */
void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern,
                   FileImportScopes *file_scopes, bool require_main);
