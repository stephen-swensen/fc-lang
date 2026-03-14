#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern) {
    p->tokens = tokens;
    p->token_count = count;
    p->pos = 0;
    p->arena = arena;
    p->intern = intern;
    p->filename = NULL;
}

/* ---- Token access ---- */

static Token *current(Parser *p) {
    return &p->tokens[p->pos];
}

static Token *peek_at(Parser *p, int offset) {
    int idx = p->pos + offset;
    if (idx >= p->token_count) return &p->tokens[p->token_count - 1];
    return &p->tokens[idx];
}

static bool check(Parser *p, TokenKind kind) {
    return current(p)->kind == kind;
}

static bool at_end_p(Parser *p) {
    return current(p)->kind == TOK_EOF;
}

static Token *advance_p(Parser *p) {
    Token *t = current(p);
    if (!at_end_p(p)) p->pos++;
    return t;
}

static Token *expect(Parser *p, TokenKind kind) {
    if (current(p)->kind != kind) {
        SrcLoc loc = loc_from_token(current(p));
        diag_fatal(loc, "expected %s, got %s",
            token_kind_name(kind), token_kind_name(current(p)->kind));
    }
    return advance_p(p);
}

static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) advance_p(p);
}

static const char *tok_intern(Parser *p, Token *t) {
    return intern(p->intern, t->start, t->length);
}

/* ---- Pratt precedence ---- */

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGN,        /* = */
    PREC_LOR,           /* || */
    PREC_LAND,          /* && */
    PREC_CMP,           /* == != < > <= >= */
    PREC_BOR,           /* | */
    PREC_BXOR,          /* ^ */
    PREC_BAND,          /* & */
    PREC_SHIFT,         /* << >> */
    PREC_ADD,           /* + - */
    PREC_MUL,           /* * / % */
    PREC_PREFIX,        /* -x !x ~x &x *x */
    PREC_POSTFIX,       /* x! x[i] x.f x->f x() */
} Prec;

static Prec infix_prec(TokenKind kind) {
    switch (kind) {
    case TOK_EQ:        return PREC_ASSIGN;
    case TOK_PIPEPIPE:  return PREC_LOR;
    case TOK_AMPAMP:    return PREC_LAND;
    case TOK_EQEQ: case TOK_BANGEQ:
    case TOK_LT: case TOK_GT:
    case TOK_LTEQ: case TOK_GTEQ:
                        return PREC_CMP;
    case TOK_PIPE:      return PREC_BOR;
    case TOK_CARET:     return PREC_BXOR;
    case TOK_AMP:       return PREC_BAND;
    case TOK_LTLT: case TOK_GTGT:
                        return PREC_SHIFT;
    case TOK_PLUS: case TOK_MINUS:
                        return PREC_ADD;
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
                        return PREC_MUL;
    case TOK_BANG:      return PREC_POSTFIX;
    case TOK_LBRACKET:  return PREC_POSTFIX;
    case TOK_DOT:       return PREC_POSTFIX;
    case TOK_ARROW:     return PREC_POSTFIX;
    case TOK_LPAREN:    return PREC_POSTFIX;
    default:            return PREC_NONE;
    }
}

/* ---- Allocators ---- */

static Expr *alloc_expr(Parser *p, ExprKind kind, SrcLoc loc) {
    Expr *e = arena_alloc(p->arena, sizeof(Expr));
    e->kind = kind;
    loc.filename = p->filename;
    e->loc = loc;
    return e;
}

/* Copy a temp array into the arena */
static Expr **arena_copy_exprs(Parser *p, Expr **arr, int count) {
    if (count == 0) return NULL;
    Expr **a = arena_alloc(p->arena, sizeof(Expr*) * (size_t)count);
    memcpy(a, arr, sizeof(Expr*) * (size_t)count);
    return a;
}

/* ---- Type parsing ---- */

static Type *parse_type(Parser *p);

static bool is_type_name(const char *s, int len) {
    struct { const char *n; int l; } names[] = {
        {"int8",2+2}, {"int16",2+3}, {"int32",2+3}, {"int64",2+3},
        {"uint8",3+1+1}, {"uint16",3+2+1}, {"uint32",3+2+1}, {"uint64",3+2+1},
        {"float32",5+2}, {"float64",5+2},
        {"bool",4}, {"char",4},
        {"str",3}, {"str32",5}, {"cstr",4},
        {"any",3},
    };
    /* Actually compute lengths correctly */
    static const struct { const char *n; int l; } types[] = {
        {"int8",4}, {"int16",5}, {"int32",5}, {"int64",5},
        {"uint8",5}, {"uint16",6}, {"uint32",6}, {"uint64",6},
        {"float32",7}, {"float64",7},
        {"bool",4}, {"char",4},
        {"str",3}, {"str32",5}, {"cstr",4},
        {"any",3},
    };
    (void)names;
    for (int i = 0; i < (int)(sizeof(types)/sizeof(types[0])); i++) {
        if (types[i].l == len && memcmp(s, types[i].n, (size_t)len) == 0) return true;
    }
    return false;
}

