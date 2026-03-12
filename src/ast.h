#pragma once
#include "token.h"
#include "types.h"
#include "diag.h"

/* ---- Expression nodes ---- */

typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_BOOL_LIT,
    EXPR_CHAR_LIT,
    EXPR_STRING_LIT,
    EXPR_CSTRING_LIT,
    EXPR_IDENT,
    EXPR_BINARY,
    EXPR_UNARY_PREFIX,
    EXPR_UNARY_POSTFIX,
    EXPR_CALL,
    EXPR_FIELD,
    EXPR_INDEX,
    EXPR_SLICE,
    EXPR_CAST,
    EXPR_IF,
    EXPR_MATCH,
    EXPR_LOOP,
    EXPR_FOR,
    EXPR_BREAK,
    EXPR_CONTINUE,
    EXPR_RETURN,
    EXPR_BLOCK,
    EXPR_FUNC,
    EXPR_STRUCT_LIT,
    EXPR_ARRAY_LIT,
    EXPR_ALLOC,
    EXPR_FREE,
    EXPR_SIZEOF,
    EXPR_DEFAULT,
    EXPR_PRINT,
    EXPR_ASSIGN,
    EXPR_SOME,
    EXPR_NONE,
    EXPR_DEREF_FIELD,   /* x->f */
    EXPR_LET,           /* let binding inside a block */
} ExprKind;

typedef struct Expr Expr;
typedef struct MatchArm MatchArm;
typedef struct Pattern Pattern;
typedef struct Param Param;
typedef struct FieldInit FieldInit;

struct Param {
    const char *name;
    Type *type;         /* parsed type annotation */
    SrcLoc loc;
};

struct FieldInit {
    const char *name;
    Expr *value;
};

struct Expr {
    ExprKind kind;
    SrcLoc loc;
    Type *type;         /* filled in by pass2 */
    union {
        /* EXPR_INT_LIT */
        struct { int64_t value; Type *lit_type; } int_lit;

        /* EXPR_FLOAT_LIT */
        struct { double value; Type *lit_type; } float_lit;

        /* EXPR_BOOL_LIT */
        struct { bool value; } bool_lit;

        /* EXPR_CHAR_LIT */
        struct { uint8_t value; } char_lit;

        /* EXPR_STRING_LIT */
        struct { const char *value; int length; } string_lit;

        /* EXPR_CSTRING_LIT */
        struct { const char *value; int length; } cstring_lit;

        /* EXPR_IDENT */
        struct { const char *name; } ident;

        /* EXPR_BINARY */
        struct { TokenKind op; Expr *left; Expr *right; } binary;

        /* EXPR_UNARY_PREFIX */
        struct { TokenKind op; Expr *operand; } unary_prefix;

        /* EXPR_UNARY_POSTFIX */
        struct { TokenKind op; Expr *operand; } unary_postfix;

        /* EXPR_CALL */
        struct {
            Expr *func;
            Expr **args;
            int arg_count;
            Type **type_args;
            int type_arg_count;
        } call;

        /* EXPR_FIELD, EXPR_DEREF_FIELD */
        struct { Expr *object; const char *name; } field;

        /* EXPR_INDEX */
        struct { Expr *object; Expr *index; } index;

        /* EXPR_SLICE */
        struct { Expr *object; Expr *lo; Expr *hi; } slice;

        /* EXPR_CAST */
        struct { Type *target; Expr *operand; } cast;

        /* EXPR_IF */
        struct { Expr *cond; Expr *then_body; Expr *else_body; } if_expr;

        /* EXPR_LOOP */
        struct { Expr **body; int body_count; } loop_expr;

        /* EXPR_FOR */
        struct {
            const char *var;
            const char *index_var;  /* NULL if not i,x form */
            Expr *iter;
            Expr **body;
            int body_count;
        } for_expr;

        /* EXPR_BREAK */
        struct { Expr *value; } break_expr;

        /* EXPR_RETURN */
        struct { Expr *value; } return_expr;

        /* EXPR_BLOCK */
        struct { Expr **stmts; int count; } block;

        /* EXPR_FUNC */
        struct {
            Param *params;
            int param_count;
            Expr **body;
            int body_count;
        } func;

