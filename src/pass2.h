#pragma once
#include "ast.h"
#include "pass1.h"
#include "monomorph.h"

/* Run pass 2: type-check all expressions, fill in type annotations.
 * `arena` owns the type nodes pass2 synthesizes (widened/option/pointer types,
 * substitutions, scopes); they are referenced from the AST, so it must outlive
 * the AST. Pass the same arena that owns the AST; the caller frees it. */
void pass2_check(Program *prog, SymbolTable *symtab, InternTable *intern, MonoTable *mono,
                  FileImportScopes *file_scopes, Arena *arena);

/* Free the state a compilation's front end built (the tables pass1 and pass2
 * fill, the instance list, the intern hash array) and then the arena, which
 * holds the AST, the types and the interned names. fcc and the language
 * server both tear a compilation down with it. */
void front_end_free(SymbolTable *symtab, InternTable *intern, MonoTable *mono,
                    FileImportScopes *file_scopes, Arena *arena);