static Type *type_from_name(const char *s, int len) {
    struct { const char *n; int l; Type *(*fn)(void); } map[] = {
        {"int8",4, type_int8},     {"int16",5, type_int16},
        {"int32",5, type_int32},   {"int64",5, type_int64},
        {"uint8",5, type_uint8},   {"uint16",6, type_uint16},
        {"uint32",6, type_uint32}, {"uint64",6, type_uint64},
        {"float32",7, type_float32}, {"float64",7, type_float64},
        {"bool",4, type_bool},     {"char",4, type_char},
        {"str",3, type_str},       {"str32",3, NULL},
        {"cstr",4, type_cstr},     {"any",3, type_any_ptr},
    };
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++) {
        if (map[i].l == len && memcmp(s, map[i].n, (size_t)len) == 0) {
            return map[i].fn ? map[i].fn() : NULL;
        }
    }
    return NULL;
}

static Type *parse_type_suffix(Parser *p, Type *base) {
    /* T*, T[], T? — left to right */
    for (;;) {
        if (check(p, TOK_STAR)) {
            advance_p(p);
            base = type_pointer(p->arena, base);
            continue;
        }
        if (check(p, TOK_LBRACKET) && peek_at(p, 1)->kind == TOK_RBRACKET) {
            advance_p(p); advance_p(p);
            base = type_slice(p->arena, base);
            continue;
        }
        if (check(p, TOK_QUESTION)) {
            advance_p(p);
            base = type_option(p->arena, base);
            continue;
        }
        break;
    }
    return base;
}

static Type *parse_type(Parser *p) {
    Token *t = current(p);

    if (t->kind == TOK_VOID) {
        advance_p(p);
        return type_void();
    }

    if (t->kind == TOK_TYPE_VAR) {
        advance_p(p);
        Type *tv = arena_alloc(p->arena, sizeof(Type));
        tv->kind = TYPE_TYPE_VAR;
        tv->type_var.name = tok_intern(p, t);
        return parse_type_suffix(p, tv);
    }

    if (t->kind == TOK_IDENT) {
        Type *base = type_from_name(t->start, t->length);
        if (base) {
            advance_p(p);
            /* Special case: 'any' must be followed by '*' */
            if (base == type_any_ptr()) {
                /* already any*, no need for * suffix unless we want any** etc */
                return parse_type_suffix(p, base);
            }
            return parse_type_suffix(p, base);
        }
        /* User-defined type name (struct/union) — create a stub with just the name */
        advance_p(p);
        Type *udt = arena_alloc(p->arena, sizeof(Type));
        udt->kind = TYPE_STRUCT;  /* will be resolved to correct kind in pass2 */
        udt->struc.name = tok_intern(p, t);
        udt->struc.fields = NULL;
        udt->struc.field_count = 0;
        return parse_type_suffix(p, udt);
    }

    /* Function type: (T1, T2) -> T */
    if (t->kind == TOK_LPAREN) {
        advance_p(p);
        Type **params = NULL;
        int pcount = 0, pcap = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                Type *pt = parse_type(p);
                DA_APPEND(params, pcount, pcap, pt);
                if (!check(p, TOK_COMMA)) break;
                advance_p(p);
            } while (1);
        }
        expect(p, TOK_RPAREN);
        expect(p, TOK_ARROW);
        Type *ret = parse_type(p);

        Type *ft = arena_alloc(p->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_count = pcount;
        ft->func.return_type = ret;
        if (pcount > 0) {
            ft->func.param_types = arena_alloc(p->arena, sizeof(Type*) * (size_t)pcount);
            memcpy(ft->func.param_types, params, sizeof(Type*) * (size_t)pcount);
            free(params);
        }
        return parse_type_suffix(p, ft);
    }

    SrcLoc loc = loc_from_token(t);
    diag_fatal(loc, "expected type, got %s", token_kind_name(t->kind));
}

/* ---- Expression parsing ---- */

static Expr *parse_expr(Parser *p, Prec min_prec);
static Expr *parse_block_item(Parser *p);
static Expr *parse_match_expr(Parser *p);
static Expr *parse_struct_literal(Parser *p, const char *type_name, SrcLoc loc);

static int64_t parse_int_value(const char *start, int length) {
    char buf[64];
    int num_len = 0;
    for (int i = 0; i < length && i < 63; i++) {
        if (start[i] >= '0' && start[i] <= '9') buf[num_len++] = start[i];
        else break;
    }
    buf[num_len] = '\0';
    errno = 0;
    return (int64_t)strtoll(buf, NULL, 10);
}

static Type *parse_int_type(const char *start, int length) {
    int num_end = 0;
    while (num_end < length && start[num_end] >= '0' && start[num_end] <= '9') num_end++;
    if (num_end >= length) return type_int32();
    Type *t = type_from_int_suffix(start + num_end, length - num_end);
    return t ? t : type_int32();
}

/* Parse a block: INDENT item (NEWLINE item)* DEDENT.
   Returns array of Expr*, sets *count. */
static Expr **parse_block(Parser *p, int *count) {
    Expr **stmts = NULL;
    int len = 0, cap = 0;

    expect(p, TOK_INDENT);
    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;
        Expr *e = parse_block_item(p);
        DA_APPEND(stmts, len, cap, e);
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    *count = len;
    Expr **arena_stmts = arena_copy_exprs(p, stmts, len);
    free(stmts);
    return arena_stmts;
}

/* Parse either a block (if INDENT) or a single statement/expression */
static Expr **parse_body(Parser *p, int *count) {
    if (check(p, TOK_INDENT)) {
        return parse_block(p, count);
    }
    /* Single statement/expression (may be break, continue, return, let, or expr) */
    *count = 1;
    Expr **stmts = arena_alloc(p->arena, sizeof(Expr*));
    stmts[0] = parse_block_item(p);
    return stmts;
}