        /* EXPR_STRUCT_LIT */
        struct {
            const char *type_name;
            FieldInit *fields;
            int field_count;
        } struct_lit;

        /* EXPR_ARRAY_LIT */
        struct {
            Type *elem_type;
            Expr *size_expr;    /* NULL for unsized */
            Expr **elems;
            int elem_count;
        } array_lit;

        /* EXPR_ALLOC */
        struct { Expr *operand; } alloc_expr;

        /* EXPR_FREE */
        struct { Expr *operand; } free_expr;

        /* EXPR_SIZEOF */
        struct { Type *target; } sizeof_expr;

        /* EXPR_DEFAULT */
        struct { Type *target; } default_expr;

        /* EXPR_PRINT (also EPRINT, FPRINT, SPRINT) */
        struct {
            TokenKind print_kind;   /* TOK_PRINT, TOK_EPRINT, etc. */
            Expr *dest;             /* file handle for fprint, buffer for sprint */
            Expr *fmt;
            Expr **args;
            int arg_count;
        } print_expr;

        /* EXPR_SOME */
        struct { Expr *value; } some_expr;

        /* EXPR_NONE */
        struct { Type *target; } none_expr;

        /* EXPR_MATCH */
        struct {
            Expr *subject;
            MatchArm *arms;
            int arm_count;
        } match_expr;

        /* EXPR_ASSIGN */
        struct { Expr *target; Expr *value; } assign;

        /* EXPR_LET (block-local) */
        struct {
            const char *let_name;
            bool let_is_mut;
            Expr *let_init;
            Type *let_type;     /* filled by pass2 */
        } let_expr;
    };
};

/* ---- Pattern nodes ---- */

typedef enum {
    PAT_WILDCARD,
    PAT_BINDING,
    PAT_INT_LIT,
    PAT_CHAR_LIT,
    PAT_BOOL_LIT,
    PAT_STRING_LIT,
    PAT_NONE,
    PAT_SOME,
    PAT_VARIANT,
    PAT_STRUCT,
} PatternKind;

struct Pattern {
    PatternKind kind;
    SrcLoc loc;
    union {
        struct { const char *name; } binding;
        struct { int64_t value; Type *lit_type; } int_lit;
        struct { uint8_t value; } char_lit;
        struct { bool value; } bool_lit;
        struct { const char *value; int length; } string_lit;
        struct { Pattern *inner; } some_pat;
        struct {
            const char *variant;
            Pattern *payload;   /* NULL if no payload */
        } variant;
        struct {
            FieldInit *fields;  /* reuse: name = pattern stored as Expr */
            int field_count;
        } struc;
    };
};

struct MatchArm {
    Pattern *pattern;
    Expr **body;
    int body_count;
    SrcLoc loc;
};

/* ---- Declaration nodes ---- */

typedef enum {
    DECL_LET,
    DECL_STRUCT,
    DECL_UNION,
    DECL_MODULE,
    DECL_IMPORT,
    DECL_EXTERN,
    DECL_NAMESPACE,
} DeclKind;

typedef struct Decl Decl;

struct Decl {
    DeclKind kind;
    SrcLoc loc;
    bool is_private;
    union {
        /* DECL_LET */
        struct {
            const char *name;
            bool is_mut;
            Expr *init;
            Type *resolved_type;    /* filled by pass2 */
        } let;

        /* DECL_STRUCT */
        struct {
            const char *name;
            StructField *fields;
            int field_count;
        } struc;

        /* DECL_UNION */
        struct {
            const char *name;
            UnionVariant *variants;
            int variant_count;
        } unio;

        /* DECL_MODULE */
        struct {
            const char *name;
            const char *from_lib;   /* NULL unless module X from "lib" */
            Decl **decls;
            int decl_count;
        } module;

        /* DECL_IMPORT */
        struct {
            const char *name;
            const char *alias;
            const char *from_module;
            const char *from_namespace;
            bool is_wildcard;
        } import;

        /* DECL_EXTERN */
        struct {
            const char *name;
            const char *alias;
            Type *type;
        } ext;

        /* DECL_NAMESPACE */
        struct { const char *name; } ns;
    };
};

/* ---- Program ---- */

typedef struct {
    Decl **decls;
    int decl_count;
} Program;
