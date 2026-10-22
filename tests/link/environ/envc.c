/* environ is copied into the executable; setenv grows libc's __environ, an
 * alias of it, so the executable must export the copy under every name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "envc.h"

extern char **environ;

int env_check(void) {
    setenv("FCC_LINK_TEST_VAR", "yes", 1);
    for (char **e = environ; *e; e++)
        if (strcmp(*e, "FCC_LINK_TEST_VAR=yes") == 0) return 1;
    return 0;
}

/* A library function's address in data: a symbolic dynamic relocation. */
int (*const putter)(const char *) = puts;

int fnptr_check(void) { putter("through a function pointer"); return 4; }
