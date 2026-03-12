#include "lexer.h"
#include "diag.h"
#include <ctype.h>
#include <stdio.h>

void lexer_init(Lexer *l, const char *source, InternTable *intern) {
    l->source = source;
    l->current = source;
    l->start = source;
    l->line = 1;
    l->col = 1;
    l->start_col = 1;
    l->intern = intern;
}

static char peek(Lexer *l)      { return *l->current; }
static char peek_next(Lexer *l) { return l->current[0] ? l->current[1] : '\0'; }
static bool at_end(Lexer *l)    { return *l->current == '\0'; }

static char advance(Lexer *l) {
    char c = *l->current++;
    if (c == '\n') { l->line++; l->col = 1; }
    else { l->col++; }
    return c;
}

static bool match(Lexer *l, char expected) {
    if (at_end(l) || *l->current != expected) return false;
    advance(l);
    return true;
}

static Token make_token(Lexer *l, TokenKind kind) {
    return (Token){
        .kind = kind,
        .start = l->start,
        .length = (int)(l->current - l->start),
        .line = l->line,
        .col = l->start_col,
    };
}

static Token error_token(Lexer *l, const char *msg) {
    return (Token){
        .kind = TOK_ERROR,
        .start = msg,
        .length = (int)strlen(msg),
        .line = l->line,
        .col = l->start_col,
    };
}

static void skip_line_comment(Lexer *l) {
    while (!at_end(l) && *l->current != '\n') advance(l);
}

static void skip_block_comment(Lexer *l) {
    int depth = 1;
    while (!at_end(l) && depth > 0) {
        if (*l->current == '/' && peek_next(l) == '*') {
            advance(l); advance(l); depth++;
        } else if (*l->current == '*' && peek_next(l) == '/') {
            advance(l); advance(l); depth--;
        } else {
            advance(l);
        }
    }
    if (depth > 0) {
        SrcLoc loc = { .line = l->line, .col = l->col };
        diag_fatal(loc, "unterminated block comment");
    }
}

static TokenKind check_keyword(const char *start, int len) {
    struct { const char *kw; int klen; TokenKind kind; } keywords[] = {
        {"let",       3,  TOK_LET},
        {"mut",       3,  TOK_MUT},
        {"struct",    6,  TOK_STRUCT},
        {"union",     5,  TOK_UNION},
        {"module",    6,  TOK_MODULE},
        {"namespace", 9,  TOK_NAMESPACE},
        {"import",    6,  TOK_IMPORT},
        {"from",      4,  TOK_FROM},
        {"as",        2,  TOK_AS},
        {"extern",    6,  TOK_EXTERN},
        {"private",   7,  TOK_PRIVATE},
        {"match",     5,  TOK_MATCH},
        {"with",      4,  TOK_WITH},
        {"if",        2,  TOK_IF},
        {"then",      4,  TOK_THEN},
        {"else",      4,  TOK_ELSE},
        {"for",       3,  TOK_FOR},
        {"in",        2,  TOK_IN},
        {"loop",      4,  TOK_LOOP},
        {"break",     5,  TOK_BREAK},
        {"continue",  8,  TOK_CONTINUE},
        {"return",    6,  TOK_RETURN},
        {"some",      4,  TOK_SOME},
        {"true",      4,  TOK_TRUE},
        {"false",     5,  TOK_FALSE},
        {"none",      4,  TOK_NONE},
        {"void",      4,  TOK_VOID},
        {"alloc",     5,  TOK_ALLOC},
        {"free",      4,  TOK_FREE},
        {"sizeof",    6,  TOK_SIZEOF},
        {"default",   7,  TOK_DEFAULT},
        {"print",     5,  TOK_PRINT},
        {"eprint",    6,  TOK_EPRINT},
        {"fprint",    6,  TOK_FPRINT},
        {"sprint",    6,  TOK_SPRINT},
    };
    for (int i = 0; i < (int)(sizeof(keywords)/sizeof(keywords[0])); i++) {
        if (keywords[i].klen == len && memcmp(start, keywords[i].kw, (size_t)len) == 0)
            return keywords[i].kind;
    }
    return TOK_IDENT;
}

