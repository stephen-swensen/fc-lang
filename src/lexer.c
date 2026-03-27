#include "lexer.h"
#include "diag.h"
#include <ctype.h>
#include <stdio.h>

void lexer_init(Lexer *l, const char *source, InternTable *intern,
                const char **flags, int flag_count) {
    l->source = source;
    l->current = source;
    l->start = source;
    l->line = 1;
    l->col = 1;
    l->start_col = 1;
    l->intern = intern;
    l->interp_depth = 0;
    l->interp_scan_fmt = false;
    l->interp_fmt_start = NULL;
    l->interp_fmt_len = 0;
    l->interp_fmt_line = 0;
    l->interp_fmt_col = 0;
    memset(l->interp_brace, 0, sizeof(l->interp_brace));
    l->flags = flags;
    l->flag_count = flag_count;
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
        {"const",     5,  TOK_CONST},
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
    /* Reject identifiers containing __ (double underscore) — reserved for
     * the compiler's name mangling of namespace/module hierarchies. */
    TokenKind kw = check_keyword(l->start, len);
    if (kw == TOK_IDENT) {
        for (int i = 0; i + 1 < len; i++) {
            if (l->start[i] == '_' && l->start[i + 1] == '_') {
                SrcLoc loc = { .line = l->line, .col = l->start_col };
                diag_fatal(loc, "identifier '%.*s' contains '__' (double underscore), "
                    "which is reserved", len, l->start);
            }
        }
    }
    return make_token(l, kw);
}

static Token scan_number(Lexer *l) {
    /* Check for 0x, 0b, 0o prefixes */
    if (l->current[-1] == '0') {
        if (peek(l) == 'x' || peek(l) == 'X') {
            advance(l); /* consume x */
            while (isxdigit((unsigned char)peek(l))) advance(l);
            if ((peek(l) == 'i' || peek(l) == 'u') && isdigit((unsigned char)peek_next(l))) {
                advance(l);
                while (isdigit((unsigned char)peek(l))) advance(l);
            } else if ((peek(l) == 'i' || peek(l) == 'u') && !isalnum((unsigned char)peek_next(l)) && peek_next(l) != '_') {
                advance(l);
            }
            return make_token(l, TOK_INT_LIT);
        }
        if (peek(l) == 'b' || peek(l) == 'B') {
            advance(l); /* consume b */
            while (peek(l) == '0' || peek(l) == '1') advance(l);
            if ((peek(l) == 'i' || peek(l) == 'u') && isdigit((unsigned char)peek_next(l))) {
                advance(l);
                while (isdigit((unsigned char)peek(l))) advance(l);
            } else if ((peek(l) == 'i' || peek(l) == 'u') && !isalnum((unsigned char)peek_next(l)) && peek_next(l) != '_') {
                advance(l);
            }
            return make_token(l, TOK_INT_LIT);
        }
        if (peek(l) == 'o' || peek(l) == 'O') {
            advance(l); /* consume o */
            while (peek(l) >= '0' && peek(l) <= '7') advance(l);
            if ((peek(l) == 'i' || peek(l) == 'u') && isdigit((unsigned char)peek_next(l))) {
                advance(l);
                while (isdigit((unsigned char)peek(l))) advance(l);
            } else if ((peek(l) == 'i' || peek(l) == 'u') && !isalnum((unsigned char)peek_next(l)) && peek_next(l) != '_') {
                advance(l);
            }
            return make_token(l, TOK_INT_LIT);
        }
    }
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
    } else if ((peek(l) == 'i' || peek(l) == 'u') && !isalnum((unsigned char)peek_next(l)) && peek_next(l) != '_') {
        advance(l);
    }
    return make_token(l, TOK_INT_LIT);
}

/* Check if position p (pointing past '%') looks like a format spec followed by '{'.
 * Returns length of the spec (not including '%' or '{'), or 0 if no match.
 * Format spec: optional flags (-+0# space), optional width (digits), optional .precision,
 * then a required conversion char (d,i,u,x,X,o,f,e,E,g,G,s,c,p). */
static int check_interp_spec(const char *p) {
    const char *s = p;
    /* optional flags */
    while (*s == '-' || *s == '+' || *s == '0' || *s == '#' || *s == ' ') s++;
    /* optional width */
    while (*s >= '0' && *s <= '9') s++;
    /* optional precision */
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') s++;
    }
    /* required conversion character */
    const char *convs = "diuxXofeEgGscpT";
    const char *conv = s;
    bool found = false;
    for (const char *c = convs; *c; c++) {
        if (*conv == *c) { found = true; break; }
    }
    if (!found) return 0;
    s++;
    /* must be followed by { */
    if (*s != '{') return 0;
    return (int)(s - p);
}

