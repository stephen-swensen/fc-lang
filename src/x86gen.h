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
 * C exactly; it does not optimize. Linux x86-64 only: on any other host it
 * reports an error. */
void x86gen_emit(Program *prog, Obj *obj, MonoTable *mono,
                 Arena *arena, InternTable *intern, SymbolTable *symtab);
