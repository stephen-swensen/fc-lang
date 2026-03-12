#pragma once
#include "ast.h"
#include <stdio.h>

/* Emit C11 code for the program to the given file handle */
void codegen_emit(Program *prog, FILE *out);
