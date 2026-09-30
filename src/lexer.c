#include "lexer.h"
#include "diag.h"
#include <ctype.h>
#include <stdio.h>

void lexer_init(Lexer *l, const char *source, InternTable *intern,
                const Flag *flags, int flag_count) {
    l->source = source;
    l->current = source;
    l->start = source;
    l->line = 1;
    l->col = 1;
    l->start_col = 1;
    l->start_line = 1;
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
    l->prev_kind = TOK_EOF;
    l->prev_prev_kind = TOK_EOF;
    l->abort_slot_input = NULL;
    l->abort_slot_output = NULL;
}

/* Publish the arrays a phase reads and builds (see Lexer.abort_slot_input). */
static void publish_phase(Lexer *l, Token *input, Token *output) {
    if (l->abort_slot_input) *l->abort_slot_input = input;
    if (l->abort_slot_output) *l->abort_slot_output = output;
}

/* Append to the current phase's output array and republish it. */
static void append_output(Lexer *l, Token **arr, int *len, int *cap, Token t) {
    DA_APPEND(*arr, *len, *cap, t);
    if (l->abort_slot_output) *l->abort_slot_output = *arr;
}

static char peek(Lexer *l)      { return *l->current; }
static char peek_next(Lexer *l) { return l->current[0] ? l->current[1] : '\0'; }
static char peek_at(Lexer *l, int n) {
    for (int i = 0; i < n; i++) if (l->current[i] == '\0') return '\0';
    return l->current[n];
}
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
        .line = l->start_line,
        .col = l->start_col,
    };
}

