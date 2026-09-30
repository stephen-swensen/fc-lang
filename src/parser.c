#include "parser.h"
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>

typedef struct Parser {
    Token *tokens;
    int token_count;
    int pos;
    Arena *arena;
    InternTable *intern;
    const char *filename;   /* source filename for SrcLoc */
    bool allow_fixed_array; /* the type being parsed may be a fixed array T[N]: a struct
                               field type or a sizeof/alignof operand */
    bool in_const_expr;     /* true inside a const-expression slot (a generic <...> argument or a
                               fixed-array size), including a parenthesized part, which accepts
                               any expression. Const expressions evaluate in the i64 domain, so
                               an unsuffixed integer literal is i64 there rather than the i32
                               expression default. */
    bool block_arm_arrow;   /* true while parsing a match-arm `when` guard, where a top-level
                               `->` ends the guard; cleared inside brackets, where a `->`
                               cannot be the arm's separator */
    int expr_start_pos;     /* token index at start of current parse_expr (for the operand
                               text of a postfix !) */
    int expr_start_errs;    /* diag_error_count() at start of current parse_expr; a change means
                               error recovery ran, so token pointers may span buffers and text
                               capture is skipped (the text only feeds codegen, which never runs
                               with errors) */
    bool half_gt;           /* the first '>' of a '>>' (TOK_GTGT) has been consumed as a
                               type-argument closer; the parser is parked on the token
                               awaiting the second */
    int half_gt_pos;        /* token index of that '>>' (valid iff half_gt) */
    /* Whole-program set of names that may head a generic instantiation
     * (`name<...>` in expression position), collected from every file's tokens
     * before parsing (parser_collect_generic_names). The expression-position
     * `<` scans claim a type-argument reading only for these names, so a
     * comparison like `f(a < 2, b > (c))` keeps its comparison reading when
     * the `<`'s left operand is not a generic declaration. Interned pointers. */
    const char **generic_names;
    int generic_name_count;
} Parser;


/* ---- Generic-name pre-pass (see parser.h) ---- */

static void gn_add(const char ***names, int *n, int *cap, const char *name) {
    for (int i = 0; i < *n; i++)
        if ((*names)[i] == name) return;
    DA_APPEND(*names, *n, *cap, name);
}

/* Does the token range [i, end) contain a type variable ('a)? */
static bool gn_range_has_type_var(Token *toks, int i, int end) {
    for (; i < end; i++)
        if (toks[i].kind == TOK_TYPE_VAR) return true;
    return false;
}

void parser_collect_generic_names(Token *toks, int count, InternTable *it,
                                  const char ***names, int *n, int *cap) {
    for (int i = 0; i < count; i++) {
        switch (toks[i].kind) {
        case TOK_STRUCT: case TOK_UNION: {
            /* struct NAME = INDENT body DEDENT: generic iff the body mentions
             * a type variable (the evidence pass1's detection reads). */
            if (i + 2 >= count || toks[i + 1].kind != TOK_IDENT ||
                toks[i + 2].kind != TOK_EQ)
                break;
            int j = i + 3;
            while (j < count && toks[j].kind == TOK_NEWLINE) j++;
            if (j >= count || toks[j].kind != TOK_INDENT) break;
            int depth = 1, start = ++j;
            while (j < count && depth > 0) {
                if (toks[j].kind == TOK_INDENT) depth++;
                else if (toks[j].kind == TOK_DEDENT) depth--;
                j++;
            }
            if (gn_range_has_type_var(toks, start, j))
                gn_add(names, n, cap, intern(it, toks[i + 1].start, toks[i + 1].length));
            break;
        }
        case TOK_LET: {
            /* let NAME = <'a,...>(...) or let NAME = (... 'a ...) -> ...:
             * generic iff the lambda header (explicit prefix or parameter
             * list) mentions a type variable. */
            int j = i + 1;
            if (j < count && toks[j].kind == TOK_MUT) j++;
            if (j + 1 >= count || toks[j].kind != TOK_IDENT ||
                toks[j + 1].kind != TOK_EQ)
                break;
            Token *name_tok = &toks[j];
            j += 2;
            while (j < count && (toks[j].kind == TOK_NEWLINE || toks[j].kind == TOK_INDENT)) j++;
            if (j >= count) break;
            if (toks[j].kind == TOK_LT) {
                /* explicit type/const-param prefix */
                gn_add(names, n, cap, intern(it, name_tok->start, name_tok->length));
                break;
            }
            if (toks[j].kind != TOK_LPAREN) break;
            int depth = 1, start = ++j;
            while (j < count && depth > 0) {
                if (toks[j].kind == TOK_LPAREN) depth++;
                else if (toks[j].kind == TOK_RPAREN) depth--;
                else if (toks[j].kind == TOK_NEWLINE || toks[j].kind == TOK_EOF) break;
                j++;
            }
            if (gn_range_has_type_var(toks, start, j))
                gn_add(names, n, cap, intern(it, name_tok->start, name_tok->length));
            break;
        }
        case TOK_IMPORT: {
            /* Admit every `as` alias on the import line: the aliased target's
             * genericness is not visible at token level, and an extra name
             * only means a `<` after it is tried as type arguments first. */
            int j = i + 1;
            while (j < count && toks[j].kind != TOK_NEWLINE && toks[j].kind != TOK_EOF) {
                if (toks[j].kind == TOK_AS && j + 1 < count &&
                    toks[j + 1].kind == TOK_IDENT)
                    gn_add(names, n, cap, intern(it, toks[j + 1].start, toks[j + 1].length));
                j++;
            }
            break;
        }
        default: break;
        }
    }
}

/* Gate for the expression-position `<` scans: claim a generic reading only
 * when the callee name is a known generic declaration. */
static bool gate_generic_name(Parser *p, Expr *left) {
    const char *name = NULL;
    if (left->kind == EXPR_IDENT) name = left->ident.name;
    else if (left->kind == EXPR_FIELD) name = left->field.name;
    if (!name) return false;
    for (int i = 0; i < p->generic_name_count; i++)
        if (p->generic_names[i] == name) return true;
    return false;
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

/* Whether the '{' at `off` opens a struct literal's fields, `{ }` or
 * `{ ident = ...`, as opposed to a tuple literal. */
static bool at_struct_lit_brace(Parser *p, int off) {
    if (peek_at(p, off)->kind != TOK_LBRACE) return false;
    TokenKind next = peek_at(p, off + 1)->kind;
    return next == TOK_RBRACE ||
           (next == TOK_IDENT && peek_at(p, off + 2)->kind == TOK_EQ);
}

/* The offset just past the bracket group that opens at `off`, or of the EOF
 * token when the group never closes. */
static int skip_group(Parser *p, int off, TokenKind open, TokenKind close) {
    int depth = 0;
    do {
        TokenKind k = peek_at(p, off)->kind;
        if (k == open) depth++;
        else if (k == close) depth--;
        else if (k == TOK_EOF) break;
        off++;
    } while (depth > 0);
    return off;
}

/* Whether `[ ... ] {` starts at `off`: the size (or empty brackets) and the
 * opening brace of a slice literal. Nothing else puts a brace after a closing
 * ']' in expression position, which is what separates a literal from indexing. */
static bool at_slice_lit_brackets(Parser *p, int off) {
    return peek_at(p, off)->kind == TOK_LBRACKET &&
           peek_at(p, skip_group(p, off, TOK_LBRACKET, TOK_RBRACKET))->kind == TOK_LBRACE;
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

/* Backtrack to a saved token position, abandoning any pending '>>' split. The
 * '>>' token itself is never mutated, so a re-parse from `save` sees it whole:
 * a tentative type parse in `(a<b>>c)` that splits the '>>' and then backtracks
 * re-parses it as the shift `b >> c`. */
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
 * closers: the first call records the split and stays parked on the token, so a
 * nested list can close; the enclosing list's call consumes the second half and
 * advances. C++11, Java and Rust split '>>' the same way in type-argument
 * context. Returns false (consuming nothing) when the current token closes no
 * type-argument list. */
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
        diag_error(loc_from_token(current(p)),
            "expected '>' to close type arguments, got %s",
            token_kind_name(current(p)->kind));
        /* No consume: an enclosing recovery loop syncs on the current token. */
    }
}

/* A token's location, tagged with the file being parsed. */
static SrcLoc tok_loc(Parser *p, const Token *t) {
    SrcLoc l = loc_from_token(t);
    l.filename = p->filename;
    return l;
}

/* Error-recovery contract: on a token mismatch, report once and return the
   current token without consuming it, so an enclosing recovery loop can
   synchronize on it. The parser never aborts: every syntax error is reported
   with diag_error and parsing continues (a lex error, reported before parsing,
   is the fatal kind). Forward progress comes from the leaf-bump rule (parse_prefix /
   parse_pattern_atom) plus the recover_progress() watchdog in every item loop.
   Callers may read the returned token's text but must not assume it was
   consumed. */
static Token *expect(Parser *p, TokenKind kind) {
    if (current(p)->kind != kind) {
        diag_error(loc_from_token(current(p)), "expected %s, got %s",
            token_kind_name(kind), token_kind_name(current(p)->kind));
        return current(p);
    }
    return advance_p(p);
}

/* A keyword token: word-shaped but not an identifier. */
static bool is_keyword_token(const Token *t) {
    return t->kind != TOK_IDENT && t->length > 0 &&
           (isalpha((unsigned char)t->start[0]) || t->start[0] == '_');
}

/* A name being declared (a binding, parameter, field, variant or type). A
 * keyword in its place is reported once, as a keyword, and read on as if it
 * were the name, so the rest of the declaration parses instead of cascading. */
static Token *expect_name(Parser *p) {
    Token *t = current(p);
    if (is_keyword_token(t)) {
        diag_error(loc_from_token(t), "'%.*s' is a keyword and cannot be used as a name",
                   t->length, t->start);
        return advance_p(p);
    }
    return expect(p, TOK_IDENT);
}

static void skip_newlines(Parser *p) {
    while (check(p, TOK_NEWLINE)) advance_p(p);
}

/* Skip statement separators between the statements of a block or inline
   sequence: newlines and `;`, which separates statements just as a newline
   does. */
static void skip_separators(Parser *p) {
    while (check(p, TOK_NEWLINE) || check(p, TOK_SEMICOLON)) advance_p(p);
}

/* ---- Error recovery ---- */

/* Tokens the leaf-bump rule never consumes: layout boundaries, closing
   delimiters and statement separators, so an outer construct keeps its
   delimiter. */
static bool is_hard_stop(TokenKind k) {
    return k == TOK_NEWLINE || k == TOK_INDENT || k == TOK_DEDENT || k == TOK_EOF ||
           k == TOK_RPAREN  || k == TOK_RBRACE || k == TOK_RBRACKET || k == TOK_SEMICOLON;
}

/* Panic-mode skip: advance until the current token is in `set` (a synchronizing
   anchor) or a block/file boundary (DEDENT/EOF). Leaves the anchor unconsumed so
   the enclosing loop can resume on it. Never crosses DEDENT/EOF, so it cannot
   escape the current block. */
static void recover_to(Parser *p, const TokenKind *set, int n) {
    while (!at_end_p(p)) {
        TokenKind k = current(p)->kind;
        if (k == TOK_DEDENT || k == TOK_EOF) return;
        for (int i = 0; i < n; i++) if (k == set[i]) return;
        advance_p(p);
    }
}

/* End one line of a declaration body (a field, a variant, a member).
   Leftover tokens are reported once (unless the line already reported an
   error) and skipped, nested blocks included, so a malformed line costs one
   diagnostic rather than one per token. `line_errs` is diag_error_count() at
   the start of the line. */
static void end_body_line(Parser *p, int line_errs) {
    if (check(p, TOK_NEWLINE) || check(p, TOK_DEDENT) || at_end_p(p)) return;
    if (diag_error_count() == line_errs)
        diag_error(loc_from_token(current(p)), "expected end of line, got %s",
                   token_kind_name(current(p)->kind));
    int depth = 0;
    while (!at_end_p(p)) {
        TokenKind k = current(p)->kind;
        if (depth == 0 && (k == TOK_NEWLINE || k == TOK_DEDENT)) return;
        if (k == TOK_INDENT) depth++;
        else if (k == TOK_DEDENT) depth--;
        advance_p(p);
    }
}

/* No-progress watchdog for every recovery loop: if an iteration consumed nothing
   (a sub-parser stalled on a token it couldn't use), force one token of progress
   so the loop can never spin. Stops short at a DEDENT/EOF, which the loop condition
   handles. `guard_pos` is the parser position captured at the top of the iteration. */
static void recover_progress(Parser *p, int guard_pos) {
    if (p->pos == guard_pos && !at_end_p(p) && !check(p, TOK_DEDENT)) advance_p(p);
}

static bool is_decl_start(TokenKind k);

/* Panic-mode skip for the top-level and module-body recovery loops: anchor on
   a declaration-starting keyword (hierarchical sync, as in the Dragon Book) so
   a dropped terminator resyncs at the next declaration rather than swallowing
   it. Block bodies resync on layout (NEWLINE/DEDENT) through leaf-bump and the
   watchdog instead, since a statement keyword is not a reliable in-block
   anchor. Like recover_to, never crosses DEDENT/EOF. */
static void recover_to_decl(Parser *p) {
    while (!at_end_p(p) && !check(p, TOK_DEDENT) && !is_decl_start(current(p)->kind))
        advance_p(p);
}

static const char *tok_intern(Parser *p, Token *t) {
    return intern(p->intern, t->start, t->length);
}

/* The C name in an extern declaration: an identifier, or any keyword, since a
 * C library may use a name FC reserves ('extern free as c_free: ...'; the
 * caller then requires the alias). */
static Token *expect_extern_c_name(Parser *p) {
    TokenKind k = current(p)->kind;
    if (k == TOK_IDENT || is_keyword_token(current(p)))
        return advance_p(p);
    diag_error(loc_from_token(current(p)),
        "expected identifier in extern declaration, got %s", token_kind_name(k));
    return current(p); /* no consume: the recovery loop syncs on this token */
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
    PREC_POSTFIX,       /* x! x? x[i] x.f x() */
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
    case TOK_QUESTION:  return PREC_POSTFIX;
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

/* Parse-error placeholder nodes. Each carries only kind+loc; they exist only when
   diag_error_count()>0 (so they never reach codegen) and pass2 types them silently. */
static Expr *alloc_expr_error(Parser *p, SrcLoc loc) {
    return alloc_expr(p, EXPR_ERROR, loc);
}
static Pattern *alloc_pat_error(Parser *p, SrcLoc loc) {
    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->kind = PAT_ERROR;
    loc.filename = p->filename;
    pat->loc = loc;
    return pat;
}
static Decl *alloc_decl_error(Parser *p, SrcLoc loc) {
    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_ERROR;
    loc.filename = p->filename;
    d->loc = loc;
    return d;
}

/* The source text from token `from` up to token `to`, trailing whitespace
 * trimmed, for the runtime messages of `assert` and `!`. Empty when a parse
 * error was reported since `errs_before`: recovery can synthesize layout tokens
 * whose text is not in the source buffer. Errors stop codegen, the only
 * consumer, so the empty text is never seen. */
static const char *source_text(Parser *p, const Token *from, const Token *to,
                               int errs_before, int *len) {
    const char *s = from->start;
    int n = 0;
    if (diag_error_count() == errs_before) {
        n = (int)(to->start - s);
        while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == '\t'))
            n--;
    }
    *len = n;
    return arena_strdup(p->arena, s, n);
}

static Expr **arena_copy_exprs(Parser *p, Expr **arr, int count) {
    return arena_dup(p->arena, arr, count, sizeof *arr);
}

/* A statement list as one expression: the statement itself when there is
 * only one, otherwise an EXPR_BLOCK over the (arena-owned) list. */
static Expr *block_or_single(Parser *p, Expr **stmts, int count, SrcLoc loc) {
    if (count == 1) return stmts[0];
    Expr *b = alloc_expr(p, EXPR_BLOCK, loc);
    b->block.stmts = stmts;
    b->block.count = count;
    return b;
}

/* ---- Type parsing ---- */

static Type *parse_type(Parser *p);
static Type *parse_type_arg(Parser *p);
static Type **parse_type_arg_list(Parser *p, int *count);
static bool parse_static_assert_line(Parser *p, Expr **out_cond, const char **out_msg, SrcLoc *out_loc);
static Expr *parse_const_arith(Parser *p, int min_prec);
static Expr *parse_const_arith_inner(Parser *p, int min_prec);
static Expr *parse_prefix(Parser *p);

/* Check if a token kind is valid inside a type argument list <...> */
static bool is_type_arg_token(TokenKind k) {
    switch (k) {
    case TOK_IDENT: case TOK_TYPE_VAR: case TOK_VOID: case TOK_ERROR_KW:
    case TOK_LT: case TOK_GT:
    case TOK_COMMA:
    case TOK_DOT:                       /* module-qualified type: m.point */
    case TOK_QUESTION: case TOK_STAR: case TOK_BANG:
    case TOK_LBRACKET: case TOK_RBRACKET:
    case TOK_LPAREN: case TOK_RPAREN:
    case TOK_LBRACE: case TOK_RBRACE:   /* tuple type {T1, T2} as a generic arg */
    case TOK_ARROW:
    case TOK_CONST:
    case TOK_INT_LIT: case TOK_MINUS:   /* const generic argument: wide<256>, wide<-1> */
    case TOK_SIZEOF: case TOK_ALIGNOF:  /* rejected in pass2 with the reason */
    case TOK_PLUS: case TOK_SLASH: case TOK_PERCENT:  /* bare const arithmetic: wide<'n * 2> */
        return true;
    default:
        return false;
    }
}

/* Scan a balanced type-argument list: `start` is the token just after the '<',
 * and every token up to the matching '>' must be type-compatible. Returns the
 * index just past the closing '>' / '>>', or -1 when the run cannot be a
 * type-argument list at all. The expression-position readings of `name<...>`
 * share this scan, so they agree on the extent of the list, and differ only in
 * what must follow it (the three predicates below). */
