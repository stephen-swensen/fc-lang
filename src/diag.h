#pragma once
#include "token.h"
#include <stdarg.h>
#include <stddef.h>

typedef struct SrcLoc {
    const char *filename;
    int line;
    int col;
} SrcLoc;

static inline SrcLoc loc_from_token(const Token *t) {
    return (SrcLoc){ .filename = NULL, .line = t->line, .col = t->col };
}

void diag_set_filename(const char *filename);

/* The current default source filename (used when a SrcLoc carries no filename of
 * its own — e.g. when rendering secondary locations in a multi-line diagnostic). */
const char *diag_filename(void);

/* Report error and increment error count. */
void diag_error(SrcLoc loc, const char *fmt, ...);

/* Report error and abort compilation. */
_Noreturn void diag_fatal(SrcLoc loc, const char *fmt, ...);

/* Report error with no location. */
_Noreturn void diag_fatal_simple(const char *fmt, ...);

int diag_error_count(void);