static Token error_token(Lexer *l, const char *msg) {
    return (Token){
        .kind = TOK_ERROR,
        .start = msg,
        .length = (int)strlen(msg),
        .line = l->start_line,
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

/* Every keyword, and the token kind it lexes as. */
#define KW(s, kind) { s, (int)sizeof(s) - 1, kind }
static const struct { const char *kw; int klen; TokenKind kind; } KEYWORDS[] = {
    KW("let", TOK_LET),
    KW("mut", TOK_MUT),
    KW("struct", TOK_STRUCT),
    KW("union", TOK_UNION),
    KW("enum", TOK_ENUM),
    KW("module", TOK_MODULE),
    KW("namespace", TOK_NAMESPACE),
    KW("import", TOK_IMPORT),
    KW("from", TOK_FROM),
    KW("as", TOK_AS),
    KW("extern", TOK_EXTERN),
    KW("private", TOK_PRIVATE),
    KW("match", TOK_MATCH),
    KW("with", TOK_WITH),
    KW("when", TOK_WHEN),
    KW("if", TOK_IF),
    KW("then", TOK_THEN),
    KW("else", TOK_ELSE),
    KW("for", TOK_FOR),
    KW("in", TOK_IN),
    KW("loop", TOK_LOOP),
    KW("do", TOK_DO),
    KW("break", TOK_BREAK),
    KW("continue", TOK_CONTINUE),
    KW("return", TOK_RETURN),
    KW("defer", TOK_DEFER),
    KW("ignore", TOK_IGNORE),
    KW("some", TOK_SOME),
    KW("true", TOK_TRUE),
    KW("false", TOK_FALSE),
    KW("none", TOK_NONE),
    KW("ok", TOK_OK),
    KW("err", TOK_ERR),
    KW("error", TOK_ERROR_KW),
    KW("void", TOK_VOID),
    KW("guarded", TOK_GUARDED),
    KW("unguarded", TOK_UNGUARDED),
    KW("checked", TOK_CHECKED),
    KW("unchecked", TOK_UNCHECKED),
    KW("alloc", TOK_ALLOC),
    KW("alloca", TOK_ALLOCA),
    KW("free", TOK_FREE),
    KW("sizeof", TOK_SIZEOF),
    KW("alignof", TOK_ALIGNOF),
    KW("bitcast", TOK_BITCAST),
    KW("enum_of", TOK_ENUM_OF),
    KW("default", TOK_DEFAULT),
    KW("const", TOK_CONST),
    KW("assert", TOK_ASSERT),
    KW("static_assert", TOK_STATIC_ASSERT),
    KW("error_name", TOK_ERROR_NAME),
    KW("atomic_load_acquire", TOK_ATOMIC_LOAD),
    KW("atomic_store_release", TOK_ATOMIC_STORE),
};
#undef KW
#define KEYWORD_COUNT ((int)(sizeof KEYWORDS / sizeof KEYWORDS[0]))

int lexer_keyword_count(void) { return KEYWORD_COUNT; }

const char *lexer_keyword(int i) { return KEYWORDS[i].kw; }

static TokenKind check_keyword(const char *start, int len) {
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        if (KEYWORDS[i].klen == len && memcmp(start, KEYWORDS[i].kw, (size_t)len) == 0)
            return KEYWORDS[i].kind;
    }
    return TOK_IDENT;
}

static Token scan_identifier(Lexer *l) {
    while (isalnum((unsigned char)peek(l)) || peek(l) == '_') advance(l);
    int len = (int)(l->current - l->start);
    /* Reject identifiers containing __ (double underscore), which is reserved
     * for the compiler's mangling of namespace and module paths. The exception
     * is the extern C-name position (`extern NAME` /
     * `extern struct|union NAME`): C symbols in the implementation-reserved
     * namespace (e.g. __errno_location) are emitted verbatim, not mangled.
     * The parser requires an `as` alias there so the FC-visible name has no
     * `__`. */
    bool extern_c_name_pos =
        l->prev_kind == TOK_EXTERN ||
        ((l->prev_kind == TOK_STRUCT || l->prev_kind == TOK_UNION) &&
         l->prev_prev_kind == TOK_EXTERN);
    TokenKind kw = check_keyword(l->start, len);
    if (kw == TOK_IDENT && !extern_c_name_pos) {
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

static bool is_bin_digit(char c) { return c == '0' || c == '1'; }
static bool is_oct_digit(char c) { return c >= '0' && c <= '7'; }
static bool is_dec_digit(char c) { return isdigit((unsigned char)c); }
static bool is_hex_digit(char c) { return isxdigit((unsigned char)c); }

/* Consume a run of digits in which a '_' may separate two digits: it needs a
 * digit on each side, so none may lead the run (`0x_ff`). The caller has
 * consumed at least one character. Returns whether any digit was consumed. */
static bool scan_digits(Lexer *l, bool (*is_digit)(char)) {
    bool any = false;
    while (is_digit(peek(l)) ||
           (peek(l) == '_' && is_digit(l->current[-1]) && is_digit(peek_next(l)))) {
        if (peek(l) != '_') any = true;
        advance(l);
    }
    return any;
}

/* An exponent: `marker` in either case, an optional sign, then decimal digits.
 * Consumed only when a digit follows immediately, so `1e3 - 2` stays a
 * subtraction. Returns whether one was consumed. */
static bool scan_exponent(Lexer *l, char marker) {
    if (tolower((unsigned char)peek(l)) != marker) return false;
    char c1 = peek_next(l);
    if (!isdigit((unsigned char)c1) &&
        !((c1 == '+' || c1 == '-') && isdigit((unsigned char)peek_at(l, 2))))
        return false;
    advance(l); /* consume the marker */
    if (peek(l) == '+' || peek(l) == '-') advance(l);
    scan_digits(l, is_dec_digit);
    return true;
}

/* An integer literal's type suffix (i8 ... usize), if any, and its token. */
static Token finish_int(Lexer *l) {
    if ((peek(l) == 'i' || peek(l) == 'u') && isalnum((unsigned char)peek_next(l))) {
        advance(l);
        while (isalnum((unsigned char)peek(l))) advance(l);
    }
    return make_token(l, TOK_INT_LIT);
}

/* A float literal's f32/f64 suffix, if any, and its token. */
static Token finish_float(Lexer *l) {
    if (peek(l) == 'f' && (peek_next(l) == '3' || peek_next(l) == '6')) {
        advance(l); advance(l);
        if (peek(l) == '2' || peek(l) == '4') advance(l);
    }
    return make_token(l, TOK_FLOAT_LIT);
}

static Token scan_number_body(Lexer *l) {
    if (l->current[-1] == '0') {
        char base = (char)tolower((unsigned char)peek(l));
        if (base == 'x') {
            advance(l); /* consume x */
            bool has_digit = scan_digits(l, is_hex_digit);
            /* Hex float: optional .hexdigits + required [pP][+-]?digits exponent.
             * The 'p' exponent is mandatory in C99 hex floats because 'e' is a hex digit. */
            bool has_dot = false;
            if (peek(l) == '.' && isxdigit((unsigned char)peek_next(l))) {
                advance(l); /* consume . */
                bool frac = scan_digits(l, is_hex_digit);
                has_digit = has_digit || frac;
                has_dot = true;
            }
            bool has_exp = scan_exponent(l, 'p');
            if (has_dot && !has_exp) {
                SrcLoc loc = { .line = l->line, .col = l->start_col };
                diag_fatal(loc, "hex float literal requires a binary exponent ('p' or 'P' followed by decimal digits)");
            }
            if (has_dot || has_exp) {
                if (!has_digit) {
                    SrcLoc loc = { .line = l->line, .col = l->start_col };
                    diag_fatal(loc, "hex float literal requires at least one mantissa digit");
                }
                return finish_float(l);
            }
            if (!has_digit)
                return error_token(l, "a '0x' prefix must be followed by a hex digit");
            return finish_int(l);
        }
        if (base == 'b') {
            advance(l); /* consume b */
            if (!scan_digits(l, is_bin_digit))
                return error_token(l, "a '0b' prefix must be followed by a binary digit");
            return finish_int(l);
        }
        if (base == 'o') {
            advance(l); /* consume o */
            if (!scan_digits(l, is_oct_digit))
                return error_token(l, "a '0o' prefix must be followed by an octal digit");
            return finish_int(l);
        }
    }
    scan_digits(l, is_dec_digit);
    bool is_float = false;
    if (peek(l) == '.' && isdigit((unsigned char)peek_next(l))) {
        advance(l); /* consume . */
        scan_digits(l, is_dec_digit);
        is_float = true;
    }
    /* An e/E that does not start an exponent is not an identifier starting
     * where the number ended: `1e` can only be a mistyped exponent. */
    if (peek(l) == 'e' || peek(l) == 'E') {
        if (!scan_exponent(l, 'e'))
            return error_token(l, "float exponent requires at least one digit");
        is_float = true;
    }
    return is_float ? finish_float(l) : finish_int(l);
}

/* A numeric literal owns its suffix: scan_number_body consumes every valid one
 * (i8...usize, f32/f64, and the hex-float forms), so an identifier character
 * still adjacent to the literal is a malformed suffix: `1e9i32`, `42foo`,
 * `0x1fz`, a trailing `1_`. Reporting it here keeps the diagnostic on the
 * literal. Otherwise the token stream splits into a number and an identifier,
 * and the mistake surfaces later as a juxtaposed statement or an undefined
 * name. FC gives `1x` no meaning, so no valid program is rejected. */
static Token scan_number(Lexer *l) {
    Token t = scan_number_body(l);
    if (t.kind != TOK_ERROR && (isalnum((unsigned char)peek(l)) || peek(l) == '_'))
        return error_token(l, "invalid suffix on numeric literal");
    return t;
}

/* Check if position p (pointing past '%') looks like a format spec followed by '{'.
 * Returns length of the spec (not including '%' or '{'), or 0 if no match.
 * Format spec: optional flags (-+0# space), optional width (digits), optional .precision,
 * then a required conversion char (d,i,u,x,X,o,f,e,E,g,G,s,c,p, or T for the
 * compile-time type name). */
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

/* Consume one escape sequence, the current character being the one after its
 * backslash. Returns NULL, or the message for an invalid escape. The set is
 * the one decode_str_lit (common.c) decodes: \n \t \r \0 \\ \" \' and \xNN
 * with exactly two hex digits, which the byte counts built on the decoder
 * assume. */
static const char *scan_escape(Lexer *l) {
    char esc = peek(l);
    if (esc == 'n' || esc == 't' || esc == 'r' || esc == '\\' ||
        esc == '"' || esc == '\'' || esc == '0') {
        advance(l);
        return NULL;
    }
    if (esc == 'x') {
        advance(l);
        for (int i = 0; i < 2; i++) {
            if (!isxdigit((unsigned char)peek(l)))
                return "\\x escape requires exactly two hex digits";
            advance(l);
        }
        return NULL;
    }
    return "unrecognized escape sequence";
}

/* Scan string literal text (after opening " or after closing } of interp expr).
 * If interpolation %spec{ is found, emits tok_kind and sets up FMT_SPEC state.
 * If closing " is found, emits STRING_LIT/CSTRING_LIT (plain) or INTERP_END.
 * tok_kind is TOK_INTERP_START (TOK_CINTERP_START for a cstring) for the first
 * segment, TOK_INTERP_MID for later ones. */
static Token scan_string_body(Lexer *l, TokenKind tok_kind) {
    const char *text_start = l->current;  /* start of literal text content */

    /* A string literal never spans a line: an unescaped newline ends the scan
     * and is reported as an unterminated string, so a missing closing quote is
     * caught on its own line instead of swallowing the rest of the file (and
     * never reaches codegen, which would emit a C literal spanning two lines).
     * Use \n for a newline byte. */
    while (!at_end(l) && peek(l) != '"' && peek(l) != '\n') {
        if (peek(l) == '\\') {
            advance(l); /* skip backslash */
            if (at_end(l)) break;
            const char *err = scan_escape(l);
            if (err) return error_token(l, err);
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

    /* Reached closing " (or end of input / end of line) */
    if (at_end(l) || peek(l) == '\n') return error_token(l, "unterminated string");

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
        advance(l); /* skip backslash */
        const char *err = scan_escape(l);
        if (err) return error_token(l, err);
    } else {
        /* Same one-line rule as strings: a raw newline ends the literal rather
         * than being taken as its byte (use '\n'). */
        if (at_end(l) || peek(l) == '\n') return error_token(l, "unterminated char literal");
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
    l->start_line = l->line;

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
            /* Reject '__' in type variable names too, as for identifiers (the
             * leading apostrophe never matches). */
            int tvlen = (int)(l->current - l->start);
            for (int i = 0; i + 1 < tvlen; i++) {
                if (l->start[i] == '_' && l->start[i + 1] == '_') {
                    SrcLoc loc = { .line = l->line, .col = l->start_col };
                    diag_fatal(loc, "type variable '%.*s' contains '__' (double underscore), "
                        "which is reserved", tvlen, l->start);
                }
            }
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
            /* Check for "else if": skip spaces, then look for "if" */
            const char *saved = l->current;
            int saved_col = l->col;
            while (*l->current == ' ') advance(l);
            if (l->current[0] == 'i' && l->current[1] == 'f' &&
                !isalpha((unsigned char)l->current[2]) && l->current[2] != '_') {
                advance(l); advance(l);
                return make_token(l, TOK_HASH_ELSE_IF);
            }
            /* Plain #else: restore position */
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
    publish_phase(l, NULL, NULL);

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

        append_output(l, &tokens, &len, &cap, t);
        l->prev_prev_kind = l->prev_kind;
        l->prev_kind = t.kind;
        if (t.kind == TOK_EOF) break;
    }

    *out_count = len;
    return tokens;
}

/* ---- Conditional compilation filter (phase 1.5) ---- */

#define MAX_COND_DEPTH 32

/* Look up a flag by name. Returns NULL if not set. */
static const Flag *flag_lookup(const Flag *flags, int flag_count,
                                const char *name, int name_len) {
    for (int i = 0; i < flag_count; i++) {
        if (flags[i].name_len == name_len &&
            memcmp(flags[i].name, name, (size_t)name_len) == 0) {
            return &flags[i];
        }
    }
    return NULL;
}

/* Recursive-descent evaluator for #if expressions.
 *
 *   cond   = or_expr
 *   or_expr  = and_expr ('||' and_expr)*
 *   and_expr = unary    ('&&' unary)*
 *   unary    = '!' unary | primary
 *   primary  = '(' cond ')'
 *            | IDENT                     (presence check)
 *            | IDENT '==' STRING_LIT     (value equality)
 *            | IDENT '!=' STRING_LIT     (value inequality)
 */
typedef struct {
    Token *tokens;
    int count;
    int pos;
    const Flag *flags;
    int flag_count;
    SrcLoc directive_loc;  /* for error reporting */
} CondEval;

static bool eval_or(CondEval *e);

static SrcLoc tok_loc(Token t) {
    return (SrcLoc){ .line = t.line, .col = t.col };
}

/* True at the end of a directive's expression: past the last token, or at the
 * NEWLINE that ends the directive line. */
static bool at_expr_end(CondEval *e) {
    return e->pos >= e->count || e->tokens[e->pos].kind == TOK_NEWLINE;
}

static bool eval_primary(CondEval *e) {
    if (at_expr_end(e)) {
        diag_fatal(e->directive_loc, "unexpected end of #if expression");
    }
    Token t = e->tokens[e->pos];
    if (t.kind == TOK_LPAREN) {
        e->pos++;
        bool v = eval_or(e);
        if (at_expr_end(e) || e->tokens[e->pos].kind != TOK_RPAREN) {
            diag_fatal(tok_loc(t), "expected ')' in #if expression");
        }
        e->pos++;
        return v;
    }
    if (t.kind == TOK_IDENT) {
        e->pos++;
        const Flag *f = flag_lookup(e->flags, e->flag_count, t.start, t.length);
        if (!at_expr_end(e) && (e->tokens[e->pos].kind == TOK_EQEQ ||
                                e->tokens[e->pos].kind == TOK_BANGEQ)) {
            TokenKind op = e->tokens[e->pos].kind;
            e->pos++;
            if (at_expr_end(e) || e->tokens[e->pos].kind != TOK_STRING_LIT) {
                diag_fatal(tok_loc(t),
                    "expected string literal after '==' or '!=' in #if expression");
            }
            Token str_tok = e->tokens[e->pos];
            e->pos++;
            /* String literal token includes surrounding quotes; content is inside. */
            int content_len = str_tok.length - 2;
            const char *content = str_tok.start + 1;
            bool eq = false;
            if (f && f->value) {
                int value_len = (int)strlen(f->value);
                eq = (value_len == content_len &&
                      memcmp(f->value, content, (size_t)content_len) == 0);
            }
            return (op == TOK_EQEQ) ? eq : !eq;
        }
        /* Bare identifier: presence check */
        return f != NULL;
    }
    diag_fatal(tok_loc(t), "expected identifier, '!', or '(' in #if expression");
    return false;
}

static bool eval_unary(CondEval *e) {
    if (!at_expr_end(e) && e->tokens[e->pos].kind == TOK_BANG) {
        e->pos++;
        return !eval_unary(e);
    }
    return eval_primary(e);
}

static bool eval_and(CondEval *e) {
    bool left = eval_unary(e);
    while (!at_expr_end(e) && e->tokens[e->pos].kind == TOK_AMPAMP) {
        e->pos++;
        bool right = eval_unary(e);
        left = left && right;
    }
    return left;
}

static bool eval_or(CondEval *e) {
    bool left = eval_and(e);
    while (!at_expr_end(e) && e->tokens[e->pos].kind == TOK_PIPEPIPE) {
        e->pos++;
        bool right = eval_and(e);
        left = left || right;
    }
    return left;
}

/* Evaluate the expression starting at *i+1 (i points to the #if/#else if directive).
 * On return, *i points to the last token consumed by the expression (caller advances). */
static bool eval_cond_expr(Token *tokens, int count, int *i,
                            const Flag *flags, int flag_count,
                            SrcLoc directive_loc) {
    CondEval e = {
        .tokens = tokens,
        .count = count,
        .pos = *i + 1,
        .flags = flags,
        .flag_count = flag_count,
        .directive_loc = directive_loc,
    };
    if (at_expr_end(&e)) {
        diag_fatal(directive_loc, "#if requires an expression");
    }
    bool result = eval_or(&e);
    if (!at_expr_end(&e)) {
        diag_fatal(tok_loc(e.tokens[e.pos]),
            "unexpected token in #if expression");
    }
    *i = e.pos - 1;
    return result;
}

/* Filter out tokens in inactive #if/#else if/#else/#end branches.
 * Directive tokens are always stripped from output. */
static Token *filter_conditionals(Lexer *l, Token *tokens, int count,
                                   const Flag *flags, int flag_count,
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
                diag_fatal(tok_loc(t), "#if must appear at column 1");
            }
            bool parent_active = active[depth];
            depth++;
            if (depth >= MAX_COND_DEPTH) {
                diag_fatal(tok_loc(t), "too many nested #if directives");
            }
            bool cond = eval_cond_expr(tokens, count, &i, flags, flag_count, tok_loc(t));
            active[depth] = parent_active && cond;
            done[depth] = cond;
            /* Skip trailing newline after directive */
            if (i + 1 < count && tokens[i + 1].kind == TOK_NEWLINE) i++;
            continue;
        }

        if (t.kind == TOK_HASH_ELSE_IF) {
            if (t.col != 1) {
                diag_fatal(tok_loc(t), "#else if must appear at column 1");
            }
            if (depth == 0) {
                diag_fatal(tok_loc(t), "#else if without matching #if");
            }
            bool parent_active = (depth >= 2) ? active[depth - 1] : true;
            bool cond = eval_cond_expr(tokens, count, &i, flags, flag_count, tok_loc(t));
            active[depth] = parent_active && !done[depth] && cond;
            if (cond) done[depth] = true;
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
            append_output(l, &out, &olen, &ocap, t);
        }
    }

    if (depth != 0) {
        /* An #if is still open at the end of the input. */
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
           k == TOK_ELSE || k == TOK_LOOP || k == TOK_DO ||
           k == TOK_GUARDED || k == TOK_UNGUARDED ||
           k == TOK_CHECKED || k == TOK_UNCHECKED;
}

Token *lexer_tokenize(Lexer *l, int *out_count) {
    int raw_count;
    Token *raw = raw_tokenize(l, &raw_count);

    /* Filter conditional compilation directives */
    int filtered_count;
    publish_phase(l, raw, NULL);
    Token *filtered = filter_conditionals(l, raw, raw_count,
        l->flags, l->flag_count, &filtered_count);
    publish_phase(l, filtered, NULL);
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

    bool saw_decl_keyword = false;    /* let/struct/union/enum/error/module on current line */
    TokenKind last_kind = TOK_EOF;    /* last non-NEWLINE token emitted */
    int bracket_depth = 0;            /* depth inside () [] {}; suppresses layout */

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
                /* Trailing newlines: skip */
                i = j - 1;
                continue;
            }

            int next_col = raw[j].col;

            /* Determine if the last token before this newline was a block-former */
            bool block_former = is_always_block_former(last_kind);
            if (!block_former && last_kind == TOK_EQ && saw_decl_keyword)
                block_former = true;

            /* Reset line-local state */
            saw_decl_keyword = false;

            /* Close same-level match blocks when next line doesn't start with | */
            while (indent_depth > 0 && is_match_block[indent_depth] &&
                   next_col <= indent_stack[indent_depth]) {
                if (next_col == indent_stack[indent_depth] && raw[j].kind == TOK_PIPE)
                    break; /* still in match block: peer arm */
                indent_depth--;
                Token dedent = make_layout_token(TOK_DEDENT, t.line, t.col);
                append_output(l, &out, &olen, &ocap, dedent);
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
                append_output(l, &out, &olen, &ocap, indent_tok);
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
                append_output(l, &out, &olen, &ocap, indent_tok);
            } else if (next_col == indent_stack[indent_depth]) {
                /* Peer separator, but not after INDENT or at the start */
                if (olen > 0 && out[olen-1].kind != TOK_INDENT && out[olen-1].kind != TOK_NEWLINE) {
                    Token nl = make_layout_token(TOK_NEWLINE, t.line, t.col);
                    append_output(l, &out, &olen, &ocap, nl);
                }
            } else if (next_col < indent_stack[indent_depth]) {
                /* Dedent: pop until we match */
                while (indent_depth > 0 && next_col < indent_stack[indent_depth]) {
                    indent_depth--;
                    Token dedent = make_layout_token(TOK_DEDENT, t.line, t.col);
                    append_output(l, &out, &olen, &ocap, dedent);
                }
                /* After dedent, close any match block we landed on */
                while (indent_depth > 0 && is_match_block[indent_depth] &&
                       next_col <= indent_stack[indent_depth]) {
                    if (next_col == indent_stack[indent_depth] && raw[j].kind == TOK_PIPE)
                        break;
                    indent_depth--;
                    Token dedent2 = make_layout_token(TOK_DEDENT, t.line, t.col);
                    append_output(l, &out, &olen, &ocap, dedent2);
                }
                if (next_col != indent_stack[indent_depth]) {
                    SrcLoc loc = { .line = raw[j].line, .col = raw[j].col };
                    diag_fatal(loc, "inconsistent indentation (col %d does not match any block)", next_col);
                }
                /* Emit newline after dedents for peer separation */
                if (olen > 0 && out[olen-1].kind != TOK_NEWLINE) {
                    Token nl = make_layout_token(TOK_NEWLINE, t.line, t.col);
                    append_output(l, &out, &olen, &ocap, nl);
                }
            } else {
                /* next_col > indent_stack top but not a block former: a
                 * continuation line. Suppress the newline; the tokens continue
                 * the current expression. */
            }

            /* Skip over the consecutive newlines we already processed */
            i = j - 1;
            continue;
        }

        /* Skip raw EOF; we emit our own after the DEDENTs */
        if (t.kind == TOK_EOF) continue;

        /* Track bracket depth for layout suppression */
        if (t.kind == TOK_LPAREN || t.kind == TOK_LBRACKET || t.kind == TOK_LBRACE)
            bracket_depth++;
        else if (t.kind == TOK_RPAREN || t.kind == TOK_RBRACKET || t.kind == TOK_RBRACE) {
            if (bracket_depth > 0) bracket_depth--;
        }

        /* Non-newline token: emit it */
        append_output(l, &out, &olen, &ocap, t);
        last_kind = t.kind;

        /* Track context for the `=` block-former detection */
        if (t.kind == TOK_LET || t.kind == TOK_STRUCT ||
            t.kind == TOK_UNION || t.kind == TOK_ENUM ||
            t.kind == TOK_ERROR_KW || t.kind == TOK_MODULE) {
            saw_decl_keyword = true;
        }
    }

    /* Emit remaining DEDENTs and our EOF, located at the raw stream's EOF (the
       raw array always ends with one) so end-of-file diagnostics point at the
       end of the source instead of the meaningless 0:0. */
    Token raw_eof = raw[raw_count - 1];
    while (indent_depth > 0) {
        indent_depth--;
        Token dedent = make_layout_token(TOK_DEDENT, raw_eof.line, raw_eof.col);
        append_output(l, &out, &olen, &ocap, dedent);
    }

    Token eof = make_layout_token(TOK_EOF, raw_eof.line, raw_eof.col);
    append_output(l, &out, &olen, &ocap, eof);

    publish_phase(l, NULL, out);
    free(raw);
    *out_count = olen;
    return out;
}
