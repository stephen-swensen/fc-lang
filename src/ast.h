#pragma once
#include "token.h"
#include "types.h"
#include "diag.h"

/* ---- Provenance tracking for escape analysis ---- */

typedef enum {
    PROV_UNKNOWN,   /* default: function params, call returns, extern results */
    PROV_STACK,     /* stack-allocated: &local, slice literals, interp strings, cstr casts */
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
    EXPR_VOID_LIT,      /* void(): a void-typed expression */
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
    EXPR_TUPLE_LIT,     /* { expr, expr, ... }: positional anonymous tuple */
    EXPR_ARRAY_LIT,
    EXPR_SLICE_LIT,     /* T[] { ptr = expr, len = expr } */
    EXPR_ALLOC,
    EXPR_FREE,
    EXPR_SIZEOF,
    EXPR_ALIGNOF,
    EXPR_BITCAST,       /* bitcast(T, x): reinterpret x's bits as scalar type T */
    EXPR_ENUM_OF,       /* enum_of(E, x): checked integer-to-enum conversion, yields E? */
    EXPR_DEFAULT,
    EXPR_INTERP_STRING,
    EXPR_ASSIGN,
    EXPR_SOME,
    EXPR_OK,            /* ok(v): result construction, infers from payload */
    EXPR_ERR,           /* err(T, code): result construction, type-anchored like none(T) */
    EXPR_ERROR_NAME,    /* error_name(e): str? name of a declared error code */
    EXPR_DEREF_FIELD,   /* p.f through a pointer (pass2 rewrites EXPR_FIELD to this) */
    EXPR_LET,           /* let binding inside a block */
    EXPR_LET_DESTRUCT,  /* let { field = name, ... } = expr */
    EXPR_TYPE_VAR_REF,  /* 'a in expression position (for 'a.min etc.) */
    EXPR_ASSERT,
    EXPR_STATIC_ASSERT,  /* compile-time predicate; statement position, emits nothing */
    EXPR_DEFER,
    EXPR_IGNORE,       /* ignore expr: evaluate for effect, yield void */
    EXPR_ATOMIC_LOAD,   /* atomic_load_acquire(p) */
    EXPR_ATOMIC_STORE,  /* atomic_store_release(p, v) */
    EXPR_GUARD,         /* guarded/unguarded (precondition guards) or checked/unchecked (overflow) */
    EXPR_ERROR,         /* parse-error placeholder; carries only kind+loc. Exists only when
                           diag_error_count()>0, so it never reaches codegen. pass2 types it
                           as type_error() without a new diagnostic (the parser reported one). */
} ExprKind;

typedef struct Expr Expr;
typedef struct MatchArm MatchArm;
typedef struct Pattern Pattern;
typedef struct Param Param;
typedef struct FieldInit FieldInit;

struct Param {
    const char *name;
    const char *codegen_name;  /* unique C name minted by pass2 (see local_c_name) */
    Type *type;         /* parsed type annotation */
    SrcLoc loc;
};

typedef struct Capture {
    const char *name;           /* FC source name */
    const char *codegen_name;   /* C codegen name from outer scope */
    Type *type;
    Provenance prov;            /* provenance of the captured value, recorded at
                                   capture time so alloc(closure) can reject
                                   promoting a context that holds stack data */
} Capture;

struct FieldInit {
    const char *name;
    Expr *value;
};

typedef struct InterpSegment {
    bool is_literal;        /* true = literal text, false = format expression */
    const char *text;       /* literal text or format specifier string */
    int text_length;        /* length of text or spec */
    char conversion;        /* for format segments: 'd', 'x', 'f', 's', etc. */
    Expr *expr;             /* for format segments: the expression (NULL for literals) */
} InterpSegment;

/* One static_assert, a line in a struct/union body or a statement in a
 * function body: an instantiation predicate over const generic params. */
