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
    EXPR_VOID_LIT,      /* void() — a void-typed expression */
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
    EXPR_TUPLE_LIT,     /* { expr, expr, ... } — positional anonymous tuple */
    EXPR_ARRAY_LIT,
    EXPR_SLICE_LIT,     /* T[] { ptr = expr, len = expr } */
    EXPR_ALLOC,
    EXPR_FREE,
    EXPR_SIZEOF,
    EXPR_ALIGNOF,
    EXPR_BITCAST,       /* bitcast(T, x) — reinterpret x's bits as scalar type T */
    EXPR_ENUM_OF,       /* enum_of(E, x) — checked integer→enum conversion, yields E? */
    EXPR_DEFAULT,
    EXPR_INTERP_STRING,
    EXPR_ASSIGN,
    EXPR_SOME,
    EXPR_OK,            /* ok(v) — result construction, infers from payload */
    EXPR_ERR,           /* err(T, code) — result construction, type-anchored like none(T) */
    EXPR_ERROR_NAME,    /* error_name(e) — str? name of a declared error code */
    EXPR_DEREF_FIELD,   /* x->f */
    EXPR_LET,           /* let binding inside a block */
    EXPR_LET_DESTRUCT,  /* let { field = name, ... } = expr */
    EXPR_TYPE_VAR_REF,  /* 'a in expression position (for 'a.min etc.) */
    EXPR_ASSERT,
    EXPR_DEFER,
    EXPR_IGNORE,       /* ignore expr — evaluate for effect, yield void */
    EXPR_ATOMIC_LOAD,   /* atomic_load_acquire(p) */
    EXPR_ATOMIC_STORE,  /* atomic_store_release(p, v) */
    EXPR_GUARD,         /* guarded/unguarded (precondition guards) OR checked/unchecked (overflow) */
    EXPR_ERROR,         /* parse-error placeholder; carries only kind+loc. Exists only when
                           diag_error_count()>0, so it never reaches codegen (gated). pass2
                           types it as type_error() silently (the diagnostic was already emitted). */
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

