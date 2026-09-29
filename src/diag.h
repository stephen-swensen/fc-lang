#pragma once
#include "token.h"
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>

typedef struct SrcLoc {
    const char *filename;
    int line;
    int col;
} SrcLoc;

static inline SrcLoc loc_from_token(const Token *t) {
    return (SrcLoc){ .filename = NULL, .line = t->line, .col = t->col };
}

void diag_set_filename(const char *filename);

/* The filename used for a SrcLoc that carries none. */
const char *diag_filename(void);

/* Report error and increment error count. */
void diag_error(SrcLoc loc, const char *fmt, ...);

/* Report error and abort compilation. */
_Noreturn void diag_fatal(SrcLoc loc, const char *fmt, ...);

/* Report error with no location. */
_Noreturn void diag_fatal_simple(const char *fmt, ...);

int diag_error_count(void);

/* ---- Server mode (fcc --lsp) ----
 *
 * By default diagnostics print to stderr and diag_fatal* calls exit(1). The
 * language server instead collects diagnostics through a sink and turns a fatal
 * into a longjmp, so a lexer error in a half-typed file ends one analysis
 * rather than the process. */

/* A sink receives the resolved location (filename already defaulted from
 * diag_filename() when the SrcLoc carried none) and the formatted message text
 * with no "file:line:col: error: " prefix. */
typedef void (*DiagSink)(SrcLoc loc, const char *msg, void *userdata);

/* Install a sink: diag_error/diag_fatal/diag_fatal_simple route their message
 * here instead of stderr. Pass NULL to restore stderr printing. */
void diag_set_sink(DiagSink sink, void *userdata);

/* Zero the error count before starting a fresh analysis. */
void diag_reset_counts(void);

/* Register a recovery point: while set (non-NULL), diag_fatal/diag_fatal_simple
 * longjmp(env, 1) instead of exit(1), aborting one analysis without killing the
 * process. Pass NULL to restore exit(1). */
void diag_set_abort_jmp(jmp_buf *env);