/* Parse a function literal: (params) -> body */
static Expr *parse_func_literal(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    expect(p, TOK_LPAREN);

    Param *params = NULL;
    int pcount = 0, pcap = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            SrcLoc ploc = loc_from_token(current(p));
            const char *name = tok_intern(p, expect(p, TOK_IDENT));
            expect(p, TOK_COLON);
            Type *type = parse_type(p);
            Param param = { .name = name, .type = type, .loc = ploc };
            DA_APPEND(params, pcount, pcap, param);
            if (!check(p, TOK_COMMA)) break;
            advance_p(p);
        } while (1);
    }
    expect(p, TOK_RPAREN);
    expect(p, TOK_ARROW);

    int body_count;
    Expr **body = parse_body(p, &body_count);

    Expr *e = alloc_expr(p, EXPR_FUNC, loc);
    if (pcount > 0) {
        e->func.params = arena_alloc(p->arena, sizeof(Param) * (size_t)pcount);
        memcpy(e->func.params, params, sizeof(Param) * (size_t)pcount);
        free(params);
    }
    e->func.param_count = pcount;
    e->func.body = body;
    e->func.body_count = body_count;
    return e;
}

/* Parse if/then/else expression */
static Expr *parse_if_expr(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    expect(p, TOK_IF);
    Expr *cond = parse_expr(p, PREC_NONE + 1);
    expect(p, TOK_THEN);

    int then_count;
    Expr **then_body = parse_body(p, &then_count);

    /* Wrap multi-expr then body in a block node */
    Expr *then_expr;
    if (then_count == 1) {
        then_expr = then_body[0];
    } else {
        then_expr = alloc_expr(p, EXPR_BLOCK, loc);
        then_expr->block.stmts = then_body;
        then_expr->block.count = then_count;
    }

    skip_newlines(p);

    Expr *else_expr = NULL;
    if (check(p, TOK_ELSE)) {
        advance_p(p);
        if (check(p, TOK_IF)) {
            /* else if chain */
            else_expr = parse_if_expr(p);
        } else {
            int else_count;
            Expr **else_body = parse_body(p, &else_count);
            if (else_count == 1) {
                else_expr = else_body[0];
            } else {
                else_expr = alloc_expr(p, EXPR_BLOCK, loc);
                else_expr->block.stmts = else_body;
                else_expr->block.count = else_count;
            }
        }
    }

    Expr *e = alloc_expr(p, EXPR_IF, loc);
    e->if_expr.cond = cond;
    e->if_expr.then_body = then_expr;
    e->if_expr.else_body = else_expr;
    return e;
}

/* Parse a single item in a block: let binding or expression */
static Expr *parse_block_item(Parser *p) {
    if (check(p, TOK_LET)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        bool is_mut = false;
        if (check(p, TOK_MUT)) {
            advance_p(p);
            is_mut = true;
        }
        const char *name = tok_intern(p, expect(p, TOK_IDENT));
        expect(p, TOK_EQ);

        Expr *init;
        if (check(p, TOK_INDENT)) {
            int count;
            Expr **body = parse_block(p, &count);
            if (count == 1) {
                init = body[0];
            } else {
                init = alloc_expr(p, EXPR_BLOCK, loc);
                init->block.stmts = body;
                init->block.count = count;
            }
        } else {
            init = parse_expr(p, PREC_NONE + 1);
        }

        Expr *e = alloc_expr(p, EXPR_LET, loc);
        e->let_expr.let_name = name;
        e->let_expr.let_is_mut = is_mut;
        e->let_expr.let_init = init;
        return e;
    }

    if (check(p, TOK_RETURN)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        Expr *value = NULL;
        /* return has a value if not followed by NEWLINE/DEDENT/EOF */
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_DEDENT) && !at_end_p(p)) {
            value = parse_expr(p, PREC_NONE + 1);
        }
        Expr *e = alloc_expr(p, EXPR_RETURN, loc);
        e->return_expr.value = value;
        return e;
    }

    if (check(p, TOK_BREAK)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        Expr *value = NULL;
        if (!check(p, TOK_NEWLINE) && !check(p, TOK_DEDENT) && !at_end_p(p)) {
            value = parse_expr(p, PREC_NONE + 1);
        }
        Expr *e = alloc_expr(p, EXPR_BREAK, loc);
        e->break_expr.value = value;
        return e;
    }

    if (check(p, TOK_CONTINUE)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_CONTINUE, loc);
        return e;
    }

    return parse_expr(p, PREC_NONE + 1);
}

/* ---- Prefix parsing ---- */

