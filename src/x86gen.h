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

/* ---- Interactive sessions (fcc --interactive) ----
 *
 * A session compiles what each statement declares into an object of its own,
 * a unit, which the session loads into the running program. A unit's object
 * holds the unit's functions, globals and new generic instances, and refers
 * to the earlier units' by name (undefined symbols). In place of the C entry
 * point it has a runner, `int runner(void (*next)(void))`, which runs the
 * unit's initializers in order, storing each into its global (a statement
 * that yields nothing stores nothing), and then calls `next`, which runs the
 * rest of the session and does not return: so the runner's frame, which holds
 * what the initializers built on the stack, lives as long as the session, as
 * main's frame lives as long as a program. A `return` at the prompt returns
 * its i32 from the runner, ending the session. */
typedef struct {
    /* in */
    Decl **env;             /* every declaration of the session so far, this
                               unit's included: their modules make the C
                               environment (headers, defines, extern layouts) */
    int env_count;
    int mono_first;         /* the first generic instance this unit compiles */
    const char *runner;     /* the runner's symbol */
    /* out */
    unsigned char *desc;    /* malloc'd: how to read the value the unit binds
                               (X86Desc), or NULL when it binds no value */
    int desc_len;
} X86Unit;

void x86gen_emit_unit(Program *prog, Obj *obj, MonoTable *mono,
                      Arena *arena, InternTable *intern, SymbolTable *symtab,
                      const char *const *include_dirs, int ninclude_dirs, X86Unit *unit);

/* A value descriptor: the layout of a type as a byte string, for showing a
 * value read from memory. Each entry is an X86Desc byte and its operands;
 * numbers are 4-byte little-endian (an enum value 8), strings a number and
 * their bytes, and a nested type is another entry:
 *   INT, UINT, FLOAT: width            BOOL, PTR, FUNC: -
 *   STR, CSTR: len width (STR)         SLICE: len width, element size, element
 *   ARRAY: count, element size, element
 *   STRUCT: name, field count, then per field: name, offset, type
 *   TUPLE: count, then per element: offset, type
 *   UNION: name, payload offset, variant count, then per variant: name,
 *          has payload (0 or 1), the payload's type when it has one
 *   ENUM: name, width, signed, count, then per variant: value, name
 *   OPT_NULL (an option over a pointer, none = 0): the inner type
 *   OPT: flag offset, the inner type     RESULT: payload offset, has payload, type
 *   OPAQUE: name (a type shown by name only: deeper than the descriptor goes, or
 *           laid out by a C header) */
typedef enum {
    XD_INT = 1, XD_UINT, XD_FLOAT, XD_BOOL, XD_PTR, XD_FUNC, XD_STR, XD_CSTR, XD_SLICE,
    XD_ARRAY, XD_STRUCT, XD_TUPLE, XD_UNION, XD_ENUM, XD_OPT_NULL, XD_OPT, XD_RESULT,
    XD_OPAQUE,
} X86Desc;
