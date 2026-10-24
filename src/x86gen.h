#pragma once
#include "ast.h"
#include "elfobj.h"
#include "monomorph.h"
#include "pass1.h"

/* Compile the program into `obj` (obj_init'ed by the caller; the caller frees
 * it): an ELF relocatable object of x86-64 machine code (System V ABI,
 * position-independent), with the C header shim merged in. The native backend
 * is an alternative to codegen_emit for fast development builds: elf_link
 * makes the object an executable, or `fcc --native -c` writes it for the C
 * compiler to link. No assembler runs. It follows the semantics of the emitted
 * C exactly, optimizing only within a function. Linux x86-64 only: on any
 * other host it reports an error. `include_dirs` (-I) are searched for the
 * headers extern declarations name, after the sources' directories and before
 * the C compiler's own (CPATH and C_INCLUDE_PATH, then the system's). With
 * `backtraces` (--backtraces), a failure prints the FC call stack before it
 * aborts. */
void x86gen_emit(Program *prog, Obj *obj, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab,
                 const char *const *include_dirs, int ninclude_dirs, bool backtraces);