typedef struct StaticAssert {
    Expr *cond;
    const char *msg;        /* NUL-terminated literal content */
    SrcLoc loc;
    const char *owner;      /* source-level owner name for diagnostics
                               ("uwide", "from_u64"); decl names get mangled */
    bool judged;            /* condition was fully concrete (no const params)
                               and pass2 judged it once, up front. A type that
                               is never monomorphized still gets its verdict,
                               and mono_register does not judge it again per
                               instance */
    bool typed;             /* a function body's condition: type-checked as an
                               ordinary expression, so each instance evaluates
                               it at those types (const_eval_typed), as a
                               concrete condition is folded. A type body's
                               condition is evaluated in the i64 const domain
                               (const_type_eval), concrete or not */
} StaticAssert;

typedef struct FieldPattern {
    const char *name;       /* struct field name */
    Pattern *pattern;       /* inner pattern (binding, literal, nested struct, etc.) */
    Type *resolved_type;    /* filled by pass2: resolved type of this field */
} FieldPattern;

struct Expr {
    ExprKind kind;
    SrcLoc loc;
    Type *type;         /* filled in by pass2 */
    Provenance prov;    /* filled in by pass2: storage provenance for escape analysis */
    /* Provenance of the values stored in this container, as distinct from
     * `prov`, which describes its backing store. A slice literal always lives
     * on the stack (`prov == PROV_STACK`) yet may hold heap or static values,
     * so an element load must not inherit the backing's tag. PROV_UNKNOWN
     * means "not tracked here" (function results, raw-parts slices, params). */
    Provenance elem_prov;
    /* Set by pass2 when this node copies a value out of read-only storage and
     * the value's type involves type variables: whether the copy is allowed
     * depends on the instance, so validate_generic_expr decides it there. For
     * a `for`, `match` or destructuring `let` it is the element, subject or
     * initializer that is copied into bindings. */
    bool readonly_copy;
    union {
        /* EXPR_INT_LIT */
        struct { uint64_t value; Type *lit_type; bool out_of_range;
                 bool negative; /* value is the two's-complement pattern of a
                                   negated literal (or a negative constant) */ } int_lit;

        /* EXPR_FLOAT_LIT */
        struct { double value; Type *lit_type; bool out_of_range; bool underflow; } float_lit;

        /* EXPR_BOOL_LIT */
        struct { bool value; } bool_lit;

        /* EXPR_CHAR_LIT */
        struct { uint8_t value; } char_lit;

        /* EXPR_STRING_LIT */
        struct { const char *value; int length; } string_lit;

        /* EXPR_CSTRING_LIT */
        struct { const char *value; int length; } cstring_lit;

        /* EXPR_IDENT */
        struct {
            const char *name;
            const char *codegen_name;
            bool is_local;
            bool is_mut;
            struct Symbol *resolved_sym;       /* Symbol resolved by pass2 (module, struct, union, let) */
            struct Symbol *companion_module;   /* non-NULL when resolved_sym is a struct/union with a companion module */
            SrcLoc resolved_local_loc;         /* def loc of a resolved block-local binding (param, let, for-var,
                                                  match pattern) for editor go-to-def; {0} if global/unresolved */
            bool resolved_local_is_param;      /* the block-local is a function parameter; hover shows name: type
                                                  only (a param has no doc comment of its own to scan for) */
            bool is_std_stream;                /* the built-in stdin, stdout or stderr (not a binding
                                                  that happens to share the name) */
            struct Expr *static_fn;            /* a local `let` bound to a static function
                                                  (fn_static_target): the expression naming
                                                  that function (set by pass2) */
        } ident;

        /* EXPR_BINARY */
        struct { TokenKind op; Expr *left; Expr *right; } binary;

        /* EXPR_UNARY_PREFIX */
        struct { TokenKind op; Expr *operand; } unary_prefix;

        /* EXPR_UNARY_POSTFIX: x! (unwrap-or-abort) and x? (propagation).
         * expr_text: operand source text for the unwrap abort message (x! only).
         * prop_fn_ret: for x?, the enclosing function's resolved return type,
         * stamped by pass2 once the body is checked; codegen builds the
         * early-return failure value (err(code)/none) at this type. */
        struct { TokenKind op; Expr *operand; const char *expr_text; int expr_text_len;
                 Type *prop_fn_ret; } unary_postfix;

