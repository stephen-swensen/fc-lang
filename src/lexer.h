#pragma once
#include "token.h"
#include "common.h"

#define MAX_INTERP_DEPTH 8

typedef struct {
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

    /* Conditional compilation flags (e.g., --flag debug) */
    const char **flags;
    int flag_count;
} Lexer;

void lexer_init(Lexer *l, const char *source, InternTable *intern,
                const char **flags, int flag_count);

/* Tokenize entire source into an array. Caller must free the array. */
Token *lexer_tokenize(Lexer *l, int *out_count);
