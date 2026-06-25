#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *g_filename = "<stdin>";
static int g_error_count = 0;

/* Diagnostic capture state (see diag.h). All NULL/zero by default, which
 * reproduces the original stderr-printing, exit(1) CLI behaviour exactly. */
static DiagSink  g_sink = NULL;
static void     *g_sink_ud = NULL;
static jmp_buf  *g_abort_env = NULL;

void diag_set_filename(const char *filename) {
    g_filename = filename;
}

const char *diag_filename(void) {
    return g_filename;
}

void diag_set_sink(DiagSink sink, void *userdata) {
    g_sink = sink;
    g_sink_ud = userdata;
}

void diag_reset_counts(void) {
    g_error_count = 0;
}

void diag_set_abort_jmp(jmp_buf *env) {
    g_abort_env = env;
}

/* Format the message into a fixed buffer, then either hand it to the sink (with
 * the location's filename defaulted from g_filename) or print it to stderr in
 * the canonical "file:line:col: error: msg" form. Shared by all three reporters
 * so the sink/stderr split lives in one place. */
static void emit(SrcLoc loc, const char *fmt, va_list ap) {
    const char *fn = loc.filename ? loc.filename : g_filename;
    if (g_sink) {
        char buf[2048];
        vsnprintf(buf, sizeof buf, fmt, ap);
        SrcLoc resolved = { .filename = fn, .line = loc.line, .col = loc.col };
        g_sink(resolved, buf, g_sink_ud);
    } else {
        /* CLI path: stream straight to stderr, byte-for-byte as before. */
        fprintf(stderr, "%s:%d:%d: error: ", fn, loc.line, loc.col);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
}

void diag_error(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(loc, fmt, ap);
    va_end(ap);
    g_error_count++;
}

_Noreturn void diag_fatal(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(loc, fmt, ap);
    va_end(ap);
    g_error_count++;
    /* Abort just this analysis (server mode) rather than the process. longjmp
     * never returns, so the _Noreturn contract still holds. */
    if (g_abort_env) longjmp(*g_abort_env, 1);
    exit(1);
}

_Noreturn void diag_fatal_simple(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_sink) {
        /* No location for a simple fatal; report at the start of the file. */
        SrcLoc resolved = { .filename = g_filename, .line = 0, .col = 0 };
        g_sink(resolved, buf, g_sink_ud);
    } else {
        fprintf(stderr, "fcc: error: %s\n", buf);
    }
    g_error_count++;
    if (g_abort_env) longjmp(*g_abort_env, 1);
    exit(1);
}

int diag_error_count(void) {
    return g_error_count;
}