        /* EXPR_CALL */
        struct {
            Expr *func;
            Expr **args;
            int arg_count;
            Type **type_args;
            int type_arg_count;
            bool is_indirect;     /* callee is a function value (fat pointer) */
            bool is_extern_call;  /* callee is an extern function (no _ctx) */
            bool bare_inst;       /* `name<Types>` in value position with no '(': explicit type
                                     args but no call; always an error, rejected in pass2 */
            const char *mangled_name;   /* C function name for monomorphized call, NULL for non-generic */
            struct Symbol *resolved_callee; /* resolved in pass2, used by mono discovery */
        } call;

        /* EXPR_FIELD, EXPR_DEREF_FIELD */
        struct {
            Expr *object; const char *name; const char *codegen_name;
            SrcLoc name_loc;        /* source loc of the field-name token (editor queries) */
            Type **type_args;       /* explicit type args for generic variant: name<Types>.variant */
            int type_arg_count;
            Type *fixed_array_type; /* non-NULL if field is a fixed-size inline array (TYPE_FIXED_ARRAY) */
            bool is_variant_constructor; /* true when this is union variant construction, not field access */
            bool is_extern_const;       /* true when this is an extern constant (not a function) */
            bool is_type_property;      /* true for static type properties (i32.min, f64.nan, ...) */
            struct Symbol *resolved_member; /* for module-member access (mod.member): the resolved
                                               member Symbol, set in pass2; used by editor queries
                                               (go-to-definition). NULL for plain struct-field access. */
            struct Symbol *companion_module; /* set in pass2 when the member is a type sharing its
                                                name with a sibling module (a companion pair), so a
                                                chain through it (`a.t.m`) reaches the module */
        } field;

        /* EXPR_INDEX */
        struct { Expr *object; Expr *index; } index;

        /* EXPR_SLICE */
        struct { Expr *object; Expr *lo; Expr *hi; } slice;

        /* EXPR_CAST */
        struct {
            Type *target;
            Expr *operand;
            bool bounded;     /* (cstr[N]): a bounded str-to-cstr cast, which copies
                                 min(len, N-1) bytes + NUL into a hoisted uint8[N] */
            int64_t buffer_size;  /* its N, as written (pass2 judges it) */
            const char *codegen_backing_name;  /* hoisted backing array name (set in codegen) */
            bool licensed;    /* true when this is the direct init of alloc(...)/alloca(...).
                                 That allows an otherwise illegal unbounded (cstr) str-to-cstr
                                 cast, since the wrapping alloc/alloca gives it a home (heap or
                                 dynamic stack). */
        } cast;

        /* EXPR_GUARD: two independent lexical axes sharing one node:
           - guard axis (guarded/unguarded): the value-precondition runtime guards
             (float-to-int saturation, integer divide/modulo zero check, slice bounds).
           - overflow axis (checked/unchecked): integer-overflow detection on
             `+ - *`, signed `/` at INT_MIN/-1, and lossy integer narrowing casts,
             plus string truncation by a (cstr[N]) cast or a `%.Ns` precision.
           is_overflow_axis selects which axis; enable is the polarity within it
           (guard axis: true=guarded; overflow axis: true=checked). */
        struct { Expr *body; bool is_overflow_axis; bool enable; } guard;

        /* EXPR_IF */
        struct { Expr *cond; Expr *then_body; Expr *else_body; } if_expr;

        /* EXPR_LOOP */
        struct { Expr **body; int body_count; } loop_expr;

