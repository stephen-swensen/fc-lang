#pragma once
#include "token.h"
#include "types.h"
#include "diag.h"

/* ---- Provenance tracking for escape analysis ---- */

typedef enum {
    PROV_UNKNOWN,   /* default: function params, call returns, extern results */
    PROV_STACK,     /* stack-allocated: &local, array literals, interp strings, cstr casts */
    PROV_HEAP,      /* heap-allocated: alloc() results */
    PROV_STATIC,    /* static storage: string/cstring literals */
} Provenance;

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
    EXPR_INTERP_STRING,
    EXPR_ASSIGN,
    EXPR_SOME,
    EXPR_DEREF_FIELD,   /* x->f */
    EXPR_LET,           /* let binding inside a block */
    EXPR_LET_DESTRUCT,  /* let { field = name, ... } = expr */
    EXPR_TYPE_VAR_REF,  /* 'a in expression position (for 'a.min etc.) */
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

typedef struct {
    const char *name;           /* FC source name */
    const char *codegen_name;   /* C codegen name from outer scope */
    Type *type;
} Capture;

struct FieldInit {
    const char *name;
    Expr *value;
};

typedef struct {
    bool is_literal;        /* true = literal text, false = format expression */
    const char *text;       /* literal text or format specifier string */
    int text_length;        /* length of text or spec */
    char conversion;        /* for format segments: 'd', 'x', 'f', 's', etc. */
    Expr *expr;             /* for format segments: the expression (NULL for literals) */
} InterpSegment;

typedef struct {
    const char *name;       /* struct field name */
    Pattern *pattern;       /* inner pattern (binding, literal, nested struct, etc.) */
    Type *resolved_type;    /* filled by pass2: resolved type of this field */
} FieldPattern;

struct Expr {
    ExprKind kind;
    SrcLoc loc;
    Type *type;         /* filled in by pass2 */
    Provenance prov;    /* filled in by pass2: storage provenance for escape analysis */
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
        struct { const char *name; const char *codegen_name; bool is_local; } ident;

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
            bool is_indirect;     /* callee is a function value (fat pointer) */
            bool is_extern_call;  /* callee is an extern function (no _ctx) */
            const char *mangled_name;   /* C function name for monomorphized call, NULL for non-generic */
        } call;

        /* EXPR_FIELD, EXPR_DEREF_FIELD */
        struct {
            Expr *object; const char *name; const char *codegen_name;
            Type **type_args;       /* explicit type args for generic variant: name<Types>.variant */
            int type_arg_count;
        } field;

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
            Expr *iter;             /* collection expr, or range start */
            Expr *range_end;        /* non-NULL for range iteration (lo..hi) */
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
            Capture *captures;      /* filled by pass2, NULL if non-capturing */
            int capture_count;
            const char *lifted_name; /* C function name for lambdas in expression position */
            const char **explicit_type_vars;    /* <'a, 'b> prefix, NULL if implicit-only */
            int explicit_type_var_count;
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
        struct {
            Type *alloc_type;     /* type to allocate (NULL for init-from-expr form) */
            Expr *size_expr;      /* array size for alloc(T[N]) — NULL for single */
            Expr *init_expr;      /* init expression for alloc(expr) — NULL for type-only */
        } alloc_expr;

        /* EXPR_FREE */
        struct { Expr *operand; } free_expr;

        /* EXPR_SIZEOF */
        struct { Type *target; } sizeof_expr;

        /* EXPR_DEFAULT */
        struct { Type *target; } default_expr;

        /* EXPR_INTERP_STRING */
        struct {
            InterpSegment *segments;
            int segment_count;
        } interp_string;

        /* EXPR_SOME */
        struct { Expr *value; } some_expr;

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
            const char *codegen_name;   /* unique C name for shadowing */
            bool let_is_mut;
            Expr *let_init;
            Type *let_type;     /* filled by pass2 */
        } let_expr;

        /* EXPR_LET_DESTRUCT */
        struct {
            Pattern *pattern;       /* PAT_STRUCT pattern */
            bool is_mut;
            Expr *init;
            Type *init_type;        /* filled by pass2 */
            const char *tmp_name;   /* codegen temp name for the RHS */
        } let_destruct;

        /* EXPR_TYPE_VAR_REF — 'a in expression position (for 'a.min etc.) */
        struct { const char *name; } type_var_ref;
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
            FieldPattern *fields;
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
            const char *codegen_name;   /* mangled C name for module members */
            bool is_mut;
            Expr *init;
            Type *resolved_type;    /* filled by pass2 */
        } let;

        /* DECL_STRUCT */
        struct {
            const char *name;
            const char *c_name;     /* C struct tag name for extern structs, NULL for normal */
            bool is_extern;         /* true for extern struct declarations */
            StructField *fields;
            int field_count;
            const char **type_params;   /* type var names, e.g. ["'a", "'b"] */
            int type_param_count;
            bool is_generic;
        } struc;

        /* DECL_UNION */
        struct {
            const char *name;
            UnionVariant *variants;
            int variant_count;
            const char **type_params;
            int type_param_count;
            bool is_generic;
        } unio;

        /* DECL_MODULE */
        struct {
            const char *name;
            const char *ns_prefix;  /* namespace prefix (mangled, e.g. "acme_graphics"), NULL = global */
            const char *from_lib;   /* NULL unless module X from "lib" */
            const char *define_macro; /* NULL unless define "MACRO" "VALUE" */
            const char *define_value; /* NULL unless define present */
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
