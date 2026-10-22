/* Thread-local storage: beyond fcc's linker. */
__thread int counter;
int c_add(int a, int b) { return a + b + counter; }