static int typearg_scan(Parser *p, int start) {
    int scan = start;
    int depth = 1;
    int paren = 0;
    while (scan < p->token_count && depth > 0) {
        TokenKind k = p->tokens[scan].kind;
        /* Inside parentheses any token is admitted (a parenthesized const
         * expression takes any expression, including shifts and comparisons)
         * and angle tokens don't count toward the <...> depth. */
        if (k == TOK_LPAREN) { paren++; scan++; continue; }
        if (k == TOK_RPAREN) { if (--paren < 0) return -1; scan++; continue; }
        if (paren > 0) {
            if (k == TOK_NEWLINE || k == TOK_EOF) return -1;
            scan++;
            continue;
        }
        if (k == TOK_LT) { depth++; scan++; continue; }
        if (k == TOK_GT) { depth--; scan++; continue; }
        if (k == TOK_GTGT) {
            /* '>>' closes two levels (split in type-argument context) */
            depth -= 2;
            if (depth < 0) return -1; /* unbalanced: read as comparison/shift */
            scan++;
            continue;
        }
        if (!is_type_arg_token(k)) return -1;
        scan++;
    }
    if (paren != 0 || depth != 0 || scan >= p->token_count) return -1;
    return scan;
}

/* A generic call or generic variant construction: the list is followed by '('
 * (call) or '.' (variant). */
static bool generic_call_scan(Parser *p, int start) {
    int end = typearg_scan(p, start);
    return end >= 0 && (p->tokens[end].kind == TOK_LPAREN ||
                        p->tokens[end].kind == TOK_DOT);
}

/* A bare generic instantiation in value position: the list is followed by a
 * token that cannot begin an expression, so `name<Type>` there cannot be a
 * comparison chain (`a < b > c` stays a comparison because `c` starts an
 * expression). The node goes to pass2, which reports that a generic cannot be
 * used as a value, instead of '>' failing later as a parse error. The set
 * lists only tokens that cannot start an expression, so no valid comparison
 * is misread; a terminator missing from it only costs the clearer message. */
static bool bare_inst_scan(Parser *p, int start) {
    int end = typearg_scan(p, start);
    if (end < 0) return false;
    switch (p->tokens[end].kind) {
    case TOK_NEWLINE: case TOK_DEDENT: case TOK_EOF:
    case TOK_RPAREN: case TOK_RBRACKET: case TOK_RBRACE:
    case TOK_COMMA:
        return true;
    default:
        return false;
    }
}

/* Explicit type arguments on a struct literal, `name<Types> { field = ... }`,
 * which FC does not have: a struct literal's type arguments are inferred from
 * its field values. The brace shape is the one parse_ident_prefix uses to tell
 * a struct literal from a tuple literal (`{}` or `{ ident =`). Neither shape
 * is a legal tuple literal, so neither can be the right operand of a
 * comparison, and this reading needs no generic-name gate: the form is wrong
 * whether or not the name is generic. */
static bool struct_lit_typearg_scan(Parser *p, int start) {
    int end = typearg_scan(p, start);
    return end >= 0 && at_struct_lit_brace(p, end - p->pos);
}

/* Flatten an `a.b.c` chain of EXPR_IDENT/EXPR_FIELD back into the dotted,
 * interned spelling parse_struct_literal takes; NULL if the chain holds
 * anything else. The buffer is sized from the components, since FC
 * identifiers are unbounded. */
static int dotted_name_len(Expr *e) {
    if (e->kind == EXPR_IDENT) return (int)strlen(e->ident.name);
    if (e->kind != EXPR_FIELD) return -1;
    int n = dotted_name_len(e->field.object);
    return n < 0 ? -1 : n + 1 + (int)strlen(e->field.name);
}

static char *dotted_name_fill(Expr *e, char *out) {
    if (e->kind == EXPR_FIELD) {
        out = dotted_name_fill(e->field.object, out);
        *out++ = '.';
    }
    const char *s = (e->kind == EXPR_FIELD) ? e->field.name : e->ident.name;
    size_t n = strlen(s);
    memcpy(out, s, n);
    return out + n;
}

static const char *dotted_name_of(Parser *p, Expr *e) {
    int len = dotted_name_len(e);
    if (len < 0) return NULL;
    char *buf = arena_alloc(p->arena, (size_t)len + 1);
    *dotted_name_fill(e, buf) = '\0';
    return intern(p->intern, buf, len);
}

static uint64_t parse_int_value(const char *start, int length, bool *out_of_range);

static Type *parse_type_suffix(Parser *p, Type *base) {
    /* T*, T[], T[N], T?, applied left to right */
    for (;;) {
        /* A fixed array is a field's storage, not a general type: C spells it
         * as an inside-out declarator (`uint8_t m[3][2]`) that no other type
         * constructor here composes with, since `u8[2][3]`, `u8[2][]`,
         * `u8[2]*` and `u8[2]?` would each emit `uint8_t[2]` as a type
         * specifier, which is not C. So a fixed array must be the outermost
         * type it appears in; a fixed array of something (`u8[][2]`) is fine. */
        if (base->kind == TYPE_FIXED_ARRAY &&
            (check(p, TOK_STAR) || check(p, TOK_LBRACKET) ||
             check(p, TOK_QUESTION) || check(p, TOK_BANG))) {
            diag_error(loc_from_token(current(p)),
                       "a fixed array must be a field's outermost type");
            break;
        }
        if (check(p, TOK_STAR)) {
            advance_p(p);
            base = type_pointer(p->arena, base);
            continue;
        }
        if (check(p, TOK_LBRACKET) && peek_at(p, 1)->kind == TOK_RBRACKET &&
            peek_at(p, 2)->kind != TOK_LBRACE) {
            /* "[]" not followed by "{" is a slice-type suffix. In "T[] {" the
             * "[]" belongs to a slice literal (T[] { ptr = .., len = .. }), not
             * to the element type, and is left for parse_array_lit_body. A type
             * is never otherwise followed by an open brace, so the test is
             * unambiguous. */
            advance_p(p); advance_p(p);
            base = type_slice(p->arena, base);
            continue;
        }
        /* T[N]: fixed-size inline array, where allow_fixed_array admits one */
        if (p->allow_fixed_array && check(p, TOK_LBRACKET) &&
            peek_at(p, 1)->kind == TOK_INT_LIT && peek_at(p, 2)->kind == TOK_RBRACKET) {
            advance_p(p); /* consume [ */
            Token *size_tok = current(p);
            bool size_oor = false;
            int64_t size = parse_int_value(size_tok->start, size_tok->length, &size_oor);
            if (size_oor || size <= 0) {
                diag_error(loc_from_token(size_tok),
                           "fixed array size must be a positive integer, got %lld",
                           (long long)size);
                size = 1;
            }
            advance_p(p); /* consume INT_LIT */
            expect(p, TOK_RBRACKET);
            base = type_fixed_array(p->arena, base, size);
            continue;
        }
        /* T['n]: fixed array sized by a const generic parameter; positivity
         * is checked per instantiation once the value is known. */
        if (p->allow_fixed_array && check(p, TOK_LBRACKET) &&
            peek_at(p, 1)->kind == TOK_TYPE_VAR && peek_at(p, 2)->kind == TOK_RBRACKET) {
            advance_p(p); /* consume [ */
            Token *var_tok = current(p);
            advance_p(p); /* consume TYPE_VAR */
            expect(p, TOK_RBRACKET);
            Type *size_ref = type_type_var(p->arena, tok_intern(p, var_tok));
            base = type_fixed_array_sym(p->arena, base, size_ref);
            continue;
        }
        /* T['n / 32], T[('n * 2)], T[4 * 2], T[cfg.word]: fixed array sized
         * by a const expression over const generic params, named constants or
         * literals. pass2 or instantiation folds it and checks positivity. */
        if (p->allow_fixed_array && check(p, TOK_LBRACKET) &&
            (peek_at(p, 1)->kind == TOK_TYPE_VAR || peek_at(p, 1)->kind == TOK_LPAREN ||
             peek_at(p, 1)->kind == TOK_MINUS || peek_at(p, 1)->kind == TOK_INT_LIT ||
             peek_at(p, 1)->kind == TOK_IDENT || peek_at(p, 1)->kind == TOK_SIZEOF ||
             peek_at(p, 1)->kind == TOK_ALIGNOF)) {
            advance_p(p); /* consume [ */
            Expr *size_expr = parse_const_arith(p, 1);
            expect(p, TOK_RBRACKET);
            base = type_fixed_array_sym(p->arena, base, type_const_expr(p->arena, size_expr));
            continue;
        }
        if (check(p, TOK_QUESTION)) {
            advance_p(p);
            base = type_option(p->arena, base);
            continue;
        }
        if (check(p, TOK_BANG)) {
            advance_p(p);
            base = type_result(p->arena, base);
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
    if (inner->kind == TYPE_RESULT &&
        inner->result.inner &&
        (inner->result.inner->kind == TYPE_POINTER ||
         inner->result.inner->kind == TYPE_SLICE ||
         inner->result.inner->kind == TYPE_ANY_PTR)) {
        Type *ci = arena_alloc(a, sizeof(Type));
        *ci = *inner->result.inner;
        ci->is_const = true;
        return type_result(a, ci);
    }
    diag_error(loc, "'const' can only modify pointer (*) or slice ([]) types, got %s",
               type_name(inner));
    return type_error();
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
        /* `void` takes one suffix only, `!`. void! is the payload-less
         * result (represented by the i32 tag alone); further suffixes compose
         * on the result as usual (void!?). An option, pointer or slice of
         * bare void is illegal, since void is not a value type. */
        if (check(p, TOK_BANG)) {
            advance_p(p);
            return parse_type_suffix(p, type_result(p->arena, type_void()));
        }
        return type_void();
    }

    if (t->kind == TOK_ERROR_KW) {
        /* `error` in type position: a display alias for i32, like str and
         * cstr. An error code is an i32 everywhere; the alias only affects
         * display. */
        advance_p(p);
        return parse_type_suffix(p, type_error_code());
    }

    if (t->kind == TOK_TYPE_VAR) {
        advance_p(p);
        return parse_type_suffix(p, type_type_var(p->arena, tok_intern(p, t)));
    }

    if (t->kind == TOK_IDENT) {
        Type *base = type_from_name(t->start, t->length);
        if (base) {
            advance_p(p);
            /* Special case: 'any' must be followed by '*' */
            if (base == type_any_ptr()) {
                if (!check(p, TOK_STAR)) {
                    diag_error(loc_from_token(current(p)),
                        "'any' must be followed by '*' (any*)");
                    return type_error();
                }
                advance_p(p); /* consume the mandatory * */
                return parse_type_suffix(p, base);
            }
            return parse_type_suffix(p, base);
        }
        /* User-defined type name (struct/union): create a stub holding just
         * the name, which may be module-qualified (module.type,
         * module.sub.type). */
        advance_p(p);
        const char *type_name = tok_intern(p, t);
        while (check(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
            advance_p(p); /* consume . */
            Token *member = current(p);
            advance_p(p); /* consume member name */
            type_name = intern_sprintf(p->intern, "%s.%.*s", type_name,
                                       member->length, member->start);
        }
        Type *udt = arena_alloc(p->arena, sizeof(Type));
        udt->kind = TYPE_STUB;
        udt->stub.name = type_name;
        udt->stub.qualified_name = NULL;
        udt->stub.type_args = NULL;
        udt->stub.type_arg_count = 0;
        /* name<Type, ...>: type arguments, unless the list does not close with
         * '>' (then the '<' was not one: backtrack). Diagnostics from the
         * attempt wait for that verdict. */
        if (check(p, TOK_LT)) {
            int save = p->pos;
            int held = diag_speculate_begin();
            advance_p(p); /* consume < */
            int ta_count = 0;
            Type **targs = parse_type_arg_list(p, &ta_count);
            if (at_typearg_gt(p)) {
                diag_speculate_end(held, true);
                consume_typearg_gt(p); /* consume > (splitting a >> for a nested list) */
                udt->stub.type_args = targs;
                udt->stub.type_arg_count = ta_count;
            } else {
                diag_speculate_end(held, false);
                restore_pos(p, save);
            }
        }
        return parse_type_suffix(p, udt);
    }

    /* Tuple type: { T1, T2, ... }, an anonymous positional product (>= 2 elements) */
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
        if (ecount < 2) {
            diag_error(loc, "tuple type requires at least 2 element types, got %d", ecount);
            free(elems);
            return type_error();
        }
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
            /* No "->": this "(...)" is a grouped type, not a function type.
             * `((i32) -> i32)?` groups the inner function type so a postfix
             * (?, *, []) applies to the whole of it. Only a single, non-variadic
             * element forms a grouped type; `(T1, T2)` or `(...)` is only a
             * parameter list and needs the `->`. */
            if (pcount != 1 || is_variadic) {
                diag_error(loc_from_token(current(p)),
                    "expected '->' after function parameter list");
                free(params);
                return type_error();
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
        ft->func.param_types = arena_dup(p->arena, params, pcount, sizeof(Type*));
        free(params);
        return parse_type_suffix(p, ft);
    }

    SrcLoc loc = loc_from_token(t);
    diag_error(loc, "expected type, got %s", token_kind_name(t->kind));
    /* No consume: the caller (a `: type` or parameter-list site) resyncs. pass2
       lets the poison type pass silently. */
    return type_error();
}

/* The comma-separated list after a '<' that has already been scanned as a
 * type-argument list; stops before the closing '>'. */
static Type **parse_type_arg_list(Parser *p, int *count) {
    Type **args = NULL;
    int n = 0, cap = 0;
    do {
        Type *ty = parse_type_arg(p);
        DA_APPEND(args, n, cap, ty);
        if (!check(p, TOK_COMMA)) break;
        advance_p(p);
    } while (1);
    Type **out = arena_dup(p->arena, args, n, sizeof *args);
    free(args);
    *count = n;
    return out;
}

/* A type in a slot that admits the fixed-array form T[N]. */
static Type *parse_type_allowing_fixed_array(Parser *p) {
    bool saved = p->allow_fixed_array;
    p->allow_fixed_array = true;
    Type *t = parse_type(p);
    p->allow_fixed_array = saved;
    return t;
}

/* ---- Expression parsing ---- */

static Expr *parse_expr(Parser *p, Prec min_prec);
static Expr *parse_block_item(Parser *p);
static Pattern *parse_pattern(Parser *p);
static Expr *parse_match_expr(Parser *p);
static Expr *parse_struct_literal(Parser *p, const char *type_name, SrcLoc loc);

/* Parse a sub-expression inside matched brackets (parens, square, curly).
   Clears `block_arm_arrow` for the duration: a `->` inside brackets is never a
   match arm's separator. */
static Expr *parse_bracketed_expr(Parser *p, Prec min_prec) {
    bool saved = p->block_arm_arrow;
    p->block_arm_arrow = false;
    Expr *e = parse_expr(p, min_prec);
    p->block_arm_arrow = saved;
    return e;
}

/* Call arguments after a consumed '(', stopping before the ')'. */
static Expr **parse_call_args(Parser *p, int *count) {
    Expr **args = NULL;
    int n = 0, cap = 0;
    if (!check(p, TOK_RPAREN)) {
        do {
            Expr *arg = parse_bracketed_expr(p, PREC_NONE + 1);
            DA_APPEND(args, n, cap, arg);
            if (!check(p, TOK_COMMA)) break;
            advance_p(p);
        } while (1);
    }
    Expr **out = arena_copy_exprs(p, args, n);
    free(args);
    *count = n;
    return out;
}

static uint64_t parse_int_value(const char *start, int length, bool *out_of_range) {
    /* Sized from the token, whose characters the digits never outnumber. A
     * clipped digit string is not detectably wrong: strtoull on the prefix
     * succeeds without ERANGE and yields a different number. */
    char *buf = xmalloc((size_t)length + 1);
    int num_len = 0;
    if (out_of_range) *out_of_range = false;

    int base = 10;
    int prefix_len = 0;
    if (length >= 2 && start[0] == '0') {
        if (start[1] == 'x' || start[1] == 'X')      { base = 16; prefix_len = 2; }
        else if (start[1] == 'b' || start[1] == 'B') { base = 2;  prefix_len = 2; }
        else if (start[1] == 'o' || start[1] == 'O') { base = 8;  prefix_len = 2; }
    }

    for (int i = prefix_len; i < length; i++) {
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
    free(buf);
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

/* The literal's type in the current slot. A const expression evaluates in the
 * i64 domain, so an unsuffixed literal inside one is i64 rather than the i32
 * expression default. A lone `f<4000000000>` and the same literal inside const
 * arithmetic are then judged against the same range. A written suffix always
 * wins. */
static Type *int_lit_type(Parser *p, const Token *tok) {
    const char *start = tok->start;
    int length = tok->length;
    int num_end = int_num_end(start, length);
    if (num_end >= length)
        return p->in_const_expr ? type_int64() : type_int32();
    Type *t = type_from_int_suffix(start + num_end, length - num_end);
    return t ? t : type_int32();
}

/* ---- Const generic arguments ----
 *
 * A generic argument is either a type or a const (value) expression. Bare
 * const expressions admit integer literals, const params ('n), named consts,
 * and + - * / % with unary minus; shifts and comparisons require parentheses
 * (`wide<('n >> 2)>`), inside which any expression is accepted. */

/* One atom of a bare const expression inside <...> or a size slot. */
static Expr *parse_const_atom(Parser *p) {
    Token *t = current(p);
    SrcLoc loc = loc_from_token(t);
    switch (t->kind) {
    case TOK_INT_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_INT_LIT, loc);
        bool oor = false;
        e->int_lit.value = parse_int_value(t->start, t->length, &oor);
        e->int_lit.lit_type = int_lit_type(p, t);
        e->int_lit.out_of_range = oor;
        return e;
    }
    case TOK_TYPE_VAR: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_TYPE_VAR_REF, loc);
        e->type_var_ref.name = tok_intern(p, t);
        return e;
    }
    case TOK_IDENT: {
        /* named constant, possibly dotted (m.x) or a type property (i32.bits) */
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_IDENT, loc);
        e->ident.name = tok_intern(p, t);
        while (check(p, TOK_DOT)) {
            advance_p(p);
            Token *m = expect(p, TOK_IDENT);
            Expr *fld = alloc_expr(p, EXPR_FIELD, loc);
            fld->field.object = e;
            fld->field.name = tok_intern(p, m);
            fld->field.name_loc = tok_loc(p, m);
            e = fld;
        }
        return e;
    }
    case TOK_MINUS: {
        advance_p(p);
        Expr *operand = parse_const_atom(p);
        Expr *e = alloc_expr(p, EXPR_UNARY_PREFIX, loc);
        e->unary_prefix.op = TOK_MINUS;
        e->unary_prefix.operand = operand;
        return e;
    }
    case TOK_LPAREN: {
        advance_p(p);
        Expr *e = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        return e;
    }
    case TOK_SIZEOF:
    case TOK_ALIGNOF:
        /* Parsed whole so pass2 can reject it with the reason (the C compiler
         * decides a type's size), instead of the argument list falling apart
         * into a comparison. */
        return parse_prefix(p);
    default:
        diag_error(loc, "expected a constant (an integer, a const parameter or a "
                   "named constant), got %s", token_kind_name(t->kind));
        if (!is_hard_stop(t->kind)) advance_p(p);
        return alloc_expr_error(p, loc);
    }
}