static Expr *parse_prefix(Parser *p) {
    Token *t = current(p);
    SrcLoc loc = loc_from_token(t);

    switch (t->kind) {
    case TOK_INT_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_INT_LIT, loc);
        e->int_lit.value = parse_int_value(t->start, t->length);
        e->int_lit.lit_type = parse_int_type(t->start, t->length);
        return e;
    }

    case TOK_FLOAT_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_FLOAT_LIT, loc);
        char buf[64];
        int num_len = 0;
        for (int i = 0; i < t->length && i < 63; i++) {
            if ((t->start[i] >= '0' && t->start[i] <= '9') || t->start[i] == '.') buf[num_len++] = t->start[i];
            else break;
        }
        buf[num_len] = '\0';
        e->float_lit.value = strtod(buf, NULL);
        if (num_len < t->length && t->start[num_len] == 'f'
            && t->length - num_len >= 3 && t->start[num_len+1] == '3' && t->start[num_len+2] == '2')
            e->float_lit.lit_type = type_float32();
        else
            e->float_lit.lit_type = type_float64();
        return e;
    }

    case TOK_STRING_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_STRING_LIT, loc);
        /* Skip opening and closing quotes */
        e->string_lit.value = t->start + 1;
        e->string_lit.length = t->length - 2;
        return e;
    }

    case TOK_CSTRING_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_CSTRING_LIT, loc);
        /* Skip c" and closing " */
        e->cstring_lit.value = t->start + 2;
        e->cstring_lit.length = t->length - 3;
        return e;
    }

    case TOK_TRUE: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_BOOL_LIT, loc);
        e->bool_lit.value = true;
        return e;
    }

    case TOK_FALSE: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_BOOL_LIT, loc);
        e->bool_lit.value = false;
        return e;
    }

    case TOK_IDENT: {
        const char *name = tok_intern(p, t);

        /* Check for array literal: type_name[size] { ... } */
        if (is_type_name(t->start, t->length) && peek_at(p, 1)->kind == TOK_LBRACKET) {
            /* Look ahead: type[expr] { — the { after ] means array literal */
            int save = p->pos;
            advance_p(p);  /* consume ident */
            advance_p(p);  /* consume [ */
            /* Skip tokens until we find matching ] */
            int depth = 1;
            while (depth > 0 && !at_end_p(p)) {
                if (check(p, TOK_LBRACKET)) depth++;
                if (check(p, TOK_RBRACKET)) depth--;
                advance_p(p);
            }
            bool is_array_lit = check(p, TOK_LBRACE);
            /* Restore position */
            p->pos = save;

            if (is_array_lit) {
                advance_p(p);  /* consume ident */
                Type *elem_type = type_from_name(t->start, t->length);
                if (!elem_type) {
                    /* User-defined type — create stub */
                    elem_type = arena_alloc(p->arena, sizeof(Type));
                    elem_type->kind = TYPE_STRUCT;
                    elem_type->struc.name = name;
                    elem_type->struc.fields = NULL;
                    elem_type->struc.field_count = 0;
                }
                expect(p, TOK_LBRACKET);
                Expr *size_expr = parse_expr(p, PREC_NONE + 1);
                expect(p, TOK_RBRACKET);
                expect(p, TOK_LBRACE);

                Expr **elems = NULL;
                int elem_count = 0, elem_cap = 0;
                if (!check(p, TOK_RBRACE)) {
                    do {
                        Expr *elem = parse_expr(p, PREC_NONE + 1);
                        DA_APPEND(elems, elem_count, elem_cap, elem);
                        if (!check(p, TOK_COMMA)) break;
                        advance_p(p);
                    } while (!check(p, TOK_RBRACE));
                }
                expect(p, TOK_RBRACE);

                Expr *e = alloc_expr(p, EXPR_ARRAY_LIT, loc);
                e->array_lit.elem_type = elem_type;
                e->array_lit.size_expr = size_expr;
                e->array_lit.elems = arena_copy_exprs(p, elems, elem_count);
                e->array_lit.elem_count = elem_count;
                free(elems);
                return e;
            }
        }

        /* Check for struct literal: name { field = expr, ... }
         * Disambiguate from block: peek past { for IDENT = or } */
        if (peek_at(p, 1)->kind == TOK_LBRACE) {
            Token *after_brace = peek_at(p, 2);
            bool is_struct_lit = false;
            /* Empty struct: name { } */
            if (after_brace->kind == TOK_RBRACE) {
                is_struct_lit = true;
            }
            /* Field init: name { ident = ... } */
            if (after_brace->kind == TOK_IDENT && peek_at(p, 3)->kind == TOK_EQ) {
                is_struct_lit = true;
            }
            if (is_struct_lit) {
                advance_p(p);  /* consume ident */
                advance_p(p);  /* consume { */
                return parse_struct_literal(p, name, loc);
            }
        }
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_IDENT, loc);
        e->ident.name = name;
        return e;
    }

    case TOK_LPAREN: {
        /* Disambiguate: function literal vs parenthesized expression */
        /* () -> ... : empty-param function */
        if (peek_at(p, 1)->kind == TOK_RPAREN && peek_at(p, 2)->kind == TOK_ARROW) {
            return parse_func_literal(p);
        }
        /* (ident : ...) -> ... : function with params */
        if (peek_at(p, 1)->kind == TOK_IDENT && peek_at(p, 2)->kind == TOK_COLON) {
            return parse_func_literal(p);
        }
        /* (Type)expr : cast — check if next token is a type name followed by ) */
        if (peek_at(p, 1)->kind == TOK_IDENT &&
            is_type_name(peek_at(p, 1)->start, peek_at(p, 1)->length) &&
            peek_at(p, 2)->kind == TOK_RPAREN) {
            /* Cast expression */
            advance_p(p); /* ( */
            Type *target = parse_type(p);
            expect(p, TOK_RPAREN);
            Expr *operand = parse_expr(p, PREC_PREFIX);
            Expr *e = alloc_expr(p, EXPR_CAST, loc);
            e->cast.target = target;
            e->cast.operand = operand;
            return e;
        }
        /* Parenthesized expression */
        advance_p(p);
        Expr *e = parse_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        return e;
    }

    case TOK_IF:
        return parse_if_expr(p);

    case TOK_MATCH:
        return parse_match_expr(p);

    case TOK_SOME: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Expr *val = parse_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_SOME, loc);
        e->some_expr.value = val;
        return e;
    }

    /* TOK_NONE removed: bare 'none' is not valid in expression context.
     * Use T.none (e.g. int32.none) instead — handled in DOT infix. */

    case TOK_LOOP: {
        advance_p(p);
        int body_count;
        Expr **body = parse_block(p, &body_count);
        Expr *e = alloc_expr(p, EXPR_LOOP, loc);
        e->loop_expr.body = body;
        e->loop_expr.body_count = body_count;
        return e;
    }

    case TOK_FOR: {
        advance_p(p);
        const char *var = tok_intern(p, expect(p, TOK_IDENT));
        const char *index_var = NULL;
        if (check(p, TOK_COMMA)) {
            advance_p(p);
            /* first was index, second is element */
            index_var = var;
            var = tok_intern(p, expect(p, TOK_IDENT));
        }
        expect(p, TOK_IN);
        Expr *iter = parse_expr(p, PREC_NONE + 1);
        Expr *range_end = NULL;
        if (check(p, TOK_DOTDOT)) {
            advance_p(p);
            range_end = parse_expr(p, PREC_NONE + 1);
        }
        int body_count;
        Expr **body = parse_body(p, &body_count);
        Expr *e = alloc_expr(p, EXPR_FOR, loc);
        e->for_expr.var = var;
        e->for_expr.index_var = index_var;
        e->for_expr.iter = iter;
        e->for_expr.range_end = range_end;
        e->for_expr.body = body;
        e->for_expr.body_count = body_count;
        return e;
    }

    case TOK_SIZEOF: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Type *ty = parse_type(p);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_SIZEOF, loc);
        e->sizeof_expr.target = ty;
        return e;
    }

    case TOK_DEFAULT: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Type *ty = parse_type(p);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_DEFAULT, loc);
        e->default_expr.target = ty;
        return e;
    }

    case TOK_FREE: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Expr *operand = parse_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_FREE, loc);
        e->free_expr.operand = operand;
        return e;
    }

    case TOK_ALLOC: {
        advance_p(p);
        expect(p, TOK_LPAREN);

        /* Try to parse as type-based alloc: alloc(T) or alloc(T[N])
         * Save position so we can backtrack if it's actually alloc(expr) */
        int save = p->pos;
        Token *first = current(p);
        bool could_be_type = (first->kind == TOK_IDENT || first->kind == TOK_VOID ||
                              first->kind == TOK_LPAREN || first->kind == TOK_TYPE_VAR);
        if (could_be_type) {
            Type *ty = parse_type(p);
            if (check(p, TOK_RPAREN)) {
                /* alloc(T) — bare type alloc */
                advance_p(p);
                Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                e->alloc_expr.alloc_type = ty;
                e->alloc_expr.size_expr = NULL;
                e->alloc_expr.init_expr = NULL;
                return e;
            }
            if (check(p, TOK_LBRACKET)) {
                /* alloc(T[N]) — array alloc */
                advance_p(p);
                Expr *size = parse_expr(p, PREC_NONE + 1);
                expect(p, TOK_RBRACKET);
                expect(p, TOK_RPAREN);
                Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                e->alloc_expr.alloc_type = ty;
                e->alloc_expr.size_expr = size;
                e->alloc_expr.init_expr = NULL;
                return e;
            }
            /* Not type-only — backtrack and parse as expression */
            p->pos = save;
        }

        /* alloc(expr) — initialized alloc */
        Expr *init = parse_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
        e->alloc_expr.alloc_type = NULL;
        e->alloc_expr.size_expr = NULL;
        e->alloc_expr.init_expr = init;
        return e;
    }

    /* Prefix unary operators: - ! ~ & * */
    case TOK_MINUS:
    case TOK_BANG:
    case TOK_TILDE:
    case TOK_AMP:
    case TOK_STAR: {
        TokenKind op = t->kind;
        advance_p(p);
        Expr *operand = parse_expr(p, PREC_PREFIX);
        Expr *e = alloc_expr(p, EXPR_UNARY_PREFIX, loc);
        e->unary_prefix.op = op;
        e->unary_prefix.operand = operand;
        return e;
    }

    default:
        diag_fatal(loc, "unexpected token %s in expression",
            token_kind_name(t->kind));
    }
}

