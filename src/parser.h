#pragma once
#include "ast.h"
#include "common.h"

typedef struct Parser {
    Token *tokens;
    int token_count;
    int pos;
    Arena *arena;
    InternTable *intern;
    const char *filename;   /* source filename for SrcLoc */
    Decl **pending_decls;   /* extra decls from multi-symbol imports */
    int pending_count;
    int pending_cap;
    bool allow_fixed_array; /* true when parsing struct/extern struct field types */
    int expr_start_pos;     /* token index at start of current parse_expr (for postfix ! text capture) */
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern);
Program *parse_program(Parser *p);
