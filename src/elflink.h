#pragma once
#include "elfobj.h"
#include <stdbool.h>

/* The native backend's linker: an object (the backend's, in memory) and the
 * inputs named on the command line become a position-independent executable
 * for glibc on Linux x86-64, as `cc -pie -z now -z relro` would make it.
 * See elflink.c for what it supports. */

typedef struct {
    /* In command-line order: object files, archives, shared libraries and
     * linker scripts naming them, and "-lNAME" for a library to search for
     * (libNAME.so, else libNAME.a). libm and libc follow them. */
    const char *const *inputs;
    int ninputs;
    /* -L directories, searched before LIBRARY_PATH's and the system's. */
    const char *const *dirs;
    int ndirs;
} LinkArgs;

/* Link `o` (resolving into it: archive members and the linker's own
 * definitions are merged in) and write the executable to `path`. Reports
 * each problem with diag_error_simple and returns false on any. Linux x86-64
 * only, like the backend. */
bool elf_link(Obj *o, const LinkArgs *a, const char *path);
