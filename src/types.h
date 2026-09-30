#pragma once
#include "common.h"
#include "diag.h"   /* SrcLoc (StructField/UnionVariant carry a source loc) */
#include "token.h"  /* TokenKind: const_binary_op takes the operator */

typedef enum {
    TYPE_INT8,
    TYPE_INT16,
    TYPE_INT32,
    TYPE_INT64,
    TYPE_UINT8,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_UINT64,
    TYPE_ISIZE,
    TYPE_USIZE,
    TYPE_FLOAT32,
    TYPE_FLOAT64,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_POINTER,
    TYPE_SLICE,
    TYPE_OPTION,
    TYPE_RESULT,
    TYPE_FUNC,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_ENUM,
    TYPE_ANY_PTR,
    TYPE_TYPE_VAR,
    TYPE_FIXED_ARRAY, /* fixed-size inline array: T[N] */
    TYPE_CONST_INT,  /* a resolved compile-time integer used as a generic argument (wide<256>) */
    TYPE_CONST_EXPR, /* a symbolic const-generic expression over const params (wide<'n * 2>);
                        folds to TYPE_CONST_INT at substitution time */
    TYPE_STUB,       /* unresolved type reference: name only, resolved by pass1/pass2 */
    TYPE_ERROR,      /* poison type for error recovery */
    TYPE_NEVER,      /* bottom type: return/break/continue; absorbed by any branch sibling */
    TYPE_UNRESOLVED, /* a recursive function's not-yet-inferred return type. Transient:
                        exists only while a recursive body is being checked, then patched
                        to the concrete inferred type (or `never` if no base case exists).
                        Absorbed by any branch sibling, like TYPE_NEVER. */

    TYPE_COUNT       /* the number of kinds, not a kind */
} TypeKind;

/* The case labels of the scalar primitive kinds, for a switch that lists every
 * TypeKind: `CASE_TYPE_PRIMITIVES: return ...;` */
#define CASE_TYPE_PRIMITIVES                                              \
    case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:   \
    case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64: \
    case TYPE_ISIZE: case TYPE_USIZE: case TYPE_FLOAT32: case TYPE_FLOAT64: \
    case TYPE_BOOL: case TYPE_VOID

typedef struct Type Type;
typedef struct StructField StructField;
typedef struct UnionVariant UnionVariant;
typedef struct EnumVariant EnumVariant;

/* Kind of a generic parameter: a type variable ('a used in a type position) or
 * a const (value) parameter ('n used in a size/value position). GP_UNKNOWN is a
 * transient state during pass1/pass2 kind inference; unconstrained params
 * finalize to GP_TYPE. Stored as uint8_t arrays parallel to type_params. */
typedef enum { GP_TYPE = 0, GP_CONST = 1, GP_UNKNOWN = 2 } GenParamKind;

struct StructField {
    const char *name;
    Type *type;
    SrcLoc loc;     /* source loc of the field name (editor go-to-definition); {0} if synthesized */
};

struct UnionVariant {
    const char *name;
    Type *payload;  /* NULL if no payload */
    SrcLoc loc;     /* source loc of the variant name (editor go-to-definition); {0} if synthesized */
};

struct EnumVariant {
    const char *name;
    uint64_t value_bits;  /* variant value as two's-complement bits truncated to the repr width
                             (canonical form: duplicate detection and codegen both read this) */
    bool negative;        /* explicit negative literal (parser); pass1 range-checks against repr */
    bool has_explicit;    /* declared `= N`; pass1 auto-numbers the rest C-style (prev+1) */
    SrcLoc loc;           /* source loc of the variant name (editor go-to-definition/doc) */
};