/* ---- Infix parsing ---- */

static Expr *parse_infix(Parser *p, Expr *left, Token *op_tok) {
    SrcLoc loc = loc_from_token(op_tok);
    TokenKind op = op_tok->kind;

    switch (op) {
    case TOK_BANG: {
        Expr *e = alloc_expr(p, EXPR_UNARY_POSTFIX, loc);
        e->unary_postfix.op = TOK_BANG;
        e->unary_postfix.operand = left;
        return e;
    }

    case TOK_DOT: {
        /* T.none: qualified none expression */
        if (check(p, TOK_NONE)) {
            advance_p(p); /* consume 'none' */
            /* left must be an identifier (the type name) */
            if (left->kind != EXPR_IDENT)
                diag_fatal(loc, "qualified none requires a type name before '.'");
            const char *tname = left->ident.name;
            /* Look up as built-in type first */
            Type *inner = type_from_name(tname, (int)strlen(tname));
            if (!inner) {
                /* User-defined type — create a stub */
                inner = arena_alloc(p->arena, sizeof(Type));
                inner->kind = TYPE_STRUCT;
                inner->struc.name = tname;
                inner->struc.fields = NULL;
                inner->struc.field_count = 0;
            }
            Type *opt = type_option(p->arena, inner);
            Expr *e = alloc_expr(p, EXPR_NONE, loc);
            e->none_expr.target = opt;
            return e;
        }
        Token *field = expect(p, TOK_IDENT);
        Expr *e = alloc_expr(p, EXPR_FIELD, loc);
        e->field.object = left;
        e->field.name = tok_intern(p, field);
        return e;
    }

    case TOK_ARROW: {
        Token *field = expect(p, TOK_IDENT);
        Expr *e = alloc_expr(p, EXPR_DEREF_FIELD, loc);
        e->field.object = left;
        e->field.name = tok_intern(p, field);
        return e;
    }

    case TOK_LBRACKET: {
        if (check(p, TOK_DOTDOT)) {
            /* s[..hi] */
            advance_p(p);
            Expr *hi = NULL;
            if (!check(p, TOK_RBRACKET)) hi = parse_expr(p, PREC_NONE + 1);
            expect(p, TOK_RBRACKET);
            Expr *e = alloc_expr(p, EXPR_SLICE, loc);
            e->slice.object = left;
            e->slice.lo = NULL;
            e->slice.hi = hi;
            return e;
        }
        Expr *index = parse_expr(p, PREC_NONE + 1);
        if (check(p, TOK_DOTDOT)) {
            advance_p(p);
            Expr *hi = NULL;
            if (!check(p, TOK_RBRACKET)) hi = parse_expr(p, PREC_NONE + 1);
            expect(p, TOK_RBRACKET);
            Expr *e = alloc_expr(p, EXPR_SLICE, loc);
            e->slice.object = left;
            e->slice.lo = index;
            e->slice.hi = hi;
            return e;
        }
        expect(p, TOK_RBRACKET);
        Expr *e = alloc_expr(p, EXPR_INDEX, loc);
        e->index.object = left;
        e->index.index = index;
        return e;
    }

    case TOK_LPAREN: {
        Expr **args = NULL;
        int arg_count = 0, arg_cap = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                Expr *arg = parse_expr(p, PREC_NONE + 1);
                DA_APPEND(args, arg_count, arg_cap, arg);
                if (!check(p, TOK_COMMA)) break;
                advance_p(p);
            } while (1);
        }
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_CALL, loc);
        e->call.func = left;
        e->call.args = arena_copy_exprs(p, args, arg_count);
        e->call.arg_count = arg_count;
        e->call.type_args = NULL;
        e->call.type_arg_count = 0;
        free(args);
        return e;
    }

    case TOK_EQ: {
        Expr *value = parse_expr(p, PREC_ASSIGN);
        Expr *e = alloc_expr(p, EXPR_ASSIGN, loc);
        e->assign.target = left;
        e->assign.value = value;
        return e;
    }

    default: {
        Prec prec = infix_prec(op);
        Expr *right = parse_expr(p, prec + 1);
        Expr *e = alloc_expr(p, EXPR_BINARY, loc);
        e->binary.op = op;
        e->binary.left = left;
        e->binary.right = right;
        return e;
    }
    }
}

