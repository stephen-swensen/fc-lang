/* What gcc puts in an ordinary object: constructors with and without a
 * priority, a destructor, a weak definition, a common symbol (-fcommon), and
 * stdout reached the way -fPIE code reaches a library's variable (a copy
 * relocation). */
#include <stdio.h>
#include "clib.h"

static int order[3], n;
__attribute__((constructor(200))) static void late(void) { order[n++] = 2; }
__attribute__((constructor(101))) static void early(void) { order[n++] = 1; }
__attribute__((constructor)) static void last(void) { order[n++] = 3; }
__attribute__((destructor)) static void bye(void) { printf("destructor\n"); }

int c_add(int a, int b) { return a + b; }
int c_order(void) { return order[0] * 100 + order[1] * 10 + order[2]; }
__attribute__((weak)) int c_weak(void) { return 1; }
int tentative;
int c_common(void) { tentative += 5; return tentative; }
int c_stdout(void) { fputs("stdout from C\n", stdout); return 0; }
