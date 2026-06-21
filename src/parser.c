#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>

void parser_init(Parser *p, Token *tokens, int count, Arena *arena, InternTable *intern) {
    p->tokens = tokens;
    p->token_count = count;
    p->pos = 0;
    p->arena = arena;
    p->intern = intern;
    p->filename = NULL;
    p->pending_decls = NULL;
    p->pending_count = 0;
    p->pending_cap = 0;
    p->allow_fixed_array = false;
    p->block_arm_arrow = false;
    p->expr_start_pos = 0;
    p->half_gt = false;
    p->half_gt_pos = -1;
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

/* Backtrack to a saved token position. Always abandons any pending '>>' split
 * (the '>>' token is never mutated, so a re-parse from `save` sees it intact),
 * which keeps a split that occurred after `save` from leaking across the
 * restore — e.g. a tentative type parse in `(a<b>>c)` that splits the '>>' then
 * backtracks must re-parse it as the shift `b >> c`. */
static void restore_pos(Parser *p, int save) {
    p->half_gt = false;
    p->half_gt_pos = -1;
    p->pos = save;
}

/* True if the current token closes a type-argument list: a '>' (TOK_GT), a
 * '>>' (TOK_GTGT, two closers), or the parked second half of an already-split
 * '>>'. */
static bool at_typearg_gt(Parser *p) {
    if (p->half_gt && p->pos == p->half_gt_pos) return true;
    TokenKind k = current(p)->kind;
    return k == TOK_GT || k == TOK_GTGT;
}

/* Consume one closing '>' of a type-argument list. A '>>' (TOK_GTGT) carries two
 * closers: the first call splits it — recording the split and staying parked on
 * the token — so a nested list can close; the enclosing list's call consumes the
 * second half and advances. This mirrors the token-splitting that C++11, Java,
 * and Rust perform for '>>' in type-argument context. Returns false (consuming
 * nothing) when the current token closes no type-arg list. */
static bool consume_typearg_gt(Parser *p) {
    if (p->half_gt && p->pos == p->half_gt_pos) {
        /* second '>' of a previously-split '>>' */
        p->half_gt = false;
        p->half_gt_pos = -1;
        advance_p(p);
        return true;
    }
    TokenKind k = current(p)->kind;
    if (k == TOK_GT) {
        advance_p(p);
        return true;
    }
    if (k == TOK_GTGT) {
        /* consume the first '>'; stay parked for the enclosing list's closer */
        p->half_gt = true;
        p->half_gt_pos = p->pos;
        return true;
    }
    return false;
}

/* Consume a required type-argument closer, erroring otherwise. */
static void expect_typearg_gt(Parser *p) {
    if (!consume_typearg_gt(p)) {
        SrcLoc loc = loc_from_token(current(p));
        diag_fatal(loc, "expected '>' to close type arguments, got %s",
            token_kind_name(current(p)->kind));
    }
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

/* Accept an identifier or a reserved keyword token as a C name in extern declarations.
 * This allows 'extern free as c_free: ...' where 'free' is normally a keyword. */
static Token *expect_extern_c_name(Parser *p) {
    TokenKind k = current(p)->kind;
    if (k == TOK_IDENT || k == TOK_ALLOC || k == TOK_FREE || k == TOK_SIZEOF ||
        k == TOK_ALIGNOF || k == TOK_DEFAULT || k == TOK_ASSERT) {
        return advance_p(p);
    }
    SrcLoc loc = loc_from_token(current(p));
    diag_fatal(loc, "expected identifier in extern declaration, got %s",
        token_kind_name(k));
    return NULL; /* unreachable */
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
    memset(e, 0, sizeof(Expr));
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

/* Check if a token kind is valid inside a type argument list <...> */
static bool is_type_arg_token(TokenKind k) {
    switch (k) {
    case TOK_IDENT: case TOK_TYPE_VAR: case TOK_VOID:
    case TOK_LT: case TOK_GT:
    case TOK_COMMA:
    case TOK_DOT:                       /* module-qualified type: m.point */
    case TOK_QUESTION: case TOK_STAR:
    case TOK_LBRACKET: case TOK_RBRACKET:
    case TOK_LPAREN: case TOK_RPAREN:
    case TOK_LBRACE: case TOK_RBRACE:   /* tuple type {T1, T2} as a generic arg */
    case TOK_ARROW:
    case TOK_CONST:
        return true;
    default:
        return false;
    }
}

/* Decide whether a '<' begins a generic-call type-argument list: scan from
 * `start` (the token just after the '<') for type-compatible tokens up to the
 * matching '>', which must be followed by '(' (call) or '.' (variant). */
static bool generic_call_scan(Parser *p, int start) {
    int scan = start;
    int depth = 1;
    while (scan < p->token_count && depth > 0) {
        TokenKind k = p->tokens[scan].kind;
        if (k == TOK_LT) { depth++; scan++; continue; }
        if (k == TOK_GT) { depth--; scan++; continue; }
        if (k == TOK_GTGT) {
            /* '>>' closes two levels (split in type-argument context) */
            depth -= 2;
            if (depth < 0) return false; /* unbalanced — read as comparison/shift */
            scan++;
            continue;
        }
        if (!is_type_arg_token(k)) return false;
        scan++;
    }
    /* depth reached 0; `scan` now sits just past the closing '>' / '>>', which
     * must be followed by '(' (call) or '.' (variant). */
    return depth == 0 && scan < p->token_count &&
           (p->tokens[scan].kind == TOK_LPAREN ||
            p->tokens[scan].kind == TOK_DOT);
}

/* Like generic_call_scan, but for a *bare* generic instantiation in value
 * position: a balanced <...> type-argument list NOT followed by '(' or '.', but
 * by a token that cannot begin the right operand of a comparison (an expression
 * terminator). `name<Type>` followed by such a token can only be a misuse of
 * explicit type arguments without a call — never a valid comparison chain
 * (`a < b > c` keeps the comparison reading because `c` starts an expression) —
 * so we route it to pass2 for a clean "cannot be used as a value" diagnostic
 * instead of letting '>' fall through to a comparison and fail with a cryptic
 * "unexpected token" parse error. The terminator set is deliberately
 * conservative: it lists only tokens that unambiguously cannot start an
 * expression, so no valid comparison is ever misread (a terminator we omit just
 * keeps the old, less-helpful error — never a regression). */
static bool bare_inst_scan(Parser *p, int start) {
    int scan = start;
    int depth = 1;
    while (scan < p->token_count && depth > 0) {
        TokenKind k = p->tokens[scan].kind;
        if (k == TOK_LT) { depth++; scan++; continue; }
        if (k == TOK_GT) { depth--; scan++; continue; }
        if (k == TOK_GTGT) {
            depth -= 2;
            if (depth < 0) return false;
            scan++;
            continue;
        }
        if (!is_type_arg_token(k)) return false;
        scan++;
    }
    if (depth != 0 || scan >= p->token_count) return false;
    switch (p->tokens[scan].kind) {
    case TOK_NEWLINE: case TOK_DEDENT: case TOK_EOF:
    case TOK_RPAREN: case TOK_RBRACKET: case TOK_RBRACE:
    case TOK_COMMA:
        return true;
    default:
        return false;
    }
}

static bool is_type_name(const char *s, int len) {
    struct { const char *n; int l; } names[] = {
        {"int8",2+2}, {"int16",2+3}, {"int32",2+3}, {"int64",2+3},
        {"uint8",3+1+1}, {"uint16",3+2+1}, {"uint32",3+2+1}, {"uint64",3+2+1},
        {"float32",5+2}, {"float64",5+2},
        {"bool",4}, {"char",4},
        {"str",3}, {"cstr",4},
        {"any",3},
    };
    /* Actually compute lengths correctly */
    static const struct { const char *n; int l; } types[] = {
        {"int8",4}, {"int16",5}, {"int32",5}, {"int64",5},
        {"uint8",5}, {"uint16",6}, {"uint32",6}, {"uint64",6},
        {"float32",7}, {"float64",7},
        {"bool",4}, {"char",4},
        {"str",3}, {"cstr",4},
        {"any",3},
        {"isize",5}, {"usize",5},
    };
    (void)names;
    for (int i = 0; i < (int)(sizeof(types)/sizeof(types[0])); i++) {
        if (types[i].l == len && memcmp(s, types[i].n, (size_t)len) == 0) return true;
    }
    return false;
}

/* type_from_name is now in types.c/h */

static uint64_t parse_int_value(const char *start, int length, bool *out_of_range);

static Type *parse_type_suffix(Parser *p, Type *base) {
    /* T*, T[], T[N], T? — left to right */
    for (;;) {
        if (check(p, TOK_STAR)) {
            advance_p(p);
            base = type_pointer(p->arena, base);
            continue;
        }
        if (check(p, TOK_LBRACKET) && peek_at(p, 1)->kind == TOK_RBRACKET &&
            peek_at(p, 2)->kind != TOK_LBRACE) {
            /* "[]" NOT followed by "{" is a slice-type suffix. "T[] {" is a slice
             * *literal* (T[] { ptr = .., len = .. }) whose "[]" belongs to the
             * literal, not the element type — leave it for parse_array_lit_body.
             * A type is never legitimately followed by an open brace, so this
             * guard is unambiguous in every context. */
            advance_p(p); advance_p(p);
            base = type_slice(p->arena, base);
            continue;
        }
        /* T[N] — fixed-size inline array, only in struct/extern field declarations */
        if (p->allow_fixed_array && check(p, TOK_LBRACKET) && peek_at(p, 1)->kind == TOK_INT_LIT) {
            advance_p(p); /* consume [ */
            Token *size_tok = current(p);
            bool size_oor = false;
            int64_t size = parse_int_value(size_tok->start, size_tok->length, &size_oor);
            if (size_oor || size <= 0) {
                SrcLoc loc = loc_from_token(size_tok);
                diag_fatal(loc, "fixed array size must be a positive integer, got %lld",
                           (long long)size);
            }
            advance_p(p); /* consume INT_LIT */
            expect(p, TOK_RBRACKET);
            base = type_fixed_array(p->arena, base, size);
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

static Type *apply_const(Arena *a, Type *inner, SrcLoc loc) {
    if (inner->kind == TYPE_POINTER || inner->kind == TYPE_SLICE ||
        inner->kind == TYPE_ANY_PTR) {
        Type *c = arena_alloc(a, sizeof(Type));
        *c = *inner;
        c->is_const = true;
        return c;
    }
    if (inner->kind == TYPE_OPTION &&
        inner->option.inner &&
        (inner->option.inner->kind == TYPE_POINTER ||
         inner->option.inner->kind == TYPE_SLICE ||
         inner->option.inner->kind == TYPE_ANY_PTR)) {
        Type *ci = arena_alloc(a, sizeof(Type));
        *ci = *inner->option.inner;
        ci->is_const = true;
        Type *opt = type_option(a, ci);
        return opt;
    }
    diag_fatal(loc, "'const' can only modify pointer (*) or slice ([]) types, got %s",
               type_name(inner));
    return NULL; /* unreachable */
}

static Type *parse_type(Parser *p) {
    Token *t = current(p);

    if (t->kind == TOK_CONST) {
        SrcLoc loc = loc_from_token(t);
        advance_p(p);
        Type *inner = parse_type(p);
        return apply_const(p->arena, inner, loc);
    }

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
                if (!check(p, TOK_STAR)) {
                    SrcLoc loc = loc_from_token(current(p));
                    diag_fatal(loc, "'any' must be followed by '*' (any*)");
                }
                advance_p(p); /* consume the mandatory * */
                return parse_type_suffix(p, base);
            }
            return parse_type_suffix(p, base);
        }
        /* User-defined type name (struct/union) — create a stub with just the name.
         * Support module-qualified names: module.type, module.sub.type, etc. */
        advance_p(p);
        const char *type_name = tok_intern(p, t);
        while (check(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
            advance_p(p); /* consume . */
            Token *member = current(p);
            advance_p(p); /* consume member name */
            char buf[512];
            snprintf(buf, sizeof(buf), "%s.%.*s", type_name, member->length, member->start);
            type_name = intern(p->intern, buf, (int)strlen(buf));
        }
        Type *udt = arena_alloc(p->arena, sizeof(Type));
        udt->kind = TYPE_STUB;
        udt->stub.name = type_name;
        udt->stub.qualified_name = NULL;
        udt->stub.type_args = NULL;
        udt->stub.type_arg_count = 0;
        /* Check for type arguments: name<Type, ...> */
        if (check(p, TOK_LT)) {
            /* Scan forward to verify this is a type arg list (not comparison) */
            int save = p->pos;
            advance_p(p); /* consume < */
            Type **targs = NULL;
            int ta_count = 0, ta_cap = 0;
            bool valid = true;
            do {
                Token *tt = current(p);
                if (tt->kind == TOK_IDENT || tt->kind == TOK_TYPE_VAR ||
                    tt->kind == TOK_LPAREN || tt->kind == TOK_VOID ||
                    tt->kind == TOK_CONST || tt->kind == TOK_LBRACE) {
                    Type *ty = parse_type(p);
                    DA_APPEND(targs, ta_count, ta_cap, ty);
                } else {
                    valid = false;
                    break;
                }
                if (!check(p, TOK_COMMA)) break;
                advance_p(p);
            } while (1);
            if (valid && at_typearg_gt(p)) {
                consume_typearg_gt(p); /* consume > (splitting a >> for a nested list) */
                udt->stub.type_args = arena_alloc(p->arena, sizeof(Type*) * (size_t)ta_count);
                memcpy(udt->stub.type_args, targs, sizeof(Type*) * (size_t)ta_count);
                udt->stub.type_arg_count = ta_count;
                free(targs);
            } else {
                /* Not type args — backtrack */
                restore_pos(p, save);
                free(targs);
            }
        }
        return parse_type_suffix(p, udt);
    }

    /* Tuple type: { T1, T2, ... } — anonymous positional product (>= 2 elements) */
    if (t->kind == TOK_LBRACE) {
        SrcLoc loc = loc_from_token(t);
        advance_p(p); /* consume { */
        Type **elems = NULL;
        int ecount = 0, ecap = 0;
        if (!check(p, TOK_RBRACE)) {
            do {
                Type *et = parse_type(p);
                DA_APPEND(elems, ecount, ecap, et);
                if (!check(p, TOK_COMMA)) break;
                advance_p(p);
            } while (!check(p, TOK_RBRACE));
        }
        expect(p, TOK_RBRACE);
        if (ecount < 2)
            diag_fatal(loc, "tuple type requires at least 2 element types, got %d", ecount);
        Type *tup = type_tuple(p->arena, elems, ecount);
        free(elems);
        return parse_type_suffix(p, tup);
    }

    /* Function type: (T1, T2) -> T  or  (T1, ...) -> T */
    if (t->kind == TOK_LPAREN) {
        advance_p(p);
        Type **params = NULL;
        int pcount = 0, pcap = 0;
        bool is_variadic = false;
        if (!check(p, TOK_RPAREN)) {
            if (check(p, TOK_ELLIPSIS)) {
                /* (...) -> T */
                advance_p(p);
                is_variadic = true;
            } else {
                do {
                    Type *pt = parse_type(p);
                    DA_APPEND(params, pcount, pcap, pt);
                    if (!check(p, TOK_COMMA)) break;
                    advance_p(p);
                    if (check(p, TOK_ELLIPSIS)) {
                        /* (T1, ...) -> T */
                        advance_p(p);
                        is_variadic = true;
                        break;
                    }
                } while (1);
            }
        }
        expect(p, TOK_RPAREN);
        if (!check(p, TOK_ARROW)) {
            /* No "->": this "(...)" was a parenthesized (grouped) type, not a
             * function type — e.g. `((int32) -> int32)?` groups the inner
             * function type so a postfix (?, *, []) can apply to the whole thing.
             * Grammar: `type_atom = "(" type_param_list ")"`, which is a valid
             * type_primary that postfixes attach to. Only a single, non-variadic
             * element forms a grouped type; a bare `(T1, T2)` or `(...)` is
             * meaningful only as a function parameter list and needs the `->`. */
            if (pcount != 1 || is_variadic) {
                SrcLoc gloc = loc_from_token(current(p));
                diag_fatal(gloc, "expected '->' after function parameter list");
            }
            Type *grouped = params[0];
            free(params);
            return parse_type_suffix(p, grouped);
        }
        advance_p(p); /* consume -> */
        Type *ret = parse_type(p);

        Type *ft = arena_alloc(p->arena, sizeof(Type));
        ft->kind = TYPE_FUNC;
        ft->func.param_count = pcount;
        ft->func.return_type = ret;
        ft->func.is_variadic = is_variadic;
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
static Pattern *parse_pattern(Parser *p);
static Expr *parse_match_expr(Parser *p);
static Expr *parse_struct_literal(Parser *p, const char *type_name, SrcLoc loc);

/* Parse a sub-expression inside matched brackets (parens, square, curly).
   Clears `block_arm_arrow` for the duration so pointer-field access (`p->x`)
   is unambiguous inside a bracketed guard sub-expression. */
static Expr *parse_bracketed_expr(Parser *p, Prec min_prec) {
    bool saved = p->block_arm_arrow;
    p->block_arm_arrow = false;
    Expr *e = parse_expr(p, min_prec);
    p->block_arm_arrow = saved;
    return e;
}

static uint64_t parse_int_value(const char *start, int length, bool *out_of_range) {
    char buf[72];  /* 64 binary digits + null + margin */
    int num_len = 0;
    if (out_of_range) *out_of_range = false;

    int base = 10;
    int prefix_len = 0;
    if (length >= 2 && start[0] == '0') {
        if (start[1] == 'x' || start[1] == 'X')      { base = 16; prefix_len = 2; }
        else if (start[1] == 'b' || start[1] == 'B') { base = 2;  prefix_len = 2; }
        else if (start[1] == 'o' || start[1] == 'O') { base = 8;  prefix_len = 2; }
    }

    for (int i = prefix_len; i < length && num_len < 71; i++) {
        char c = start[i];
        bool ok = false;
        switch (base) {
        case 16: ok = isxdigit((unsigned char)c); break;
        case 10: ok = (c >= '0' && c <= '9'); break;
        case 8:  ok = (c >= '0' && c <= '7'); break;
        case 2:  ok = (c == '0' || c == '1'); break;
        }
        if (ok) buf[num_len++] = c;
        else if (c == '_') continue;
        else break;
    }
    buf[num_len] = '\0';
    errno = 0;
    uint64_t v = strtoull(buf, NULL, base);
    if (errno == ERANGE && out_of_range) *out_of_range = true;
    return v;
}

/* Find where the numeric part ends (for suffix extraction) */
static int int_num_end(const char *start, int length) {
    if (length >= 2 && start[0] == '0') {
        if (start[1] == 'x' || start[1] == 'X') {
            int i = 2;
            while (i < length && (isxdigit((unsigned char)start[i]) || start[i] == '_')) i++;
            return i;
        }
        if (start[1] == 'b' || start[1] == 'B') {
            int i = 2;
            while (i < length && (start[i] == '0' || start[i] == '1' || start[i] == '_')) i++;
            return i;
        }
        if (start[1] == 'o' || start[1] == 'O') {
            int i = 2;
            while (i < length && ((start[i] >= '0' && start[i] <= '7') || start[i] == '_')) i++;
            return i;
        }
    }
    int i = 0;
    while (i < length && ((start[i] >= '0' && start[i] <= '9') || start[i] == '_')) i++;
    return i;
}

static Type *parse_int_type(const char *start, int length) {
    int num_end = int_num_end(start, length);
    if (num_end >= length) return type_int32();
    Type *t = type_from_int_suffix(start + num_end, length - num_end);
    return t ? t : type_int32();
}

/* Parse the byte value from a char literal token (e.g., 'a', '\n', '\x41').
   Token includes the surrounding single quotes. */
static uint8_t parse_char_value(const char *start, int length) {
    /* start[0] = ', start[length-1] = ' */
    if (length < 3) return 0;
    if (start[1] != '\\') return (uint8_t)start[1];
    /* escape sequence */
    if (length < 4) return 0;
    switch (start[2]) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '\\': return '\\';
    case '\'': return '\'';
    case '0':  return '\0';
    case 'x': {
        /* \xNN — two hex digits */
        if (length < 6) return 0;
        unsigned val = 0;
        for (int i = 3; i < 5; i++) {
            val <<= 4;
            char c = start[i];
            if (c >= '0' && c <= '9') val += (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') val += (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val += (unsigned)(c - 'A' + 10);
        }
        return (uint8_t)val;
    }
    default: return (uint8_t)start[2];
    }
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

/* Parse a guarded/unguarded expression: the keyword (already consumed) is
   followed by either an indented block or a single inline expression. The body
   is wrapped in EXPR_GUARD with the given polarity. The inline operand parses at
   PREC_NONE+1 (as let-init / if-cond do), so `unguarded a / b` captures the whole
   `a / b` while parentheses bound it inside a larger expression. */
static Expr *parse_guard(Parser *p, bool guarded, SrcLoc loc) {
    Expr *body;
    if (check(p, TOK_INDENT)) {
        int count;
        Expr **stmts = parse_block(p, &count);
        if (count == 1) {
            body = stmts[0];
        } else {
            body = alloc_expr(p, EXPR_BLOCK, loc);
            body->block.stmts = stmts;
            body->block.count = count;
        }
    } else {
        body = parse_expr(p, PREC_NONE + 1);
    }
    Expr *e = alloc_expr(p, EXPR_GUARD, loc);
    e->guard.body = body;
    e->guard.guarded = guarded;
    return e;
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
            ploc.filename = p->filename;
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

    int save_pos = p->pos;
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
    } else {
        /* No else — restore position so NEWLINE is visible to Pratt loop */
        restore_pos(p, save_pos);
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

        /* Struct destructuring: let { field = name, ... } = expr */
        if (check(p, TOK_LBRACE)) {
            /* Reuse parse_pattern to handle nested destructuring */
            Pattern *pat = parse_pattern(p);
            expect(p, TOK_EQ);

            Expr *init = parse_expr(p, PREC_NONE + 1);
            Expr *e = alloc_expr(p, EXPR_LET_DESTRUCT, loc);
            e->let_destruct.pattern = pat;
            e->let_destruct.is_mut = is_mut;
            e->let_destruct.init = init;
            return e;
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

    if (check(p, TOK_DEFER)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        Expr *value = parse_expr(p, PREC_NONE + 1);
        Expr *e = alloc_expr(p, EXPR_DEFER, loc);
        e->defer_expr.value = value;
        return e;
    }

    return parse_expr(p, PREC_NONE + 1);
}

/* ---- Prefix parsing ---- */

/* Parse the body of an array or slice literal given an already-parsed element
 * type. The parser must be positioned just before the '[':
 *   T[]  { ptr = expr, len = expr }   → EXPR_SLICE_LIT
 *   T[N] { e0, e1, ... }              → EXPR_ARRAY_LIT
 * Shared by the IDENT-typed and tuple-typed literal paths. */
static Expr *parse_array_lit_body(Parser *p, Type *elem_type, SrcLoc loc) {
    expect(p, TOK_LBRACKET);

    /* T[] { ptr = expr, len = expr } — slice construction from raw parts */
    if (check(p, TOK_RBRACKET)) {
        advance_p(p); /* consume ] */
        expect(p, TOK_LBRACE);
        Expr *ptr_val = NULL, *len_val = NULL;
        while (!check(p, TOK_RBRACE)) {
            const char *fn = tok_intern(p, expect(p, TOK_IDENT));
            expect(p, TOK_EQ);
            Expr *val = parse_bracketed_expr(p, PREC_NONE + 1);
            if (strcmp(fn, "ptr") == 0) ptr_val = val;
            else if (strcmp(fn, "len") == 0) len_val = val;
            else {
                SrcLoc floc = loc_from_token(current(p));
                diag_fatal(floc, "slice literal field must be 'ptr' or 'len', got '%s'", fn);
            }
            if (check(p, TOK_COMMA)) advance_p(p);
        }
        expect(p, TOK_RBRACE);
        if (!ptr_val || !len_val)
            diag_fatal(loc, "slice literal requires both 'ptr' and 'len' fields");
        Expr *e = alloc_expr(p, EXPR_SLICE_LIT, loc);
        e->slice_lit.elem_type = elem_type;
        e->slice_lit.ptr_expr = ptr_val;
        e->slice_lit.len_expr = len_val;
        return e;
    }

    Expr *size_expr = parse_bracketed_expr(p, PREC_NONE + 1);
    expect(p, TOK_RBRACKET);
    expect(p, TOK_LBRACE);

    Expr **elems = NULL;
    int elem_count = 0, elem_cap = 0;
    if (!check(p, TOK_RBRACE)) {
        do {
            Expr *elem = parse_bracketed_expr(p, PREC_NONE + 1);
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

static Expr *parse_prefix(Parser *p) {
    Token *t = current(p);
    SrcLoc loc = loc_from_token(t);

    switch (t->kind) {
    case TOK_INT_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_INT_LIT, loc);
        bool int_oor = false;
        e->int_lit.value = parse_int_value(t->start, t->length, &int_oor);
        e->int_lit.lit_type = parse_int_type(t->start, t->length);
        e->int_lit.out_of_range = int_oor;
        return e;
    }

    case TOK_FLOAT_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_FLOAT_LIT, loc);
        /* Determine suffix length first so the strtod buffer excludes it */
        int suffix_len = 0;
        if (t->length >= 3 && t->start[t->length - 3] == 'f'
            && (t->start[t->length - 2] == '3' || t->start[t->length - 2] == '6')
            && (t->start[t->length - 1] == '2' || t->start[t->length - 1] == '4')) {
            suffix_len = 3;
        }
        int num_end = t->length - suffix_len;
        char buf[128];
        int num_len = 0;
        bool mantissa_nonzero = false;
        bool past_mantissa = false;
        bool is_hex = t->length >= 2 && t->start[0] == '0' && (t->start[1] == 'x' || t->start[1] == 'X');
        for (int i = 0; i < num_end && num_len < (int)sizeof(buf) - 1; i++) {
            char c = t->start[i];
            if (c == '_') continue;
            if (is_hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E')) past_mantissa = true;
            if (!past_mantissa) {
                if (c >= '1' && c <= '9') mantissa_nonzero = true;
                else if (is_hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) mantissa_nonzero = true;
            }
            buf[num_len++] = c;
        }
        buf[num_len] = '\0';
        errno = 0;
        double v = strtod(buf, NULL);
        bool oor = false, underflow = false;
        /* strtod sets ERANGE on overflow (returns ±HUGE_VAL) and may also
         * set it on underflow even for subnormal results. Distinguish by
         * checking the result: only ±inf is overflow; only 0 from a nonzero
         * source is underflow; subnormals are accepted silently. */
        if (isinf(v)) oor = true;
        else if (v == 0.0 && mantissa_nonzero) underflow = true;
        bool is_f32 = (suffix_len == 3 && t->start[t->length - 2] == '3');
        if (is_f32 && !oor && !underflow) {
            float fv = (float)v;
            if (isinf(fv)) oor = true;
            else if (fv == 0.0f && mantissa_nonzero) underflow = true;
        }
        e->float_lit.value = v;
        e->float_lit.lit_type = is_f32 ? type_float32() : type_float64();
        e->float_lit.out_of_range = oor;
        e->float_lit.underflow = underflow;
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

    case TOK_CINTERP_START:
    case TOK_INTERP_START: {
        advance_p(p); /* consume INTERP_START / CINTERP_START */
        InterpSegment *segs = NULL;
        int seg_count = 0, seg_cap = 0;

        /* Add leading literal text segment */
        InterpSegment lit_seg;
        memset(&lit_seg, 0, sizeof(lit_seg));
        lit_seg.is_literal = true;
        lit_seg.text = t->start;
        lit_seg.text_length = t->length;
        DA_APPEND(segs, seg_count, seg_cap, lit_seg);

        for (;;) {
            /* Expect FMT_SPEC */
            Token *fmt = expect(p, TOK_FMT_SPEC);
            InterpSegment fmt_seg;
            memset(&fmt_seg, 0, sizeof(fmt_seg));
            fmt_seg.is_literal = false;
            fmt_seg.text = fmt->start;
            fmt_seg.text_length = fmt->length;
            /* Extract conversion character (last char of format spec) */
            fmt_seg.conversion = fmt->start[fmt->length - 1];

            /* Parse expression */
            fmt_seg.expr = parse_bracketed_expr(p, PREC_NONE + 1);

            DA_APPEND(segs, seg_count, seg_cap, fmt_seg);

            if (check(p, TOK_INTERP_MID)) {
                Token *mid = advance_p(p);
                InterpSegment mid_seg;
                memset(&mid_seg, 0, sizeof(mid_seg));
                mid_seg.is_literal = true;
                mid_seg.text = mid->start;
                mid_seg.text_length = mid->length;
                DA_APPEND(segs, seg_count, seg_cap, mid_seg);
                continue;
            }

            if (check(p, TOK_INTERP_END)) {
                Token *end = advance_p(p);
                InterpSegment end_seg;
                memset(&end_seg, 0, sizeof(end_seg));
                end_seg.is_literal = true;
                end_seg.text = end->start;
                end_seg.text_length = end->length;
                DA_APPEND(segs, seg_count, seg_cap, end_seg);
                break;
            }

            diag_fatal(loc, "expected interpolation continuation or end, got %s",
                token_kind_name(current(p)->kind));
        }

        /* Copy segments into arena */
        InterpSegment *arena_segs = arena_alloc(p->arena,
            sizeof(InterpSegment) * (size_t)seg_count);
        memcpy(arena_segs, segs, sizeof(InterpSegment) * (size_t)seg_count);
        free(segs);

        Expr *e = alloc_expr(p, EXPR_INTERP_STRING, loc);
        e->interp_string.segments = arena_segs;
        e->interp_string.segment_count = seg_count;
        e->interp_string.is_cstr = (t->kind == TOK_CINTERP_START);
        return e;
    }

    case TOK_CHAR_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_CHAR_LIT, loc);
        e->char_lit.value = parse_char_value(t->start, t->length);
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

        /* Check for slice literal: type_name[size] { ... }
         * Supports built-in types, user-defined structs, and module-qualified types.
         * Disambiguate from indexing: scan past [expr] and check for { */
        {
        int arr_start = 1; /* offset past first IDENT to find [ */
        /* Scan past optional dot-qualified path: name.sub.type */
        while (peek_at(p, arr_start)->kind == TOK_DOT &&
               peek_at(p, arr_start + 1)->kind == TOK_IDENT)
            arr_start += 2;
        /* Scan past optional generic type args: <T, T, ...> (depth-counted so
           nested args like box<box<int32>> are skipped; '>>' closes two levels).
           Only matches when every token inside <...> is valid in a type position. */
        if (peek_at(p, arr_start)->kind == TOK_LT) {
            int scan = arr_start + 1;
            int depth = 1;
            bool ok = true;
            while (depth > 0) {
                TokenKind k = peek_at(p, scan)->kind;
                if (k == TOK_EOF) { ok = false; break; }
                if (k == TOK_LT) depth++;
                else if (k == TOK_GT) depth--;
                else if (k == TOK_GTGT) { depth -= 2; if (depth < 0) { ok = false; break; } }
                else if (!is_type_arg_token(k)) { ok = false; break; }
                scan++;
            }
            if (ok) arr_start = scan; /* scan sits just past the closing > / >> */
        }
        /* Scan past ? / * type suffixes on the element type: name?*[N]{...} */
        while (peek_at(p, arr_start)->kind == TOK_QUESTION ||
               peek_at(p, arr_start)->kind == TOK_STAR)
            arr_start += 1;
        if (peek_at(p, arr_start)->kind == TOK_LBRACKET) {
            /* Look ahead: type[expr] { — the { after ] means slice literal */
            int save = p->pos;
            /* Advance past IDENT (.IDENT)* [ */
            for (int skip = 0; skip < arr_start + 1; skip++) advance_p(p);
            /* Skip tokens until we find matching ] */
            int depth = 1;
            while (depth > 0 && !at_end_p(p)) {
                if (check(p, TOK_LBRACKET)) depth++;
                if (check(p, TOK_RBRACKET)) depth--;
                advance_p(p);
            }
            bool is_array_lit = check(p, TOK_LBRACE);
            /* Restore position */
            restore_pos(p, save);

            if (is_array_lit) {
                /* Build the type name (possibly dotted) */
                char buf[512];
                int pos = snprintf(buf, sizeof(buf), "%s", name);
                advance_p(p);  /* consume first ident */
                while (check(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
                    advance_p(p); /* consume . */
                    Token *seg = current(p);
                    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ".%.*s",
                                    seg->length, seg->start);
                    advance_p(p); /* consume ident */
                }
                const char *type_name_str = intern(p->intern, buf, (int)strlen(buf));
                Type *elem_type = type_from_name(t->start, t->length);
                if (!elem_type) {
                    /* User-defined type — create stub */
                    elem_type = arena_alloc(p->arena, sizeof(Type));
                    elem_type->kind = TYPE_STUB;
                    elem_type->stub.name = type_name_str;
                    elem_type->stub.qualified_name = NULL;
                    elem_type->stub.type_args = NULL;
                    elem_type->stub.type_arg_count = 0;
                }
                /* Parse optional generic type args: <T, T, ...>. Only attaches to
                   stub (user-defined) element types; built-in types can't have args. */
                if (check(p, TOK_LT) && elem_type->kind == TYPE_STUB) {
                    advance_p(p); /* consume < */
                    Type **targs = NULL;
                    int ta_count = 0, ta_cap = 0;
                    do {
                        Type *ty = parse_type(p);
                        DA_APPEND(targs, ta_count, ta_cap, ty);
                        if (!check(p, TOK_COMMA)) break;
                        advance_p(p);
                    } while (1);
                    expect_typearg_gt(p);
                    elem_type->stub.type_args = arena_alloc(p->arena,
                        sizeof(Type*) * (size_t)ta_count);
                    memcpy(elem_type->stub.type_args, targs,
                           sizeof(Type*) * (size_t)ta_count);
                    elem_type->stub.type_arg_count = ta_count;
                    free(targs);
                }
                /* Apply ? (option) and * (pointer) suffixes to the element type */
                while (check(p, TOK_QUESTION) || check(p, TOK_STAR)) {
                    if (check(p, TOK_QUESTION)) {
                        advance_p(p);
                        elem_type = type_option(p->arena, elem_type);
                    } else {
                        advance_p(p);
                        elem_type = type_pointer(p->arena, elem_type);
                    }
                }
                return parse_array_lit_body(p, elem_type, loc);
            }
        }
        } /* end slice literal check */

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
                /* str { ptr = expr, len = expr } — slice literal for string type */
                if (strcmp(name, "str") == 0) {
                    advance_p(p);  /* consume ident */
                    advance_p(p);  /* consume { */
                    Expr *ptr_val = NULL, *len_val = NULL;
                    while (!check(p, TOK_RBRACE)) {
                        const char *fn = tok_intern(p, expect(p, TOK_IDENT));
                        expect(p, TOK_EQ);
                        Expr *val = parse_bracketed_expr(p, PREC_NONE + 1);
                        if (strcmp(fn, "ptr") == 0) ptr_val = val;
                        else if (strcmp(fn, "len") == 0) len_val = val;
                        else diag_fatal(loc, "slice literal field must be 'ptr' or 'len', got '%s'", fn);
                        if (check(p, TOK_COMMA)) advance_p(p);
                    }
                    expect(p, TOK_RBRACE);
                    if (!ptr_val || !len_val)
                        diag_fatal(loc, "slice literal requires both 'ptr' and 'len' fields");
                    Type *et = arena_alloc(p->arena, sizeof(Type));
                    et->kind = TYPE_UINT8;
                    Expr *e = alloc_expr(p, EXPR_SLICE_LIT, loc);
                    e->slice_lit.elem_type = et;
                    e->slice_lit.ptr_expr = ptr_val;
                    e->slice_lit.len_expr = len_val;
                    return e;
                }
                advance_p(p);  /* consume ident */
                advance_p(p);  /* consume { */
                return parse_struct_literal(p, name, loc);
            }
        }
        /* Check for module-qualified struct literal: mod.name { ... }, mod.sub.name { ... }
         * Scan ahead past (. IDENT)* to find a { that looks like a struct literal */
        if (peek_at(p, 1)->kind == TOK_DOT && peek_at(p, 2)->kind == TOK_IDENT) {
            int ahead = 1; /* start after first IDENT */
            while (peek_at(p, ahead)->kind == TOK_DOT &&
                   peek_at(p, ahead + 1)->kind == TOK_IDENT) {
                ahead += 2; /* skip . IDENT */
            }
            /* Now peek_at(p, ahead) should be { if this is a struct literal */
            if (peek_at(p, ahead)->kind == TOK_LBRACE) {
                Token *after_brace = peek_at(p, ahead + 1);
                bool is_struct_lit = false;
                if (after_brace->kind == TOK_RBRACE) is_struct_lit = true;
                if (after_brace->kind == TOK_IDENT && peek_at(p, ahead + 2)->kind == TOK_EQ)
                    is_struct_lit = true;
                if (is_struct_lit) {
                    /* Build dotted name by consuming IDENT (. IDENT)* */
                    char buf[512];
                    int pos = snprintf(buf, sizeof(buf), "%s", name);
                    advance_p(p); /* consume first IDENT */
                    while (check(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
                        advance_p(p); /* consume . */
                        Token *seg = current(p);
                        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ".%.*s",
                                        seg->length, seg->start);
                        advance_p(p); /* consume IDENT */
                    }
                    advance_p(p); /* consume { */
                    const char *dotted_name = intern(p->intern, buf, (int)strlen(buf));
                    return parse_struct_literal(p, dotted_name, loc);
                }
            }
        }
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_IDENT, loc);
        e->ident.name = name;
        return e;
    }

    case TOK_LPAREN: {
        /* Array/slice literal with a parenthesized (grouped/function) element
         * type: `((A) -> B)[N] { e0, ... }` or `((A) -> B)[] { ptr = .., len = .. }`.
         * Detected by the shape "( ... ) [ ... ] {" — the trailing { after ] is
         * what distinguishes it from a cast or an indexed parenthesized
         * expression, neither of which is ever followed by a brace here (same
         * reasoning as the tuple-element path in the TOK_LBRACE case). */
        {
            int depth = 0, k = 0;
            do {
                TokenKind tk = peek_at(p, k)->kind;
                if (tk == TOK_LPAREN) depth++;
                else if (tk == TOK_RPAREN) depth--;
                else if (tk == TOK_EOF) break;
                k++;
            } while (depth > 0);
            if (peek_at(p, k)->kind == TOK_LBRACKET) {
                int bd = 0, j = k;
                do {
                    TokenKind tk = peek_at(p, j)->kind;
                    if (tk == TOK_LBRACKET) bd++;
                    else if (tk == TOK_RBRACKET) bd--;
                    else if (tk == TOK_EOF) break;
                    j++;
                } while (bd > 0);
                if (peek_at(p, j)->kind == TOK_LBRACE) {
                    Type *elem_type = parse_type(p); /* parses the grouped/function type */
                    return parse_array_lit_body(p, elem_type, loc);
                }
            }
        }

        /* Disambiguate: function literal vs parenthesized expression */
        /* () -> ... : empty-param function */
        if (peek_at(p, 1)->kind == TOK_RPAREN && peek_at(p, 2)->kind == TOK_ARROW) {
            return parse_func_literal(p);
        }
        /* (ident : ...) -> ... : function with params */
        if (peek_at(p, 1)->kind == TOK_IDENT && peek_at(p, 2)->kind == TOK_COLON) {
            return parse_func_literal(p);
        }
        /* (Type)expr : cast — check if next token is a type name, type var, or const.
         * Also try when (IDENT*) pattern is seen — user-defined struct pointer casts.
         * For module-qualified casts like (mod.type*)expr, scan past dots to find
         * a type suffix (*, ?, []) — bare (mod.name) is parenthesized field access.
         * Backtracking handles false positives like (a * b). */
        {
        bool try_cast = false;
        if (peek_at(p, 1)->kind == TOK_IDENT &&
            (is_type_name(peek_at(p, 1)->start, peek_at(p, 1)->length) ||
             peek_at(p, 2)->kind == TOK_STAR ||
             peek_at(p, 2)->kind == TOK_LT)) {
            try_cast = true;
        } else if (peek_at(p, 1)->kind == TOK_IDENT && peek_at(p, 2)->kind == TOK_DOT) {
            /* Scan past IDENT (.IDENT)* and check for type suffix */
            int ca = 1; /* start at first IDENT */
            while (peek_at(p, ca)->kind == TOK_IDENT && peek_at(p, ca + 1)->kind == TOK_DOT)
                ca += 2; /* skip IDENT . */
            /* ca now points to the last IDENT; check what follows */
            if (peek_at(p, ca)->kind == TOK_IDENT) {
                TokenKind after = peek_at(p, ca + 1)->kind;
                if (after == TOK_STAR || after == TOK_QUESTION || after == TOK_LBRACKET)
                    try_cast = true;
                /* Also allow (mod.type) as cast when followed by RPAREN and the
                 * final ident is a known type name — but we can't check that here.
                 * Only trigger for pointer/option/slice casts to avoid (a.b) ambiguity. */
            }
        }
        if (try_cast || peek_at(p, 1)->kind == TOK_TYPE_VAR ||
            peek_at(p, 1)->kind == TOK_CONST) {
            /* Try to parse as cast with backtracking */
            int save = p->pos;
            advance_p(p); /* ( */
            Type *target = parse_type(p);
            /* (cstr[N]) — bounded str→cstr cast. cstr is uint8*, so allow_fixed_array
             * (off in expression context) left the [N] unconsumed for us to claim. */
            int buffer_size = 0;
            if (is_cstr_type(target) && check(p, TOK_LBRACKET) &&
                peek_at(p, 1)->kind == TOK_INT_LIT && peek_at(p, 2)->kind == TOK_RBRACKET) {
                advance_p(p); /* [ */
                Token *nt = current(p);
                bool oor = false;
                int64_t n = parse_int_value(nt->start, nt->length, &oor);
                if (oor || n < 1) {
                    SrcLoc nloc = loc_from_token(nt);
                    diag_fatal(nloc, "(cstr[N]) buffer size must be a positive integer, got %lld",
                               (long long)n);
                }
                advance_p(p); /* N */
                advance_p(p); /* ] */
                buffer_size = (int)n;
            }
            if (check(p, TOK_RPAREN)) {
                advance_p(p);
                Expr *operand = parse_expr(p, PREC_PREFIX);
                Expr *e = alloc_expr(p, EXPR_CAST, loc);
                e->cast.target = target;
                e->cast.operand = operand;
                e->cast.buffer_size = buffer_size;
                return e;
            }
            /* Not a cast — backtrack */
            restore_pos(p, save);
        }
        } /* end try_cast block */
        /* Parenthesized expression */
        advance_p(p);
        Expr *e = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        return e;
    }

    case TOK_LBRACE: {
        /* Array/slice literal with a tuple element type: {T1, T2}[N] { ... }.
         * Detected by the shape "{ ... } [ ... ] {" — the trailing { after ] is
         * what distinguishes it from a tuple literal that is merely indexed
         * ({a, b}[i], which is never followed by a brace). */
        {
            int depth = 0, k = 0;
            do {
                TokenKind tk = peek_at(p, k)->kind;
                if (tk == TOK_LBRACE) depth++;
                else if (tk == TOK_RBRACE) depth--;
                else if (tk == TOK_EOF) break;
                k++;
            } while (depth > 0);
            if (peek_at(p, k)->kind == TOK_LBRACKET) {
                int bd = 0, j = k;
                do {
                    TokenKind tk = peek_at(p, j)->kind;
                    if (tk == TOK_LBRACKET) bd++;
                    else if (tk == TOK_RBRACKET) bd--;
                    else if (tk == TOK_EOF) break;
                    j++;
                } while (bd > 0);
                if (peek_at(p, j)->kind == TOK_LBRACE) {
                    Type *elem_type = parse_type(p);   /* parses the {T1, T2, ...} tuple type */
                    return parse_array_lit_body(p, elem_type, loc);
                }
            }
        }

        /* Bare positional braces — anonymous tuple literal { e0, e1, ... } (>= 2 elements).
         * Struct and slice literals always carry a type prefix, so a leading { is
         * unambiguously a tuple; blocks are indentation-based, never brace-delimited. */
        advance_p(p); /* consume { */
        Expr **elems = NULL;
        int elem_count = 0, elem_cap = 0;
        if (!check(p, TOK_RBRACE)) {
            do {
                Expr *elem = parse_bracketed_expr(p, PREC_NONE + 1);
                DA_APPEND(elems, elem_count, elem_cap, elem);
                if (!check(p, TOK_COMMA)) break;
                advance_p(p);
            } while (!check(p, TOK_RBRACE));
        }
        expect(p, TOK_RBRACE);
        if (elem_count < 2)
            diag_fatal(loc, "tuple literal requires at least 2 elements, got %d", elem_count);
        Expr *e = alloc_expr(p, EXPR_TUPLE_LIT, loc);
        e->tuple_lit.elems = arena_copy_exprs(p, elems, elem_count);
        e->tuple_lit.elem_count = elem_count;
        free(elems);
        return e;
    }

    case TOK_IF:
        return parse_if_expr(p);

    case TOK_MATCH:
        return parse_match_expr(p);

    case TOK_SOME: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Expr *val = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_SOME, loc);
        e->some_expr.value = val;
        return e;
    }

    case TOK_NONE: {
        /* `none(T)` is sugar for `default(T?)`: it constructs an empty option
         * whose inner type is the parsed type.  We desugar at parse time by
         * wrapping the type in an option and emitting an EXPR_DEFAULT node, so
         * the two spellings produce identical ASTs and share all downstream
         * handling (type-checking, codegen, monomorphization, escape analysis).
         * The general `default(T?)` mechanism is retained unchanged.  A bare
         * `none` (e.g. `x == none`) carries no type and is rejected here with a
         * targeted message, mirroring the `void`/`void()` handling above. */
        advance_p(p);
        if (!check(p, TOK_LPAREN)) {
            diag_fatal(loc, "'none' needs a type argument; write none(T) "
                "(or test an option with .is_none / a 'none' match arm)");
        }
        advance_p(p); /* ( */
        Type *ty = parse_type(p);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_DEFAULT, loc);
        e->default_expr.target = type_option(p->arena, ty);
        return e;
    }

    case TOK_LOOP: {
        advance_p(p);
        int body_count;
        Expr **body = parse_block(p, &body_count);
        Expr *e = alloc_expr(p, EXPR_LOOP, loc);
        e->loop_expr.body = body;
        e->loop_expr.body_count = body_count;
        return e;
    }

    case TOK_GUARDED:
        advance_p(p);
        return parse_guard(p, true, loc);

    case TOK_UNGUARDED:
        advance_p(p);
        return parse_guard(p, false, loc);

    case TOK_FOR: {
        advance_p(p);
        const char *var = NULL;
        Pattern *var_pattern = NULL;
        const char *index_var = NULL;
        /* The element binding may be a destructure pattern (`{ ... }`); the
         * index, when present, is always a plain identifier. */
        if (check(p, TOK_LBRACE)) {
            var_pattern = parse_pattern(p);
        } else {
            var = tok_intern(p, expect(p, TOK_IDENT));
            if (check(p, TOK_COMMA)) {
                advance_p(p);
                /* first was index, second is element (ident or destructure) */
                index_var = var;
                var = NULL;
                if (check(p, TOK_LBRACE))
                    var_pattern = parse_pattern(p);
                else
                    var = tok_intern(p, expect(p, TOK_IDENT));
            }
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
        e->for_expr.var_pattern = var_pattern;
        e->for_expr.index_var = index_var;
        e->for_expr.iter = iter;
        e->for_expr.range_end = range_end;
        e->for_expr.body = body;
        e->for_expr.body_count = body_count;
        return e;
    }

    case TOK_VOID: {
        /* `void()` — the void-typed expression.  `void` alone (without the
         * trailing `()`) is still only valid in type position, so we require
         * the parentheses here and emit a targeted diagnostic otherwise. */
        advance_p(p);
        if (!check(p, TOK_LPAREN)) {
            diag_fatal(loc, "'void' is a type; use 'void()' to produce a void value");
        }
        advance_p(p); /* ( */
        if (!check(p, TOK_RPAREN)) {
            diag_fatal(loc_from_token(current(p)),
                "void() takes no arguments");
        }
        advance_p(p); /* ) */
        return alloc_expr(p, EXPR_VOID_LIT, loc);
    }

    case TOK_SIZEOF: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        p->allow_fixed_array = true;
        Type *ty = parse_type(p);
        p->allow_fixed_array = false;
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_SIZEOF, loc);
        e->sizeof_expr.target = ty;
        return e;
    }

    case TOK_ALIGNOF: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        p->allow_fixed_array = true;
        Type *ty = parse_type(p);
        p->allow_fixed_array = false;
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ALIGNOF, loc);
        e->alignof_expr.target = ty;
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
        Expr *operand = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_FREE, loc);
        e->free_expr.operand = operand;
        return e;
    }

    case TOK_ATOMIC_LOAD: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Expr *ptr = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ATOMIC_LOAD, loc);
        e->atomic_load.ptr = ptr;
        return e;
    }

    case TOK_ATOMIC_STORE: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Expr *ptr = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_COMMA);
        Expr *value = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ATOMIC_STORE, loc);
        e->atomic_store.ptr = ptr;
        e->atomic_store.value = value;
        return e;
    }

    case TOK_ASSERT: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        /* Capture source text of the condition expression */
        Token *cond_start = current(p);
        Expr *condition = parse_bracketed_expr(p, PREC_NONE + 1);
        /* Expression text: from cond_start to just before current token (, or )) */
        const char *text_start = cond_start->start;
        int text_len = (int)(current(p)->start - text_start);
        while (text_len > 0 && (text_start[text_len-1] == ' ' ||
               text_start[text_len-1] == '\n' || text_start[text_len-1] == '\r' ||
               text_start[text_len-1] == '\t'))
            text_len--;
        Expr *message = NULL;
        if (check(p, TOK_COMMA)) {
            advance_p(p);
            message = parse_bracketed_expr(p, PREC_NONE + 1);
        }
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ASSERT, loc);
        e->assert_expr.condition = condition;
        e->assert_expr.message = message;
        e->assert_expr.expr_text = arena_strdup(p->arena, text_start, text_len);
        e->assert_expr.expr_text_len = text_len;
        return e;
    }

    case TOK_ALLOC:
    case TOK_ALLOCA: {
        bool is_stack = (current(p)->kind == TOK_ALLOCA);
        advance_p(p);
        expect(p, TOK_LPAREN);

        /* Decide type vs expression without backtracking.
         * Built-in types, void, and type variables are syntactically unambiguous.
         * Unknown identifiers followed by [ are treated as alloc(T[N]).
         * Bare unknown identifiers (followed by )) are parsed as expressions —
         * pass2 disambiguates type names from variables.
         * Unknown identifiers followed by < need a tentative parse for generic
         * type args (the <> ambiguity is inherent to the grammar). */
        Token *first = current(p);
        bool is_type = (first->kind == TOK_VOID || first->kind == TOK_TYPE_VAR);
        bool try_type = false;
        int save = 0;

        if (!is_type && first->kind == TOK_IDENT) {
            if (type_from_name(first->start, first->length)) {
                is_type = true;
            } else {
                Token *next = peek_at(p, 1);
                if (next->kind == TOK_LBRACKET || next->kind == TOK_COMMA) {
                    is_type = true;
                } else if (next->kind == TOK_DOT) {
                    /* Could be module-qualified type — try type with backtracking */
                    try_type = true;
                    save = p->pos;
                } else if (next->kind == TOK_LT) {
                    /* Could be generic type args or comparison — try type */
                    try_type = true;
                    save = p->pos;
                }
            }
        }

        if (is_type || try_type) {
            Type *ty = parse_type(p);
            if (check(p, TOK_RPAREN)) {
                /* alloc(T) — bare type alloc */
                advance_p(p);
                Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                e->alloc_expr.alloc_type = ty;
                e->alloc_expr.size_expr = NULL;
                e->alloc_expr.init_expr = NULL;
                e->alloc_expr.is_stack = is_stack;
                return e;
            }
            if (check(p, TOK_LBRACKET)) {
                /* alloc(T[N] { elems }) — heap array allocation */
                advance_p(p);
                Expr *size = parse_bracketed_expr(p, PREC_NONE + 1);
                expect(p, TOK_RBRACKET);
                if (!check(p, TOK_LBRACE)) {
                    SrcLoc eloc = loc_from_token(current(p));
                    diag_fatal(eloc, "expected '{' after alloc(T[N] — use alloc(T[N] { })");
                }
                advance_p(p);
                Expr **elems = NULL;
                int elem_count = 0, elem_cap = 0;
                if (!check(p, TOK_RBRACE)) {
                    do {
                        Expr *el = parse_bracketed_expr(p, PREC_NONE + 1);
                        DA_APPEND(elems, elem_count, elem_cap, el);
                    } while (check(p, TOK_COMMA) && advance_p(p));
                }
                expect(p, TOK_RBRACE);
                expect(p, TOK_RPAREN);
                /* Runtime-sized alloc: alloc(T[n] { }) where n is not a literal */
                if (size->kind != EXPR_INT_LIT) {
                    if (elem_count > 0) {
                        diag_fatal(loc, "alloc with runtime size cannot have explicit elements");
                    }
                    free(elems);
                    Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                    e->alloc_expr.alloc_type = ty;
                    e->alloc_expr.size_expr = size;
                    e->alloc_expr.init_expr = NULL;
                    e->alloc_expr.is_stack = is_stack;
                    return e;
                }
                Expr **arena_elems = arena_alloc(p->arena,
                    sizeof(Expr*) * (size_t)(elem_count > 0 ? elem_count : 1));
                if (elem_count > 0)
                    memcpy(arena_elems, elems, sizeof(Expr*) * (size_t)elem_count);
                free(elems);
                Expr *arr = alloc_expr(p, EXPR_ARRAY_LIT, loc);
                arr->array_lit.elem_type = ty;
                arr->array_lit.size_expr = size;
                arr->array_lit.elems = arena_elems;
                arr->array_lit.elem_count = elem_count;
                Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                e->alloc_expr.alloc_type = NULL;
                e->alloc_expr.size_expr = NULL;
                e->alloc_expr.init_expr = arr;
                e->alloc_expr.is_stack = is_stack;
                return e;
            }
            if (check(p, TOK_COMMA)) {
                /* alloc(T, N) — raw buffer alloc */
                advance_p(p);
                Expr *size = parse_bracketed_expr(p, PREC_NONE + 1);
                expect(p, TOK_RPAREN);
                Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                e->alloc_expr.alloc_type = ty;
                e->alloc_expr.size_expr = size;
                e->alloc_expr.init_expr = NULL;
                e->alloc_expr.alloc_raw = true;
                e->alloc_expr.is_stack = is_stack;
                return e;
            }
            if (try_type) {
                /* Generic type args didn't pan out — backtrack */
                restore_pos(p, save);
            } else {
                diag_fatal(loc, "expected ')', '[', or ',' after type in alloc");
            }
        }

        /* alloc(expr) / alloca(expr) — initialized alloc. An interpolated string
         * init is "wrapped" and a bare (cstr) cast init is "licensed": either licenses
         * an otherwise-illegal unbounded (runtime-sized) str→cstr conversion that the
         * wrapping alloc/alloca gives a home (heap / dynamic stack). */
        Expr *init = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        if (init->kind == EXPR_INTERP_STRING)
            init->interp_string.wrapped = true;
        else if (init->kind == EXPR_CAST && init->cast.buffer_size == 0)
            init->cast.licensed = true;
        Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
        e->alloc_expr.alloc_type = NULL;
        e->alloc_expr.size_expr = NULL;
        e->alloc_expr.init_expr = init;
        e->alloc_expr.is_stack = is_stack;
        return e;
    }

    /* <'a, 'b>(params) -> body : generic function literal with explicit type vars */
    case TOK_LT: {
        if (peek_at(p, 1)->kind != TOK_TYPE_VAR) {
            diag_fatal(loc, "unexpected '<' in expression");
        }
        advance_p(p); /* consume < */
        const char **tvars = NULL;
        int tv_count = 0, tv_cap = 0;
        do {
            Token *tv = expect(p, TOK_TYPE_VAR);
            const char *tvname = tok_intern(p, tv);
            DA_APPEND(tvars, tv_count, tv_cap, tvname);
            if (!check(p, TOK_COMMA)) break;
            advance_p(p);
        } while (1);
        expect(p, TOK_GT);
        /* Parse the following function literal */
        Expr *func = parse_func_literal(p);
        func->func.explicit_type_vars = arena_alloc(p->arena, sizeof(const char*) * (size_t)tv_count);
        memcpy(func->func.explicit_type_vars, tvars, sizeof(const char*) * (size_t)tv_count);
        func->func.explicit_type_var_count = tv_count;
        free(tvars);
        return func;
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

    case TOK_TYPE_VAR: {
        /* Slice literal with a type-variable element type: 'a[N] { e0, ... }
         * (or 'a[] { ptr =, len = }, and 'a?/'a* element-type suffixes). The
         * element type is the abstract type var inside a generic body; it is
         * substituted per-monomorphization. Mirrors the IDENT slice-literal
         * lookahead: a bare 'a[expr] with no following '{' is not a value and
         * falls through to the type-var-ref path, which pass2 rejects. */
        {
            int arr_start = 1; /* offset past the type-var token */
            while (peek_at(p, arr_start)->kind == TOK_QUESTION ||
                   peek_at(p, arr_start)->kind == TOK_STAR)
                arr_start += 1;
            if (peek_at(p, arr_start)->kind == TOK_LBRACKET) {
                int save = p->pos;
                for (int skip = 0; skip < arr_start + 1; skip++) advance_p(p);
                int depth = 1;
                while (depth > 0 && !at_end_p(p)) {
                    if (check(p, TOK_LBRACKET)) depth++;
                    if (check(p, TOK_RBRACKET)) depth--;
                    advance_p(p);
                }
                bool is_array_lit = check(p, TOK_LBRACE);
                restore_pos(p, save);
                if (is_array_lit) {
                    advance_p(p); /* consume the type-var token */
                    Type *elem_type = arena_alloc(p->arena, sizeof(Type));
                    elem_type->kind = TYPE_TYPE_VAR;
                    elem_type->type_var.name = tok_intern(p, t);
                    while (check(p, TOK_QUESTION) || check(p, TOK_STAR)) {
                        if (check(p, TOK_QUESTION)) {
                            advance_p(p);
                            elem_type = type_option(p->arena, elem_type);
                        } else {
                            advance_p(p);
                            elem_type = type_pointer(p->arena, elem_type);
                        }
                    }
                    return parse_array_lit_body(p, elem_type, loc);
                }
            }
        }
        /* Allow 'a in expression position for type-variable property access ('a.min etc.) */
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_TYPE_VAR_REF, loc);
        e->type_var_ref.name = tok_intern(p, t);
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
        /* Capture source text of the operand expression for unwrap diagnostics */
        const char *text_start = p->tokens[p->expr_start_pos].start;
        int text_len = (int)(op_tok->start - text_start);
        while (text_len > 0 && (text_start[text_len-1] == ' ' ||
               text_start[text_len-1] == '\n' || text_start[text_len-1] == '\r' ||
               text_start[text_len-1] == '\t'))
            text_len--;
        e->unary_postfix.expr_text = arena_strdup(p->arena, text_start, text_len);
        e->unary_postfix.expr_text_len = text_len;
        return e;
    }

    case TOK_DOT: {
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
            if (!check(p, TOK_RBRACKET)) hi = parse_bracketed_expr(p, PREC_NONE + 1);
            expect(p, TOK_RBRACKET);
            Expr *e = alloc_expr(p, EXPR_SLICE, loc);
            e->slice.object = left;
            e->slice.lo = NULL;
            e->slice.hi = hi;
            return e;
        }
        Expr *index = parse_bracketed_expr(p, PREC_NONE + 1);
        if (check(p, TOK_DOTDOT)) {
            advance_p(p);
            Expr *hi = NULL;
            if (!check(p, TOK_RBRACKET)) hi = parse_bracketed_expr(p, PREC_NONE + 1);
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
                Expr *arg = parse_bracketed_expr(p, PREC_NONE + 1);
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

    case TOK_LT: {
        /* Check if this is a generic call: left<Type, ...>(args)
         * The '<' token has already been consumed by the Pratt loop.
         * Scan forward to see if tokens between here and '>' are all
         * type-compatible, and '>' is followed by '('. */
        if (left->kind == EXPR_IDENT || left->kind == EXPR_FIELD) {
            if (generic_call_scan(p, p->pos)) {
                /* Parse type args */
                Type **type_args = NULL;
                int ta_count = 0, ta_cap = 0;
                do {
                    Type *ty = parse_type(p);
                    DA_APPEND(type_args, ta_count, ta_cap, ty);
                    if (!check(p, TOK_COMMA)) break;
                    advance_p(p);
                } while (1);
                expect_typearg_gt(p);

                if (check(p, TOK_DOT)) {
                    /* name<Types>.variant — generic union variant construction */
                    advance_p(p); /* consume '.' */
                    Token *field = expect(p, TOK_IDENT);
                    const char *vname = tok_intern(p, field);

                    /* Store type_args on left (the union name ident) for pass2 */
                    /* Build EXPR_FIELD: left.variant */
                    Expr *fld = alloc_expr(p, EXPR_FIELD, loc);
                    fld->field.object = left;
                    fld->field.name = vname;

                    /* Store type args on the field expr for pass2 to resolve */
                    fld->field.type_args = arena_alloc(p->arena, sizeof(Type*) * (size_t)ta_count);
                    memcpy(fld->field.type_args, type_args, sizeof(Type*) * (size_t)ta_count);
                    fld->field.type_arg_count = ta_count;

                    if (check(p, TOK_LPAREN)) {
                        /* Payload variant: name<Types>.variant(arg) */
                        advance_p(p);
                        Expr **args = NULL;
                        int arg_count = 0, arg_cap = 0;
                        if (!check(p, TOK_RPAREN)) {
                            do {
                                Expr *arg = parse_bracketed_expr(p, PREC_NONE + 1);
                                DA_APPEND(args, arg_count, arg_cap, arg);
                                if (!check(p, TOK_COMMA)) break;
                                advance_p(p);
                            } while (1);
                        }
                        expect(p, TOK_RPAREN);

                        Expr *e = alloc_expr(p, EXPR_CALL, loc);
                        e->call.func = fld;
                        e->call.args = arena_copy_exprs(p, args, arg_count);
                        e->call.arg_count = arg_count;
                        e->call.type_args = arena_alloc(p->arena, sizeof(Type*) * (size_t)ta_count);
                        memcpy(e->call.type_args, type_args, sizeof(Type*) * (size_t)ta_count);
                        e->call.type_arg_count = ta_count;
                        free(type_args);
                        free(args);
                        return e;
                    }
                    /* No-payload variant: name<Types>.variant */
                    /* Store type args on the field expr for pass2 to pick up */
                    fld->field.type_args = arena_alloc(p->arena, sizeof(Type*) * (size_t)ta_count);
                    memcpy(fld->field.type_args, type_args, sizeof(Type*) * (size_t)ta_count);
                    fld->field.type_arg_count = ta_count;
                    free(type_args);
                    return fld;
                }

                /* Generic function call: name<Types>(args) */
                expect(p, TOK_LPAREN);

                /* Parse call arguments */
                Expr **args = NULL;
                int arg_count = 0, arg_cap = 0;
                if (!check(p, TOK_RPAREN)) {
                    do {
                        Expr *arg = parse_bracketed_expr(p, PREC_NONE + 1);
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
                e->call.type_args = arena_alloc(p->arena, sizeof(Type*) * (size_t)ta_count);
                memcpy(e->call.type_args, type_args, sizeof(Type*) * (size_t)ta_count);
                e->call.type_arg_count = ta_count;
                free(type_args);
                free(args);
                return e;
            }
            if (bare_inst_scan(p, p->pos)) {
                /* `name<Types>` with no '(' — explicit type args in value
                 * position. Parse the type args so they are consumed, then build
                 * a marked EXPR_CALL with no arguments; pass2 rejects it with a
                 * located diagnostic (a generic function/type cannot be a value). */
                Type **type_args = NULL;
                int ta_count = 0, ta_cap = 0;
                do {
                    Type *ty = parse_type(p);
                    DA_APPEND(type_args, ta_count, ta_cap, ty);
                    if (!check(p, TOK_COMMA)) break;
                    advance_p(p);
                } while (1);
                expect_typearg_gt(p);

                Expr *e = alloc_expr(p, EXPR_CALL, loc);
                e->call.func = left;
                e->call.args = NULL;
                e->call.arg_count = 0;
                e->call.type_args = arena_alloc(p->arena, sizeof(Type*) * (size_t)ta_count);
                memcpy(e->call.type_args, type_args, sizeof(Type*) * (size_t)ta_count);
                e->call.type_arg_count = ta_count;
                e->call.bare_inst = true;
                free(type_args);
                return e;
            }
        }
        /* Not a generic call — fall through to comparison */
        Prec prec = infix_prec(op);
        Expr *right = parse_expr(p, prec + 1);
        Expr *e = alloc_expr(p, EXPR_BINARY, loc);
        e->binary.op = op;
        e->binary.left = left;
        e->binary.right = right;
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
    int saved_start = p->expr_start_pos;
    p->expr_start_pos = p->pos;
    Expr *left = parse_prefix(p);
    for (;;) {
        Token *t = current(p);
        /* Inside a `when` guard, treat top-level `->` as the arm-body separator
           rather than as a pointer-field postfix. Bracketed sub-expressions
           clear this flag so pointer-field access still works when wrapped. */
        if (p->block_arm_arrow && t->kind == TOK_ARROW) break;
        Prec prec = infix_prec(t->kind);
        /* A generic call name<T,...>(args) is a call, so its '<' binds at
         * call precedence — not comparison. Without the bump the callee gets
         * absorbed as an operand of any tighter-binding operator on its left
         * (`1 + size_of<int32>()`, `!is_big<int32>()`) and the '<' is never
         * reached with the bare name as `left`. */
        if (t->kind == TOK_LT &&
            (left->kind == EXPR_IDENT || left->kind == EXPR_FIELD) &&
            generic_call_scan(p, p->pos + 1))
            prec = PREC_POSTFIX;
        if (prec == PREC_NONE || prec < min_prec) break;
        Token *op_tok = advance_p(p);
        left = parse_infix(p, left, op_tok);
    }
    p->expr_start_pos = saved_start;
    return left;
}

/* ---- Pattern parsing ---- */

static Pattern *parse_pattern(Parser *p);

static Pattern *parse_pattern_atom(Parser *p) {
    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->loc = loc_from_token(current(p));
    pat->loc.filename = p->filename;

    /* Negative integer pattern: -42 */
    if (check(p, TOK_MINUS) && peek_at(p, 1)->kind == TOK_INT_LIT) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p); /* consume - */
        Token *t = advance_p(p);
        Type *lt = parse_int_type(t->start, t->length);
        if (type_is_unsigned(lt))
            diag_fatal(loc, "cannot negate unsigned integer literal");
        pat->kind = PAT_INT_LIT;
        bool pat_oor = false;
        pat->int_lit.value = -parse_int_value(t->start, t->length, &pat_oor);
        pat->int_lit.lit_type = lt;
        pat->int_lit.out_of_range = pat_oor;
        pat->int_lit.negative = true;
        return pat;
    }

    if (check(p, TOK_INT_LIT)) {
        Token *t = advance_p(p);
        pat->kind = PAT_INT_LIT;
        bool pat_oor = false;
        pat->int_lit.value = parse_int_value(t->start, t->length, &pat_oor);
        pat->int_lit.lit_type = parse_int_type(t->start, t->length);
        pat->int_lit.out_of_range = pat_oor;
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

    if (check(p, TOK_CHAR_LIT)) {
        Token *t2 = advance_p(p);
        pat->kind = PAT_CHAR_LIT;
        pat->char_lit.value = parse_char_value(t2->start, t2->length);
        return pat;
    }

    if (check(p, TOK_STRING_LIT)) {
        Token *t2 = advance_p(p);
        pat->kind = PAT_STRING_LIT;
        pat->string_lit.value = t2->start + 1;  /* skip opening quote */
        pat->string_lit.length = t2->length - 2; /* skip both quotes */
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

    if (check(p, TOK_LBRACE)) {
        /* Named struct destructuring `{ f = p, ... }` vs positional tuple
         * destructuring `{ a, b, ... }`. Named requires `IDENT =` as the first
         * element; the empty `{}` stays a 0-field struct pattern. Anything else
         * (a leading binding/wildcard/literal/nested pattern not followed by `=`)
         * is positional. */
        bool is_named = peek_at(p, 1)->kind == TOK_RBRACE ||
                        (peek_at(p, 1)->kind == TOK_IDENT && peek_at(p, 2)->kind == TOK_EQ);
        advance_p(p); /* consume { */
        if (is_named) {
            pat->kind = PAT_STRUCT;
            FieldPattern *fields = NULL;
            int count = 0, cap = 0;
            if (!check(p, TOK_RBRACE)) {
                do {
                    skip_newlines(p);
                    const char *fname = tok_intern(p, expect(p, TOK_IDENT));
                    expect(p, TOK_EQ);
                    Pattern *inner = parse_pattern(p);
                    FieldPattern fp;
                    fp.name = fname;
                    fp.pattern = inner;
                    DA_APPEND(fields, count, cap, fp);
                    if (!check(p, TOK_COMMA)) break;
                    advance_p(p);
                } while (!check(p, TOK_RBRACE));
            }
            skip_newlines(p);
            expect(p, TOK_RBRACE);
            pat->struc.fields = arena_alloc(p->arena, sizeof(FieldPattern) * (count > 0 ? (size_t)count : 1));
            memcpy(pat->struc.fields, fields, sizeof(FieldPattern) * (size_t)count);
            pat->struc.field_count = count;
            free(fields);
            return pat;
        }
        /* Positional tuple pattern */
        pat->kind = PAT_TUPLE;
        Pattern **pats = NULL;
        int count = 0, cap = 0;
        do {
            skip_newlines(p);
            Pattern *inner = parse_pattern(p);
            DA_APPEND(pats, count, cap, inner);
            if (!check(p, TOK_COMMA)) break;
            advance_p(p);
        } while (!check(p, TOK_RBRACE));
        skip_newlines(p);
        expect(p, TOK_RBRACE);
        pat->tuple_pat.patterns = arena_alloc(p->arena, sizeof(Pattern*) * (count > 0 ? (size_t)count : 1));
        memcpy(pat->tuple_pat.patterns, pats, sizeof(Pattern*) * (size_t)count);
        pat->tuple_pat.pattern_count = count;
        pat->tuple_pat.resolved_types = NULL;
        free(pats);
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

/* parse_pattern handles or-patterns by collecting alternatives joined by `|`.
   Nested ORs are flattened: (a | b) | c parses as a single 3-alt PAT_OR. */
static Pattern *parse_pattern(Parser *p) {
    Pattern *first = parse_pattern_atom(p);
    if (!check(p, TOK_PIPE)) return first;

    /* Accumulate alternatives, flattening any inner PAT_OR. */
    Pattern **alts = NULL;
    int count = 0, cap = 0;
    if (first->kind == PAT_OR) {
        for (int i = 0; i < first->or_pat.alt_count; i++)
            DA_APPEND(alts, count, cap, first->or_pat.alts[i]);
    } else {
        DA_APPEND(alts, count, cap, first);
    }
    SrcLoc or_loc = first->loc;

    while (check(p, TOK_PIPE)) {
        advance_p(p); /* consume | */
        Pattern *alt = parse_pattern_atom(p);
        if (alt->kind == PAT_OR) {
            for (int i = 0; i < alt->or_pat.alt_count; i++)
                DA_APPEND(alts, count, cap, alt->or_pat.alts[i]);
        } else {
            DA_APPEND(alts, count, cap, alt);
        }
    }

    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->kind = PAT_OR;
    pat->loc = or_loc;
    pat->or_pat.alts = arena_alloc(p->arena, sizeof(Pattern *) * (size_t)count);
    memcpy(pat->or_pat.alts, alts, sizeof(Pattern *) * (size_t)count);
    pat->or_pat.alt_count = count;
    free(alts);
    return pat;
}

/* ---- Match expression parsing ---- */

/* Combine a list of buffered body-less or-alternatives with the current arm's
   pattern, producing a flattened PAT_OR. The buffered patterns are stored
   pre-flattened (each is either a non-OR pattern or a PAT_OR whose alts are
   already flat). */
static Pattern *or_prepend(Parser *p, Pattern **buffered, int buf_count, Pattern *tail) {
    Pattern **alts = NULL;
    int count = 0, cap = 0;
    for (int i = 0; i < buf_count; i++) {
        Pattern *b = buffered[i];
        if (b->kind == PAT_OR) {
            for (int j = 0; j < b->or_pat.alt_count; j++)
                DA_APPEND(alts, count, cap, b->or_pat.alts[j]);
        } else {
            DA_APPEND(alts, count, cap, b);
        }
    }
    if (tail->kind == PAT_OR) {
        for (int j = 0; j < tail->or_pat.alt_count; j++)
            DA_APPEND(alts, count, cap, tail->or_pat.alts[j]);
    } else {
        DA_APPEND(alts, count, cap, tail);
    }
    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->kind = PAT_OR;
    pat->loc = buffered[0]->loc;
    pat->or_pat.alts = arena_alloc(p->arena, sizeof(Pattern *) * (size_t)count);
    memcpy(pat->or_pat.alts, alts, sizeof(Pattern *) * (size_t)count);
    pat->or_pat.alt_count = count;
    free(alts);
    return pat;
}

static Expr *parse_match_expr(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    expect(p, TOK_MATCH);

    /* Save the outer `block_arm_arrow` state: a match nested inside another
       match's guard must still be able to parse its own subject / guards /
       arm bodies without the outer guard's `->`-blocker interfering. */
    bool saved_block = p->block_arm_arrow;
    p->block_arm_arrow = false;

    Expr *subject = parse_expr(p, PREC_NONE + 1);
    expect(p, TOK_WITH);

    /* Expect INDENT then a series of | pattern -> body */
    expect(p, TOK_INDENT);

    MatchArm *arms = NULL;
    int arm_count = 0, arm_cap = 0;

    /* Fall-through buffer: when an arm has no `->`, its pattern is buffered
       as an or-alternative to be folded into the next arm that does. */
    Pattern **buffered = NULL;
    int buf_count = 0, buf_cap = 0;
    SrcLoc last_body_less_loc = {0};

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        expect(p, TOK_PIPE);
        MatchArm arm;
        arm.loc = loc_from_token(current(p));
        arm.loc.filename = p->filename;
        arm.pattern = parse_pattern(p);
        arm.guard = NULL;

        /* Optional `when <expr>` guard. The guard attaches to the full
           or-pattern (any body-less arms buffered above plus this arm's
           pattern), so it's only permitted on an arm that ends with `->`. */
        if (check(p, TOK_WHEN)) {
            SrcLoc when_loc = loc_from_token(current(p));
            advance_p(p);
            p->block_arm_arrow = true;
            arm.guard = parse_expr(p, PREC_NONE + 1);
            p->block_arm_arrow = false;
            if (!check(p, TOK_ARROW)) {
                diag_fatal(when_loc,
                    "'when' guard requires an arm body: expected '->' after guard expression");
            }
        }

        if (!check(p, TOK_ARROW)) {
            /* Body-less arm — buffer as or-alternative for the next arm. */
            last_body_less_loc = arm.loc;
            DA_APPEND(buffered, buf_count, buf_cap, arm.pattern);
            continue;
        }

        if (buf_count > 0) {
            arm.pattern = or_prepend(p, buffered, buf_count, arm.pattern);
            buf_count = 0;
        }

        advance_p(p); /* consume -> */

        /* Parse arm body */
        arm.body = parse_body(p, &arm.body_count);

        DA_APPEND(arms, arm_count, arm_cap, arm);
        skip_newlines(p);
    }
    if (buf_count > 0) {
        diag_fatal(last_body_less_loc,
                   "unterminated or-pattern: body-less arm must be followed by an arm with '->'");
    }
    free(buffered);
    expect(p, TOK_DEDENT);

    p->block_arm_arrow = saved_block;

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
            Expr *val = parse_bracketed_expr(p, PREC_NONE + 1);
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
        p->allow_fixed_array = true;
        Type *ftype = parse_type(p);
        p->allow_fixed_array = false;

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

    /* Optional from "lib" clause */
    const char *from_lib = NULL;
    if (check(p, TOK_FROM)) {
        advance_p(p);
        Token *lib_tok = expect(p, TOK_STRING_LIT);
        from_lib = intern(p->intern, lib_tok->start + 1, lib_tok->length - 2);
    }

    /* Optional define "MACRO" "VALUE" clause (only valid with from) */
    const char *define_macro = NULL;
    const char *define_value = NULL;
    if (from_lib && check(p, TOK_IDENT) &&
        current(p)->length == 6 &&
        memcmp(current(p)->start, "define", 6) == 0) {
        advance_p(p);
        Token *macro_tok = expect(p, TOK_STRING_LIT);
        define_macro = intern(p->intern, macro_tok->start + 1, macro_tok->length - 2);
        Token *value_tok = expect(p, TOK_STRING_LIT);
        define_value = intern(p->intern, value_tok->start + 1, value_tok->length - 2);
    }

    expect(p, TOK_EQ);

    /* Parse body: INDENT { decl } DEDENT */
    expect(p, TOK_INDENT);

    Decl **decls = NULL;
    int count = 0, cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;
        Decl *child = parse_decl(p);
        DA_APPEND(decls, count, cap, child);
        /* Drain any pending decls from multi-symbol imports */
        while (p->pending_count > 0) {
            DA_APPEND(decls, count, cap, p->pending_decls[--p->pending_count]);
        }
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_MODULE;
    d->loc = loc;
    d->is_private = false;
    d->module.name = name;
    d->module.ns_prefix = NULL;
    d->module.from_lib = from_lib;
    d->module.define_macro = define_macro;
    d->module.define_value = define_value;
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

/* Parse a from clause: from [namespace::path::]module
 * Sets *out_ns and *out_mod. */
static void parse_from_clause(Parser *p, const char **out_ns, const char **out_mod) {
    /* Parse IDENT [:: IDENT [:: ...]]
     * If path ends with ::, last part is namespace; out_mod is the next IDENT (or NULL for bare ns).
     * If path does NOT end with ::, last IDENT is the module name. */
    const char *first = tok_intern(p, expect(p, TOK_IDENT));

    if (!check(p, TOK_COLONCOLON)) {
        /* Simple: from module_name */
        *out_ns = NULL;
        *out_mod = first;
        return;
    }

    /* Build namespace path: ident::ident::... */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", first);

    while (check(p, TOK_COLONCOLON)) {
        advance_p(p); /* consume :: */
        if (check(p, TOK_IDENT)) {
            const char *part = tok_intern(p, expect(p, TOK_IDENT));
            if (!check(p, TOK_COLONCOLON)) {
                /* This IDENT is NOT followed by ::, so it's the module name */
                *out_ns = intern_cstr(p->intern, buf);
                *out_mod = part;
                return;
            }
            /* More :: follows — this is still namespace.
             * Use __ to separate segments so foo::bar != foo_bar. */
            size_t cur = strlen(buf);
            snprintf(buf + cur, sizeof(buf) - cur, "__%s", part);
        } else {
            /* Bare namespace ending: from acme:: or from acme::graphics:: */
            *out_ns = intern_cstr(p->intern, buf);
            *out_mod = NULL;
            return;
        }
    }

    /* Shouldn't reach here, but just in case */
    *out_ns = intern_cstr(p->intern, buf);
    *out_mod = NULL;
}

static Decl *parse_import_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_IMPORT);

    /* import * from [ns::]MODULE */
    if (check(p, TOK_STAR)) {
        advance_p(p);
        expect(p, TOK_FROM);
        const char *from_ns = NULL, *from_mod = NULL;
        parse_from_clause(p, &from_ns, &from_mod);
        Decl *d = arena_alloc(p->arena, sizeof(Decl));
        d->kind = DECL_IMPORT;
        d->loc = loc;
        d->is_private = false;
        d->import.name = NULL;
        d->import.alias = NULL;
        d->import.from_module = from_mod;
        d->import.from_namespace = from_ns;
        d->import.is_wildcard = true;
        return d;
    }

    /* Parse first name [as alias] */
    const char *name = tok_intern(p, expect(p, TOK_IDENT));
    const char *alias = NULL;

    /* Check for import module.submodule syntax */
    if (check(p, TOK_DOT) && !check(p, TOK_AS) && !check(p, TOK_FROM) && !check(p, TOK_COMMA)) {
        /* import module_a.module_b → equivalent to import module_b from module_a */
        advance_p(p); /* consume . */
        const char *sub = tok_intern(p, expect(p, TOK_IDENT));
        Decl *d = arena_alloc(p->arena, sizeof(Decl));
        d->kind = DECL_IMPORT;
        d->loc = loc;
        d->is_private = false;
        d->import.name = sub;
        d->import.alias = NULL;
        d->import.from_module = name;
        d->import.from_namespace = NULL;
        d->import.is_wildcard = false;
        return d;
    }

    if (check(p, TOK_AS)) {
        advance_p(p);
        alias = tok_intern(p, expect(p, TOK_IDENT));
    }

    /* Check for multi-symbol import: name1 [as a1], name2 [as a2], ... from mod */
    if (check(p, TOK_COMMA)) {
        /* Collect all name/alias pairs */
        typedef struct { const char *n; const char *a; } ImportItem;
        ImportItem *items = NULL;
        int item_count = 0, item_cap = 0;

        ImportItem first = { name, alias };
        DA_APPEND(items, item_count, item_cap, first);

        while (check(p, TOK_COMMA)) {
            advance_p(p);
            const char *n = tok_intern(p, expect(p, TOK_IDENT));
            const char *a = NULL;
            if (check(p, TOK_AS)) {
                advance_p(p);
                a = tok_intern(p, expect(p, TOK_IDENT));
            }
            ImportItem it = { n, a };
            DA_APPEND(items, item_count, item_cap, it);
        }

        expect(p, TOK_FROM);
        const char *from_ns = NULL, *from_mod = NULL;
        parse_from_clause(p, &from_ns, &from_mod);

        /* Create import decl for item 0 (returned), push rest to pending */
        for (int i = 1; i < item_count; i++) {
            Decl *extra = arena_alloc(p->arena, sizeof(Decl));
            extra->kind = DECL_IMPORT;
            extra->loc = loc;
            extra->is_private = false;
            extra->import.name = items[i].n;
            extra->import.alias = items[i].a;
            extra->import.from_module = from_mod;
            extra->import.from_namespace = from_ns;
            extra->import.is_wildcard = false;
            DA_APPEND(p->pending_decls, p->pending_count, p->pending_cap, extra);
        }

        Decl *d = arena_alloc(p->arena, sizeof(Decl));
        d->kind = DECL_IMPORT;
        d->loc = loc;
        d->is_private = false;
        d->import.name = items[0].n;
        d->import.alias = items[0].a;
        d->import.from_module = from_mod;
        d->import.from_namespace = from_ns;
        d->import.is_wildcard = false;
        free(items);
        return d;
    }

    /* Single import with optional from clause */
    const char *from_module = NULL;
    const char *from_ns = NULL;

    if (check(p, TOK_FROM)) {
        advance_p(p);
        parse_from_clause(p, &from_ns, &from_module);
    }

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_IMPORT;
    d->loc = loc;
    d->is_private = false;
    d->import.name = name;
    d->import.alias = alias;
    d->import.from_module = from_module;
    d->import.from_namespace = from_ns;
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
            snprintf(buf + cur, sizeof(buf) - cur, "__%s", part);
        }
    }

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_NAMESPACE;
    d->loc = loc;
    d->is_private = false;
    d->ns.name = intern_cstr(p->intern, buf);
    return d;
}

static Decl *parse_extern_decl(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    loc.filename = p->filename;
    expect(p, TOK_EXTERN);

    /* extern struct/union — C struct or union layout import */
    bool is_c_union = check(p, TOK_UNION);
    if (check(p, TOK_STRUCT) || is_c_union) {
        advance_p(p);
        const char *c_name = tok_intern(p, expect(p, TOK_IDENT));
        const char *fc_name = c_name;
        if (check(p, TOK_AS)) {
            advance_p(p);
            fc_name = tok_intern(p, expect(p, TOK_IDENT));
        }
        expect(p, TOK_EQ);
        expect(p, TOK_INDENT);
        StructField *fields = NULL;
        int field_count = 0, field_cap = 0;
        while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
            skip_newlines(p);
            if (check(p, TOK_DEDENT)) break;
            const char *fname = tok_intern(p, expect(p, TOK_IDENT));
            expect(p, TOK_COLON);
            p->allow_fixed_array = true;
            Type *ftype = parse_type(p);
            p->allow_fixed_array = false;
            StructField f = { .name = fname, .type = ftype };
            DA_APPEND(fields, field_count, field_cap, f);
            skip_newlines(p);
        }
        expect(p, TOK_DEDENT);
        Decl *d = arena_alloc(p->arena, sizeof(Decl));
        d->kind = DECL_STRUCT;
        d->loc = loc;
        d->is_private = false;
        d->struc.name = fc_name;
        d->struc.c_name = c_name;
        d->struc.is_extern = true;
        d->struc.is_c_union = is_c_union;
        d->struc.is_generic = false;
        d->struc.type_params = NULL;
        d->struc.type_param_count = 0;
        d->struc.field_count = field_count;
        if (field_count > 0) {
            d->struc.fields = arena_alloc(p->arena, sizeof(StructField) * (size_t)field_count);
            memcpy(d->struc.fields, fields, sizeof(StructField) * (size_t)field_count);
            free(fields);
        } else {
            d->struc.fields = NULL;
        }
        return d;
    }

    /* extern function declaration */
    Token *name_tok = expect_extern_c_name(p);
    const char *name = tok_intern(p, name_tok);
    const char *alias = NULL;
    if (check(p, TOK_AS)) {
        advance_p(p);
        alias = tok_intern(p, expect(p, TOK_IDENT));
    }
    /* A reserved-identifier C name (free, default, sizeof, ...) is accepted only
     * with an alias: the bare name is a keyword in FC and would be unreferenceable.
     * Require `extern <name> as <ident>: ...` in that case. */
    if (!alias && name_tok->kind != TOK_IDENT) {
        diag_fatal(loc, "extern declaration uses reserved name '%s', which would be "
            "unreferenceable; give it an alias: `extern %s as <name>: ...`", name, name);
    }
    expect(p, TOK_COLON);
    Type *type = parse_type(p);
    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_EXTERN;
    d->loc = loc;
    d->is_private = false;
    d->ext.name = name;
    d->ext.alias = alias;
    d->ext.type = type;
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
    if (check(p, TOK_EXTERN)) return parse_extern_decl(p);

    SrcLoc loc = loc_from_token(current(p));
    diag_fatal(loc, "expected declaration, got %s",
        token_kind_name(current(p)->kind));
}

Program *parse_program(Parser *p) {
    Decl **decls = NULL;
    int count = 0, cap = 0;
    bool seen_non_ns = false;
    skip_newlines(p);
    while (!at_end_p(p)) {
        Decl *d = parse_decl(p);
        if (d->kind == DECL_NAMESPACE && seen_non_ns) {
            diag_fatal(d->loc, "namespace declaration must be the first line of the file");
        }
        if (d->kind != DECL_NAMESPACE) seen_non_ns = true;
        DA_APPEND(decls, count, cap, d);
        /* Drain any pending decls from multi-symbol imports */
        while (p->pending_count > 0) {
            DA_APPEND(decls, count, cap, p->pending_decls[--p->pending_count]);
        }
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
