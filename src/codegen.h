#pragma once
#include "ast.h"
#include "monomorph.h"
#include "pass1.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    bool debug_trace;
} CodegenOptions;

/* Emit C11 code for the program to the given file handle */
void codegen_emit(Program *prog, FILE *out, MonoTable *mono,
                  Arena *arena, InternTable *intern, SymbolTable *symtab,
                  const CodegenOptions *opts);