struct Type {
    TypeKind kind;
    const char *alias;  /* display name override (e.g. "str" for u8[], "cstr" for u8*) */
    bool is_const;      /* const qualifier for pointer/slice types */
    union {
        struct { Type *pointee; } pointer;
        struct { Type *elem; } slice;
        struct { Type *inner; } option;
        struct { Type *inner; } result;   /* T!: ok(T) | err(i32); repr { int32_t err; T value; } */
        struct {
            Type **param_types;
            int param_count;
            Type *return_type;
            bool is_variadic;
            const char **type_params;   /* explicit type var names (e.g. "'a", "'b"), NULL if none */
            int type_param_count;
        } func;
        struct {
            const char *name;
            const char *qualified_name;  /* fully qualified FC path (e.g. "std::data.array_list") */
            const char *c_name;         /* C struct/union tag name for extern types, NULL for normal */
            bool is_c_union;            /* true for extern union (untagged C union layout) */
            bool is_tuple;              /* true for synthesized tuple structs (fields e0..eN-1, display "{...}") */
            StructField *fields;
            int field_count;
            Type **type_args;
            int type_arg_count;
            struct Symbol *resolved_sym; /* template type symbol, set by pass1/pass2; used by mono to avoid symtab re-lookup */
        } struc;
        struct {
            const char *name;
            const char *qualified_name;  /* fully qualified FC path (e.g. "geometry.shape") */
            UnionVariant *variants;
            int variant_count;
            Type **type_args;
            int type_arg_count;
            struct Symbol *resolved_sym; /* template type symbol, set by pass1/pass2; used by mono to avoid symtab re-lookup */
        } unio;
        struct {
            const char *name;            /* mangled C name (fc__difficulty / fc__m__door_lock) */
            const char *qualified_name;  /* fully qualified FC path for diagnostics */
            Type *repr;                  /* underlying type: i8..u64 singleton; i32 default */
            EnumVariant *variants;
            int variant_count;
            struct Symbol *resolved_sym; /* set by pass1/pass2 */
        } enu;
        struct {
            Type *elem;
            int64_t size;    /* concrete element count; valid when size_ref == NULL */
            Type *size_ref;  /* symbolic size (TYPE_TYPE_VAR const param or TYPE_CONST_EXPR)
                                inside a generic template; NULL once concrete */
        } fixed_array;
        struct { const char *name; } type_var;
        struct { int64_t value; } const_int;              /* TYPE_CONST_INT */
        struct { struct Expr *expr; } const_expr;         /* TYPE_CONST_EXPR */
        /* TYPE_STUB: unresolved type reference created by the parser.
         * Resolved to the struct, union or enum type it names by pass1/pass2. */
        struct {
            const char *name;
            const char *qualified_name;
            Type **type_args;
            int type_arg_count;
            /* Set when monomorphization renames a concrete generic stub in
             * place to its instance name (box<i32> -> box__3_i32, keeping the
             * arguments): the template's base name, so the instance name can
             * be derived again without mangling twice. NULL otherwise. */
            const char *base_name;
        } stub;
    };
};

/* Get singleton types for primitives */
Type *type_int8(void);
Type *type_int16(void);
Type *type_int32(void);
Type *type_int64(void);
Type *type_uint8(void);
Type *type_uint16(void);
Type *type_uint32(void);
Type *type_uint64(void);
Type *type_isize(void);
Type *type_usize(void);
Type *type_float32(void);
Type *type_float64(void);
Type *type_bool(void);
Type *type_void(void);
Type *type_str(void);
Type *type_cstr(void);
Type *type_const_str(void);
Type *type_const_cstr(void);
Type *type_char(void);
Type *type_error_code(void);
Type *type_any_ptr(void);
Type *type_error(void);
Type *type_never(void);
Type *type_unresolved(void);
bool type_is_error(Type *t);
bool type_is_never(Type *t);
bool type_is_unresolved(Type *t);
bool type_is_const(Type *t);

/* Type construction */
Type *type_pointer(Arena *a, Type *pointee);
Type *type_slice(Arena *a, Type *elem);
Type *type_option(Arena *a, Type *inner);
Type *type_result(Arena *a, Type *inner);
Type *type_fixed_array(Arena *a, Type *elem, int64_t size);

/* Alias type helpers: str = uint8[], cstr = uint8* */
bool is_str_type(Type *t);
bool is_cstr_type(Type *t);

/* Copy and const helpers */
Type *type_copy(Arena *a, Type *t);
/* Recursive copy: fresh nodes for every constructor + fresh field/variant/param
 * arrays (leaves and type_args shared). Use when a copy will be mutated in place
 * by mono name canonicalization and must not alias a live/template subtree. */
Type *type_deep_copy(Arena *a, Type *t);
Type *type_make_const(Arena *a, Type *t);
Type *type_strip_const(Arena *a, Type *t);
/* `t` as read out of read-only storage: a reference (pointer, slice or any*)
 * is const-qualified, also inside an option or result. Other types are
 * returned unchanged; a struct, tuple or union has no read-only form. */
Type *type_read_only(Arena *a, Type *t);
/* The type of an element loaded from `slice`: read-only (type_read_only)
 * when the slice is. */
Type *type_slice_elem_read(Arena *a, Type *slice);

/* Queries */
bool type_is_integer(Type *t);
bool type_is_signed(Type *t);
bool type_is_unsigned(Type *t);
bool type_is_float(Type *t);
bool type_is_numeric(Type *t);
bool type_eq(Type *a, Type *b);
bool type_eq_ignore_const(Type *a, Type *b);
const char *type_name(Type *t);

