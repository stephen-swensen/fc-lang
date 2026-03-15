#pragma once
#include "ast.h"
#include "pass1.h"
#include "monomorph.h"

/* Run pass 2: type-check all expressions, fill in type annotations */
void pass2_check(Program *prog, SymbolTable *symtab, InternTable *intern, MonoTable *mono);
