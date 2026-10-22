/* Pulls chain_second.o out of the archive in turn. */
#include "chain.h"
int chain_second(int x);
int chain_first(int x) { return chain_second(x) + 1; }