        /* EXPR_FOR */
        struct {
            const char *var;        /* NULL when var_pattern is set */
            const char *var_codegen_name;   /* unique C name minted by pass2 */
            struct Pattern *var_pattern; /* non-NULL: destructure the element (PAT_TUPLE/PAT_STRUCT) */
            const char *elem_tmp;   /* codegen temp holding the element when destructuring */
            const char *index_var;  /* NULL if not i,x form */
            const char *index_codegen_name; /* unique C name minted by pass2 */
            SrcLoc var_loc;         /* source loc of `var` (editor go-to-def); {0} if pattern/absent */
            SrcLoc index_var_loc;   /* source loc of `index_var` (editor go-to-def); {0} if absent */
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
            const char *codegen_ctx_backing_name; /* hoisted _ctx_<lifted> backing local (capturing lambdas) */
            const char *self_codegen_name; /* non-NULL: self-recursive let binding's codegen name */
            bool self_referenced;          /* set by pass2 if the self name is actually used */
            bool heap_alloc;               /* set by pass2: alloc(lambda); the context goes to
                                              the heap at the alloc site, no stack backing hoisted */
            const char **explicit_type_vars;    /* <'a, 'b> prefix, NULL if implicit-only */
            int explicit_type_var_count;
        } func;

        /* EXPR_STRUCT_LIT */
        struct {
            const char *type_name;
            FieldInit *fields;
            int field_count;
            struct Symbol *resolved_sym; /* resolved in pass2, used by mono discovery */
        } struct_lit;

        /* EXPR_TUPLE_LIT: { e0, e1, ... } positional anonymous tuple */
        struct {
            Expr **elems;
            int elem_count;
        } tuple_lit;

        /* EXPR_ARRAY_LIT */
        struct {
            Type *elem_type;
            Expr *size_expr;    /* NULL for unsized */
            Expr **elems;
            int elem_count;
            const char *codegen_backing_name; /* non-NULL when a backing array was
                                                 lifted out of line: a file-scope
                                                 static (const context) or a
                                                 function-entry local (stack context,
                                                 reused per loop iteration) */
            bool codegen_backing_rodata;      /* the lifted backing belongs to a frozen
                                                 module constant, so it is emitted
                                                 `static const` and the slice header
                                                 casts its .ptr; FC has already
                                                 rejected every write through it */
        } array_lit;

        /* EXPR_SLICE_LIT: T[] { ptr = expr, len = expr } */
        struct {
            Type *elem_type;    /* element type (e.g., uint8 for str) */
            Expr *ptr_expr;
            Expr *len_expr;
            bool  len_nonneg;   /* pass2 proved len >= 0, so codegen skips the
                                 * negative-length runtime guard */
        } slice_lit;

        /* EXPR_ALLOC */
        struct {
            Type *alloc_type;     /* type to allocate (NULL for init-from-expr form) */
            Expr *size_expr;      /* array size for alloc(T[N])/alloc(T,N); NULL for single */
            Expr *init_expr;      /* init expression for alloc(expr); NULL for type-only */
            bool alloc_raw;       /* true for alloc(T, N) -> T*?, false for alloc(T[N]) -> T[]? */
            bool is_stack;        /* true for alloca(...): dynamic stack, no option, no free */
            Expr *closure_src;    /* set by pass2 for alloc(f) where f is a local let bound
                                     to a capturing lambda: the lambda whose context layout
                                     the heap copy uses (init_expr stays the EXPR_IDENT) */
        } alloc_expr;

        /* EXPR_FREE */
        struct { Expr *operand; } free_expr;

        /* EXPR_SIZEOF */
        struct { Type *target; } sizeof_expr;

        /* EXPR_ALIGNOF */
        struct { Type *target; } alignof_expr;

        /* EXPR_BITCAST: bitcast(T, x) reinterprets x's bytes as scalar type T.
         * target and operand must be equal-size fixed-width scalars (checked in
         * pass2); no runtime failure mode, so no guard/checked variant. */
        struct { Type *target; Expr *operand; } bitcast_expr;

        /* EXPR_ENUM_OF: enum_of(E, x), a membership-checked conversion of an
         * integer to enum E; yields E? (some on a declared value, none otherwise). */
        struct { Type *target; Expr *operand; } enum_of_expr;

        /* EXPR_DEFAULT */
        struct { Type *target; } default_expr;

