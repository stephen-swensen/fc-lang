/* Nothing refers to it, so it stays out of the executable; its undefined
 * symbol does not matter. */
int nowhere_defined(void);
int unused(void) { return nowhere_defined(); }
