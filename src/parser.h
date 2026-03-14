#pragma once
#include "ast.h"
#include "common.h"

typedef struct {
    Token *tokens;
    int token_count;
    int pos;
    Arena *arena;
    InternTable *intern;
    const char *filename;   /* source filename for SrcLoc */
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern);
Program *parse_program(Parser *p);
