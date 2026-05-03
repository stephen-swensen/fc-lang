#include "version.h"
#include "platform.h"
#include "fcc_version.h"
#include <stdio.h>

void print_version(void) {
    printf("fcc %s (%s %s%s)\n",
           FCC_VERSION_BASE, FCC_GIT_HASH, FCC_GIT_DATE, FCC_GIT_DIRTY);

    const char *os   = platform_get_os();
    const char *arch = platform_get_arch();
    const char *env  = platform_get_env();
    printf("Target:");
    if (os)   printf(" %s", os);
    if (arch) printf(" %s", arch);
    if (env)  printf(" %s", env);
    printf("\n");

    printf("Built: %s with %s (%s)\n",
           FCC_BUILD_DATE, FCC_BUILD_OPT, FCC_BUILD_CC);
}