static Expr *parse_expr(Parser *p, Prec min_prec) {
    Expr *left = parse_prefix(p);
    for (;;) {
        Token *t = current(p);
        Prec prec = infix_prec(t->kind);
        if (prec == PREC_NONE || prec < min_prec) break;
        Token *op_tok = advance_p(p);
        left = parse_infix(p, left, op_tok);
    }
    return left;
}

/* ---- Pattern parsing ---- */

static Pattern *parse_pattern(Parser *p) {
    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->loc = loc_from_token(current(p));

    /* Negative integer pattern: -42 */
    if (check(p, TOK_MINUS) && peek_at(p, 1)->kind == TOK_INT_LIT) {
        advance_p(p); /* consume - */
        Token *t = advance_p(p);
        pat->kind = PAT_INT_LIT;
        pat->int_lit.value = -parse_int_value(t->start, t->length);
        pat->int_lit.lit_type = parse_int_type(t->start, t->length);
        return pat;
    }

    if (check(p, TOK_INT_LIT)) {
        Token *t = advance_p(p);
        pat->kind = PAT_INT_LIT;
        pat->int_lit.value = parse_int_value(t->start, t->length);
        pat->int_lit.lit_type = parse_int_type(t->start, t->length);
        return pat;
    }

    if (check(p, TOK_TRUE)) {
        advance_p(p);
        pat->kind = PAT_BOOL_LIT;
        pat->bool_lit.value = true;
        return pat;
    }

    if (check(p, TOK_FALSE)) {
        advance_p(p);
        pat->kind = PAT_BOOL_LIT;
        pat->bool_lit.value = false;
        return pat;
    }

    if (check(p, TOK_NONE)) {
        advance_p(p);
        pat->kind = PAT_NONE;
        return pat;
    }

    if (check(p, TOK_SOME)) {
        advance_p(p);
        expect(p, TOK_LPAREN);
        pat->kind = PAT_SOME;
        pat->some_pat.inner = parse_pattern(p);
        expect(p, TOK_RPAREN);
        return pat;
    }

    if (check(p, TOK_IDENT)) {
        Token *t = current(p);
        const char *name = tok_intern(p, t);

        /* Check if it's _ (wildcard) */
        if (t->length == 1 && t->start[0] == '_') {
            advance_p(p);
            pat->kind = PAT_WILDCARD;
            return pat;
        }

        /* Check if it's a variant: name(pattern) */
        if (peek_at(p, 1)->kind == TOK_LPAREN) {
            advance_p(p);  /* consume ident */
            advance_p(p);  /* consume ( */
            pat->kind = PAT_VARIANT;
            pat->variant.variant = name;
            pat->variant.payload = parse_pattern(p);
            expect(p, TOK_RPAREN);
            return pat;
        }

        /* Just a binding name */
        advance_p(p);
        pat->kind = PAT_BINDING;
        pat->binding.name = name;
        return pat;
    }

    SrcLoc loc = loc_from_token(current(p));
    diag_fatal(loc, "expected pattern, got %s", token_kind_name(current(p)->kind));
}

