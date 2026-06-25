#pragma once
#include "ast.h"
#include "pass1.h"
#include "monomorph.h"

/* Run pass 2: type-check all expressions, fill in type annotations.
 * `arena` owns the type nodes pass2 synthesizes (widened/option/pointer types,
 * substitutions, scopes); they are referenced from the AST, so it must outlive
 * the AST. Pass the same arena that owns the AST — the caller frees it. */
void pass2_check(Program *prog, SymbolTable *symtab, InternTable *intern, MonoTable *mono,
                  FileImportScopes *file_scopes, Arena *arena);
