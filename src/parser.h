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
    Decl **pending_decls;   /* extra decls from multi-symbol imports */
    int pending_count;
    int pending_cap;
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern);
Program *parse_program(Parser *p);