/* The C spelling of built-in property `prop` of numeric type `t` (`i32.max` is
 * INT32_MAX, `f64.nan` is ((double)NAN), `u8.bits` is 8), or NULL when `t` has
 * no such property. */
const char *type_property_c(Type *t, const char *prop);

/* The built-in members of a value: a slice's `len` and `ptr`, an option's
 * `is_some` and `is_none`, a result's `is_ok` and `is_err`. None is
 * assignable. type_builtin_members lists `t`'s (NULL when it has none);
 * type_builtin_member_type types one (NULL when `t` has no such member). */
const char *const *type_builtin_members(Type *t, int *count);
Type *type_builtin_member_type(Arena *a, Type *t, const char *name);

/* The width in bits of a fixed-width integer type (i8..u64), or 0 for any
 * other type, including isize/usize, whose width is the target's. */
int type_fixed_int_bits(Type *t);

/* The static properties of the numeric types (i32.min, f64.nan, 'a.bits), by
 * index, for listing them. */
int type_property_count(void);
const char *type_property_name(int i);

/* The type of property `prop` of `t`: i32 for `bits`, `t` itself for the
 * others. NULL when `t` has no such property. For a type variable, whether any
 * numeric type has it (each instance is checked again with its own type). */
Type *type_property_type(Type *t, const char *prop);

/* The C header a type property's spelling needs beyond <stdint.h>: float.h for
 * a float's min, max and epsilon, math.h for its nan, inf and neg_inf. */
typedef enum { PROP_HEADER_NONE, PROP_HEADER_FLOAT, PROP_HEADER_MATH } PropHeader;
PropHeader type_property_header(Type *t, const char *prop);

/* An instantiation spelled the way the user writes it, "uwide<100>", for a
 * diagnostic. `name` may be a mangled C name: everything through its last
 * "__" is dropped (a user name never contains "__"). Caller frees. */
char *type_inst_display(const char *name, Type **args, int count);

/* The structural nesting depth of a type argument: the axis along which a
 * divergent instantiation grows. It recurses through wrapper constructors and
 * generic type arguments but never into struct/union fields, which the
 * definition bounds; that also keeps it finite on by-value-recursive types. */
int type_arg_depth(Type *t);

/* Implicit widening: can 'from' widen to 'to' without explicit cast? */
bool type_can_widen(Type *from, Type *to);
/* The widenings that change no bits: identical types, added const, and a
 * typed pointer to any*. */
bool type_widen_repr_preserving(Type *from, Type *to);

/* Find the common (wider) numeric type for two types, or NULL if no widening possible */
Type *type_common_numeric(Type *a, Type *b);

/* The underlying integer type of an enum (its declared repr); identity for all other types. */
Type *type_enum_underlying(Type *t);

/* Map a type suffix string (e.g., "i8", "u64") to a type, or NULL */
Type *type_from_int_suffix(const char *suffix, int len);

/* Map a type name string (e.g., "int32", "bool", "str") to a type, or NULL */
Type *type_from_name(const char *s, int len);
/* The built-in type names type_from_name recognizes, for tools that list
 * them (editor completion). */
int type_primitive_count(void);
const char *type_primitive_name(int i);

/* Type variable constructor */
Type *type_type_var(Arena *a, const char *name);

/* Const-generic constructors */
Type *type_const_int(Arena *a, int64_t value);
Type *type_const_expr(Arena *a, struct Expr *expr);
Type *type_fixed_array_sym(Arena *a, Type *elem, Type *size_ref);

/* True for TYPE_CONST_INT / TYPE_CONST_EXPR (a value argument, not a type). */
bool type_is_const_arg(Type *t);

/* Evaluate a fixed array's size: concrete `size`, or a fully-substituted
 * size_ref (TYPE_CONST_INT). Returns false if still symbolic. */
bool type_fixed_array_size(Type *t, int64_t *out);

/* Mask a 64-bit value to `width` bits, sign-extending back to 64 if signed. */
uint64_t const_mask_extend(uint64_t v, int width, bool is_signed);

typedef enum { CONST_OP_OK, CONST_OP_DIV_ZERO, CONST_OP_UNSUPPORTED } ConstOpStatus;

/* `l op r` on constant operands, with the semantics the generated code has at
 * run time. Arithmetic and bitwise operators work on `width`-bit operands of
 * the given signedness: the result wraps, a shift count is masked to
 * width - 1, a signed right shift is arithmetic, and a signed x / -1 wraps
 * (x % -1 is 0). A comparison compares the operands, as given, signed or
 * unsigned; comparisons, && and || yield 0 or 1. Values are 64-bit patterns
 * (a narrower value sign- or zero-extended). Every const evaluator in the
 * compiler goes through this, so they agree with each other and with the C. */
