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

/* Errors held back by a speculative parse (see diag_speculate_begin). */
typedef struct {
    SrcLoc loc;
    char *msg;
} HeldDiag;
static HeldDiag *g_held = NULL;
static int g_held_count = 0, g_held_cap = 0;
static int g_speculation_depth = 0;

static void drop_held(int from) {
    for (int i = from; i < g_held_count; i++) free(g_held[i].msg);
    g_held_count = from;
}

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
    drop_held(0);
    g_speculation_depth = 0;
}

void diag_set_abort_jmp(jmp_buf *env) {
    g_abort_env = env;
}

/* Hand the message to the sink, or print it to stderr as
 * "file:line:col: error: msg", and count it. A location without a filename
 * gets g_filename. */
static void report(SrcLoc loc, const char *msg) {
    const char *fn = loc.filename ? loc.filename : g_filename;
    if (g_sink) {
        SrcLoc resolved = { .filename = fn, .line = loc.line, .col = loc.col };
        g_sink(resolved, msg, g_sink_ud);
    } else {
        fprintf(stderr, "%s:%d:%d: error: %s\n", fn, loc.line, loc.col, msg);
    }
    g_error_count++;
}

/* Report everything a speculation held, in order. */
static void flush_held(void) {
    for (int i = 0; i < g_held_count; i++) report(g_held[i].loc, g_held[i].msg);
    drop_held(0);
    g_speculation_depth = 0;
}

int diag_speculate_begin(void) {
    g_speculation_depth++;
    return g_held_count;
}

void diag_speculate_end(int mark, bool keep) {
    if (!keep) drop_held(mark);
    if (--g_speculation_depth == 0) flush_held();
}

void diag_error(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Sized to the message, so the editor shows the same text as the CLI; the
     * longest messages (deep instantiation chains) need their tail. */
    char *msg = str_vsprintf(fmt, ap);
    va_end(ap);
    if (g_speculation_depth > 0) {
        DA_APPEND(g_held, g_held_count, g_held_cap, ((HeldDiag){ loc, msg }));
        return;
    }
    report(loc, msg);
    free(msg);
}

_Noreturn void diag_fatal(SrcLoc loc, const char *fmt, ...) {
    flush_held();
    va_list ap;
    va_start(ap, fmt);
    char *msg = str_vsprintf(fmt, ap);
    va_end(ap);
    report(loc, msg);
    free(msg);          /* the sink copies; the longjmp path must not leak */
    /* In server mode, abort this analysis rather than the process. */
    if (g_abort_env) longjmp(*g_abort_env, 1);
    exit(1);
}

_Noreturn void diag_fatal_simple(const char *fmt, ...) {
    flush_held();
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
