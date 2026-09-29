#include "diag.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static const char *g_filename = "<stdin>";
static int g_error_count = 0;

/* Server-mode state (see diag.h). NULL means print to stderr and exit(1). */
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

/* Hand the message to the sink, or print it to stderr as
 * "file:line:col: error: msg". A location without a filename gets g_filename. */
static void emit(SrcLoc loc, const char *fmt, va_list ap) {
    const char *fn = loc.filename ? loc.filename : g_filename;
    if (g_sink) {
        /* Sized to the message, so the editor shows the same text as the CLI;
         * the longest messages (deep instantiation chains) need their tail. */
        char *buf = str_vsprintf(fmt, ap);
        SrcLoc resolved = { .filename = fn, .line = loc.line, .col = loc.col };
        g_sink(resolved, buf, g_sink_ud);
        free(buf);
    } else {
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
    /* In server mode, abort this analysis rather than the process. */
    if (g_abort_env) longjmp(*g_abort_env, 1);
    exit(1);
}

_Noreturn void diag_fatal_simple(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *buf = str_vsprintf(fmt, ap);   /* messages embed unbounded paths */
    va_end(ap);
    if (g_sink) {
        /* No location for a simple fatal; report at the start of the file. */
        SrcLoc resolved = { .filename = g_filename, .line = 0, .col = 0 };
        g_sink(resolved, buf, g_sink_ud);
    } else {
        fprintf(stderr, "fcc: error: %s\n", buf);
    }
    free(buf);          /* the sink copies; the longjmp path must not leak */
    g_error_count++;
    if (g_abort_env) longjmp(*g_abort_env, 1);
    exit(1);
}

int diag_error_count(void) {
    return g_error_count;
}