/* min_prec: 1 = additive level, 2 = multiplicative level. */
static Expr *parse_const_arith(Parser *p, int min_prec) {
    bool saved_const = p->in_const_expr;
    p->in_const_expr = true;
    Expr *r = parse_const_arith_inner(p, min_prec);
    p->in_const_expr = saved_const;
    return r;
}

static Expr *parse_const_arith_inner(Parser *p, int min_prec) {
    Expr *left = parse_const_atom(p);
    while (1) {
        TokenKind k = current(p)->kind;
        int prec;
        if (k == TOK_STAR || k == TOK_SLASH || k == TOK_PERCENT) prec = 2;
        else if (k == TOK_PLUS || k == TOK_MINUS) prec = 1;
        else break;
        if (prec < min_prec) break;
        Token *op_tok = current(p);
        advance_p(p);
        Expr *right = parse_const_arith_inner(p, prec + 1);
        Expr *e = alloc_expr(p, EXPR_BINARY, loc_from_token(op_tok));
        e->binary.op = k;
        e->binary.left = left;
        e->binary.right = right;
        left = e;
    }
    return left;
}

/* Can a token of kind `k` begin a const atom? Decides `t*` vs `a * b`: after
 * `*`, a const atom never continues a pointer type. */
static bool starts_const_atom(TokenKind k) {
    return k == TOK_INT_LIT || k == TOK_TYPE_VAR || k == TOK_IDENT ||
           k == TOK_LPAREN || k == TOK_MINUS;
}

/* Lookahead over a dotted name (IDENT ('.' IDENT)*) starting at offset `off`;
 * returns the offset of the first token past it. */
static int scan_dotted_ident(Parser *p, int off) {
    off++;  /* the IDENT itself */
    while (peek_at(p, off)->kind == TOK_DOT &&
           peek_at(p, off + 1)->kind == TOK_IDENT)
        off += 2;
    return off;
}

/* Would the tokens ahead read as a bare const expression rather than a type?
 * Decides for the IDENT/TYPE_VAR-headed case: a following + - / % makes it
 * arithmetic; a following * is arithmetic iff the token after it can begin a
 * const atom (no type continues past `T*` with an expression atom).
 *
 * A dotted name is the one shape the parser cannot settle on its own (`m.point`
 * is a module-qualified type, `dir.count` a value), so it reads as a type here
 * and pass2 takes the const reading when the name denotes one
 * (try_named_const_arg). The exception is a built-in type name with a member
 * (`i32.bits`): no type continues past `i32`, so only the value reading is
 * possible, and parse_type would stop at the `.` and abandon the whole
 * argument list. */
static bool ident_arg_is_const_expr(Parser *p) {
    Token *head = current(p);
    if (head->kind == TOK_IDENT && type_from_name(head->start, head->length) &&
        peek_at(p, 1)->kind == TOK_DOT && peek_at(p, 2)->kind == TOK_IDENT)
        return true;
    int off = head->kind == TOK_TYPE_VAR ? 1 : scan_dotted_ident(p, 0);
    TokenKind k = peek_at(p, off)->kind;
    if (k == TOK_PLUS || k == TOK_SLASH || k == TOK_PERCENT) return true;
    if (k == TOK_MINUS)
        /* `t - x` is never a type, and a valid program has no other use for
         * a '-' here. */
        return true;
    if (k == TOK_STAR) {
        /* arithmetic iff a const atom follows the '*' */
        return starts_const_atom(peek_at(p, off + 1)->kind);
    }
    return false;
}

/* Parse one generic argument inside <...>: a const (value) expression or a
 * type. Concrete const expressions fold in pass2; expressions over const
 * params stay symbolic (TYPE_CONST_EXPR) until instantiation. */
static Type *parse_type_arg(Parser *p) {
    Token *t = current(p);
    SrcLoc loc = loc_from_token(t);

    bool is_const_expr = false;
    switch (t->kind) {
    case TOK_INT_LIT:
    case TOK_MINUS:
    case TOK_SIZEOF:
    case TOK_ALIGNOF:
        is_const_expr = true;
        break;
    case TOK_TYPE_VAR:
    case TOK_IDENT:
        is_const_expr = ident_arg_is_const_expr(p);
        break;
    case TOK_LPAREN: {
        /* `(...)` followed by `->` is a function type; otherwise a
         * parenthesized const expression (possibly continued by arithmetic). */
        int depth = 0, off = 0;
        while (peek_at(p, off)->kind != TOK_EOF) {
            TokenKind k = peek_at(p, off)->kind;
            if (k == TOK_LPAREN) depth++;
            else if (k == TOK_RPAREN && --depth == 0) break;
            off++;
        }
        is_const_expr = peek_at(p, off + 1)->kind != TOK_ARROW;
        break;
    }
    default:
        break;
    }

    if (!is_const_expr)
        return parse_type(p);

    Expr *expr = parse_const_arith(p, 1);
    if (expr->kind == EXPR_INT_LIT) {
        if (expr->int_lit.out_of_range) {
            diag_error(loc, "const generic argument out of range");
            return type_error();
        }
        /* Suffixed literals are allowed; the value is taken as-is (i64 domain). */
        return type_const_int(p->arena, (int64_t)expr->int_lit.value);
    }
    return type_const_expr(p->arena, expr);
}

/* A char literal's byte ('a', '\n', '\x41'; the token includes the quotes).
 * The lexer admits one character or escape between the quotes, so the body
 * decodes to one byte. */
static uint8_t char_lit_value(const Token *t) {
    unsigned char b = 0;
    decode_str_lit(t->start + 1, t->length - 2, &b);
    return b;
}

static bool at_stmt_terminator(Parser *p);

/* Parse a block: INDENT item (NEWLINE item)* DEDENT.
   Returns array of Expr*, sets *count. */
static Expr **parse_block(Parser *p, int *count) {
    Expr **stmts = NULL;
    int len = 0, cap = 0;

    expect(p, TOK_INDENT);
    /* Error count as of the start of the current line (see the separator check). */
    int line_errs = diag_error_count();
    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        int at = p->pos;
        skip_separators(p);
        if (p->pos != at) line_errs = diag_error_count(); /* fresh line, fresh slate */
        if (check(p, TOK_DEDENT)) break;
        int guard = p->pos;
        Expr *e = parse_block_item(p);
        DA_APPEND(stmts, len, cap, e);
        /* Items in a block must be separated: an item that stops short of a
           separator leaves the rest of the line unclaimed. Without this check,
           juxtaposed items (`let x = 5 6`, `assert(a) assert(b)`, `continue 5`)
           would parse as separate statements and silently discard values.
           Reported only for a line with no error so far: once a line has
           failed, its leftover tokens are that error's debris. */
        if (p->pos > guard && diag_error_count() == line_errs && !at_stmt_terminator(p)) {
            diag_error(loc_from_token(current(p)),
                "expected a newline or ';' between statements, got %s",
                token_kind_name(current(p)->kind));
        }
        /* Watchdog: a leaf error that stalled on a hard-stop token (e.g. a stray
           ')' or '}') won't have consumed it; force one token of progress. The
           leaf-bump rule handles ordinary garbage; the loop top eats the NEWLINE. */
        recover_progress(p, guard);
    }
    expect(p, TOK_DEDENT);

    *count = len;
    Expr **arena_stmts = arena_copy_exprs(p, stmts, len);
    free(stmts);
    return arena_stmts;
}

/* True at a token that terminates an inline statement: a statement separator,
   a block end, an `else` clause, or EOF.  Governs whether break/return carry a
   value and where an inline `;`-sequence stops. */
static bool at_stmt_terminator(Parser *p) {
    return check(p, TOK_NEWLINE) || check(p, TOK_DEDENT) ||
           check(p, TOK_SEMICOLON) || check(p, TOK_ELSE) || at_end_p(p);
}

/* Parse an inline body: one statement, optionally followed by more statements
   separated by `;` on the same logical line. `;` is a same-level separator
   equivalent to a newline; redundant/trailing `;` are tolerated.  Returns the
   statement array, sets *count. */
static Expr **parse_inline_seq(Parser *p, int *count) {
    Expr **stmts = NULL;
    int len = 0, cap = 0;
    DA_APPEND(stmts, len, cap, parse_block_item(p));
    while (check(p, TOK_SEMICOLON)) {
        while (check(p, TOK_SEMICOLON)) advance_p(p);
        if (at_stmt_terminator(p)) break;
        DA_APPEND(stmts, len, cap, parse_block_item(p));
    }
    *count = len;
    Expr **arena_stmts = arena_copy_exprs(p, stmts, len);
    free(stmts);
    return arena_stmts;
}

/* Parse either a block (if INDENT) or an inline `;`-separated statement
   sequence (break, continue, return, let, defer, or expressions). */
static Expr **parse_body(Parser *p, int *count) {
    if (check(p, TOK_INDENT)) {
        return parse_block(p, count);
    }
    return parse_inline_seq(p, count);
}

/* Parse a guarded/unguarded/checked/unchecked expression: the keyword (already
   consumed) is followed by either an indented block or a single inline expression.
   The body is wrapped in EXPR_GUARD on the given axis and polarity. The inline
   operand parses at PREC_NONE+1 (as let-init / if-cond do), so `unguarded a / b`
   captures the whole `a / b` while parentheses bound it inside a larger expression. */
static Expr *parse_guard(Parser *p, bool is_overflow_axis, bool enable, SrcLoc loc) {
    Expr *body;
    if (check(p, TOK_INDENT)) {
        int count;
        Expr **stmts = parse_block(p, &count);
        body = block_or_single(p, stmts, count, loc);
    } else {
        body = parse_expr(p, PREC_NONE + 1);
    }
    Expr *e = alloc_expr(p, EXPR_GUARD, loc);
    e->guard.body = body;
    e->guard.is_overflow_axis = is_overflow_axis;
    e->guard.enable = enable;
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
            SrcLoc ploc = tok_loc(p, current(p));
            const char *name = tok_intern(p, expect_name(p));
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
    e->func.params = arena_dup(p->arena, params, pcount, sizeof(Param));
    free(params);
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

    Expr *then_expr = block_or_single(p, then_body, then_count, loc);

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
            else_expr = block_or_single(p, else_body, else_count, loc);
        }
    } else {
        /* No else: restore position so the Pratt loop sees the NEWLINE */
        restore_pos(p, save_pos);
    }

    Expr *e = alloc_expr(p, EXPR_IF, loc);
    e->if_expr.cond = cond;
    e->if_expr.then_body = then_expr;
    e->if_expr.else_body = else_expr;
    return e;
}

/* Parse a let-binding: `let [mut] <target> = <init>`. When an explicit `in`
   follows (required if require_in), `let x = v in body` desugars to an
   EXPR_BLOCK of the `let` followed by `body`, which gives the binding an inner
   scope and yields body's value. Without `in` (the offside form) the bare let
   node is returned; the rest of the enclosing block is its implied body. */
static Expr *parse_let_binding(Parser *p, bool require_in) {
    SrcLoc loc = loc_from_token(current(p));
    advance_p(p); /* consume 'let' */
    bool is_mut = false;
    if (check(p, TOK_MUT)) {
        advance_p(p);
        is_mut = true;
    }

    Expr *letnode;
    /* Struct destructuring: let { field = name, ... } = expr */
    if (check(p, TOK_LBRACE)) {
        /* Reuse parse_pattern to handle nested destructuring */
        Pattern *pat = parse_pattern(p);
        expect(p, TOK_EQ);
        Expr *init = parse_expr(p, PREC_NONE + 1);
        letnode = alloc_expr(p, EXPR_LET_DESTRUCT, loc);
        letnode->let_destruct.pattern = pat;
        letnode->let_destruct.is_mut = is_mut;
        letnode->let_destruct.init = init;
    } else {
        Token *name_tok = expect_name(p);
        const char *name = tok_intern(p, name_tok);
        SrcLoc name_loc = tok_loc(p, name_tok);
        expect(p, TOK_EQ);

        Expr *init;
        if (check(p, TOK_INDENT)) {
            int count;
            Expr **body = parse_block(p, &count);
            init = block_or_single(p, body, count, loc);
        } else {
            init = parse_expr(p, PREC_NONE + 1);
        }

        letnode = alloc_expr(p, EXPR_LET, loc);
        letnode->let_expr.let_name = name;
        letnode->let_expr.let_name_loc = name_loc;
        letnode->let_expr.let_is_mut = is_mut;
        letnode->let_expr.let_init = init;
    }

    if (check(p, TOK_IN)) {
        advance_p(p);
        /* The `in` body is greedy, as in ML and F#: an inline `;`-sequence
           that extends to the enclosing boundary (newline, dedent, `)`, `,`,
           `else`, EOF), so `let x = v in a; b` binds `x` over both `a` and `b`. */
        int body_count;
        Expr **body = parse_inline_seq(p, &body_count);
        Expr **stmts = arena_alloc(p->arena, sizeof(Expr *) * (size_t)(body_count + 1));
        stmts[0] = letnode;
        memcpy(&stmts[1], body, sizeof(Expr *) * (size_t)body_count);
        Expr *blk = alloc_expr(p, EXPR_BLOCK, loc);
        blk->block.stmts = stmts;
        blk->block.count = body_count + 1;
        return blk;
    }
    if (require_in) {
        diag_error(loc, "expected 'in' after let-binding in expression position");
        return alloc_expr_error(p, loc);
    }
    return letnode;
}

/* Parse a single item in a block: let binding or expression */
static Expr *parse_block_item(Parser *p) {
    if (check(p, TOK_LET)) {
        return parse_let_binding(p, false);
    }

    if (check(p, TOK_RETURN)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        Expr *value = NULL;
        /* return carries a value unless an inline statement terminator follows */
        if (!at_stmt_terminator(p)) {
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
        if (!at_stmt_terminator(p)) {
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
        /* `defer` schedules an expression to run at scope exit, where a
           control transfer (break/continue/return) has no meaning. These
           tokens parse only as statements, so parse_expr would reject them
           with a bare "unexpected token". Report the reason instead, then parse
           the transfer as an ordinary statement, dropping the defer, so no
           cascade follows. */
        if (check(p, TOK_BREAK) || check(p, TOK_CONTINUE) || check(p, TOK_RETURN)) {
            diag_error(loc_from_token(current(p)),
                "cannot defer a control-flow expression (break, continue, or return)");
            return parse_block_item(p);
        }
        Expr *value = parse_expr(p, PREC_NONE + 1);
        Expr *e = alloc_expr(p, EXPR_DEFER, loc);
        e->defer_expr.value = value;
        return e;
    }

    if (check(p, TOK_IGNORE)) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p);
        Expr *value = parse_expr(p, PREC_NONE + 1);
        Expr *e = alloc_expr(p, EXPR_IGNORE, loc);
        e->ignore_expr.value = value;
        return e;
    }

    return parse_expr(p, PREC_NONE + 1);
}

/* ---- Prefix parsing ---- */

/* Scan a type-expression head at the current token, returning the offset of
 * the token just past it, or -1 if no type expression starts there. Covers the
 * heads that can front a slice literal in expression position:
 *   const* (IDENT (.IDENT)* <args>? | 'a) (? | * | !)*
 * `void!` and `error` have their own paths in parse_prefix; a bare
 * `void`/`error` head is a diagnostic, not a type expression. An unbalanced or
 * non-type-argument `<...>` leaves `off` at the '<', which no caller accepts,
 * so the comparison reading survives. */
static int scan_type_head(Parser *p) {
    int off = 0;
    while (peek_at(p, off)->kind == TOK_CONST) off++;
    TokenKind k = peek_at(p, off)->kind;
    if (k == TOK_TYPE_VAR) {
        off++;
    } else if (k == TOK_IDENT) {
        off++;
        while (peek_at(p, off)->kind == TOK_DOT && peek_at(p, off + 1)->kind == TOK_IDENT)
            off += 2;
        /* Generic arguments <T, 4, ('n >> 1), ...>, by the same scan every
           expression-position `name<...>` reading uses. */
        if (peek_at(p, off)->kind == TOK_LT) {
            int end = typearg_scan(p, p->pos + off + 1);
            if (end >= 0) off = end - p->pos;
        }
    } else {
        return -1;
    }
    while (peek_at(p, off)->kind == TOK_QUESTION || peek_at(p, off)->kind == TOK_STAR ||
           peek_at(p, off)->kind == TOK_BANG)
        off++;
    return off;
}

