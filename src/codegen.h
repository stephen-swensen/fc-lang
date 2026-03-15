#pragma once
#include "ast.h"
#include "monomorph.h"
#include "pass1.h"
#include <stdio.h>

/* Emit C11 code for the program to the given file handle */
void codegen_emit(Program *prog, FILE *out, MonoTable *mono,
                  Arena *arena, InternTable *intern, SymbolTable *symtab);