ConstOpStatus const_binary_op(TokenKind op, uint64_t l, uint64_t r, int width,
                              bool is_signed, uint64_t *out);

/* Whether const_binary_op's `op` yields a truth value (a comparison, && or
 * ||), evaluated at its operands' type rather than at its own. */
bool const_op_yields_bool(TokenKind op);

/* Evaluate a const-arg carrier (TYPE_CONST_INT / TYPE_CONST_EXPR / a const
 * param TYPE_TYPE_VAR) under name-to-Type bindings where const params bind to
 * TYPE_CONST_INT. Context-free evaluation in the i64 domain of const arguments
 * and sizes (const_binary_op at 64 bits, signed; div-by-zero = error). Returns
 * false when still symbolic (no error) or on a hard failure (error stashed; see
 * const_eval_take_error). */
bool const_type_eval(Type *t, const char **var_names, Type **concrete,
                     int count, int64_t *out);

/* Evaluate `e`, a const expression that was type-checked as an ordinary
 * expression (a static_assert condition in a function body), under the same
 * bindings, with every node at its own type: the result is what the same
 * expression computes at run time, as it is for a concrete condition. A const
 * parameter is read as the i32 its expression type says, so its value must
 * fit. Same failure contract as const_type_eval. */
bool const_eval_typed(struct Expr *e, const char **var_names, Type **concrete,
                      int count, int64_t *out);

/* Take (and clear) the last const-generic evaluation error, or NULL if none.
 * The caller owning a diagnostic site reports it with the returned loc. */
const char *const_eval_take_error(SrcLoc *loc);

/* Whether an error is waiting in that slot. The slot keeps the first failure,
 * so a site that substitutes only for a message drains the slot afterwards
 * exactly when it was empty before, leaving another site's error in place. */
bool const_eval_error_pending(void);

/* Does this type need a generated eq function (as opposed to C native ==)? */
bool type_needs_eq_func(Type *t);

/* Check if a type contains any type variables (recursively) */
bool type_contains_type_var(Type *t);

/* Collect unique type variable names from a type in order of first appearance */
void type_collect_vars(Type *t, const char ***vars, int *count, int *cap);

/* Visit each type-variable occurrence in `t`, in order, with the kind its
 * position gives it: GP_TYPE in a type position, GP_CONST in an array size or
 * const expression. In the i-th type-argument slot of a named type (`named`,
 * a struct, union or stub) the kind is whatever the referenced declaration's
 * parameter is: `arg_kind` supplies it, or GP_UNKNOWN when it is NULL. */
typedef void (*TypeVarVisitFn)(const char *name, uint8_t kind, void *ctx);
typedef uint8_t (*TypeArgKindFn)(Type *named, int i, void *ctx);
void type_walk_vars(Type *t, TypeVarVisitFn visit, TypeArgKindFn arg_kind, void *ctx);

/* Like type_collect_vars, but also records each variable's inferred kind
 * (GP_TYPE for type positions, GP_CONST for size/value positions, GP_UNKNOWN
 * for positions whose kind depends on another symbol's params, e.g. a stub's
 * type-arg slot). A var seen in conflicting kinds sets *conflict_var to its
 * name (first conflict wins). `kinds` is a malloc'd/realloc'd array parallel
 * to `vars`, grown with the same cap. */
void type_collect_vars_kinds(Type *t, const char ***vars, uint8_t **kinds,
                             int *count, int *cap, const char **conflict_var);

/* Substitute type variables: replace TYPE_TYPE_VAR with concrete types */
Type *type_substitute(Arena *a, Type *t, const char **var_names, Type **concrete, int count);

/* Mangle a type name for use in C identifiers.
 * Returns a malloc'd string that the caller must free. */
char *mangle_type_name(Type *t);

/* The interned C name of a generic instantiation: `base` "__" and each
 * argument's mangling, length-prefixed (box<i32> from base "fc__box" is
 * "fc__box__3_i32"). */
const char *mangle_generic_name(InternTable *intern,
                                const char *base, Type **type_args, int count);

/* The canonical interned name of a tuple struct, from its element types
 * ({i32, str} is "fc_tuple2__3_i323_str"). Structurally identical tuples share
 * the name, so type_eq (which compares struc.name) is structural for them. */
const char *tuple_canonical_name(InternTable *intern, StructField *fields, int n);

/* Construct a synthesized tuple struct type from element types (fields e0..eN-1).
 * Sets is_tuple=true; name/qualified_name are left for the canonicalizer/resolve_type. */
Type *type_tuple(Arena *a, Type **elems, int n);
