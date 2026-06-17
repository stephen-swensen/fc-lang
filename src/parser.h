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
    bool block_arm_arrow;   /* true while parsing a match-arm `when` guard: stop expr at top-level `->`; cleared inside bracketed sub-expressions so pointer-field `p->x` still works when parenthesized */
    int expr_start_pos;     /* token index at start of current parse_expr (for postfix ! text capture) */
    bool half_gt;           /* a '>>' (TOK_GTGT) token has had its first '>' consumed as a type-argument closer; the parser is parked on it awaiting the second */
    int half_gt_pos;        /* token index of that '>>' (valid iff half_gt) */
} Parser;

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern);
Program *parse_program(Parser *p);
