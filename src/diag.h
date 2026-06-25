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

/* ---- Diagnostic capture (for the in-process LSP server) ----
 *
 * By default diagnostics print to stderr and diag_fatal* exit(1) — the CLI
 * behaviour, unchanged. A long-running server instead wants to (a) collect
 * diagnostics as structured records and (b) survive the lexer/parser's many
 * _Noreturn diag_fatal sites while the user is mid-typing. Both are opt-in and
 * leave the default (sink == NULL, abort env == NULL) byte-for-byte as today. */

/* A sink receives the resolved location (filename already defaulted from
 * diag_filename() when the SrcLoc carried none) and the formatted message text
 * with no "file:line:col: error: " prefix. */
typedef void (*DiagSink)(SrcLoc loc, const char *msg, void *userdata);

/* Install a sink: diag_error/diag_fatal/diag_fatal_simple route their message
 * here instead of stderr. Pass NULL to restore stderr printing. */
void diag_set_sink(DiagSink sink, void *userdata);

/* Zero the error count (call before reusing the diagnostics globals for a fresh
 * analysis). */
void diag_reset_counts(void);

/* Register a recovery point: while set (non-NULL), diag_fatal/diag_fatal_simple
 * longjmp(env, 1) instead of exit(1), aborting one analysis without killing the
 * process. Pass NULL to restore exit(1). */
void diag_set_abort_jmp(jmp_buf *env);
