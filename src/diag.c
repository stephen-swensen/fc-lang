#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *g_filename = "<stdin>";
static int g_error_count = 0;

void diag_set_filename(const char *filename) {
    g_filename = filename;
}

void diag_error(SrcLoc loc, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", g_filename, loc.line, loc.col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    g_error_count++;
}

_Noreturn void diag_fatal(SrcLoc loc, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", g_filename, loc.line, loc.col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

_Noreturn void diag_fatal_simple(const char *fmt, ...) {
    fprintf(stderr, "fc: error: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

int diag_error_count(void) {
    return g_error_count;
}