/* Is a slice literal, `type_expr [ size ] { ... }` or `type_expr [] { ... }`,
 * starting at the current token? The '{' after the matching ']' separates a
 * literal from indexing (`xs[i]`) or a comparison chain, neither of which is
 * followed by a brace here.
 *
 * This decides only the shape; parse_type then parses the element type (const,
 * dotted names, type and const arguments, the ? * ! suffixes). The two agree
 * because '[' is never part of a type in expression position: `T[]` before
 * '{' belongs to the literal (parse_type_suffix declines it), and a fixed
 * array `T[N]` needs allow_fixed_array, which is off here. So the first '['
 * past the head opens the literal. */
static bool at_slice_literal(Parser *p) {
    int off = scan_type_head(p);
    return off >= 0 && at_slice_lit_brackets(p, off);
}

/* The fields of a raw-parts slice literal, after its '{': `ptr = e, len = e`
 * in either order, through the closing '}'. */
static Expr *parse_raw_slice_fields(Parser *p, Type *elem_type, SrcLoc loc) {
    Expr *ptr_val = NULL, *len_val = NULL;
    while (!check(p, TOK_RBRACE) && !at_end_p(p)) {
        int guard = p->pos;
        Token *name = expect(p, TOK_IDENT);
        expect(p, TOK_EQ);
        Expr *val = parse_bracketed_expr(p, PREC_NONE + 1);
        if (name->kind == TOK_IDENT) {
            const char *fn = tok_intern(p, name);
            if (strcmp(fn, "ptr") == 0) ptr_val = val;
            else if (strcmp(fn, "len") == 0) len_val = val;
            else diag_error(loc_from_token(name),
                            "slice literal field must be 'ptr' or 'len', got '%s'", fn);
        }
        if (check(p, TOK_COMMA)) advance_p(p);
        recover_progress(p, guard);
    }
    expect(p, TOK_RBRACE);
    if (!ptr_val || !len_val) {
        diag_error(loc, "slice literal requires both 'ptr' and 'len' fields");
        return alloc_expr_error(p, loc);
    }
    Expr *e = alloc_expr(p, EXPR_SLICE_LIT, loc);
    e->slice_lit.elem_type = elem_type;
    e->slice_lit.ptr_expr = ptr_val;
    e->slice_lit.len_expr = len_val;
    return e;
}

/* The elements of a slice literal, after its '{': `e, e, ...` with an
 * optional trailing comma, through the closing '}'. */
static Expr **parse_slice_elems(Parser *p, int *count) {
    Expr **elems = NULL;
    int n = 0, cap = 0;
    while (!check(p, TOK_RBRACE)) {
        Expr *elem = parse_bracketed_expr(p, PREC_NONE + 1);
        DA_APPEND(elems, n, cap, elem);
        if (!check(p, TOK_COMMA)) break;
        advance_p(p);
    }
    expect(p, TOK_RBRACE);
    Expr **copy = arena_copy_exprs(p, elems, n);
    free(elems);
    *count = n;
    return copy;
}

/* Parse the rest of a slice literal, from its '[', given the already-parsed
 * element type:
 *   T[]  { ptr = expr, len = expr }   raw parts: EXPR_SLICE_LIT
 *   T[N] { e0, e1, ... }              elements:  EXPR_ARRAY_LIT
 * Shared by the IDENT-typed and tuple-typed literal paths. */
static Expr *parse_array_lit_body(Parser *p, Type *elem_type, SrcLoc loc) {
    expect(p, TOK_LBRACKET);

    /* T[] { ptr = expr, len = expr }: slice construction from raw parts */
    if (check(p, TOK_RBRACKET)) {
        advance_p(p); /* consume ] */
        expect(p, TOK_LBRACE);
        return parse_raw_slice_fields(p, elem_type, loc);
    }

    Expr *size_expr = parse_bracketed_expr(p, PREC_NONE + 1);
    expect(p, TOK_RBRACKET);
    expect(p, TOK_LBRACE);

    int elem_count;
    Expr **elems = parse_slice_elems(p, &elem_count);

    Expr *e = alloc_expr(p, EXPR_ARRAY_LIT, loc);
    e->array_lit.elem_type = elem_type;
    e->array_lit.size_expr = size_expr;
    e->array_lit.elems = elems;
    e->array_lit.elem_count = elem_count;
    return e;
}

/* Can this token begin a prefix expression (parse_prefix)? Used by the
 * (IDENT!) cast disambiguation: a `!`-triggered cast attempt commits only
 * when the token after `)` starts an expression (the cast operand). */
static bool token_starts_prefix_expr(TokenKind k) {
    switch (k) {
    case TOK_INT_LIT: case TOK_FLOAT_LIT: case TOK_STRING_LIT:
    case TOK_CSTRING_LIT: case TOK_CHAR_LIT:
    case TOK_INTERP_START: case TOK_CINTERP_START:
    case TOK_TRUE: case TOK_FALSE: case TOK_VOID:
    case TOK_IDENT: case TOK_TYPE_VAR:
    case TOK_SOME: case TOK_NONE: case TOK_OK: case TOK_ERR:
    case TOK_ALLOC: case TOK_ALLOCA: case TOK_FREE:
    case TOK_SIZEOF: case TOK_ALIGNOF: case TOK_BITCAST: case TOK_ENUM_OF:
    case TOK_DEFAULT:
    case TOK_ASSERT: case TOK_STATIC_ASSERT: case TOK_ERROR_NAME:
    case TOK_ATOMIC_LOAD: case TOK_ATOMIC_STORE:
    case TOK_GUARDED: case TOK_UNGUARDED: case TOK_CHECKED: case TOK_UNCHECKED:
    case TOK_IF: case TOK_MATCH: case TOK_LOOP: case TOK_FOR: case TOK_LET:
    case TOK_LPAREN: case TOK_LBRACE: case TOK_LT:
    case TOK_MINUS: case TOK_BANG: case TOK_TILDE: case TOK_AMP: case TOK_STAR:
        return true;
    default:
        return false;
    }
}

/* alloc(...) or alloca(...): a type, a type with a count, a slice literal,
 * or an expression to copy. */
static Expr *parse_alloc(Parser *p, SrcLoc loc) {
    bool is_stack = (current(p)->kind == TOK_ALLOCA);
    advance_p(p);
    expect(p, TOK_LPAREN);

    /* Decide type vs expression. Built-in types, void, error, const and type
     * variables can only begin a type. An unknown identifier followed by '['
     * or ',' is a type (alloc(T[N] { }), alloc(T, n)); a bare one followed by
     * ')' is parsed as an expression, and pass2 tells type names from
     * variables. An unknown identifier followed by '<' or '.' gets a tentative
     * type parse that backtracks when no ')', '[' or ',' follows the type. */
    Token *first = current(p);
    /* `alloc(const str[n] { })` allocates n slots holding read-only views,
     * the heap twin of the `const str[N] { ... }` literal. Read as an
     * expression it would be a nested slice literal, which needs a
     * compile-time length. */
    bool is_type = (first->kind == TOK_VOID || first->kind == TOK_TYPE_VAR ||
                    first->kind == TOK_ERROR_KW || first->kind == TOK_CONST);
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
                /* Module-qualified name: a type (alloc(shapes.point)) or a
                 * value (alloc(cfg.origin)), which only name resolution can
                 * tell apart. A name followed by ')' stays an expression and
                 * pass2 decides it against the symbol table; anything else
                 * gets the tentative type parse. */
                int off = 1;
                while (peek_at(p, off)->kind == TOK_DOT &&
                       peek_at(p, off + 1)->kind == TOK_IDENT)
                    off += 2;
                if (peek_at(p, off)->kind != TOK_RPAREN) {
                    try_type = true;
                    save = p->pos;
                }
            } else if (next->kind == TOK_LT) {
                /* Generic type args or a comparison: try the type first */
                try_type = true;
                save = p->pos;
            }
        }
    }

    if (is_type || try_type) {
        /* A tentative type parse holds its diagnostics until a type form
         * (`)`, `[` or `,`) confirms it. */
        int held = try_type ? diag_speculate_begin() : 0;
        Type *ty = parse_type(p);
        if (try_type)
            diag_speculate_end(held, check(p, TOK_RPAREN) || check(p, TOK_LBRACKET) ||
                                     check(p, TOK_COMMA));
        if (check(p, TOK_RPAREN)) {
            /* alloc(T): bare type alloc */
            advance_p(p);
            Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
            e->alloc_expr.alloc_type = ty;
            e->alloc_expr.size_expr = NULL;
            e->alloc_expr.init_expr = NULL;
            e->alloc_expr.is_stack = is_stack;
            return e;
        }
        if (check(p, TOK_LBRACKET)) {
            /* alloc(T[N] { elems }): allocate a slice */
            advance_p(p);
            Expr *size = parse_bracketed_expr(p, PREC_NONE + 1);
            expect(p, TOK_RBRACKET);
            int elem_count = 0;
            Expr **elems = NULL;
            if (check(p, TOK_LBRACE)) {
                advance_p(p);
                elems = parse_slice_elems(p, &elem_count);
            } else {
                /* Read on as if the empty `{ }` were there. */
                diag_error(loc_from_token(current(p)),
                           "expected '{' after alloc(T[N]; use alloc(T[N] { })");
            }
            expect(p, TOK_RPAREN);
            /* Runtime-sized alloc: alloc(T[n] { }) where n is not a literal */
            if (size->kind != EXPR_INT_LIT) {
                if (elem_count > 0)
                    diag_error(loc, "alloc with runtime size cannot have explicit elements");
                Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
                e->alloc_expr.alloc_type = ty;
                e->alloc_expr.size_expr = size;
                e->alloc_expr.init_expr = NULL;
                e->alloc_expr.is_stack = is_stack;
                return e;
            }
            Expr *arr = alloc_expr(p, EXPR_ARRAY_LIT, loc);
            arr->array_lit.elem_type = ty;
            arr->array_lit.size_expr = size;
            arr->array_lit.elems = elems;
            arr->array_lit.elem_count = elem_count;
            Expr *e = alloc_expr(p, EXPR_ALLOC, loc);
            e->alloc_expr.alloc_type = NULL;
            e->alloc_expr.size_expr = NULL;
            e->alloc_expr.init_expr = arr;
            e->alloc_expr.is_stack = is_stack;
            return e;
        }
        if (check(p, TOK_COMMA)) {
            /* alloc(T, N): raw buffer alloc */
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
            /* No type form follows: backtrack to the expression reading */
            restore_pos(p, save);
        } else {
            diag_error(loc, "expected ')', '[', or ',' after type in alloc");
            return alloc_expr_error(p, loc);
        }
    }

    /* alloc(expr) / alloca(expr): initialized alloc. An interpolated-string
     * init is marked `wrapped` and a plain (cstr) cast init `licensed`. Both
     * permit an otherwise illegal runtime-sized result, which the enclosing
     * alloc/alloca gives a home (heap or dynamic stack). */
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

/* An expression starting with `(`: a slice literal with a parenthesized
 * element type, a lambda, a cast, or a parenthesized expression or
 * sequence. */
static Expr *parse_paren_prefix(Parser *p, SrcLoc loc) {
    /* Slice literal with a parenthesized (grouped or function) element type:
     * `((A) -> B)[N] { e0, ... }` or `((A) -> B)[] { ptr = .., len = .. }`.
     * Detected by the shape "( ... ) [ ... ] {": the { after ] distinguishes
     * it from a cast or an indexed parenthesized expression, neither of which
     * is followed by a brace here (as in the tuple-element path of the
     * TOK_LBRACE case). */
    if (at_slice_lit_brackets(p, skip_group(p, 0, TOK_LPAREN, TOK_RPAREN))) {
        Type *elem_type = parse_type(p); /* parses the grouped/function type */
        return parse_array_lit_body(p, elem_type, loc);
    }

    /* Disambiguate: function literal vs parenthesized expression */
    /* () -> ... : empty-param function */
    if (peek_at(p, 1)->kind == TOK_RPAREN && peek_at(p, 2)->kind == TOK_ARROW) {
        return parse_func_literal(p);
    }
    /* (ident : ...) -> ... : function with params. A keyword before the ':'
     * is a misnamed parameter, reported by the parameter parse. */
    if ((peek_at(p, 1)->kind == TOK_IDENT || is_keyword_token(peek_at(p, 1))) &&
        peek_at(p, 2)->kind == TOK_COLON) {
        return parse_func_literal(p);
    }
    /* (Type)expr: cast. Tried when the next token is a built-in type name, a
     * type var, `error` or `const`, and for (IDENT* ...) and (IDENT< ...),
     * which cover user-defined pointer and generic types. For module-qualified
     * casts like (mod.type*)expr, scan past the dots for a type suffix
     * (*, ?, [, !); a bare (mod.name) is a parenthesized field access.
     * Backtracking handles false positives like (a * b). */
    {
    bool try_cast = false;
    bool bang_cast = false; /* triggered only by `!`; see the guard below */
    if (peek_at(p, 1)->kind == TOK_IDENT &&
        (type_from_name(peek_at(p, 1)->start, peek_at(p, 1)->length) ||
         peek_at(p, 2)->kind == TOK_STAR ||
         peek_at(p, 2)->kind == TOK_LT)) {
        try_cast = true;
    } else if (peek_at(p, 1)->kind == TOK_IDENT && peek_at(p, 2)->kind == TOK_BANG) {
        /* (IDENT!): result-type cast. Unlike (a*), `x!` is a complete
         * expression (postfix unwrap), so this trigger alone is ambiguous
         * with a parenthesized unwrap; the RPAREN guard below resolves it. */
        try_cast = true;
        bang_cast = true;
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
            if (after == TOK_BANG) {
                /* (mod.type!): same unwrap ambiguity as (IDENT!) above */
                try_cast = true;
                bang_cast = true;
            }
            /* A plain (mod.type) is not tried: whether the last name is a
             * type is unknown here, and (a.b) is a field access. */
        }
    }
    if (try_cast || peek_at(p, 1)->kind == TOK_TYPE_VAR ||
        peek_at(p, 1)->kind == TOK_ERROR_KW ||
        peek_at(p, 1)->kind == TOK_CONST ||
        /* ((const T)[]) x: a parenthesized element type under a suffix.
         * `const` never starts an expression, so `((const` can only be a
         * type; every other `((` stays an expression and is not probed. */
        (peek_at(p, 1)->kind == TOK_LPAREN && peek_at(p, 2)->kind == TOK_CONST)) {
        /* Try to parse as cast with backtracking. Diagnostics from the type
         * parse are held until the cast is confirmed. */
        int save = p->pos;
        int held = diag_speculate_begin();
        advance_p(p); /* ( */
        Type *target = parse_type(p);
        /* (cstr[N]): bounded str-to-cstr cast. cstr is u8*, and with
         * allow_fixed_array off in expression context parse_type leaves the
         * [N] for this code to claim. */
        int buffer_size = 0;
        if (is_cstr_type(target) && check(p, TOK_LBRACKET) &&
            peek_at(p, 1)->kind == TOK_INT_LIT && peek_at(p, 2)->kind == TOK_RBRACKET) {
            advance_p(p); /* [ */
            Token *nt = current(p);
            bool oor = false;
            int64_t n = parse_int_value(nt->start, nt->length, &oor);
            if (oor || n < 1) {
                diag_error(loc_from_token(nt),
                           "(cstr[N]) buffer size must be a positive integer, got %lld",
                           (long long)n);
                n = 1;
            }
            advance_p(p); /* N */
            advance_p(p); /* ] */
            buffer_size = (int)n;
        }
        if (check(p, TOK_RPAREN) &&
            !(bang_cast && !token_starts_prefix_expr(peek_at(p, 1)->kind))) {
            /* A `!`-triggered attempt commits to the cast only when the token
             * after `)` starts an expression; otherwise `(x!)` is a
             * parenthesized unwrap, as in `f((x!))` or `(x!) == y`. `(x!) e`
             * is a cast to type `x!`, meaningless when `x` isn't a type, like
             * `(a*) b`. */
            diag_speculate_end(held, true);
            advance_p(p);
            Expr *operand = parse_expr(p, PREC_PREFIX);
            Expr *e = alloc_expr(p, EXPR_CAST, loc);
            e->cast.target = target;
            e->cast.operand = operand;
            e->cast.buffer_size = buffer_size;
            return e;
        }
        /* Not a cast: backtrack */
        diag_speculate_end(held, false);
        restore_pos(p, save);
    }
    } /* end try_cast block */
    /* Parenthesized expression, optionally a `;`-sequence: `(a; b)` is a
       sequence expression yielding the last item's value, the others
       evaluated for effect (same as a block).  A single item is plain
       grouping: `(a)` is `a`.  Layout is suppressed inside brackets, so `;`
       is the only separator and `)` bounds the sequence. */
    advance_p(p);
    Expr **stmts = NULL;
    int len = 0, cap = 0;
    DA_APPEND(stmts, len, cap, parse_bracketed_expr(p, PREC_NONE + 1));
    while (check(p, TOK_SEMICOLON)) {
        while (check(p, TOK_SEMICOLON)) advance_p(p);
        if (check(p, TOK_RPAREN)) break;
        DA_APPEND(stmts, len, cap, parse_bracketed_expr(p, PREC_NONE + 1));
    }
    expect(p, TOK_RPAREN);
    Expr *e = block_or_single(p, arena_copy_exprs(p, stmts, len), len, loc);
    free(stmts);
    return e;
}

/* An interpolated string literal: its literal text and each format segment
 * with its expression. */
