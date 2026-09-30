#pragma once
#include "token.h"
#include "common.h"

#define MAX_INTERP_DEPTH 8

typedef struct Lexer {
    const char *source;
    const char *current;
    const char *start;      /* start of current token */
    int line;
    int col;
    int start_col;          /* column at start of current token */
    int start_line;         /* line at start of current token. A token's loc is
                               where it begins; l->line has already advanced past
                               a consumed '\n', so a newline token would otherwise
                               pair the next line with its start column */
    InternTable *intern;

    /* String interpolation state */
    int interp_depth;                       /* 0 = not in interpolation */
    int interp_brace[MAX_INTERP_DEPTH];     /* brace depth at each nesting level */
    bool interp_scan_fmt;                   /* next scan_token should emit FMT_SPEC */
    const char *interp_fmt_start;           /* start of format spec text */
    int interp_fmt_len;                     /* length of format spec text */
    int interp_fmt_line;                    /* line of format spec */
    int interp_fmt_col;                     /* col of format spec */

    /* Conditional compilation flags (e.g., --flag debug, --flag os=windows) */
    const struct Flag *flags;
    int flag_count;

    /* Last two significant token kinds emitted (raw scan order). Lets
     * scan_identifier recognize the extern C-name position (`extern NAME`
     * or `extern struct|union NAME`), where a C symbol may contain '__'
     * (the C implementation-reserved namespace, e.g. __errno_location).
     * Everywhere else '__' is a lex error. */
    TokenKind prev_kind;
    TokenKind prev_prev_kind;

    /* Optional abort-cleanup hooks (in-process server / LSP mode). When
     * non-NULL, these caller-owned slots always hold the malloc'd token arrays
     * the lexer has in progress, so an analysis that aborts via longjmp out of
     * a lex fatal (unterminated string, bad indentation, an #if error, ...)
     * can free them instead of leaking. Each phase reads one array and builds
     * another; the output slot is republished after every append, since
     * growth moves the array. Both NULL in the one-shot CLI path. */
    Token **abort_slot_input;   /* the array the current phase reads */
    Token **abort_slot_output;  /* the array the current phase is building */
} Lexer;

/* A conditional compilation flag. value is NULL for bare (valueless) flags. */
typedef struct Flag {
    const char *name;    /* points into the argument text; not NUL-terminated, use name_len */
    int name_len;
    const char *value;   /* NULL if bare; else NUL-terminated, into the argument text */
} Flag;

void lexer_init(Lexer *l, const char *source, InternTable *intern,
                const Flag *flags, int flag_count);

/* Tokenize entire source into an array. Caller must free the array. */
Token *lexer_tokenize(Lexer *l, int *out_count);

/* The keywords the lexer recognizes, for tools that list them (editor
 * completion). */
int lexer_keyword_count(void);
const char *lexer_keyword(int i);
