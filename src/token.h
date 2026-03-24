#pragma once

typedef enum {
    /* Literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_CSTRING_LIT,
    TOK_CHAR_LIT,

    /* Identifiers */
    TOK_IDENT,
    TOK_TYPE_VAR,       /* 'a, 'b */

    /* Keywords */
    TOK_LET,
    TOK_MUT,
    TOK_STRUCT,
    TOK_UNION,
    TOK_MODULE,
    TOK_NAMESPACE,
    TOK_IMPORT,
    TOK_FROM,
    TOK_AS,
    TOK_EXTERN,
    TOK_PRIVATE,
    TOK_MATCH,
    TOK_WITH,
    TOK_IF,
    TOK_THEN,
    TOK_ELSE,
    TOK_FOR,
    TOK_IN,
    TOK_LOOP,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_RETURN,
    TOK_SOME,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NONE,
    TOK_VOID,

    /* Built-in operators (reserved identifiers) */
    TOK_ALLOC,
    TOK_FREE,
    TOK_SIZEOF,
    TOK_DEFAULT,
    TOK_CONST,
    /* String interpolation */
    TOK_INTERP_START,       /* leading text of interpolated string */
    TOK_CINTERP_START,      /* leading text of interpolated cstring */
    TOK_INTERP_MID,         /* middle text between interpolation segments */
    TOK_INTERP_END,         /* trailing text of interpolated string */
    TOK_FMT_SPEC,           /* format specifier e.g. "d", "04x", "8.2f" */

    /* Operators */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_STAR,           /* * */
    TOK_SLASH,          /* / */
    TOK_PERCENT,        /* % */
    TOK_AMP,            /* & */
    TOK_PIPE,           /* | */
    TOK_CARET,          /* ^ */
    TOK_TILDE,          /* ~ */
    TOK_BANG,           /* ! */
    TOK_EQ,             /* = */
    TOK_EQEQ,          /* == */
    TOK_BANGEQ,         /* != */
    TOK_LT,            /* < */
    TOK_GT,             /* > */
    TOK_LTEQ,          /* <= */
    TOK_GTEQ,          /* >= */
    TOK_LTLT,          /* << */
    TOK_GTGT,          /* >> */
    TOK_AMPAMP,         /* && */
    TOK_PIPEPIPE,       /* || */
    TOK_ARROW,          /* -> */
    TOK_DOTDOT,         /* .. */
    TOK_ELLIPSIS,       /* ... */
    TOK_DOT,            /* . */
    TOK_COLONCOLON,     /* :: */
    TOK_COLON,          /* : */
    TOK_COMMA,          /* , */
    TOK_QUESTION,       /* ? */
    TOK_SEMICOLON,      /* ; */

    /* Delimiters */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */

    /* Layout */
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,

    /* Preprocessor */
    TOK_HASH_IF,
    TOK_HASH_ELSE,
    TOK_HASH_ELSE_IF,
    TOK_HASH_END,

    /* Special */
    TOK_EOF,
    TOK_ERROR,

    TOK_COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;  /* pointer into source buffer */
    int length;
    int line;
    int col;
} Token;

const char *token_kind_name(TokenKind kind);
