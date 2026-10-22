/* Overrides clib.c's weak c_weak, and defines its common symbol again. */
int c_weak(void) { return 2; }
int tentative;