        /* EXPR_INTERP_STRING */
        struct {
            InterpSegment *segments;
            int segment_count;
            bool is_cstr;       /* true for c"..." interpolation (cstr result) */
            const char *codegen_backing_name; /* non-NULL when the buffer size is a
                                                 compile-time constant and a fixed
                                                 backing array was hoisted to function
                                                 entry (reused per loop iteration)
                                                 instead of alloca'd each evaluation */
            int64_t backing_size;             /* byte budget N (excludes the NUL slot);
                                                 the hoisted array is uint8_t[N + 1] */
            bool wrapped;                     /* true when this interp is the direct init of
                                                 alloc(...)/alloca(...), which allows an
                                                 otherwise illegal unbounded (runtime-sized)
                                                 interpolation */
        } interp_string;

        /* EXPR_SOME */
        struct { Expr *value; } some_expr;

        /* EXPR_OK: ok(v), result construction, type inferred from payload */
        struct { Expr *value; } ok_expr;

        /* EXPR_ERR: err(T, code). T is the ok-payload type (the node's type is T!);
         * code is i32 and must be non-zero, since 0 is the ok tag (a compile error
         * when provably zero, a runtime guard otherwise, as for some(null)). */
        struct { Type *target; Expr *code; } err_expr;

        /* EXPR_ERROR_NAME: error_name(e), a str? holding the fully-qualified name of a
         * declared error code (some("file_io.not_found")), none for reserved-range and
         * negative codes. Backed by a static name table emitted only when used (or
         * unconditionally under --backtraces). */
        struct { Expr *code; } error_name_expr;

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
            SrcLoc let_name_loc;        /* source loc of the binding name (editor go-to-def) */
        } let_expr;

        /* EXPR_LET_DESTRUCT */
        struct {
            Pattern *pattern;       /* PAT_STRUCT (named) or PAT_TUPLE (positional) */
            bool is_mut;
            Expr *init;
            Type *init_type;        /* filled by pass2 */
            const char *tmp_name;   /* codegen temp name for the RHS */
        } let_destruct;

        /* EXPR_TYPE_VAR_REF: 'a in expression position (for 'a.min etc.),
         * or a const generic param 'n used as a value (is_const_param, typed
         * i32; codegen emits the bound value as a literal). */
        struct { const char *name; bool is_const_param; } type_var_ref;

        /* EXPR_ASSERT */
        struct {
            Expr *condition;        /* must be bool */
            Expr *message;          /* optional str, NULL if single-arg */
            const char *expr_text;  /* source text of condition */
            int expr_text_len;
        } assert_expr;

        /* EXPR_STATIC_ASSERT: static_assert(const_expr, "msg"), checked at
         * compile time (immediately when concrete; per instantiation when the
         * condition uses const generic params), emits no code. */
        struct {
            Expr *condition;        /* restricted const-expr grammar */
            const char *msg;        /* string literal (guardrail: no computation) */
        } static_assert_expr;

        /* EXPR_DEFER */
        struct { Expr *value; } defer_expr;

        /* EXPR_IGNORE */
        struct { Expr *value; } ignore_expr;

        /* EXPR_ATOMIC_LOAD: atomic_load_acquire(p) */
        struct { Expr *ptr; } atomic_load;

        /* EXPR_ATOMIC_STORE: atomic_store_release(p, v) */
        struct { Expr *ptr; Expr *value; } atomic_store;
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
    PAT_OK,
    PAT_ERR,
    PAT_CONST_PATH, /* group.member / mod.group.member: a declared error constant.
                       Resolved in pass2 and rewritten in place to PAT_INT_LIT with the
                       assigned code, so exhaustiveness/duplicate analysis and codegen
                       see a plain integer literal. */
    PAT_VARIANT,
    PAT_STRUCT,
    PAT_TUPLE, /* { a, b, ... }: positional tuple destructuring */
    PAT_OR,    /* p1 | p2 | ...: disjunction; alternatives must be binding-free */
    PAT_ERROR, /* parse-error placeholder; treated like PAT_WILDCARD (matches anything, binds
                  nothing) so a malformed arm produces no spurious exhaustiveness cascade. */
} PatternKind;