/* Scan string literal text (after opening " or after closing } of interp expr).
 * If interpolation %spec{ is found, emits INTERP_START/MID and sets up FMT_SPEC state.
 * If closing " is found, emits STRING_LIT (plain) or INTERP_END.
 * tok_kind is TOK_INTERP_START for first segment, TOK_INTERP_MID for subsequent. */
static Token scan_string_body(Lexer *l, TokenKind tok_kind) {
    const char *text_start = l->current;  /* start of literal text content */

    while (!at_end(l) && peek(l) != '"') {
        if (peek(l) == '\\') {
            advance(l); /* skip backslash */
            if (!at_end(l)) advance(l); /* skip escaped char */
            continue;
        }
        if (peek(l) == '%') {
            /* Check for %% (literal percent escape) */
            if (l->current[1] == '%') {
                advance(l); advance(l); /* consume both % */
                continue;
            }
            /* Check for %spec{ (interpolation start) */
            int spec_len = check_interp_spec(l->current + 1);
            if (spec_len > 0) {
                /* Found interpolation: emit the text before it */
                Token t;
                t.kind = tok_kind;
                t.start = text_start;
                t.length = (int)(l->current - text_start);
                t.line = l->line;
                t.col = l->start_col;

                /* Advance past % */
                advance(l);

                /* Store format spec location for next scan_token call */
                l->interp_fmt_start = l->current;
                l->interp_fmt_len = spec_len;
                l->interp_fmt_line = l->line;
                l->interp_fmt_col = l->col;

                /* Advance past spec and opening { */
                for (int i = 0; i < spec_len; i++) advance(l);
                advance(l); /* consume { */

                /* Enter interpolation mode */
                if (l->interp_depth >= MAX_INTERP_DEPTH) {
                    return error_token(l, "string interpolation nested too deeply");
                }
                l->interp_depth++;
                l->interp_brace[l->interp_depth - 1] = 1;
                l->interp_scan_fmt = true;

                return t;
            }
        }
        advance(l);
    }

    /* Reached closing " (or end of input) */
    if (at_end(l)) return error_token(l, "unterminated string");

    if (tok_kind == TOK_INTERP_MID) {
        /* End of an interpolated string (after at least one interp segment) */
        Token t;
        t.kind = TOK_INTERP_END;
        t.start = text_start;
        t.length = (int)(l->current - text_start);
        t.line = l->line;
        t.col = l->start_col;
        advance(l); /* consume closing " */
        return t;
    }

    /* Plain string or cstring that found no interpolation */
    advance(l); /* consume closing " */
    return make_token(l, tok_kind == TOK_CINTERP_START ? TOK_CSTRING_LIT : TOK_STRING_LIT);
}

static Token scan_string(Lexer *l) {
    advance(l); /* opening " */
    return scan_string_body(l, TOK_INTERP_START);
}

