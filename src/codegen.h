#pragma once
#include "ast.h"
#include "monomorph.h"
#include "pass1.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    bool backtraces;
} CodegenOptions;

/* Emit the program as C (C11 plus the GNU extensions the spec lists) to `out`,
 * which must be open for update ("w+"): the body is written first, read back
 * to choose the preamble, and the file rewritten from its start. */
void codegen_emit(Program *prog, FILE *out, MonoTable *mono,
                  Arena *arena, InternTable *intern, SymbolTable *symtab,
                  const CodegenOptions *opts);