static Expr *parse_interp_string(Parser *p, Token *t, SrcLoc loc) {
    advance_p(p); /* consume INTERP_START / CINTERP_START */
    InterpSegment *segs = NULL;
    int seg_count = 0, seg_cap = 0;

    /* Add leading literal text segment */
    InterpSegment lit_seg = {0};
    lit_seg.is_literal = true;
    lit_seg.text = t->start;
    lit_seg.text_length = t->length;
    DA_APPEND(segs, seg_count, seg_cap, lit_seg);

    for (;;) {
        /* Expect FMT_SPEC */
        Token *fmt = expect(p, TOK_FMT_SPEC);
        InterpSegment fmt_seg = {0};
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
            InterpSegment mid_seg = {0};
            mid_seg.is_literal = true;
            mid_seg.text = mid->start;
            mid_seg.text_length = mid->length;
            DA_APPEND(segs, seg_count, seg_cap, mid_seg);
            continue;
        }

        if (check(p, TOK_INTERP_END)) {
            Token *end = advance_p(p);
            InterpSegment end_seg = {0};
            end_seg.is_literal = true;
            end_seg.text = end->start;
            end_seg.text_length = end->length;
            DA_APPEND(segs, seg_count, seg_cap, end_seg);
            break;
        }

        diag_error(loc, "expected interpolation continuation or end, got %s",
            token_kind_name(current(p)->kind));
        /* Resync past the rest of the string. */
        while (!check(p, TOK_INTERP_END) && !check(p, TOK_NEWLINE) && !at_end_p(p))
            advance_p(p);
        if (check(p, TOK_INTERP_END)) advance_p(p);
        free(segs);
        return alloc_expr_error(p, loc);
    }

    InterpSegment *arena_segs = arena_dup(p->arena, segs, seg_count, sizeof *segs);
    free(segs);

    Expr *e = alloc_expr(p, EXPR_INTERP_STRING, loc);
    e->interp_string.segments = arena_segs;
    e->interp_string.segment_count = seg_count;
    e->interp_string.is_cstr = (t->kind == TOK_CINTERP_START);
    return e;
}

/* A float literal, with its f32/f64 suffix and the overflow and underflow
 * the value reading detects. */
static Expr *parse_float_lit(Parser *p, Token *t, SrcLoc loc) {
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
    /* Sized from the token, as in parse_int_value: dropping the tail of a
     * float is silently wrong rather than an error, since cutting
     * `0.00...01e300` before its exponent yields 0.0 with neither ERANGE nor
     * an out-of-range flag. */
    char *buf = xmalloc((size_t)num_end + 1);
    int num_len = 0;
    bool mantissa_nonzero = false;
    bool past_mantissa = false;
    bool is_hex = t->length >= 2 && t->start[0] == '0' && (t->start[1] == 'x' || t->start[1] == 'X');
    for (int i = 0; i < num_end; i++) {
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
    free(buf);
    bool oor = false, underflow = false;
    /* strtod sets ERANGE on overflow (returning +-HUGE_VAL) and may also
     * set it on underflow even for subnormal results. Distinguish by
     * checking the result: only +-inf is overflow; only 0 from a nonzero
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

/* A for loop over a range or a slice, with an index, element or
 * destructuring binding. */
static Expr *parse_for_expr(Parser *p, SrcLoc loc) {
    advance_p(p);
    const char *var = NULL;
    Pattern *var_pattern = NULL;
    const char *index_var = NULL;
    SrcLoc var_loc = {0}, index_var_loc = {0};
    /* The element binding may be a destructure pattern (`{ ... }`); the
     * index, when present, is always a plain identifier. */
    if (check(p, TOK_LBRACE)) {
        var_pattern = parse_pattern(p);
    } else {
        Token *vt = expect(p, TOK_IDENT);
        var = tok_intern(p, vt);
        var_loc = tok_loc(p, vt);
        if (check(p, TOK_COMMA)) {
            advance_p(p);
            /* first was index, second is element (ident or destructure) */
            index_var = var;
            index_var_loc = var_loc;
            var = NULL;
            var_loc = (SrcLoc){0};
            if (check(p, TOK_LBRACE)) {
                var_pattern = parse_pattern(p);
            } else {
                Token *vt2 = expect(p, TOK_IDENT);
                var = tok_intern(p, vt2);
                var_loc = tok_loc(p, vt2);
            }
        }
    }
    expect(p, TOK_IN);
    Expr *iter = parse_expr(p, PREC_NONE + 1);
    Expr *range_end = NULL;
    if (check(p, TOK_DOTDOT)) {
        advance_p(p);
        range_end = parse_expr(p, PREC_NONE + 1);
    }
    /* `do` is required and marks the header/body boundary, uniformly with
       `then`/`with`. As a block-former it admits either an indented block
       or an inline body, so `parse_body` handles both forms. */
    expect(p, TOK_DO);
    int body_count;
    Expr **body = parse_body(p, &body_count);
    Expr *e = alloc_expr(p, EXPR_FOR, loc);
    e->for_expr.var = var;
    e->for_expr.var_pattern = var_pattern;
    e->for_expr.index_var = index_var;
    e->for_expr.var_loc = var_loc;
    e->for_expr.index_var_loc = index_var_loc;
    e->for_expr.iter = iter;
    e->for_expr.range_end = range_end;
    e->for_expr.body = body;
    e->for_expr.body_count = body_count;
    return e;
}

/* An expression starting with an identifier: a slice literal, a struct
 * literal (possibly module-qualified), a raw-parts str literal, or a plain
 * name. */
static Expr *parse_ident_prefix(Parser *p, Token *t, SrcLoc loc) {
    const char *name = tok_intern(p, t);

    /* Slice literal: type_name[size] { ... }, for built-in, user-defined,
     * module-qualified or generic element types alike. at_slice_literal
     * decides the shape; parse_type parses the element type. */
    if (at_slice_literal(p)) {
        Type *elem_type = parse_type(p);
        return parse_array_lit_body(p, elem_type, loc);
    }

    /* Struct literal: name { field = expr, ... }, recognized by `IDENT =` or
     * `}` after the brace. */
    if (at_struct_lit_brace(p, 1)) {
        advance_p(p);  /* consume ident */
        advance_p(p);  /* consume { */
        /* str { ptr = expr, len = expr }: raw-parts literal of type str */
        if (strcmp(name, "str") == 0)
            return parse_raw_slice_fields(p, type_uint8(), loc);
        return parse_struct_literal(p, name, loc);
    }
    /* Check for module-qualified struct literal: mod.name { ... }, mod.sub.name { ... }
     * Scan ahead past (. IDENT)* to find a { that looks like a struct literal */
    if (peek_at(p, 1)->kind == TOK_DOT && peek_at(p, 2)->kind == TOK_IDENT) {
        int ahead = 1; /* start after first IDENT */
        while (peek_at(p, ahead)->kind == TOK_DOT &&
               peek_at(p, ahead + 1)->kind == TOK_IDENT) {
            ahead += 2; /* skip . IDENT */
        }
        if (at_struct_lit_brace(p, ahead)) {
            /* Build dotted name by consuming IDENT (. IDENT)* */
            const char *dotted_name = name;
            advance_p(p); /* consume first IDENT */
            while (check(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
                advance_p(p); /* consume . */
                Token *seg = current(p);
                dotted_name = intern_sprintf(p->intern, "%s.%.*s", dotted_name,
                                             seg->length, seg->start);
                advance_p(p); /* consume IDENT */
            }
            advance_p(p); /* consume { */
            return parse_struct_literal(p, dotted_name, loc);
        }
    }
    advance_p(p);
    Expr *e = alloc_expr(p, EXPR_IDENT, loc);
    e->ident.name = name;
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
        e->int_lit.lit_type = int_lit_type(p, t);
        e->int_lit.out_of_range = int_oor;
        return e;
    }

    case TOK_FLOAT_LIT:
        return parse_float_lit(p, t, loc);

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
    case TOK_INTERP_START:
        return parse_interp_string(p, t, loc);

    case TOK_CHAR_LIT: {
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_CHAR_LIT, loc);
        e->char_lit.value = char_lit_value(t);
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

    case TOK_IDENT:
        return parse_ident_prefix(p, t, loc);

    case TOK_LPAREN:
        return parse_paren_prefix(p, loc);

    case TOK_LBRACE: {
        /* Slice literal with a tuple element type: {T1, T2}[N] { ... }.
         * Detected by the shape "{ ... } [ ... ] {": the { after ] distinguishes
         * it from an indexed tuple literal ({a, b}[i], which is never followed
         * by a brace). */
        if (at_slice_lit_brackets(p, skip_group(p, 0, TOK_LBRACE, TOK_RBRACE))) {
            Type *elem_type = parse_type(p);   /* parses the {T1, T2, ...} tuple type */
            return parse_array_lit_body(p, elem_type, loc);
        }

        /* Bare positional braces: anonymous tuple literal { e0, e1, ... } (>= 2
         * elements). Struct and slice literals always carry a type prefix, so a
         * leading { is a tuple; blocks are indentation-based, never braced. */
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
        if (elem_count < 2) {
            diag_error(loc, "tuple literal requires at least 2 elements, got %d", elem_count);
            free(elems);
            return alloc_expr_error(p, loc);
        }
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
        /* `none(T)` is sugar for `default(T?)`: it builds the same
         * EXPR_DEFAULT node over the option type, so the two spellings share
         * all downstream handling. A bare `none` (e.g. `x == none`) carries no
         * type and is rejected with a targeted message, like a bare `void`. */
        advance_p(p);
        if (!check(p, TOK_LPAREN)) {
            diag_error(loc, "'none' needs a type argument; write none(T) "
                "(or test an option with .is_none / a 'none' match arm)");
            return alloc_expr_error(p, loc);
        }
        advance_p(p); /* ( */
        Type *ty = parse_type(p);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_DEFAULT, loc);
        e->default_expr.target = type_option(p->arena, ty);
        return e;
    }

    case TOK_OK: {
        advance_p(p);
        /* Bare `ok` (no parens) constructs the payload-less void! result,
         * the expression twin of the bare `ok` pattern (written like a
         * payload-less variant such as `| empty`). No void value
         * materializes: the ok arm of void! has no payload. */
        if (!check(p, TOK_LPAREN)) {
            Expr *e = alloc_expr(p, EXPR_OK, loc);
            e->ok_expr.value = NULL;
            return e;
        }
        advance_p(p); /* ( */
        Expr *val = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_OK, loc);
        e->ok_expr.value = val;
        return e;
    }

    case TOK_ERR: {
        /* err(T, code): T is the ok-payload type (the result is T!) and code
         * is the i32 error code. Type-anchored like none(T) and bitcast(T, x),
         * since directional inference cannot find the payload type from the
         * code alone. A bare `err` is rejected like a bare `none`. */
        advance_p(p);
        if (!check(p, TOK_LPAREN)) {
            diag_error(loc, "'err' needs a type and a code; write err(T, code) "
                "(or test a result with .is_err / an 'err' match arm)");
            return alloc_expr_error(p, loc);
        }
        advance_p(p); /* ( */
        Type *ty = parse_type(p);
        expect(p, TOK_COMMA);
        Expr *code = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ERR, loc);
        e->err_expr.target = ty;
        e->err_expr.code = code;
        return e;
    }

    case TOK_LET:
        /* A let in expression position must bound its scope with an explicit
           `in` body (the offside form only exists at statement position). */
        return parse_let_binding(p, true);

    case TOK_LOOP: {
        advance_p(p);
        /* `loop` has no header expression to delimit, so it takes its body
           directly, with no `do` keyword: a block after INDENT, otherwise an
           inline `;`-separated sequence. */
        int body_count;
        Expr **body = parse_body(p, &body_count);
        Expr *e = alloc_expr(p, EXPR_LOOP, loc);
        e->loop_expr.body = body;
        e->loop_expr.body_count = body_count;
        return e;
    }

    case TOK_GUARDED:
        advance_p(p);
        return parse_guard(p, false, true, loc);

    case TOK_UNGUARDED:
        advance_p(p);
        return parse_guard(p, false, false, loc);

    case TOK_CHECKED:
        advance_p(p);
        return parse_guard(p, true, true, loc);

    case TOK_UNCHECKED:
        advance_p(p);
        return parse_guard(p, true, false, loc);

    case TOK_FOR:
        return parse_for_expr(p, loc);

    case TOK_VOID: {
        /* void![N] {...}: slice literal with the payload-less result element
         * type, like the built-in type names (error[N]{...}). */
        if (peek_at(p, 1)->kind == TOK_BANG &&
            peek_at(p, 2)->kind == TOK_LBRACKET) {
            advance_p(p); /* void */
            advance_p(p); /* ! */
            return parse_array_lit_body(p, type_result(p->arena, type_void()), loc);
        }
        /* `void()` is the void-typed expression. `void` alone is valid only
         * in type position, so the parentheses are required here, with a
         * targeted diagnostic when they are missing. */
        advance_p(p);
        if (!check(p, TOK_LPAREN)) {
            diag_error(loc, "'void' is a type; use 'void()' to produce a void value");
            return alloc_expr_error(p, loc);
        }
        advance_p(p); /* ( */
        if (!check(p, TOK_RPAREN)) {
            diag_error(loc_from_token(current(p)), "void() takes no arguments");
            return alloc_expr_error(p, loc);
        }
        advance_p(p); /* ) */
        return alloc_expr(p, EXPR_VOID_LIT, loc);
    }

    case TOK_SIZEOF: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Type *ty = parse_type_allowing_fixed_array(p);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_SIZEOF, loc);
        e->sizeof_expr.target = ty;
        return e;
    }

    case TOK_ERROR_NAME: {
        /* error_name(e): the str? name of a declared error code */
        advance_p(p);
        expect(p, TOK_LPAREN);
        Expr *code = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ERROR_NAME, loc);
        e->error_name_expr.code = code;
        return e;
    }

    case TOK_ERROR_KW: {
        /* `error` in expression position: only as the element type of a
         * slice literal (error[N]{...} / error[]{...}), like the built-in
         * numeric type names. */
        if (peek_at(p, 1)->kind == TOK_LBRACKET) {
            advance_p(p);
            return parse_array_lit_body(p, type_error_code(), loc);
        }
        diag_error(loc, "'error' is a type/declaration keyword and cannot "
            "appear bare in an expression");
        advance_p(p); /* leaf-bump: guarantee progress */
        return alloc_expr_error(p, loc);
    }

    case TOK_ALIGNOF: {
        advance_p(p);
        expect(p, TOK_LPAREN);
        Type *ty = parse_type_allowing_fixed_array(p);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ALIGNOF, loc);
        e->alignof_expr.target = ty;
        return e;
    }

    case TOK_BITCAST: {
        /* bitcast(T, x): a type argument (like sizeof) plus a value (like the
         * second arg of atomic_store). Reinterprets x's bytes as T. */
        advance_p(p);
        expect(p, TOK_LPAREN);
        Type *ty = parse_type(p);
        expect(p, TOK_COMMA);
        Expr *operand = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_BITCAST, loc);
        e->bitcast_expr.target = ty;
        e->bitcast_expr.operand = operand;
        return e;
    }

    case TOK_ENUM_OF: {
        /* enum_of(E, x): a type argument plus a value, like bitcast. The
         * membership-checked integer-to-enum conversion; yields E?. */
        advance_p(p);
        expect(p, TOK_LPAREN);
        Type *ty = parse_type(p);
        expect(p, TOK_COMMA);
        Expr *operand = parse_bracketed_expr(p, PREC_NONE + 1);
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ENUM_OF, loc);
        e->enum_of_expr.target = ty;
        e->enum_of_expr.operand = operand;
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

    case TOK_STATIC_ASSERT: {
        Expr *sa_cond; const char *sa_msg; SrcLoc sa_loc;
        if (parse_static_assert_line(p, &sa_cond, &sa_msg, &sa_loc)) {
            Expr *e = alloc_expr(p, EXPR_STATIC_ASSERT, sa_loc);
            e->static_assert_expr.condition = sa_cond;
            e->static_assert_expr.msg = sa_msg;
            return e;
        }
        return alloc_expr_error(p, loc);
    }

    case TOK_ASSERT: {
        advance_p(p);
        int errs_before = diag_error_count();
        expect(p, TOK_LPAREN);
        Token *cond_start = current(p);
        Expr *condition = parse_bracketed_expr(p, PREC_NONE + 1);
        int text_len;
        const char *text = source_text(p, cond_start, current(p), errs_before, &text_len);
        Expr *message = NULL;
        if (check(p, TOK_COMMA)) {
            advance_p(p);
            message = parse_bracketed_expr(p, PREC_NONE + 1);
        }
        expect(p, TOK_RPAREN);
        Expr *e = alloc_expr(p, EXPR_ASSERT, loc);
        e->assert_expr.condition = condition;
        e->assert_expr.message = message;
        e->assert_expr.expr_text = text;
        e->assert_expr.expr_text_len = text_len;
        return e;
    }

    case TOK_ALLOC:
    case TOK_ALLOCA:
        return parse_alloc(p, loc);

    /* <'a, 'b>(params) -> body : generic function literal with explicit type vars */
    case TOK_LT: {
        if (peek_at(p, 1)->kind != TOK_TYPE_VAR) {
            diag_error(loc, "unexpected '<' in expression");
            advance_p(p); /* leaf-bump past '<' */
            return alloc_expr_error(p, loc);
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
        func->func.explicit_type_vars = arena_dup(p->arena, tvars, tv_count, sizeof(const char*));
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
         * substituted per-monomorphization. A bare 'a[expr] with no following
         * '{' is not a value and falls through to the type-var-ref path, which
         * pass2 rejects. */
        if (at_slice_literal(p)) {
            Type *elem_type = parse_type(p);
            return parse_array_lit_body(p, elem_type, loc);
        }
        /* Allow 'a in expression position for type-variable property access ('a.min etc.) */
        advance_p(p);
        Expr *e = alloc_expr(p, EXPR_TYPE_VAR_REF, loc);
        e->type_var_ref.name = tok_intern(p, t);
        return e;
    }

    case TOK_CONST:
        /* `const T[N] { ... }`: a slice literal over a const element type. It
         * is the only spelling that can hold string literals (a string literal
         * is a `const str`, which does not narrow to `str`). A `const`
         * anywhere else in expression position is not a value and gets the
         * default diagnostic. */
        if (at_slice_literal(p)) {
            Type *elem_type = parse_type(p);
            return parse_array_lit_body(p, elem_type, loc);
        }
        goto unexpected;

    default:
    unexpected:
        diag_error(loc, "unexpected token %s in expression",
            token_kind_name(t->kind));
        /* Leaf-bump: consume the offending token (unless it's a hard stop the
           enclosing loop must keep) so the parser always makes progress. */
        if (!is_hard_stop(t->kind)) advance_p(p);
        return alloc_expr_error(p, loc);
    }
}