/* ---- Match expression parsing ---- */

static Expr *parse_match_expr(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    expect(p, TOK_MATCH);

    Expr *subject = parse_expr(p, PREC_NONE + 1);
    expect(p, TOK_WITH);

    /* Expect INDENT then a series of | pattern -> body */
    expect(p, TOK_INDENT);

    MatchArm *arms = NULL;
    int arm_count = 0, arm_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        expect(p, TOK_PIPE);
        MatchArm arm;
        arm.loc = loc_from_token(current(p));
        arm.pattern = parse_pattern(p);
        expect(p, TOK_ARROW);

        /* Parse arm body */
        arm.body = parse_body(p, &arm.body_count);

        DA_APPEND(arms, arm_count, arm_cap, arm);
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Expr *e = alloc_expr(p, EXPR_MATCH, loc);
    e->match_expr.subject = subject;
    e->match_expr.arm_count = arm_count;
    if (arm_count > 0) {
        e->match_expr.arms = arena_alloc(p->arena, sizeof(MatchArm) * (size_t)arm_count);
        memcpy(e->match_expr.arms, arms, sizeof(MatchArm) * (size_t)arm_count);
        free(arms);
    }
    return e;
}

/* ---- Struct literal parsing ---- */

static Expr *parse_struct_literal(Parser *p, const char *type_name, SrcLoc loc) {
    /* We already consumed IDENT and LBRACE */
    FieldInit *fields = NULL;
    int field_count = 0, field_cap = 0;

    if (!check(p, TOK_RBRACE)) {
        do {
            const char *fname = tok_intern(p, expect(p, TOK_IDENT));
            expect(p, TOK_EQ);
            Expr *val = parse_expr(p, PREC_NONE + 1);
            FieldInit fi = { .name = fname, .value = val };
            DA_APPEND(fields, field_count, field_cap, fi);
            if (!check(p, TOK_COMMA)) break;
            advance_p(p);
        } while (!check(p, TOK_RBRACE));
    }
    expect(p, TOK_RBRACE);

    Expr *e = alloc_expr(p, EXPR_STRUCT_LIT, loc);
    e->struct_lit.type_name = type_name;
    e->struct_lit.field_count = field_count;
    if (field_count > 0) {
        e->struct_lit.fields = arena_alloc(p->arena, sizeof(FieldInit) * (size_t)field_count);
        memcpy(e->struct_lit.fields, fields, sizeof(FieldInit) * (size_t)field_count);
        free(fields);
    }
    return e;
}

/* ---- Top-level declaration parsing ---- */

static Decl *parse_decl(Parser *p);

static Decl *parse_let_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_LET);

    bool is_mut = false;
    if (check(p, TOK_MUT)) {
        advance_p(p);
        is_mut = true;
    }

    const char *name = tok_intern(p, expect(p, TOK_IDENT));
    expect(p, TOK_EQ);

    Expr *init;
    if (check(p, TOK_INDENT)) {
        /* Block body for let binding */
        int count;
        Expr **body = parse_block(p, &count);
        if (count == 1) {
            init = body[0];
        } else {
            init = alloc_expr(p, EXPR_BLOCK, loc);
            init->block.stmts = body;
            init->block.count = count;
        }
    } else {
        init = parse_expr(p, PREC_NONE + 1);
    }

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_LET;
    d->loc = loc;
    d->let.name = name;
    d->let.is_mut = is_mut;
    d->let.init = init;
    return d;
}

static Decl *parse_struct_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_STRUCT);
    const char *name = tok_intern(p, expect(p, TOK_IDENT));
    expect(p, TOK_EQ);

    /* Expect INDENT then field: type lines */
    expect(p, TOK_INDENT);

    StructField *fields = NULL;
    int field_count = 0, field_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        const char *fname = tok_intern(p, expect(p, TOK_IDENT));
        expect(p, TOK_COLON);
        Type *ftype = parse_type(p);

        StructField f = { .name = fname, .type = ftype };
        DA_APPEND(fields, field_count, field_cap, f);
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_STRUCT;
    d->loc = loc;
    d->struc.name = name;
    d->struc.field_count = field_count;
    if (field_count > 0) {
        d->struc.fields = arena_alloc(p->arena, sizeof(StructField) * (size_t)field_count);
        memcpy(d->struc.fields, fields, sizeof(StructField) * (size_t)field_count);
        free(fields);
    }
    return d;
}

