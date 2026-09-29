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
    struct Symbol *parent;  /* enclosing module's Symbol, for every member kind
                               (modules, lets, types); NULL for top-level. Set
                               at the end of pass1 (set_module_parents). */
    bool is_private;
    bool is_generic;
    const char **type_params;    /* ["'a", "'b"]: explicit vars first, then implicit */
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
/* One name an import brings into scope: the name as written there (an `as`
 * alias, or the source name) and the symbol it resolved to. The symbol is held
 * directly, resolved once where the import is processed; every symbol exists
 * by then, so the pointer stays valid. */
typedef struct ImportRef {
    const char *local_name;        /* interned; name visible in importing scope */
    DeclKind kind;
    Symbol *sym;                   /* the imported symbol */
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
 * error_name table / --backtraces aborts) and the CLI (the .errcodes map). */
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
/* The struct, union or enum named `name`, ignoring namespaces. */
Symbol *symtab_lookup_type(SymbolTable *t, const char *name);
Symbol *symtab_lookup_module(SymbolTable *t, const char *name, const char *ns_prefix);
/* Add a symbol and return it. The pointer is valid until the next symtab_add
 * on the same table, which may move the array. */
Symbol *symtab_add(SymbolTable *t, const char *name, DeclKind kind, Decl *decl);

/* Free everything a symbol table owns: its symbols and, recursively, each
 * module's members table and imports table (ImportRefs only reference other
 * tables, so every table is freed once). */
void symtab_free(SymbolTable *t);
void file_scopes_free(FileImportScopes *s);

/* The file-level imports of `filename` (its interned name), NULL when it has
 * none or either argument is NULL. */
ImportTable *file_imports_find(FileImportScopes *scopes, const char *filename);

/* Run pass 1: collect top-level declarations into symbol table.
 *
 * `require_main` gates the entry-point requirement: when true (the CLI), a
 * program with no `let main` is an error. When false (the in-process LSP, which
 * analyzes library code like the stdlib that has no entry point), the missing
 * `main` is tolerated and the entry-point-file restriction on top-level `let`
 * is skipped. */
void pass1_collect(Program *prog, SymbolTable *symtab, InternTable *intern,
                   FileImportScopes *file_scopes, bool require_main);