struct Pattern {
    PatternKind kind;
    SrcLoc loc;
    union {
        /* PAT_BINDING. `name` stays the source spelling (diagnostics, LSP,
         * variant/enum rewrite); `codegen_name` is the unique C name pass2
         * mints so a binding can never collide with a codegen temporary, a
         * libc symbol, or a C keyword. */
        struct { const char *name; const char *codegen_name; } binding;
        struct { uint64_t value; Type *lit_type; bool out_of_range; bool negative; } int_lit;
        struct { uint8_t value; } char_lit;
        struct { bool value; } bool_lit;
        struct { const char *value; int length; } string_lit;
        struct { const char **parts; int part_count; } const_path; /* PAT_CONST_PATH */
        struct { Pattern *inner; } some_pat;   /* PAT_SOME, PAT_OK (inner over T), PAT_ERR (inner over i32) */
        struct {
            const char *variant;
            Pattern *payload;   /* NULL if no payload */
        } variant;
        struct {
            FieldPattern *fields;
            int field_count;
        } struc;
        struct {
            Pattern **patterns;     /* positional element patterns */
            int pattern_count;
            Type **resolved_types;  /* filled by pass2: element type for each position */
        } tuple_pat;
        struct { Pattern **alts; int alt_count; } or_pat;
    };
};

struct MatchArm {
    Pattern *pattern;
    Expr *guard;        /* optional `when` boolean guard; NULL if absent */
    Expr **body;
    int body_count;
    SrcLoc loc;
};

/* ---- Traversal ---- */

/* Call fn(child, ctx) for each direct subexpression of e, in source order,
 * skipping absent (NULL) children. Match arm guards, interpolation segments
 * and a slice literal's size are children too; types are not. Every kind is
 * listed explicitly, so adding an ExprKind is a -Wswitch warning in ast.c
 * instead of a subtree that some walker silently skips. A walker handles the
 * kinds it treats specially and passes every other node here. */
typedef void (*ExprVisitFn)(Expr *child, void *ctx);
void expr_for_each_child(Expr *e, ExprVisitFn fn, void *ctx);

/* The same children, for a search: true as soon as pred(child, ctx) is true
 * for one of them, false if it holds for none. */
typedef bool (*ExprPredFn)(Expr *child, void *ctx);
bool expr_any_child(Expr *e, ExprPredFn pred, void *ctx);

/* The same for the direct subpatterns of p. */
typedef void (*PatternVisitFn)(Pattern *child, void *ctx);
void pattern_for_each_child(Pattern *p, PatternVisitFn fn, void *ctx);

/* ---- Declaration nodes ---- */

/* Extern error protocols: the `from <protocol>` tail on an extern function
 * returning T!. A closed set, one entry per well-defined C failure convention:
 * the protocol names the failure test and where the error code lives, and
 * codegen wraps the raw C return into the declared result at the call site.
 * Codes pass through unchanged, with no arithmetic (spec: "Extern error
 * protocols"). */
typedef enum {
    EXT_PROTO_NONE = 0,     /* no protocol declared */
    EXT_PROTO_ERROR,        /* malformed protocol clause; the parser already
                               reported it, so pass1 skips agreement checks */
    EXT_PROTO_ERRNO_NEG1,   /* errno(-1):        ret == -1   -> err(errno) */
    EXT_PROTO_ERRNO_NULL,   /* errno(null):      ret == NULL -> err(errno) */
    EXT_PROTO_STATUS,       /* status:           ret != 0    -> err(ret); void payload */
    EXT_PROTO_NEG_ERRNO,    /* neg_errno:        ret < 0     -> err(ret), raw */
    EXT_PROTO_HRESULT,      /* hresult:          ret < 0     -> err(ret), raw */
    EXT_PROTO_LASTERR_0,    /* last_error(0):    ret == 0    -> err(GetLastError()) */
    EXT_PROTO_LASTERR_NULL, /* last_error(null): ret == NULL -> err(GetLastError()) */
    EXT_PROTO_LASTERR_NEG1, /* last_error(-1):   ret == -1   -> err(GetLastError()) */
    EXT_PROTO_WSA_NEG1,     /* wsa_error(-1):    ret == -1   -> err(WSAGetLastError()) */
} ExternProtocol;