static Decl *parse_union_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_UNION);
    const char *name = tok_intern(p, expect(p, TOK_IDENT));
    expect(p, TOK_EQ);

    /* Expect INDENT then | variant(type) lines */
    expect(p, TOK_INDENT);

    UnionVariant *variants = NULL;
    int variant_count = 0, variant_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        expect(p, TOK_PIPE);
        const char *vname = tok_intern(p, expect(p, TOK_IDENT));
        Type *payload = NULL;
        if (check(p, TOK_LPAREN)) {
            advance_p(p);
            payload = parse_type(p);
            expect(p, TOK_RPAREN);
        }

        UnionVariant v = { .name = vname, .payload = payload };
        DA_APPEND(variants, variant_count, variant_cap, v);
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_UNION;
    d->loc = loc;
    d->unio.name = name;
    d->unio.variant_count = variant_count;
    if (variant_count > 0) {
        d->unio.variants = arena_alloc(p->arena, sizeof(UnionVariant) * (size_t)variant_count);
        memcpy(d->unio.variants, variants, sizeof(UnionVariant) * (size_t)variant_count);
        free(variants);
    }
    return d;
}

static Decl *parse_module_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_MODULE);
    const char *name = tok_intern(p, expect(p, TOK_IDENT));
    expect(p, TOK_EQ);

    /* Parse body: INDENT { decl } DEDENT */
    expect(p, TOK_INDENT);

    Decl **decls = NULL;
    int count = 0, cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;
        Decl *d = parse_decl(p);
        DA_APPEND(decls, count, cap, d);
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_MODULE;
    d->loc = loc;
    d->is_private = false;
    d->module.name = name;
    d->module.from_lib = NULL;
    d->module.decl_count = count;
    if (count > 0) {
        d->module.decls = arena_alloc(p->arena, sizeof(Decl*) * (size_t)count);
        memcpy(d->module.decls, decls, sizeof(Decl*) * (size_t)count);
        free(decls);
    } else {
        d->module.decls = NULL;
    }
    return d;
}

static Decl *parse_import_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_IMPORT);

    /* import * from MODULE */
    if (check(p, TOK_STAR)) {
        advance_p(p);
        expect(p, TOK_FROM);
        const char *mod_name = tok_intern(p, expect(p, TOK_IDENT));
        Decl *d = arena_alloc(p->arena, sizeof(Decl));
        d->kind = DECL_IMPORT;
        d->loc = loc;
        d->is_private = false;
        d->import.name = NULL;
        d->import.alias = NULL;
        d->import.from_module = mod_name;
        d->import.from_namespace = NULL;
        d->import.is_wildcard = true;
        return d;
    }

    /* import NAME [as ALIAS] [from MODULE] */
    const char *name = tok_intern(p, expect(p, TOK_IDENT));
    const char *alias = NULL;
    const char *from_module = NULL;

    if (check(p, TOK_AS)) {
        advance_p(p);
        alias = tok_intern(p, expect(p, TOK_IDENT));
    }

    if (check(p, TOK_FROM)) {
        advance_p(p);
        from_module = tok_intern(p, expect(p, TOK_IDENT));
    }

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_IMPORT;
    d->loc = loc;
    d->is_private = false;
    d->import.name = name;
    d->import.alias = alias;
    d->import.from_module = from_module;
    d->import.from_namespace = NULL;
    d->import.is_wildcard = false;
    return d;
}

static Decl *parse_namespace_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_NAMESPACE);

    /* namespace IDENT :: [IDENT ::] ... */
    const char *first = tok_intern(p, expect(p, TOK_IDENT));
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", first);

    while (check(p, TOK_COLONCOLON)) {
        advance_p(p);
        if (check(p, TOK_IDENT)) {
            const char *part = tok_intern(p, expect(p, TOK_IDENT));
            size_t cur = strlen(buf);
            snprintf(buf + cur, sizeof(buf) - cur, "_%s", part);
        }
    }

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_NAMESPACE;
    d->loc = loc;
    d->is_private = false;
    d->ns.name = intern_cstr(p->intern, buf);
    return d;
}

static Decl *parse_decl(Parser *p) {
    skip_newlines(p);

    /* private modifier */
    if (check(p, TOK_PRIVATE)) {
        advance_p(p);
        Decl *d = parse_decl(p);
        d->is_private = true;
        return d;
    }

    if (check(p, TOK_LET)) return parse_let_decl(p);
    if (check(p, TOK_STRUCT)) return parse_struct_decl(p);
    if (check(p, TOK_UNION)) return parse_union_decl(p);
    if (check(p, TOK_MODULE)) return parse_module_decl(p);
    if (check(p, TOK_IMPORT)) return parse_import_decl(p);
    if (check(p, TOK_NAMESPACE)) return parse_namespace_decl(p);

    SrcLoc loc = loc_from_token(current(p));
    diag_fatal(loc, "expected declaration, got %s",
        token_kind_name(current(p)->kind));
}

Program *parse_program(Parser *p) {
    Decl **decls = NULL;
    int count = 0, cap = 0;
    skip_newlines(p);
    while (!at_end_p(p)) {
        Decl *d = parse_decl(p);
        DA_APPEND(decls, count, cap, d);
        skip_newlines(p);
    }
    Program *prog = arena_alloc(p->arena, sizeof(Program));
    if (count > 0) {
        prog->decls = arena_alloc(p->arena, sizeof(Decl*) * (size_t)count);
        memcpy(prog->decls, decls, sizeof(Decl*) * (size_t)count);
    }
    prog->decl_count = count;
    free(decls);
    return prog;
}