static Token scan_identifier(Lexer *l) {
    while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    int len = (int)(l->current - l->start);
    return make_token(l, check_keyword(l->start, len));
}

static Token scan_number(Lexer *l) {
    while (isdigit((unsigned char)peek(l))) advance(l);
    if (peek(l) == '.' && isdigit((unsigned char)peek_next(l))) {
        advance(l);
        while (isdigit((unsigned char)peek(l))) advance(l);
        if (peek(l) == 'f' && (peek_next(l) == '3' || peek_next(l) == '6')) {
            advance(l); advance(l);
            if (peek(l) == '2' || peek(l) == '4') advance(l);
        }
        return make_token(l, TOK_FLOAT_LIT);
    }
    if ((peek(l) == 'i' || peek(l) == 'u') && isdigit((unsigned char)peek_next(l))) {
        advance(l);
        while (isdigit((unsigned char)peek(l))) advance(l);
    }
    return make_token(l, TOK_INT_LIT);
}

static Token scan_string(Lexer *l) {
    advance(l); /* opening " */
    while (!at_end(l) && peek(l) != '"') {
        if (peek(l) == '\\') advance(l);
        if (!at_end(l)) advance(l);
    }
    if (at_end(l)) return error_token(l, "unterminated string");
    advance(l);
    return make_token(l, TOK_STRING_LIT);
}

static Token scan_cstring(Lexer *l) {
    advance(l); /* opening " */
    while (!at_end(l) && peek(l) != '"') {
        if (peek(l) == '\\') advance(l);
        if (!at_end(l)) advance(l);
    }
    if (at_end(l)) return error_token(l, "unterminated cstring");
    advance(l);
    return make_token(l, TOK_CSTRING_LIT);
}

static Token scan_char_lit(Lexer *l) {
    advance(l); /* opening ' */
    if (peek(l) == '\\') {
        advance(l); advance(l);
        if (l->current[-1] == 'x') {
            if (isxdigit((unsigned char)peek(l))) advance(l);
            if (isxdigit((unsigned char)peek(l))) advance(l);
        }
    } else {
        advance(l);
    }
    if (peek(l) != '\'') return error_token(l, "unterminated char literal");
    advance(l);
    return make_token(l, TOK_CHAR_LIT);
}

