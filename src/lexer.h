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
} Lexer;

/* A conditional compilation flag. value is NULL for bare (valueless) flags. */
typedef struct Flag {
    const char *name;    /* pointer into argv (NOT null-terminated — use name_len) */
    int name_len;
    const char *value;   /* NULL if bare; else null-terminated pointer into argv */
} Flag;

void lexer_init(Lexer *l, const char *source, InternTable *intern,
                const Flag *flags, int flag_count);

/* Tokenize entire source into an array. Caller must free the array. */
Token *lexer_tokenize(Lexer *l, int *out_count);
