#pragma once
#include "args.h"

/* The interactive session, `fcc --interactive`: a prompt at which FC
 * declarations and statements are entered, compiled to native code and run
 * as they are entered, each against what the inputs before it left. `.fc`
 * inputs are loaded first, with the standard library, and their `main` does
 * not run; `.fcx` inputs are then entered as if typed. Returns the process
 * exit status: 0 when the session ends at the end of the input (Ctrl+D), the
 * code a `return` at the prompt gives or the program exits with, 1 when an
 * input does not compile and stdin is not a terminal, and 128 plus the
 * signal's number for a statement killed by one (an abort) then. */
int repl_main(const CompileArgs *ca);