static Token scan_token(Lexer *l) {
    l->start = l->current;
    l->start_col = l->col;

    if (at_end(l)) return make_token(l, TOK_EOF);

    char c = advance(l);

    if (isalpha((unsigned char)c) || c == '_') {
        if (c == 'c' && peek(l) == '"') return scan_cstring(l);
        return scan_identifier(l);
    }

    if (isdigit((unsigned char)c)) return scan_number(l);

    if (c == '\'') {
        if (peek(l) == '\\') {
            l->current = l->start; l->col = l->start_col;
            return scan_char_lit(l);
        }
        if (isalpha((unsigned char)peek(l))) {
            const char *saved = l->current; int saved_col = l->col;
            advance(l);
            if (peek(l) == '\'') {
                l->current = l->start; l->col = l->start_col;
                return scan_char_lit(l);
            }
            l->current = saved; l->col = saved_col;
            while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
            return make_token(l, TOK_TYPE_VAR);
        }
        l->current = l->start; l->col = l->start_col;
        return scan_char_lit(l);
    }

    if (c == '"') {
        l->current = l->start; l->col = l->start_col;
        return scan_string(l);
    }

    if (c == '\n') return make_token(l, TOK_NEWLINE);

    switch (c) {
    case '+': return make_token(l, TOK_PLUS);
    case '*': return make_token(l, TOK_STAR);
    case '/': return make_token(l, TOK_SLASH);
    case '%': return make_token(l, TOK_PERCENT);
    case '^': return make_token(l, TOK_CARET);
    case '~': return make_token(l, TOK_TILDE);
    case '?': return make_token(l, TOK_QUESTION);
    case ';': return make_token(l, TOK_SEMICOLON);
    case '(': return make_token(l, TOK_LPAREN);
    case ')': return make_token(l, TOK_RPAREN);
    case '[': return make_token(l, TOK_LBRACKET);
    case ']': return make_token(l, TOK_RBRACKET);
    case '{': return make_token(l, TOK_LBRACE);
    case '}': return make_token(l, TOK_RBRACE);
    case ',': return make_token(l, TOK_COMMA);
    case '-': return match(l, '>') ? make_token(l, TOK_ARROW) : make_token(l, TOK_MINUS);
    case '=': return match(l, '=') ? make_token(l, TOK_EQEQ) : make_token(l, TOK_EQ);
    case '!': return match(l, '=') ? make_token(l, TOK_BANGEQ) : make_token(l, TOK_BANG);
    case '<':
        if (match(l, '=')) return make_token(l, TOK_LTEQ);
        if (match(l, '<')) return make_token(l, TOK_LTLT);
        return make_token(l, TOK_LT);
    case '>':
        if (match(l, '=')) return make_token(l, TOK_GTEQ);
        if (match(l, '>')) return make_token(l, TOK_GTGT);
        return make_token(l, TOK_GT);
    case '&': return match(l, '&') ? make_token(l, TOK_AMPAMP) : make_token(l, TOK_AMP);
    case '|': return match(l, '|') ? make_token(l, TOK_PIPEPIPE) : make_token(l, TOK_PIPE);
    case '.': return match(l, '.') ? make_token(l, TOK_DOTDOT) : make_token(l, TOK_DOT);
    case ':': return match(l, ':') ? make_token(l, TOK_COLONCOLON) : make_token(l, TOK_COLON);
    case '#': {
        while (*l->current == ' ') advance(l);
        const char *dir = l->current;
        while (isalpha((unsigned char)peek(l)) || peek(l) == ' ') advance(l);
        int dlen = (int)(l->current - dir);
        if (dlen >= 7 && memcmp(dir, "else if", 7) == 0) return make_token(l, TOK_HASH_ELSE_IF);
        if (dlen >= 4 && memcmp(dir, "else", 4) == 0) return make_token(l, TOK_HASH_ELSE);
        if (dlen >= 3 && memcmp(dir, "end", 3) == 0) return make_token(l, TOK_HASH_END);
        if (dlen >= 2 && memcmp(dir, "if", 2) == 0) return make_token(l, TOK_HASH_IF);
        return error_token(l, "unknown preprocessor directive");
    }
    }

    return error_token(l, "unexpected character");
}

/* ---- Raw tokenization (phase 1) ---- */

static Token *raw_tokenize(Lexer *l, int *out_count) {
    Token *tokens = NULL;
    int len = 0, cap = 0;

    for (;;) {
        /* Skip spaces and comments but not newlines */
        for (;;) {
            if (peek(l) == ' ' || peek(l) == '\r') { advance(l); continue; }
            if (peek(l) == '\t') {
                SrcLoc loc = { .line = l->line, .col = l->col };
                diag_fatal(loc, "tabs are not allowed; use spaces for indentation");
            }
            if (peek(l) == '/' && peek_next(l) == '/') { skip_line_comment(l); continue; }
            if (peek(l) == '/' && peek_next(l) == '*') {
                advance(l); advance(l);
                skip_block_comment(l);
                continue;
            }
            break;
        }

        Token t = scan_token(l);
        if (t.kind == TOK_ERROR) {
            SrcLoc loc = { .line = t.line, .col = t.col };
            diag_fatal(loc, "%.*s", t.length, t.start);
        }

        DA_APPEND(tokens, len, cap, t);
        if (t.kind == TOK_EOF) break;
    }

    *out_count = len;
    return tokens;
}

/* ---- Layout pass (phase 2): insert INDENT/DEDENT/NEWLINE ---- */

#define MAX_INDENT 128

static Token make_layout_token(TokenKind kind, int line, int col) {
    return (Token){ .kind = kind, .start = "", .length = 0, .line = line, .col = col };
}

static bool is_always_block_former(TokenKind k) {
    return k == TOK_ARROW || k == TOK_WITH || k == TOK_THEN ||
           k == TOK_ELSE || k == TOK_LOOP;
}