/* ---- Infix parsing ---- */

/* The right operand of binary operator `op` (at `loc`), after `left`. */
static Expr *parse_binary_rhs(Parser *p, Expr *left, TokenKind op, SrcLoc loc) {
    Expr *right = parse_expr(p, infix_prec(op) + 1);
    Expr *e = alloc_expr(p, EXPR_BINARY, loc);
    e->binary.op = op;
    e->binary.left = left;
    e->binary.right = right;
    return e;
}

/* A `<` after `left` read as type arguments: a generic call or generic
 * variant construction, a bare instantiation in value position, or a struct
 * literal wrongly given type arguments. NULL when it is a comparison. */
static Expr *parse_generic_suffix(Parser *p, Expr *left, SrcLoc loc) {
    /* `left<Types> { ... }`: a struct literal carrying explicit type
     * arguments, which FC does not have (the field values determine them).
     * Diagnose the form itself, then parse the literal as if the arguments
     * were absent so the rest of the expression still type-checks. Read as a
     * comparison, the braces would become a tuple literal and the error would
     * land on them. The shape is unambiguous, so it needs no generic-name
     * gate. */
    if (left->kind == EXPR_IDENT || left->kind == EXPR_FIELD) {
        const char *lit_name = struct_lit_typearg_scan(p, p->pos)
                             ? dotted_name_of(p, left) : NULL;
        if (lit_name) {
            diag_error(loc, "explicit type arguments are not allowed on a "
                            "struct literal; write '%s { ... }', since the field "
                            "values determine the type arguments", lit_name);
            int ignored;
            parse_type_arg_list(p, &ignored);
            expect_typearg_gt(p);
            expect(p, TOK_LBRACE);
            return parse_struct_literal(p, lit_name, left->loc);
        }
    }

    /* A generic call left<Type, ...>(args), or variant construction
     * left<Type, ...>.variant. The Pratt loop has consumed the '<'. The
     * callee must name a known generic declaration (gate_generic_name), so
     * an ordinary comparison whose operand happens to scan as a type-argument
     * list keeps its comparison reading. */
    if ((left->kind == EXPR_IDENT || left->kind == EXPR_FIELD) &&
        gate_generic_name(p, left)) {
        if (generic_call_scan(p, p->pos)) {
            int ta_count;
            Type **type_args = parse_type_arg_list(p, &ta_count);
            expect_typearg_gt(p);

            if (check(p, TOK_DOT)) {
                /* name<Types>.variant: generic union variant construction */
                advance_p(p); /* consume '.' */
                Token *field = expect(p, TOK_IDENT);
                const char *vname = tok_intern(p, field);

                /* The type args ride on the field for pass2 to resolve. */
                Expr *fld = alloc_expr(p, EXPR_FIELD, loc);
                fld->field.object = left;
                fld->field.name = vname;
                fld->field.name_loc = tok_loc(p, field);
                fld->field.type_args = type_args;
                fld->field.type_arg_count = ta_count;
                if (!check(p, TOK_LPAREN)) return fld;  /* no payload */

                /* Payload variant: name<Types>.variant(arg) */
                advance_p(p);
                Expr *e = alloc_expr(p, EXPR_CALL, loc);
                e->call.func = fld;
                e->call.args = parse_call_args(p, &e->call.arg_count);
                expect(p, TOK_RPAREN);
                e->call.type_args = type_args;
                e->call.type_arg_count = ta_count;
                return e;
            }

            /* Generic function call: name<Types>(args) */
            expect(p, TOK_LPAREN);
            Expr *e = alloc_expr(p, EXPR_CALL, loc);
            e->call.func = left;
            e->call.args = parse_call_args(p, &e->call.arg_count);
            expect(p, TOK_RPAREN);
            e->call.type_args = type_args;
            e->call.type_arg_count = ta_count;
            return e;
        }
        if (bare_inst_scan(p, p->pos)) {
            /* `name<Types>` with no '(': explicit type args in value
             * position. Consume the type args and build a marked EXPR_CALL
             * with no arguments; pass2 rejects it with a located diagnostic
             * (a generic function or type cannot be a value). */
            Expr *e = alloc_expr(p, EXPR_CALL, loc);
            e->call.func = left;
            e->call.type_args = parse_type_arg_list(p, &e->call.type_arg_count);
            expect_typearg_gt(p);
            e->call.bare_inst = true;
            return e;
        }
    }
    return NULL;
}

static Expr *parse_infix(Parser *p, Expr *left, Token *op_tok) {
    SrcLoc loc = loc_from_token(op_tok);
    TokenKind op = op_tok->kind;

    switch (op) {
    case TOK_BANG: {
        Expr *e = alloc_expr(p, EXPR_UNARY_POSTFIX, loc);
        e->unary_postfix.op = TOK_BANG;
        e->unary_postfix.operand = left;
        /* The operand's source text, for the unwrap failure message. */
        e->unary_postfix.expr_text = source_text(p, &p->tokens[p->expr_start_pos], op_tok,
                                                 p->expr_start_errs,
                                                 &e->unary_postfix.expr_text_len);
        return e;
    }

    case TOK_QUESTION: {
        /* x?: propagation. Unwrap on success, early-return the failure
         * (err/none) from the enclosing function otherwise. No operand text
         * capture: propagation has no abort message. */
        Expr *e = alloc_expr(p, EXPR_UNARY_POSTFIX, loc);
        e->unary_postfix.op = TOK_QUESTION;
        e->unary_postfix.operand = left;
        return e;
    }

    case TOK_DOT: {
        Token *field = expect(p, TOK_IDENT);
        Expr *e = alloc_expr(p, EXPR_FIELD, loc);
        e->field.object = left;
        e->field.name = tok_intern(p, field);
        e->field.name_loc = tok_loc(p, field);
        return e;
    }

    case TOK_ARROW: {
        /* `->` is not member access in FC: `.` auto-derefs one level
           (`p.field`). Report that and consume the field name so the rest of
           the expression still parses. A match arm's or a lambda's `->` never
           reaches here: block_arm_arrow and the lambda parser stop before it. */
        diag_error(loc, "'->' is not pointer field access in FC; use '.' (e.g. p.field)");
        if (check(p, TOK_IDENT)) advance_p(p);
        (void)left;
        return alloc_expr_error(p, loc);
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
        Expr *e = alloc_expr(p, EXPR_CALL, loc);
        e->call.func = left;
        e->call.args = parse_call_args(p, &e->call.arg_count);
        expect(p, TOK_RPAREN);
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
        Expr *generic = parse_generic_suffix(p, left, loc);
        if (generic) return generic;
        return parse_binary_rhs(p, left, op, loc);
    }

    default:
        return parse_binary_rhs(p, left, op, loc);
    }
}

static Expr *parse_expr(Parser *p, Prec min_prec) {
    int saved_start = p->expr_start_pos;
    int saved_errs = p->expr_start_errs;
    p->expr_start_pos = p->pos;
    p->expr_start_errs = diag_error_count();
    Expr *left = parse_prefix(p);
    for (;;) {
        Token *t = current(p);
        /* Inside a `when` guard, a top-level `->` ends the guard: it separates
           the arm's body. */
        if (p->block_arm_arrow && t->kind == TOK_ARROW) break;
        Prec prec = infix_prec(t->kind);
        /* A generic call name<T,...>(args) is a call, so its '<' binds at
         * call precedence, not comparison precedence. Otherwise the callee
         * would be taken as the operand of a tighter-binding operator on its
         * left (`1 + size_of<i32>()`, `!is_big<i32>()`) and the '<' would
         * never be reached with the bare name as `left`. */
        if (t->kind == TOK_LT &&
            (left->kind == EXPR_IDENT || left->kind == EXPR_FIELD) &&
            gate_generic_name(p, left) &&
            generic_call_scan(p, p->pos + 1))
            prec = PREC_POSTFIX;
        if (prec == PREC_NONE || prec < min_prec) break;
        Token *op_tok = advance_p(p);
        left = parse_infix(p, left, op_tok);
    }
    p->expr_start_pos = saved_start;
    p->expr_start_errs = saved_errs;
    return left;
}

/* ---- Pattern parsing ---- */

static Pattern *parse_pattern(Parser *p);

static Pattern *parse_pattern_atom(Parser *p) {
    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->loc = tok_loc(p, current(p));

    /* Negative integer pattern: -42 */
    if (check(p, TOK_MINUS) && peek_at(p, 1)->kind == TOK_INT_LIT) {
        SrcLoc loc = loc_from_token(current(p));
        advance_p(p); /* consume - */
        Token *t = advance_p(p);
        Type *lt = int_lit_type(p, t);
        if (type_is_unsigned(lt))
            diag_error(loc, "cannot negate unsigned integer literal");
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
        pat->int_lit.lit_type = int_lit_type(p, t);
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
        pat->char_lit.value = char_lit_value(t2);
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

    if (check(p, TOK_OK)) {
        advance_p(p);
        pat->kind = PAT_OK;
        if (check(p, TOK_LPAREN)) {
            advance_p(p);
            pat->some_pat.inner = parse_pattern(p);
            expect(p, TOK_RPAREN);
        } else {
            /* Bare `ok` matches the payload-less ok of a void! result
             * (pass2 rejects it on non-void results, and ok(<pat>) on void!). */
            pat->some_pat.inner = NULL;
        }
        return pat;
    }

    if (check(p, TOK_ERR)) {
        /* err(<pat>): the inner pattern matches the i32 code (a literal,
         * binding, or wildcard). */
        advance_p(p);
        expect(p, TOK_LPAREN);
        pat->kind = PAT_ERR;
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
            pat->struc.fields = arena_dup(p->arena, fields, count, sizeof(FieldPattern));
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
        pat->tuple_pat.patterns = arena_dup(p->arena, pats, count, sizeof(Pattern*));
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

        /* Qualified constant path, group.member or mod.group.member: a
         * declared error constant. Qualification is required (a bare member
         * name would be indistinguishable from a binding); pass2 resolves the
         * path and rewrites the node to PAT_INT_LIT with the assigned code. */
        if (peek_at(p, 1)->kind == TOK_DOT && peek_at(p, 2)->kind == TOK_IDENT) {
            const char **parts = NULL;
            int count = 0, cap = 0;
            DA_APPEND(parts, count, cap, name);
            advance_p(p); /* consume first ident */
            while (check(p, TOK_DOT) && peek_at(p, 1)->kind == TOK_IDENT) {
                advance_p(p); /* consume . */
                DA_APPEND(parts, count, cap, tok_intern(p, current(p)));
                advance_p(p); /* consume ident */
            }
            pat->kind = PAT_CONST_PATH;
            pat->const_path.parts = arena_dup(p->arena, parts, count, sizeof(const char*));
            pat->const_path.part_count = count;
            free(parts);
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
    diag_error(loc, "expected pattern, got %s", token_kind_name(current(p)->kind));
    if (!is_hard_stop(current(p)->kind)) advance_p(p);  /* leaf-bump */
    return alloc_pat_error(p, loc);
}

typedef struct {
    Pattern **items;
    int count;
    int cap;
} PatList;

/* Add an or-pattern alternative, splicing in the alternatives of a nested
 * or-pattern so the result stays flat: (a | b) | c has three. */
static void add_alt(PatList *alts, Pattern *alt) {
    if (alt->kind == PAT_OR) {
        for (int i = 0; i < alt->or_pat.alt_count; i++)
            DA_APPEND(alts->items, alts->count, alts->cap, alt->or_pat.alts[i]);
    } else {
        DA_APPEND(alts->items, alts->count, alts->cap, alt);
    }
}

static Pattern *make_or_pattern(Parser *p, PatList *alts, SrcLoc loc) {
    Pattern *pat = arena_alloc(p->arena, sizeof(Pattern));
    pat->kind = PAT_OR;
    pat->loc = loc;
    pat->or_pat.alts = arena_dup(p->arena, alts->items, alts->count, sizeof(Pattern *));
    pat->or_pat.alt_count = alts->count;
    free(alts->items);
    return pat;
}

/* A pattern, or an or-pattern of alternatives joined by `|`. */
static Pattern *parse_pattern(Parser *p) {
    Pattern *first = parse_pattern_atom(p);
    if (!check(p, TOK_PIPE)) return first;
    PatList alts = {0};
    add_alt(&alts, first);
    while (check(p, TOK_PIPE)) {
        advance_p(p); /* consume | */
        add_alt(&alts, parse_pattern_atom(p));
    }
    return make_or_pattern(p, &alts, first->loc);
}

/* ---- Match expression parsing ---- */

/* The or-pattern of a run of body-less arms (`| a` `| b`) and the arm with the
 * body that ends the run. */
static Pattern *or_prepend(Parser *p, Pattern **buffered, int buf_count, Pattern *tail) {
    PatList alts = {0};
    for (int i = 0; i < buf_count; i++) add_alt(&alts, buffered[i]);
    add_alt(&alts, tail);
    return make_or_pattern(p, &alts, buffered[0]->loc);
}

static Expr *parse_match_expr(Parser *p) {
    SrcLoc loc = loc_from_token(current(p));
    expect(p, TOK_MATCH);

    /* Clear the outer `block_arm_arrow` for the duration: a match nested in
       another match's guard parses its own subject, guards and arm bodies,
       whose `->` must not end the outer guard. */
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

    /* Recovery anchor: resync a malformed arm to the next arm's `|` (or the
       match's DEDENT, where recover_to always stops). */
    const TokenKind arm_sync[] = { TOK_PIPE };

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        int guard = p->pos;
        expect(p, TOK_PIPE);
        MatchArm arm;
        arm.loc = tok_loc(p, current(p));
        arm.pattern = parse_pattern(p);
        arm.guard = NULL;

        if (arm.pattern->kind == PAT_ERROR) {
            /* Malformed pattern: neither buffer it (that would corrupt the
               or-chain) nor add an arm; resync to the next arm. */
            recover_to(p, arm_sync, 1);
            recover_progress(p, guard);
            continue;
        }

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
                diag_error(when_loc,
                    "'when' guard requires an arm body: expected '->' after guard expression");
                recover_to(p, arm_sync, 1);
                recover_progress(p, guard);
                continue;
            }
        }

        if (!check(p, TOK_ARROW)) {
            /* Body-less arm: buffer as an or-alternative for the next arm. */
            last_body_less_loc = arm.loc;
            DA_APPEND(buffered, buf_count, buf_cap, arm.pattern);
            recover_progress(p, guard);
            continue;
        }

        if (buf_count > 0) {
            arm.pattern = or_prepend(p, buffered, buf_count, arm.pattern);
            buf_count = 0;
        }

        advance_p(p); /* consume -> */

        /* Parse arm body. An empty body after `->` is an error; `void()` is
           the no-op arm's spelling. */
        int body_errs = diag_error_count();
        arm.body = parse_body(p, &arm.body_count);

        /* An inline arm body goes through parse_inline_seq, which, unlike
           parse_block, does not require a separator between juxtaposed
           statements. In `| 3 -> n = 1 n = 2` the `n = 2` is left unclaimed,
           and the arm loop would report it as a cascade of "expected '|'"
           errors. Report the separator error a block would, then resync to
           the next arm. Skipped if the body itself failed (its leftovers are
           that error's debris). */
        if (diag_error_count() == body_errs && !at_stmt_terminator(p)) {
            diag_error(loc_from_token(current(p)),
                "expected a newline or ';' between statements, got %s",
                token_kind_name(current(p)->kind));
            recover_to(p, arm_sync, 1);
        }

        DA_APPEND(arms, arm_count, arm_cap, arm);
        recover_progress(p, guard);
        skip_newlines(p);
    }
    if (buf_count > 0) {
        /* Recovery: a trailing body-less or-pattern with no closing `-> body`.
           Report it and discard the buffered alternatives (no synthetic arm). */
        diag_error(last_body_less_loc,
                   "unterminated or-pattern: body-less arm must be followed by an arm with '->'");
    }
    free(buffered);
    expect(p, TOK_DEDENT);

    p->block_arm_arrow = saved_block;

    Expr *e = alloc_expr(p, EXPR_MATCH, loc);
    e->match_expr.subject = subject;
    e->match_expr.arm_count = arm_count;
    e->match_expr.arms = arena_dup(p->arena, arms, arm_count, sizeof(MatchArm));
    free(arms);
    return e;
}

/* ---- Struct literal parsing ---- */

static Expr *parse_struct_literal(Parser *p, const char *type_name, SrcLoc loc) {
    /* The caller has consumed the type name and the '{' */
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
    e->struct_lit.fields = arena_dup(p->arena, fields, field_count, sizeof(FieldInit));
    free(fields);
    return e;
}

/* ---- Top-level declaration parsing ---- */

typedef struct {
    Decl **items;
    int count;
    int cap;
} DeclList;

static Decl *parse_decl(Parser *p, DeclList *out);

static Decl *decl_list_add(DeclList *l, Decl *d) {
    DA_APPEND(l->items, l->count, l->cap, d);
    return d;
}

/* Append a parsed declaration unless it is the error node standing in for a
 * malformed one. */
static Decl *add_parsed(DeclList *l, Decl *d) {
    return d->kind == DECL_ERROR ? d : decl_list_add(l, d);
}

/* Move a finished list into the arena (NULL when empty). */
static Decl **decl_list_finish(Parser *p, DeclList *l) {
    Decl **decls = arena_dup(p->arena, l->items, l->count, sizeof(Decl*));
    free(l->items);
    return decls;
}

