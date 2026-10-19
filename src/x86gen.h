#pragma once
#include "ast.h"
#include "monomorph.h"
#include "pass1.h"
#include <stdio.h>

/* Emit the program as x86-64 assembly (GNU as, AT&T syntax, System V ABI,
 * position-independent) to `out`. The native backend is an alternative to
 * codegen_emit for fast development builds: the output is assembled and
 * linked against the C library with `cc out.s -o prog`. It follows the
 * semantics of the emitted C exactly; it does not optimize. */
void x86gen_emit(Program *prog, FILE *out, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab);