static Token scan_cstring(Lexer *l) {
    advance(l); /* opening " */
    return scan_string_body(l, TOK_CINTERP_START);
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
    /* If we need to emit a format specifier from interpolation */
    if (l->interp_scan_fmt) {
        l->interp_scan_fmt = false;
        return (Token){
            .kind = TOK_FMT_SPEC,
            .start = l->interp_fmt_start,
            .length = l->interp_fmt_len,
            .line = l->interp_fmt_line,
            .col = l->interp_fmt_col,
        };
    }

    l->start = l->current;
    l->start_col = l->col;

    if (at_end(l)) return make_token(l, TOK_EOF);

    /* If inside interpolation expression, track brace depth */
    if (l->interp_depth > 0) {
        int idx = l->interp_depth - 1;
        if (peek(l) == '{') {
            l->interp_brace[idx]++;
        } else if (peek(l) == '}') {
            l->interp_brace[idx]--;
            if (l->interp_brace[idx] == 0) {
                /* Closing brace of interpolation expression.
                 * Resume scanning the string continuation. */
                advance(l); /* consume } */
                l->interp_depth--;
                return scan_string_body(l, TOK_INTERP_MID);
            }
        }
    }

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
    case '\'': return scan_char_lit(l);
    case '.':
        if (match(l, '.')) return match(l, '.') ? make_token(l, TOK_ELLIPSIS) : make_token(l, TOK_DOTDOT);
        return make_token(l, TOK_DOT);
    case ':': return match(l, ':') ? make_token(l, TOK_COLONCOLON) : make_token(l, TOK_COLON);
    case '#': {
        while (*l->current == ' ') advance(l);
        const char *dir = l->current;
        /* Read only alpha chars for the directive keyword */
        while (isalpha((unsigned char)peek(l))) advance(l);
        int dlen = (int)(l->current - dir);
        if (dlen == 3 && memcmp(dir, "end", 3) == 0) return make_token(l, TOK_HASH_END);
        if (dlen == 4 && memcmp(dir, "else", 4) == 0) {
            /* Check for "else if" — skip spaces, check for "if" */
            const char *saved = l->current;
            int saved_col = l->col;
            while (*l->current == ' ') advance(l);
            if (l->current[0] == 'i' && l->current[1] == 'f' &&
                !isalpha((unsigned char)l->current[2]) && l->current[2] != '_') {
                advance(l); advance(l);
                return make_token(l, TOK_HASH_ELSE_IF);
            }
            /* Plain #else — restore position */
            l->current = saved;
            l->col = saved_col;
            return make_token(l, TOK_HASH_ELSE);
        }
        if (dlen == 2 && memcmp(dir, "if", 2) == 0) return make_token(l, TOK_HASH_IF);
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

/* ---- Conditional compilation filter (phase 1.5) ---- */

#define MAX_COND_DEPTH 32

static bool flag_is_set(const char **flags, int flag_count, const char *name, int name_len) {
    for (int i = 0; i < flag_count; i++) {
        if ((int)strlen(flags[i]) == name_len && memcmp(flags[i], name, (size_t)name_len) == 0)
            return true;
    }
    return false;
}

/* Filter out tokens in inactive #if/#else if/#else/#end branches.
 * Directive tokens are always stripped from output. */
static Token *filter_conditionals(Token *tokens, int count,
                                   const char **flags, int flag_count,
                                   int *out_count) {
    Token *out = NULL;
    int olen = 0, ocap = 0;

    /* State stack: active[depth] = whether current branch is being included.
     * done[depth] = whether any branch at this level has been taken. */
    bool active[MAX_COND_DEPTH];
    bool done[MAX_COND_DEPTH];
    int depth = 0;
    active[0] = true;
    done[0] = true;

    for (int i = 0; i < count; i++) {
        Token t = tokens[i];

        if (t.kind == TOK_HASH_IF) {
            /* Validate column 1 */
            if (t.col != 1) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#if must appear at column 1");
            }
            /* Next non-newline token must be an identifier (the flag name) */
            int j = i + 1;
            while (j < count && tokens[j].kind == TOK_NEWLINE) j++;
            if (j >= count || tokens[j].kind != TOK_IDENT) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#if requires a flag name");
            }
            bool parent_active = active[depth];
            depth++;
            if (depth >= MAX_COND_DEPTH) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "too many nested #if directives");
            }
            bool flag_set = flag_is_set(flags, flag_count, tokens[j].start, tokens[j].length);
            active[depth] = parent_active && flag_set;
            done[depth] = flag_set;
            i = j; /* skip flag name token */
            /* Skip trailing newline after directive */
            if (i + 1 < count && tokens[i + 1].kind == TOK_NEWLINE) i++;
            continue;
        }

        if (t.kind == TOK_HASH_ELSE_IF) {
            if (t.col != 1) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#else if must appear at column 1");
            }
            if (depth == 0) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#else if without matching #if");
            }
            int j = i + 1;
            while (j < count && tokens[j].kind == TOK_NEWLINE) j++;
            if (j >= count || tokens[j].kind != TOK_IDENT) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#else if requires a flag name");
            }
            bool parent_active = (depth >= 2) ? active[depth - 1] : true;
            bool flag_set = flag_is_set(flags, flag_count, tokens[j].start, tokens[j].length);
            active[depth] = parent_active && !done[depth] && flag_set;
            if (flag_set) done[depth] = true;
            i = j;
            if (i + 1 < count && tokens[i + 1].kind == TOK_NEWLINE) i++;
            continue;
        }

        if (t.kind == TOK_HASH_ELSE) {
            if (t.col != 1) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#else must appear at column 1");
            }
            if (depth == 0) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#else without matching #if");
            }
            bool parent_active = (depth >= 2) ? active[depth - 1] : true;
            active[depth] = parent_active && !done[depth];
            done[depth] = true;
            /* Skip trailing newline after directive */
            if (i + 1 < count && tokens[i + 1].kind == TOK_NEWLINE) i++;
            continue;
        }

        if (t.kind == TOK_HASH_END) {
            if (t.col != 1) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#end must appear at column 1");
            }
            if (depth == 0) {
                SrcLoc loc = { .line = t.line, .col = t.col };
                diag_fatal(loc, "#end without matching #if");
            }
            depth--;
            /* Skip trailing newline after directive */
            if (i + 1 < count && tokens[i + 1].kind == TOK_NEWLINE) i++;
            continue;
        }

        /* Copy token if in an active branch */
        if (active[depth]) {
            DA_APPEND(out, olen, ocap, t);
        }
    }

    if (depth != 0) {
        /* Find the last #if for a better error location */
        SrcLoc loc = { .line = tokens[count - 1].line, .col = 1 };
        diag_fatal(loc, "unclosed #if (missing #end)");
    }

    *out_count = olen;
    return out;
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

    /* Filter conditional compilation directives */
    int filtered_count;
    Token *filtered = filter_conditionals(raw, raw_count,
        l->flags, l->flag_count, &filtered_count);
    free(raw);
    raw = filtered;
    raw_count = filtered_count;

    Token *out = NULL;
    int olen = 0, ocap = 0;

    int indent_stack[MAX_INDENT];
    bool is_match_block[MAX_INDENT]; /* true if this level is a same-level match block */
    int indent_depth = 0;
    indent_stack[indent_depth] = 1;  /* base column */
    is_match_block[indent_depth] = false;

    bool saw_decl_keyword = false;    /* let, struct, union, module on current line */
    bool saw_for = false;             /* for on current line (before in) */
    bool saw_for_in = false;          /* saw 'for ... in' — line is block-former */
    TokenKind last_kind = TOK_EOF;    /* last non-NEWLINE token emitted */
    int bracket_depth = 0;            /* depth inside () [] {} — suppresses layout */

    for (int i = 0; i < raw_count; i++) {
        Token t = raw[i];

        if (t.kind == TOK_NEWLINE) {
            /* Inside matched brackets: suppress all layout tokens */
            if (bracket_depth > 0) {
                while (i + 1 < raw_count && raw[i + 1].kind == TOK_NEWLINE) i++;
                continue;
            }
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
            if (!block_former && saw_for_in)
                block_former = true;

            /* Reset line-local state */
            saw_decl_keyword = false;
            saw_for = false;
            saw_for_in = false;

            /* Close same-level match blocks when next line doesn't start with | */
            while (indent_depth > 0 && is_match_block[indent_depth] &&
                   next_col <= indent_stack[indent_depth]) {
                if (next_col == indent_stack[indent_depth] && raw[j].kind == TOK_PIPE)
                    break; /* still in match block — peer arm */
                indent_depth--;
                Token dedent = make_layout_token(TOK_DEDENT, t.line, t.col);
                DA_APPEND(out, olen, ocap, dedent);
            }

            if (block_former && next_col > indent_stack[indent_depth]) {
                /* Start new block */
                indent_depth++;
                if (indent_depth >= MAX_INDENT) {
                    diag_fatal_simple("indentation too deep");
                }
                indent_stack[indent_depth] = next_col;
                is_match_block[indent_depth] = false;
                Token indent_tok = make_layout_token(TOK_INDENT, raw[j].line, raw[j].col);
                DA_APPEND(out, olen, ocap, indent_tok);
            } else if (last_kind == TOK_WITH &&
                       next_col == indent_stack[indent_depth] &&
                       raw[j].kind == TOK_PIPE) {
                /* Match block at same indentation level (F#-style) */
                indent_depth++;
                if (indent_depth >= MAX_INDENT) {
                    diag_fatal_simple("indentation too deep");
                }
                indent_stack[indent_depth] = next_col;
                is_match_block[indent_depth] = true;
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
                /* After dedent, close any match block we landed on */
                while (indent_depth > 0 && is_match_block[indent_depth] &&
                       next_col <= indent_stack[indent_depth]) {
                    if (next_col == indent_stack[indent_depth] && raw[j].kind == TOK_PIPE)
                        break;
                    indent_depth--;
                    Token dedent2 = make_layout_token(TOK_DEDENT, t.line, t.col);
                    DA_APPEND(out, olen, ocap, dedent2);
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

        /* Track bracket depth for layout suppression */
        if (t.kind == TOK_LPAREN || t.kind == TOK_LBRACKET || t.kind == TOK_LBRACE)
            bracket_depth++;
        else if (t.kind == TOK_RPAREN || t.kind == TOK_RBRACKET || t.kind == TOK_RBRACE) {
            if (bracket_depth > 0) bracket_depth--;
        }

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
        if (t.kind == TOK_IN && saw_for) {
            saw_for_in = true;
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