static Decl *parse_let_decl(Parser *p) {
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_LET);

    bool is_mut = false;
    if (check(p, TOK_MUT)) {
        advance_p(p);
        is_mut = true;
    }

    const char *name = tok_intern(p, expect_name(p));
    expect(p, TOK_EQ);

    Expr *init;
    if (check(p, TOK_INDENT)) {
        /* Block body for let binding */
        int count;
        Expr **body = parse_block(p, &count);
        init = block_or_single(p, body, count, loc);
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

/* Parse `static_assert(cond, "message")`. The message must be a string
 * literal (no computation on the failure path). Returns false if the leading
 * keyword is absent. Used in struct/union bodies (collected as instantiation
 * predicates) and via parse_prefix in statement position. */
static bool parse_static_assert_line(Parser *p, Expr **out_cond, const char **out_msg, SrcLoc *out_loc) {
    if (!check(p, TOK_STATIC_ASSERT)) return false;
    SrcLoc loc = tok_loc(p, current(p));
    advance_p(p);
    expect(p, TOK_LPAREN);
    Expr *cond = parse_bracketed_expr(p, PREC_NONE + 1);
    expect(p, TOK_COMMA);
    const char *msg = NULL;
    if (check(p, TOK_STRING_LIT)) {
        Expr *m = parse_expr(p, PREC_NONE + 1);
        if (m && m->kind == EXPR_STRING_LIT)
            msg = arena_strdup(p->arena, m->string_lit.value, m->string_lit.length);
    }
    if (!msg) {
        diag_error(loc_from_token(current(p)),
            "static_assert message must be a string literal");
        /* consume up to ')' so the line recovers */
        while (!check(p, TOK_RPAREN) && !check(p, TOK_NEWLINE) && !at_end_p(p))
            advance_p(p);
        msg = "";
    }
    expect(p, TOK_RPAREN);
    *out_cond = cond;
    *out_msg = msg;
    *out_loc = loc;
    return true;
}

/* A struct/union/enum/error body must open with its own indented block. When
   the header is followed straight by a newline (an empty body), the member
   loop would run on into the next declaration and produce a cascade of
   errors. Report one diagnostic (suppressed if the header already failed) and
   return false so the caller returns a DECL_ERROR. Returns true when a body is
   present. */
static bool decl_body_present(Parser *p, const char *kind, const char *name,
                              const char *item, SrcLoc loc, int errs0) {
    if (check(p, TOK_INDENT)) return true;
    if (diag_error_count() == errs0)
        diag_error(loc, "%s '%s' must declare at least one %s", kind, name, item);
    return false;
}

/* The `name: type` lines of a struct body, after its INDENT and through its
 * DEDENT. An FC struct (not an extern one) may also hold static_assert lines. */
static void parse_struct_body(Parser *p, Decl *d, bool allow_static_assert) {
    StructField *fields = NULL;
    int field_count = 0, field_cap = 0;
    StaticAssert *sasserts = NULL;
    int sassert_count = 0, sassert_cap = 0;
    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;
        int guard = p->pos;
        int line_errs = diag_error_count();
        Expr *sa_cond; const char *sa_msg; SrcLoc sa_loc;
        if (allow_static_assert && parse_static_assert_line(p, &sa_cond, &sa_msg, &sa_loc)) {
            StaticAssert sa = { .cond = sa_cond, .msg = sa_msg, .loc = sa_loc, .owner = d->struc.name };
            DA_APPEND(sasserts, sassert_count, sassert_cap, sa);
        } else {
            Token *ftok = expect_name(p);
            StructField f = { .name = tok_intern(p, ftok), .loc = tok_loc(p, ftok) };
            expect(p, TOK_COLON);
            f.type = parse_type_allowing_fixed_array(p);
            DA_APPEND(fields, field_count, field_cap, f);
        }
        end_body_line(p, line_errs);
        recover_progress(p, guard);  /* a fully malformed line consumes nothing */
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);
    d->struc.field_count = field_count;
    d->struc.fields = arena_dup(p->arena, fields, field_count, sizeof(StructField));
    free(fields);
    d->struc.static_assert_count = sassert_count;
    d->struc.static_asserts = arena_dup(p->arena, sasserts, sassert_count, sizeof(StaticAssert));
    free(sasserts);
}

static Decl *parse_struct_decl(Parser *p) {
    int errs0 = diag_error_count();
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_STRUCT);
    const char *name = tok_intern(p, expect_name(p));
    expect(p, TOK_EQ);

    if (!decl_body_present(p, "struct", name, "field", loc, errs0))
        return alloc_decl_error(p, loc);
    expect(p, TOK_INDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_STRUCT;
    d->loc = loc;
    d->struc.name = name;
    parse_struct_body(p, d, true);
    return d;
}

static Decl *parse_union_decl(Parser *p) {
    int errs0 = diag_error_count();
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_UNION);
    const char *name = tok_intern(p, expect_name(p));
    expect(p, TOK_EQ);

    /* Expect INDENT then | variant(type) lines */
    if (!decl_body_present(p, "union", name, "variant", loc, errs0))
        return alloc_decl_error(p, loc);
    expect(p, TOK_INDENT);

    UnionVariant *variants = NULL;
    int variant_count = 0, variant_cap = 0;
    StaticAssert *sasserts = NULL;
    int sassert_count = 0, sassert_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        int guard = p->pos;
        int line_errs = diag_error_count();
        {
            Expr *sa_cond; const char *sa_msg; SrcLoc sa_loc;
            if (parse_static_assert_line(p, &sa_cond, &sa_msg, &sa_loc)) {
                StaticAssert sa = { .cond = sa_cond, .msg = sa_msg, .loc = sa_loc, .owner = name };
                DA_APPEND(sasserts, sassert_count, sassert_cap, sa);
                recover_progress(p, guard);
                skip_newlines(p);
                continue;
            }
        }
        expect(p, TOK_PIPE);
        Token *vtok = expect_name(p);
        const char *vname = tok_intern(p, vtok);
        SrcLoc vloc = tok_loc(p, vtok);
        Type *payload = NULL;
        if (check(p, TOK_LPAREN)) {
            advance_p(p);
            payload = parse_type(p);
            expect(p, TOK_RPAREN);
        }

        UnionVariant v = { .name = vname, .payload = payload, .loc = vloc };
        DA_APPEND(variants, variant_count, variant_cap, v);
        end_body_line(p, line_errs);
        recover_progress(p, guard);  /* a fully malformed variant line consumes nothing */
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_UNION;
    d->loc = loc;
    d->unio.name = name;
    d->unio.variant_count = variant_count;
    d->unio.variants = arena_dup(p->arena, variants, variant_count, sizeof(UnionVariant));
    free(variants);
    d->unio.static_assert_count = sassert_count;
    d->unio.static_asserts = arena_dup(p->arena, sasserts, sassert_count, sizeof(StaticAssert));
    free(sasserts);
    return d;
}

/* enum <name> [of <repr>] =
 *     | variant [= value]
 *     | variant
 *
 * Closed set of named integer constants over a declared fixed-width repr
 * (default i32). `of` is contextual (an ordinary identifier here), not a
 * reserved word. Values are integer literals (unary minus allowed); omitted
 * values continue C-style from the previous one. pass1 resolves/validates the
 * values (repr fit, duplicates, mandatory zero variant). */
static Decl *parse_enum_decl(Parser *p) {
    int errs0 = diag_error_count();
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_ENUM);
    const char *name = tok_intern(p, expect_name(p));

    if (check(p, TOK_LT)) {
        diag_error(loc_from_token(current(p)), "enum types take no type parameters");
    }

    Type *repr = NULL;
    if (check(p, TOK_IDENT) && current(p)->length == 2 &&
        memcmp(current(p)->start, "of", 2) == 0) {
        advance_p(p);
        Token *rt = expect(p, TOK_IDENT);
        if (rt->kind == TOK_IDENT) {
            Type *r = type_from_name(rt->start, rt->length);
            if (r && r->kind >= TYPE_INT8 && r->kind <= TYPE_UINT64) {
                repr = r;
            } else {
                diag_error(loc_from_token(rt),
                    "enum repr must be a fixed-width integer type (i8..u64), got '%.*s'",
                    rt->length, rt->start);
            }
        }
    }

    expect(p, TOK_EQ);
    if (!decl_body_present(p, "enum", name, "variant", loc, errs0))
        return alloc_decl_error(p, loc);
    expect(p, TOK_INDENT);

    EnumVariant *variants = NULL;
    int variant_count = 0, variant_cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        int guard = p->pos;
        int line_errs = diag_error_count();
        expect(p, TOK_PIPE);
        Token *vtok = expect_name(p);
        const char *vname = tok_intern(p, vtok);
        SrcLoc vloc = tok_loc(p, vtok);
        if (check(p, TOK_LPAREN)) {
            diag_error(loc_from_token(current(p)),
                "enum variants carry no payload; use a union for variants with data");
            /* Consume the (type) so recovery doesn't cascade a second error. */
            advance_p(p);
            parse_type(p);
            if (check(p, TOK_RPAREN)) advance_p(p);
        }

        EnumVariant v = { .name = vname, .loc = vloc };
        if (check(p, TOK_EQ)) {
            advance_p(p);
            v.has_explicit = true;
            if (check(p, TOK_MINUS)) {
                advance_p(p);
                v.negative = true;
            }
            Token *nt = expect(p, TOK_INT_LIT);
            if (nt->kind == TOK_INT_LIT) {
                for (int i = 0; i < nt->length; i++) {
                    if (nt->start[i] == 'i' || nt->start[i] == 'u') {
                        diag_error(loc_from_token(nt),
                            "enum values take no type suffix; the enum declares its repr");
                        break;
                    }
                }
                bool oor = false;
                v.value_bits = parse_int_value(nt->start, nt->length, &oor);
                if (oor)
                    diag_error(loc_from_token(nt), "enum value out of range");
            }
        }
        DA_APPEND(variants, variant_count, variant_cap, v);
        end_body_line(p, line_errs);
        recover_progress(p, guard);  /* a fully malformed variant line consumes nothing */
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_ENUM;
    d->loc = loc;
    d->enu.name = name;
    d->enu.repr = repr;
    d->enu.variant_count = variant_count;
    d->enu.variants = arena_dup(p->arena, variants, variant_count, sizeof(EnumVariant));
    free(variants);
    return d;
}

/* error <name> =
 *     | member
 *     | member
 *
 * Error-group declaration. Desugared here to a DECL_MODULE flagged
 * is_error_group whose members are synthesized immutable i32-const lets, so
 * pass1 registration, imports, member access, privacy, the LSP features and
 * codegen all reuse the module machinery. Each member's init is an
 * EXPR_INT_LIT placeholder whose value pass1 assigns once the whole program
 * has been collected (sorted fully-qualified names, numbered from
 * FC_ERROR_CODE_BASE). */
static Decl *parse_error_decl(Parser *p) {
    int errs0 = diag_error_count();
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_ERROR_KW);
    const char *name = tok_intern(p, expect_name(p));
    expect(p, TOK_EQ);

    /* Expect INDENT then | member lines (union-style layout, no payloads) */
    if (!decl_body_present(p, "error group", name, "member", loc, errs0))
        return alloc_decl_error(p, loc);
    expect(p, TOK_INDENT);

    Decl **members = NULL;
    int count = 0, cap = 0;

    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;

        int guard = p->pos;
        int line_errs = diag_error_count();
        expect(p, TOK_PIPE);
        Token *mtok = expect_name(p);
        const char *mname = tok_intern(p, mtok);
        SrcLoc mloc = tok_loc(p, mtok);
        if (check(p, TOK_LPAREN)) {
            diag_error(loc_from_token(current(p)),
                "error members carry no payload; the assigned code itself is the value");
        }

        Expr *init = alloc_expr(p, EXPR_INT_LIT, mloc);
        init->int_lit.value = 0;  /* patched by pass1's error-code assignment */
        init->int_lit.lit_type = type_error_code();

        Decl *m = arena_alloc(p->arena, sizeof(Decl));
        m->kind = DECL_LET;
        m->loc = mloc;
        m->let.name = mname;
        m->let.init = init;
        DA_APPEND(members, count, cap, m);
        end_body_line(p, line_errs);
        recover_progress(p, guard);  /* a fully malformed member line consumes nothing */
        skip_newlines(p);
    }
    expect(p, TOK_DEDENT);

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_MODULE;
    d->loc = loc;
    d->module.name = name;
    d->module.is_error_group = true;
    d->module.decl_count = count;
    d->module.decls = arena_dup(p->arena, members, count, sizeof(Decl*));
    free(members);
    return d;
}

static Decl *parse_module_decl(Parser *p) {
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_MODULE);
    const char *name = tok_intern(p, expect_name(p));

    /* Optional from "lib" clause. On a mismatch `expect` returns the
     * offending token unconsumed, so strip quotes only from a real string
     * token: slicing a 1-char token by length-2 would underflow the intern
     * length. */
    const char *from_lib = NULL;
    if (check(p, TOK_FROM)) {
        advance_p(p);
        Token *lib_tok = expect(p, TOK_STRING_LIT);
        if (lib_tok->kind == TOK_STRING_LIT)
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
        if (macro_tok->kind == TOK_STRING_LIT)
            define_macro = intern(p->intern, macro_tok->start + 1, macro_tok->length - 2);
        Token *value_tok = expect(p, TOK_STRING_LIT);
        if (value_tok->kind == TOK_STRING_LIT)
            define_value = intern(p->intern, value_tok->start + 1, value_tok->length - 2);
    }

    expect(p, TOK_EQ);

    /* Parse body: INDENT { decl } DEDENT */
    expect(p, TOK_INDENT);

    DeclList members = {0};
    while (!check(p, TOK_DEDENT) && !at_end_p(p)) {
        skip_newlines(p);
        if (check(p, TOK_DEDENT)) break;
        int guard = p->pos;
        if (parse_decl(p, &members)->kind == DECL_ERROR) {
            /* Sync to the next module-member declaration; recover_to_decl stops at the
               module's DEDENT, so recovery cannot escape this module body. */
            recover_to_decl(p);
        }
        recover_progress(p, guard);
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
    d->module.decl_count = members.count;
    d->module.decls = decl_list_finish(p, &members);
    return d;
}

/* What a `from` clause names: an optional namespace, the head module, and the
 * dotted route of nested modules written after it. */
typedef struct {
    const char *ns;
    const char *mod;          /* head module; NULL for a bare `from ns::` */
    SrcLoc mod_loc;           /* head token; zeroed when there is no head */
    ImportRouteSeg *route;    /* segments after the head, arena-allocated */
    int route_count;
} FromClause;

/* The `.b.c` tail of a `from` route. `from a.b.c` names module `a`'s nested
 * module `b`'s nested module `c`: every segment after the head is a member of
 * its predecessor, the same navigation `.` performs in expression position.
 * Leaves the route empty when no dot follows. */
static void parse_from_route(Parser *p, FromClause *fc) {
    ImportRouteSeg *segs = NULL;
    int count = 0, cap = 0;
    while (check(p, TOK_DOT)) {
        advance_p(p);
        Token *t = expect(p, TOK_IDENT);
        ImportRouteSeg seg = { tok_intern(p, t), tok_loc(p, t), NULL };
        DA_APPEND(segs, count, cap, seg);
    }
    fc->route = arena_dup(p->arena, segs, count, sizeof *segs);
    fc->route_count = count;
    free(segs);
}

/* Parse a from clause: from [namespace::path::]module[.module...] */
static void parse_from_clause(Parser *p, FromClause *fc) {
    /* Parse IDENT [:: IDENT [:: ...]]
     * The IDENT that is not followed by :: is the head module and everything
     * before it the namespace; a path ending in :: is a bare namespace (no
     * head). A `.` route may follow the head. */
    memset(fc, 0, sizeof *fc);
    Token *first_tok = expect(p, TOK_IDENT);
    const char *first = tok_intern(p, first_tok);

    if (!check(p, TOK_COLONCOLON)) {
        /* Simple: from module_name */
        fc->mod = first;
        fc->mod_loc = tok_loc(p, first_tok);
        parse_from_route(p, fc);
        return;
    }

    /* Build namespace path: ident::ident::... */
    const char *ns = first;

    while (check(p, TOK_COLONCOLON)) {
        advance_p(p); /* consume :: */
        if (check(p, TOK_IDENT)) {
            Token *part_tok = expect(p, TOK_IDENT);
            const char *part = tok_intern(p, part_tok);
            if (!check(p, TOK_COLONCOLON)) {
                /* This IDENT is not followed by ::, so it's the head module */
                fc->ns = ns;
                fc->mod = part;
                fc->mod_loc = tok_loc(p, part_tok);
                parse_from_route(p, fc);
                return;
            }
            /* More :: follows, so this is still namespace. Segments are
             * joined with __ (which no identifier contains), so foo::bar
             * and foo_bar stay distinct. */
            ns = intern_sprintf(p->intern, "%s__%s", ns, part);
        } else {
            /* Bare namespace ending: from acme:: or from acme::graphics:: */
            fc->ns = ns;
            return;
        }
    }

    /* Not reached: every path through the loop returns */
    fc->ns = ns;
}

/* The optional `as ALIAS` tail of an import item. Returns NULL (leaving
 * *out_loc zeroed) when there is no alias. Shared by both forms that name a
 * single imported thing: a lone `name` and a comma-list item. */
static const char *parse_import_alias(Parser *p, SrcLoc *out_loc) {
    memset(out_loc, 0, sizeof *out_loc);
    if (!check(p, TOK_AS)) return NULL;
    advance_p(p);
    Token *t = expect(p, TOK_IDENT);
    *out_loc = tok_loc(p, t);
    return tok_intern(p, t);
}

/* An import statement is `import <names> from [<ns>::]<route>`: it always has
 * a `from`, and only its right side is a path. The right side is a route to
 * one module, dotted like the same navigation in expression position; the
 * left side lists the names being bound, where a dot would give each item a
 * different source and put routing where a binding name belongs. So
 * `import a.b` is an error: it names module `a`'s member `b` on the side that
 * binds names, and means `import b from a`.
 *
 * The rejection consumes the whole statement (the dotted run, an `as` alias
 * and any `from` clause), so a malformed left side yields one diagnostic
 * rather than one per segment plus a cascade from the tail. */