Token *lexer_tokenize(Lexer *l, int *out_count) {
    int raw_count;
    Token *raw = raw_tokenize(l, &raw_count);

    Token *out = NULL;
    int olen = 0, ocap = 0;

    int indent_stack[MAX_INDENT];
    int indent_depth = 0;
    indent_stack[indent_depth] = 1;  /* base column */

    bool saw_decl_keyword = false;    /* let, struct, union, module on current line */
    bool saw_for = false;             /* for on current line (before in) */
    TokenKind last_kind = TOK_EOF;    /* last non-NEWLINE token emitted */

    for (int i = 0; i < raw_count; i++) {
        Token t = raw[i];

        if (t.kind == TOK_NEWLINE) {
            /* Find next non-newline token */
            int j = i + 1;
            while (j < raw_count && raw[j].kind == TOK_NEWLINE) j++;
            if (j >= raw_count || raw[j].kind == TOK_EOF) {
                /* Trailing newlines — skip */
                i = j - 1;
                continue;
            }

            int next_col = raw[j].col;

            /* Determine if the last token before this newline was a block-former */
            bool block_former = is_always_block_former(last_kind);
            if (!block_former && last_kind == TOK_EQ && saw_decl_keyword)
                block_former = true;
            if (!block_former && last_kind == TOK_IN && saw_for)
                block_former = true;

            /* Reset line-local state */
            saw_decl_keyword = false;
            saw_for = false;

            if (block_former && next_col > indent_stack[indent_depth]) {
                /* Start new block */
                indent_depth++;
                if (indent_depth >= MAX_INDENT) {
                    diag_fatal_simple("indentation too deep");
                }
                indent_stack[indent_depth] = next_col;
                Token indent_tok = make_layout_token(TOK_INDENT, raw[j].line, raw[j].col);
                DA_APPEND(out, olen, ocap, indent_tok);
            } else if (next_col == indent_stack[indent_depth]) {
                /* Peer separator — but don't emit after INDENT or at start */
                if (olen > 0 && out[olen-1].kind != TOK_INDENT && out[olen-1].kind != TOK_NEWLINE) {
                    Token nl = make_layout_token(TOK_NEWLINE, t.line, t.col);
                    DA_APPEND(out, olen, ocap, nl);
                }
            } else if (next_col < indent_stack[indent_depth]) {
                /* Dedent — pop until we match */
                while (indent_depth > 0 && next_col < indent_stack[indent_depth]) {
                    indent_depth--;
                    Token dedent = make_layout_token(TOK_DEDENT, t.line, t.col);
                    DA_APPEND(out, olen, ocap, dedent);
                }
                if (next_col != indent_stack[indent_depth]) {
                    SrcLoc loc = { .line = raw[j].line, .col = raw[j].col };
                    diag_fatal(loc, "inconsistent indentation (col %d does not match any block)", next_col);
                }
                /* Emit newline after dedents for peer separation */
                if (olen > 0 && out[olen-1].kind != TOK_NEWLINE) {
                    Token nl = make_layout_token(TOK_NEWLINE, t.line, t.col);
                    DA_APPEND(out, olen, ocap, nl);
                }
            } else {
                /* next_col > indent_stack top but not block former → continuation */
                /* Suppress the newline — tokens continue the current expression */
            }

            /* Skip over the consecutive newlines we already processed */
            i = j - 1;
            continue;
        }

        /* Skip raw EOF — we emit our own after DEDENTs */
        if (t.kind == TOK_EOF) continue;

        /* Non-newline token: emit it */
        DA_APPEND(out, olen, ocap, t);
        last_kind = t.kind;

        /* Track context for = and in block-former detection */
        if (t.kind == TOK_LET || t.kind == TOK_STRUCT ||
            t.kind == TOK_UNION || t.kind == TOK_MODULE) {
            saw_decl_keyword = true;
        }
        if (t.kind == TOK_FOR) {
            saw_for = true;
        }
    }

    /* Emit remaining DEDENTs */
    while (indent_depth > 0) {
        indent_depth--;
        Token dedent = make_layout_token(TOK_DEDENT, 0, 0);
        DA_APPEND(out, olen, ocap, dedent);
    }

    /* Emit EOF */
    Token eof = make_layout_token(TOK_EOF, 0, 0);
    DA_APPEND(out, olen, ocap, eof);

    free(raw);
    *out_count = olen;
    return out;
}
