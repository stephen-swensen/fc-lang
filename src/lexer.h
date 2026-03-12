#pragma once
#include "token.h"
#include "common.h"

typedef struct {
    const char *source;
    const char *current;
    const char *start;      /* start of current token */
    int line;
    int col;
    int start_col;          /* column at start of current token */
    InternTable *intern;
} Lexer;

void lexer_init(Lexer *l, const char *source, InternTable *intern);

/* Tokenize entire source into an array. Caller must free the array. */
Token *lexer_tokenize(Lexer *l, int *out_count);