static void reject_dotted_import(Parser *p, SrcLoc loc, const char *head) {
    const char *first = NULL;
    int dots = 0;
    while (check(p, TOK_DOT)) {
        advance_p(p);
        const char *seg = tok_intern(p, expect(p, TOK_IDENT));
        if (!first) first = seg;
        dots++;
    }
    SrcLoc discard;
    parse_import_alias(p, &discard);
    bool had_from = check(p, TOK_FROM);
    if (had_from) {
        advance_p(p);
        FromClause fc;
        parse_from_clause(p, &fc);
    }
    /* `import a.b` has one unambiguous rewrite, so quote it. With a `from`
     * already present the fix is to extend that route, and the head written
     * here need not be reachable on its own, so name the rule instead of
     * inventing a path that may not resolve. */
    if (dots == 1 && !had_from) {
        diag_error(loc, "'.' is not allowed on the left of 'from'; "
            "use 'import %s from %s'", first, head);
    } else {
        diag_error(loc, "'.' is not allowed on the left of 'from'; the left names "
            "what is imported, so the module path belongs on the right of 'from'");
    }
}

/* One DECL_IMPORT. `name` is NULL for `import *`. */
static Decl *import_decl(Parser *p, SrcLoc loc, const char *name, SrcLoc name_loc,
                         const char *alias, SrcLoc alias_loc, const FromClause *fc) {
    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_IMPORT;
    d->loc = loc;
    d->import.name = name;
    d->import.alias = alias;
    d->import.from_module = fc->mod;
    d->import.from_namespace = fc->ns;
    d->import.is_wildcard = name == NULL;
    d->import.name_loc = name_loc;
    d->import.alias_loc = alias_loc;
    d->import.module_loc = fc->mod_loc;
    d->import.route = fc->route;
    d->import.route_count = fc->route_count;
    return d;
}

/* An import statement. A comma list (`import a, b as c from m`) declares one
 * DECL_IMPORT per name, in order, all sharing the one `from` clause: pass1
 * resolves the shared route the same way for each. */
static Decl *parse_import_decl(Parser *p, DeclList *out) {
    SrcLoc loc = tok_loc(p, current(p));
    const SrcLoc none = {0};
    expect(p, TOK_IMPORT);

    /* import * from [ns::]MODULE[.MODULE...] */
    if (check(p, TOK_STAR)) {
        advance_p(p);
        expect(p, TOK_FROM);
        FromClause fc;
        parse_from_clause(p, &fc);
        return decl_list_add(out, import_decl(p, loc, NULL, none, NULL, none, &fc));
    }

    /* Parse first name [as alias] */
    Token *name_tok = expect(p, TOK_IDENT);
    const char *name = tok_intern(p, name_tok);
    SrcLoc name_loc = tok_loc(p, name_tok);
    SrcLoc alias_loc = {0};

    if (check(p, TOK_DOT)) {
        reject_dotted_import(p, loc, name);
        parse_import_alias(p, &alias_loc); /* swallow a trailing `as A` too */
        return alloc_decl_error(p, loc);
    }
    const char *alias = parse_import_alias(p, &alias_loc);

    if (!check(p, TOK_COMMA)) {
        /* Single import with optional from clause */
        FromClause fc = {0};
        if (check(p, TOK_FROM)) {
            advance_p(p);
            parse_from_clause(p, &fc);
        }
        return decl_list_add(out, import_decl(p, loc, name, name_loc, alias, alias_loc, &fc));
    }

    /* name1 [as a1], name2 [as a2], ... from mod */
    typedef struct { const char *n; const char *a; SrcLoc nl, al; } ImportItem;
    ImportItem *items = NULL;
    int item_count = 0, item_cap = 0;
    ImportItem first = { name, alias, name_loc, alias_loc };
    DA_APPEND(items, item_count, item_cap, first);
    while (check(p, TOK_COMMA)) {
        advance_p(p);
        Token *n_tok = expect(p, TOK_IDENT);
        ImportItem it = { tok_intern(p, n_tok), NULL, tok_loc(p, n_tok), {0} };
        it.a = parse_import_alias(p, &it.al);
        DA_APPEND(items, item_count, item_cap, it);
    }
    expect(p, TOK_FROM);
    FromClause fc;
    parse_from_clause(p, &fc);
    Decl *head = NULL;
    for (int i = 0; i < item_count; i++) {
        Decl *d = decl_list_add(out, import_decl(p, loc, items[i].n, items[i].nl,
                                                 items[i].a, items[i].al, &fc));
        if (!head) head = d;
    }
    free(items);
    return head;
}

static Decl *parse_namespace_decl(Parser *p) {
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_NAMESPACE);

    /* namespace IDENT :: [IDENT ::] ... */
    const char *name = tok_intern(p, expect(p, TOK_IDENT));

    while (check(p, TOK_COLONCOLON)) {
        advance_p(p);
        if (check(p, TOK_IDENT)) {
            const char *part = tok_intern(p, expect(p, TOK_IDENT));
            name = intern_sprintf(p->intern, "%s__%s", name, part);
        }
    }

    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_NAMESPACE;
    d->loc = loc;
    d->is_private = false;
    d->ns.name = name;
    return d;
}

/* Parse the error-protocol tail of an extern declaration: `from <protocol>`.
 * The sentinel tokens (-1 / null / 0) are protocol-local syntax, not
 * expressions; `null` exists only here and is not a null literal in the
 * language. Returns EXT_PROTO_ERROR after reporting a malformed clause, so
 * pass1 skips the agreement checks (no cascade). */
static ExternProtocol parse_extern_protocol(Parser *p) {
    Token *t = current(p);
    SrcLoc loc = tok_loc(p, t);
    if (t->kind != TOK_IDENT) {
        diag_error(loc, "expected error protocol name after 'from' "
            "(errno(-1), errno(null), status, neg_errno, hresult, "
            "last_error(<sentinel>), wsa_error(-1)), got %s",
            token_kind_name(t->kind));
        return EXT_PROTO_ERROR;
    }
    const char *name = tok_intern(p, t);
    advance_p(p);

    /* Sentinel argument: -1, 0, or null. SENT_NONE = no parens present. */
    enum { SENT_NONE, SENT_NEG1, SENT_ZERO, SENT_NULL, SENT_BAD } sent = SENT_NONE;
    if (check(p, TOK_LPAREN)) {
        advance_p(p);
        Token *s = current(p);
        if (s->kind == TOK_MINUS && peek_at(p, 1)->kind == TOK_INT_LIT &&
            peek_at(p, 1)->length == 1 && peek_at(p, 1)->start[0] == '1') {
            advance_p(p); advance_p(p);
            sent = SENT_NEG1;
        } else if (s->kind == TOK_INT_LIT && s->length == 1 && s->start[0] == '0') {
            advance_p(p);
            sent = SENT_ZERO;
        } else if (s->kind == TOK_IDENT && strncmp(s->start, "null", 4) == 0 &&
                   s->length == 4) {
            advance_p(p);
            sent = SENT_NULL;
        } else {
            diag_error(loc, "invalid protocol sentinel; expected -1, 0, or null");
            sent = SENT_BAD;
            /* leaf-bump so the item loop makes progress on garbage */
            if (!check(p, TOK_RPAREN) && !check(p, TOK_NEWLINE) &&
                !check(p, TOK_DEDENT) && !at_end_p(p))
                advance_p(p);
        }
        expect(p, TOK_RPAREN);
        if (sent == SENT_BAD) return EXT_PROTO_ERROR;
    }

    if (strcmp(name, "errno") == 0) {
        if (sent == SENT_NEG1) return EXT_PROTO_ERRNO_NEG1;
        if (sent == SENT_NULL) return EXT_PROTO_ERRNO_NULL;
        diag_error(loc, "errno protocol needs its sentinel: errno(-1) or errno(null)");
        return EXT_PROTO_ERROR;
    }
    if (strcmp(name, "last_error") == 0) {
        if (sent == SENT_ZERO) return EXT_PROTO_LASTERR_0;
        if (sent == SENT_NULL) return EXT_PROTO_LASTERR_NULL;
        if (sent == SENT_NEG1) return EXT_PROTO_LASTERR_NEG1;
        diag_error(loc, "last_error protocol needs its sentinel: "
            "last_error(0), last_error(null), or last_error(-1)");
        return EXT_PROTO_ERROR;
    }
    if (strcmp(name, "wsa_error") == 0) {
        if (sent == SENT_NEG1) return EXT_PROTO_WSA_NEG1;
        diag_error(loc, "wsa_error protocol takes exactly the -1 sentinel: wsa_error(-1)");
        return EXT_PROTO_ERROR;
    }
    ExternProtocol bare = strcmp(name, "status") == 0    ? EXT_PROTO_STATUS
                        : strcmp(name, "neg_errno") == 0 ? EXT_PROTO_NEG_ERRNO
                        : strcmp(name, "hresult") == 0   ? EXT_PROTO_HRESULT
                        : EXT_PROTO_ERROR;
    if (bare == EXT_PROTO_ERROR) {
        diag_error(loc, "unknown error protocol '%s'; valid protocols: errno(-1), "
            "errno(null), status, neg_errno, hresult, last_error(<sentinel>), "
            "wsa_error(-1)", name);
        return EXT_PROTO_ERROR;
    }
    if (sent != SENT_NONE) {
        diag_error(loc, "protocol '%s' takes no sentinel (the failure test is "
            "fixed: %s)", name,
            bare == EXT_PROTO_STATUS ? "ret != 0" : "ret < 0");
        return EXT_PROTO_ERROR;
    }
    return bare;
}

/* Reject an extern C name under `fc__`, the reserved root every FC
 * declaration's emitted name lives under (mangle_root, pass1.c). Nothing a
 * header can legitimately export starts with `fc__`, so such an extern could
 * only alias a compiler-emitted symbol. For names that exist only after
 * monomorphization (`fc__pair__3_i32`) it would do so silently, since
 * pass1's check_c_name_collisions never sees them. Returns true when it
 * reported. */
static bool extern_c_name_in_reserved_root(SrcLoc loc, const char *c_name) {
    if (!is_mangled_root_name(c_name)) return false;
    diag_error(loc, "extern C name '%s' starts with 'fc__', the reserved root "
        "every FC declaration is emitted under, so it could only alias a "
        "compiler-emitted symbol; refer to the FC declaration directly instead",
        c_name);
    return true;
}

static Decl *parse_extern_decl(Parser *p) {
    SrcLoc loc = tok_loc(p, current(p));
    expect(p, TOK_EXTERN);

    /* extern struct/union: C struct or union layout import */
    bool is_c_union = check(p, TOK_UNION);
    if (check(p, TOK_STRUCT) || is_c_union) {
        advance_p(p);
        const char *c_name = tok_intern(p, expect(p, TOK_IDENT));
        const char *fc_name = c_name;
        if (check(p, TOK_AS)) {
            advance_p(p);
            fc_name = tok_intern(p, expect(p, TOK_IDENT));
        }
        /* Same rules as extern functions: nothing under the reserved `fc__`
         * emission root (see extern_c_name_in_reserved_root), and a '__' C tag
         * needs a clean FC alias. */
        if (extern_c_name_in_reserved_root(loc, c_name)) {
            /* reported; keep parsing the body */
        } else if (fc_name == c_name && strstr(c_name, "__") != NULL) {
            diag_error(loc, "extern C name '%s' contains '__', which is reserved in "
                "FC names; give it an alias: `extern %s %s as <name> = ...`",
                c_name, is_c_union ? "union" : "struct", c_name);
        }
        expect(p, TOK_EQ);
        expect(p, TOK_INDENT);
        Decl *d = arena_alloc(p->arena, sizeof(Decl));
        d->kind = DECL_STRUCT;
        d->loc = loc;
        d->struc.name = fc_name;
        d->struc.c_name = c_name;
        d->struc.is_extern = true;
        d->struc.is_c_union = is_c_union;
        parse_struct_body(p, d, false);
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
    /* Nothing under the reserved emission root, alias or not. */
    bool in_reserved_root = extern_c_name_in_reserved_root(loc, name);
    /* A reserved-identifier C name (free, default, sizeof, ...) is accepted only
     * with an alias: the bare name is a keyword in FC and would be unreferenceable.
     * Require `extern <name> as <ident>: ...` in that case. */
    if (!alias && name_tok->kind != TOK_IDENT) {
        diag_error(loc, "extern declaration uses reserved name '%s', which would be "
            "unreferenceable; give it an alias: `extern %s as <name>: ...`", name, name);
    }
    /* A C name containing '__' (implementation-reserved namespace, e.g.
     * __errno_location) is emitted verbatim, but the FC-visible name must not
     * contain the mangling separator, so require an alias. */
    if (!in_reserved_root && !alias && strstr(name, "__") != NULL) {
        diag_error(loc, "extern C name '%s' contains '__', which is reserved in FC "
            "names; give it an alias: `extern %s as <name>: ...`", name, name);
    }
    expect(p, TOK_COLON);
    Type *type = parse_type(p);
    /* Optional error-protocol tail, required (checked in pass1) if and only
     * if the declared return type is a result. */
    ExternProtocol proto = EXT_PROTO_NONE;
    if (check(p, TOK_FROM)) {
        advance_p(p);
        proto = parse_extern_protocol(p);
    }
    Decl *d = arena_alloc(p->arena, sizeof(Decl));
    d->kind = DECL_EXTERN;
    d->loc = loc;
    d->is_private = false;
    d->ext.name = name;
    d->ext.alias = alias;
    d->ext.type = type;
    d->ext.protocol = proto;
    return d;
}

/* Parse one declaration and append what it declares to `out`; a comma-list
 * import appends several. Returns the first appended, or a DECL_ERROR node
 * (not appended) after a syntax error. */
/* The declaration keywords and their parsers. `import`, which may append
 * several declarations, and the `private` modifier are handled around this
 * table; together they are the declaration-starting set the recovery loops
 * anchor on (is_decl_start). */
static const struct {
    TokenKind keyword;
    Decl *(*parse)(Parser *p);
} DECL_PARSERS[] = {
    { TOK_LET, parse_let_decl },       { TOK_STRUCT, parse_struct_decl },
    { TOK_UNION, parse_union_decl },   { TOK_ENUM, parse_enum_decl },
    { TOK_ERROR_KW, parse_error_decl }, { TOK_MODULE, parse_module_decl },
    { TOK_NAMESPACE, parse_namespace_decl }, { TOK_EXTERN, parse_extern_decl },
};

static bool is_decl_start(TokenKind k) {
    if (k == TOK_PRIVATE || k == TOK_IMPORT) return true;
    for (size_t i = 0; i < sizeof DECL_PARSERS / sizeof DECL_PARSERS[0]; i++)
        if (DECL_PARSERS[i].keyword == k) return true;
    return false;
}

static Decl *parse_decl(Parser *p, DeclList *out) {
    skip_newlines(p);

    /* private modifier */
    if (check(p, TOK_PRIVATE)) {
        advance_p(p);
        int first = out->count;
        Decl *d = parse_decl(p, out);
        for (int i = first; i < out->count; i++) out->items[i]->is_private = true;
        return d;
    }
    if (check(p, TOK_IMPORT)) return parse_import_decl(p, out);
    for (size_t i = 0; i < sizeof DECL_PARSERS / sizeof DECL_PARSERS[0]; i++)
        if (check(p, DECL_PARSERS[i].keyword))
            return add_parsed(out, DECL_PARSERS[i].parse(p));

    SrcLoc loc = loc_from_token(current(p));
    diag_error(loc, "expected declaration, got %s",
        token_kind_name(current(p)->kind));
    /* No leaf-bump here: the parse_program / parse_module_decl loop recovers to
       the next declaration keyword (recover_to_decl, and runs the watchdog),
       which keeps any stray block intact. */
    return alloc_decl_error(p, loc);
}

static Program *parse_program(Parser *p) {
    DeclList decls = {0};
    bool seen_non_ns = false;
    skip_newlines(p);
    while (!at_end_p(p)) {
        int guard = p->pos;
        Decl *d = parse_decl(p, &decls);
        if (d->kind == DECL_ERROR) {
            /* Sync to the next top-level declaration so one malformed line doesn't
               swallow the rest of the file. The error node is not appended (later
               passes have nothing to do with it; the diagnostic was already emitted). */
            recover_to_decl(p);
        } else {
            if (d->kind == DECL_NAMESPACE && seen_non_ns) {
                diag_error(d->loc, "namespace declaration must be the first line of the file");
            }
            if (d->kind != DECL_NAMESPACE) seen_non_ns = true;
        }
        /* Watchdog: this loop ends only at EOF, so force progress unconditionally
           (a stray DEDENT here is not a terminator). */
        if (p->pos == guard && !at_end_p(p)) advance_p(p);
        skip_newlines(p);
    }
    Program *prog = arena_alloc(p->arena, sizeof(Program));
    prog->decl_count = decls.count;
    prog->decls = decl_list_finish(p, &decls);
    return prog;
}

Program *parse_file(Token *tokens, int count, const char *filename,
                    const char **generic_names, int generic_name_count,
                    Arena *arena, InternTable *intern) {
    diag_set_filename(filename);
    Parser p = {
        .tokens = tokens,
        .token_count = count,
        .arena = arena,
        .intern = intern,
        .filename = filename,
        .half_gt_pos = -1,
        .generic_names = generic_names,
        .generic_name_count = generic_name_count,
    };
    return parse_program(&p);
}

Program *parse_files(Token **tokens, const int *counts, const char **filenames, int n,
                       Arena *arena, InternTable *intern) {
    const char **generic_names = NULL;
    int gn_count = 0, gn_cap = 0;
    for (int i = 0; i < n; i++)
        parser_collect_generic_names(tokens[i], counts[i], intern,
                                     &generic_names, &gn_count, &gn_cap);
    Program **programs = arena_alloc(arena, sizeof(Program *) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++)
        programs[i] = parse_file(tokens[i], counts[i], filenames[i], generic_names, gn_count,
                                 arena, intern);
    Program *prog = program_merge(arena, programs, n);
    prog->generic_names = arena_dup(arena, generic_names, gn_count, sizeof *generic_names);
    prog->generic_name_count = gn_count;
    free(generic_names);
    return prog;
}
