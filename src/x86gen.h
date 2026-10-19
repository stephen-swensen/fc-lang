#pragma once
#include "ast.h"
#include "monomorph.h"
#include "pass1.h"
#include <stdio.h>

/* Emit the program as an ELF relocatable object of x86-64 machine code
 * (System V ABI, position-independent) to `out`, a binary stream. The native
 * backend is an alternative to codegen_emit for fast development builds: the
 * output is linked against the C library with `cc out.o -o prog`, with no
 * assembler. It follows the semantics of the emitted C exactly; it does not
 * optimize. */
void x86gen_emit(Program *prog, FILE *out, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab);