typedef struct Capture {
    const char *name;           /* FC source name */
    const char *codegen_name;   /* C codegen name from outer scope */
    Type *type;
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
    union {
        /* EXPR_INT_LIT */
        struct { uint64_t value; Type *lit_type; bool out_of_range; } int_lit;

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
            bool resolved_local_is_param;      /* the block-local is a function parameter — hover shows name: type
                                                  only (a param has no doc comment of its own to scan for) */
        } ident;

        /* EXPR_BINARY */
        struct { TokenKind op; Expr *left; Expr *right; } binary;

        /* EXPR_UNARY_PREFIX */
        struct { TokenKind op; Expr *operand; } unary_prefix;

        /* EXPR_UNARY_POSTFIX — x! (unwrap-or-abort) and x? (propagation).
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
            bool bare_inst;       /* `name<Types>` in value position with no '(' — explicit type
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
            bool is_type_property;      /* true for static type properties (int32.min, float64.nan, ...) */
            struct Symbol *resolved_member; /* for module-member access (mod.member): the resolved
                                               member Symbol, set in pass2; used by editor queries
                                               (go-to-definition). NULL for plain struct-field access. */
        } field;

        /* EXPR_INDEX */
        struct { Expr *object; Expr *index; } index;

        /* EXPR_SLICE */
        struct { Expr *object; Expr *lo; Expr *hi; } slice;

        /* EXPR_CAST */
        struct {
            Type *target;
            Expr *operand;
            int buffer_size;  /* (cstr[N]) bounded str→cstr cast: N > 0; 0 = plain cast.
                                 Copies min(len, N-1) bytes + NUL into a hoisted uint8[N]. */
            const char *codegen_backing_name;  /* hoisted backing array name (set in codegen) */
            bool licensed;    /* true when this is the direct init of alloc(...)/alloca(...) —
                                 licenses an otherwise illegal unbounded (cstr) str→cstr cast,
                                 whose home (heap/dynamic stack) the wrapping alloc/alloca gives. */
        } cast;

        /* EXPR_GUARD — two orthogonal lexical axes sharing one node:
           - guard axis (guarded/unguarded): the value-precondition runtime guards
             (float→int saturation, integer divide/modulo zero check, slice bounds).
           - overflow axis (checked/unchecked): integer-overflow detection on
             `+ - *`, signed `/` at INT_MIN/-1, and lossy integer narrowing casts.
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
            struct Pattern *var_pattern; /* non-NULL: destructure the element (PAT_TUPLE/PAT_STRUCT) */
            const char *elem_tmp;   /* codegen temp holding the element when destructuring */
            const char *index_var;  /* NULL if not i,x form */
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
            bool heap_alloc;               /* set by pass2: alloc(lambda) — context goes to the
                                              heap at the alloc site, no stack backing hoisted */
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

        /* EXPR_TUPLE_LIT — { e0, e1, ... } positional anonymous tuple */
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
        } array_lit;

        /* EXPR_SLICE_LIT — T[] { ptr = expr, len = expr } */
        struct {
            Type *elem_type;    /* element type (e.g., uint8 for str) */
            Expr *ptr_expr;
            Expr *len_expr;
            bool  len_nonneg;   /* pass2 proved len >= 0 → codegen skips the
                                 * negative-length runtime guard */
        } slice_lit;

        /* EXPR_ALLOC */
        struct {
            Type *alloc_type;     /* type to allocate (NULL for init-from-expr form) */
            Expr *size_expr;      /* array size for alloc(T[N])/alloc(T,N) — NULL for single */
            Expr *init_expr;      /* init expression for alloc(expr) — NULL for type-only */
            bool alloc_raw;       /* true for alloc(T, N) → T*?, false for alloc(T[N]) → T[]? */
            bool is_stack;        /* true for alloca(...) → dynamic stack, no option, no free */
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

        /* EXPR_BITCAST — bitcast(T, x): reinterpret x's bytes as scalar type T.
         * target and operand must be equal-size fixed-width scalars (checked in
         * pass2); no runtime failure mode, so no guard/checked variant. */
        struct { Type *target; Expr *operand; } bitcast_expr;

        /* EXPR_ENUM_OF — enum_of(E, x): membership-checked conversion of an
         * integer to enum E; yields E? (some on a declared value, none otherwise). */
        struct { Type *target; Expr *operand; } enum_of_expr;

        /* EXPR_DEFAULT */
        struct { Type *target; } default_expr;

        /* EXPR_INTERP_STRING */
        struct {
            InterpSegment *segments;
            int segment_count;
            bool is_cstr;       /* true for c"..." interpolation → cstr result */
            const char *codegen_backing_name; /* non-NULL when the buffer size is a
                                                 compile-time constant and a fixed
                                                 backing array was hoisted to function
                                                 entry (reused per loop iteration)
                                                 instead of alloca'd each evaluation */
            int64_t backing_size;             /* byte budget N (excludes the NUL slot);
                                                 the hoisted array is uint8_t[N + 1] */
            bool wrapped;                     /* true when this interp is the direct init of
                                                 alloc(...)/alloca(...) — licenses an otherwise
                                                 illegal unbounded (runtime-sized) interpolation */
        } interp_string;

        /* EXPR_SOME */
        struct { Expr *value; } some_expr;

        /* EXPR_OK — ok(v): result construction, type inferred from payload */
        struct { Expr *value; } ok_expr;

        /* EXPR_ERR — err(T, code): T is the ok-payload type (the node's type is T!);
         * code is i32, must be non-zero (0 is the ok tag — compile error when provably
         * zero, runtime guard otherwise, mirroring some(null)). */
        struct { Type *target; Expr *code; } err_expr;

        /* EXPR_ERROR_NAME — error_name(e): str? holding the fully-qualified name of a
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

        /* EXPR_TYPE_VAR_REF — 'a in expression position (for 'a.min etc.) */
        struct { const char *name; } type_var_ref;

        /* EXPR_ASSERT */
        struct {
            Expr *condition;        /* must be bool */
            Expr *message;          /* optional str, NULL if single-arg */
            const char *expr_text;  /* source text of condition */
            int expr_text_len;
        } assert_expr;

        /* EXPR_DEFER */
        struct { Expr *value; } defer_expr;

        /* EXPR_IGNORE */
        struct { Expr *value; } ignore_expr;

        /* EXPR_ATOMIC_LOAD — atomic_load_acquire(p) */
        struct { Expr *ptr; } atomic_load;

        /* EXPR_ATOMIC_STORE — atomic_store_release(p, v) */
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
    PAT_CONST_PATH, /* group.member / mod.group.member — a declared error constant.
                       Resolved in pass2 and rewritten in place to PAT_INT_LIT with the
                       assigned code, so exhaustiveness/duplicate analysis and codegen
                       see a plain integer literal. */
    PAT_VARIANT,
    PAT_STRUCT,
    PAT_TUPLE, /* { a, b, ... } — positional tuple destructuring (let-bindings only) */
    PAT_OR,    /* p1 | p2 | ... — disjunction; alternatives must be binding-free */
    PAT_ERROR, /* parse-error placeholder; treated like PAT_WILDCARD (matches anything, binds
                  nothing) so a malformed arm produces no spurious exhaustiveness cascade. */
} PatternKind;