typedef enum {
    DECL_LET,
    DECL_STRUCT,
    DECL_UNION,
    DECL_ENUM,
    DECL_MODULE,
    DECL_IMPORT,
    DECL_EXTERN,
    DECL_NAMESPACE,
    DECL_ERROR,    /* parse-error placeholder for a malformed top-level/module declaration;
                      skipped by pass1 collect and the pass2 top loop, never reaches codegen. */
} DeclKind;

typedef struct Decl Decl;

/* One module segment written after the head of an import's `from` route
 * (`from a.b.c` has segments `b` and `c`). `sym` is what the segment resolved
 * to, stamped by pass1 where the import is processed, so editor queries read
 * the resolution rather than repeating it; NULL if the route did not resolve. */
typedef struct {
    const char *name;
    SrcLoc loc;
    struct Symbol *sym;
} ImportRouteSeg;

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
            bool is_module_member;      /* true if declared inside a module body */
            /* Read-only module constant (is_module_member && !is_mut && non-function)
             * whose storage is all emitted by the compiler: no pointer value and no
             * slice built over a raw address anywhere in its initializer. Only then
             * are its contents frozen: reference-typed reads out of it carry const and
             * its lifted backing arrays emit `static const`. Set in pass2's
             * check_decl_let. When false, only the constant's own storage is
             * read-only, which keeps `let vga = (u8*) 0xA0000usize` writable
             * through. */
            bool is_frozen;
            Expr *init;
            /* An immutable non-function `let` bound to a static function
             * (fn_static_target), such as `let alias = inc`: the expression
             * naming that function (set by pass2). */
            struct Expr *static_fn;
            /* A generic function: its initializer is a function with a type
             * variable among its explicit ones or in a parameter type. Set by
             * pass1 (detect_generic_func), with the symbol's is_generic. */
            bool is_generic;
            Type *resolved_type;    /* filled by pass2 */
            /* Const-fold cache for module-member lets, filled lazily by the
             * const-expr gate in pass2. Zero-init is the unvisited state. */
            int const_fold_state;       /* CONST_FOLD_* in pass2.c: 0=unvisited,
                                           1=visiting, 2=done, 3=failed */
            Expr *const_fold_value;     /* folded literal tree (may be == init) */
            /* The initializer as written, kept when folding replaces `init`.
             * Checks about what the source refers to (module cycles) read it. */
            Expr *written_init;
            /* static_assert statements over const params in this (generic)
             * function's body, collected by pass2 for the per-instantiation
             * check in mono_register. */
            StaticAssert *static_asserts;   /* arena-backed */
            int static_assert_count;
        } let;

        /* DECL_STRUCT */
        struct {
            const char *name;
            const char *c_name;     /* C struct/union tag name for extern types, NULL for normal */
            bool is_extern;         /* true for extern struct/union declarations */
            bool is_c_union;        /* true for extern union (untagged C union layout) */
            StructField *fields;
            int field_count;
            const char **type_params;   /* type var names, e.g. ["'a", "'b"] */
            int type_param_count;
            uint8_t *param_kinds;       /* GenParamKind per param; NULL = all GP_TYPE */
            bool is_generic;
            StaticAssert *static_asserts;   /* instantiation predicates; checked in mono_register */
            int static_assert_count;
        } struc;

        /* DECL_UNION */
        struct {
            const char *name;
            UnionVariant *variants;
            int variant_count;
            const char **type_params;
            int type_param_count;
            uint8_t *param_kinds;       /* GenParamKind per param; NULL = all GP_TYPE */
            bool is_generic;
            StaticAssert *static_asserts;   /* instantiation predicates; checked in mono_register */
            int static_assert_count;
        } unio;

        /* DECL_ENUM: closed set of named integer constants over a declared repr.
         * variants is the same array pass1 wires into the TYPE_ENUM, so values
         * resolved there are visible everywhere. */
        struct {
            const char *name;
            Type *repr;             /* declared `of` repr (i8..u64); NULL = i32 default */
            EnumVariant *variants;
            int variant_count;
            bool values_resolved;   /* pass1 guard: value normalization runs once */
        } enu;

        /* DECL_MODULE */
        struct {
            const char *name;
            const char *ns_prefix;  /* namespace prefix (mangled, e.g. "acme__graphics"), NULL = global */
            const char *from_lib;   /* NULL unless module X from "lib" */
            const char *define_macro; /* NULL unless define "MACRO" "VALUE" */
            const char *define_value; /* NULL unless define present */
            Decl **decls;
            int decl_count;
            bool is_error_group;    /* true when this module was desugared from an `error`
                                       declaration: decls are synthesized i32-const lets whose
                                       init values pass1 assigns deterministically from 65536 */
        } module;

        /* DECL_IMPORT */
        struct {
            const char *name;
            const char *alias;
            const char *from_module;
            const char *from_namespace;
            bool is_wildcard;
            /* Token locations of the identifiers written in the statement, for
             * editor queries. `.line == 0` where the token is absent (a wildcard
             * has no name, an unaliased import no alias, `from ns::` no module). */
            SrcLoc name_loc;
            SrcLoc alias_loc;
            SrcLoc module_loc;
            /* What the statement resolved to, stamped by pass1 where the import
             * is processed (the single-resolution invariant: consumers read these
             * rather than re-resolving). `resolved_sym` is what `name` denotes
             * (and `alias`, which is only another spelling of it);
             * `resolved_companion` the module imported alongside a type of the
             * same name; `resolved_module` the module named in the `from` clause.
             * All NULL when the import did not resolve. `resolved_module` is the
             * head of the `from` route; the module the import reads from is the
             * route's last segment, i.e.
             * `route_count ? route[route_count-1].sym : resolved_module`. */
            struct Symbol *resolved_sym;
            struct Symbol *resolved_companion;
            struct Symbol *resolved_module;
            /* The `from` clause names a route of one or more modules. The head,
             * resolved in the enclosing scope, is `from_module`, and `route`
             * holds the segments written after it (`from a.b.c` has head "a"
             * and route {"b","c"}), each a module member of its predecessor, as
             * `.` navigates in expression position. Empty for the one-segment
             * form and for a bare `from ns::`. */
            ImportRouteSeg *route;
            int route_count;
        } import;

        /* DECL_EXTERN */
        struct {
            const char *name;
            const char *alias;
            Type *type;
            ExternProtocol protocol;  /* error protocol (`from <protocol>`);
                                         EXT_PROTO_NONE when absent */
        } ext;

        /* DECL_NAMESPACE */
        struct { const char *name; } ns;
    };
};

/* ---- Program ---- */

typedef struct Program {
    Decl **decls;
    int decl_count;
    /* The declarations the parser's pre-pass read as generic
     * (parser_collect_generic_names); pass1 checks it saw every generic one. */
    const char **generic_names;
    int generic_name_count;
} Program;

/* One Program holding every file's declarations in order. A file that does
 * not open with a namespace declaration is preceded by a namespace reset
 * (DECL_NAMESPACE with a NULL name), so a namespace never carries over from
 * one file to the next. A single file is returned as it is. */
Program *program_merge(Arena *a, Program **files, int count);

/* Whether `d` declares a function: a `let` (not `let mut`) bound to a lambda
 * literal. A `let mut` bound to one is a variable holding a function value,
 * like any `let mut`: it may be reassigned, and a call goes through its
 * current value. */
bool decl_is_function(const Decl *d);

/* Whether `d` declares a variable: a `let` or `let mut` that is not a
 * function declaration (decl_is_function). A name bound to one holds a value;
 * a function or extern it names is code. */
bool decl_is_variable(const Decl *d);