struct Pattern {
    PatternKind kind;
    SrcLoc loc;
    union {
        struct { const char *name; } binding;
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

/* ---- Declaration nodes ---- */

/* Extern error protocols — the `from <protocol>` tail on an extern function
 * returning T!. A closed set, one entry per crisp C failure convention: the
 * protocol names the failure test and where the error code lives, and codegen
 * wraps the raw C return into the declared result at the call site. Codes pass
 * through raw — no arithmetic, ever (spec/result-type-design.md §C interop). */
typedef enum {
    EXT_PROTO_NONE = 0,     /* no protocol declared */
    EXT_PROTO_ERROR,        /* malformed protocol clause; parse error already
                               reported — pass1 skips agreement checks */
    EXT_PROTO_ERRNO_NEG1,   /* errno(-1):        ret == -1   → err(errno)         */
    EXT_PROTO_ERRNO_NULL,   /* errno(null):      ret == NULL → err(errno)         */
    EXT_PROTO_STATUS,       /* status:           ret != 0    → err(ret); void payload */
    EXT_PROTO_NEG_ERRNO,    /* neg_errno:        ret < 0     → err(ret), raw      */
    EXT_PROTO_HRESULT,      /* hresult:          ret < 0     → err(ret), raw      */
    EXT_PROTO_LASTERR_0,    /* last_error(0):    ret == 0    → err(GetLastError())    */
    EXT_PROTO_LASTERR_NULL, /* last_error(null): ret == NULL → err(GetLastError())    */
    EXT_PROTO_LASTERR_NEG1, /* last_error(-1):   ret == -1   → err(GetLastError())    */
    EXT_PROTO_WSA_NEG1,     /* wsa_error(-1):    ret == -1   → err(WSAGetLastError()) */
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
            Expr *init;
            Type *resolved_type;    /* filled by pass2 */
            /* Const-fold cache for module-member lets. Lazily populated during
             * the const-expr gate in pass2; zero-init (UNVISITED) is correct. */
            int const_fold_state;       /* 0=unvisited, 1=visiting, 2=done, 3=failed */
            Expr *const_fold_value;     /* folded literal tree (may be == init) */
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

        /* DECL_ENUM — closed set of named integer constants over a declared repr.
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
            const char *ns_prefix;  /* namespace prefix (mangled, e.g. "acme_graphics"), NULL = global */
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
} Program;

/* True if an interpolated string's buffer size is not a compile-time constant —
 * i.e. it contains a %s/cstr segment with no explicit precision, making its byte
 * budget depend on a runtime string length. Such interpolations must be given a
 * home explicitly (a precision, alloc, or alloca); a bare one is rejected in
 * pass2. Defined in codegen.c so it shares the exact const-size logic the buffer
 * emitter uses (the two can never disagree on what counts as bounded). */
bool interp_is_runtime_sized(const struct Expr *e);

/* Explicit truncating precision of a `%s` format segment (>= 0), or -1 when the
 * segment is literal, non-%s, or unbounded. A precision hard-caps the segment's
 * bytes (printf semantics), so these segments are governed by the overflow axis
 * (`checked` aborts instead of clipping). Defined in codegen.c beside the format
 * -spec parser so pass2 and the emitter share one notion of "truncating". */
int interp_seg_trunc_prec(const struct InterpSegment *seg);

/* Pointer-value null-status predicates for null-sentinel options (T*?, any*?,
 * cstr?), where none is represented by a null pointer. provably_nonnull is true
 * only when a value can never be null (codegen elides the some() null-guard);
 * provably_null is true only when it is always null (pass2 rejects some(p) of
 * it). Both are false for anything uncertain → a runtime guard. Defined in
 * codegen.c so the guard/elide/reject decisions share one source of truth. */
bool ptr_value_provably_nonnull(const struct Expr *e);
bool ptr_value_provably_null(const struct Expr *e);
bool int_value_provably_nonzero(const struct Expr *e);
bool int_value_provably_zero(const struct Expr *e);

/* If e is a resolved reference to a declared error constant (a member of an
 * `error` group, reached as `group.member`/`mod.group.member` or through an
 * import as a bare name), return the member's assigned EXPR_INT_LIT; else
 * NULL. Defined in codegen.c beside the provably-nonzero predicates so the
 * err(T,0) guard-elision and const-folding decisions share one source of
 * truth. */
const struct Expr *error_const_literal(const struct Expr *e);
