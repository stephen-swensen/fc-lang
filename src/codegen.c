#include "codegen.h"
#include "monomorph.h"
#include "pass1.h"
#include "diag.h"
#include "facts.h"
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

/* Substitution context for monomorphized emission */
typedef struct {
    const char **var_names;
    Type **concrete;
    int count;
} SubstCtx;
static SubstCtx *g_subst = NULL;

/* Declared early: read by the result-unwrap emitter (named aborts) well before
 * the feature-detection block that owns the rest of the g_* flags. */
static bool g_backtraces;
static bool g_uses_error_name;   /* any error_name(...) in emitted code */
static bool g_errname_emitted;   /* the fc_errname table/lookup were emitted */
/* Suffix appended to every lifted-lambda name while emitting a monomorphized
 * generic instance.  Lambdas inside generic bodies are emitted once per
 * instantiation (a capture/param/return may be typed by the enclosing type
 * variable), so the lifted function and its _ctx_ struct are mangled with the
 * instance's name.  NULL outside generic-instance emission. */
static const char *g_lambda_suffix = NULL;

/* Emit in the context of generic instance `inst`: its type arguments substitute
 * for the template's type variables, and its lambdas take its name as a
 * suffix. `subst` is caller storage that must live until leave_instance. */
static void enter_instance(MonoInstance *inst, SubstCtx *subst) {
    *subst = (SubstCtx){ inst->type_param_names, inst->type_args, inst->type_param_count };
    g_subst = subst;
    g_lambda_suffix = inst->mangled_name;
}

static void leave_instance(void) {
    g_subst = NULL;
    g_lambda_suffix = NULL;
}
static MonoTable *g_mono = NULL;
static Arena *g_arena = NULL;
static InternTable *g_intern = NULL;
static SymbolTable *g_symtab = NULL;

/* The C name of a function-local binding. pass2 mints a unique `_l_<name>_<id>`
 * for every binding form (see local_c_name there), which keeps source names out
 * of the file-scope namespaces this file emits into and subsumes the C-keyword
 * escape. The fallbacks cover nodes pass2 never reached, which happens only
 * after an error, and errors keep codegen from running. */
static const char *param_c_name(const Param *p) {
    return p->codegen_name ? p->codegen_name : c_safe_ident(g_intern, p->name);
}

static const char *pat_binding_c_name(const Pattern *p) {
    return p->binding.codegen_name ? p->binding.codegen_name : p->binding.name;
}

/* Extend a C expression with a member/element access, for the recursive
 * emitters that descend into a value (`emit_pat_predicate`, `emit_pat_bindings`,
 * `emit_value_eq`).
 *
 * Sized to the result: FC identifiers are unbounded and each level of descent
 * appends a suffix. A path truncated inside a member name can land on a shorter
 * member of the same struct or union (`.u.abq` -> `.u.ab`), which compiles
 * clean under -Wall -Werror and reads the wrong bytes. */
static const char *path_cat(const char *base, const char *sep, const char *tail) {
    return arena_sprintf(g_arena, "%s%s%s", base, sep, tail);
}

/* The C member holding a union's variant payloads. The inner union is named
 * because C lets an anonymous one's members share the enclosing struct's
 * namespace, where a variant literally named `tag` would duplicate the injected
 * discriminant. The outer struct declares only `tag` and `u`, neither derived
 * from source, so no variant name can reach that namespace. */
#define FC_PAYLOAD_MEMBER "u"

/* Names derived from a union's C name: the tag enum's typedef (`fc_tag_<U>`)
 * and its enumerators (`fc_tv_<V>__<U>`).
 *
 * Both live outside the `fc__` namespace that every user-declared name is
 * rooted at (see mangle_root in pass1.c), so no FC declaration can spell them.
 * A suffixed spelling such as `<U>_tag` would sit inside `fc__<name>` space,
 * where a user type named `shape_tag` collides with union `shape`'s enum.
 *
 * The two kinds take different prefixes because a single `_`-joined space is
 * ambiguous both ways: union `a` variant `b_c` would spell like union `a_b`
 * variant `c`, and union `shape` variant `circle` like union `shape_circle`'s
 * typedef.
 *
 * The enumerator puts the variant first because only that order is injective.
 * An FC identifier may begin with `_`, so `<U>__<V>` aliases: union `a_`
 * variant `b` and union `a` variant `_b` both spell `fc__a___b`. Variant-first
 * cannot: `V` contains no `__` (the lexer forbids it) and `U` always starts
 * with `fc__`, so if two splits of one string agreed, the shorter variant would
 * have to be the longer one minus a trailing `_`, and then its separator would
 * read `_f` rather than `__`. (`mangled_tail` in pass1.c guards against the
 * same hazard.) */
static const char *union_tag_type(const char *uname) {
    return intern_sprintf(g_intern, "fc_tag_%s", uname);
}

static const char *union_tag_value(const char *uname, const char *variant) {
    return intern_sprintf(g_intern, "fc_tv_%s__%s", variant, uname);
}

/* Forward declaration for TypeSet (defined later) */
typedef struct TypeSet TypeSet;
static TypeSet *g_eq_set = NULL;
static TypeSet *g_results_set = NULL;   /* result (T!) types needing typedefs */
static TypeSet *g_enum_of_set = NULL;   /* enum types needing an enum_of helper */

static int g_indent_level = 0;
static int g_temp_counter = 0;

/* Report a compiler bug found during emission: a form the earlier passes
 * should have rejected or prepared. It goes through diag_error, so the build
 * fails (the driver checks the error count after codegen) instead of
 * succeeding with broken C. */
static void internal_error(SrcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = str_vsprintf(fmt, ap);
    va_end(ap);
    diag_error(loc, "internal compiler error: %s", msg);
    free(msg);
}

/* File-level non-function globals whose initialization is hoisted into C main */
static Decl **g_file_globals = NULL;
static int g_file_global_count = 0;

/* True while emitting a module-member initializer at C file scope.  Flips
 * EXPR_ARRAY_LIT and EXPR_STRUCT_LIT-with-fixed-array-fields onto an
 * aggregate-initializer emission path instead of the default statement-
 * expression path, which C11 rejects at file scope. */
static bool g_const_context = false;

/* Lexical guard context, toggled by EXPR_GUARD (guarded/unguarded). When true,
 * the three value-precondition guards (float-to-int saturation, integer
 * divide/modulo, slice bounds) emit their bare C operation with no runtime
 * check. A child marker fully overrides its parent, so EXPR_GUARD saves and
 * restores this around its body; function/lambda bodies reset it to false so a
 * marker never leaks across a call boundary. */
static bool g_guards_suppressed = false;

/* Lexical overflow context, toggled by EXPR_GUARD on the overflow axis
 * (checked/unchecked). When true, integer `+ - *`, signed `/` at INT_MIN/-1,
 * signed negation, and lossy integer narrowing casts abort on overflow instead
 * of wrapping/truncating. Default false (unchecked: wrap, the FC default).
 * Independent of g_guards_suppressed: the two axes are orthogonal. Saved and
 * restored around each marker's body; reset to false at function/lambda
 * boundaries so a marker never leaks across a call. */
static bool g_overflow_checked = false;

/* Backing arrays for file-scope slice literals.  Collected by
 * a pre-pass over module-member inits (including nested slice lits), emitted
 * as `static T _fc_const_backing_N[] = {...};` ahead of the module-member
 * definitions.  Each entry's AST node carries the backing name; the slice
 * header emitted at the use site references it. */
static Expr **g_const_backings = NULL;
static int g_const_backing_count = 0;
static int g_const_backing_cap = 0;
static int g_const_backing_counter = 0;

/* Hoisted locals (see HoistCand): declared at function top, assigned at the
 * original site. */
typedef struct {
    const char *codegen_name;
    Type *type;
} HoistedDecl;

static HoistedDecl *g_hoisted = NULL;
static int g_hoisted_count = 0;
static int g_hoisted_cap = 0;

/* One walk of a function body records every local `let` as a hoist candidate,
 * plus the codegen names whose address is taken. begin_hoisted_scope hoists a
 * candidate if it is `let mut` or its address is taken, so a pointer to an
 * inner-scope `let` stays valid for the whole call. Immutable, never-addressed
 * lets stay inline. */
typedef struct {
    const char *codegen_name;
    Type *type;
    bool is_mut;
} HoistCand;
static HoistCand *g_hoist_cand = NULL;
static int g_hoist_cand_count = 0;
static int g_hoist_cand_cap = 0;

static const char **g_addressed = NULL;
static int g_addressed_count = 0;
static int g_addressed_cap = 0;

static bool name_addressed(const char *codegen_name) {
    for (int i = 0; i < g_addressed_count; i++)
        if (g_addressed[i] == codegen_name) return true;
    return false;
}

/* Function-entry backing arrays for stack slice literals and constant-size
 * interpolation buffers.  Each entry is the slice-literal or interp-string node;
 * a fixed C array is emitted at function top and the node's use site references
 * it by name (via codegen_backing_name).  Unlike a per-evaluation alloca, the
 * slot is reused across loop iterations, so stack use stays bounded.  The
 * backing has function lifetime, which is the lifetime escape analysis assumes
 * for a stack slice or str.  Collected fresh per function body. */
static Expr **g_fn_backings = NULL;
static int g_fn_backing_count = 0;
static int g_fn_backing_cap = 0;
static int g_fn_backing_counter = 0;

/* Block-scoped defer tracking */
typedef struct DeferScope DeferScope;
struct DeferScope {
    Expr **defers;      /* array of deferred expressions */
    int count;
    int cap;
    bool is_loop;       /* true for loop/for body scopes */
    DeferScope *parent;
};
static DeferScope *g_defer_scope = NULL;

/* Storage-class prefix for emitted FC functions, monomorphs, lambdas, and
 * trampolines.  The program is one translation unit, so functions are `static`
 * and the C compiler can inline freely and drop unused ones.  With --backtraces
 * we add `noinline` so a call that isn't tail-eliminated keeps its own frame in
 * the execinfo walk.  Tail/sibling-call optimization stays on: tail-recursive
 * loops rely on it to stay stack-safe, so a frame elided by TCO is accepted.
 * The failing line is still exact because it is passed to the abort helper as
 * data, not read off the stack.  GCC also gets a preamble pragma disabling
 * hot/cold block splitting, which would otherwise attribute a return address in
 * .text.unlikely to the wrong function.  Backtrace addresses are resolved
 * through an FC-emitted table keyed by `&fn`, so `-rdynamic` is not needed.
 * Set in codegen_emit(). */
static const char *g_fn_attr = "static __attribute__((unused)) ";

static void emit_type(Type *t, FILE *out);
static void emit_elem_type(Type *t, FILE *out);
static void emit_indent(FILE *out);
static void emit_expr(Expr *e, FILE *out);
static Type *resolve_struct_stub(Type *t);
static void emit_enum_variant_literal(Type *et, const char *vname, FILE *out);
static bool type_valueless(Type *t);
static void emit_c_escaped(const char *text, int len, FILE *out);
static const char *unsigned_counterpart(Type *t);
static bool type_is_subint(Type *t);
static void emit_loc_args(SrcLoc loc, FILE *out);
static void emit_loc_text(SrcLoc loc, FILE *out);

/* ---- String literal decode / re-encode ----
 *
 * A string literal reaches codegen as its source text: the bytes between the
 * quotes with escapes unprocessed.  That text cannot be echoed into C.  FC and
 * C do not agree on what an escape denotes (FC's `\x` takes two hex digits, C's
 * is greedy), and C reads sequences FC does not write at all (trigraphs).  So
 * the text is decoded to bytes once, and the bytes are re-encoded for C. */

/* decode_str_lit lives in common.c: pass2's --len-repr capacity checks and
 * codegen's emission must agree on the decoded byte count. */

/* Decoded byte length of string-literal source text. */
static int str_lit_len(const char *s, int slen) {
    return decode_str_lit(s, slen, NULL);
}

/* Emit one raw byte as the body of a C string literal.  Non-printables take a
 * three-digit octal escape rather than `\x`: C's hex escape is greedy, so
 * `\x41` followed by the byte `1` would be re-read as one out-of-range escape,
 * while `\101` is self-delimiting.  `?` is always escaped so no `??` run can be
 * taken as a trigraph.  `fmt` marks a printf format string, where a literal `%`
 * must be written `%%`. */
static void emit_c_byte(unsigned char b, bool fmt, FILE *out) {
    switch (b) {
    case '\\': fputs("\\\\", out); return;
    case '"':  fputs("\\\"", out); return;
    case '\n': fputs("\\n", out);  return;
    case '\r': fputs("\\r", out);  return;
    case '\t': fputs("\\t", out);  return;
    case '?':  fputs("\\?", out);  return;
    case '%':  fputs(fmt ? "%%" : "%", out); return;
    default:
        if (b < 0x20 || b == 0x7f) fprintf(out, "\\%03o", b);
        else fputc((char)b, out);
        return;
    }
}

/* Emit string-literal source text as a C string literal body, decoding FC's
 * escapes and re-encoding for C. */
static void emit_str_lit_body(const char *s, int slen, FILE *out) {
    int n = decode_str_lit(s, slen, NULL);
    unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
    decode_str_lit(s, slen, buf);
    for (int i = 0; i < n; i++) emit_c_byte(buf[i], false, out);
    free(buf);
}

/* C name of a lifted lambda: pass2's lifted_name, plus the active
 * generic-instance suffix (see g_lambda_suffix). */
static const char *lambda_c_name(Expr *lam) {
    if (!g_lambda_suffix) return lam->func.lifted_name;
    return arena_sprintf(g_arena, "%s__%s", lam->func.lifted_name, g_lambda_suffix);
}

static bool is_hoisted(const char *codegen_name) {
    for (int i = 0; i < g_hoisted_count; i++)
        if (g_hoisted[i].codegen_name == codegen_name) return true;
    return false;
}

/* ---- Defer scope helpers ---- */

static void defer_scope_push(bool is_loop) {
    DeferScope *ds = calloc(1, sizeof(DeferScope));
    ds->is_loop = is_loop;
    ds->parent = g_defer_scope;
    g_defer_scope = ds;
}

static void defer_scope_pop(void) {
    DeferScope *ds = g_defer_scope;
    g_defer_scope = ds->parent;
    free(ds->defers);
    free(ds);
}

static void defer_scope_add(Expr *e) {
    DeferScope *ds = g_defer_scope;
    DA_APPEND(ds->defers, ds->count, ds->cap, e);
}

/* True if a deferred expression lowers to a C statement (bare `for`/`while`/
 * `if`) rather than an expression, so it must be emitted bare instead of wrapped
 * in `(void)(...)`. These are the kinds emit_block_stmts emits as statements.
 * Blocks and matches always emit as statement-expressions `({...})`, so they
 * are safe to wrap. */
static bool defer_emits_statement(Expr *e) {
    switch (e->kind) {
    case EXPR_FOR:  return true;
    case EXPR_LOOP:
    case EXPR_IF:   return type_valueless(e->type);
    case EXPR_GUARD: return defer_emits_statement(e->guard.body);
    default:        return false;
    }
}

/* Emit defers for a single scope in LIFO order */
static void emit_scope_defers(DeferScope *ds, FILE *out) {
    for (int i = ds->count - 1; i >= 0; i--) {
        emit_indent(out);
        Expr *d = ds->defers[i];
        if (defer_emits_statement(d)) {
            /* Statement-emitting form: `(void)(while(1){...})` would be invalid C. */
            emit_expr(d, out);
            fprintf(out, "\n");
        } else {
            fprintf(out, "(void)(");
            emit_expr(d, out);
            fprintf(out, ");\n");
        }
    }
}

/* Emit defers from current scope outward up to and including loop boundary.
 * Used for break and continue. */
static void emit_defers_to_loop(FILE *out) {
    for (DeferScope *ds = g_defer_scope; ds; ds = ds->parent) {
        emit_scope_defers(ds, out);
        if (ds->is_loop) break;
    }
}

/* Emit defers from current scope outward through every scope.
 * Used for return. */
static void emit_defers_to_func(FILE *out) {
    for (DeferScope *ds = g_defer_scope; ds; ds = ds->parent) {
        emit_scope_defers(ds, out);
    }
}

/* Check if any defer scope in the chain has pending defers */
static bool has_pending_defers(void) {
    for (DeferScope *ds = g_defer_scope; ds; ds = ds->parent) {
        if (ds->count > 0) return true;
    }
    return false;
}

static void collect_hoisted_bindings(Expr *e);
static void collect_hoisted_pat(Pattern *pat, Type *type, bool is_mut);

/* Give e a backing slot declared at function entry, named once so a body that
 * is emitted more than once reuses the name. */
static void add_fn_backing(Expr *e, const char **name) {
    if (!*name) *name = arena_sprintf(g_arena, "_fc_back_%d", g_fn_backing_counter++);
    DA_APPEND(g_fn_backings, g_fn_backing_count, g_fn_backing_cap, e);
}

static void collect_hoisted_child(Expr *child, void *ctx) {
    (void)ctx;
    collect_hoisted_bindings(child);
}

/* Recursively collect hoist candidates (every local let), addressed names, and
 * the stack temporaries that get a function-entry backing slot. */
static void collect_hoisted_bindings(Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_LET:
        collect_hoisted_bindings(e->let_expr.let_init);
        if (e->let_expr.codegen_name && e->let_expr.let_type) {
            HoistCand c = { e->let_expr.codegen_name, e->let_expr.let_type,
                            e->let_expr.let_is_mut };
            DA_APPEND(g_hoist_cand, g_hoist_cand_count, g_hoist_cand_cap, c);
        }
        return;
    case EXPR_LET_DESTRUCT:
        collect_hoisted_bindings(e->let_destruct.init);
        collect_hoisted_pat(e->let_destruct.pattern, e->let_destruct.init_type,
                            e->let_destruct.is_mut);
        return;
    case EXPR_UNARY_PREFIX:
        /* &x on a local binding forces x to be hoisted to function scope so
         * the resulting pointer outlives the block x was declared in. */
        if (e->unary_prefix.op == TOK_AMP &&
            e->unary_prefix.operand->kind == EXPR_IDENT &&
            e->unary_prefix.operand->ident.is_local &&
            e->unary_prefix.operand->ident.codegen_name) {
            DA_APPEND(g_addressed, g_addressed_count, g_addressed_cap,
                      e->unary_prefix.operand->ident.codegen_name);
        }
        break;
    case EXPR_CAST:
        /* (cstr[N]) bounded str->cstr cast: the truncating copy lands in a fixed
         * uint8[N] slot, reused across loop iterations, and the produced cstr
         * keeps function-frame lifetime. */
        if (e->cast.buffer_size > 0)
            add_fn_backing(e, &e->cast.codegen_backing_name);
        break;
    case EXPR_ARRAY_LIT:
        /* The size N is a compile-time literal (enforced in pass2), so the array
         * gets a fixed slot reused per loop iteration. C has no zero-length
         * arrays, so an empty literal falls back to alloca at the use site. */
        if (e->array_lit.size_expr &&
            e->array_lit.size_expr->kind == EXPR_INT_LIT &&
            e->array_lit.size_expr->int_lit.value > 0)
            add_fn_backing(e, &e->array_lit.codegen_backing_name);
        break;
    case EXPR_INTERP_STRING: {
        /* A constant-size buffer (no runtime-length %s) gets a fixed slot.
         * Runtime-sized buffers stay on alloca, which grows per loop iteration
         * (documented; alloc(s)! promotes to the heap). */
        int64_t bsize = 0;
        if (interp_const_buffer_size(e, &bsize)) {
            add_fn_backing(e, &e->interp_string.codegen_backing_name);
            e->interp_string.backing_size = bsize;
        }
        break;
    }
    case EXPR_ALLOC:
        /* alloc(T[N]{...}) and alloc("...%d") build their init straight into the
         * heap buffer, so the init node itself gets no stack slot; its children
         * may still need theirs. */
        collect_hoisted_bindings(e->alloc_expr.size_expr);
        if (e->alloc_expr.init_expr) {
            Expr *init = e->alloc_expr.init_expr;
            if (init->kind == EXPR_ARRAY_LIT || init->kind == EXPR_INTERP_STRING)
                expr_for_each_child(init, collect_hoisted_child, NULL);
            else
                collect_hoisted_bindings(init);
        }
        return;
    case EXPR_FUNC:
        /* A capturing lambda's context struct is created in this function's
         * frame and must live as long as the frame (spec: "Stack frames and
         * lifetime"), not just the block the lambda sits in, so it gets a
         * function-entry slot too. The body is not walked: it is its own
         * function, with its own hoisting scope. alloc(lambda) puts the context
         * on the heap instead. */
        if (e->func.capture_count > 0 && !e->func.heap_alloc)
            add_fn_backing(e, &e->func.codegen_ctx_backing_name);
        return;
    default:
        break;
    }
    expr_for_each_child(e, collect_hoisted_child, NULL);
}

/* Collect hoisted bindings from destructuring pattern */
static void collect_hoisted_pat(Pattern *pat, Type *type, bool is_mut) {
    if (!pat || !type) return;
    switch (pat->kind) {
    case PAT_BINDING:
        if (pat->binding.name) {
            HoistCand c = { pat_binding_c_name(pat), type, is_mut };
            DA_APPEND(g_hoist_cand, g_hoist_cand_count, g_hoist_cand_cap, c);
        }
        break;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            collect_hoisted_pat(pat->struc.fields[i].pattern, pat->struc.fields[i].resolved_type, is_mut);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            collect_hoisted_pat(pat->tuple_pat.patterns[i], pat->tuple_pat.resolved_types[i], is_mut);
        break;
    case PAT_SOME:
        if (pat->some_pat.inner && type->kind == TYPE_OPTION)
            collect_hoisted_pat(pat->some_pat.inner, type->option.inner, is_mut);
        break;
    case PAT_OK:
        if (pat->some_pat.inner && type->kind == TYPE_RESULT)
            collect_hoisted_pat(pat->some_pat.inner, type->result.inner, is_mut);
        break;
    case PAT_ERR:
        if (pat->some_pat.inner)
            collect_hoisted_pat(pat->some_pat.inner, type_int32(), is_mut);
        break;
    case PAT_VARIANT:
        if (pat->variant.payload && type->kind == TYPE_UNION) {
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->variant.variant) {
                    collect_hoisted_pat(pat->variant.payload, type->unio.variants[v].payload, is_mut);
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
}

/* Emit hoisted declarations at the top of a function body */
static void emit_hoisted_decls(FILE *out) {
    for (int i = 0; i < g_hoisted_count; i++) {
        emit_indent(out);
        emit_type(g_hoisted[i].type, out);
        Type *t = g_hoisted[i].type;
        /* Use appropriate zero initializer for the type */
        bool is_scalar = type_is_numeric(t) || t->kind == TYPE_BOOL ||
            t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR;
        if (is_scalar)
            fprintf(out, " %s = 0;\n", g_hoisted[i].codegen_name);
        else
            fprintf(out, " %s = {0};\n", g_hoisted[i].codegen_name);
    }
}

/* Emit the function-entry backing arrays for stack slice literals and
 * constant-size interpolation buffers (see g_fn_backings).  Each is a plain
 * fixed C array whose single slot is reused on every loop iteration. */
static void emit_fn_backing_decls(FILE *out) {
    for (int i = 0; i < g_fn_backing_count; i++) {
        Expr *e = g_fn_backings[i];
        emit_indent(out);
        if (e->kind == EXPR_ARRAY_LIT) {
            emit_elem_type(e->array_lit.elem_type, out);
            fprintf(out, " %s[%" PRIu64 "];\n",
                    e->array_lit.codegen_backing_name,
                    e->array_lit.size_expr->int_lit.value);
        } else if (e->kind == EXPR_CAST) { /* (cstr[N]) */
            fprintf(out, "uint8_t %s[%d];\n",
                    e->cast.codegen_backing_name, e->cast.buffer_size);
        } else if (e->kind == EXPR_FUNC) { /* capturing-lambda context struct */
            fprintf(out, "_ctx_%s %s;\n",
                    lambda_c_name(e), e->func.codegen_ctx_backing_name);
        } else { /* EXPR_INTERP_STRING */
            fprintf(out, "uint8_t %s[%" PRId64 "];\n",
                    e->interp_string.codegen_backing_name,
                    e->interp_string.backing_size + 1);
        }
    }
}

/* Set up hoisting for a function body, emit declarations, then tear down */
static void begin_hoisted_scope(Expr **body, int body_count, FILE *out) {
    g_hoisted_count = 0;
    g_hoist_cand_count = 0;
    g_addressed_count = 0;
    g_fn_backing_count = 0;
    for (int i = 0; i < body_count; i++)
        collect_hoisted_bindings(body[i]);
    /* Hoist a candidate iff it is `let mut` or its address is taken. */
    for (int i = 0; i < g_hoist_cand_count; i++) {
        HoistCand *c = &g_hoist_cand[i];
        if (c->is_mut || name_addressed(c->codegen_name)) {
            HoistedDecl d = { c->codegen_name, c->type };
            DA_APPEND(g_hoisted, g_hoisted_count, g_hoisted_cap, d);
        }
    }
    emit_hoisted_decls(out);
    emit_fn_backing_decls(out);
}

static void end_hoisted_scope(void) {
    g_hoisted_count = 0;
    g_hoist_cand_count = 0;
    g_addressed_count = 0;
    g_fn_backing_count = 0;
}

/* Width (bits) at which a slice bounds compare runs: wide enough to hold the
 * index's full static range and the stored len width, so the truncating casts
 * in the fused unsigned compare preserve semantics (every stored len is in
 * [0, FC_LEN_MAX]). At --len-repr 64 this is always 64. With a narrow index
 * type at a narrow --len-repr, the compare is a single native-width op. */
static Type *subst_resolve(Type *t);
static int guard_bits(Type *idx_type) {
    if (g_len_repr >= 64) return 64;
    int b;
    Type *t = idx_type ? subst_resolve(idx_type) : NULL;
    if (t) t = type_enum_underlying(t);
    switch (t ? t->kind : TYPE_INT64) {
    case TYPE_INT8:  case TYPE_UINT8:  b = 8;  break;
    case TYPE_INT16: case TYPE_UINT16: b = 16; break;
    case TYPE_INT32: case TYPE_UINT32: b = 32; break;
    default:                           b = 64; break; /* i64/u64/isize/usize */
    }
    return b > g_len_repr ? b : g_len_repr;
}

/* The concrete type the instance being emitted binds type variable `name` to;
 * NULL outside an instance, or for a variable it does not bind. */
static Type *subst_lookup(const char *name) {
    if (!g_subst) return NULL;
    for (int i = 0; i < g_subst->count; i++)
        if (g_subst->var_names[i] == name)
            return g_subst->concrete[i];
    return NULL;
}

/* `t` with a bare type variable replaced by its binding in the instance being
 * emitted. */
static Type *subst_resolve(Type *t) {
    Type *bound = (t && t->kind == TYPE_TYPE_VAR) ? subst_lookup(t->type_var.name) : NULL;
    return bound ? bound : t;
}

/* pass2 widens a narrower numeric operand into a wider slot by wrapping it
 * in a cast, but it checks a generic body once, with type variables, so an
 * instance reaches the emitter without the casts its concrete types call
 * for. This supplies one for the instance being emitted, so that it compiles
 * as the same code written with those types would: when the operand
 * in `*slot` resolves to a different numeric type than `target`, the slot is
 * pointed at a cast to `target`. Returns the operand it replaced, which the
 * caller puts back after emitting (every instance shares the AST), or NULL
 * when nothing changed. */
static Expr *widen_for_instance(Expr **slot, Type *target) {
    if (!g_subst || !target) return NULL;
    Expr *operand = *slot;
    Type *from = subst_resolve(operand->type);
    target = subst_resolve(target);
    if (!from || type_eq(from, target) || !type_is_numeric(from) || !type_is_numeric(target))
        return NULL;
    Expr *cast = arena_alloc(g_arena, sizeof(Expr));
    cast->kind = EXPR_CAST;
    cast->loc = operand->loc;
    cast->type = target;
    cast->prov = operand->prov;
    cast->cast.target = target;
    cast->cast.operand = operand;
    *slot = cast;
    return operand;
}

/* True when `t` is a u8[] slice (the `str` alias), accounting for the active
 * monomorphization substitution. A generic field/local typed `'a[]` where `'a`
 * is bound to u8 is `str` and must share fc_str's C spelling (and its always-
 * emitted typedef). is_str_type alone inspects the raw type-var element, so one
 * instance would spell the type both `fc_str` (where the element is already
 * concrete) and `fc_slice_uint8_t` (under the type var), and the latter has no
 * typedef. */
static bool slice_is_str_under_subst(Type *t) {
    if (!t || t->kind != TYPE_SLICE) return false;
    Type *elem = subst_resolve(t->slice.elem);
    return elem && elem->kind == TYPE_UINT8;
}

/* Does the option inner type use null-sentinel optimization (bare pointer, NULL = none)? */
static bool is_null_sentinel(Type *opt_type) {
    opt_type = subst_resolve(opt_type);
    if (!opt_type || opt_type->kind != TYPE_OPTION) return false;
    Type *inner = subst_resolve(opt_type->option.inner);
    return inner && (inner->kind == TYPE_POINTER ||
                     inner->kind == TYPE_ANY_PTR);
}

/* Concrete element count of a fixed array at emit time: the folded size, or
 * the symbolic size_ref evaluated under the active monomorphization
 * substitution (a generic body's template types keep 'n symbolic; g_subst
 * binds it per instance). */
static int64_t fixarr_size(Type *t) {
    if (!t || t->kind != TYPE_FIXED_ARRAY) return 0;
    if (!t->fixed_array.size_ref) return t->fixed_array.size;
    Type *r = subst_resolve(t->fixed_array.size_ref);
    if (r && r->kind == TYPE_CONST_INT) return r->const_int.value;
    if (g_subst && r) {
        int64_t v;
        if (const_type_eval(r, g_subst->var_names, g_subst->concrete,
                            g_subst->count, &v))
            return v;
        SrcLoc dummy; (void)const_eval_take_error(&dummy);  /* defensive: pass2 validated */
    }
    return t->fixed_array.size;
}

/* Number of aggregate levels along the first-member chain of `t`'s C
 * representation: how many brace levels a fully-braced zero initializer opens
 * before reaching the first scalar. Used to emit (T){{0}} instead of (T){0}
 * where the bare-zero idiom would trip gcc's -Wmissing-braces (a compound
 * literal nested inside another initializer, e.g. a union variant payload). */
static int zero_agg_depth(Type *t) {
    t = subst_resolve(t);
    if (t && t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_FIXED_ARRAY: return 1 + zero_agg_depth(t->fixed_array.elem);
    case TYPE_STRUCT:
        return t->struc.field_count > 0
            ? 1 + zero_agg_depth(t->struc.fields[0].type) : 1;
    case TYPE_UNION:   return 1;  /* C repr: struct { tag; union {...} }; tag is scalar */
    case TYPE_SLICE:   return 1;  /* struct { ptr; len } */
    case TYPE_RESULT:  return 1;  /* struct { err; value } */
    case TYPE_FUNC:    return 1;  /* struct { fn_ptr; ctx } */
    case TYPE_OPTION:
        /* C repr is value-first, struct { T value; bool has_value; }, so the
         * first-member chain continues through the payload. */
        return is_null_sentinel(t) ? 0 : 1 + zero_agg_depth(t->option.inner);
    default: return 0;            /* scalars, pointers, enums, any* */
    }
}

/* Extra brace levels for the zero in a (T){0}-style compound literal: the
 * outer braces already exist, so this is the aggregate depth of T's first
 * member. */
static int zero_brace_extra(Type *t) {
    t = subst_resolve(t);
    if (t && t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if (!t) return 0;
    switch (t->kind) {
    case TYPE_STRUCT:
        return t->struc.field_count > 0
            ? zero_agg_depth(t->struc.fields[0].type) : 0;
    case TYPE_FIXED_ARRAY: return zero_agg_depth(t->fixed_array.elem);
    case TYPE_OPTION:
        /* value-first repr: the payload is the first member */
        return is_null_sentinel(t) ? 0 : zero_agg_depth(t->option.inner);
    default: return 0;  /* union (tag first), slice/result/fn (scalar first), ... */
    }
}


static void emit_indent(FILE *out) {
    for (int i = 0; i < g_indent_level; i++) fprintf(out, "    ");
}

/* The C name of a fully-concrete generic struct/union instance.
 *
 * A type node can reach emission still carrying its template's base name
 * (nodes produced by substitution bypass pass2's at-creation renames), so the
 * name is spelled by mangle_type_name purely from the node's structure:
 * canonical base from the defining symbol, recursion over the type arguments.
 * Non-generic types (no type arguments) and template residue (an argument
 * still containing a type variable) keep their stored name. */
static const char *generic_instance_c_name(Type *t) {
    const char *stored; Type **targs; int tac;
    if (t->kind == TYPE_STRUCT) {
        stored = t->struc.name; targs = t->struc.type_args; tac = t->struc.type_arg_count;
    } else if (t->kind == TYPE_UNION) {
        stored = t->unio.name; targs = t->unio.type_args; tac = t->unio.type_arg_count;
    } else return NULL;
    if (tac <= 0 || !targs) return stored;
    for (int i = 0; i < tac; i++)
        if (!targs[i] || type_contains_type_var(targs[i])) return stored;
    char *m = mangle_type_name(t);
    const char *r = intern_cstr(g_intern, m);
    free(m);
    return r;
}

/* Compute mangled name for a generic struct/union type under g_subst */
static const char *mangle_generic_with_subst(const char *base_name, Type *t) {
    /* When the type carries explicit type args, substitute those. This folds
     * const-generic expressions: wide<'n * 2> under 'n=128 must mangle as
     * wide<256>, not as the binding of 'n. The var-collect fallback below
     * covers arg-less template types, where args and vars coincide. */
    Type **targs = (t->kind == TYPE_STRUCT) ? t->struc.type_args
                 : (t->kind == TYPE_UNION)  ? t->unio.type_args
                 : (t->kind == TYPE_STUB)   ? t->stub.type_args : NULL;
    int tac = (t->kind == TYPE_STRUCT) ? t->struc.type_arg_count
            : (t->kind == TYPE_UNION)  ? t->unio.type_arg_count
            : (t->kind == TYPE_STUB)   ? t->stub.type_arg_count : 0;
    if (targs && tac > 0) {
        Type **conc = arena_alloc(g_arena, sizeof(Type*) * (size_t)tac);
        for (int i = 0; i < tac; i++)
            conc[i] = type_substitute(g_arena, targs[i], g_subst->var_names,
                                      g_subst->concrete, g_subst->count);
        return mangle_generic_name(g_intern, base_name, conc, tac);
    }
    const char **vars = NULL;
    int vc = 0, vcap = 0;
    type_collect_vars(t, &vars, &vc, &vcap);
    Type **concrete_args = arena_alloc(g_arena, sizeof(Type*) * (size_t)vc);
    for (int i = 0; i < vc; i++) {
        concrete_args[i] = subst_lookup(vars[i]);
        if (!concrete_args[i]) concrete_args[i] = type_type_var(g_arena, vars[i]);
    }
    const char *mangled = mangle_generic_name(g_intern,
        base_name, concrete_args, vc);
    free(vars);
    return mangled;
}

/* The C name of struct or union type `t` where it is emitted: the instance's
 * name under the current substitution when `t` mentions a type variable,
 * otherwise the name spelled from its structure. */
static const char *aggregate_c_name(Type *t) {
    const char *stored = t->kind == TYPE_STRUCT ? t->struc.name : t->unio.name;
    if (g_subst && type_contains_type_var(t)) return mangle_generic_with_subst(stored, t);
    return generic_instance_c_name(t);
}

/* Canonical C name for a tuple type whose element types contain type variables,
 * resolved under the active g_subst. Substitutes the elements, then runs
 * mono_resolve_type_names (which re-derives tuple names and mangles any nested
 * generic struct/union instances) so the result matches the registered name. */
static const char *tuple_name_under_subst(Type *t) {
    Type *sub = type_substitute(g_arena, t, g_subst->var_names,
                                g_subst->concrete, g_subst->count);
    mono_resolve_type_names(g_mono, g_arena, g_intern, sub);
    return sub->struc.name;
}

/* Emit the identifier portion of a function type typedef name */
static void emit_fn_type_suffix(Type *t, FILE *out);

static void emit_type_ident(Type *t, FILE *out);

/* Spell a slice's element type in C modulo its top-level `const`.
 *
 * The C slice struct is named after its element by emit_type_ident, which
 * never prints a qualifier, so `(const i32*)[]` (writable slots holding
 * read-only views) and `const i32*[]` (a read-only slice) share the one name
 * fc_slice_int32_t_ptr. Every position that spells the element must therefore
 * drop the qualifier too (the struct member, a literal's backing array, an
 * alloca'd one), or the two disagree on whether `ptr` is `const int32_t**` and
 * whichever definition is emitted first wins the other's storage.
 *
 * Dropping it loses nothing. FC enforces const itself, and the C projection
 * cannot model what FC means anyway: the read-only slice is the one whose
 * member would come out non-const, while the writable-slot form would be the
 * const one. Keeping the qualifier out of slice storage also keeps writes to a
 * slot free of qualified/unqualified aliasing. */
static void emit_elem_type(Type *t, FILE *out);

/* True when a cast only moves `const` around and the C conversion happens
 * anyway, so the cast itself can be dropped. Two shapes qualify:
 *
 *  - a slice, whose constness never reaches its C type (fc_slice_T is spelled
 *    modulo const, elements included), so every const variant is one C type.
 *    Emitting the cast would make it a struct cast to its own type: a no-op
 *    where the C compiler tolerates it, and rejected in a static initializer,
 *    where a cast stops the expression being a constant one.
 *  - a plain top-level const-add on a pointer with an otherwise identical
 *    pointee, which C performs implicitly at every assignment and argument.
 *    Dropping it also keeps a `(const int32_t*)&x` out of a store into slice
 *    storage, whose element type is spelled modulo const (emit_elem_type) and
 *    would report the cast as discarding a qualifier the slot never had.
 *
 * A pointer const-strip is excluded (C needs that one spelled), as is a
 * const-add that also rearranges the pointee's own qualifier
 * (`(const i32*)* -> const i32**`), whose two C types are not implicitly
 * convertible either way.
 *
 * pass2 still needs the cast node in every case: the node wrap_widen builds
 * carries provenance through for escape analysis. */
static bool cast_is_const_only_noop(Type *to, Type *from) {
    if (!to || !from || to->kind != from->kind) return false;
    if (to->kind == TYPE_SLICE) return type_eq_ignore_const(to, from);
    if (to->kind == TYPE_POINTER)
        return to->is_const && !from->is_const &&
               type_eq(to->pointer.pointee, from->pointer.pointee);
    return false;
}

/* True when an element's `const` would show up in its C spelling: pointers
 * and any*, since a slice element like `const str` already emits as a plain
 * fc_str. A value of such a type needs an explicit strip when it is stored
 * into slice storage, which is spelled modulo const. */
static bool elem_const_shows_in_c(Type *t) {
    return t && t->is_const && (t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR);
}

/* Emit the cast that strips such a `const`, for a store into slice storage. */
static void emit_elem_store_cast(Type *elem_type, FILE *out) {
    if (!elem_const_shows_in_c(elem_type)) return;
    fprintf(out, "(");
    emit_elem_type(elem_type, out);
    fprintf(out, ")");
}

static void emit_elem_type(Type *t, FILE *out) {
    if (t && t->is_const) {
        Type bare = *t;
        bare.is_const = false;
        emit_type(&bare, out);
        return;
    }
    emit_type(t, out);
}

/* The ident naming a slice's typedef (`fc_slice_<elem ident>`). The element is
 * spelled modulo const, as its storage is (emit_elem_type), so `(const i32*)[]`
 * and `const i32*[]` name one typedef. That is what lets the former widen into
 * the latter in C, where two slice structs of different names would be
 * unrelated types. FC enforces element constness at the type level; it never
 * reaches C. */
static void emit_slice_elem_ident(Type *elem, FILE *out) {
    Type *el = subst_resolve(elem);
    if (el && el->is_const) {
        Type bare = *el;
        bare.is_const = false;
        emit_type_ident(&bare, out);
        return;
    }
    emit_type_ident(elem, out);
}

static void emit_type(Type *t, FILE *out) {
    /* A type variable of the instance being emitted is its binding. */
    Type *bound = subst_resolve(t);
    if (bound != t) {
        emit_type(bound, out);
        return;
    }
    switch (t->kind) {
    case TYPE_INT8:    fprintf(out, "int8_t");    break;
    case TYPE_INT16:   fprintf(out, "int16_t");   break;
    case TYPE_INT32:   fprintf(out, "int32_t");   break;
    case TYPE_INT64:   fprintf(out, "int64_t");   break;
    case TYPE_UINT8:   fprintf(out, "uint8_t");   break;
    case TYPE_UINT16:  fprintf(out, "uint16_t");  break;
    case TYPE_UINT32:  fprintf(out, "uint32_t");  break;
    case TYPE_UINT64:  fprintf(out, "uint64_t");  break;
    case TYPE_ISIZE:   fprintf(out, "ptrdiff_t"); break;
    case TYPE_USIZE:   fprintf(out, "size_t");    break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_VOID:    fprintf(out, "void");      break;
    case TYPE_NEVER:   fprintf(out, "void");      break; /* defensive: never materialized */
    case TYPE_UNRESOLVED: fprintf(out, "void");   break; /* defensive: patched before codegen */
    case TYPE_POINTER:
        /* East const: the qualifier binds to the pointee, which may itself be
         * a pointer. Spelled west, `const int32_t**` reads in C as "pointer to
         * pointer to const int", a different type that permits the `*p = q`
         * FC rejects and rejects the `int32_t**` argument FC accepts.
         * `int32_t* const*` is what FC means; for a value pointee,
         * `int32_t const*` is `const int32_t*` spelled the other way. */
        emit_type(t->pointer.pointee, out);
        if (t->is_const) fprintf(out, " const");
        fprintf(out, "*");
        break;
    case TYPE_SLICE:
        if (slice_is_str_under_subst(t)) {
            fprintf(out, "fc_str");
        } else {
            fprintf(out, "fc_slice_");
            emit_slice_elem_ident(t->slice.elem, out);
        }
        break;
    case TYPE_OPTION: {
        Type *inner = subst_resolve(t->option.inner);
        if (inner && (inner->kind == TYPE_POINTER ||
            inner->kind == TYPE_ANY_PTR)) {
            /* Pointer-like inner: a plain pointer, NULL = none */
            emit_type(inner, out);
        } else {
            fprintf(out, "fc_option_");
            if (inner) emit_type_ident(inner, out);
            else fprintf(out, "void");
        }
        break;
    }
    case TYPE_RESULT: {
        /* Always the { err; value } struct; no null-sentinel form */
        Type *inner = subst_resolve(t->result.inner);
        fprintf(out, "fc_result_");
        if (inner) emit_type_ident(inner, out);
        else fprintf(out, "void");
        break;
    }
    case TYPE_STRUCT:
        if (t->struc.is_tuple) {
            if (g_subst && type_contains_type_var(t))
                fprintf(out, "%s", tuple_name_under_subst(t));
            else
                fprintf(out, "%s", t->struc.name ? t->struc.name
                    : tuple_canonical_name(g_intern, t->struc.fields, t->struc.field_count));
            break;
        }
        if (t->struc.c_name)   /* an extern struct is never generic */
            fprintf(out, "%s %s", t->struc.is_c_union ? "union" : "struct", t->struc.c_name);
        else
            fprintf(out, "%s", aggregate_c_name(t));
        break;
    case TYPE_UNION:
        fprintf(out, "%s", aggregate_c_name(t));
        break;
    case TYPE_ENUM:
        fprintf(out, "%s", t->enu.name);
        break;
    case TYPE_FUNC:
        fprintf(out, "fc_fn_");
        emit_fn_type_suffix(t, out);
        break;
    case TYPE_FIXED_ARRAY:
        /* Emitted as a C array type, as sizeof(T[N]) needs */
        emit_type(t->fixed_array.elem, out);
        fprintf(out, "[%lld]", (long long)fixarr_size(t));
        break;
    case TYPE_ANY_PTR:
        if (t->is_const) fprintf(out, "const ");
        fprintf(out, "void*");
        break;
    case TYPE_STUB: {
        /* Resolve stub to the actual type and emit it */
        Type *resolved = resolve_struct_stub(t);
        if (resolved != t) {
            emit_type(resolved, out);
        } else {
            /* Fallback: use the stub name directly as the C type name */
            fprintf(out, "%s", t->stub.name);
        }
        break;
    }
    default:
        internal_error((SrcLoc){0}, "no C spelling for type kind %d", t->kind);
        break;
    }
}

/* Emit a C type name suitable for use in identifiers (slice/option typedef names) */
static void emit_type_ident(Type *t, FILE *out) {
    Type *bound = subst_resolve(t);
    if (bound != t) {
        emit_type_ident(bound, out);
        return;
    }
    switch (t->kind) {
    case TYPE_INT8:    fprintf(out, "int8_t");    break;
    case TYPE_INT16:   fprintf(out, "int16_t");   break;
    case TYPE_INT32:   fprintf(out, "int32_t");   break;
    case TYPE_INT64:   fprintf(out, "int64_t");   break;
    case TYPE_UINT8:   fprintf(out, "uint8_t");   break;
    case TYPE_UINT16:  fprintf(out, "uint16_t");  break;
    case TYPE_UINT32:  fprintf(out, "uint32_t");  break;
    case TYPE_UINT64:  fprintf(out, "uint64_t");  break;
    case TYPE_ISIZE:   fprintf(out, "ptrdiff_t"); break;
    case TYPE_USIZE:   fprintf(out, "size_t");    break;
    case TYPE_FLOAT32: fprintf(out, "float");     break;
    case TYPE_FLOAT64: fprintf(out, "double");    break;
    case TYPE_BOOL:    fprintf(out, "bool");      break;
    case TYPE_ANY_PTR: fprintf(out, t->is_const ? "void_cptr" : "void_ptr");  break;
    case TYPE_STRUCT:
        if (t->struc.is_tuple) {
            if (g_subst && type_contains_type_var(t))
                fprintf(out, "%s", tuple_name_under_subst(t));
            else
                fprintf(out, "%s", t->struc.name ? t->struc.name
                    : tuple_canonical_name(g_intern, t->struc.fields, t->struc.field_count));
        }
        else if (t->struc.c_name)
            fprintf(out, "%s", t->struc.c_name);
        else
            fprintf(out, "%s", aggregate_c_name(t));
        break;
    case TYPE_UNION:
        fprintf(out, "%s", aggregate_c_name(t));
        break;
    case TYPE_ENUM:
        fprintf(out, "%s", t->enu.name);
        break;
    case TYPE_SLICE:
        if (slice_is_str_under_subst(t)) {
            fprintf(out, "fc_str");
        } else {
            fprintf(out, "fc_slice_");
            emit_slice_elem_ident(t->slice.elem, out);
        }
        break;
    case TYPE_POINTER:
        /* const is part of the ident: a read-only pointer has a different C
         * type (`T const*`) from a writable one, so an option, result or
         * function type over each needs its own typedef. Slices stay
         * const-blind (their C storage is). */
        emit_type_ident(t->pointer.pointee, out);
        fprintf(out, t->is_const ? "_cptr" : "_ptr");
        break;
    case TYPE_FUNC:
        fprintf(out, "fc_fn_");
        emit_fn_type_suffix(t, out);
        break;
    case TYPE_OPTION:
        fprintf(out, "fc_option_");
        if (t->option.inner) emit_type_ident(t->option.inner, out);
        else fprintf(out, "void");
        break;
    case TYPE_RESULT:
        fprintf(out, "fc_result_");
        if (t->result.inner) emit_type_ident(t->result.inner, out);
        else fprintf(out, "void");
        break;
    case TYPE_FIXED_ARRAY:
        fprintf(out, "fixarr%lld_", (long long)fixarr_size(t));
        emit_type_ident(t->fixed_array.elem, out);
        break;
    case TYPE_VOID:
        fprintf(out, "void");
        break;
    case TYPE_STUB: {
        Type *resolved = resolve_struct_stub(t);
        if (resolved != t) {
            emit_type_ident(resolved, out);
        } else {
            fprintf(out, "%s", t->stub.name);
        }
        break;
    }
    default:           fprintf(out, "unknown");   break;
    }
}

/* Emit the identifier suffix for function type typedef names.
   E.g., for (i32, i32) -> bool: "int32_t_int32_t__bool" */
static void emit_fn_type_suffix(Type *t, FILE *out) {
    if (t->func.param_count == 0) {
        fprintf(out, "v");
    } else {
        for (int i = 0; i < t->func.param_count; i++) {
            if (i > 0) fprintf(out, "_");
            emit_type_ident(t->func.param_types[i], out);
        }
    }
    fprintf(out, "__");
    if (t->func.return_type->kind == TYPE_VOID) {
        fprintf(out, "void");
    } else {
        emit_type_ident(t->func.return_type, out);
    }
    /* Variadic and non-variadic function types are distinct (type_eq compares
     * is_variadic), so their typedef names must differ too. Otherwise
     * `(const cstr, ...) -> i32` (printf) and `(const cstr) -> i32`
     * (remove) mangle to the same name and emit a duplicate typedef. */
    if (t->func.is_variadic) {
        fprintf(out, "_vararg");
    }
}

static void emit_expr(Expr *e, FILE *out);
static void emit_eq_func_name(Type *t, FILE *out);

/* ---- Left-to-right evaluation-order sequencing ---------------------------
 *
 * C leaves the evaluation order of function-call arguments and of most binary
 * operands unspecified.  FC guarantees left-to-right, evaluate-once semantics
 * (see spec "Evaluation order").  Where an operand list contains a
 * side-effecting expression, codegen forces the order by evaluating the earlier
 * operands into temporaries, in source order, inside a statement-expression,
 * then rewriting those operand slots to read the temporaries.  Pure operands
 * (no calls/assignments/allocations) need no ordering and are emitted in place.
 */

/* Conservative "may produce an observable side effect" test: a call,
 * assignment, allocation, free, or atomic op anywhere in the tree.  Pure reads,
 * literals, and lambda values return false; control-flow operands default to
 * true (they may contain anything). */
static bool expr_has_side_effects(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT: case EXPR_STRING_LIT: case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT: case EXPR_IDENT: case EXPR_SIZEOF:
    case EXPR_ALIGNOF: case EXPR_TYPE_VAR_REF: case EXPR_DEFAULT:
    case EXPR_FUNC:
        return false;
    case EXPR_CALL: case EXPR_ASSIGN: case EXPR_ALLOC: case EXPR_FREE:
    case EXPR_ATOMIC_LOAD: case EXPR_ATOMIC_STORE:
        return true;
    case EXPR_BINARY:
        return expr_has_side_effects(e->binary.left) ||
               expr_has_side_effects(e->binary.right);
    case EXPR_UNARY_PREFIX:  return expr_has_side_effects(e->unary_prefix.operand);
    case EXPR_UNARY_POSTFIX:
        /* x? may return early from the enclosing function; treat it as an
         * effect so argument-evaluation sequencing stays left-to-right. */
        if (e->unary_postfix.op == TOK_QUESTION) return true;
        return expr_has_side_effects(e->unary_postfix.operand);
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        return expr_has_side_effects(e->field.object);
    case EXPR_INDEX:
        return expr_has_side_effects(e->index.object) ||
               expr_has_side_effects(e->index.index);
    case EXPR_SLICE:
        return expr_has_side_effects(e->slice.object) ||
               expr_has_side_effects(e->slice.lo) ||
               expr_has_side_effects(e->slice.hi);
    case EXPR_CAST: return expr_has_side_effects(e->cast.operand);
    case EXPR_BITCAST: return expr_has_side_effects(e->bitcast_expr.operand);
    case EXPR_ENUM_OF: return expr_has_side_effects(e->enum_of_expr.operand);
    case EXPR_GUARD: return expr_has_side_effects(e->guard.body);
    case EXPR_SOME: return expr_has_side_effects(e->some_expr.value);
    case EXPR_OK:   return expr_has_side_effects(e->ok_expr.value);
    case EXPR_ERR:  return true;   /* may abort via the zero-code guard */
    case EXPR_ERROR_NAME: return expr_has_side_effects(e->error_name_expr.code);
    case EXPR_INTERP_STRING:
        for (int i = 0; i < e->interp_string.segment_count; i++)
            if (!e->interp_string.segments[i].is_literal &&
                expr_has_side_effects(e->interp_string.segments[i].expr))
                return true;
        return false;
    case EXPR_STRUCT_LIT:
        for (int i = 0; i < e->struct_lit.field_count; i++)
            if (expr_has_side_effects(e->struct_lit.fields[i].value)) return true;
        return false;
    case EXPR_TUPLE_LIT:
        for (int i = 0; i < e->tuple_lit.elem_count; i++)
            if (expr_has_side_effects(e->tuple_lit.elems[i])) return true;
        return false;
    case EXPR_ARRAY_LIT:
        for (int i = 0; i < e->array_lit.elem_count; i++)
            if (expr_has_side_effects(e->array_lit.elems[i])) return true;
        return false;
    case EXPR_SLICE_LIT:
        return expr_has_side_effects(e->slice_lit.ptr_expr) ||
               expr_has_side_effects(e->slice_lit.len_expr);
    default:
        /* if/match/loop/for/block/break/return/... may contain effects */
        return true;
    }
}

/* True for operands that read no mutable state and have no side effects, so
 * their position relative to a side-effecting sibling can never be observed;
 * leaving them in place avoids a pointless temporary. */
static bool seq_is_atom(Expr *e) {
    switch (e->kind) {
    case EXPR_INT_LIT: case EXPR_FLOAT_LIT: case EXPR_BOOL_LIT:
    case EXPR_CHAR_LIT: case EXPR_STRING_LIT: case EXPR_CSTRING_LIT:
    case EXPR_VOID_LIT: case EXPR_SIZEOF: case EXPR_ALIGNOF:
    case EXPR_TYPE_VAR_REF: case EXPR_DEFAULT:
        return true;
    case EXPR_IDENT:
        /* a top-level function reference is a constant, not a variable read */
        return e->type && e->type->kind == TYPE_FUNC && !e->ident.is_local;
    case EXPR_FIELD:
        return e->field.is_type_property || e->field.is_extern_const;
    default:
        return false;
    }
}

/* A synthetic local-ident expr that emits as the bare temp name (is_local
 * suppresses the function-value fat-pointer wrapping in EXPR_IDENT). */
static Expr seq_make_ref(const char *name, Type *type) {
    Expr e;
    memset(&e, 0, sizeof e);
    e.kind = EXPR_IDENT;
    e.type = type;
    e.ident.name = name;
    e.ident.codegen_name = name;
    e.ident.is_local = true;
    return e;
}

/* Does any operand in this list have side effects?  (If not, C's order is
 * already unobservable and no sequencing temps are needed.) */
static bool seq_needed(Expr ***slots, int n) {
    for (int i = 0; i < n; i++)
        if (expr_has_side_effects(*slots[i])) return true;
    return false;
}

/* Structural equality of two operands, used only to spot a self-comparison.
 * `x == x` is legal FC (the spec's no-op rule covers only self-assignment),
 * but gcc and clang reject the emitted C as tautological, so the comparison is
 * emitted with its left operand hoisted into a temporary, which the C
 * compilers do not equate.  Only side-effect-free shapes answer true.  That is
 * also the set the C compilers flag, since they never equate operands they
 * cannot prove pure, so a false here costs nothing. */
static bool expr_structurally_equal(Expr *a, Expr *b) {
    if (a == b) return a != NULL;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
    case EXPR_INT_LIT:   return a->int_lit.value == b->int_lit.value;
    case EXPR_FLOAT_LIT: return a->float_lit.value == b->float_lit.value;
    case EXPR_BOOL_LIT:  return a->bool_lit.value == b->bool_lit.value;
    case EXPR_CHAR_LIT:  return a->char_lit.value == b->char_lit.value;
    case EXPR_IDENT: {
        const char *an = a->ident.codegen_name ? a->ident.codegen_name : a->ident.name;
        const char *bn = b->ident.codegen_name ? b->ident.codegen_name : b->ident.name;
        return an && bn && strcmp(an, bn) == 0;
    }
    case EXPR_FIELD: case EXPR_DEREF_FIELD:
        return a->field.name == b->field.name &&
               expr_structurally_equal(a->field.object, b->field.object);
    case EXPR_INDEX:
        return expr_structurally_equal(a->index.object, b->index.object) &&
               expr_structurally_equal(a->index.index, b->index.index);
    case EXPR_UNARY_PREFIX:
        return a->unary_prefix.op == b->unary_prefix.op &&
               expr_structurally_equal(a->unary_prefix.operand,
                                       b->unary_prefix.operand);
    case EXPR_BINARY:
        return a->binary.op == b->binary.op &&
               expr_structurally_equal(a->binary.left, b->binary.left) &&
               expr_structurally_equal(a->binary.right, b->binary.right);
    case EXPR_CAST:
        return type_eq(a->cast.target, b->cast.target) &&
               expr_structurally_equal(a->cast.operand, b->cast.operand);
    default:
        /* calls, allocations, control flow, aggregates: never equated */
        return false;
    }
}

/* A comparison whose two operands emit identical C (see
 * expr_structurally_equal).  Types that route through a generated `fc_eq_*`
 * helper are excluded: those emit a call, which is never tautological. */
static bool binary_is_self_compare(Expr *e) {
    switch (e->binary.op) {
    case TOK_EQEQ: case TOK_BANGEQ: case TOK_LT:
    case TOK_GT:   case TOK_LTEQ:   case TOK_GTEQ: break;
    default: return false;
    }
    if (type_needs_eq_func(e->binary.left->type)) return false;
    return expr_structurally_equal(e->binary.left, e->binary.right);
}

/* Evaluate operands [0, n-1) into temporaries in source order (skipping
 * side-effect-free atoms and any operand lacking a type), rewriting their slots
 * to read the temporaries.  The final operand is left in place, since it is
 * evaluated last regardless.  Assumes a statement-expression `({ ` is already
 * open.  `scratch` (length >= n) backs the synthetic ident refs and must
 * outlive the body emit; `saved` (length >= n) records originals for restore. */
static void seq_hoist(Expr ***slots, int n, Expr *scratch, Expr **saved, FILE *out) {
    for (int i = 0; i < n - 1; i++) {
        Expr *op = *slots[i];
        saved[i] = op;
        if (!op->type || seq_is_atom(op)) continue;
        char *nm = arena_alloc(g_arena, 24);
        snprintf(nm, 24, "_sq%d", g_temp_counter++);
        emit_type(op->type, out);
        fprintf(out, " %s = ", nm);
        emit_expr(op, out);
        fprintf(out, "; ");
        scratch[i] = seq_make_ref(nm, op->type);
        *slots[i] = &scratch[i];
    }
}

/* Restore operand slots rewritten by seq_hoist. */
static void seq_restore(Expr ***slots, int n, Expr **saved) {
    for (int i = 0; i < n - 1; i++) *slots[i] = saved[i];
}

/* A list of operands that C would evaluate in an unspecified order (call
 * arguments, initializer lists). `active` when their side effects must be
 * forced into source order: outside a constant context, with two or more
 * operands, one of which has effects. Then seq_hoist and seq_restore use the
 * scratch space allocated here. */
typedef struct {
    Expr ***slots;
    int n;
    Expr *scratch;
    Expr **saved;
    bool active;
} SeqOperands;

static SeqOperands seq_operands(Expr ***slots, int n) {
    SeqOperands s = { slots, n, NULL, NULL, false };
    s.active = !g_const_context && n >= 2 && seq_needed(slots, n);
    if (s.active) {
        s.scratch = arena_alloc(g_arena, sizeof(Expr) * (size_t)n);
        s.saved = arena_alloc(g_arena, sizeof(Expr*) * (size_t)n);
    }
    return s;
}

/* The slots of an array of `n` expressions. */
static Expr ***expr_slots(Expr **xs, int n) {
    Expr ***slots = arena_alloc(g_arena, sizeof(Expr**) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) slots[i] = &xs[i];
    return slots;
}

/* True when emitting `e` yields a plainly parenthesized `(...)` expression, so
 * a controlling-position `if ` may follow it without adding its own parens
 * (avoiding clang's -Wparentheses-equality on `if ((x == y))`).  A binary
 * operand list with side effects, a self-comparison, or a division/modulo
 * instead emits a statement-expression `({...})` and must be parenthesized
 * normally. */
static bool emit_self_parens(Expr *e) {
    if (e->kind != EXPR_BINARY) return false;
    if (e->binary.op == TOK_SLASH || e->binary.op == TOK_PERCENT) return false;
    Expr **slots[2] = { &e->binary.left, &e->binary.right };
    bool seq = e->binary.op != TOK_AMPAMP && e->binary.op != TOK_PIPEPIPE &&
               !g_const_context &&
               (seq_needed(slots, 2) || binary_is_self_compare(e));
    return !seq;
}

/* Emit a function call argument for an extern call, inserting casts at the
 * C boundary: cstr (uint8*) to const char*, cstr* (uint8**) to char**,
 * and any** (void**) to void* (C's void* converts to any T** implicitly,
 * but void** does not). */
static void emit_extern_arg(Expr *e, Type *param_type, FILE *out) {
    if (param_type && param_type->kind == TYPE_FUNC) {
        /* Function at the C boundary: pass its C-compatible trampoline */
        if (e->kind == EXPR_IDENT && !e->ident.is_local) {
            const char *name = e->ident.codegen_name ? e->ident.codegen_name : e->ident.name;
            fprintf(out, "fc_ctramp_%s", name);
            return;
        }
        if (e->kind == EXPR_FUNC && e->func.capture_count == 0 && e->func.lifted_name) {
            fprintf(out, "fc_ctramp_%s", lambda_c_name(e));
            return;
        }
        /* pass2 rejects any other function value passed to an extern */
    }
    if (param_type && is_cstr_type(param_type)) {
        if (param_type->is_const)
            fprintf(out, "(const char*)");
        else
            fprintf(out, "(char*)");
    } else if (param_type && param_type->kind == TYPE_POINTER &&
               is_cstr_type(param_type->pointer.pointee)) {
        /* uint8** to char** at C boundary (e.g. strtol's char** out-param) */
        fprintf(out, "(char**)");
    } else if (param_type && param_type->kind == TYPE_POINTER &&
               param_type->pointer.pointee->kind == TYPE_ANY_PTR) {
        /* any** (void**) to void* at C boundary: void* converts to any T**
         * implicitly in C, but void** does not (e.g. sqlite3_open's sqlite3**) */
        fprintf(out, "(void*)");
    } else if (!param_type && e->type) {
        /* Variadic arg: apply C default argument promotions and boundary casts */
        Type *at = e->type;
        if (is_cstr_type(at)) {
            fprintf(out, "(const char*)");
        } else if (at->kind == TYPE_FLOAT32) {
            fprintf(out, "(double)");
        } else if (type_is_subint(at)) {
            fprintf(out, "(int)");
        }
    }
    emit_expr(e, out);
}

/* The raw C call of an extern function: cstr-boundary casts on each arg, and
 * the char* to uint8_t* return cast keyed on `ret_like`: the extern's declared
 * return type for plain calls, or the result payload for protocol calls
 * (whose raw C return is the payload, not the T!). */
static void emit_raw_extern_call(Expr *e, const char *fn_name, Type *call_ft,
                                 Type *ret_like, FILE *out) {
    bool ret_is_cstr = ret_like && is_cstr_type(ret_like);
    bool ret_is_cstr_opt = ret_like && ret_like->kind == TYPE_OPTION &&
        is_cstr_type(ret_like->option.inner);
    bool ret_is_cstr_ptr = ret_like && ret_like->kind == TYPE_POINTER &&
        is_cstr_type(ret_like->pointer.pointee);
    if (ret_is_cstr || ret_is_cstr_opt)
        fprintf(out, "(uint8_t*)");
    else if (ret_is_cstr_ptr)
        fprintf(out, "(uint8_t**)");
    fprintf(out, "%s(", fn_name);
    for (int i = 0; i < e->call.arg_count; i++) {
        if (i > 0) fprintf(out, ", ");
        Type *pt = (call_ft && call_ft->kind == TYPE_FUNC &&
                    i < call_ft->func.param_count)
            ? call_ft->func.param_types[i] : NULL;
        emit_extern_arg(e->call.args[i], pt, out);
    }
    fprintf(out, ")");
}

/* Emit an extern call declared with an error protocol (`from <protocol>`):
 * call the raw C function, test the protocol's failure convention, capture
 * the code, and build the declared T! in place, as a hand-written adapter
 * would. The err-path payload is zero-filled (the sentinel is protocol noise,
 * not data), and codes pass through raw, with no arithmetic. `status` is a
 * repr identity: the return value is the err tag, one struct wrap and no
 * branch. The out-of-band protocols (errno / last_error / wsa_error) run the
 * err(T,0) runtime guard, so a library that reports failure but leaves the
 * code 0 aborts. neg_errno/hresult codes are nonzero by the failure test
 * (< 0), so they need no guard. */
static void emit_protocol_extern_call(Expr *e, const char *fn_name,
                                      ExternProtocol proto, Type *call_ft,
                                      Type *ret_type, FILE *out) {
    Type *pay = subst_resolve(ret_type->result.inner);
    bool pay_void = pay && pay->kind == TYPE_VOID;

    if (proto == EXT_PROTO_STATUS) {
        fprintf(out, "(");
        emit_type(ret_type, out);
        fprintf(out, "){ .err = (int32_t)");
        emit_raw_extern_call(e, fn_name, call_ft, NULL, out);
        fprintf(out, " }");
        return;
    }

    int tid = g_temp_counter++;
    fprintf(out, "({ ");
    /* Raw-return temp: the payload's C type when there is one; a wide signed
     * integer otherwise (any C status return converts in losslessly). */
    if (pay_void)
        fprintf(out, "%s", proto == EXT_PROTO_HRESULT ? "int32_t" : "long long");
    else
        emit_type(pay, out);
    fprintf(out, " _xv%d = ", tid);
    emit_raw_extern_call(e, fn_name, call_ft, pay_void ? NULL : pay, out);
    fprintf(out, "; ");

    if (proto == EXT_PROTO_NEG_ERRNO || proto == EXT_PROTO_HRESULT) {
        /* The code is the (negative) return itself, nonzero by the test */
        if (pay_void) {
            fprintf(out, "(");
            emit_type(ret_type, out);
            fprintf(out, "){ .err = _xv%d < 0 ? (int32_t)_xv%d : 0 }; })",
                    tid, tid);
        } else {
            fprintf(out, "_xv%d < 0 ? (", tid);
            emit_type(ret_type, out);
            fprintf(out, "){ .err = (int32_t)_xv%d } : (", tid);
            emit_type(ret_type, out);
            fprintf(out, "){ .err = 0, .value = _xv%d }; })", tid);
        }
        return;
    }

    /* Sentinel-tested protocols with an out-of-band code source */
    const char *sentinel =
        (proto == EXT_PROTO_ERRNO_NULL || proto == EXT_PROTO_LASTERR_NULL)
            ? "NULL"
        : (proto == EXT_PROTO_LASTERR_0) ? "0" : "-1";
    const char *code_src =
        (proto == EXT_PROTO_ERRNO_NEG1 || proto == EXT_PROTO_ERRNO_NULL)
            ? "errno"
        : (proto == EXT_PROTO_WSA_NEG1) ? "WSAGetLastError()"
                                        : "GetLastError()";
    fprintf(out, "int32_t _xe%d = 0; if (_xv%d == %s) { _xe%d = (int32_t)%s; "
                 "if (__builtin_expect(_xe%d == 0, 0)) fc_zero_err(",
            tid, tid, sentinel, tid, code_src, tid);
    emit_loc_args(e->loc, out);
    fprintf(out, "); } ");
    if (pay_void) {
        fprintf(out, "(");
        emit_type(ret_type, out);
        fprintf(out, "){ .err = _xe%d }; })", tid);
    } else {
        fprintf(out, "_xe%d ? (", tid);
        emit_type(ret_type, out);
        fprintf(out, "){ .err = _xe%d } : (", tid);
        emit_type(ret_type, out);
        fprintf(out, "){ .err = 0, .value = _xv%d }; })", tid);
    }
}

/* Returns true if this pattern produces any condition predicate: false for
   wildcard/binding patterns and for struct/tuple/or patterns whose subpatterns
   are all wildcard-only. Used to decide whether an arm needs an `if` at all. */
static bool pattern_has_predicate(Pattern *pat) {
    switch (pat->kind) {
    case PAT_WILDCARD:
    case PAT_BINDING:
        return false;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            if (pattern_has_predicate(pat->struc.fields[i].pattern)) return true;
        return false;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            if (pattern_has_predicate(pat->tuple_pat.patterns[i])) return true;
        return false;
    case PAT_OR:
        for (int i = 0; i < pat->or_pat.alt_count; i++)
            if (pattern_has_predicate(pat->or_pat.alts[i])) return true;
        return false;
    default:
        return true;
    }
}

/* Emit predicates joined by ` && `, without any enclosing `if (`.
   `first` tracks whether any predicate has been emitted yet in the current
   conjunction chain; it flips to false on the first emission. */
static void emit_pat_predicate(Pattern *pat, const char *expr, Type *type, bool *first, FILE *out) {
    /* A payload/field type reached by descending into a pattern arrives as the
     * name-only TYPE_STUB the declaration wrote, not the definition the subject
     * expression carried, so the kind dispatch below (enum scalar compare vs.
     * union tag compare, option/result shape) must resolve it first.  Only the
     * outermost type came from a checked expression.  A no-op for an
     * already-resolved type. */
    if (type) type = resolve_struct_stub(type);
    switch (pat->kind) {
    case PAT_BINDING:
    case PAT_WILDCARD:
    case PAT_ERROR:      /* unreachable: error nodes never reach codegen */
    case PAT_CONST_PATH: /* unreachable: rewritten to PAT_INT_LIT in pass2 */
        break;
    case PAT_INT_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        if (pat->int_lit.lit_type && (pat->int_lit.lit_type->kind == TYPE_UINT64 ||
            pat->int_lit.lit_type->kind == TYPE_UINT32 || pat->int_lit.lit_type->kind == TYPE_UINT16 ||
            pat->int_lit.lit_type->kind == TYPE_UINT8 || pat->int_lit.lit_type->kind == TYPE_USIZE))
            fprintf(out, "%s == %" PRIu64, expr, pat->int_lit.value);
        else
            fprintf(out, "%s == %" PRId64, expr, (int64_t)pat->int_lit.value);
        break;
    case PAT_BOOL_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "%s == %s", expr, pat->bool_lit.value ? "true" : "false");
        break;
    case PAT_CHAR_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "%s == '\\x%02x'", expr, pat->char_lit.value);
        break;
    case PAT_SOME: {
        if (!*first) fprintf(out, " && ");
        *first = false;
        bool is_ptr = is_null_sentinel(type);
        if (is_ptr) fprintf(out, "%s != NULL", expr);
        else fprintf(out, "%s.has_value", expr);
        if (pat->some_pat.inner) {
            const char *inner_expr = is_ptr ? expr : path_cat(expr, ".value", "");
            emit_pat_predicate(pat->some_pat.inner, inner_expr, type->option.inner, first, out);
        }
        break;
    }
    case PAT_NONE:
        if (!*first) fprintf(out, " && ");
        *first = false;
        if (is_null_sentinel(type))
            fprintf(out, "%s == NULL", expr);
        else
            fprintf(out, "!%s.has_value", expr);
        break;
    case PAT_OK: {
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "%s.err == 0", expr);
        if (pat->some_pat.inner) {
            const char *inner_expr = path_cat(expr, ".value", "");
            emit_pat_predicate(pat->some_pat.inner, inner_expr, type->result.inner, first, out);
        }
        break;
    }
    case PAT_ERR: {
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "%s.err != 0", expr);
        if (pat->some_pat.inner) {
            const char *inner_expr = path_cat(expr, ".err", "");
            emit_pat_predicate(pat->some_pat.inner, inner_expr, type_int32(), first, out);
        }
        break;
    }
    case PAT_VARIANT: {
        if (!*first) fprintf(out, " && ");
        *first = false;
        if (type->kind == TYPE_ENUM) {
            fprintf(out, "%s == ", expr);
            emit_enum_variant_literal(type, pat->variant.variant, out);
            break;
        }
        /* Derived names (the tag enum) hang off the union's C name, so it must
         * be the instance's, not the template's (see generic_instance_c_name). */
        const char *uname = aggregate_c_name(type);
        fprintf(out, "%s.tag == %s", expr,
                union_tag_value(uname, pat->variant.variant));
        if (pat->variant.payload) {
            const char *payload_expr = path_cat(expr, "." FC_PAYLOAD_MEMBER ".",
                c_safe_ident(g_intern, pat->variant.variant));
            Type *payload_type = NULL;
            for (int v = 0; v < type->unio.variant_count; v++) {
                if (type->unio.variants[v].name == pat->variant.variant) {
                    payload_type = type->unio.variants[v].payload;
                    break;
                }
            }
            if (payload_type)
                emit_pat_predicate(pat->variant.payload, payload_expr, payload_type, first, out);
        }
        break;
    }
    case PAT_STRUCT:
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            const char *path = path_cat(expr, ".",
                c_safe_ident(g_intern, pat->struc.fields[fi].name));
            emit_pat_predicate(pat->struc.fields[fi].pattern, path, pat->struc.fields[fi].resolved_type, first, out);
        }
        break;
    case PAT_STRING_LIT:
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "fc_eq_fc_str(%s, (fc_str){(uint8_t*)\"", expr);
        emit_str_lit_body(pat->string_lit.value, pat->string_lit.length, out);
        fprintf(out, "\", %d})",
            str_lit_len(pat->string_lit.value, pat->string_lit.length));
        break;
    case PAT_OR: {
        if (!pattern_has_predicate(pat)) break;
        if (!*first) fprintf(out, " && ");
        *first = false;
        fprintf(out, "(");
        for (int i = 0; i < pat->or_pat.alt_count; i++) {
            if (i > 0) fprintf(out, " || ");
            fprintf(out, "(");
            if (pattern_has_predicate(pat->or_pat.alts[i])) {
                bool alt_first = true;
                emit_pat_predicate(pat->or_pat.alts[i], expr, type, &alt_first, out);
            } else {
                fprintf(out, "1");
            }
            fprintf(out, ")");
        }
        fprintf(out, ")");
        break;
    }
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            char elem[16];
            snprintf(elem, sizeof(elem), "e%d", i);
            const char *path = path_cat(expr, ".", elem);
            emit_pat_predicate(pat->tuple_pat.patterns[i], path,
                pat->tuple_pat.resolved_types[i], first, out);
        }
        break;
    }
}

/* Recursively emit condition checks for any pattern.
   expr is the C expression for the value being matched.
   type is the FC type of the value.
   has_cond tracks whether "if (" has been emitted. */
static void emit_pat_conditions(Pattern *pat, const char *expr, Type *type, bool *has_cond, FILE *out) {
    if (!pattern_has_predicate(pat)) return;
    if (!*has_cond) {
        fprintf(out, "if (");
        *has_cond = true;
    } else {
        fprintf(out, " && ");
    }
    bool first = true;
    emit_pat_predicate(pat, expr, type, &first, out);
}

/* Recursively emit variable declarations for all bindings in a pattern.
   expr is the C expression for the value being matched.
   type is the FC type of the value. */
static void emit_pat_bindings(Pattern *pat, const char *expr, Type *type, FILE *out) {
    /* Same stub resolution as emit_pat_predicate: without it a nested union's
     * variant table is never found and its bindings are silently not declared. */
    if (type) type = resolve_struct_stub(type);
    switch (pat->kind) {
    case PAT_ERROR:      /* unreachable: error nodes never reach codegen */
    case PAT_CONST_PATH: /* unreachable: rewritten to PAT_INT_LIT in pass2 */
        break;
    case PAT_BINDING:
        emit_indent(out);
        if (is_hoisted(pat_binding_c_name(pat))) {
            fprintf(out, "%s = %s;\n", pat_binding_c_name(pat), expr);
        } else {
            emit_type(type, out);
            fprintf(out, " %s = %s;\n", pat_binding_c_name(pat), expr);
            emit_indent(out);
            fprintf(out, "(void)%s;\n", pat_binding_c_name(pat));
        }
        break;
    case PAT_WILDCARD:
    case PAT_INT_LIT:
    case PAT_BOOL_LIT:
    case PAT_CHAR_LIT:
    case PAT_NONE:
    case PAT_STRING_LIT:
        break;
    case PAT_SOME: {
        if (pat->some_pat.inner) {
            Type *inner_type = type->option.inner;
            const char *inner_expr = is_null_sentinel(type)
                ? expr : path_cat(expr, ".value", "");
            emit_pat_bindings(pat->some_pat.inner, inner_expr, inner_type, out);
        }
        break;
    }
    case PAT_OK: {
        if (pat->some_pat.inner) {
            const char *inner_expr = path_cat(expr, ".value", "");
            emit_pat_bindings(pat->some_pat.inner, inner_expr, type->result.inner, out);
        }
        break;
    }
    case PAT_ERR: {
        if (pat->some_pat.inner) {
            const char *inner_expr = path_cat(expr, ".err", "");
            emit_pat_bindings(pat->some_pat.inner, inner_expr, type_int32(), out);
        }
        break;
    }
    case PAT_VARIANT: {
        if (pat->variant.payload) {
            /* Get variant payload type from the mono table's concrete_type if available,
             * since the pass2 expression type may have unsubstituted type variables
             * in variant payloads (pass2 renames but doesn't substitute them). */
            Type *source_union = type;
            if (type->kind == TYPE_UNION && g_mono) {
                MonoInstance *mi = mono_find(g_mono, type->unio.name);
                if (mi && mi->concrete_type && mi->concrete_type->kind == TYPE_UNION)
                    source_union = mi->concrete_type;
            }
            Type *payload_type = NULL;
            for (int v = 0; v < source_union->unio.variant_count; v++) {
                if (source_union->unio.variants[v].name == pat->variant.variant) {
                    payload_type = source_union->unio.variants[v].payload;
                    break;
                }
            }
            /* Substitute type variables when inside a monomorphized context */
            if (payload_type && g_subst) {
                payload_type = type_substitute(g_arena, payload_type,
                    g_subst->var_names, g_subst->concrete, g_subst->count);
            }
            if (payload_type) {
                const char *payload_expr = path_cat(expr, "." FC_PAYLOAD_MEMBER ".",
                    c_safe_ident(g_intern, pat->variant.variant));
                emit_pat_bindings(pat->variant.payload, payload_expr, payload_type, out);
            }
        }
        break;
    }
    case PAT_STRUCT:
        for (int fi = 0; fi < pat->struc.field_count; fi++) {
            const char *path = path_cat(expr, ".",
                c_safe_ident(g_intern, pat->struc.fields[fi].name));
            emit_pat_bindings(pat->struc.fields[fi].pattern, path, pat->struc.fields[fi].resolved_type, out);
        }
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++) {
            char elem[16];
            snprintf(elem, sizeof(elem), "e%d", i);
            const char *path = path_cat(expr, ".", elem);
            emit_pat_bindings(pat->tuple_pat.patterns[i], path, pat->tuple_pat.resolved_types[i], out);
        }
        break;
    case PAT_OR:
        /* Or-pattern alternatives cannot bind (pass2 rejects it). */
        break;
    }
}

/* A type that materializes no C value: void, or never (return/break/continue).
   Used to decide whether an if/match needs a result temp and whether a tail
   expression should be wrapped in an implicit return. */
static bool type_valueless(Type *t) {
    return !t || t->kind == TYPE_VOID || t->kind == TYPE_NEVER;
}

/* `let { a, b } = expr` in statement position: bind the RHS to a temp, then let
 * the pattern emitter declare each name off it. Shared by every statement
 * context: a block body and a match arm's body both call it, because the arm
 * loop emits its statements itself rather than delegating to emit_block_stmts.
 * The caller has already emitted the leading indent. */
static void emit_let_destruct_stmt(Expr *s, FILE *out) {
    emit_type(s->let_destruct.init_type, out);
    fprintf(out, " %s = ", s->let_destruct.tmp_name);
    emit_expr(s->let_destruct.init, out);
    fprintf(out, ";\n");
    emit_pat_bindings(s->let_destruct.pattern, s->let_destruct.tmp_name,
                      s->let_destruct.init_type, out);
    emit_indent(out);
    fprintf(out, "(void)%s;\n", s->let_destruct.tmp_name);
}

/* `return value;` with pending defers, as statements: the value is computed
 * first, as the source orders it, then the defers run, then it is returned. */
static void emit_return_through_defers(Expr *value, FILE *out) {
    emit_type(value->type, out);
    int tid = g_temp_counter++;
    fprintf(out, " _ret%d = ", tid);
    emit_expr(value, out);
    fprintf(out, ";\n");
    emit_defers_to_func(out);
    emit_indent(out);
    fprintf(out, "return _ret%d;\n", tid);
}

static void emit_block_stmts(Expr **stmts, int count, FILE *out, bool as_return, bool discard_value) {
    /* Find last non-defer statement index (needed for block-value handling) */
    int last_real_idx = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (stmts[i]->kind != EXPR_DEFER) { last_real_idx = i; break; }
    }

    for (int i = 0; i < count; i++) {
        Expr *s = stmts[i];
        bool is_final_stmt = (i == count - 1);

        /* A defer is recorded here and emitted at scope exit */
        if (s->kind == EXPR_DEFER) {
            defer_scope_add(s->defer_expr.value);
            continue;
        }

        emit_indent(out);

        if (s->kind == EXPR_LET) {
            const char *vname = s->let_expr.codegen_name ? s->let_expr.codegen_name : s->let_expr.let_name;
            if (is_hoisted(vname)) {
                /* Hoisted: declaration already at function top, just assign */
                fprintf(out, "%s = ", vname);
            } else {
                emit_type(s->let_expr.let_type, out);
                fprintf(out, " %s = ", vname);
            }
            emit_expr(s->let_expr.let_init, out);
            fprintf(out, ";\n");
            emit_indent(out);
            fprintf(out, "(void)%s;\n", vname);
        } else if (s->kind == EXPR_LET_DESTRUCT) {
            emit_let_destruct_stmt(s, out);
        } else if (s->kind == EXPR_RETURN) {
            /* Emit defers before return */
            if (s->return_expr.value && has_pending_defers()) {
                emit_return_through_defers(s->return_expr.value, out);
            } else {
                if (has_pending_defers()) emit_defers_to_func(out);
                emit_indent(out);
                if (s->return_expr.value) {
                    fprintf(out, "return ");
                    emit_expr(s->return_expr.value, out);
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "return;\n");
                }
            }
        } else if (s->kind == EXPR_ASSIGN) {
            emit_expr(s, out);
            fprintf(out, ";\n");
        } else if (s->kind == EXPR_BREAK) {
            /* Emit defers before break */
            if (s->break_expr.value && has_pending_defers()) {
                emit_type(s->break_expr.value->type, out);
                int tid = g_temp_counter++;
                fprintf(out, " _brk%d = ", tid);
                emit_expr(s->break_expr.value, out);
                fprintf(out, ";\n");
                emit_defers_to_loop(out);
                emit_indent(out);
                fprintf(out, "_loop_result = _brk%d; break;\n", tid);
            } else {
                if (has_pending_defers()) emit_defers_to_loop(out);
                emit_indent(out);
                if (s->break_expr.value) {
                    fprintf(out, "_loop_result = ");
                    emit_expr(s->break_expr.value, out);
                    fprintf(out, "; break;\n");
                } else {
                    fprintf(out, "break;\n");
                }
            }
        } else if (s->kind == EXPR_CONTINUE) {
            if (has_pending_defers()) emit_defers_to_loop(out);
            emit_indent(out);
            fprintf(out, "continue;\n");
        } else if (s->kind == EXPR_IF && type_valueless(s->type)) {
            /* void- or never-typed if: emit as a C if statement */
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (s->kind == EXPR_FOR) {
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (s->kind == EXPR_LOOP && type_valueless(s->type)) {
            emit_expr(s, out);
            fprintf(out, "\n");
        } else if (is_final_stmt && as_return && s->type && !type_valueless(s->type)) {
            /* Implicit return: emit defers before returning */
            if (has_pending_defers()) {
                emit_return_through_defers(s, out);
            } else {
                fprintf(out, "return ");
                emit_expr(s, out);
                fprintf(out, ";\n");
            }
        } else if (!as_return && i == last_real_idx && s->type &&
                   s->type->kind != TYPE_VOID &&
                   g_defer_scope && g_defer_scope->count > 0) {
            /* Last expression in a value-producing block with pending defers
             * in the current scope. Save to temp, emit defers, then produce
             * temp as block value. Only triggers when the current scope itself
             * has defers; parent scope defers are handled at their own level. */
            emit_type(s->type, out);
            int tid = g_temp_counter++;
            fprintf(out, " _blk%d = ", tid);
            emit_expr(s, out);
            fprintf(out, ";\n");
            emit_scope_defers(g_defer_scope, out);
            emit_indent(out);
            if (discard_value)
                fprintf(out, "(void)_blk%d;\n", tid);
            else
                fprintf(out, "_blk%d;\n", tid);
        } else {
            /* Cast non-void expressions to (void) when their value is
             * discarded (non-last statement, or last with discard_value).
             * Prevents -Wunused-value for GCC statement expressions like
             * non-void match/block used as statements. */
            bool is_block_value = (i == last_real_idx);
            bool void_cast = s->type && s->type->kind != TYPE_VOID &&
                             (!is_block_value || discard_value);
            if (void_cast) fprintf(out, "(void)(");
            emit_expr(s, out);
            if (void_cast) fprintf(out, ");\n");
            else fprintf(out, ";\n");
        }
    }
    /* Emit end-of-block defers for fall-through.
     * Skip when already handled: as_return with non-void last expr (defers
     * emitted with the return), or non-void block value (handled above). */
    if (g_defer_scope && g_defer_scope->count > 0) {
        bool already_handled = false;
        if (as_return && last_real_idx >= 0) {
            Expr *last_real = stmts[last_real_idx];
            already_handled = last_real->type &&
                last_real->type->kind != TYPE_VOID &&
                last_real->kind != EXPR_RETURN;
        }
        if (!as_return && last_real_idx >= 0) {
            Expr *last_real = stmts[last_real_idx];
            already_handled = last_real->type &&
                last_real->type->kind != TYPE_VOID;
        }
        if (!already_handled) {
            emit_scope_defers(g_defer_scope, out);
        }
    }
}

static void emit_if_stmt(Expr *e, FILE *out) {
    /* Emit if as a C statement (not expression).
     * Binary expressions emit their own outer parens, so use "if "
     * to avoid double-parens like if ((x == y)) which triggers
     * clang's -Wparentheses-equality. */
    if (emit_self_parens(e->if_expr.cond)) {
        fprintf(out, "if ");
        emit_expr(e->if_expr.cond, out);
        fprintf(out, " {\n");
    } else {
        fprintf(out, "if (");
        emit_expr(e->if_expr.cond, out);
        fprintf(out, ") {\n");
    }
    g_indent_level++;
    if (e->if_expr.then_body->kind == EXPR_BLOCK) {
        defer_scope_push(false);
        emit_block_stmts(e->if_expr.then_body->block.stmts,
            e->if_expr.then_body->block.count, out, false, true);
        defer_scope_pop();
    } else {
        emit_indent(out);
        emit_expr(e->if_expr.then_body, out);
        fprintf(out, ";\n");
    }
    g_indent_level--;
    emit_indent(out);
    fprintf(out, "}");
    if (e->if_expr.else_body) {
        if (e->if_expr.else_body->kind == EXPR_IF &&
            type_valueless(e->if_expr.else_body->type)) {
            fprintf(out, " else ");
            emit_if_stmt(e->if_expr.else_body, out);
        } else {
            fprintf(out, " else {\n");
            g_indent_level++;
            if (e->if_expr.else_body->kind == EXPR_BLOCK) {
                defer_scope_push(false);
                emit_block_stmts(e->if_expr.else_body->block.stmts,
                    e->if_expr.else_body->block.count, out, false, true);
                defer_scope_pop();
            } else {
                emit_indent(out);
                emit_expr(e->if_expr.else_body, out);
                fprintf(out, ";\n");
            }
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        }
    }
}

/* Emit one if-branch (a single expression, possibly an EXPR_BLOCK) inside a
   statement-expression. When the branch diverges (`never`: its tail is
   return/break/continue or an all-diverging if/match) it is emitted as plain
   statements: the control-flow exit fires and nothing is assigned. Otherwise
   the branch's value is assigned to res_var. */
static void emit_branch_into(Expr *branch, const char *res_var, FILE *out) {
    if (type_is_never(branch->type)) {
        if (branch->kind == EXPR_BLOCK) {
            defer_scope_push(false);
            emit_block_stmts(branch->block.stmts, branch->block.count, out, false, true);
            defer_scope_pop();
        } else {
            emit_indent(out);
            emit_expr(branch, out);
            fprintf(out, ";\n");
        }
    } else {
        emit_indent(out);
        fprintf(out, "%s = ", res_var);
        emit_expr(branch, out);
        fprintf(out, ";\n");
    }
}

/* The C unsigned type of an integer type's width */
static const char *unsigned_counterpart(Type *t) {
    switch (t->kind) {
    case TYPE_INT8:  case TYPE_UINT8:  return "uint8_t";
    case TYPE_INT16: case TYPE_UINT16: return "uint16_t";
    case TYPE_INT32: case TYPE_UINT32: return "uint32_t";
    case TYPE_INT64: case TYPE_UINT64: return "uint64_t";
    case TYPE_ISIZE: case TYPE_USIZE:  return "size_t";
    default: return NULL;
    }
}

/* Sub-int integer types (int8/uint8/int16/uint16). C's integer-promotion rules
 * widen these to `int` in arithmetic, so a plain `a + b` yields an un-truncated
 * value (200u8 + 100u8 observes as 300, not 44) and narrow signed multiply hits
 * signed-overflow UB (-1i16 * -1i16 promotes to int and 65535*65535 overflows).
 * Such results must be routed through `unsigned` and/or truncated back to the
 * FC type. Types at least as wide as int (int32+/uint32+) don't need this. */
static bool type_is_subint(Type *t) {
    return t->kind == TYPE_INT8  || t->kind == TYPE_UINT8 ||
           t->kind == TYPE_INT16 || t->kind == TYPE_UINT16;
}

/* Shift mask for an integer type's bit width.
 * Returns -1 for platform-dependent types (isize/usize); the caller handles those. */
static int shift_mask_for(Type *t) {
    switch (t->kind) {
    case TYPE_INT8:  case TYPE_UINT8:  return 7;
    case TYPE_INT16: case TYPE_UINT16: return 15;
    case TYPE_INT32: case TYPE_UINT32: return 31;
    case TYPE_INT64: case TYPE_UINT64: return 63;
    case TYPE_ISIZE: case TYPE_USIZE:  return -1;
    default: return 0;
    }
}


/* Emit interpolated string code.
 * alloc_opt_type: NULL for a stack buffer, non-NULL inside alloc() (malloc).
 * Handles both str and cstr interpolation (e->interp_string.is_cstr). */
static void emit_interp_string(Expr *e, FILE *out, Type *alloc_opt_type) {
    bool use_heap = (alloc_opt_type != NULL);
    bool is_cstr = e->interp_string.is_cstr;
    int tid = g_temp_counter++;
    int seg_count = e->interp_string.segment_count;
    InterpSegment *segs = e->interp_string.segments;

    fprintf(out, "({ ");

    /* Pre-evaluate every non-literal, non-%T segment into a temp, in textual
     * order, so each interpolated expression is evaluated once and in source
     * order. snprintf evaluates its arguments in unspecified order, and a cstr
     * %s would otherwise be evaluated twice (for strlen and as the argument).
     * %T is excluded: its operand is never evaluated, only its static type
     * name is used. */
    for (int i = 0, k = 0; i < seg_count; i++) {
        if (segs[i].is_literal || segs[i].conversion == 'T') continue;
        emit_type(segs[i].expr->type, out);
        fprintf(out, " _sg%d_%d = ", tid, k);
        emit_expr(segs[i].expr, out);
        fprintf(out, "; ");
        k++;
    }

    /* Under `checked`, a precision-bounded %s segment aborts instead of
     * clipping, since the clip is data loss on the overflow axis. pass2's
     * expr_node_is_governed_overflow (EXPR_INTERP_STRING arm) must agree.
     * A str segment reads its fat-pointer length; a cstr segment probes the
     * first prec+1 bytes for the NUL (a bounded scan, no full strlen on the
     * hot path) and pays the strlen only in the aborting branch. */
    if (g_overflow_checked) {
        for (int i = 0, k = 0; i < seg_count; i++) {
            if (segs[i].is_literal || segs[i].conversion == 'T') continue;
            int prec = interp_seg_trunc_prec(&segs[i]);
            if (prec >= 0) {
                SrcLoc sl = segs[i].expr->loc;
                Type *st = segs[i].expr->type;
                if (st && is_str_type(st)) {
                    fprintf(out, "if (_sg%d_%d.len > (int64_t)%d) fc_trunc(", tid, k, prec);
                    emit_loc_args(sl, out);
                    fprintf(out, ", \"%%.%ds segment\", (long long)_sg%d_%d.len, %d); ",
                            prec, tid, k, prec);
                } else {
                    fprintf(out, "if (!memchr(_sg%d_%d, 0, %d)) fc_trunc(", tid, k, prec + 1);
                    emit_loc_args(sl, out);
                    fprintf(out, ", \"%%.%ds segment\", (long long)strlen((const char*)_sg%d_%d), %d); ",
                            prec, tid, k, prec);
                }
            }
            k++;
        }
    }

    /* Compute the buffer size expression. The buffer must be large enough: a
     * printf field width is a minimum, never a maximum, so it can only widen a
     * bound. String precision is the one true maximum. */
    fprintf(out, "int64_t _flen%d = ", tid);
    bool first_term = true;
    int k = 0;
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) {
            int actual_len = interp_literal_len(&segs[i]);
            if (actual_len > 0) {
                if (!first_term) fprintf(out, " + ");
                fprintf(out, "%d", actual_len);
                first_term = false;
            }
            continue;
        }
        if (segs[i].conversion == 'T') {
            /* %T contributes the fixed length of the compile-time type name. */
            const char *tname = type_name(segs[i].expr->type);
            if (!first_term) fprintf(out, " + ");
            fprintf(out, "%d", (int)strlen(tname));
            first_term = false;
            continue;
        }

        char conv = segs[i].conversion;
        int64_t explicit_width = 0, explicit_prec = -1;
        parse_format_width_prec(segs[i].text, &explicit_width, &explicit_prec);
        Type *t = segs[i].expr->type;
        bool is_str_arg = (conv == 's' && t && is_str_type(t));
        bool is_cstr_arg = (conv == 's' && t && is_cstr_type(t));

        if (!first_term) fprintf(out, " + ");
        first_term = false;

        if (is_str_arg || is_cstr_arg) {
            /* String-valued: precision is a hard maximum; width is a minimum
             * field. With a precision the bound is the constant max(prec,
             * width); otherwise it is the runtime length, floored at the
             * minimum field width. */
            if (explicit_prec >= 0) {
                int64_t b = explicit_prec;
                if (explicit_width > b) b = explicit_width;
                fprintf(out, "%" PRId64, b);
            } else if (is_str_arg) {
                if (explicit_width > 0)
                    fprintf(out, "(%" PRId64 " > _sg%d_%d.len ? %" PRId64 " : _sg%d_%d.len)",
                        explicit_width, tid, k, explicit_width, tid, k);
                else
                    fprintf(out, "_sg%d_%d.len", tid, k);
            } else {
                if (explicit_width > 0)
                    fprintf(out, "((int64_t)%" PRId64 " > (int64_t)strlen((const char*)_sg%d_%d)"
                                 " ? (int64_t)%" PRId64 " : (int64_t)strlen((const char*)_sg%d_%d))",
                        explicit_width, tid, k, explicit_width, tid, k);
                else
                    fprintf(out, "(int64_t)strlen((const char*)_sg%d_%d)", tid, k);
            }
        } else {
            int64_t bound = interp_numeric_bound(conv, t, segs[i].text,
                                                 explicit_width, explicit_prec);
            fprintf(out, "%" PRId64, bound);
        }
        k++;
    }
    if (first_term) fprintf(out, "0");
    fprintf(out, "; ");

    /* Allocate the buffer.  A constant-size buffer points at the fixed array
     * hoisted to function entry (codegen_backing_name), whose slot is reused on
     * every loop iteration, so stack use stays bounded.  A runtime-sized buffer
     * (a %s/cstr without an explicit precision) is alloca'd afresh on each
     * evaluation, which grows the frame per loop iteration (a documented cost;
     * alloc(s)! promotes to the heap), or malloc'd under alloc(s)!. */
    if (g_len_repr < 64) {
        /* The result's len is bounded by _flen; cap _flen once, before the
         * buffer exists, so the stores below fit fc_len_t. */
        fprintf(out, "if (__builtin_expect(_flen%d > FC_LEN_MAX, 0)) fc_len_cap(", tid);
        emit_loc_args(e->loc, out);
        fprintf(out, ", (long long)_flen%d); ", tid);
    }
    if (use_heap)
        fprintf(out, "uint8_t *_fbuf%d = (uint8_t*)malloc(fc_to_size(_flen%d + 1)); ", tid, tid);
    else if (e->interp_string.codegen_backing_name)
        fprintf(out, "uint8_t *_fbuf%d = %s; ", tid, e->interp_string.codegen_backing_name);
    else
        fprintf(out, "uint8_t *_fbuf%d = (uint8_t*)__builtin_alloca(fc_to_size(_flen%d + 1)); ", tid, tid);

    /* Build snprintf call */
    if (use_heap)
        fprintf(out, "int _fw%d = _fbuf%d ? snprintf((char*)_fbuf%d, fc_to_size(_flen%d + 1), \"",
            tid, tid, tid, tid);
    else
        fprintf(out, "int _fw%d = snprintf((char*)_fbuf%d, fc_to_size(_flen%d + 1), \"",
            tid, tid, tid);

    /* Emit format string */
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) {
            /* The segment's decoded bytes ride the format string itself,
             * except a NUL byte, which would terminate the format and drop
             * every later segment (and trip -Werror=format-contains-nul).  A
             * NUL is written as a `%c` whose argument is 0, which snprintf
             * copies into the buffer like any other byte and counts in its
             * return. */
            int n = interp_literal_len(&segs[i]);
            unsigned char *bytes = malloc(n > 0 ? (size_t)n : 1);
            decode_str_lit(segs[i].text, segs[i].text_length, bytes);
            for (int j = 0; j < n; j++) {
                if (bytes[j] == 0) fputs("%c", out);
                else emit_c_byte(bytes[j], true, out);
            }
            free(bytes);
        } else if (segs[i].conversion == 'T') {
            const char *tname = type_name(segs[i].expr->type);
            fputs(tname, out);
        } else {
            bool is_str_arg2 = (segs[i].conversion == 's' &&
                               segs[i].expr->type && is_str_type(segs[i].expr->type));
            if (is_str_arg2) {
                fprintf(out, "%%");
                const char *sp = segs[i].text;
                int splen = segs[i].text_length;
                int j = 0;
                while (j < splen - 1 && (sp[j] == '-' || sp[j] == '+' ||
                       sp[j] == '0' || sp[j] == '#' || sp[j] == ' ')) {
                    fputc(sp[j], out);
                    j++;
                }
                while (j < splen - 1 && sp[j] >= '0' && sp[j] <= '9') {
                    fputc(sp[j], out);
                    j++;
                }
                if (j < splen - 1 && sp[j] == '.') {
                    j++;
                    while (j < splen - 1 && sp[j] >= '0' && sp[j] <= '9') j++;
                }
                fprintf(out, ".*s");
            } else {
                Type *t = segs[i].expr->type;
                char conv = segs[i].conversion;
                /* Every integer-number conversion is passed as (unsigned) long
                 * long (see the argument pass), so it always takes the `ll`
                 * length modifier, independent of the target's `int` width.
                 * %c (char/uint8) is passed as int and keeps no modifier. */
                bool int_number = t && type_is_integer(t) &&
                    (conv == 'd' || conv == 'i' || conv == 'u' ||
                     conv == 'x' || conv == 'X' || conv == 'o');
                const char *sp = segs[i].text;
                int splen = segs[i].text_length;
                fprintf(out, "%%");
                if (int_number) {
                    for (int j = 0; j < splen - 1; j++) fputc(sp[j], out);
                    /* `%d`/`%i` on an unsigned operand emits as `%llu` (see
                     * interp_conv_is_unsigned); the argument is cast to match. */
                    char cc = sp[splen - 1];
                    if ((cc == 'd' || cc == 'i') && interp_conv_is_unsigned(cc, t))
                        cc = 'u';
                    fprintf(out, "ll%c", cc);
                } else {
                    fwrite(sp, 1, (size_t)splen, out);
                }
            }
        }
    }
    fprintf(out, "\"");

    /* Emit arguments.  Each reads its pre-evaluated temp (_sg<tid>_<k>), so
     * each expression is evaluated once and in source order. */
    int ak = 0;
    for (int i = 0; i < seg_count; i++) {
        if (segs[i].is_literal) {
            /* One `0` per NUL byte the format writes with `%c` (see above). */
            int n = interp_literal_len(&segs[i]);
            unsigned char *bytes = malloc(n > 0 ? (size_t)n : 1);
            decode_str_lit(segs[i].text, segs[i].text_length, bytes);
            for (int j = 0; j < n; j++)
                if (bytes[j] == 0) fprintf(out, ", 0");
            free(bytes);
            continue;
        }
        if (segs[i].conversion == 'T') continue;
        fprintf(out, ", ");
        char conv = segs[i].conversion;
        Type *t = segs[i].expr->type;
        bool is_str_arg2 = (conv == 's' && t && is_str_type(t));
        if (is_str_arg2) {
            int64_t prec_w = 0, prec_p = -1;
            parse_format_width_prec(segs[i].text, &prec_w, &prec_p);
            if (prec_p >= 0) {
                /* Compare in int64 to avoid truncating before the min; the
                 * fc_to_int branch is only taken when len < prec_p, so the
                 * narrowing assert is trivially satisfied. */
                fprintf(out, "(_sg%d_%d.len < (int64_t)%" PRId64 " ? fc_to_int(_sg%d_%d.len)"
                             " : %" PRId64 "), _sg%d_%d.ptr",
                    tid, ak, prec_p, tid, ak, prec_p, tid, ak);
            } else {
                fprintf(out, "fc_to_int(_sg%d_%d.len), _sg%d_%d.ptr",
                    tid, ak, tid, ak);
            }
        } else {
            if (t && type_is_integer(t)) {
                if (interp_conv_is_unsigned(conv, t)) {
                    /* Unsigned conversions print the operand's bit pattern.
                     * First reinterpret it at its own width via the unsigned
                     * counterpart: casting a signed narrow operand straight to
                     * a wider unsigned type sign-extends it (-16i8 would
                     * become 0xFFFFFFF0, formatted "fffffff0", overrunning the
                     * 2-digit budget). Then widen to unsigned long long so the
                     * `ll` length modifier is correct on every target: a plain
                     * (unsigned int) is only 16 bits where C's `int` is, which
                     * would truncate a uint32 value. */
                    fprintf(out, "(unsigned long long)(%s)", unsigned_counterpart(t));
                } else if (conv == 'c') {
                    /* %c (uint8 operand) takes an int argument, no modifier. */
                    fprintf(out, "(int)");
                } else {
                    /* Signed decimal: widen to long long (matches the `ll`
                     * modifier) so int32 is not truncated on a 16-bit-`int`
                     * target, where int32_t is `long`, not `int`. */
                    fprintf(out, "(long long)");
                }
            } else if (t && t->kind == TYPE_FLOAT32) {
                fprintf(out, "(double)");
            } else if (t && is_cstr_type(t)) {
                if (conv == 'p')
                    fprintf(out, "(void*)");
                else
                    fprintf(out, "(const char*)");
            } else if (t && (t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR)) {
                fprintf(out, "(void*)");
            }
            fprintf(out, "_sg%d_%d", tid, ak);
        }
        ak++;
    }
    fprintf(out, ")");
    if (use_heap) fprintf(out, " : 0");  /* close the _fbuf ? snprintf(...) : 0 ternary */
    fprintf(out, "; ");
    if (is_cstr) fprintf(out, "(void)_fw%d; ", tid);  /* suppress -Wunused-variable for cstr paths */

    /* Construct result */
    if (use_heap && is_cstr) {
        /* cstr? = bare pointer (null sentinel) */
        fprintf(out, "_fbuf%d; })", tid);
    } else if (use_heap) {
        /* str? = fc_option_fc_str */
        fprintf(out, "_fbuf%d ? (", tid);
        emit_type(alloc_opt_type, out);
        fprintf(out, "){ .value = (fc_str){ .ptr = _fbuf%d, .len = _fw%d >= 0 && _fw%d <= _flen%d ? _fw%d : _flen%d }, .has_value = true } : (",
            tid, tid, tid, tid, tid, tid);
        emit_type(alloc_opt_type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else if (is_cstr) {
        /* standalone cstr = bare pointer */
        fprintf(out, "_fbuf%d; })", tid);
    } else {
        /* standalone str = fc_str */
        fprintf(out, "(fc_str){ .ptr = _fbuf%d, .len = _fw%d >= 0 && _fw%d <= _flen%d ? _fw%d : _flen%d }; })",
            tid, tid, tid, tid, tid, tid);
    }
}

/* Emit already-raw bytes (a file name, the source text of an asserted
 * expression) as a C string literal body.  These land in printf format strings,
 * so `%` doubles; the rest of the encoding is emit_c_byte's, notably `\?`, so
 * source text like `i32??` cannot be re-read as a trigraph. */
static void emit_c_escaped(const char *text, int len, FILE *out) {
    for (int i = 0; i < len; i++)
        emit_c_byte((unsigned char)text[i], true, out);
}

/* A source location as the first two arguments of a runtime check: the file as
 * a C string literal, then the line. */
static void emit_loc_args(SrcLoc loc, FILE *out) {
    const char *file = loc.filename ? loc.filename : "<unknown>";
    fputc('"', out);
    emit_c_escaped(file, (int)strlen(file), out);
    fprintf(out, "\", %d", loc.line);
}

/* "file:line" inside a C string literal that is already open. */
static void emit_loc_text(SrcLoc loc, FILE *out) {
    const char *file = loc.filename ? loc.filename : "<unknown>";
    emit_c_escaped(file, (int)strlen(file), out);
    fprintf(out, ":%d", loc.line);
}

/* Per-integer-type data for FC's saturating float->int conversion: NaN -> 0,
 * clamp to [min,max] at the range edge, else truncate toward zero. Both the
 * prelude's runtime helpers `fc_f2*` and float_to_int_emit's constant-expression
 * ternary are generated from this table. The guard constants are the
 * power-of-two boundaries just outside each type's range (all representable as
 * double), so the inner cast never sees an unrepresentable value. `lo`/`hi` are
 * the double-typed saturation guards; `imin`/`imax` the integer results
 * at/beyond them. Returns false if `k` is not an integer type. */
typedef struct { const char *fn, *cty, *lo, *imin, *hi, *imax; } F2iInfo;

static bool float_to_int_info(TypeKind k, F2iInfo *o) {
    switch (k) {
    case TYPE_INT8:   *o = (F2iInfo){"fc_f2i8",  "int8_t",  "-128.0",                 "INT8_MIN",  "128.0",                 "INT8_MAX"};  return true;
    case TYPE_INT16:  *o = (F2iInfo){"fc_f2i16", "int16_t", "-32768.0",               "INT16_MIN", "32768.0",               "INT16_MAX"}; return true;
    case TYPE_INT32:  *o = (F2iInfo){"fc_f2i32", "int32_t", "-2147483648.0",          "INT32_MIN", "2147483648.0",          "INT32_MAX"}; return true;
    case TYPE_INT64:  *o = (F2iInfo){"fc_f2i64", "int64_t", "-9223372036854775808.0", "INT64_MIN", "9223372036854775808.0", "INT64_MAX"}; return true;
    case TYPE_UINT8:  *o = (F2iInfo){"fc_f2u8",  "uint8_t",  "0.0", "0", "256.0",                  "UINT8_MAX"};  return true;
    case TYPE_UINT16: *o = (F2iInfo){"fc_f2u16", "uint16_t", "0.0", "0", "65536.0",                "UINT16_MAX"}; return true;
    case TYPE_UINT32: *o = (F2iInfo){"fc_f2u32", "uint32_t", "0.0", "0", "4294967296.0",           "UINT32_MAX"}; return true;
    case TYPE_UINT64: *o = (F2iInfo){"fc_f2u64", "uint64_t", "0.0", "0", "18446744073709551616.0", "UINT64_MAX"}; return true;
    case TYPE_ISIZE:  *o = (F2iInfo){"fc_f2isize", "ptrdiff_t", "(double)PTRDIFF_MIN", "PTRDIFF_MIN", "-(double)PTRDIFF_MIN", "PTRDIFF_MAX"}; return true;
    case TYPE_USIZE:  *o = (F2iInfo){"fc_f2usize", "size_t", "0.0", "0", "(double)SIZE_MAX", "SIZE_MAX"}; return true;
    default: return false;
    }
}

/* Emit a saturating float->int conversion of `operand` to integer type `info`.
 * At runtime, call the single-evaluation helper. In const context (a C
 * file-scope initializer, where a function call is not a constant expression)
 * emit the equivalent ternary directly. The operand is a pure constant there,
 * so evaluating it up to four times is harmless, and leaving the float
 * arithmetic to the C compiler keeps it target-correct (FC does not fold
 * floats). */
static void float_to_int_emit(const F2iInfo *info, Expr *operand, FILE *out) {
    if (!g_const_context) {
        fprintf(out, "%s(", info->fn);
        emit_expr(operand, out);
        fprintf(out, ")");
        return;
    }
    fprintf(out, "((");
    emit_expr(operand, out);
    fprintf(out, ") != (");
    emit_expr(operand, out);
    fprintf(out, ") ? 0 : ((");
    emit_expr(operand, out);
    fprintf(out, ") < %s ? %s : ((", info->lo, info->imin);
    emit_expr(operand, out);
    fprintf(out, ") >= %s ? %s : (%s)(", info->hi, info->imax, info->cty);
    emit_expr(operand, out);
    fprintf(out, "))))");
}

static bool cast_is_ptr_kind(Type *t) {
    return t && (t->kind == TYPE_POINTER || t->kind == TYPE_ANY_PTR);
}

/* Emit a `checked` integer->integer narrowing cast of `operand` (type `from`)
 * to `to`: abort if the value falls outside the target's range, else the plain
 * cast. The condition is built per signedness so it stays -Wsign-compare-clean
 * and width-agnostic (bounds come from type_property_c; the unsigned
 * domain handles same-width sign changes and the int64<->uint64 boundary). */
static void emit_checked_int_narrow(Type *from, Type *to, Expr *operand,
                                    SrcLoc loc, FILE *out) {
    int tid = g_temp_counter++;
    const char *uf = unsigned_counterpart(from);
    const char *tmin = type_property_c(to, "min");
    const char *tmax = type_property_c(to, "max");
    fprintf(out, "({ ");
    emit_type(from, out);
    fprintf(out, " _cv%d = ", tid);
    emit_expr(operand, out);
    fprintf(out, "; if (");
    if (type_is_signed(from)) {
        if (type_is_signed(to))
            fprintf(out, "_cv%d < %s || _cv%d > %s", tid, tmin, tid, tmax);
        else   /* signed -> unsigned: negatives lost; upper bound in unsigned domain */
            fprintf(out, "_cv%d < 0 || (%s)_cv%d > (%s)%s", tid, uf, tid, uf, tmax);
    } else {
        /* unsigned source (>= 0): only the upper bound, in the unsigned domain */
        fprintf(out, "_cv%d > (%s)%s", tid, uf, tmax);
    }
    fprintf(out, ") fc_overflow(");
    emit_loc_args(loc, out);
    fprintf(out, ", \"cast\"); (");
    emit_type(to, out);
    fprintf(out, ")_cv%d; })", tid);
}

/* Emit the `none` value of an option type: NULL for a pointer-packed option,
 * a has_value=false compound literal otherwise. Used by x? propagation to
 * build the early-return failure at the enclosing function's return type. */
static void emit_none_of_type(Type *opt, FILE *out) {
    if (is_null_sentinel(opt)) {
        fprintf(out, "NULL");
    } else {
        fprintf(out, "(");
        emit_type(opt, out);
        fprintf(out, "){ .has_value = false }");
    }
}

/* An integer literal: decimal digits, or hexadecimal when `hex` (a direct
 * operand of `^`, see the binary emitter). A negative value keeps its decimal
 * spelling: hex digits of a negative literal would not denote the same value. */
static void emit_int_lit(Expr *e, bool hex, FILE *out) {
    char num[32];
    hex = hex && (int64_t)e->int_lit.value >= 0;
    if (hex)
        snprintf(num, sizeof num, "0x%" PRIx64, e->int_lit.value);
    else if (e->int_lit.lit_type->kind == TYPE_UINT64 ||
             e->int_lit.lit_type->kind == TYPE_USIZE)
        snprintf(num, sizeof num, "%" PRIu64, e->int_lit.value);
    else
        snprintf(num, sizeof num, "%" PRId64, (int64_t)e->int_lit.value);

    if (e->int_lit.lit_type->kind == TYPE_INT64) {
        /* INT64_MIN cannot be written as a single literal (its magnitude
         * exceeds INT64_MAX); emit the portable (-MAX - 1) idiom. */
        if ((int64_t)e->int_lit.value == INT64_MIN)
            fprintf(out, "(-9223372036854775807LL - 1)");
        else
            fprintf(out, "INT64_C(%s)", num);
    }
    else if (e->int_lit.lit_type->kind == TYPE_UINT64)
        fprintf(out, "UINT64_C(%s)", num);
    else if (e->int_lit.lit_type->kind == TYPE_ISIZE)
        fprintf(out, "((ptrdiff_t)%sLL)", num);
    else if (e->int_lit.lit_type->kind == TYPE_USIZE)
        fprintf(out, "((size_t)%sULL)", num);
    else
        fprintf(out, "%s", num);
}

/* The start of an unwrap failure report for `e` (a postfix `!`): the call
 * fprintf(stderr, "file:line: unwrap failed: <operand source>, with the string
 * literal still open for the caller to finish. */
static void emit_unwrap_failure_open(Expr *e, FILE *out) {
    fprintf(out, "fprintf(stderr, \"");
    emit_loc_text(e->loc, out);
    fprintf(out, ": unwrap failed: ");
    if (e->unary_postfix.expr_text)
        emit_c_escaped(e->unary_postfix.expr_text, e->unary_postfix.expr_text_len, out);
}

static void emit_expr(Expr *e, FILE *out);

/* An operand of a binary operator; a literal operand of `^` goes out in hex. */
static void emit_binary_operand(Expr *operand, bool is_xor, FILE *out) {
    if (is_xor && operand->kind == EXPR_INT_LIT)
        emit_int_lit(operand, true, out);
    else
        emit_expr(operand, out);
}

/* Integer +, -, *: overflow is defined as modular wraparound.
 *
 * Sub-int result types (int8/uint8/int16/uint16) promote to `int`, which
 * leaves the value un-truncated in the surrounding expression
 * (200u8 + 100u8 would observe as 300, not 44) and causes signed-overflow
 * UB on multiply (-1i16 * -1i16 promotes to int and 65535*65535 overflows
 * int). Route through `unsigned` so the math is modular regardless of
 * int's width, then truncate back to the FC type:
 *   (int16_t)(uint16_t)((unsigned)(uint16_t)a * (unsigned)(uint16_t)b)
 *
 * Signed result types at least as wide as int (int32/int64/isize) cast
 * through the unsigned counterpart so overflow wraps instead of being UB:
 *   (int32_t)((uint32_t)a + (uint32_t)b)
 *
 * Unsigned result types at least as wide as int (uint32/uint64/usize) are
 * already modular in C: returns false, and the caller emits the plain
 * operator. */
static bool emit_int_arith(Expr *e, int op, Type *rt, FILE *out) {
    const char *op_c = op == TOK_PLUS ? "+" : op == TOK_MINUS ? "-" : "*";
    /* `checked`: trap overflow (signed and unsigned) instead of wrapping.
     * __builtin_<op>_overflow computes in infinite precision and reports
     * whether the result fits _r's type (rt), which is correct for any `int`
     * width, 16-bit included. */
    if (g_overflow_checked) {
        const char *bi = op == TOK_PLUS ? "add" : op == TOK_MINUS ? "sub" : "mul";
        int tid = g_temp_counter++;
        fprintf(out, "({ ");
        emit_type(rt, out);
        fprintf(out, " _r%d; if (__builtin_%s_overflow(", tid, bi);
        emit_expr(e->binary.left, out);
        fprintf(out, ", ");
        emit_expr(e->binary.right, out);
        fprintf(out, ", &_r%d)) fc_overflow(", tid);
        emit_loc_args(e->loc, out);
        fprintf(out, ", \"%s\"); _r%d; })", op_c, tid);
        return true;
    }
    if (type_is_subint(rt)) {
        const char *ut = unsigned_counterpart(rt);
        fprintf(out, "(");
        emit_type(rt, out);
        fprintf(out, ")");
        /* well-defined truncation before reinterpreting as signed */
        if (type_is_signed(rt)) fprintf(out, "(%s)", ut);
        fprintf(out, "((unsigned)(%s)", ut);
        emit_expr(e->binary.left, out);
        fprintf(out, " %s (unsigned)(%s)", op_c, ut);
        emit_expr(e->binary.right, out);
        fprintf(out, ")");
        return true;
    }
    if (type_is_signed(rt)) {
        const char *ut = unsigned_counterpart(rt);
        fprintf(out, "(");
        emit_type(rt, out);
        fprintf(out, ")(((%s)", ut);
        emit_expr(e->binary.left, out);
        fprintf(out, ") %s ((%s)", op_c, ut);
        emit_expr(e->binary.right, out);
        fprintf(out, "))");
        return true;
    }
    return false;
}

/* A shift, with its count masked to the operand width (a << (b & 31) for a
 * 32-bit operand), so an out-of-range count is not undefined behavior. */
static bool emit_shift(Expr *e, int op, Type *rt, FILE *out) {
    int mask = shift_mask_for(rt);
    /* For platform-dependent types, emit a computed mask */
    const char *mask_expr = NULL;
    char mask_buf[64];
    if (mask < 0) {
        snprintf(mask_buf, sizeof(mask_buf), "((int)(sizeof(size_t)*8)-1)");
        mask_expr = mask_buf;
    }
    if (op == TOK_LTLT) {
        /* Left shift through the unsigned counterpart, then cast back to
         * the result type: makes signed shift well-defined (no overflow
         * UB) and truncates narrow results that would otherwise promote
         * to int (200u8 << 1 observes as 144, not 400). Wide unsigned
         * types go through the same form: the cast changes nothing for a
         * variable but gives an unsigned literal left operand its type.
         * `1u32 << 31` must be `(uint32_t)1 << 31`, not the bare
         * `1 << 31`, which is int-overflow UB (and wrong where int is
         * 16-bit). */
        const char *ut = unsigned_counterpart(rt);
        fprintf(out, "(");
        emit_type(rt, out);
        fprintf(out, ")(((%s)", ut);
        emit_expr(e->binary.left, out);
        fprintf(out, ") << (");
        emit_expr(e->binary.right, out);
        if (mask_expr) fprintf(out, " & %s))", mask_expr);
        else fprintf(out, " & %d))", mask);
    } else {
        fprintf(out, "(");
        emit_expr(e->binary.left, out);
        fprintf(out, " %s (", op == TOK_LTLT ? "<<" : ">>");
        emit_expr(e->binary.right, out);
        if (mask_expr) fprintf(out, " & %s))", mask_expr);
        else fprintf(out, " & %d))", mask);
    }
    return true;
    return false;
}

/* Integer division/modulo. Two orthogonal concerns, on two axes:
 *  - divide-by-zero: a precondition, on the guard axis. Aborts when guarded;
 *    `unguarded` drops the check (bare divide, UB on a zero divisor).
 *  - signed `min / -1` (and `min % -1`): two's-complement overflow (C UB;
 *    x86 traps it like /0), on the overflow axis. The handling branch is
 *    always emitted (to dodge the hardware trap and keep the result
 *    defined), but its meaning is set by checked/unchecked:
 *      `/`  unchecked: wrap to `min` (a / -1 == -a, via unsigned negation,
 *           width-agnostic); checked: abort (min/-1 is unrepresentable).
 *      `%`  always 0 (a % -1 == 0 is representable, never an overflow, so
 *           checked does not change it).
 *    Unsigned division never overflows, so the min/-1 branch is signed-only.
 *  Decomposing this way makes `unguarded` consistent across operators:
 *  like `unguarded (a + b)`, `unguarded (a / b)` keeps defined overflow
 *  semantics (the wrap) and only trusts the precondition (divisor != 0).
 *  A statement-expr is emitted whenever either concern needs code; an
 *  unsigned divide under `unguarded` needs neither: returns false, and the
 *  caller emits the bare operator. */
static bool emit_int_divmod(Expr *e, int op, Type *rt, FILE *out) {
    bool is_signed = type_is_signed(rt);
    bool need_zero  = !g_guards_suppressed;   /* guard axis owns divisor==0 */
    bool need_minus1 = is_signed;             /* signed min/-1: always handled */
    if (need_zero || need_minus1) {
        int tid = g_temp_counter++;
        const char *opc = op == TOK_SLASH ? "/" : "%";
        /* dividend then divisor, left-to-right, each once. */
        fprintf(out, "({ ");
        emit_type(rt, out);
        fprintf(out, " _nv%d = ", tid);
        emit_expr(e->binary.left, out);
        fprintf(out, "; ");
        emit_type(rt, out);
        fprintf(out, " _dv%d = ", tid);
        emit_expr(e->binary.right, out);
        fprintf(out, "; ");
        if (need_zero) {
            fprintf(out, "if (_dv%d == 0) { fprintf(stderr, \"", tid);
            emit_loc_text(e->loc, out);
            fprintf(out, ": %s by zero\\n\"); FC_ABORT(); } ", op == TOK_SLASH ? "divide" : "modulo");
        }
        if (!need_minus1) {
            /* unsigned: no min/-1 case */
            fprintf(out, "_nv%d %s _dv%d; })", tid, opc, tid);
        } else if (op == TOK_PERCENT) {
            /* signed modulo: a % -1 == 0, always (representable) */
            fprintf(out, "(_dv%d == -1) ? (", tid);
            emit_type(rt, out);
            fprintf(out, ")0 : (");
            emit_type(rt, out);
            fprintf(out, ")(_nv%d %% _dv%d); })", tid, tid);
        } else if (g_overflow_checked) {
            /* checked signed divide: min / -1 overflows, so abort */
            fprintf(out, "if (_dv%d == -1 && _nv%d == %s) fc_overflow(",
                    tid, tid, type_property_c(rt, "min"));
            emit_loc_args(e->loc, out);
            fprintf(out, ", \"divide\"); _nv%d / _dv%d; })", tid, tid);
        } else {
            /* unchecked signed divide: min / -1 wraps to min (a / -1 == -a) */
            const char *ut = unsigned_counterpart(rt);
            fprintf(out, "(_dv%d == -1) ? (", tid);
            emit_type(rt, out);
            fprintf(out, ")(-(%s)_nv%d) : (", ut, tid);
            emit_type(rt, out);
            fprintf(out, ")(_nv%d / _dv%d); })", tid, tid);
        }
        return true;
    }
    return false;
}

/* The operation of a binary expression whose operands are ready (widened for
 * an instance, and hoisted into temps when their order matters). */
static void emit_binary_op(Expr *e, FILE *out) {
    /* Structural equality on complex types */
    if ((e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) && e->binary.left->type) {
        Type *cmp_type = subst_resolve(e->binary.left->type);
        if (type_needs_eq_func(cmp_type)) {
            if (e->binary.op == TOK_BANGEQ) fprintf(out, "(!"); else fprintf(out, "(");
            emit_eq_func_name(cmp_type, out);
            fprintf(out, "(");
            emit_expr(e->binary.left, out);
            fprintf(out, ", ");
            emit_expr(e->binary.right, out);
            fprintf(out, "))");
            return;
        }
    }
    int op = e->binary.op;
    /* Resolve a type-var result type to its concrete instance type: every
     * defined-behavior decision below (wrap casts, shift mask, div guards,
     * checked traps) must see the monomorphized type, not the raw 'a. */
    Type *rt = subst_resolve(e->type);

    /* Pointer difference (ptr - ptr): a ptrdiff_t element count. Must emit a
     * plain subtraction: the signed-wrap path below would cast the pointers
     * to unsigned and compute a byte difference instead. */
    if (op == TOK_MINUS && e->binary.left->type &&
        e->binary.left->type->kind == TYPE_POINTER &&
        e->binary.right->type &&
        e->binary.right->type->kind == TYPE_POINTER) {
        fprintf(out, "(");
        emit_expr(e->binary.left, out);
        fprintf(out, " - ");
        emit_expr(e->binary.right, out);
        fprintf(out, ")");
        return;
    }

    if ((op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR) && rt && type_is_integer(rt) &&
        emit_int_arith(e, op, rt, out))
        return;

    if ((op == TOK_LTLT || op == TOK_GTGT) && rt && type_is_integer(rt) &&
        emit_shift(e, op, rt, out))
        return;

    if ((op == TOK_SLASH || op == TOK_PERCENT) && rt && type_is_integer(rt) &&
        emit_int_divmod(e, op, rt, out))
        return;

    const char *op_str;
    switch (op) {
    case TOK_PLUS:     op_str = "+";  break;
    case TOK_MINUS:    op_str = "-";  break;
    case TOK_STAR:     op_str = "*";  break;
    case TOK_SLASH:    op_str = "/";  break;
    case TOK_PERCENT:  op_str = "%"; break;
    case TOK_EQEQ:    op_str = "=="; break;
    case TOK_BANGEQ:  op_str = "!="; break;
    case TOK_LT:      op_str = "<";  break;
    case TOK_GT:      op_str = ">";  break;
    case TOK_LTEQ:    op_str = "<="; break;
    case TOK_GTEQ:    op_str = ">="; break;
    case TOK_AMPAMP:  op_str = "&&"; break;
    case TOK_PIPEPIPE: op_str = "||"; break;
    case TOK_AMP:     op_str = "&";  break;
    case TOK_PIPE:    op_str = "|";  break;
    case TOK_CARET:   op_str = "^";  break;
    case TOK_LTLT:    op_str = "<<"; break;
    case TOK_GTGT:    op_str = ">>"; break;
    default: op_str = "?"; break;
    }
    fprintf(out, "(");
    /* gcc and clang read a decimal `10 ^ 6` as a mistyped power of ten
     * (-Wxor-used-as-pow), and only a hexadecimal operand silences them;
     * parentheses do not.  FC's `^` is exclusive-or and has no other
     * reading, so its literal operands go out in hex. */
    emit_binary_operand(e->binary.left, op == TOK_CARET, out);
    fprintf(out, " %s ", op_str);
    emit_binary_operand(e->binary.right, op == TOK_CARET, out);
    fprintf(out, ")");
}

/* A binary operation: overflow-defined integer arithmetic, masked shifts,
 * guarded division, structural equality, and operand ordering when an
 * operand has side effects. */
static void emit_binary(Expr *e, FILE *out) {
    /* Mixed numeric operands meet at their common type, as pass2 arranges
     * for concrete code; a shift keeps its operands as they are. */
    Expr *unwidened[2] = { NULL, NULL };
    if (g_subst && e->binary.op != TOK_LTLT && e->binary.op != TOK_GTGT) {
        Type *common = type_common_numeric(subst_resolve(e->binary.left->type),
                                           subst_resolve(e->binary.right->type));
        unwidened[0] = widen_for_instance(&e->binary.left, common);
        unwidened[1] = widen_for_instance(&e->binary.right, common);
    }
    /* Force left-to-right operand evaluation when an operand has side
     * effects (C leaves binary-operand order unspecified).  /, %, &&, ||
     * are excluded: division self-sequences its temps below, and &&/|| are
     * already left-to-right and must not have their right operand hoisted
     * (it is conditionally evaluated). */
    Expr **_bslots[2] = { &e->binary.left, &e->binary.right };
    Expr _bscratch[2]; Expr *_bsaved[2];
    bool _bseq = e->binary.op != TOK_SLASH && e->binary.op != TOK_PERCENT &&
                 e->binary.op != TOK_AMPAMP && e->binary.op != TOK_PIPEPIPE &&
                 !g_const_context &&
                 (seq_needed(_bslots, 2) || binary_is_self_compare(e));
    if (_bseq) { fprintf(out, "({ "); seq_hoist(_bslots, 2, _bscratch, _bsaved, out); }
    emit_binary_op(e, out);
    if (_bseq) { fprintf(out, "; })"); seq_restore(_bslots, 2, _bsaved); }
    if (unwidened[0]) e->binary.left = unwidened[0];
    if (unwidened[1]) e->binary.right = unwidened[1];
}

/* alloc(...) and alloca(...) as a statement expression yielding the
 * pointer, slice or option. */
static void emit_alloc(Expr *e, FILE *out) {
    if (e->alloc_expr.is_stack) {
        /* alloca(...): dynamic stack, no option wrapper, no failure sentinel.
         * Reclaimed when the enclosing function returns. */
        if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
            /* alloca(T, N) yields T* (raw buffer) */
            fprintf(out, "(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)__builtin_alloca(fc_to_size(");
            emit_expr(e->alloc_expr.size_expr, out);
            fprintf(out, ") * sizeof(");
            emit_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "))");
        } else if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr) {
            /* alloca(T[n] { }) yields T[] (zero-initialized runtime-sized slice) */
            int tid = g_temp_counter++;
            fprintf(out, "({ int64_t _asz%d = (int64_t)", tid);
            emit_expr(e->alloc_expr.size_expr, out);
            fprintf(out, "; ");
            if (g_len_repr < 64) {
                /* Cap before the alloca so an impossible length traps
                 * instead of dimensioning the stack buffer. */
                fprintf(out, "if (__builtin_expect(_asz%d > FC_LEN_MAX, 0)) fc_len_cap(", tid);
                emit_loc_args(e->loc, out);
                fprintf(out, ", (long long)_asz%d); ", tid);
            }
            emit_elem_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "* _aptr%d = (", tid);
            emit_elem_type(e->alloc_expr.alloc_type, out);
            fprintf(out, "*)__builtin_alloca(fc_to_size(_asz%d) * sizeof(", tid);
            emit_elem_type(e->alloc_expr.alloc_type, out);
            fprintf(out, ")); memset(_aptr%d, 0, fc_to_size(_asz%d) * sizeof(", tid, tid);
            emit_elem_type(e->alloc_expr.alloc_type, out);
            fprintf(out, ")); (");
            emit_type(e->type, out);
            fprintf(out, "){ .ptr = _aptr%d, .len = _asz%d }; })", tid, tid);
        } else if (e->alloc_expr.init_expr &&
                   e->alloc_expr.init_expr->kind == EXPR_INTERP_STRING) {
            /* alloca("interp %s{x}") / alloca(c"...") yields str/cstr on the
             * stack. The hoist pass gives it no fixed slot, so the NULL
             * (stack) path emits __builtin_alloca. */
            emit_interp_string(e->alloc_expr.init_expr, out, NULL);
        } else if (e->alloc_expr.init_expr &&
                   e->alloc_expr.init_expr->kind == EXPR_ARRAY_LIT) {
            /* alloca(T[N] { elems }): reuse the slice-literal stack emission.
             * The hoist pass gives this literal no fixed slot, so it is
             * alloca'd. */
            emit_expr(e->alloc_expr.init_expr, out);
        } else {
            emit_expr(e->alloc_expr.init_expr, out);
        }
        return;
    }
    if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr && e->alloc_expr.alloc_raw) {
        /* alloc(T, N) yields T*? (raw buffer, null sentinel) */
        fprintf(out, "(");
        emit_type(e->alloc_expr.alloc_type, out);
        fprintf(out, "*)calloc(fc_alloc_n(");
        emit_expr(e->alloc_expr.size_expr, out);
        fprintf(out, "), sizeof(");
        emit_type(e->alloc_expr.alloc_type, out);
        fprintf(out, "))");
    } else if (e->alloc_expr.alloc_type && e->alloc_expr.size_expr) {
        /* alloc(T[N]) yields T[]?. Under a narrow --len-repr, a count over
         * the stored width is an allocation that cannot succeed, so it
         * answers through the failure channel (none), not a trap. */
        int tid = g_temp_counter++;
        fprintf(out, "({ int64_t _asz%d = (int64_t)", tid);
        emit_expr(e->alloc_expr.size_expr, out);
        fprintf(out, "; ");
        emit_elem_type(e->alloc_expr.alloc_type, out);
        fprintf(out, "* _aptr%d = ", tid);
        if (g_len_repr < 64)
            fprintf(out, "_asz%d > FC_LEN_MAX ? NULL : ", tid);
        fprintf(out, "(");
        emit_elem_type(e->alloc_expr.alloc_type, out);
        fprintf(out, "*)calloc(fc_alloc_n(_asz%d), sizeof(", tid);
        emit_elem_type(e->alloc_expr.alloc_type, out);
        fprintf(out, ")); _aptr%d ? (", tid);
        emit_type(e->type, out);
        fprintf(out, "){ .value = (");
        Type *slice_type = e->type->option.inner;
        emit_type(slice_type, out);
        fprintf(out, "){ .ptr = _aptr%d, .len = _asz%d }, .has_value = true } : (", tid, tid);
        emit_type(e->type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else if (e->alloc_expr.alloc_type) {
        /* alloc(T) yields T*? (bare pointer, calloc returns NULL on failure) */
        fprintf(out, "(");
        emit_type(e->alloc_expr.alloc_type, out);
        fprintf(out, "*)calloc(1, sizeof(");
        emit_type(e->alloc_expr.alloc_type, out);
        fprintf(out, "))");
    } else if (e->alloc_expr.init_expr->kind == EXPR_STRING_LIT) {
        /* alloc("literal") yields str? (direct to heap) */
        int tid = g_temp_counter++;
        Expr *ie = e->alloc_expr.init_expr;
        int actual_len = str_lit_len(ie->string_lit.value, ie->string_lit.length);
        fprintf(out, "({ uint8_t *_ap%d = (uint8_t*)malloc(%d); ", tid,
            actual_len > 0 ? actual_len : 1);
        fprintf(out, "_ap%d ? (memcpy(_ap%d, (uint8_t*)\"", tid, tid);
        emit_str_lit_body(ie->string_lit.value, ie->string_lit.length, out);
        fprintf(out, "\", %d), (", actual_len);
        emit_type(e->type, out);
        fprintf(out, "){ .value = (fc_str){ .ptr = _ap%d, .len = %d }, .has_value = true }) : (",
            tid, actual_len);
        emit_type(e->type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else if (e->alloc_expr.init_expr->kind == EXPR_CSTRING_LIT) {
        /* alloc(c"literal") yields cstr? (direct to heap, null sentinel) */
        int tid = g_temp_counter++;
        Expr *ie = e->alloc_expr.init_expr;
        int actual_len = str_lit_len(ie->cstring_lit.value, ie->cstring_lit.length);
        fprintf(out, "({ uint8_t *_ap%d = (uint8_t*)malloc(%d + 1); ", tid, actual_len);
        fprintf(out, "if (_ap%d) memcpy(_ap%d, (uint8_t*)\"", tid, tid);
        emit_str_lit_body(ie->cstring_lit.value, ie->cstring_lit.length, out);
        fprintf(out, "\", %d + 1); _ap%d; })", actual_len, tid);
    } else if (e->alloc_expr.init_expr->kind == EXPR_CAST &&
               is_cstr_type(e->alloc_expr.init_expr->type)) {
        /* alloc((cstr) str) yields cstr?: a heap copy of str + NUL (null
         * sentinel on malloc failure). The cast's own (alloca) codegen is
         * bypassed: the copy goes straight into the heap buffer. */
        int tid = g_temp_counter++;
        Expr *operand = e->alloc_expr.init_expr->cast.operand;
        fprintf(out, "({ fc_str _sc%d = ", tid);
        emit_expr(operand, out);
        fprintf(out, "; uint8_t *_ap%d = (uint8_t*)malloc(fc_to_size(_sc%d.len + 1)); ", tid, tid);
        fprintf(out, "if (_ap%d) { memcpy(_ap%d, _sc%d.ptr, fc_to_size(_sc%d.len)); "
                     "_ap%d[_sc%d.len] = '\\0'; } _ap%d; })",
                tid, tid, tid, tid, tid, tid, tid);
    } else if (e->alloc_expr.init_expr->kind == EXPR_INTERP_STRING) {
        /* alloc("interp %d{x}") or alloc(c"interp %d{x}") yields str?/cstr? */
        emit_interp_string(e->alloc_expr.init_expr, out, e->type);
    } else if (e->alloc_expr.init_expr->kind == EXPR_ARRAY_LIT) {
        /* alloc(T[N] { elems }) yields T[]? (direct to heap) */
        int tid = g_temp_counter++;
        Expr *ie = e->alloc_expr.init_expr;
        Type *elem_type = ie->array_lit.elem_type;
        int64_t size = ie->array_lit.size_expr->int_lit.value;
        int ec = ie->array_lit.elem_count;
        fprintf(out, "({ ");
        emit_elem_type(elem_type, out);
        if (ec == 0) {
            fprintf(out, " *_ap%d = (", tid);
            emit_elem_type(elem_type, out);
            fprintf(out, "*)calloc(%d, sizeof(", (int)(size > 0 ? size : 1));
        } else {
            fprintf(out, " *_ap%d = (", tid);
            emit_elem_type(elem_type, out);
            fprintf(out, "*)malloc(%d * sizeof(", (int)size);
        }
        emit_elem_type(elem_type, out);
        fprintf(out, ")); ");
        fprintf(out, "_ap%d ? (", tid);
        for (int i = 0; i < ec; i++) {
            fprintf(out, "_ap%d[%d] = ", tid, i);
            emit_elem_store_cast(elem_type, out);
            emit_expr(ie->array_lit.elems[i], out);
            fprintf(out, ", ");
        }
        fprintf(out, "(");
        emit_type(e->type, out);
        fprintf(out, "){ .value = (");
        /* inner slice type */
        Type *sl = e->type->option.inner;
        emit_type(sl, out);
        fprintf(out, "){ .ptr = _ap%d, .len = %d }, .has_value = true }) : (", tid, (int)size);
        emit_type(e->type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else if (e->alloc_expr.init_expr->kind == EXPR_FUNC ||
               e->alloc_expr.closure_src) {
        /* alloc(lambda) / alloc(f) yields F?, a heap closure. Malloc the
         * context struct and yield a fat pointer whose .ctx owns the heap
         * block (free(f) frees f.ctx). Option struct, none on malloc failure
         * (function options have no null-sentinel form). The literal form
         * fills the captures directly; the binding form memcpys the source
         * closure's live context (captures are immutable copies, so the two
         * are identical), which also works where only the binding, not the
         * captured locals, is in scope (e.g. inside a nested lambda). */
        int tid = g_temp_counter++;
        bool literal = e->alloc_expr.init_expr->kind == EXPR_FUNC;
        Expr *lam = literal ? e->alloc_expr.init_expr : e->alloc_expr.closure_src;
        const char *ln = lambda_c_name(lam);
        fprintf(out, "({ _ctx_%s* _cp%d = (_ctx_%s*)malloc(sizeof(_ctx_%s)); ",
            ln, tid, ln, ln);
        if (literal) {
            fprintf(out, "if (_cp%d) { ", tid);
            for (int i = 0; i < lam->func.capture_count; i++)
                fprintf(out, "_cp%d->%s = %s; ", tid,
                    lam->func.captures[i].codegen_name,
                    lam->func.captures[i].codegen_name);
            fprintf(out, "} _cp%d ? (", tid);
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            emit_type(e->type->option.inner, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = _cp%d }, .has_value = true } : (",
                ln, tid);
        } else {
            emit_type(e->type->option.inner, out);
            fprintf(out, " _src%d = ", tid);
            emit_expr(e->alloc_expr.init_expr, out);
            fprintf(out, "; if (_cp%d) memcpy(_cp%d, _src%d.ctx, sizeof(_ctx_%s)); ",
                tid, tid, tid, ln);
            fprintf(out, "_cp%d ? (", tid);
            emit_type(e->type, out);
            fprintf(out, "){ .value = (");
            emit_type(e->type->option.inner, out);
            fprintf(out, "){ .fn_ptr = _src%d.fn_ptr, .ctx = _cp%d }, "
                ".has_value = true } : (", tid, tid);
        }
        emit_type(e->type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else if (e->alloc_expr.init_expr->kind == EXPR_STRUCT_LIT ||
               (e->alloc_expr.init_expr->type &&
                e->alloc_expr.init_expr->type->kind == TYPE_UNION)) {
        /* alloc(struct_lit) and alloc(union_variant) yield T*? (malloc + compound
         * literal, null sentinel). Braces around the if body so multi-line
         * literals don't trigger clang's -Wmisleading-indentation. */
        int tid = g_temp_counter++;
        Type *val_type = e->alloc_expr.init_expr->type;
        fprintf(out, "({ ");
        emit_type(val_type, out);
        fprintf(out, "* _ap%d = (", tid);
        emit_type(val_type, out);
        fprintf(out, "*)malloc(sizeof(");
        emit_type(val_type, out);
        fprintf(out, ")); if (_ap%d) { *_ap%d = ", tid, tid);
        emit_expr(e->alloc_expr.init_expr, out);
        fprintf(out, "; } _ap%d; })", tid);
    } else if (e->alloc_expr.init_expr->type &&
               e->alloc_expr.init_expr->type->kind == TYPE_SLICE) {
        /* alloc(slice_expr) yields T[]? (deep-copy slice data to heap) */
        int tid = g_temp_counter++;
        Type *st = e->alloc_expr.init_expr->type;
        Type *elem_type = st->slice.elem;
        fprintf(out, "({ ");
        emit_type(st, out);
        fprintf(out, " _as%d = ", tid);
        emit_expr(e->alloc_expr.init_expr, out);
        fprintf(out, "; ");
        emit_type(elem_type, out);
        fprintf(out, " *_ap%d = (", tid);
        emit_type(elem_type, out);
        fprintf(out, "*)malloc(fc_alloc_n(_as%d.len) * sizeof(", tid);
        emit_type(elem_type, out);
        fprintf(out, ")); ");
        fprintf(out, "_ap%d ? (memcpy(_ap%d, _as%d.ptr, fc_to_size(_as%d.len) * sizeof(",
            tid, tid, tid, tid);
        emit_type(elem_type, out);
        fprintf(out, ")), (");
        emit_type(e->type, out);
        fprintf(out, "){ .value = (");
        emit_type(st, out);
        fprintf(out, "){ .ptr = _ap%d, .len = _as%d.len }, .has_value = true }) : (",
            tid, tid);
        emit_type(e->type, out);
        fprintf(out, "){ .has_value = false }; })");
    } else {
        internal_error(e->loc, "alloc form reached codegen unchecked");
    }
}

/* A match as a statement expression: the subject evaluated once into a
 * temp, then an if-chain over the arm predicates and guards. */
static void emit_match(Expr *e, FILE *out) {
    /* Emit as statement expression. When no arm has a `when` guard, use a
       plain if/else chain with an unconditional last arm (pass2 proved the
       match exhaustive). When at least one arm has a guard, use a done-flag
       so guard-false arms can fall through to subsequent arms. */
    /* No result temp when the match yields no value: void, or never (every arm
       diverges via return/break/continue). */
    bool match_is_void = type_valueless(e->type);

    bool has_any_guard = false;
    for (int i = 0; i < e->match_expr.arm_count; i++) {
        if (e->match_expr.arms[i].guard) { has_any_guard = true; break; }
    }

    fprintf(out, "({\n");
    g_indent_level++;

    /* Emit subject into a temp variable */
    int subj_id = g_temp_counter++;
    emit_indent(out);
    emit_type(e->match_expr.subject->type, out);
    fprintf(out, " _subj%d = ", subj_id);
    emit_expr(e->match_expr.subject, out);
    fprintf(out, ";\n");
    emit_indent(out);
    fprintf(out, "(void)_subj%d;\n", subj_id);

    /* Emit result variable (skip for void matches) */
    int res_id = -1;
    if (!match_is_void) {
        res_id = g_temp_counter++;
        emit_indent(out);
        emit_type(e->type, out);
        fprintf(out, " _match%d;\n", res_id);
    }

    int done_id = -1;
    if (has_any_guard) {
        done_id = g_temp_counter++;
        emit_indent(out);
        fprintf(out, "int _matchdone%d = 0;\n", done_id);
    }

    bool last_arm_unconditional = false;
    for (int i = 0; i < e->match_expr.arm_count; i++) {
        MatchArm *arm = &e->match_expr.arms[i];
        Pattern *pat = arm->pattern;
        char subj_expr[64];
        snprintf(subj_expr, sizeof(subj_expr), "_subj%d", subj_id);

        bool is_last_arm = (i == e->match_expr.arm_count - 1);

        if (has_any_guard) {
            /* if (!_matchdoneN) { [if (pat_cond)] { bindings; [if (guard)] { body; _matchdoneN = 1; } } } */
            emit_indent(out);
            fprintf(out, "if (!_matchdone%d) {\n", done_id);
            g_indent_level++;

            emit_indent(out);
            bool has_cond = false;
            emit_pat_conditions(pat, subj_expr, e->match_expr.subject->type, &has_cond, out);
            if (has_cond) fprintf(out, ") {\n");
            else fprintf(out, "{\n");
            g_indent_level++;

            emit_pat_bindings(pat, subj_expr, e->match_expr.subject->type, out);

            bool has_guard = (arm->guard != NULL);
            if (has_guard) {
                emit_indent(out);
                /* Avoid double-parens like if ((x == y)) which triggers
                   clang's -Wparentheses-equality when the guard is a
                   binary expression (emit_expr wraps binaries in parens). */
                if (emit_self_parens(arm->guard)) {
                    fprintf(out, "if ");
                    emit_expr(arm->guard, out);
                    fprintf(out, " {\n");
                } else {
                    fprintf(out, "if (");
                    emit_expr(arm->guard, out);
                    fprintf(out, ") {\n");
                }
                g_indent_level++;
            }
        } else {
            emit_indent(out);
            if (i > 0) fprintf(out, "else ");
            bool has_cond = false;
            if (!is_last_arm) {
                emit_pat_conditions(pat, subj_expr, e->match_expr.subject->type, &has_cond, out);
            }
            if (has_cond) fprintf(out, ") {\n");
            else fprintf(out, "{\n");
            g_indent_level++;
            emit_pat_bindings(pat, subj_expr, e->match_expr.subject->type, out);
            /* An unguarded catch-all (`_`, a binding, an all-wildcard
             * struct/tuple) tests nothing, so it emits a bare block and
             * every later arm is unreachable under first-match-wins. FC
             * permits such redundant arms; they must be dropped, not
             * emitted, because an `else` after a block with no `if` is not
             * C. */
            last_arm_unconditional = !has_cond;
        }

        /* Emit arm body */
        if (match_is_void) {
            defer_scope_push(false);
            emit_block_stmts(arm->body, arm->body_count, out, false, true);
            defer_scope_pop();
        } else if (arm->body_count == 1) {
            emit_indent(out);
            if (type_is_never(arm->body[0]->type)) {
                /* Diverging arm (return/break/continue): emit as a statement,
                   no assignment; its exit fires before the temp is read. */
                emit_expr(arm->body[0], out);
            } else {
                fprintf(out, "_match%d = ", res_id);
                emit_expr(arm->body[0], out);
            }
            fprintf(out, ";\n");
        } else {
            /* Multiple statements: emit all; the last is the value */
            defer_scope_push(false);
            for (int s = 0; s < arm->body_count; s++) {
                if (arm->body[s]->kind == EXPR_DEFER) {
                    defer_scope_add(arm->body[s]->defer_expr.value);
                    continue;
                }
                emit_indent(out);
                if (s == arm->body_count - 1) {
                    if (type_is_never(arm->body[s]->type)) {
                        /* Diverging tail: emit as a statement, no assignment.
                           emit_expr handles its own defers (return/break). */
                        emit_expr(arm->body[s], out);
                        fprintf(out, ";\n");
                    } else if (has_pending_defers()) {
                        emit_type(arm->body[s]->type, out);
                        int tid = g_temp_counter++;
                        fprintf(out, " _mret%d = ", tid);
                        emit_expr(arm->body[s], out);
                        fprintf(out, ";\n");
                        emit_scope_defers(g_defer_scope, out);
                        emit_indent(out);
                        fprintf(out, "_match%d = _mret%d;\n", res_id, tid);
                    } else {
                        fprintf(out, "_match%d = ", res_id);
                        emit_expr(arm->body[s], out);
                        fprintf(out, ";\n");
                    }
                } else if (arm->body[s]->kind == EXPR_LET_DESTRUCT) {
                    emit_let_destruct_stmt(arm->body[s], out);
                } else {
                    emit_expr(arm->body[s], out);
                    fprintf(out, ";\n");
                    if (arm->body[s]->kind == EXPR_LET) {
                        /* Same -Wunused-variable silencer emit_block_stmts
                         * uses; this path emits arm statements directly. */
                        const char *vn = arm->body[s]->let_expr.codegen_name
                            ? arm->body[s]->let_expr.codegen_name
                            : arm->body[s]->let_expr.let_name;
                        emit_indent(out);
                        fprintf(out, "(void)%s;\n", vn);
                    }
                }
            }
            defer_scope_pop();
        }

        if (has_any_guard) {
            emit_indent(out);
            fprintf(out, "_matchdone%d = 1;\n", done_id);
            bool has_guard = (arm->guard != NULL);
            if (has_guard) {
                g_indent_level--;
                emit_indent(out);
                fprintf(out, "}\n");
            }
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
        } else {
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
        }
        if (last_arm_unconditional) break;
    }

    if (has_any_guard) {
        /* Unreachable if pass2 exhaustiveness holds: at least one unguarded
           arm must have covered the value. The abort keeps _matchN provably
           initialized for -Wuninitialized and stops the program if a
           compiler bug ever leaves a case uncovered. */
        emit_indent(out);
        fprintf(out, "if (!_matchdone%d) FC_ABORT();\n", done_id);
    }

    if (!match_is_void) {
        emit_indent(out);
        fprintf(out, "_match%d;\n", res_id);
    }

    g_indent_level--;
    emit_indent(out);
    fprintf(out, "})");
}

/* A struct literal as a compound literal, or field by field when a field is
 * a fixed array. */
static void emit_struct_lit(Expr *e, FILE *out) {
    /* Use the resolved type name (handles mangled module types) */
    const char *sname = (e->type && e->type->kind == TYPE_STRUCT)
        ? aggregate_c_name(e->type) : e->struct_lit.type_name;
    /* A fixed-array field requires a statement expression */
    bool has_fixed_array = false;
    Type *st = e->type;
    if (st && st->kind == TYPE_STRUCT) {
        for (int f = 0; f < st->struc.field_count; f++) {
            if (st->struc.fields[f].type->kind == TYPE_FIXED_ARRAY) {
                has_fixed_array = true;
                break;
            }
        }
    }
    /* Force left-to-right field evaluation when a field has side effects (C
     * leaves initializer-list order unspecified).  Only the plain
     * compound-literal path needs this: the fixed-array paths already
     * assign field-by-field in order, and const context has no effects. */
    int sln = has_fixed_array ? 0 : e->struct_lit.field_count;
    Expr ***field_slots = arena_alloc(g_arena, sizeof(Expr**) * (size_t)(sln > 0 ? sln : 1));
    for (int i = 0; i < sln; i++) field_slots[i] = &e->struct_lit.fields[i].value;
    SeqOperands slseq = seq_operands(field_slots, sln);
    if (slseq.active) {
        fprintf(out, "({ ");
        seq_hoist(slseq.slots, slseq.n, slseq.scratch, slseq.saved, out);
    }
    if (has_fixed_array && g_const_context) {
        /* File-scope aggregate initializer: emit each field inline.  A
         * fixed-array field's value is a slice or string literal or the empty
         * slice (const_fold_expr rejects anything else, and any overflow),
         * emitted as a bare { e0, e1, ... } C array initializer, since the
         * slice header form cannot initialize a raw C array. */
        if (st->struc.c_name) {
            fprintf(out, "(%s %s){",
                st->struc.is_c_union ? "union" : "struct", st->struc.c_name);
        } else {
            fprintf(out, "(%s){", sname);
        }
        bool first = true;
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            const char *fname = e->struct_lit.fields[i].name;
            Type *field_type = NULL;
            for (int f = 0; f < st->struc.field_count; f++) {
                if (st->struc.fields[f].name == fname) {
                    field_type = st->struc.fields[f].type;
                    break;
                }
            }
            if (!first) fprintf(out, ", ");
            first = false;
            fprintf(out, ".%s = ", c_safe_ident(g_intern, fname));
            Expr *v = e->struct_lit.fields[i].value;
            if (field_type && field_type->kind == TYPE_FIXED_ARRAY &&
                v && v->kind == EXPR_DEFAULT) {
                /* The empty slice copies nothing: the array is all zero. */
                fprintf(out, "{0}");
            } else if (field_type && field_type->kind == TYPE_FIXED_ARRAY &&
                       v && v->kind == EXPR_STRING_LIT) {
                /* A string literal's bytes, as integers: a C string
                 * initializer would also try to store the terminating NUL. */
                int n = decode_str_lit(v->string_lit.value, v->string_lit.length, NULL);
                unsigned char *bytes = malloc(n > 0 ? (size_t)n : 1);
                decode_str_lit(v->string_lit.value, v->string_lit.length, bytes);
                fprintf(out, "{");
                if (n == 0) fprintf(out, "0");
                for (int j = 0; j < n; j++)
                    fprintf(out, "%s%u", j > 0 ? ", " : "", (unsigned)bytes[j]);
                fprintf(out, "}");
                free(bytes);
            } else if (field_type && field_type->kind == TYPE_FIXED_ARRAY &&
                v && v->kind == EXPR_ARRAY_LIT) {
                /* Bare aggregate: no slice header, no backing */
                fprintf(out, "{");
                if (v->array_lit.elem_count == 0) {
                    fprintf(out, "0");
                } else {
                    for (int j = 0; j < v->array_lit.elem_count; j++) {
                        if (j > 0) fprintf(out, ", ");
                        emit_expr(v->array_lit.elems[j], out);
                    }
                }
                fprintf(out, "}");
            } else {
                emit_expr(v, out);
            }
        }
        fprintf(out, "}");
        return;
    }
    if (has_fixed_array) {
        int tid = g_temp_counter++;
        fprintf(out, "({ ");
        if (st->struc.c_name) {
            fprintf(out, "%s %s", st->struc.is_c_union ? "union" : "struct",
                st->struc.c_name);
        } else {
            fprintf(out, "%s", sname);
        }
        fprintf(out, " _sl%d = {0}; ", tid);
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            const char *fname = e->struct_lit.fields[i].name;
            /* Find the field type in the struct */
            Type *field_type = NULL;
            for (int f = 0; f < st->struc.field_count; f++) {
                if (st->struc.fields[f].name == fname) {
                    field_type = st->struc.fields[f].type;
                    break;
                }
            }
            if (field_type && field_type->kind == TYPE_FIXED_ARRAY) {
                int sid = g_temp_counter++;
                SrcLoc vloc = e->struct_lit.fields[i].value->loc;
                emit_type(e->struct_lit.fields[i].value->type, out);
                fprintf(out, " _fas%d = ", sid);
                emit_expr(e->struct_lit.fields[i].value, out);
                fprintf(out, "; if (_fas%d.len > %lld) { fprintf(stderr, \"", sid,
                        (long long)fixarr_size(field_type));
                emit_loc_text(vloc, out);
                fprintf(out, ": fixed-array field '%s' overflow: "
                             "len=%%lld capacity=%lld\\n\", "
                             "(long long)_fas%d.len); FC_ABORT(); } ",
                        fname, (long long)fixarr_size(field_type),
                        sid);
                fprintf(out, "memcpy(_sl%d.%s, _fas%d.ptr, fc_to_size(_fas%d.len) * sizeof(",
                        tid, c_safe_ident(g_intern, fname), sid, sid);
                emit_type(field_type->fixed_array.elem, out);
                fprintf(out, ")); ");
            } else {
                fprintf(out, "_sl%d.%s = ", tid, c_safe_ident(g_intern, fname));
                emit_expr(e->struct_lit.fields[i].value, out);
                fprintf(out, "; ");
            }
        }
        fprintf(out, "_sl%d; })", tid);
    } else {
        /* Multi-line when 2+ fields: each field on its own line so nested
         * statement-expressions (e.g. alloc(...)!) don't concatenate into
         * one huge line that trips gcc's column-tracking limit. */
        bool multiline = e->struct_lit.field_count >= 2;
        if (st && st->kind == TYPE_STRUCT && st->struc.c_name) {
            fprintf(out, "(%s %s){", st->struc.is_c_union ? "union" : "struct",
                st->struc.c_name);
        } else {
            fprintf(out, "(%s){", sname);
        }
        if (multiline) {
            fprintf(out, "\n");
            g_indent_level++;
        } else {
            fprintf(out, " ");
        }
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            if (i > 0) {
                fprintf(out, ",");
                if (multiline) fprintf(out, "\n");
                else fprintf(out, " ");
            }
            if (multiline) emit_indent(out);
            fprintf(out, ".%s = ", c_safe_ident(g_intern, e->struct_lit.fields[i].name));
            emit_expr(e->struct_lit.fields[i].value, out);
        }
        if (multiline) {
            fprintf(out, "\n");
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        } else {
            fprintf(out, " }");
        }
    }
    if (slseq.active) { fprintf(out, "; })"); seq_restore(slseq.slots, slseq.n, slseq.saved); }
}

/* A call: a variant constructor, a direct call with its context argument,
 * an indirect call through a function value, or an extern call. */
static void emit_call(Expr *e, FILE *out) {
    /* A variant constructor with a payload: `u.variant(x)` */
    if (e->call.func->kind == EXPR_FIELD &&
        e->call.func->field.is_variant_constructor) {
        const char *union_name = aggregate_c_name(e->type);
        const char *variant_name = e->call.func->field.name;
        fprintf(out, "(%s){ .tag = %s, ." FC_PAYLOAD_MEMBER " = { .%s = ",
            union_name, union_tag_value(union_name, variant_name),
            c_safe_ident(g_intern, variant_name));
        emit_expr(e->call.args[0], out);
        fprintf(out, " } }");
        return;
    }

    /* Get callee function type for coercion */
    Type *call_ft = e->call.func->type;

    /* Arguments widen to their parameters' types, as pass2 arranges for
     * concrete code. */
    Expr **unwidened = NULL;
    if (g_subst && call_ft && call_ft->kind == TYPE_FUNC && e->call.arg_count > 0) {
        unwidened = arena_alloc(g_arena, sizeof(Expr*) * (size_t)e->call.arg_count);
        for (int i = 0; i < e->call.arg_count && i < call_ft->func.param_count; i++)
            unwidened[i] = widen_for_instance(&e->call.args[i], call_ft->func.param_types[i]);
    }

    /* Force left-to-right argument evaluation when any argument has side
     * effects: evaluate the earlier arguments into temps in source order
     * (the callee is evaluated first, the final argument last). */
    SeqOperands aseq = seq_operands(expr_slots(e->call.args, e->call.arg_count),
                                    e->call.arg_count);

    if (e->call.is_indirect) {
        /* Indirect call through fat pointer */
        int tid = g_temp_counter++;
        fprintf(out, "({ ");
        emit_type(call_ft, out);
        fprintf(out, " _cf%d = ", tid);
        emit_expr(e->call.func, out);
        fprintf(out, "; ");
        if (aseq.active) seq_hoist(aseq.slots, aseq.n, aseq.scratch, aseq.saved, out);
        fprintf(out, "_cf%d.fn_ptr(", tid);
        for (int i = 0; i < e->call.arg_count; i++) {
            if (i > 0) fprintf(out, ", ");
            emit_expr(e->call.args[i], out);
        }
        if (e->call.arg_count > 0) fprintf(out, ", ");
        fprintf(out, "_cf%d.ctx); })", tid);
        if (aseq.active) seq_restore(aseq.slots, aseq.n, aseq.saved);
    } else {
        if (aseq.active) {
            fprintf(out, "({ ");
            seq_hoist(aseq.slots, aseq.n, aseq.scratch, aseq.saved, out);
        }
        /* Direct call: emit the function name directly (not via emit_expr,
           which would wrap it in a fat pointer) and append NULL for _ctx */
        Expr *callee = e->call.func;
        const char *fn_name = e->call.mangled_name; /* monomorphized name if generic */

        /* Resolve deferred generic call under substitution context */
        if (!fn_name && g_subst && e->call.type_arg_count > 0) {
            Type **concrete_args = malloc(sizeof(Type*) * (size_t)e->call.type_arg_count);
            for (int i = 0; i < e->call.type_arg_count; i++) {
                concrete_args[i] = type_substitute(g_arena, e->call.type_args[i],
                    g_subst->var_names, g_subst->concrete, g_subst->count);
            }
            /* pass2's resolved_callee, set for single-level and multi-level
               qualified calls alike */
            const char *base_name = NULL;
            Symbol *callee_sym = e->call.resolved_callee;
            if (callee_sym) {
                base_name = (callee_sym->decl && callee_sym->decl->kind == DECL_LET
                             && callee_sym->decl->let.codegen_name)
                            ? callee_sym->decl->let.codegen_name : callee_sym->name;
            }
            /* Monomorphization registered every instance before emission
             * began, so this is a lookup. */
            if (base_name && callee_sym) {
                fn_name = mangle_generic_name(g_intern, base_name, concrete_args,
                                              e->call.type_arg_count);
                if (!mono_find(g_mono, fn_name))
                    internal_error(e->loc, "generic instance '%s' was not discovered",
                                   fn_name);
            }
            free(concrete_args);
        }

        if (!fn_name) {
            if (callee->kind == EXPR_IDENT) {
                fn_name = callee->ident.codegen_name
                    ? callee->ident.codegen_name : callee->ident.name;
            } else if (callee->kind == EXPR_FIELD && callee->field.codegen_name) {
                fn_name = callee->field.codegen_name;
            }
        }

        if (fn_name && !e->call.is_extern_call) {
            fprintf(out, "%s(", fn_name);
            for (int i = 0; i < e->call.arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_expr(e->call.args[i], out);
            }
            if (e->call.arg_count > 0) fprintf(out, ", ");
            fprintf(out, "NULL)");
        } else if (fn_name) {
            /* Extern call: cast cstr-aliased params to const char* for C
             * headers, and cast cstr/cstr? return values from char* back
             * to uint8_t*. A protocol extern (from <protocol>) wraps the
             * raw return into the declared T! in place. */
            Type *ret_type = (call_ft && call_ft->kind == TYPE_FUNC)
                ? call_ft->func.return_type : NULL;
            Decl *ext_decl = (e->call.resolved_callee &&
                              e->call.resolved_callee->kind == DECL_EXTERN)
                ? e->call.resolved_callee->decl : NULL;
            ExternProtocol proto = ext_decl ? ext_decl->ext.protocol
                                            : EXT_PROTO_NONE;
            if (proto > EXT_PROTO_ERROR && ret_type &&
                ret_type->kind == TYPE_RESULT)
                emit_protocol_extern_call(e, fn_name, proto, call_ft,
                                          ret_type, out);
            else
                emit_raw_extern_call(e, fn_name, call_ft, ret_type, out);
        } else {
            /* Fallback: emit normally */
            emit_expr(e->call.func, out);
            fprintf(out, "(");
            for (int i = 0; i < e->call.arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_expr(e->call.args[i], out);
            }
            fprintf(out, ")");
        }
        if (aseq.active) {
            fprintf(out, "; })");
            seq_restore(aseq.slots, aseq.n, aseq.saved);
        }
    }
    for (int i = 0; unwidened && i < e->call.arg_count; i++)
        if (unwidened[i]) e->call.args[i] = unwidened[i];
}

/* A field access: a type property, extern constant, module member, variant
 * constructor, or a value field (a fixed-array field is viewed as a slice). */
static void emit_field(Expr *e, FILE *out) {
    /* Module member access: emit mangled name directly */
    if (e->field.codegen_name) {
        if (e->type && e->type->kind == TYPE_FUNC) {
            /* Module function used as a value: wrap in a fat pointer */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                e->field.codegen_name);
        } else if (is_cstr_type(e->type)) {
            /* C string #defines are char*; FC cstr is uint8_t*, so cast at the boundary.
             * Outer parens are required so a following postfix (e.g. `c.s[0]`)
             * binds to the cast result, not the raw #define literal. */
            fprintf(out, "((%s uint8_t*)(%s))",
                (e->type->is_const ? "const" : ""), e->field.codegen_name);
        } else {
            fprintf(out, "%s", e->field.codegen_name);
        }
        return;
    }
    /* No-payload variant constructor: color.green emits
     * (fc__color){ .tag = fc_tv_green__fc__color } */
    if (e->field.is_variant_constructor) {
        if (e->type && e->type->kind == TYPE_ENUM) {
            emit_enum_variant_literal(e->type, e->field.name, out);
            return;
        }
        const char *union_name = aggregate_c_name(e->type);
        fprintf(out, "(%s){ .tag = %s }",
            union_name, union_tag_value(union_name, e->field.name));
        return;
    }
    /* Type variable property access ('a.min): resolve via g_subst */
    if (e->field.object->kind == EXPR_TYPE_VAR_REF && g_subst) {
        Type *concrete = subst_lookup(e->field.object->type_var_ref.name);
        if (concrete) {
            const char *cstr = type_property_c(concrete, e->field.name);
            if (cstr) {
                fprintf(out, "%s", cstr);
                return;
            }
            diag_error(e->loc, "type '%s' has no property '%s'",
                type_name(concrete), e->field.name);
            fprintf(out, "0 /* error */");
            return;
        }
    }
    /* Fixed-array field: create slice view */
    if (e->field.fixed_array_type) {
        Type *fat = e->field.fixed_array_type;
        /* Strip const from the emitted slice type; FC enforces const itself */
        Type *slice_type = e->type;
        if (slice_type->is_const) {
            slice_type = type_slice(g_arena, fat->fixed_array.elem);
        }
        fprintf(out, "(");
        emit_type(slice_type, out);
        fprintf(out, "){ .ptr = (");
        emit_type(fat->fixed_array.elem, out);
        fprintf(out, "*)");
        emit_expr(e->field.object, out);
        fprintf(out, ".%s, .len = %lld }", c_safe_ident(g_intern, e->field.name),
                (long long)fixarr_size(fat));
        return;
    }
    /* Option .is_some / .is_none synthetic fields */
    if (e->field.object->type && e->field.object->type->kind == TYPE_OPTION) {
        Type *opt_type = e->field.object->type;
        if (strcmp(e->field.name, "is_some") == 0) {
            if (is_null_sentinel(opt_type)) {
                fprintf(out, "(");
                emit_expr(e->field.object, out);
                fprintf(out, " != NULL)");
            } else {
                emit_expr(e->field.object, out);
                fprintf(out, ".has_value");
            }
            return;
        }
        if (strcmp(e->field.name, "is_none") == 0) {
            if (is_null_sentinel(opt_type)) {
                fprintf(out, "(");
                emit_expr(e->field.object, out);
                fprintf(out, " == NULL)");
            } else {
                fprintf(out, "(!");
                emit_expr(e->field.object, out);
                fprintf(out, ".has_value)");
            }
            return;
        }
    }
    /* Result .is_ok / .is_err synthetic fields (ok is err == 0) */
    if (e->field.object->type && e->field.object->type->kind == TYPE_RESULT) {
        if (strcmp(e->field.name, "is_ok") == 0) {
            fprintf(out, "(");
            emit_expr(e->field.object, out);
            fprintf(out, ".err == 0)");
            return;
        }
        if (strcmp(e->field.name, "is_err") == 0) {
            fprintf(out, "(");
            emit_expr(e->field.object, out);
            fprintf(out, ".err != 0)");
            return;
        }
    }
    /* Slice .len read: stored at fc_len_t width, FC type i64, so widen at
     * every read (a no-op cast at --len-repr 64). */
    if (e->field.object->type && e->field.object->type->kind == TYPE_SLICE &&
        strcmp(e->field.name, "len") == 0) {
        fprintf(out, "((int64_t)");
        emit_expr(e->field.object, out);
        fprintf(out, ".len)");
        return;
    }
    emit_expr(e->field.object, out);
    fprintf(out, ".%s", c_safe_ident(g_intern, e->field.name));
}

/* A postfix `!` (unwrap, aborting on none or err) or `?` (propagate a
 * failure to the caller after running the pending defers). */
static void emit_unary_postfix(Expr *e, FILE *out) {
    if (e->unary_postfix.op == TOK_BANG) {
        /* Unwrap an option or result, aborting with a diagnostic on failure */
        Type *opt_type = e->unary_postfix.operand->type;
        Type *rt = subst_resolve(opt_type);
        if (rt && rt->kind == TYPE_RESULT) {
            /* Result unwrap: check the tag, abort with the code if err.
             * (long long)/%lld keeps the print int-width-agnostic. */
            fprintf(out, "({ ");
            emit_type(rt, out);
            int tid = g_temp_counter++;
            fprintf(out, " _uw%d = ", tid);
            emit_expr(e->unary_postfix.operand, out);
            fprintf(out, "; if (_uw%d.err != 0) { ", tid);
            /* Under --backtraces the error-name table is present, so print
             * the qualified name alongside the code; lean builds print
             * the number only. */
            bool named = g_backtraces && g_errname_emitted &&
                         error_code_count() > 0;
            if (named)
                fprintf(out, "const fc_errname *_nm%d = fc_error_lookup(_uw%d.err); ",
                        tid, tid);
            emit_unwrap_failure_open(e, out);
            if (named)
                fprintf(out, " (error code %%lld%%s%%s)\\n\", (long long)_uw%d.err, "
                             "_nm%d ? \": \" : \"\", _nm%d ? _nm%d->name : \"\"); ",
                        tid, tid, tid, tid);
            else
                fprintf(out, " (error code %%lld)\\n\", (long long)_uw%d.err); ", tid);
            /* void! unwrap yields nothing: the statement expression ends
             * with the check and has void type. */
            if (rt->result.inner && rt->result.inner->kind == TYPE_VOID)
                fprintf(out, "FC_ABORT(); } })");
            else
                fprintf(out, "FC_ABORT(); } _uw%d.value; })", tid);
        } else {
            /* An option: T*? is a plain pointer, unwrapped by a null check;
             * any other T? by .has_value. */
            bool ptr = is_null_sentinel(opt_type);
            fprintf(out, "({ ");
            emit_type(ptr ? opt_type->option.inner : opt_type, out);
            int tid = g_temp_counter++;
            fprintf(out, " _uw%d = ", tid);
            emit_expr(e->unary_postfix.operand, out);
            fprintf(out, ptr ? "; if (!_uw%d) { " : "; if (!_uw%d.has_value) { ", tid);
            emit_unwrap_failure_open(e, out);
            fprintf(out, ptr ? "\\n\"); FC_ABORT(); } _uw%d; })"
                             : "\\n\"); FC_ABORT(); } _uw%d.value; })", tid);
        }
    } else if (e->unary_postfix.op == TOK_QUESTION) {
        /* Propagation: unwrap on success; on failure run every pending
         * defer (a propagation is a return) and return the failure rebuilt
         * at the enclosing function's return type (stamped by pass2).
         * GCC documents jumping out of a statement expression with `return`
         * as permitted; the temp stays in scope inside the if. */
        Type *ot = subst_resolve(e->unary_postfix.operand->type);
        Type *rt = subst_resolve(e->unary_postfix.prop_fn_ret);
        int tid = g_temp_counter++;
        fprintf(out, "({ ");
        if (ot->kind == TYPE_RESULT) {
            emit_type(ot, out);
            fprintf(out, " _pr%d = ", tid);
            emit_expr(e->unary_postfix.operand, out);
            fprintf(out, "; if (_pr%d.err != 0) { ", tid);
            emit_defers_to_func(out);
            fprintf(out, "return (");
            emit_type(rt, out);
            fprintf(out, "){ .err = _pr%d.err }; } ", tid);
            /* void! propagation yields nothing: the statement expression
             * ends with the check and has void type. */
            if (ot->result.inner && ot->result.inner->kind == TYPE_VOID)
                fprintf(out, "})");
            else
                fprintf(out, "_pr%d.value; })", tid);
        } else if (is_null_sentinel(ot)) {
            /* T*? is a plain pointer: the failure test is the null check and
             * the value is the pointer itself. */
            emit_type(ot->option.inner, out);
            fprintf(out, " _pr%d = ", tid);
            emit_expr(e->unary_postfix.operand, out);
            fprintf(out, "; if (!_pr%d) { ", tid);
            emit_defers_to_func(out);
            fprintf(out, "return ");
            emit_none_of_type(rt, out);
            fprintf(out, "; } _pr%d; })", tid);
        } else {
            emit_type(ot, out);
            fprintf(out, " _pr%d = ", tid);
            emit_expr(e->unary_postfix.operand, out);
            fprintf(out, "; if (!_pr%d.has_value) { ", tid);
            emit_defers_to_func(out);
            fprintf(out, "return ");
            emit_none_of_type(rt, out);
            fprintf(out, "; } _pr%d.value; })", tid);
        }
    }
}

/* A cast: numeric conversions (range-checked in a checked context), pointer
 * and enum conversions, and the str/cstr conversions. */
static void emit_cast(Expr *e, FILE *out) {
    F2iInfo f2i_info;
    /* str -> cstr: stack copy with null terminator */
    if (e->cast.operand->type && is_str_type(e->cast.operand->type) &&
        is_cstr_type(e->cast.target)) {
        int tid = g_temp_counter++;
        if (e->cast.buffer_size > 0) {
            /* (cstr[N]): truncating copy into a fixed N-byte backing array
             * hoisted to function entry: bounded, loop-safe. Copy at most
             * N-1 bytes, always NUL-terminate. */
            int n = e->cast.buffer_size;
            const char *bk = e->cast.codegen_backing_name;
            fprintf(out, "({ fc_str _sc%d = ", tid);
            emit_expr(e->cast.operand, out);
            if (g_overflow_checked) {
                /* `checked`: the clip is data loss on the overflow axis, so
                 * abort instead of truncating. pass2's
                 * expr_node_is_governed_overflow (cstr[N] arm) must agree. */
                fprintf(out, "; if (_sc%d.len > %d) fc_trunc(", tid, n - 1);
                emit_loc_args(e->loc, out);
                fprintf(out, ", \"(cstr[%d]) cast\", (long long)_sc%d.len, %d)", n, tid, n - 1);
            }
            fprintf(out, "; int64_t _cn%d = _sc%d.len < %d ? _sc%d.len : %d",
                    tid, tid, n - 1, tid, n - 1);
            fprintf(out, "; memcpy(%s, _sc%d.ptr, fc_to_size(_cn%d))", bk, tid, tid);
            fprintf(out, "; %s[_cn%d] = '\\0'; (uint8_t*)%s; })", bk, tid, bk);
        } else {
            /* Unbounded (cstr): only alloca((cstr) s) reaches here (pass2
             * rejects the cast anywhere else, and alloc((cstr) s) is emitted
             * by emit_alloc). A dynamic-stack copy with a terminator. */
            fprintf(out, "({ fc_str _sc%d = ", tid);
            emit_expr(e->cast.operand, out);
            fprintf(out, "; uint8_t *_cb%d = (uint8_t*)__builtin_alloca(fc_to_size(_sc%d.len + 1))", tid, tid);
            fprintf(out, "; memcpy(_cb%d, _sc%d.ptr, fc_to_size(_sc%d.len))", tid, tid, tid);
            fprintf(out, "; _cb%d[_sc%d.len] = '\\0'; (uint8_t*)_cb%d; })", tid, tid, tid);
        }
    /* cstr -> str: wrap pointer with strlen-computed length */
    } else if (e->cast.operand->type && is_cstr_type(e->cast.operand->type) &&
               is_str_type(e->cast.target)) {
        int tid = g_temp_counter++;
        bool src_const = e->cast.operand->type->is_const;
        fprintf(out, "({ %suint8_t *_cp%d = ", src_const ? "const " : "", tid);
        emit_expr(e->cast.operand, out);
        if (g_len_repr < 64) {
            /* strlen's result can exceed a narrow stored width; a slice
             * that big cannot exist, so trap at construction. */
            fprintf(out, "; (fc_str){ .ptr = %s_cp%d, .len = fc_chk_len(",
                    src_const ? "(uint8_t*)" : "", tid);
            emit_loc_args(e->loc, out);
            fprintf(out, ", (int64_t)strlen((const char*)_cp%d)) }; })", tid);
        } else {
            fprintf(out, "; (fc_str){ .ptr = %s_cp%d, .len = (int64_t)strlen((const char*)_cp%d) }; })",
                    src_const ? "(uint8_t*)" : "", tid, tid);
        }
    } else if (!g_guards_suppressed && e->cast.operand->type &&
               type_is_float(e->cast.operand->type) &&
               float_to_int_info(e->cast.target->kind, &f2i_info)) {
        /* float -> int: saturating conversion (NaN->0, clamp to range, else
         * truncate toward zero). A raw C cast here is UB out of range.
         * An enclosing `unguarded` opts out and falls through to the bare
         * cast below. */
        float_to_int_emit(&f2i_info, e->cast.operand, out);
    } else if (g_overflow_checked && e->cast.operand->type &&
               type_is_integer(type_enum_underlying(e->cast.operand->type)) &&
               type_is_integer(e->cast.target) &&
               !type_can_widen(type_enum_underlying(e->cast.operand->type),
                               e->cast.target)) {
        /* checked: an integer narrowing cast that can lose information aborts
         * out of range, instead of the bare (truncating) C cast below.
         * An enum operand narrows as its repr does, which is also how
         * pass2's expr_node_is_governed_overflow judges it. */
        emit_checked_int_narrow(type_enum_underlying(e->cast.operand->type),
                                e->cast.target,
                                e->cast.operand, e->loc, out);
    } else if (e->cast.operand->type &&
               ((cast_is_ptr_kind(e->cast.operand->type) && type_is_integer(e->cast.target)) ||
                (type_is_integer(e->cast.operand->type) && cast_is_ptr_kind(e->cast.target)))) {
        /* pointer<->integer: route through uintptr_t so the cast is exact
         * pointer-width on every target (no -Wpointer-to-int-cast /
         * -Wint-to-pointer-cast). pass2 restricts the integer side to
         * usize/isize; uintptr_t bridges to the C pointer width cleanly. */
        fprintf(out, "((");
        emit_type(e->cast.target, out);
        fprintf(out, ")(uintptr_t)");
        emit_expr(e->cast.operand, out);
        fprintf(out, ")");
    } else if (e->cast.target && e->cast.operand->type &&
               cast_is_const_only_noop(e->cast.target, e->cast.operand->type)) {
        emit_expr(e->cast.operand, out);
    } else {
        fprintf(out, "((");
        emit_type(e->cast.target, out);
        fprintf(out, ")");
        emit_expr(e->cast.operand, out);
        fprintf(out, ")");
    }
}

/* A for loop over a range or a slice, with a fresh defer scope per
 * iteration. */
static void emit_for(Expr *e, FILE *out) {
    if (e->for_expr.range_end) {
        /* Range iteration: for i in lo..hi. The end bound goes into a temp
         * so a side-effecting end expression (`for i in 0..get_end()`) runs
         * once, not on every iteration. pass2 has widened both endpoints to
         * the loop variable's type, so the start, end temp, `<` test, and
         * `++` all share `var_type`. */
        int tid = g_temp_counter++;
        Type *var_type = e->for_expr.iter->type;
        emit_type(var_type, out);
        fprintf(out, " _fe%d = ", tid);
        emit_expr(e->for_expr.range_end, out);
        fprintf(out, ";\n");
        emit_indent(out);
        fprintf(out, "for (");
        emit_type(var_type, out);
        const char *rvar = e->for_expr.var_codegen_name
            ? e->for_expr.var_codegen_name : c_safe_ident(g_intern, e->for_expr.var);
        fprintf(out, " %s = ", rvar);
        emit_expr(e->for_expr.iter, out);
        fprintf(out, "; %s < _fe%d; %s++) {\n", rvar, tid, rvar);
    } else {
        /* Collection iteration. The iterable goes into a temp so it is
         * evaluated once; the `.len` bound and the `.ptr[]` element read
         * both use the temp. */
        int tid = g_temp_counter++;
        Type *iter_type = e->for_expr.iter->type;

        emit_type(iter_type, out);
        fprintf(out, " _fs%d = ", tid);
        emit_expr(e->for_expr.iter, out);
        fprintf(out, ";\n");
        emit_indent(out);
        /* The element-iteration counter is internal (the index binding below
         * is a separate i64), so it runs at the stored length width. */
        fprintf(out, "for (fc_len_t _fi%d = 0; _fi%d < _fs%d.len; _fi%d++) {\n",
            tid, tid, tid, tid);
        g_indent_level++;

        /* Element binding, into a temp first when destructuring. The
         * (void) cast suppresses -Wunused-variable when the binding is
         * referenced only inside a %T{} interpolation, which reads the
         * binding's type rather than its value and so emits no runtime
         * use of the C variable. */
        emit_indent(out);
        Type *elem_type = iter_type->slice.elem;
        const char *elem_name = e->for_expr.var_pattern
            ? e->for_expr.elem_tmp
            : (e->for_expr.var_codegen_name ? e->for_expr.var_codegen_name
                                            : c_safe_ident(g_intern, e->for_expr.var));
        emit_type(elem_type, out);
        fprintf(out, " %s = _fs%d.ptr[_fi%d];\n", elem_name, tid, tid);
        emit_indent(out);
        fprintf(out, "(void)%s;\n", elem_name);
        if (e->for_expr.var_pattern) {
            emit_pat_bindings(e->for_expr.var_pattern, elem_name, elem_type, out);
        }

        /* Index binding if present (same %T{}-only-use guard) */
        if (e->for_expr.index_var) {
            const char *iname = e->for_expr.index_codegen_name
                ? e->for_expr.index_codegen_name
                : c_safe_ident(g_intern, e->for_expr.index_var);
            emit_indent(out);
            fprintf(out, "int64_t %s = _fi%d;\n", iname, tid);
            emit_indent(out);
            fprintf(out, "(void)%s;\n", iname);
        }

        /* Body (already indented by g_indent_level++) */
        defer_scope_push(true);
        emit_block_stmts(e->for_expr.body, e->for_expr.body_count, out, false, true);
        defer_scope_pop();
        g_indent_level--;
        emit_indent(out);
        fprintf(out, "}");
        return;
    }

    /* Body for range iteration */
    g_indent_level++;
    defer_scope_push(true);
    emit_block_stmts(e->for_expr.body, e->for_expr.body_count, out, false, true);
    defer_scope_pop();
    g_indent_level--;
    emit_indent(out);
    fprintf(out, "}");
}

/* An assignment, including a slice into a fixed-array field (copied with a
 * length check). */
static void emit_assign(Expr *e, FILE *out) {
    /* Assignment to a fixed-array field: length-checked memcpy. */
    Expr *target = e->assign.target;
    if ((target->kind == EXPR_FIELD || target->kind == EXPR_DEREF_FIELD) &&
        target->field.fixed_array_type) {
        Type *fat = target->field.fixed_array_type;
        int tid = g_temp_counter++;
        bool deref = target->kind == EXPR_DEREF_FIELD;
        fprintf(out, "({ ");
        /* Evaluate the destination object once and before the source
         * (left-to-right) by hoisting a pointer to the containing struct.
         * For a field through a pointer (EXPR_DEREF_FIELD) the object is
         * already a pointer; otherwise take its address. */
        emit_type(target->field.object->type, out);
        if (deref) fprintf(out, " _ao%d = (", tid);
        else       fprintf(out, " *_ao%d = &(", tid);
        emit_expr(target->field.object, out);
        fprintf(out, "); ");
        /* Evaluate source slice */
        emit_type(e->assign.value->type, out);
        fprintf(out, " _fas%d = ", tid);
        emit_expr(e->assign.value, out);
        fprintf(out, "; if (_fas%d.len > %lld) { fprintf(stderr, \"", tid,
                (long long)fixarr_size(fat));
        emit_loc_text(e->loc, out);
        fprintf(out, ": fixed-array field '%s' overflow: "
                     "len=%%lld capacity=%lld\\n\", "
                     "(long long)_fas%d.len); FC_ABORT(); } ",
                target->field.name,
                (long long)fixarr_size(fat), tid);
        /* memcpy the data */
        fprintf(out, "memcpy(_ao%d->%s, _fas%d.ptr, fc_to_size(_fas%d.len) * sizeof(",
                tid, c_safe_ident(g_intern, target->field.name), tid, tid);
        emit_type(fat->fixed_array.elem, out);
        fprintf(out, ")); ");
        /* Zero-fill remainder */
        fprintf(out, "if (_fas%d.len < %lld) memset(_ao%d->%s + _fas%d.len, 0, "
                     "fc_to_size(%lld - _fas%d.len) * sizeof(",
                tid, (long long)fixarr_size(fat), tid,
                c_safe_ident(g_intern, target->field.name),
                tid, (long long)fixarr_size(fat), tid);
        emit_type(fat->fixed_array.elem, out);
        fprintf(out, ")); })");
        return;
    }
    /* Every other target, a slice element included (EXPR_INDEX emits an
     * lvalue), takes the generic path.
     *
     * When the target subexpressions or the value have side effects, force
     * left-to-right order (target's lvalue subexpressions, then the value):
     * take the target's address first, then evaluate the value, then store.
     * &(target) is well-defined for every assignment target (it is an
     * lvalue), and the index/field lowering inside it is already
     * left-to-right. */
    /* A slice's element storage is spelled modulo const (emit_elem_type),
     * so a store into a slot has to strip the qualifier the value carries:
     * the slot's C type never had it. Only a writable slice of read-only
     * views ((const i32*)[]) reaches here; pass2 rejects assignment through
     * a read-only slice. */
    bool slice_slot = e->assign.target->kind == EXPR_INDEX &&
                      e->assign.target->index.object->type &&
                      e->assign.target->index.object->type->kind == TYPE_SLICE &&
                      elem_const_shows_in_c(e->assign.target->type);
    if (!g_const_context &&
        (expr_has_side_effects(e->assign.value) ||
         expr_has_side_effects(e->assign.target))) {
        int tid = g_temp_counter++;
        fprintf(out, "({ ");
        if (slice_slot) emit_elem_type(e->assign.target->type, out);
        else emit_type(e->assign.target->type, out);
        fprintf(out, " *_at%d = &(", tid);
        emit_expr(e->assign.target, out);
        fprintf(out, "); ");
        if (slice_slot) emit_elem_type(e->assign.value->type, out);
        else emit_type(e->assign.value->type, out);
        fprintf(out, " _av%d = ", tid);
        if (slice_slot) emit_elem_store_cast(e->assign.value->type, out);
        emit_expr(e->assign.value, out);
        fprintf(out, "; *_at%d = _av%d; })", tid, tid);
    } else {
        emit_expr(e->assign.target, out);
        fprintf(out, " = ");
        if (slice_slot) emit_elem_store_cast(e->assign.value->type, out);
        emit_expr(e->assign.value, out);
    }
}

/* A slice literal T[N] { ... } over its hoisted backing array. */
static void emit_array_lit(Expr *e, FILE *out) {
    /* Module-scope const context: emit a slice header referencing the
     * static backing array that the pre-pass lifted to file scope.
     * Only zero-length literals have no backing (C rejects zero-length
     * arrays); those emit a NULL/0 slice.  The empty form `T[N]{ }`
     * with N > 0 references a `{0}`-initialized backing. */
    if (g_const_context) {
        fprintf(out, "(");
        emit_type(e->type, out);
        if (!e->array_lit.codegen_backing_name) {
            fprintf(out, "){ .ptr = 0, .len = ");
            emit_expr(e->array_lit.size_expr, out);
            fprintf(out, " }");
        } else {
            fprintf(out, "){ .ptr = ");
            if (e->array_lit.codegen_backing_rodata) {
                fprintf(out, "(");
                emit_elem_type(e->array_lit.elem_type, out);
                fprintf(out, "*)");
            }
            fprintf(out, "%s, .len = ", e->array_lit.codegen_backing_name);
            emit_expr(e->array_lit.size_expr, out);
            fprintf(out, " }");
        }
        return;
    }
    /* Stack slice literal: a slice over backing storage.
     *
     * The common case uses a fixed C array hoisted to function entry
     * (codegen_backing_name, set during the hoist pass): its single slot is
     * reused on every loop iteration, so stack use is bounded.  Zero-length
     * literals (and any not reached by the hoist pass) fall back to
     * __builtin_alloca.  Both give the backing function-frame lifetime: the
     * memory must outlive this statement-expression because the produced
     * slice escapes it (e.g. into a `let` binding used later). */
    int tid = g_temp_counter++;
    const char *bk = e->array_lit.codegen_backing_name;
    char arrname[24];
    const char *tgt;
    fprintf(out, "({ ");
    if (bk) {
        tgt = bk;
    } else {
        snprintf(arrname, sizeof arrname, "_arr%d", tid);
        tgt = arrname;
        emit_elem_type(e->array_lit.elem_type, out);
        fprintf(out, " *%s = (", tgt);
        emit_elem_type(e->array_lit.elem_type, out);
        fprintf(out, "*)__builtin_alloca(fc_to_size(");
        emit_expr(e->array_lit.size_expr, out);
        fprintf(out, ") * sizeof(");
        emit_elem_type(e->array_lit.elem_type, out);
        fprintf(out, ")); ");
    }
    if (e->array_lit.elem_count == 0) {
        if (bk) {
            /* backing is a real array, so sizeof yields its byte count */
            fprintf(out, "memset(%s, 0, sizeof %s); ", tgt, tgt);
        } else {
            fprintf(out, "memset(%s, 0, fc_to_size(", tgt);
            emit_expr(e->array_lit.size_expr, out);
            fprintf(out, ") * sizeof(");
            emit_elem_type(e->array_lit.elem_type, out);
            fprintf(out, ")); ");
        }
    } else {
        for (int i = 0; i < e->array_lit.elem_count; i++) {
            fprintf(out, "%s[%d] = ", tgt, i);
            emit_elem_store_cast(e->array_lit.elem_type, out);
            emit_expr(e->array_lit.elems[i], out);
            fprintf(out, "; ");
        }
    }
    fprintf(out, "(");
    emit_type(e->type, out);
    fprintf(out, "){ .ptr = %s, .len = ", tgt);
    emit_expr(e->array_lit.size_expr, out);
    fprintf(out, " }; })");
}

/* A prefix operator: negation, not, bitwise not, dereference or address-of. */
static void emit_unary_prefix(Expr *e, FILE *out) {
    /* Resolve a type-var result type (see emit_binary_op): the negation-wrap,
     * checked-negation, and sub-int NOT decisions need the concrete type. */
    Type *rt = subst_resolve(e->type);
    /* Signed negation. `checked`: only INT_MIN overflows (-INT_MIN is
     * unrepresentable), detected with __builtin_sub_overflow(0, x). */
    if (e->unary_prefix.op == TOK_MINUS && rt && type_is_signed(rt) &&
        g_overflow_checked) {
        int tid = g_temp_counter++;
        fprintf(out, "({ ");
        emit_type(rt, out);
        fprintf(out, " _r%d; if (__builtin_sub_overflow((", tid);
        emit_type(rt, out);
        fprintf(out, ")0, ");
        emit_expr(e->unary_prefix.operand, out);
        fprintf(out, ", &_r%d)) fc_overflow(", tid);
        emit_loc_args(e->loc, out);
        fprintf(out, ", \"negation\"); _r%d; })", tid);
        return;
    }
    /* Signed negation wrapping (unchecked): (int32_t)(-(uint32_t)x) */
    if (e->unary_prefix.op == TOK_MINUS && rt && type_is_signed(rt)) {
        const char *ut = unsigned_counterpart(rt);
        fprintf(out, "(");
        emit_type(rt, out);
        fprintf(out, ")(-((%s)", ut);
        emit_expr(e->unary_prefix.operand, out);
        fprintf(out, "))");
        return;
    }
    /* Bitwise NOT on sub-int types: cast the result back, since C promotes
     * the operand to int. */
    if (e->unary_prefix.op == TOK_TILDE && rt && type_is_integer(rt) &&
        (rt->kind == TYPE_INT8  || rt->kind == TYPE_UINT8 ||
         rt->kind == TYPE_INT16 || rt->kind == TYPE_UINT16)) {
        fprintf(out, "((");
        emit_type(rt, out);
        fprintf(out, ")(~");
        emit_expr(e->unary_prefix.operand, out);
        fprintf(out, "))");
        return;
    }
    /* &f on a top-level function or non-capturing lambda: emit the raw
     * C-boundary trampoline instead of the address of a fat-pointer literal.
     * This is the C-interop escape hatch documented in the spec. */
    if (e->unary_prefix.op == TOK_AMP) {
        Expr *operand = e->unary_prefix.operand;
        if (operand->kind == EXPR_IDENT && !operand->ident.is_local &&
            operand->type && operand->type->kind == TYPE_FUNC) {
            const char *name = operand->ident.codegen_name
                ? operand->ident.codegen_name : operand->ident.name;
            fprintf(out, "fc_ctramp_%s", name);
            return;
        }
        if (operand->kind == EXPR_FIELD && operand->field.codegen_name &&
            operand->type && operand->type->kind == TYPE_FUNC) {
            fprintf(out, "fc_ctramp_%s", operand->field.codegen_name);
            return;
        }
        if (operand->kind == EXPR_FUNC && operand->func.capture_count == 0 &&
            operand->func.lifted_name && operand->type &&
            operand->type->kind == TYPE_FUNC) {
            fprintf(out, "fc_ctramp_%s", lambda_c_name(operand));
            return;
        }
    }
    const char *op_str;
    switch (e->unary_prefix.op) {
    case TOK_MINUS: op_str = "-"; break;
    case TOK_BANG:  op_str = "!"; break;
    case TOK_TILDE: op_str = "~"; break;
    case TOK_AMP:   op_str = "&"; break;
    case TOK_STAR:  op_str = "*"; break;
    default: op_str = "?"; break;
    }
    fprintf(out, "(%s", op_str);
    emit_expr(e->unary_prefix.operand, out);
    fprintf(out, ")");
}

static void emit_expr(Expr *e, FILE *out) {
    switch (e->kind) {
    case EXPR_STATIC_ASSERT:
        /* Proven at compile time (pass2 for concrete conditions, mono_register
         * per instantiation); a no-op in the emitted C. */
        fprintf(out, "((void)0)");
        break;
    case EXPR_TYPE_VAR_REF: {
        /* A const generic param in expression position: emit the bound value
         * as a plain int literal (typed i32 in pass2; range-checked per
         * instance in validate_generic_body). */
        Type tv = {0};
        tv.kind = TYPE_TYPE_VAR;
        tv.type_var.name = e->type_var_ref.name;
        Type *bound = subst_resolve(&tv);
        if (e->type_var_ref.is_const_param && bound &&
            bound->kind == TYPE_CONST_INT) {
            fprintf(out, "%" PRId64, bound->const_int.value);
            break;
        }
        /* 'a.prop objects are handled at EXPR_FIELD; pass2 rejects a bare
         * non-const 'a. */
        internal_error(e->loc, "unresolved type variable %s", e->type_var_ref.name);
        break;
    }
    case EXPR_INT_LIT:
        emit_int_lit(e, false, out);
        break;

    case EXPR_FLOAT_LIT: {
        /* Use enough precision to round-trip IEEE 754 doubles (17 digits)
         * and floats (9 digits). %g may strip the decimal point (0.0 prints
         * as "0"), and "0f" is invalid C, so add ".0" when it is missing. */
        char fbuf[64];
        int prec = (e->float_lit.lit_type->kind == TYPE_FLOAT32) ? 9 : 17;
        snprintf(fbuf, sizeof(fbuf), "%.*g", prec, e->float_lit.value);
        bool has_dot = (strchr(fbuf, '.') || strchr(fbuf, 'e') || strchr(fbuf, 'E'));
        if (e->float_lit.lit_type->kind == TYPE_FLOAT32) {
            if (has_dot) fprintf(out, "%sf", fbuf);
            else fprintf(out, "%s.0f", fbuf);
        } else {
            if (has_dot) fprintf(out, "%s", fbuf);
            else fprintf(out, "%s.0", fbuf);
        }
        break;
    }

    case EXPR_BOOL_LIT:
        fprintf(out, "%s", e->bool_lit.value ? "true" : "false");
        break;

    case EXPR_VOID_LIT:
        fprintf(out, "((void)0)");
        break;

    case EXPR_CHAR_LIT:
        fprintf(out, "'\\x%02x'", e->char_lit.value);
        break;

    case EXPR_STRING_LIT:
        fprintf(out, "((fc_str){(uint8_t*)\"");
        emit_str_lit_body(e->string_lit.value, e->string_lit.length, out);
        fprintf(out, "\", %d})",
            str_lit_len(e->string_lit.value, e->string_lit.length));
        break;

    case EXPR_CSTRING_LIT:
        fprintf(out, "(uint8_t*)\"");
        emit_str_lit_body(e->cstring_lit.value, e->cstring_lit.length, out);
        fprintf(out, "\"");
        break;

    case EXPR_IDENT:
        if (e->ident.is_std_stream) {
            fprintf(out, "(void*)%s", e->ident.name);
            break;
        }
        if (e->type && e->type->kind == TYPE_FUNC &&
            !e->ident.is_local) {
            /* Top-level function used as a value: wrap in a fat pointer */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
        } else {
            fprintf(out, "%s", e->ident.codegen_name ? e->ident.codegen_name : e->ident.name);
        }
        break;

    case EXPR_BINARY:
        emit_binary(e, out); break;

    case EXPR_UNARY_PREFIX:
        emit_unary_prefix(e, out); break;

    case EXPR_UNARY_POSTFIX:
        emit_unary_postfix(e, out); break;

    case EXPR_CALL:
        emit_call(e, out); break;

    case EXPR_IF: {
        bool then_div = type_is_never(e->if_expr.then_body->type);
        bool else_div = e->if_expr.else_body && type_is_never(e->if_expr.else_body->type);
        if (type_valueless(e->type)) {
            /* Void- or never-typed if: a C if statement. Any control-flow exits in
               the branches fire inside; nothing is produced as a value. */
            emit_if_stmt(e, out);
        } else if (e->if_expr.else_body && !then_div && !else_div) {
            /* Value if/then/else where both branches produce values: ternary */
            fprintf(out, "(");
            emit_expr(e->if_expr.cond, out);
            fprintf(out, " ? ");
            emit_expr(e->if_expr.then_body, out);
            fprintf(out, " : ");
            emit_expr(e->if_expr.else_body, out);
            fprintf(out, ")");
        } else {
            /* Value if where one branch diverges (return/break/continue): a ternary
               can't hold a statement, so use a statement-expression with a result
               temp. The value branch assigns it; the diverging branch emits as
               statements (its exit fires before the temp is read). */
            int res_id = g_temp_counter++;
            char res_var[32];
            snprintf(res_var, sizeof(res_var), "_ifres%d", res_id);
            fprintf(out, "({\n");
            g_indent_level++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " %s;\n", res_var);
            emit_indent(out);
            if (emit_self_parens(e->if_expr.cond)) {
                fprintf(out, "if ");
                emit_expr(e->if_expr.cond, out);
                fprintf(out, " {\n");
            } else {
                fprintf(out, "if (");
                emit_expr(e->if_expr.cond, out);
                fprintf(out, ") {\n");
            }
            g_indent_level++;
            emit_branch_into(e->if_expr.then_body, res_var, out);
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "} else {\n");
            g_indent_level++;
            emit_branch_into(e->if_expr.else_body, res_var, out);
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
            emit_indent(out);
            fprintf(out, "%s;\n", res_var);
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "})");
        }
        break;
    }

    case EXPR_BLOCK: {
        /* Statement expression */
        fprintf(out, "({\n");
        g_indent_level++;
        defer_scope_push(false);
        emit_block_stmts(e->block.stmts, e->block.count, out, false, false);
        defer_scope_pop();
        g_indent_level--;
        emit_indent(out);
        fprintf(out, "})");
        break;
    }

    case EXPR_GUARD: {
        /* Toggle the relevant lexical context for the body, then restore. A
         * nested marker overrides its parent, so a plain save/restore is
         * enough. The two axes are independent globals. */
        if (e->guard.is_overflow_axis) {
            bool saved = g_overflow_checked;
            g_overflow_checked = e->guard.enable;   /* checked: on */
            emit_expr(e->guard.body, out);
            g_overflow_checked = saved;
        } else {
            bool saved = g_guards_suppressed;
            g_guards_suppressed = !e->guard.enable;  /* unguarded: suppress */
            emit_expr(e->guard.body, out);
            g_guards_suppressed = saved;
        }
        break;
    }

    case EXPR_CAST:
        emit_cast(e, out); break;

    case EXPR_FIELD:
        emit_field(e, out); break;

    case EXPR_DEREF_FIELD: {
        if (e->field.fixed_array_type) {
            Type *fat = e->field.fixed_array_type;
            /* Strip const from the emitted slice type; pass2 enforces const */
            Type *slice_type = e->type;
            if (slice_type->is_const) {
                slice_type = type_slice(g_arena, fat->fixed_array.elem);
            }
            fprintf(out, "(");
            emit_type(slice_type, out);
            fprintf(out, "){ .ptr = (");
            emit_type(fat->fixed_array.elem, out);
            fprintf(out, "*)");
            emit_expr(e->field.object, out);
            fprintf(out, "->%s, .len = %lld }", c_safe_ident(g_intern, e->field.name),
                    (long long)fixarr_size(fat));
            break;
        }
        /* Slice .len read through a pointer: same widening as the value form. */
        if (e->field.object->type && e->field.object->type->kind == TYPE_POINTER &&
            e->field.object->type->pointer.pointee &&
            e->field.object->type->pointer.pointee->kind == TYPE_SLICE &&
            strcmp(e->field.name, "len") == 0) {
            fprintf(out, "((int64_t)");
            emit_expr(e->field.object, out);
            fprintf(out, "->len)");
            break;
        }
        emit_expr(e->field.object, out);
        fprintf(out, "->%s", c_safe_ident(g_intern, e->field.name));
        break;
    }

    case EXPR_INDEX: {
        /* Bounds-checked slice access, an lvalue via pointer dereference.
         * Unsigned-compare fusion: a negative signed index cast to unsigned
         * is a huge value that compares greater than any valid len (slice
         * lengths are non-negative by invariant), so one compare covers both
         * the negative-index and past-end checks. */
        Type *obj_type = e->index.object->type;
        /* Tuple element access: object.e<const>. The index was proven to be a
         * literal in range during pass2, so no runtime bounds check is needed. */
        if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struc.is_tuple) {
            emit_expr(e->index.object, out);
            fprintf(out, ".e%llu", (unsigned long long)e->index.index->int_lit.value);
            break;
        }
        if (obj_type && obj_type->kind == TYPE_SLICE) {
            int tid = g_temp_counter++;
            fprintf(out, "(*({ ");
            emit_type(obj_type, out);
            fprintf(out, " _s%d = ", tid);
            emit_expr(e->index.object, out);
            fprintf(out, "; int64_t _i%d = (int64_t)", tid);
            emit_expr(e->index.index, out);
            if (g_guards_suppressed) {
                /* unguarded: bare access, no bounds check (UB out of range). */
                fprintf(out, "; _s%d.ptr + _i%d; }))", tid, tid);
            } else {
                /* Render the reported index with the source operand's own
                   signedness: a genuinely-negative signed index prints as -1,
                   while a huge usize (which int64_t would render as a bogus
                   negative) prints its true magnitude. The bounds compare is
                   unsigned either way. */
                bool idx_unsigned = type_is_unsigned(e->index.index->type);
                /* Compare at guard_bits width: for a narrow index at a narrow
                 * --len-repr this is one native compare (truncating _i%d's
                 * sign-extension preserves its value's bit pattern; the stored
                 * len is in [0, FC_LEN_MAX] by invariant). */
                int gb = guard_bits(e->index.index->type);
                fprintf(out, "; if (__builtin_expect((uint%d_t)_i%d >= (uint%d_t)_s%d.len, 0)) "
                             "%s(", gb, tid, gb, tid, idx_unsigned ? "fc_oob_u" : "fc_oob");
                emit_loc_args(e->loc, out);
                fprintf(out, ", (%s)_i%d, (unsigned long long)_s%d.len); "
                             "_s%d.ptr + _i%d; }))", idx_unsigned ? "unsigned long long" : "long long", tid, tid, tid, tid);
            }
        } else {
            /* Pointer indexing: no bounds check */
            emit_expr(e->index.object, out);
            fprintf(out, "[");
            emit_expr(e->index.index, out);
            fprintf(out, "]");
        }
        break;
    }

    case EXPR_SLICE: {
        /* Subslice: s[lo..hi].  Unsigned-compare fusion covers negative lo/hi
         * and reversed ranges in a single pair of compares:
         *   (unsigned)lo > (unsigned)hi  catches lo>hi and (if both negative)
         *     any case where |lo| < |hi|.
         *   (unsigned)hi > (unsigned)s.len catches past-end and negative hi
         *     (negative cast to unsigned is huge). */
        int tid = g_temp_counter++;
        Type *obj_type = e->slice.object->type;
        fprintf(out, "({ ");
        emit_type(obj_type, out);
        fprintf(out, " _s%d = ", tid);
        emit_expr(e->slice.object, out);
        fprintf(out, "; int64_t _lo%d = ", tid);
        if (e->slice.lo) {
            fprintf(out, "(int64_t)");
            emit_expr(e->slice.lo, out);
        } else {
            fprintf(out, "0");
        }
        fprintf(out, "; int64_t _hi%d = ", tid);
        if (e->slice.hi) {
            fprintf(out, "(int64_t)");
            emit_expr(e->slice.hi, out);
        } else {
            fprintf(out, "_s%d.len", tid);
        }
        if (g_guards_suppressed) {
            /* unguarded: no bounds check (UB on reversed/past-end range). */
            fprintf(out, "; ");
        } else {
            /* Width covers both bound expressions and the stored len. A NULL
             * lo is the constant 0 and a NULL hi is the len itself; both fit
             * the stored width, so they don't widen the compare. */
            int lw = g_len_repr >= 64 ? 64 : g_len_repr;
            int glo = e->slice.lo ? guard_bits(e->slice.lo->type) : lw;
            int ghi = e->slice.hi ? guard_bits(e->slice.hi->type) : lw;
            int gb = glo > ghi ? glo : ghi;
            fprintf(out, "; if (__builtin_expect("
                         "(uint%d_t)_lo%d > (uint%d_t)_hi%d || "
                         "(uint%d_t)_hi%d > (uint%d_t)_s%d.len, 0)) "
                         "fc_oob_sub(",
                gb, tid, gb, tid, gb, tid, gb, tid);
            emit_loc_args(e->loc, out);
            fprintf(out, ", (long long)_lo%d, (long long)_hi%d, (long long)_s%d.len); ",
                    tid, tid, tid);
        }
        fprintf(out, "(");
        emit_type(obj_type, out);
        fprintf(out, "){ .ptr = _s%d.ptr + _lo%d, .len = _hi%d - _lo%d }; })",
            tid, tid, tid, tid);
        break;
    }

    case EXPR_SOME: {
        if (is_null_sentinel(e->type)) {
            /* T*?/any*?/cstr? is a plain pointer: some(x) = x.  null would be
             * indistinguishable from none, so guard a not-provably-non-null
             * payload: evaluate once into a temp and abort if it is null.
             * Elided when the payload is provably non-null, and in const context
             * (a file-scope initializer cannot contain a statement-expression;
             * pass2 guarantees only provably-non-null payloads reach here). */
            Expr *val = e->some_expr.value;
            if (g_const_context || ptr_value_provably_nonnull(val)) {
                emit_expr(val, out);
            } else {
                int tid = g_temp_counter++;
                fprintf(out, "({ ");
                emit_type(e->type->option.inner, out);
                fprintf(out, " _sm%d = ", tid);
                emit_expr(val, out);
                fprintf(out, "; if (__builtin_expect(_sm%d == NULL, 0)) fc_null_some(", tid);
                emit_loc_args(e->loc, out);
                fprintf(out, "); _sm%d; })", tid);
            }
        } else {
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .value = ");
            emit_expr(e->some_expr.value, out);
            fprintf(out, ", .has_value = true }");
        }
        break;
    }

    case EXPR_OK:
        /* ok(v): err = 0 is the ok tag. Always the struct repr. Bare `ok`
         * (void!, which has no payload field) initializes the tag alone. */
        fprintf(out, "(");
        emit_type(e->type, out);
        if (!e->ok_expr.value) {
            fprintf(out, "){ .err = 0 }");
            break;
        }
        fprintf(out, "){ .err = 0, .value = ");
        emit_expr(e->ok_expr.value, out);
        fprintf(out, " }");
        break;

    case EXPR_ERR: {
        /* err(T, code): code 0 would read as ok, so guard a not-provably-
         * nonzero code: evaluate once into a temp and abort if zero. Elided
         * when the code is provably non-zero, and in const context (a
         * file-scope initializer cannot contain a statement-expression;
         * pass2 guarantees only provably-non-zero codes reach here). The
         * unmentioned .value field zero-fills (C11 designated initializer). */
        Expr *code = e->err_expr.code;
        if (g_const_context || int_value_provably_nonzero(code)) {
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .err = ");
            emit_expr(code, out);
            fprintf(out, " }");
        } else {
            int tid = g_temp_counter++;
            fprintf(out, "({ int32_t _ec%d = ", tid);
            emit_expr(code, out);
            fprintf(out, "; if (__builtin_expect(_ec%d == 0, 0)) fc_zero_err(", tid);
            emit_loc_args(e->loc, out);
            fprintf(out, "); (");
            emit_type(e->type, out);
            fprintf(out, "){ .err = _ec%d }; })", tid);
        }
        break;
    }

    case EXPR_ERROR_NAME: {
        /* error_name(e): some(qualified name) for a declared code, none
         * otherwise. The str points into the static fc_errnames table. */
        int tid = g_temp_counter++;
        fprintf(out, "({ const fc_errname *_en%d = fc_error_lookup(", tid);
        emit_expr(e->error_name_expr.code, out);
        fprintf(out, "); _en%d ? (", tid);
        emit_type(e->type, out);
        fprintf(out, "){ .value = { .ptr = (uint8_t *)_en%d->name, .len = _en%d->len }, "
                     ".has_value = true } : (", tid, tid);
        emit_type(e->type, out);
        fprintf(out, "){ .has_value = false }; })");
        break;
    }

    case EXPR_ARRAY_LIT:
        emit_array_lit(e, out); break;

    case EXPR_SLICE_LIT: {
        /* Slice construction from raw parts: (fc_slice_T){ .ptr = ptr, .len = len }
         *
         * A slice's length must be non-negative: the index/subslice bounds
         * checks fuse the negative-index test into an unsigned compare
         * ((unsigned)idx >= (unsigned)len), which a negative len defeats.
         * When pass2 could not prove len >= 0, guard a runtime len so a
         * negative value aborts at construction instead of corrupting every
         * later access.  Skipped in const context (file-scope initializer: len
         * is a pass2-verified constant, and a function call is not a constant
         * expression) and when pass2 proved len non-negative. */
        bool guard = !g_const_context && !e->slice_lit.len_nonneg;
        /* Under a narrow --len-repr, every runtime len also needs the
         * capacity check (a non-negative u32 can still exceed an i16 len);
         * pass2 judges compile-time lens statically. */
        Expr *len_e = e->slice_lit.len_expr;
        bool len_is_lit = len_e->kind == EXPR_INT_LIT ||
                          (len_e->kind == EXPR_CAST && len_e->cast.operand &&
                           len_e->cast.operand->kind == EXPR_INT_LIT);
        bool cap = !g_const_context && g_len_repr < 64 && !len_is_lit;
        Type *ptr_type = e->slice_lit.ptr_expr->type;
        bool const_ptr = ptr_type && ptr_type->kind == TYPE_POINTER && ptr_type->is_const;
        /* The guard hoists len into a temp; to keep ptr-before-len source order
         * (left-to-right) we then hoist ptr first.  Also sequence when both
         * operands have side effects even without the guard. */
        bool seq = guard || cap || (!g_const_context &&
                             expr_has_side_effects(e->slice_lit.ptr_expr) &&
                             expr_has_side_effects(e->slice_lit.len_expr));
        if (seq) {
            int tid = g_temp_counter++;
            fprintf(out, "({ ");
            /* ptr first (left-to-right) */
            emit_elem_type(e->slice_lit.elem_type, out);
            fprintf(out, " *_sp%d = ", tid);
            if (const_ptr) {
                fprintf(out, "(");
                emit_elem_type(e->slice_lit.elem_type, out);
                fprintf(out, "*)");
            }
            emit_expr(e->slice_lit.ptr_expr, out);
            fprintf(out, "; int64_t _sll%d = (", tid);
            emit_expr(e->slice_lit.len_expr, out);
            fprintf(out, "); ");
            if (guard) {
                fprintf(out, "if (__builtin_expect(_sll%d < 0, 0)) fc_neg_len(", tid);
                emit_loc_args(e->loc, out);
                fprintf(out, ", (long long)_sll%d); ", tid);
            }
            if (cap) {
                fprintf(out, "if (__builtin_expect(_sll%d > FC_LEN_MAX, 0)) fc_len_cap(", tid);
                emit_loc_args(e->loc, out);
                fprintf(out, ", (long long)_sll%d); ", tid);
            }
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .ptr = _sp%d, .len = _sll%d }; })", tid, tid);
            break;
        }
        fprintf(out, "(");
        emit_type(e->type, out);
        fprintf(out, "){ .ptr = ");
        /* Cast away const if source pointer is const (FC tracks constness at slice level) */
        if (const_ptr) {
            fprintf(out, "(");
            emit_elem_type(e->slice_lit.elem_type, out);
            fprintf(out, "*)");
        }
        emit_expr(e->slice_lit.ptr_expr, out);
        fprintf(out, ", .len = ");
        emit_expr(e->slice_lit.len_expr, out);
        fprintf(out, " }");
        break;
    }

    case EXPR_STRUCT_LIT:
        emit_struct_lit(e, out); break;

    case EXPR_TUPLE_LIT: {
        /* Anonymous tuple: a C compound literal of the synthesized struct, with
         * positional fields e0, e1, ... Multi-line (always >= 2 elements) so
         * nested statement-expressions (alloc(...)!, interpolated strings) don't
         * collapse into one over-long line that trips gcc's column tracking. */
        bool multiline = e->tuple_lit.elem_count >= 2;
        /* Force left-to-right element evaluation when an element has side
         * effects (C leaves initializer-list order unspecified). */
        SeqOperands tseq = seq_operands(expr_slots(e->tuple_lit.elems, e->tuple_lit.elem_count),
                                        e->tuple_lit.elem_count);
        if (tseq.active) {
            fprintf(out, "({ ");
            seq_hoist(tseq.slots, tseq.n, tseq.scratch, tseq.saved, out);
        }
        fprintf(out, "(");
        emit_type(e->type, out);
        fprintf(out, "){");
        if (multiline) { fprintf(out, "\n"); g_indent_level++; }
        else fprintf(out, " ");
        for (int i = 0; i < e->tuple_lit.elem_count; i++) {
            if (i > 0) {
                fprintf(out, ",");
                if (multiline) fprintf(out, "\n");
                else fprintf(out, " ");
            }
            if (multiline) emit_indent(out);
            fprintf(out, ".e%d = ", i);
            emit_expr(e->tuple_lit.elems[i], out);
        }
        if (multiline) {
            fprintf(out, "\n");
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        } else {
            fprintf(out, " }");
        }
        if (tseq.active) { fprintf(out, "; })"); seq_restore(tseq.slots, tseq.n, tseq.saved); }
        break;
    }

    case EXPR_LOOP: {
        if (e->type && e->type->kind != TYPE_VOID) {
            /* Loop as expression: produces value via break */
            fprintf(out, "({\n");
            g_indent_level++;
            emit_indent(out);
            emit_type(e->type, out);
            fprintf(out, " _loop_result;\n");
            emit_indent(out);
            fprintf(out, "while (1) {\n");
            g_indent_level++;
            defer_scope_push(true);
            emit_block_stmts(e->loop_expr.body, e->loop_expr.body_count, out, false, true);
            defer_scope_pop();
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}\n");
            emit_indent(out);
            fprintf(out, "_loop_result;\n");
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "})");
        } else {
            /* Void loop */
            fprintf(out, "while (1) {\n");
            g_indent_level++;
            defer_scope_push(true);
            emit_block_stmts(e->loop_expr.body, e->loop_expr.body_count, out, false, true);
            defer_scope_pop();
            g_indent_level--;
            emit_indent(out);
            fprintf(out, "}");
        }
        break;
    }

    case EXPR_FOR:
        emit_for(e, out); break;

    case EXPR_MATCH:
        emit_match(e, out); break;

    case EXPR_BREAK:
        if (has_pending_defers()) {
            fprintf(out, "({ ");
            if (e->break_expr.value) {
                emit_type(e->break_expr.value->type, out);
                int tid = g_temp_counter++;
                fprintf(out, " _brk%d = ", tid);
                emit_expr(e->break_expr.value, out);
                fprintf(out, "; ");
                emit_defers_to_loop(out);
                fprintf(out, "_loop_result = _brk%d; break; })", tid);
            } else {
                emit_defers_to_loop(out);
                fprintf(out, "break; })");
            }
        } else {
            if (e->break_expr.value) {
                fprintf(out, "_loop_result = ");
                emit_expr(e->break_expr.value, out);
                fprintf(out, "; break");
            } else {
                fprintf(out, "break");
            }
        }
        break;

    case EXPR_CONTINUE:
        if (has_pending_defers()) {
            fprintf(out, "({ ");
            emit_defers_to_loop(out);
            fprintf(out, "continue; })");
        } else {
            fprintf(out, "continue");
        }
        break;

    case EXPR_RETURN:
        if (has_pending_defers()) {
            fprintf(out, "({ ");
            if (e->return_expr.value) {
                emit_type(e->return_expr.value->type, out);
                int tid = g_temp_counter++;
                fprintf(out, " _ret%d = ", tid);
                emit_expr(e->return_expr.value, out);
                fprintf(out, "; ");
                emit_defers_to_func(out);
                fprintf(out, "return _ret%d; })", tid);
            } else {
                emit_defers_to_func(out);
                fprintf(out, "return; })");
            }
        } else {
            if (e->return_expr.value) {
                fprintf(out, "return ");
                emit_expr(e->return_expr.value, out);
            } else {
                fprintf(out, "return");
            }
        }
        break;

    case EXPR_IGNORE:
        /* Evaluate the operand for its side effects, ignore the value, yield
         * void. Valid as a bare statement and as a (void-typed) block tail. */
        fprintf(out, "(void)(");
        emit_expr(e->ignore_expr.value, out);
        fprintf(out, ")");
        break;

    case EXPR_DEFER:
        /* Handled in emit_block_stmts; should not reach here */
        break;

    case EXPR_SIZEOF: {
        fprintf(out, "(int64_t)sizeof(");
        emit_type(e->sizeof_expr.target, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_ALIGNOF: {
        fprintf(out, "(int64_t)_Alignof(");
        emit_type(e->alignof_expr.target, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_BITCAST: {
        /* Reinterpret the operand's bytes as the target scalar type via a C11
         * union compound literal, the defined type-pun (a pointer-cast
         * reinterpret is strict-aliasing UB). pass2 has verified both sides
         * are equal-size fixed-width scalars, so the union has no padding and
         * reading `.to` yields the operand's bytes. GCC and clang lower this
         * to a plain register move. */
        fprintf(out, "(((union { ");
        emit_type(e->bitcast_expr.operand->type, out);
        fprintf(out, " from; ");
        emit_type(e->bitcast_expr.target, out);
        fprintf(out, " to; }){ .from = ");
        emit_expr(e->bitcast_expr.operand, out);
        fprintf(out, " }).to)");
        break;
    }

    case EXPR_ENUM_OF: {
        /* Membership check via the per-enum helper pair. u64/usize operands
         * take the unsigned entry point; everything else fits int64. */
        Type *ot = e->enum_of_expr.operand->type;
        bool unsigned_wide = ot && (ot->kind == TYPE_UINT64 || ot->kind == TYPE_USIZE);
        fprintf(out, "fc_enum_of_%s_%s(", e->enum_of_expr.target->enu.name,
            unsigned_wide ? "u" : "i");
        emit_expr(e->enum_of_expr.operand, out);
        fprintf(out, ")");
        break;
    }

    case EXPR_DEFAULT: {
        Type *t = e->default_expr.target;
        switch (t->kind) {
        case TYPE_INT8: case TYPE_INT16: case TYPE_INT32: case TYPE_INT64:
        case TYPE_UINT8: case TYPE_UINT16: case TYPE_UINT32: case TYPE_UINT64:
        case TYPE_ISIZE: case TYPE_USIZE:
            fprintf(out, "0");
            break;
        case TYPE_FLOAT32:
            fprintf(out, "0.0f");
            break;
        case TYPE_FLOAT64:
            fprintf(out, "0.0");
            break;
        case TYPE_BOOL:
            fprintf(out, "false");
            break;
        case TYPE_POINTER:
        case TYPE_ANY_PTR:
            fprintf(out, "NULL");
            break;
        case TYPE_ENUM:
            /* The mandatory zero variant: default is zero-filled memory. */
            fprintf(out, "((%s)0)", t->enu.name);
            break;
        case TYPE_OPTION:
            if (is_null_sentinel(t))
                fprintf(out, "NULL");
            else {
                fprintf(out, "(");
                emit_type(t, out);
                fprintf(out, "){ .has_value = false }");
            }
            break;
        default: {
            /* Structs, unions, slices: a compound literal with a zero
             * initializer. Results (T!) take this path too: all-zeros is
             * err == 0 with a zero-filled payload, which is ok(default(T)).
             * For every FC type, default is zero-filled memory.
             * The zero is wrapped in one brace level per aggregate
             * along the first-member chain ((T){{0}} when the first field is
             * an array): gcc's -Wmissing-braces accepts the bare (T){0} idiom
             * at statement level but not nested inside another initializer
             * (e.g. as a union variant payload). */
            int extra = zero_brace_extra(t);
            fprintf(out, "(");
            emit_type(t, out);
            fprintf(out, "){");
            for (int i = 0; i < extra; i++) fputc('{', out);
            fputc('0', out);
            for (int i = 0; i < extra; i++) fputc('}', out);
            fprintf(out, "}");
            break;
        }
        }
        break;
    }

    case EXPR_FREE: {
        Type *ot = e->free_expr.operand->type;
        if (ot && ot->kind == TYPE_SLICE) {
            fprintf(out, "free((");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ").ptr)");
        } else if (ot && subst_resolve(ot)->kind == TYPE_FUNC) {
            /* Heap closure: the heap block is the context struct. free(NULL) is a
             * no-op, so freeing a non-capturing function value is harmless. */
            fprintf(out, "free((");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ").ctx)");
        } else {
            fprintf(out, "free(");
            emit_expr(e->free_expr.operand, out);
            fprintf(out, ")");
        }
        break;
    }

    case EXPR_ASSERT: {
        fprintf(out, "({ if (!");
        if (e->assert_expr.condition->kind != EXPR_BINARY) fprintf(out, "(");
        emit_expr(e->assert_expr.condition, out);
        if (e->assert_expr.condition->kind != EXPR_BINARY) fprintf(out, ")");
        fprintf(out, ") { ");
        if (e->assert_expr.message) {
            int tid = g_temp_counter++;
            fprintf(out, "fc_str _am%d = ", tid);
            emit_expr(e->assert_expr.message, out);
            fprintf(out, "; fprintf(stderr, \"");
            emit_loc_text(e->loc, out);
            fprintf(out, ": assertion failed: ");
            emit_c_escaped(e->assert_expr.expr_text, e->assert_expr.expr_text_len, out);
            fprintf(out, ": %%.*s\\n\", fc_to_int(_am%d.len), (const char*)_am%d.ptr); ", tid, tid);
        } else {
            fprintf(out, "fprintf(stderr, \"");
            emit_loc_text(e->loc, out);
            fprintf(out, ": assertion failed: ");
            emit_c_escaped(e->assert_expr.expr_text, e->assert_expr.expr_text_len, out);
            fprintf(out, "\\n\"); ");
        }
        fprintf(out, "FC_ABORT(); } })");
        break;
    }

    case EXPR_ATOMIC_LOAD: {
        Type *cell = e->atomic_load.ptr->type->pointer.pointee;
        fprintf(out, "({ _Static_assert(__atomic_always_lock_free(sizeof(");
        emit_type(cell, out);
        fprintf(out, "), 0), \"FC atomics require lock-free access for this type on the target\"); "
                     "__atomic_load_n((");
        emit_expr(e->atomic_load.ptr, out);
        fprintf(out, "), __ATOMIC_ACQUIRE); })");
        break;
    }

    case EXPR_ATOMIC_STORE: {
        Type *cell = e->atomic_store.ptr->type->pointer.pointee;
        fprintf(out, "({ _Static_assert(__atomic_always_lock_free(sizeof(");
        emit_type(cell, out);
        fprintf(out, "), 0), \"FC atomics require lock-free access for this type on the target\"); "
                     "__atomic_store_n((");
        emit_expr(e->atomic_store.ptr, out);
        fprintf(out, "), (");
        emit_expr(e->atomic_store.value, out);
        fprintf(out, "), __ATOMIC_RELEASE); })");
        break;
    }

    case EXPR_ALLOC:
        emit_alloc(e, out); break;

    case EXPR_ASSIGN:
        emit_assign(e, out); break;

    case EXPR_LET: {
        const char *vname = e->let_expr.codegen_name ? e->let_expr.codegen_name : e->let_expr.let_name;
        if (is_hoisted(vname)) {
            fprintf(out, "%s = ", vname);
        } else {
            emit_type(e->let_expr.let_type, out);
            fprintf(out, " %s = ", vname);
        }
        emit_expr(e->let_expr.let_init, out);
        break;
    }

    case EXPR_FUNC: {
        /* Lambda in expression position: emit a fat pointer */
        if (e->func.capture_count > 0 && e->func.codegen_ctx_backing_name) {
            /* Capturing lambda: write the captures into the function-entry _ctx
             * backing (hoisted in collect_hoisted_bindings), then yield a fat
             * pointer whose .ctx addresses it.  &backing has function-frame
             * lifetime, so the closure stays valid for the whole call even when
             * this lambda is the tail of a nested block (let...in / if- / match-arm),
             * where an inline compound literal would get block-scope lifetime and
             * dangle.  The single slot is reused per loop iteration. */
            const char *bk = e->func.codegen_ctx_backing_name;
            fprintf(out, "({ ");
            for (int i = 0; i < e->func.capture_count; i++)
                fprintf(out, "%s.%s = %s; ",
                    bk,
                    e->func.captures[i].codegen_name,
                    e->func.captures[i].codegen_name);
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = &%s }; })",
                lambda_c_name(e), bk);
        } else if (e->func.capture_count > 0) {
            /* Fallback: no hoisted backing (e.g. emitted outside a hoisted scope).
             * An inline compound literal has block-scope lifetime, so this is
             * safe only when the lambda is consumed within the same block. */
            const char *ln = lambda_c_name(e);
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = &(_ctx_%s){ ",
                ln, ln);
            for (int i = 0; i < e->func.capture_count; i++) {
                if (i > 0) fprintf(out, ", ");
                fprintf(out, ".%s = %s",
                    e->func.captures[i].codegen_name,
                    e->func.captures[i].codegen_name);
            }
            fprintf(out, " } }");
        } else {
            /* Non-capturing lambda: NULL context */
            fprintf(out, "(");
            emit_type(e->type, out);
            fprintf(out, "){ .fn_ptr = %s, .ctx = NULL }",
                lambda_c_name(e));
        }
        break;
    }

    case EXPR_INTERP_STRING:
        emit_interp_string(e, out, NULL);
        break;


    case EXPR_ERROR:
        /* Tripwire: error nodes exist only when diag_error_count()>0, and codegen is
           gated behind a clean compile, so reaching here is a compiler bug. */
        assert(0 && "EXPR_ERROR reached codegen");
        break;

    default:
        internal_error(e->loc, "no emission for expression kind %d", e->kind);
        break;
    }
}

/* Check if a top-level decl is a function (its init expr is EXPR_FUNC) */
static bool is_func_decl(Decl *d) {
    return d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC;
}

static bool is_generic_decl(Decl *d) {
    if (d->kind == DECL_STRUCT) return d->struc.is_generic;
    if (d->kind == DECL_UNION) return d->unio.is_generic;
    if (d->kind == DECL_LET && d->let.init && d->let.init->kind == EXPR_FUNC) {
        /* Check if any param contains type vars */
        Expr *fn = d->let.init;
        if (fn->func.explicit_type_var_count > 0) return true;
        for (int i = 0; i < fn->func.param_count; i++)
            if (type_contains_type_var(fn->func.params[i].type)) return true;
    }
    return false;
}

/* The program entry point: a file-scope `let main`. A module member spelled
 * `main` is an ordinary function (pass2's entry-point signature check does not
 * apply to it either), so it is not emitted as `fc_main` with an
 * `int main(int, char**)` wrapper; that path also reads params[0]
 * unconditionally. */
static bool is_entry_point(const Decl *d) {
    return d->kind == DECL_LET && !d->let.is_module_member &&
           strcmp(d->let.name, "main") == 0;
}

/* The C signature of an FC function, lambda or instance, through its closing
 * ')'. Every one takes a trailing context pointer, unused unless it captures. */
static void emit_fn_signature(const char *c_name, Type *ret, Param *params, int n,
                              FILE *out) {
    fprintf(out, "%s", g_fn_attr);
    emit_type(ret, out);
    fprintf(out, " %s(", c_name);
    for (int i = 0; i < n; i++) {
        emit_type(params[i].type, out);
        fprintf(out, " %s, ", param_c_name(&params[i]));
    }
    fprintf(out, "void* _ctx)");
}

static void emit_func_decl(Decl *d, FILE *out) {
    Expr *fn = d->let.init;
    Type *ft = d->let.resolved_type;
    const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
    bool is_main = is_entry_point(d);

    if (is_main) {
        /* Emit the FC main body as fc_main(str[] args).  Static: only the
         * int main(int, char**) wrapper (emitted below) calls it. */
        fprintf(out, "%sint32_t fc_main(", g_fn_attr);
        emit_type(fn->func.params[0].type, out);
        fprintf(out, " %s) {\n", param_c_name(&fn->func.params[0]));
    } else {
        emit_fn_signature(cname, ft->func.return_type, fn->func.params,
                          fn->func.param_count, out);
        fprintf(out, " {\n    (void)_ctx;\n");
    }

    g_indent_level = 1;
    g_guards_suppressed = false;   /* guards on at a function boundary: an
                                      `unguarded` marker never reaches a callee */
    g_overflow_checked = false;    /* unchecked at a function boundary too */
    begin_hoisted_scope(fn->func.body, fn->func.body_count, out);
    defer_scope_push(false);
    emit_block_stmts(fn->func.body, fn->func.body_count, out, true, true);
    defer_scope_pop();
    end_hoisted_scope();
    g_indent_level = 0;

    /* main returns 0 unless its body ends in a return or an integer value
     * (which emit_block_stmts already returned). */
    if (is_main && fn->func.body_count > 0) {
        Expr *last = fn->func.body[fn->func.body_count - 1];
        if (last->kind != EXPR_RETURN && !(last->type && type_is_integer(last->type)))
            fprintf(out, "    return 0;\n");
    }

    fprintf(out, "}\n\n");

    /* Emit C main wrapper that converts argc/argv to str[] */
    if (is_main) {
        fprintf(out, "int main(int argc, char **argv) {\n");
        /* Hoisted file-level global initializations */
        for (int gi = 0; gi < g_file_global_count; gi++) {
            Decl *gd = g_file_globals[gi];
            fprintf(out, "    %s = ",
                gd->let.codegen_name ? gd->let.codegen_name : gd->let.name);
            emit_expr(gd->let.init, out);
            fprintf(out, ";\n");
        }
        fprintf(out, "    fc_str *_args = (fc_str*)__builtin_alloca((size_t)argc * sizeof(fc_str));\n");
        fprintf(out, "    for (int _i = 0; _i < argc; _i++) {\n");
        fprintf(out, "        _args[_i].ptr = (uint8_t*)argv[_i];\n");
        if (g_len_repr < 64)
            fprintf(out, "        _args[_i].len = fc_chk_len(\"<startup>\", 0, (int64_t)strlen(argv[_i]));\n");
        else
            fprintf(out, "        _args[_i].len = (int64_t)strlen(argv[_i]);\n");
        fprintf(out, "    }\n");
        fprintf(out, "    return fc_main((");
        emit_type(fn->func.params[0].type, out);
        if (g_len_repr < 64)
            fprintf(out, "){ .ptr = _args, .len = fc_chk_len(\"<startup>\", 0, (int64_t)argc) });\n");
        else
            fprintf(out, "){ .ptr = _args, .len = (int64_t)argc });\n");
        fprintf(out, "}\n\n");
    }
}

static void emit_struct_forward(Decl *d, FILE *out) {
    fprintf(out, "typedef struct %s %s;\n", d->struc.name, d->struc.name);
}

static void emit_struct_field(Type *ft, const char *name, FILE *out) {
    if (ft->kind == TYPE_FIXED_ARRAY) {
        fprintf(out, " ");
        emit_type(ft->fixed_array.elem, out);
        fprintf(out, " %s[%lld];", name, (long long)fixarr_size(ft));
    } else {
        fprintf(out, " ");
        emit_type(ft, out);
        fprintf(out, " %s;", name);
    }
}

/* The C definition of a struct (a declaration's, or a generic instance's under
 * its instance name). */
static void emit_struct_def(const char *name, StructField *fields, int n, FILE *out) {
    fprintf(out, "struct %s {", name);
    for (int i = 0; i < n; i++)
        emit_struct_field(fields[i].type, c_safe_ident(g_intern, fields[i].name), out);
    fprintf(out, " };\n");
}

/* The value an enum variant's stored bits (two's complement, truncated to the
 * repr's width) denote in a signed repr. */
static int64_t enum_bits_signed(uint64_t bits, Type *repr) {
    int w = repr->kind == TYPE_INT8 ? 8 : repr->kind == TYPE_INT16 ? 16
          : repr->kind == TYPE_INT32 ? 32 : 64;
    uint64_t mask = (w == 64) ? UINT64_MAX : ((1ULL << w) - 1);
    return bits > (mask >> 1) ? (int64_t)(bits | ~mask) : (int64_t)bits;
}

/* Render an enum variant reference as a cast integer literal ((fc__E)N) in
 * the repr's signedness. INT64_MIN needs C's two-token spelling; 64-bit
 * magnitudes carry LL/ULL suffixes so the literal never exceeds `int`. */
static void emit_enum_variant_literal(Type *et, const char *vname, FILE *out) {
    uint64_t bits = 0;
    for (int i = 0; i < et->enu.variant_count; i++) {
        if (et->enu.variants[i].name == vname) {
            bits = et->enu.variants[i].value_bits;
            break;
        }
    }
    Type *repr = et->enu.repr ? et->enu.repr : type_int32();
    fprintf(out, "((%s)", et->enu.name);
    if (type_is_signed(repr)) {
        int64_t sv = enum_bits_signed(bits, repr);
        if (sv == INT64_MIN) fprintf(out, "(-9223372036854775807LL - 1)");
        else if (sv < 0) fprintf(out, "-%lldLL", (long long)-sv);
        else fprintf(out, "%lldLL", (long long)sv);
    } else {
        fprintf(out, "%lluULL", (unsigned long long)bits);
    }
    fprintf(out, ")");
}

/* An enum is a fixed-width integer typedef, never a C `enum`: C enum width is
 * implementation-defined (and `int`-sized ints are 16 bits on some FC targets),
 * while the declared repr is part of the language contract. Variant references
 * are emitted as cast literals, so the typedef is the whole definition. */
static void emit_enum_typedef(Decl *d, FILE *out) {
    fprintf(out, "typedef ");
    emit_type(d->enu.repr, out);
    fprintf(out, " %s;\n", d->enu.name);
}

static void emit_union_forward(Decl *d, FILE *out) {
    fprintf(out, "typedef struct %s %s;\n", d->unio.name, d->unio.name);
}

/* A union's tag enum, and (emit_union_def) its C definition: a declaration's,
 * or a generic instance's under its instance name. */
static void emit_union_tag_enum(const char *name, UnionVariant *variants, int n, FILE *out) {
    fprintf(out, "typedef enum {");
    for (int i = 0; i < n; i++) {
        if (i > 0) fprintf(out, ",");
        fprintf(out, " %s", union_tag_value(name, variants[i].name));
    }
    fprintf(out, " } %s;\n", union_tag_type(name));
}

static void emit_union_def(const char *name, UnionVariant *variants, int n, FILE *out) {
    bool has_any_payload = false;
    for (int i = 0; i < n; i++) {
        if (variants[i].payload) { has_any_payload = true; break; }
    }
    if (has_any_payload) {
        fprintf(out, "struct %s { %s tag; union {", name, union_tag_type(name));
        for (int i = 0; i < n; i++) {
            if (variants[i].payload) {
                fprintf(out, " ");
                emit_type(variants[i].payload, out);
                fprintf(out, " %s;", c_safe_ident(g_intern, variants[i].name));
            }
        }
        fprintf(out, " } " FC_PAYLOAD_MEMBER "; };\n");
    } else {
        /* Tag-only union (enum-like): no payload union needed */
        fprintf(out, "struct %s { %s tag; };\n", name, union_tag_type(name));
    }
}

/* ---- Collect used slice/option types for typedef generation ---- */

struct TypeSet {
    Type **types;
    int count;
    int cap;
};

/* Two types share one typedef iff they emit the same C ident (emit_type_ident).
 * That is not type_eq_ignore_const: const on a pointer or any* is part of the
 * ident (a read-only payload has a different C type, `T const*`), while const
 * on a slice element is not (slice storage is spelled modulo const), and named
 * types compare nominally. Keying the set on anything coarser merges two
 * distinct C types onto one typedef; the second then stores or passes the
 * wrong constness and the emitted C fails under -Wall -Werror. The comparison
 * is structural rather than rendered, so it needs no memstream. */
static bool type_ident_eq(Type *a, Type *b) {
    a = subst_resolve(a); b = subst_resolve(b);
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return type_eq_ignore_const(a, b);   /* stub vs. resolved struct */
    switch (a->kind) {
    case TYPE_POINTER:
        return a->is_const == b->is_const &&
               type_ident_eq(a->pointer.pointee, b->pointer.pointee);
    case TYPE_ANY_PTR:
        return a->is_const == b->is_const;
    case TYPE_SLICE: {
        /* As in emit_slice_elem_ident, the element is spelled modulo const. */
        Type *ea = subst_resolve(a->slice.elem), *eb = subst_resolve(b->slice.elem);
        if (!ea || !eb) return ea == eb;
        Type ba = *ea, bb = *eb;
        ba.is_const = bb.is_const = false;
        return type_ident_eq(&ba, &bb);
    }
    case TYPE_OPTION:
        return type_ident_eq(a->option.inner, b->option.inner);
    case TYPE_RESULT:
        return type_ident_eq(a->result.inner, b->result.inner);
    case TYPE_FIXED_ARRAY:
        return fixarr_size(a) == fixarr_size(b) &&
               type_ident_eq(a->fixed_array.elem, b->fixed_array.elem);
    case TYPE_FUNC: {
        if (a->func.param_count != b->func.param_count ||
            a->func.is_variadic != b->func.is_variadic) return false;
        for (int i = 0; i < a->func.param_count; i++)
            if (!type_ident_eq(a->func.param_types[i], b->func.param_types[i])) return false;
        return type_ident_eq(a->func.return_type, b->func.return_type);
    }
    default:
        return type_eq_ignore_const(a, b);
    }
}

static bool typeset_contains(TypeSet *ts, Type *t) {
    for (int i = 0; i < ts->count; i++) {
        if (type_ident_eq(ts->types[i], t)) return true;
    }
    return false;
}

static void typeset_add(TypeSet *ts, Type *t) {
    if (!typeset_contains(ts, t)) {
        DA_APPEND(ts->types, ts->count, ts->cap, t);
    }
}

static void collect_types_in_type(Type *t, TypeSet *slices, TypeSet *options, TypeSet *fns) {
    if (!t) return;
    /* Apply type variable substitution if available */
    if (g_subst && type_contains_type_var(t)) {
        t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
        if (!type_contains_type_var(t))
            mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    }
    if (type_contains_type_var(t)) return;  /* still has unresolved type vars */
    /* Resolve stubs before type classification */
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if (t->kind == TYPE_POINTER) {
        /* A pointer's C declarator names its pointee, so the pointee's typedef
         * must exist even where the pointee value itself never appears: the
         * `T*?` that `alloc(T)` yields may be the only occurrence of a nested
         * option like `i32??`, and a `i32??*` parameter is another.  Structs are
         * not collected here at all, so a self-referential `next: node*` still
         * terminates. */
        collect_types_in_type(t->pointer.pointee, slices, options, fns);
    } else if (t->kind == TYPE_FIXED_ARRAY) {
        /* Fixed-array field: need slice typedef for the element type (field access returns slice) */
        collect_types_in_type(t->fixed_array.elem, slices, options, fns);
        Type *slice_t = type_slice(g_arena, t->fixed_array.elem);
        typeset_add(slices, slice_t);
    } else if (t->kind == TYPE_SLICE) {
        collect_types_in_type(t->slice.elem, slices, options, fns);
        typeset_add(slices, t);
    } else if (t->kind == TYPE_OPTION) {
        /* Canonicalize a stub inner (box<int32>?) to its monomorphized struct so
         * the stored option's inner carries the mangled name. Two things depend
         * on this: (1) the def-ordering interleave matches the option to its inner
         * def by name, and (2) an option collected from a concrete field type
         * (inner = base-name stub) dedupes against the same option collected from
         * a some()/value expression (inner = resolved struct); otherwise both
         * emit `fc_option_box__5_int32`, a duplicate typedef. */
        Type *inner = t->option.inner;
        if (inner && inner->kind == TYPE_STUB) {
            Type *resolved = resolve_struct_stub(inner);
            if (resolved != inner) {
                t = type_option(g_arena, resolved);
                inner = resolved;
            }
        }
        /* Recurse into the inner type first so dependencies are emitted before this type */
        collect_types_in_type(inner, slices, options, fns);
        /* Only non-pointer options need typedefs */
        if (!inner || inner->kind != TYPE_POINTER) {
            typeset_add(options, t);
        }
    } else if (t->kind == TYPE_RESULT) {
        /* Same stub canonicalization + inner-first recursion as options; every
         * result needs a typedef (no pointer-sentinel exemption). */
        Type *inner = t->result.inner;
        if (inner && inner->kind == TYPE_STUB) {
            Type *resolved = resolve_struct_stub(inner);
            if (resolved != inner) {
                t = type_result(g_arena, resolved);
                inner = resolved;
            }
        }
        collect_types_in_type(inner, slices, options, fns);
        if (g_results_set) typeset_add(g_results_set, t);
    } else if (t->kind == TYPE_FUNC) {
        /* Same stub canonicalization the option/result arms do, for the same
         * reason: the emitted typedef name resolves a stub to its mangled
         * definition, but type_eq does not. So `(wide<64>) -> i32` written as a
         * struct field's type (a name-only stub) and the identical signature of
         * a declared function (resolved by pass2) would compare unequal and both
         * emit `fc_fn_fc__wide__5___k64__int32_t`.  C11 forbids redefining a
         * typedef to a distinct struct type, so the duplicate is an error. */
        Type **params = t->func.param_types;
        Type *ret = t->func.return_type;
        bool changed = false;
        for (int i = 0; i < t->func.param_count; i++) {
            Type *pt = t->func.param_types[i];
            if (!pt || pt->kind != TYPE_STUB) continue;
            Type *r = resolve_struct_stub(pt);
            if (r == pt) continue;
            if (!changed) {
                params = arena_alloc(g_arena,
                    sizeof(Type*) * (size_t)t->func.param_count);
                memcpy(params, t->func.param_types,
                    sizeof(Type*) * (size_t)t->func.param_count);
                changed = true;
            }
            params[i] = r;
        }
        if (ret && ret->kind == TYPE_STUB) {
            Type *r = resolve_struct_stub(ret);
            if (r != ret) { ret = r; changed = true; }
        }
        if (changed) {
            Type *nf = arena_alloc(g_arena, sizeof(Type));
            *nf = *t;
            nf->func.param_types = params;
            nf->func.return_type = ret;
            t = nf;
        }
        /* Recurse into param/return types first so dependencies are emitted before this type */
        for (int i = 0; i < t->func.param_count; i++)
            collect_types_in_type(t->func.param_types[i], slices, options, fns);
        collect_types_in_type(t->func.return_type, slices, options, fns);
        typeset_add(fns, t);
    }
}

/* Resolve TYPE_STUB types (unresolved name-only references) to full definitions */
static Type *resolve_struct_stub(Type *t) {
    if (!g_symtab) return t;
    if (t->kind == TYPE_STUB) {
        /* A concrete generic instance (e.g. box<int32>) used as a by-value field
         * resolves to its monomorphized instance, not the generic template (the
         * template is never emitted as a complete type). The field stub keeps its
         * base name (the pass2 type is shared with the symbol table, so it must
         * not be mutated); the mangled name is computed here for the lookup. */
        if (t->stub.type_arg_count > 0 && !type_contains_type_var(t)) {
            const char *mangled = mangle_generic_name(g_intern,
                t->stub.name, t->stub.type_args, t->stub.type_arg_count);
            if (g_mono) {
                for (int i = 0; i < g_mono->count; i++) {
                    if (g_mono->entries[i].mangled_name == mangled &&
                        g_mono->entries[i].concrete_type)
                        return g_mono->entries[i].concrete_type;
                }
            }
        }
        Symbol *sym = symtab_lookup(g_symtab, t->stub.name);
        if (sym && sym->type) {
            if ((sym->type->kind == TYPE_STRUCT && sym->type->struc.field_count > 0) ||
                (sym->type->kind == TYPE_UNION && sym->type->unio.variant_count > 0) ||
                sym->type->kind == TYPE_ENUM)
                return sym->type;
        }
        if (g_mono) {
            for (int i = 0; i < g_mono->count; i++) {
                if (g_mono->entries[i].mangled_name == t->stub.name &&
                    g_mono->entries[i].concrete_type)
                    return g_mono->entries[i].concrete_type;
            }
        }
    }
    return t;
}

/* Recursively collect types that need generated eq functions */
static void collect_eq_types(Type *t, TypeSet *eqs) {
    if (!t) return;
    /* Apply type variable substitution if available */
    if (g_subst && type_contains_type_var(t)) {
        t = type_substitute(g_arena, t, g_subst->var_names, g_subst->concrete, g_subst->count);
        if (!type_contains_type_var(t))
            mono_resolve_type_names(g_mono, g_arena, g_intern, t);
    }
    if (type_contains_type_var(t)) return;
    /* Resolve stubs before checking if eq func is needed */
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    /* Fixed-array types: recurse into element, don't generate standalone eq func */
    if (t->kind == TYPE_FIXED_ARRAY) {
        collect_eq_types(t->fixed_array.elem, eqs);
        return;
    }
    if (!type_needs_eq_func(t)) return;
    t = resolve_struct_stub(t);
    /* Strip const for eq function collection: constness doesn't affect equality */
    if (t->is_const) {
        Type *nc = arena_alloc(g_arena, sizeof(Type));
        *nc = *t;
        nc->is_const = false;
        t = nc;
    }
    if (typeset_contains(eqs, t)) return;
    typeset_add(eqs, t);
    switch (t->kind) {
    case TYPE_STRUCT:
        if (!t->struc.is_c_union) {
            for (int i = 0; i < t->struc.field_count; i++)
                collect_eq_types(t->struc.fields[i].type, eqs);
        }
        break;
    case TYPE_UNION:
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload)
                collect_eq_types(t->unio.variants[i].payload, eqs);
        break;
    case TYPE_SLICE:
        collect_eq_types(t->slice.elem, eqs);
        break;
    case TYPE_OPTION:
        collect_eq_types(t->option.inner, eqs);
        break;
    case TYPE_RESULT:
        collect_eq_types(t->result.inner, eqs);
        break;
    default:
        break;
    }
}

/* Walk patterns looking for PAT_STRING_LIT to register str eq */
static void collect_eq_from_pattern(Pattern *pat, TypeSet *eqs) {
    if (!pat) return;
    switch (pat->kind) {
    case PAT_STRING_LIT:
        collect_eq_types(type_str(), eqs);
        break;
    case PAT_SOME:
    case PAT_OK:
    case PAT_ERR:
        if (pat->some_pat.inner) collect_eq_from_pattern(pat->some_pat.inner, eqs);
        break;
    case PAT_VARIANT:
        if (pat->variant.payload) collect_eq_from_pattern(pat->variant.payload, eqs);
        break;
    case PAT_STRUCT:
        for (int i = 0; i < pat->struc.field_count; i++)
            collect_eq_from_pattern(pat->struc.fields[i].pattern, eqs);
        break;
    case PAT_TUPLE:
        for (int i = 0; i < pat->tuple_pat.pattern_count; i++)
            collect_eq_from_pattern(pat->tuple_pat.patterns[i], eqs);
        break;
    case PAT_OR:
        for (int i = 0; i < pat->or_pat.alt_count; i++)
            collect_eq_from_pattern(pat->or_pat.alts[i], eqs);
        break;
    default:
        break;
    }
}

/* The sets collect_types_expr fills, bundled into one visitor context. */
typedef struct {
    TypeSet *slices;
    TypeSet *options;
    TypeSet *fns;
} TypeSets;

/* Collect the slice, option, result and function types an expression tree
 * uses, and the types that need generated equality or enum_of helpers.
 * `sets` is a TypeSets. */
static void collect_types_expr(Expr *e, void *sets) {
    if (!e) return;
    TypeSets *s = sets;
    collect_types_in_type(e->type, s->slices, s->options, s->fns);

    switch (e->kind) {
    case EXPR_BINARY:
        expr_for_each_child(e, collect_types_expr, sets);
        if (g_eq_set && (e->binary.op == TOK_EQEQ || e->binary.op == TOK_BANGEQ) &&
            e->binary.left->type)
            collect_eq_types(e->binary.left->type, g_eq_set);
        return;
    case EXPR_CALL:
        /* A direct call emits its callee by name, so it needs no function-type
         * typedef, and collecting one is wrong for a generic callee. The
         * callee's signature is written in the callee's own type parameters,
         * but this walk substitutes the caller's bindings into whatever it
         * sees: inside `mk2<'n>`, the call `mk<'n * 2>()` would have mk's
         * `() -> wide<'n>` read with the caller's 'n, emitting a typedef over
         * a `wide<64>` that is never instantiated.  Only an indirect call,
         * whose callee is a function value, needs the typedef. */
        if (e->call.is_indirect)
            collect_types_expr(e->call.func, sets);
        else if (e->call.func->kind == EXPR_FIELD || e->call.func->kind == EXPR_DEREF_FIELD)
            collect_types_expr(e->call.func->field.object, sets);
        else if (e->call.func->kind != EXPR_IDENT)
            collect_types_expr(e->call.func, sets);
        for (int i = 0; i < e->call.arg_count; i++)
            collect_types_expr(e->call.args[i], sets);
        return;
    case EXPR_FUNC:
        expr_for_each_child(e, collect_types_expr, sets);
        for (int i = 0; i < e->func.param_count; i++)
            collect_types_in_type(e->func.params[i].type, s->slices, s->options, s->fns);
        return;
    case EXPR_LET:
        collect_types_in_type(e->let_expr.let_type, s->slices, s->options, s->fns);
        break;
    case EXPR_CAST:
        collect_types_in_type(e->cast.target, s->slices, s->options, s->fns);
        break;
    case EXPR_ENUM_OF:
        /* the enum needs its membership-check helper pair */
        if (g_enum_of_set) typeset_add(g_enum_of_set, e->enum_of_expr.target);
        break;
    case EXPR_ARRAY_LIT:
        collect_types_in_type(e->array_lit.elem_type, s->slices, s->options, s->fns);
        break;
    case EXPR_SLICE_LIT:
        collect_types_in_type(e->slice_lit.elem_type, s->slices, s->options, s->fns);
        break;
    case EXPR_MATCH:
        /* A string-literal pattern compares with the generated str equality. */
        collect_types_expr(e->match_expr.subject, sets);
        for (int i = 0; i < e->match_expr.arm_count; i++) {
            MatchArm *arm = &e->match_expr.arms[i];
            if (g_eq_set) collect_eq_from_pattern(arm->pattern, g_eq_set);
            collect_types_expr(arm->guard, sets);
            for (int j = 0; j < arm->body_count; j++)
                collect_types_expr(arm->body[j], sets);
        }
        return;
    case EXPR_SIZEOF:
        collect_types_in_type(e->sizeof_expr.target, s->slices, s->options, s->fns);
        break;
    case EXPR_ALIGNOF:
        collect_types_in_type(e->alignof_expr.target, s->slices, s->options, s->fns);
        break;
    case EXPR_DEFAULT:
        collect_types_in_type(e->default_expr.target, s->slices, s->options, s->fns);
        break;
    default:
        break;
    }
    expr_for_each_child(e, collect_types_expr, sets);
}

/* ---- Lambda collection ---- */

typedef struct {
    Expr **exprs;
    int count;
    int cap;
} LambdaSet;

/* ---- Trampoline set ---- */
/* Tracks FC functions that need C-compatible trampolines for extern call boundaries.
 * A trampoline wraps an FC function (which has an extra void* _ctx param) into a
 * plain C function pointer compatible with the extern's expected signature. */

typedef struct {
    const char *name;   /* C function name (codegen_name or lifted_name) */
    Type *type;         /* the function's type (TYPE_FUNC) */
} TrampolineEntry;

/* A trampoline's C signature, through its closing ')': the wrapped function's
 * parameters with no context pointer. */
static void emit_trampoline_signature(TrampolineEntry *te, FILE *out) {
    Type *ft = te->type;
    fprintf(out, "%s", g_fn_attr);
    emit_type(ft->func.return_type, out);
    fprintf(out, " fc_ctramp_%s(", te->name);
    for (int j = 0; j < ft->func.param_count; j++) {
        if (j > 0) fprintf(out, ", ");
        emit_type(ft->func.param_types[j], out);
        fprintf(out, " _p%d", j);
    }
    if (ft->func.param_count == 0) fprintf(out, "void");
    fprintf(out, ")");
}

typedef struct {
    TrampolineEntry *entries;
    int count;
    int cap;
} TrampolineSet;

static bool trampolineset_contains(TrampolineSet *ts, const char *name) {
    for (int i = 0; i < ts->count; i++) {
        if (strcmp(ts->entries[i].name, name) == 0) return true;
    }
    return false;
}

static void trampolineset_add(TrampolineSet *ts, const char *name, Type *type) {
    if (!trampolineset_contains(ts, name)) {
        TrampolineEntry entry = { name, type };
        DA_APPEND(ts->entries, ts->count, ts->cap, entry);
    }
}

/* Pre-pass for module-member initializers: assigns a unique backing-array
 * name to every EXPR_ARRAY_LIT inside a constant initializer, and appends
 * the node to g_const_backings so the file-scope emission pass can emit
 * `static T _fc_const_backing_N[] = {...};` before the module-member defs.
 * pass2 has already restricted the initializer to constant forms. A struct
 * field whose type is a fixed array takes an inline aggregate initializer at
 * the use site rather than a backing array. `rodata` points to a bool: the
 * constant is frozen, so its backings are emitted `static const`. */
static void collect_const_backings(Expr *e, void *rodata) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_ARRAY_LIT: {
        /* Children first.  A backing array's initializer names the backing
         * arrays of any slice literals nested inside it, and at C file scope
         * a name must be declared before it is used, so the nested ones have
         * to be emitted first.  g_const_backings is emitted in order, so
         * post-order collection is dependency order. */
        for (int i = 0; i < e->array_lit.elem_count; i++)
            collect_const_backings(e->array_lit.elems[i], rodata);
        /* Zero-length literals don't need a backing: the use site emits a
         * NULL/0 slice (C rejects zero-length arrays).  The empty form
         * `T[N]{ }` with N > 0 means N zero-initialized elements and does
         * need one, emitted as `static T name[N] = {0};`, since a
         * null-backed slice whose .len says N would pass every bounds check
         * and fault on first access.  pass2 guarantees the size of a
         * concrete slice literal is a folded EXPR_INT_LIT. */
        bool zero_fill = e->array_lit.elem_count == 0 &&
            e->array_lit.size_expr &&
            e->array_lit.size_expr->kind == EXPR_INT_LIT &&
            e->array_lit.size_expr->int_lit.value > 0;
        if (e->array_lit.elem_count > 0 || zero_fill) {
            e->array_lit.codegen_backing_name =
                arena_sprintf(g_arena, "_fc_const_backing_%d", g_const_backing_counter++);
            e->array_lit.codegen_backing_rodata = *(bool *)rodata;
            DA_APPEND(g_const_backings, g_const_backing_count,
                      g_const_backing_cap, e);
        }
        return;
    }
    case EXPR_STRUCT_LIT: {
        Type *st = e->type;
        for (int i = 0; i < e->struct_lit.field_count; i++) {
            /* Fixed-array fields take an inline aggregate: the inner
             * array-lit is emitted raw, no backing needed.  Identify by
             * looking up the field type in the resolved struct. */
            bool is_fixed_field = false;
            if (st && st->kind == TYPE_STRUCT) {
                const char *fname = e->struct_lit.fields[i].name;
                for (int f = 0; f < st->struc.field_count; f++) {
                    if (st->struc.fields[f].name == fname) {
                        is_fixed_field = st->struc.fields[f].type->kind == TYPE_FIXED_ARRAY;
                        break;
                    }
                }
            }
            Expr *v = e->struct_lit.fields[i].value;
            if (is_fixed_field && v && v->kind == EXPR_ARRAY_LIT) {
                /* Recurse into elements but do not lift the outer array */
                for (int j = 0; j < v->array_lit.elem_count; j++)
                    collect_const_backings(v->array_lit.elems[j], rodata);
            } else {
                collect_const_backings(v, rodata);
            }
        }
        return;
    }
    default:
        break;
    }
    expr_for_each_child(e, collect_const_backings, rodata);
}

/* Record the FC functions that are handed to C as raw function pointers and so
 * need a trampoline: arguments of function type at an extern call, and `&f`.
 * `set` is a TrampolineSet. */
static void collect_trampolines_expr(Expr *e, void *set) {
    if (!e) return;
    TrampolineSet *ts = set;
    if (e->kind == EXPR_CALL && e->call.is_extern_call) {
        Type *call_ft = e->call.func->type;
        for (int i = 0; i < e->call.arg_count; i++) {
            Type *pt = (call_ft && call_ft->kind == TYPE_FUNC && i < call_ft->func.param_count)
                ? call_ft->func.param_types[i] : NULL;
            if (!pt || pt->kind != TYPE_FUNC) continue;
            Expr *arg = e->call.args[i];
            if (arg->kind == EXPR_IDENT && !arg->ident.is_local && arg->type &&
                arg->type->kind == TYPE_FUNC) {
                /* Top-level function passed at extern boundary */
                const char *fname = arg->ident.codegen_name
                    ? arg->ident.codegen_name : arg->ident.name;
                trampolineset_add(ts, fname, arg->type);
            } else if (arg->kind == EXPR_FUNC && arg->func.capture_count == 0 &&
                       arg->func.lifted_name && arg->type &&
                       arg->type->kind == TYPE_FUNC) {
                /* Non-capturing lambda passed at extern boundary */
                trampolineset_add(ts, lambda_c_name(arg), arg->type);
            }
        }
    } else if (e->kind == EXPR_UNARY_PREFIX && e->unary_prefix.op == TOK_AMP) {
        /* &f on a top-level function or non-capturing lambda yields a raw C
         * function pointer. */
        Expr *operand = e->unary_prefix.operand;
        if (operand->kind == EXPR_IDENT && !operand->ident.is_local &&
            operand->type && operand->type->kind == TYPE_FUNC) {
            const char *name = operand->ident.codegen_name
                ? operand->ident.codegen_name : operand->ident.name;
            trampolineset_add(ts, name, operand->type);
        } else if (operand->kind == EXPR_FIELD && operand->field.codegen_name &&
                   operand->type && operand->type->kind == TYPE_FUNC) {
            trampolineset_add(ts, operand->field.codegen_name, operand->type);
        } else if (operand->kind == EXPR_FUNC && operand->func.capture_count == 0 &&
                   operand->func.lifted_name && operand->type &&
                   operand->type->kind == TYPE_FUNC) {
            trampolineset_add(ts, lambda_c_name(operand), operand->type);
        }
    }
    expr_for_each_child(e, collect_trampolines_expr, set);
}

/* Collect every lambda that is lifted to its own C function, in source order.
 * `set` is a LambdaSet. */
static void collect_lambdas_expr(Expr *e, void *set) {
    if (!e) return;
    if (e->kind == EXPR_FUNC && e->func.lifted_name) {
        LambdaSet *ls = set;
        DA_APPEND(ls->exprs, ls->count, ls->cap, e);
    }
    expr_for_each_child(e, collect_lambdas_expr, set);
}

/* ---- Eq function generation ---- */

static void emit_eq_func_name(Type *t, FILE *out) {
    /* Strip const: equality semantics don't depend on constness */
    Type tmp;
    if (t->is_const) { tmp = *t; tmp.is_const = false; t = &tmp; }
    fprintf(out, "fc_eq_");
    emit_type_ident(t, out);
}

static void emit_eq_forward(Type *t, FILE *out) {
    fprintf(out, "static inline bool ");
    emit_eq_func_name(t, out);
    fprintf(out, "(");
    emit_type(t, out);
    fprintf(out, " a, ");
    emit_type(t, out);
    fprintf(out, " b);\n");
}

/* Emit the comparison expression for two values of type t.
   a_expr/b_expr are "a"/"b" (or "a.field"/"b.field"). */
static void emit_value_eq(Type *t, const char *a_expr, const char *b_expr, FILE *out) {
    if (t->kind == TYPE_STUB) t = resolve_struct_stub(t);
    if (type_needs_eq_func(t)) {
        emit_eq_func_name(t, out);
        fprintf(out, "(%s, %s)", a_expr, b_expr);
    } else {
        fprintf(out, "%s == %s", a_expr, b_expr);
    }
}

/* enum_of(E, x) membership check: a signed/unsigned helper pair per enum.
 * The switch lives in whichever domain covers the repr; the other clamps and
 * delegates, so enum_of(E_i8, u64max) and enum_of(E_u64, -1) are both none
 * without any implementation-defined conversion. Call sites pick _u for
 * u64/usize operands, _i otherwise. */
static void emit_enum_of_helpers(Type *et, FILE *out) {
    const char *en = et->enu.name;
    Type *repr = et->enu.repr ? et->enu.repr : type_int32();
    bool u64_repr = (repr->kind == TYPE_UINT64);
    /* option typedef name: fc_option_<enum> */
    if (!u64_repr) {
        /* switch in the signed (int64) domain; every non-u64 repr's values fit */
        fprintf(out, "%sfc_option_%s fc_enum_of_%s_i(int64_t v) {\n", g_fn_attr, en, en);
        fprintf(out, "    switch (v) {\n");
        for (int i = 0; i < et->enu.variant_count; i++) {
            uint64_t bits = et->enu.variants[i].value_bits;
            int64_t sv = type_is_signed(repr) ? enum_bits_signed(bits, repr) : (int64_t)bits;
            if (sv == INT64_MIN)
                fprintf(out, "    case (-9223372036854775807LL - 1):\n");
            else
                fprintf(out, "    case %lldLL:\n", (long long)sv);
        }
        fprintf(out, "        return (fc_option_%s){ .value = (%s)v, .has_value = true };\n", en, en);
        fprintf(out, "    default: return (fc_option_%s){ .has_value = false };\n", en);
        fprintf(out, "    }\n}\n");
        fprintf(out, "%sfc_option_%s fc_enum_of_%s_u(uint64_t v) {\n", g_fn_attr, en, en);
        fprintf(out, "    if (v > (uint64_t)INT64_MAX) return (fc_option_%s){ .has_value = false };\n", en);
        fprintf(out, "    return fc_enum_of_%s_i((int64_t)v);\n}\n", en);
    } else {
        /* u64 repr: switch in the unsigned domain */
        fprintf(out, "%sfc_option_%s fc_enum_of_%s_u(uint64_t v) {\n", g_fn_attr, en, en);
        fprintf(out, "    switch (v) {\n");
        for (int i = 0; i < et->enu.variant_count; i++)
            fprintf(out, "    case %lluULL:\n",
                (unsigned long long)et->enu.variants[i].value_bits);
        fprintf(out, "        return (fc_option_%s){ .value = (%s)v, .has_value = true };\n", en, en);
        fprintf(out, "    default: return (fc_option_%s){ .has_value = false };\n", en);
        fprintf(out, "    }\n}\n");
        fprintf(out, "%sfc_option_%s fc_enum_of_%s_i(int64_t v) {\n", g_fn_attr, en, en);
        fprintf(out, "    if (v < 0) return (fc_option_%s){ .has_value = false };\n", en);
        fprintf(out, "    return fc_enum_of_%s_u((uint64_t)v);\n}\n", en);
    }
}

static void emit_eq_func(Type *t, FILE *out) {
    fprintf(out, "static inline bool ");
    emit_eq_func_name(t, out);
    fprintf(out, "(");
    emit_type(t, out);
    fprintf(out, " a, ");
    emit_type(t, out);
    fprintf(out, " b) {\n");

    t = resolve_struct_stub(t);
    switch (t->kind) {
    case TYPE_STRUCT: {
        if (t->struc.is_c_union) {
            /* Extern unions: byte-level comparison via memcmp */
            fprintf(out, "    return memcmp(&a, &b, sizeof(");
            emit_type(t, out);
            fprintf(out, ")) == 0;\n");
        } else {
            int fc = t->struc.field_count;
            if (fc == 0) {
                fprintf(out, "    (void)a; (void)b;\n    return true;\n");
            } else {
                fprintf(out, "    return ");
                for (int i = 0; i < fc; i++) {
                    if (i > 0) fprintf(out, " && ");
                    Type *ft = t->struc.fields[i].type;
                    const char *fname = c_safe_ident(g_intern, t->struc.fields[i].name);
                    if (ft->kind == TYPE_FIXED_ARRAY) {
                        Type *elem = ft->fixed_array.elem;
                        if (!type_needs_eq_func(elem) && !type_is_float(elem)) {
                            fprintf(out, "memcmp(a.%s, b.%s, sizeof(a.%s)) == 0",
                                    fname, fname, fname);
                        } else {
                            /* Element-wise comparison for complex types */
                            fprintf(out, "({ bool _eq = true; "
                                    "for (int _k = 0; _k < %lld; _k++) if (!",
                                    (long long)fixarr_size(ft));
                            if (type_needs_eq_func(elem)) {
                                emit_eq_func_name(elem, out);
                                fprintf(out, "(a.%s[_k], b.%s[_k])", fname, fname);
                            } else {
                                fprintf(out, "(a.%s[_k] == b.%s[_k])", fname, fname);
                            }
                            fprintf(out, ") { _eq = false; break; } _eq; })");
                        }
                    } else {
                        emit_value_eq(ft, path_cat("a.", fname, ""),
                                      path_cat("b.", fname, ""), out);
                    }
                }
                fprintf(out, ";\n");
            }
        }
        break;
    }
    case TYPE_UNION: {
        /* Check if any variant has a payload */
        bool has_payload = false;
        for (int i = 0; i < t->unio.variant_count; i++)
            if (t->unio.variants[i].payload) { has_payload = true; break; }

        if (!has_payload) {
            /* Tag-only union: just compare tags */
            fprintf(out, "    return a.tag == b.tag;\n");
        } else {
            fprintf(out, "    if (a.tag != b.tag) return false;\n");
            fprintf(out, "    switch (a.tag) {\n");
            const char *uname = t->unio.name;
            for (int i = 0; i < t->unio.variant_count; i++) {
                fprintf(out, "    case %s: ",
                    union_tag_value(uname, t->unio.variants[i].name));
                if (t->unio.variants[i].payload) {
                    const char *vfield = c_safe_ident(g_intern, t->unio.variants[i].name);
                    fprintf(out, "return ");
                    emit_value_eq(t->unio.variants[i].payload,
                        path_cat("a." FC_PAYLOAD_MEMBER ".", vfield, ""),
                        path_cat("b." FC_PAYLOAD_MEMBER ".", vfield, ""), out);
                    fprintf(out, ";\n");
                } else {
                    fprintf(out, "return true;\n");
                }
            }
            fprintf(out, "    }\n");
            fprintf(out, "    return true;\n");
        }
        break;
    }
    case TYPE_SLICE: {
        Type *elem = t->slice.elem;
        fprintf(out, "    if (a.len != b.len) return false;\n");
        fprintf(out, "    if (a.len == 0) return true;\n");
        /* Use memcmp for non-float primitives that don't need eq funcs */
        if (!type_needs_eq_func(elem) && !type_is_float(elem)) {
            fprintf(out, "    return memcmp(a.ptr, b.ptr, fc_to_size(a.len) * sizeof(");
            emit_type(elem, out);
            fprintf(out, ")) == 0;\n");
        } else {
            fprintf(out, "    for (fc_len_t _i = 0; _i < a.len; _i++)\n");
            fprintf(out, "        if (!");
            if (type_needs_eq_func(elem)) {
                emit_eq_func_name(elem, out);
                fprintf(out, "(a.ptr[_i], b.ptr[_i])");
            } else {
                /* float: use C == */
                fprintf(out, "(a.ptr[_i] == b.ptr[_i])");
            }
            fprintf(out, ") return false;\n");
            fprintf(out, "    return true;\n");
        }
        break;
    }
    case TYPE_OPTION:
        fprintf(out, "    if (a.has_value != b.has_value) return false;\n");
        fprintf(out, "    if (!a.has_value) return true;\n");
        fprintf(out, "    return ");
        emit_value_eq(t->option.inner, "a.value", "b.value", out);
        fprintf(out, ";\n");
        break;
    case TYPE_RESULT:
        /* Equal iff same variant; ok payloads are compared, and an err code is
         * the tag itself. void! has no payload, so the tag comparison is the
         * whole answer. */
        if (t->result.inner && t->result.inner->kind == TYPE_VOID) {
            fprintf(out, "    return a.err == b.err;\n");
            break;
        }
        fprintf(out, "    if (a.err != b.err) return false;\n");
        fprintf(out, "    if (a.err != 0) return true;\n");
        fprintf(out, "    return ");
        emit_value_eq(t->result.inner, "a.value", "b.value", out);
        fprintf(out, ";\n");
        break;
    case TYPE_FUNC:
        fprintf(out, "    return a.fn_ptr == b.fn_ptr && a.ctx == b.ctx;\n");
        break;
    default:
        internal_error((SrcLoc){0}, "no equality for type %s", type_name(t));
        break;
    }

    fprintf(out, "}\n");
}

/* ---- Option/result body emission (shared fixpoint) ----
 *
 * Option and result bodies embed their inner type by value, and the two
 * wrappers nest across sets in both orders (u8?! embeds fc_option_uint8_t,
 * u8!? embeds fc_result_uint8_t), so neither set can be emitted wholesale
 * before the other. Bodies are emitted by fixpoint instead: a wrapper emits
 * once its inner is complete. Scalars/pointers are always complete; slices
 * and fn typedefs are complete by the time the first fixpoint runs;
 * struct/union/stub inners complete as their defs land in the topo
 * interleave (the caller records each def name in `defs_done`). */
typedef struct {
    TypeSet *options, *results;
    bool *opt_done, *res_done;
    const char **defs_done;     /* interned names of emitted struct/union defs */
    int defs_done_count;
    FILE *out;
} WrapEmit;

static bool wrap_inner_complete(WrapEmit *we, Type *inner) {
    if (!inner) return true;
    const char *name = NULL;
    switch (inner->kind) {
    case TYPE_STRUCT: name = inner->struc.name; break;
    case TYPE_UNION:  name = inner->unio.name; break;
    case TYPE_STUB:   name = inner->stub.name; break;
    case TYPE_OPTION:
        for (int i = 0; i < we->options->count; i++)
            if (type_ident_eq(we->options->types[i], inner))
                return we->opt_done[i];
        return true;  /* not in the set: a null-sentinel option, a plain pointer */
    case TYPE_RESULT:
        for (int i = 0; i < we->results->count; i++)
            if (type_ident_eq(we->results->types[i], inner))
                return we->res_done[i];
        return true;  /* not collected: nothing to wait for */
    default:
        return true;  /* scalars, pointers, slices, fns: bodies already out */
    }
    for (int i = 0; i < we->defs_done_count; i++)
        if (we->defs_done[i] == name) return true;
    return false;
}

static void emit_wrapper_bodies(WrapEmit *we) {
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < we->options->count; i++) {
            if (we->opt_done[i]) continue;
            Type *o = we->options->types[i];
            if (!wrap_inner_complete(we, o->option.inner)) continue;
            fprintf(we->out, "struct fc_option_");
            emit_type_ident(o->option.inner, we->out);
            fprintf(we->out, " { ");
            emit_type(o->option.inner, we->out);
            fprintf(we->out, " value; bool has_value; };\n");
            we->opt_done[i] = true;
            progress = true;
        }
        for (int i = 0; i < we->results->count; i++) {
            if (we->res_done[i]) continue;
            Type *r = we->results->types[i];
            if (!wrap_inner_complete(we, r->result.inner)) continue;
            fprintf(we->out, "struct fc_result_");
            emit_type_ident(r->result.inner, we->out);
            if (r->result.inner && r->result.inner->kind == TYPE_VOID) {
                /* void!: the payload-less result is the tag alone */
                fprintf(we->out, " { int32_t err; };\n");
            } else {
                fprintf(we->out, " { int32_t err; ");
                emit_type(r->result.inner, we->out);
                fprintf(we->out, " value; };\n");
            }
            we->res_done[i] = true;
            progress = true;
        }
    }
}

/* Flatten top-level decls and (recursively) module member decls into one
 * array. */
static void flatten_decls(Decl **decls, int decl_count, Decl ***out, int *count, int *cap) {
    for (int i = 0; i < decl_count; i++) {
        Decl *d = decls[i];
        if (d->kind == DECL_MODULE) {
            flatten_decls(d->module.decls, d->module.decl_count, out, count, cap);
        } else {
            DA_APPEND(*out, *count, *cap, d);
        }
    }
}

/* Find a struct/union name referenced by value in a type (not through
 * pointer/slice/func). Like monomorph.c's find_by_value_dep, but for
 * decl-level types (which may still contain stubs), and it also looks
 * through options. */
static const char *find_by_value_dep_name(Type *type) {
    if (!type) return NULL;
    switch (type->kind) {
    case TYPE_STRUCT: return type->struc.name;
    case TYPE_UNION:  return type->unio.name;
    case TYPE_STUB:
        /* A concrete generic-instance field depends on its monomorphized
         * instance's mangled name, so it is emitted after that instance. */
        if (type->stub.type_arg_count > 0 && !type_contains_type_var(type))
            return mangle_generic_name(g_intern, type->stub.name,
                                       type->stub.type_args, type->stub.type_arg_count);
        return type->stub.name;
    case TYPE_FIXED_ARRAY: return find_by_value_dep_name(type->fixed_array.elem);
    case TYPE_OPTION:
        /* A by-value option-of-struct/union/stub embeds its inner aggregate, so
         * the enclosing def must be ordered after the inner's def (the option
         * body, fc_option_<inner>, is emitted in the interleave right after
         * the inner def). Option-of-pointer/scalar/fn carries no by-value
         * aggregate dependency (inner recurses to NULL). */
        return find_by_value_dep_name(type->option.inner);
    case TYPE_RESULT:
        /* A result always embeds its payload by value: same interleave rule */
        return find_by_value_dep_name(type->result.inner);
    default: return NULL;
    }
}

enum { DECL_TOPO_UNVISITED = 0, DECL_TOPO_VISITING = 1, DECL_TOPO_DONE = 2 };

static const char *decl_su_name(Decl *d) {
    if (d->kind == DECL_STRUCT) return d->struc.name;
    if (d->kind == DECL_UNION)  return d->unio.name;
    return NULL;
}

/* A struct/union definition to emit: either a top-level decl or a monomorphized
 * instance (which includes synthesized tuples). Both kinds participate in one
 * topological sort so by-value dependencies emit before dependents in either
 * direction (a top-level struct holding a tuple, or a generic instance holding a
 * top-level struct). */
typedef struct {
    const char *name;
    Decl *decl;          /* non-NULL: top-level struct/union decl */
    MonoInstance *mi;    /* non-NULL: monomorphized struct/union/tuple instance */
} SuDef;

static void sudef_deps(SuDef *s, const char ***deps, int *dep_count, int *dep_cap) {
    StructField *fields = NULL; int field_count = 0;
    UnionVariant *variants = NULL; int variant_count = 0;
    if (s->decl) {
        Decl *d = s->decl;
        if (d->kind == DECL_STRUCT && !d->struc.is_extern) {
            fields = d->struc.fields; field_count = d->struc.field_count;
        } else if (d->kind == DECL_UNION) {
            variants = d->unio.variants; variant_count = d->unio.variant_count;
        }
    } else if (s->mi && s->mi->concrete_type) {
        Type *ct = s->mi->concrete_type;
        if (ct->kind == TYPE_STRUCT) { fields = ct->struc.fields; field_count = ct->struc.field_count; }
        else if (ct->kind == TYPE_UNION) { variants = ct->unio.variants; variant_count = ct->unio.variant_count; }
    }
    for (int f = 0; f < field_count; f++) {
        const char *dep = find_by_value_dep_name(fields[f].type);
        if (dep) DA_APPEND(*deps, *dep_count, *dep_cap, dep);
    }
    for (int v = 0; v < variant_count; v++) {
        const char *dep = find_by_value_dep_name(variants[v].payload);
        if (dep) DA_APPEND(*deps, *dep_count, *dep_cap, dep);
    }
}

static void topo_visit_sudef(SuDef *items, int n, int idx, int *state,
                             int *order, int *order_count) {
    if (state[idx] != DECL_TOPO_UNVISITED) return;
    state[idx] = DECL_TOPO_VISITING;
    const char **deps = NULL;
    int dep_count = 0, dep_cap = 0;
    sudef_deps(&items[idx], &deps, &dep_count, &dep_cap);
    for (int di = 0; di < dep_count; di++) {
        for (int j = 0; j < n; j++) {
            if (j != idx && items[j].name == deps[di]) {
                topo_visit_sudef(items, n, j, state, order, order_count);
                break;
            }
        }
    }
    free(deps);
    order[(*order_count)++] = idx;
    state[idx] = DECL_TOPO_DONE;
}

/* Recursively collect unique from_lib strings from module declarations */
static void collect_from_libs(Decl *d, const char ***seen, int *count, int *cap) {
    if (d->kind != DECL_MODULE) return;
    if (d->module.from_lib) {
        for (int i = 0; i < *count; i++)
            if (strcmp((*seen)[i], d->module.from_lib) == 0) return;
        DA_APPEND(*seen, *count, *cap, d->module.from_lib);
    }
    for (int i = 0; i < d->module.decl_count; i++)
        collect_from_libs(d->module.decls[i], seen, count, cap);
}

/* ---- Define collection ---- */
/* Tracks #define macros from module `define` annotations */
typedef struct {
    const char *macro;
    const char *value;
} CDefine;

static void collect_defines(Decl *d, CDefine **defs, int *count, int *cap) {
    if (d->kind != DECL_MODULE) return;
    if (d->module.define_macro) {
        for (int i = 0; i < *count; i++) {
            CDefine *prev = &(*defs)[i];
            if (strcmp(prev->macro, d->module.define_macro) == 0) {
                if (strcmp(prev->value, d->module.define_value) != 0) {
                    diag_error(d->loc,
                        "conflicting define for '%s': '%s' vs '%s'",
                        d->module.define_macro, prev->value, d->module.define_value);
                }
                return;
            }
        }
        CDefine def = { d->module.define_macro, d->module.define_value };
        DA_APPEND(*defs, *count, *cap, def);
    }
    for (int i = 0; i < d->module.decl_count; i++)
        collect_defines(d->module.decls[i], defs, count, cap);
}

/* ---- Feature detection ---- */
/* Lightweight AST scan to determine which feature-gated headers are needed */
static bool g_needs_stdio;
static bool g_needs_math;
static bool g_needs_float;
static bool g_needs_errno;   /* an errno-protocol extern call is reachable */
/* g_backtraces / g_uses_error_name / g_errname_emitted are declared at the
 * top of the file because the result-unwrap emitter reads them. */

/* Symbol map populated during codegen when g_backtraces is set; emitted as
 * _fc_symtab[] in the preamble so fc_dump_backtrace() can render FC-level names. */
typedef struct {
    const char *c_name;   /* mangled C identifier as emitted */
    const char *fc_name;  /* display name (e.g. "foo<i32>", "<lambda>") */
    const char *file;     /* FC source file of the definition */
    int line;             /* FC source line of the definition */
} FcSymEntry;
static FcSymEntry *g_symmap = NULL;
static int g_symmap_count = 0;
static int g_symmap_cap = 0;

static void symmap_reset(void) {
    free(g_symmap);
    g_symmap = NULL;
    g_symmap_count = 0;
    g_symmap_cap = 0;
}

/* Add a function to the symbol table for --backtraces.  Pass fc_name=NULL to
 * mark a runtime helper (fc_oob, fc_oob_sub, ...) whose frames are left out
 * of the printed backtrace.  Otherwise fc_name is the FC display name (e.g.
 * "foo<i32>", "<lambda>"). */
static void symmap_add(const char *c_name, const char *fc_name,
                       const char *file, int line) {
    if (!g_backtraces || !c_name) return;
    /* Dedup: a function can be emitted in multiple walks; skip if seen. */
    for (int i = 0; i < g_symmap_count; i++) {
        if (g_symmap[i].c_name && strcmp(g_symmap[i].c_name, c_name) == 0)
            return;
    }
    FcSymEntry e = { c_name, fc_name,
                     file ? file : "<unknown>", line };
    DA_APPEND(g_symmap, g_symmap_count, g_symmap_cap, e);
}

/* Build "template<T1, T2>" for a monomorphized instance into an arena
 * allocation. Returns a stable pointer suitable for storing in the symmap. */
static const char *fmt_mono_display(Arena *arena, const char *template_name,
                                    Type **type_args, int type_arg_count) {
    const char *disp = arena_sprintf(arena, "%s<", template_name);
    for (int i = 0; i < type_arg_count; i++) {
        const char *tn = type_name(type_args[i]);
        disp = arena_sprintf(arena, "%s%s%s", disp, i == 0 ? "" : ", ", tn ? tn : "?");
    }
    return arena_sprintf(arena, "%s>", disp);
}

static void detect_features_expr(Expr *e);

static void detect_features_child(Expr *child, void *ctx) {
    (void)ctx;
    detect_features_expr(child);
}

/* Record which headers and runtime helpers the emitted C needs: <stdio.h> for
 * every runtime abort message, <math.h>/<float.h> for float properties, and
 * <errno.h> for errno-protocol externs. */
static void detect_features_expr(Expr *e) {
    if (!e) return;
    if (g_needs_stdio && g_needs_math && g_needs_float && g_needs_errno)
        return; /* all found */

    switch (e->kind) {
    case EXPR_STATIC_ASSERT:
        return;   /* proven at compile time; emits nothing */
    case EXPR_ARRAY_LIT:
        /* The size is folded at compile time and never evaluated at run time. */
        for (int i = 0; i < e->array_lit.elem_count; i++)
            detect_features_expr(e->array_lit.elems[i]);
        return;
    case EXPR_INTERP_STRING:
        g_needs_stdio = true;
        break;
    case EXPR_FIELD:
    case EXPR_DEREF_FIELD:
        /* A float type property is spelled with a float.h or math.h macro. A
         * type variable's could be a float's, so it counts as one. */
        if (e->field.is_type_property || e->field.object->kind == EXPR_TYPE_VAR_REF) {
            Type *owner = e->field.is_type_property && e->field.object->kind == EXPR_IDENT
                ? type_from_name(e->field.object->ident.name,
                                 (int)strlen(e->field.object->ident.name))
                : type_float64();
            PropHeader h = owner ? type_property_header(owner, e->field.name) : PROP_HEADER_NONE;
            if (h == PROP_HEADER_MATH) g_needs_math = true;
            if (h == PROP_HEADER_FLOAT) g_needs_float = true;
        }
        break;
    case EXPR_BINARY:
        /* Integer div/mod emits a by-zero abort with stderr message */
        if ((e->binary.op == TOK_SLASH || e->binary.op == TOK_PERCENT) &&
            e->type && type_is_integer(e->type))
            g_needs_stdio = true;
        break;
    case EXPR_UNARY_POSTFIX:
        if (e->unary_postfix.op == TOK_BANG)
            g_needs_stdio = true;
        break;
    case EXPR_CALL:
        /* A protocol extern call (from <protocol>) may need <errno.h> and,
         * for the out-of-band code sources (errno / GetLastError / WSA),
         * emits the err(T,0) guard (fc_zero_err -> stderr). */
        if (e->call.resolved_callee &&
            e->call.resolved_callee->kind == DECL_EXTERN &&
            e->call.resolved_callee->decl) {
            switch (e->call.resolved_callee->decl->ext.protocol) {
            case EXT_PROTO_ERRNO_NEG1:
            case EXT_PROTO_ERRNO_NULL:
                g_needs_errno = true;
                g_needs_stdio = true;
                break;
            case EXT_PROTO_LASTERR_0:
            case EXT_PROTO_LASTERR_NULL:
            case EXT_PROTO_LASTERR_NEG1:
            case EXT_PROTO_WSA_NEG1:
                g_needs_stdio = true;
                break;
            default: break;
            }
        }
        break;
    case EXPR_GUARD:
        /* A `checked` body emits fc_overflow (stderr) on overflow. pass2 rejects a
         * checked marker with no governed op, so any that survives will emit one. */
        if (e->guard.is_overflow_axis && e->guard.enable)
            g_needs_stdio = true;
        break;
    case EXPR_ASSIGN:
        /* Assignment to fixed-array field emits an overflow abort with stderr */
        if ((e->assign.target->kind == EXPR_FIELD ||
             e->assign.target->kind == EXPR_DEREF_FIELD) &&
            e->assign.target->field.fixed_array_type)
            g_needs_stdio = true;
        break;
    case EXPR_SOME: {
        /* A null-sentinel some(p) over a not-provably-non-null pointer emits a
         * null-pointer abort via stderr (fc_null_some). A type variable may
         * monomorphize to a pointer, so treat it as possibly guarded too; at
         * worst this pulls in stdio.h for a program that does not need it. */
        Type *inner = e->type && e->type->kind == TYPE_OPTION
                          ? e->type->option.inner : NULL;
        if (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_ANY_PTR ||
                      inner->kind == TYPE_TYPE_VAR) &&
            !ptr_value_provably_nonnull(e->some_expr.value))
            g_needs_stdio = true;
        break;
    }
    case EXPR_ERR:
        /* A not-provably-nonzero code emits the zero-code abort (fc_zero_err) */
        if (!int_value_provably_nonzero(e->err_expr.code))
            g_needs_stdio = true;
        break;
    case EXPR_ERROR_NAME:
        g_uses_error_name = true;
        break;
    case EXPR_ASSERT:
        g_needs_stdio = true;
        break;
    case EXPR_INDEX:
        /* Slice indexing emits a bounds-check abort with stderr message */
        if (e->index.object->type && e->index.object->type->kind == TYPE_SLICE)
            g_needs_stdio = true;
        break;
    case EXPR_SLICE:
        /* Subslice emits a bounds-check abort with stderr message */
        g_needs_stdio = true;
        break;
    case EXPR_STRUCT_LIT:
        /* Struct literal with fixed-array field emits an overflow abort with stderr */
        if (e->type && e->type->kind == TYPE_STRUCT) {
            for (int f = 0; f < e->type->struc.field_count; f++) {
                if (e->type->struc.fields[f].type->kind == TYPE_FIXED_ARRAY) {
                    g_needs_stdio = true;
                    break;
                }
            }
        }
        break;
    case EXPR_SLICE_LIT:
        /* A runtime-len slice literal emits a negative-length abort via stderr
         * (fc_neg_len). Provably non-negative lens skip the guard. */
        if (!e->slice_lit.len_nonneg)
            g_needs_stdio = true;
        break;
    case EXPR_IDENT:
        if (e->ident.is_std_stream) g_needs_stdio = true;
        break;
    default:
        break;
    }
    expr_for_each_child(e, detect_features_child, NULL);
}

static void detect_features_decl(Decl *d) {
    if (!d) return;
    switch (d->kind) {
    case DECL_LET: {
        Expr *fn = d->let.init;
        if (fn && fn->kind == EXPR_FUNC) {
            for (int i = 0; i < fn->func.body_count; i++)
                detect_features_expr(fn->func.body[i]);
        } else if (fn) {
            detect_features_expr(fn);
        }
        break;
    }
    case DECL_MODULE:
        for (int i = 0; i < d->module.decl_count; i++)
            detect_features_decl(d->module.decls[i]);
        break;
    default:
        break;
    }
}


static void collect_all_decls(Program *prog, Decl ***out_decls, int *out_count) {
    Decl **all = NULL;
    int count = 0, cap = 0;
    flatten_decls(prog->decls, prog->decl_count, &all, &count, &cap);
    *out_decls = all;
    *out_count = count;
}

/* ---- Lifted-lambda emission ----
 * Each pass runs once over the non-generic lambdas (no SubstCtx) and once per
 * monomorphized instance over that instance's lambdas, with g_subst /
 * g_lambda_suffix active so type-var-typed captures/params/returns resolve and
 * the emitted names are per-instance. */

static void emit_lambda_ctx_structs(LambdaSet *ls, FILE *out) {
    for (int i = 0; i < ls->count; i++) {
        Expr *lam = ls->exprs[i];
        if (lam->func.capture_count > 0) {
            fprintf(out, "typedef struct {");
            for (int j = 0; j < lam->func.capture_count; j++) {
                fprintf(out, " ");
                emit_type(lam->func.captures[j].type, out);
                fprintf(out, " %s;", lam->func.captures[j].codegen_name);
            }
            fprintf(out, " } _ctx_%s;\n", lambda_c_name(lam));
        }
    }
}

static void emit_lambda_fwd_decls(LambdaSet *ls, FILE *out) {
    for (int i = 0; i < ls->count; i++) {
        Expr *lam = ls->exprs[i];
        emit_fn_signature(lambda_c_name(lam), lam->type->func.return_type,
                          lam->func.params, lam->func.param_count, out);
        fprintf(out, ";\n");
    }
}

static void emit_lambda_defs(LambdaSet *ls, FILE *out) {
    for (int i = 0; i < ls->count; i++) {
        Expr *lam = ls->exprs[i];
        Type *ft = lam->type;
        const char *ln = lambda_c_name(lam);
        emit_fn_signature(ln, ft->func.return_type, lam->func.params,
                          lam->func.param_count, out);
        fprintf(out, " {\n");

        g_indent_level = 1;
        if (lam->func.capture_count > 0) {
            /* Extract captures from context struct */
            emit_indent(out);
            fprintf(out, "_ctx_%s* _c = (_ctx_%s*)_ctx;\n", ln, ln);
            for (int j = 0; j < lam->func.capture_count; j++) {
                emit_indent(out);
                emit_type(lam->func.captures[j].type, out);
                fprintf(out, " %s = _c->%s;\n",
                    lam->func.captures[j].codegen_name,
                    lam->func.captures[j].codegen_name);
            }
        } else {
            emit_indent(out);
            fprintf(out, "(void)_ctx;\n");
        }

        /* Self-recursion: materialize the binding as a local fat pointer so the body
           can call/use itself by name. Non-capturing passes a NULL context; capturing
           threads the same _ctx through recursive calls. Emitted only when the name was
           actually referenced, so non-recursive lambdas stay -Werror-clean. */
        if (lam->func.self_codegen_name && lam->func.self_referenced) {
            emit_indent(out);
            emit_type(lam->type, out);
            fprintf(out, " %s = { .fn_ptr = %s, .ctx = %s };\n",
                lam->func.self_codegen_name, ln,
                lam->func.capture_count > 0 ? "_ctx" : "NULL");
        }

        g_guards_suppressed = false;   /* lambda body: guards on (function boundary) */
        g_overflow_checked = false;    /* lambda body: unchecked (function boundary) */
        begin_hoisted_scope(lam->func.body, lam->func.body_count, out);
        defer_scope_push(false);
        emit_block_stmts(lam->func.body, lam->func.body_count, out, true, true);
        defer_scope_pop();
        end_hoisted_scope();
        g_indent_level = 0;
        fprintf(out, "}\n\n");
    }
}

/* What collect_code gathers for emit_functions. */
typedef struct {
    LambdaSet lambdas;          /* lambdas of the non-generic declarations */
    LambdaSet *inst_lambdas;    /* per generic instance, indexed like mono->entries */
    TrampolineSet trampolines;
} EmittedCode;

/* The #defines and #includes the program needs, then the settings every
 * later declaration assumes (GCC's block placement under --backtraces,
 * Windows abort behavior). */
static void emit_preamble(FILE *out, CDefine *defines, int define_count,
                          const char **from_libs, int from_lib_count) {
    /* Module `define`s come before all includes */
    for (int i = 0; i < define_count; i++)
        fprintf(out, "#define %s %s\n", defines[i].macro, defines[i].value);

    /* Core headers, always emitted (FC's platform contract) */
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include <stddef.h>\n");
    fprintf(out, "#include <stdbool.h>\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n");
    fprintf(out, "#include <assert.h>\n");
    fprintf(out, "#include <limits.h>\n");

    /* On Windows, FC programs must link against UCRT; msvcrt lacks symbols
     * the emitted code relies on (e.g. _set_abort_behavior). Fail here with a
     * clear message rather than with confusing link errors. The check comes
     * after the core headers because _UCRT is defined in MinGW-w64's
     * <_mingw.h> (pulled in transitively by <stdint.h> et al.), not by the
     * compiler itself. */
    fprintf(out,
        "#if defined(_WIN32) && !defined(_UCRT)\n"
        "#error \"FC on Windows requires the UCRT runtime; msvcrt is not supported.\"\n"
        "#endif\n");

    /* Feature-gated headers, emitted only when needed.  --backtraces forces
     * <stdio.h> because the emitted fc_dump_backtrace helper uses fprintf/stderr
     * regardless of whether user code does any I/O. */
    if (g_needs_stdio || g_backtraces) fprintf(out, "#include <stdio.h>\n");
    if (g_needs_math)  fprintf(out, "#include <math.h>\n");
    if (g_needs_float) fprintf(out, "#include <float.h>\n");
    if (g_needs_errno) fprintf(out, "#include <errno.h>\n");

    /* Emit #include for each unique from_lib in extern modules */
    for (int i = 0; i < from_lib_count; i++) {
        /* Skip headers already emitted in core or feature-gated preamble */
        if (strcmp(from_libs[i], "stdint.h") == 0 ||
            strcmp(from_libs[i], "stddef.h") == 0 ||
            strcmp(from_libs[i], "stdbool.h") == 0 ||
            strcmp(from_libs[i], "stdlib.h") == 0 ||
            strcmp(from_libs[i], "string.h") == 0)
            continue;
        if (g_needs_stdio && strcmp(from_libs[i], "stdio.h") == 0) continue;
        if (g_needs_math  && strcmp(from_libs[i], "math.h") == 0) continue;
        if (g_needs_float && strcmp(from_libs[i], "float.h") == 0) continue;
        if (g_needs_errno && strcmp(from_libs[i], "errno.h") == 0) continue;
        fprintf(out, "#include <%s>\n", from_libs[i]);
    }
    fprintf(out, "\n");

    /* --backtraces: keep FC frames visible to the execinfo stack walk.
     * fc_dump_backtrace maps a return address to the FC function whose &fn is the
     * largest one <= it, which mis-fires if a function's code is hot/cold-split
     * into .text.unlikely: a return address in the cold shard lands outside every
     * function's [&fn, &next) range and gets attributed to the wrong function.
     * On GCC (which enables -freorder-blocks-and-partition at -O2) a pragma
     * disables that split TU-wide for functions defined below; it only
     * changes code layout, not behavior.  Clang doesn't do this split by
     * default, so it needs no pragma.  The pragma is fenced to GCC (Clang
     * would reject the unknown pragma under -Werror; Clang also defines
     * __GNUC__, hence the !defined(__clang__)).  Tail-call optimization stays
     * on: TCO is semantic in FC (see g_fn_attr). */
    if (g_backtraces) {
        fprintf(out,
            "#if defined(__GNUC__) && !defined(__clang__)\n"
            "#  pragma GCC optimize(\"no-reorder-blocks-and-partition\")\n"
            "#endif\n\n");
    }

    /* Windows: suppress the Watson/Windows-Error-Reporting dialog when
     * abort() fires (bounds checks, option unwrap, div-by-zero, assert).
     * Without this, CI and test runs on Windows hang on the error popup.
     * GCC constructor runs before main. UCRT is guaranteed by the prelude
     * #error above, so _set_abort_behavior is always available here. */
    fprintf(out,
        "#ifdef _WIN32\n"
        "__attribute__((unused, constructor))\n"
        "static void fc_win_abort_init(void) {\n"
        "    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);\n"
        "}\n"
        "#endif\n");
}

/* The runtime support every program gets: the slice length type, fc_str,
 * the narrowing and float-to-int helpers, the abort macro, the failure
 * helpers the checks call, and the error-name table. */
static void emit_runtime_support(FILE *out) {
    /* Slice length storage (--len-repr). The FC-level type of every length is
     * i64 on every profile; fc_len_t is only the stored width. Soundness:
     * every stored len is proven in [0, FC_LEN_MAX] at slice construction
     * (statically for compile-time lens, via fc_chk_len otherwise), so reads
     * widen losslessly and guards may compare at stored width. */
    fprintf(out, "typedef int%d_t fc_len_t;\n", g_len_repr);
    fprintf(out, "#define FC_LEN_MAX INT%d_MAX\n", g_len_repr);

    /* Always emit fc_str (alias for uint8 slice) */
    fprintf(out, "typedef struct { uint8_t* ptr; fc_len_t len; } fc_str;\n");

    /* Narrowing-conversion guards. FC uses int64 for all slice/string lengths
     * and sizes; these helpers assert that a value fits in the target width
     * before narrowing. On 64-bit hosts SIZE_MAX == UINT64_MAX so the size_t
     * check is trivially true; on 32-bit (and smaller) hosts the assert fires
     * if user-derived math overflows size_t. Under -DNDEBUG the assert
     * compiles out and the helper reduces to a plain cast. */
    fprintf(out,
        "__attribute__((unused))\n"
        "static inline size_t fc_to_size(int64_t n) {\n"
        "    assert(n >= 0 && (uint64_t)n <= SIZE_MAX);\n"
        "    return (size_t)n;\n"
        "}\n"
        "__attribute__((unused))\n"
        "static inline int fc_to_int(int64_t n) {\n"
        "    assert(n >= 0 && n <= INT_MAX);\n"
        "    return (int)n;\n"
        "}\n"
        /* Element/byte count for a heap request that may legitimately be zero
         * (alloc(T, 0), an empty slice literal, a copy of an empty slice).
         * C leaves a zero-size malloc/calloc implementation-defined: it may
         * return NULL, which FC's option would read as an allocation failure,
         * so the same program would abort on one libc and not another. Ask for
         * one unit instead, giving a unique freeable pointer on every
         * platform; the FC-level length stays 0, so nothing may be read
         * through it. A constant count folds this away entirely. */
        "__attribute__((unused))\n"
        "static inline size_t fc_alloc_n(int64_t n) {\n"
        "    assert(n >= 0 && (uint64_t)n <= SIZE_MAX);\n"
        "    return n > 0 ? (size_t)n : 1;\n"
        "}\n");

    /* Saturating float->int conversion helpers. A raw C cast of an
     * out-of-range or NaN float to an integer is undefined behavior (and
     * observably divergent across -O levels), so FC defines `(intT) floatexpr` to
     * saturate. These are the runtime form (single operand evaluation); the
     * const-context form is an equivalent ternary emitted at the use site. Both
     * read their bounds from float_to_int_info. Each takes `double`: a float32
     * operand promotes losslessly, so the bounds and truncation are the same
     * regardless of source width. Fixed-width targets use absolute constants
     * (int-width-agnostic); isize/usize use PTRDIFF_MIN/SIZE_MAX. */
    {
        static const TypeKind f2i_kinds[] = {
            TYPE_INT8, TYPE_UINT8, TYPE_INT16, TYPE_UINT16, TYPE_INT32,
            TYPE_UINT32, TYPE_INT64, TYPE_UINT64, TYPE_ISIZE, TYPE_USIZE,
        };
        for (size_t i = 0; i < sizeof f2i_kinds / sizeof f2i_kinds[0]; i++) {
            F2iInfo fi = {0};   /* table holds only handled kinds; {0} silences
                                 * -Wmaybe-uninitialized for the unreachable default */
            float_to_int_info(f2i_kinds[i], &fi);
            fprintf(out,
                "__attribute__((unused)) static inline %s %s(double f) { "
                "if (f!=f) return 0; if (f < %s) return %s; if (f >= %s) return %s; "
                "return (%s)f; }\n",
                fi.cty, fi.fn, fi.lo, fi.imin, fi.hi, fi.imax, fi.cty);
        }
    }

    /* Abort macro.  When --backtraces is on, every abort site dumps a
     * backtrace before bailing.  Forward-declare fc_dump_backtrace here so
     * callers can see it; body and the FC-symbol mapping table are emitted at
     * the end of the TU once all emitted function names are known. */
    if (g_backtraces) {
        fprintf(out, "__attribute__((cold, unused)) static void fc_dump_backtrace(void);\n");
        fprintf(out, "#define FC_ABORT() ({ fc_dump_backtrace(); abort(); })\n");
    } else {
        fprintf(out, "#define FC_ABORT() abort()\n");
    }

    /* Out-of-bounds failure helpers.  Cold+noreturn so the hot path stays
     * clean: the compiler can schedule these out of line, skip keeping
     * caller state live across the call, and is free to vectorize/reorder
     * loads around checked accesses.  Static so unused copies get DCE'd. */
    if (g_needs_stdio) {
        fprintf(out,
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_oob(const char *file, int line, long long idx, unsigned long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: slice index out of range: index=%%lld len=%%llu\\n\",\n"
            "            file, line, idx, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_oob_u(const char *file, int line, unsigned long long idx, unsigned long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: slice index out of range: index=%%llu len=%%llu\\n\",\n"
            "            file, line, idx, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_oob_sub(const char *file, int line, long long lo, long long hi, long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: subslice out of range: lo=%%lld hi=%%lld len=%%lld\\n\",\n"
            "            file, line, lo, hi, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_neg_len(const char *file, int line, long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: slice literal length is negative: len=%%lld\\n\",\n"
            "            file, line, len);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_len_cap(const char *file, int line, long long len) {\n"
            "    fprintf(stderr, \"%%s:%%d: slice length %%lld exceeds length capacity %%lld\\n\",\n"
            "            file, line, len, (long long)FC_LEN_MAX);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((unused))\n"
            "static inline fc_len_t fc_chk_len(const char *file, int line, int64_t n) {\n"
            "    if (__builtin_expect(n < 0, 0)) fc_neg_len(file, line, (long long)n);\n"
            "    if (__builtin_expect(n > FC_LEN_MAX, 0)) fc_len_cap(file, line, (long long)n);\n"
            "    return (fc_len_t)n;\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_null_some(const char *file, int line) {\n"
            "    fprintf(stderr, \"%%s:%%d: some() of a null pointer "
            "(pointer options use null as the none sentinel)\\n\", file, line);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_zero_err(const char *file, int line) {\n"
            "    fprintf(stderr, \"%%s:%%d: err() with code 0 "
            "(0 is the ok tag; error codes must be non-zero)\\n\", file, line);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_overflow(const char *file, int line, const char *what) {\n"
            "    fprintf(stderr, \"%%s:%%d: integer overflow in %%s\\n\", file, line, what);\n"
            "    FC_ABORT();\n"
            "}\n"
            "__attribute__((cold, noreturn, unused))\n"
            "static void fc_trunc(const char *file, int line, const char *what, long long len, long long max) {\n"
            "    fprintf(stderr, \"%%s:%%d: string truncation in %%s: len=%%lld max=%%lld\\n\",\n"
            "            file, line, what, len, max);\n"
            "    FC_ABORT();\n"
            "}\n");
        /* Register the helpers as skip-entries so their frames don't pollute
         * the user-visible backtrace when a bounds check fires. */
        symmap_add("fc_oob", NULL, "<runtime>", 0);
        symmap_add("fc_oob_u", NULL, "<runtime>", 0);
        symmap_add("fc_oob_sub", NULL, "<runtime>", 0);
        symmap_add("fc_neg_len", NULL, "<runtime>", 0);
        symmap_add("fc_len_cap", NULL, "<runtime>", 0);
        symmap_add("fc_chk_len", NULL, "<runtime>", 0);
        symmap_add("fc_null_some", NULL, "<runtime>", 0);
        symmap_add("fc_zero_err", NULL, "<runtime>", 0);
        symmap_add("fc_overflow", NULL, "<runtime>", 0);
        symmap_add("fc_trunc", NULL, "<runtime>", 0);
    }

    /* Declared-error name table + O(1) lookup: backs error_name() and, under
     * --backtraces, the named result-unwrap abort. It is static data, emitted
     * only when error_name is used or --backtraces (which trades static data
     * for readable failures) is on. Codes are assigned contiguously from
     * FC_ERROR_CODE_BASE, so the table is indexed directly by (e - base). */
    if (g_uses_error_name || (g_backtraces && error_code_count() > 0)) {
        int n = error_code_count();
        fprintf(out, "typedef struct { const char *name; int32_t len; } fc_errname;\n");
        if (n > 0) {
            fprintf(out, "static const fc_errname fc_errnames[%d] = {\n", n);
            for (int i = 0; i < n; i++) {
                ErrorCodeInfo info = error_code_info(i);
                fprintf(out, "    { \"");
                emit_c_escaped(info.qualified, (int)strlen(info.qualified), out);
                fprintf(out, "\", %d },\n", (int)strlen(info.qualified));
            }
            fprintf(out, "};\n");
            /* %dL literals keep the comparisons int-width-agnostic (a 16-bit
             * int platform promotes to long on both sides). */
            fprintf(out,
                "__attribute__((unused))\n"
                "static const fc_errname *fc_error_lookup(int32_t e) {\n"
                "    if (e < %dL || e >= %dL + %dL) return 0;\n"
                "    return &fc_errnames[e - %dL];\n"
                "}\n",
                FC_ERROR_CODE_BASE, FC_ERROR_CODE_BASE, n, FC_ERROR_CODE_BASE);
        } else {
            fprintf(out,
                "__attribute__((unused))\n"
                "static const fc_errname *fc_error_lookup(int32_t e) { (void)e; return 0; }\n");
        }
        g_errname_emitted = true;
    }
}

/* Every slice, option, result and function type the program uses, and
 * every struct and union (generic instances included), declared and
 * defined in an order C accepts, followed by the equality and enum_of
 * helpers they need. */
static void emit_types(FILE *out, Decl **all_decls, int all_count, MonoTable *mono) {
    /* Collect all slice, option, result, function, and eq types used in the
     * program. Results ride a module-level set (like g_eq_set) rather than a
     * fourth parameter threaded through every collect_* call. */
    TypeSet slices = {0};
    TypeSet options = {0};
    TypeSet results = {0};
    TypeSet fns = {0};
    TypeSet eqs = {0};
    TypeSet enum_ofs = {0};
    TypeSets sets = { &slices, &options, &fns };
    g_eq_set = &eqs;
    g_results_set = &results;
    g_enum_of_set = &enum_ofs;

    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_LET) {
            collect_types_in_type(d->let.resolved_type, &slices, &options, &fns);
            collect_types_expr(d->let.init, &sets);
        }
        if (d->kind == DECL_STRUCT) {
            for (int j = 0; j < d->struc.field_count; j++)
                collect_types_in_type(d->struc.fields[j].type, &slices, &options, &fns);
        }
        if (d->kind == DECL_UNION) {
            for (int j = 0; j < d->unio.variant_count; j++)
                collect_types_in_type(d->unio.variants[j].payload, &slices, &options, &fns);
        }
    }
    /* Also collect types from monomorphized instances */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->concrete_type) {
            if (inst->concrete_type->kind == TYPE_STRUCT) {
                for (int f = 0; f < inst->concrete_type->struc.field_count; f++)
                    collect_types_in_type(inst->concrete_type->struc.fields[f].type, &slices, &options, &fns);
            } else if (inst->concrete_type->kind == TYPE_UNION) {
                for (int v = 0; v < inst->concrete_type->unio.variant_count; v++)
                    collect_types_in_type(inst->concrete_type->unio.variants[v].payload, &slices, &options, &fns);
            }
        }
    }

    /* Also collect types from monomorphized function bodies (with substitution) */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst;
        enter_instance(inst, &subst);
        for (int j = 0; j < fn->func.body_count; j++)
            collect_types_expr(fn->func.body[j], &sets);
        leave_instance();
    }
    g_eq_set = NULL;
    g_results_set = NULL;

    /* Enum typedefs: complete scalar types with no dependencies, emitted
       before every forward declaration so struct fields, option/result inners,
       and slice elements can use them as complete types. */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_ENUM) emit_enum_typedef(d, out);
    }

    /* Emit forward declarations for all structs and unions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_STRUCT && !d->struc.is_extern) emit_struct_forward(d, out);
        else if (d->kind == DECL_UNION) emit_union_forward(d, out);
    }
    /* Forward declarations for monomorphized structs/unions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind == DECL_STRUCT || inst->decl_kind == DECL_UNION)
            fprintf(out, "typedef struct %s %s;\n", inst->mangled_name, inst->mangled_name);
    }

    /* Emit union tag enums (skip generics) */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_UNION)
            emit_union_tag_enum(d->unio.name, d->unio.variants, d->unio.variant_count, out);
    }
    /* Tag enums for monomorphized unions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_UNION || !inst->concrete_type) continue;
        Type *ct = inst->concrete_type;
        emit_union_tag_enum(inst->mangled_name, ct->unio.variants, ct->unio.variant_count, out);
    }

    /* Forward-declare all option typedefs (as named struct tags). This lets slice
       typedefs reference fc_option_T* without requiring the option body to be
       emitted first, breaking the slice<option<T>> vs option<slice<T>> cycle. */
    for (int i = 0; i < options.count; i++) {
        Type *o = options.types[i];
        fprintf(out, "typedef struct fc_option_");
        if (o->option.inner) emit_type_ident(o->option.inner, out);
        else fprintf(out, "void");
        fprintf(out, " fc_option_");
        if (o->option.inner) emit_type_ident(o->option.inner, out);
        else fprintf(out, "void");
        fprintf(out, ";\n");
    }

    /* Forward-declare all result typedefs, for the same cycle-breaking reasons. */
    for (int i = 0; i < results.count; i++) {
        Type *r = results.types[i];
        fprintf(out, "typedef struct fc_result_");
        if (r->result.inner) emit_type_ident(r->result.inner, out);
        else fprintf(out, "void");
        fprintf(out, " fc_result_");
        if (r->result.inner) emit_type_ident(r->result.inner, out);
        else fprintf(out, "void");
        fprintf(out, ";\n");
    }

    /* Slice typedefs are split into a forward declaration (named struct tag) and
       a body. The forward decls precede the function typedefs so a function type
       can take a slice by value as a parameter (an incomplete by-value param is
       fine in a function-pointer typedef). The bodies follow the fn typedefs so
       a slice of a function type can name the already-declared fn typedef in its
       `elem* ptr` field. Both precede the scalar option bodies below, since an
       option-of-slice embeds the slice body by value. */
    for (int i = 0; i < slices.count; i++) {
        Type *s = slices.types[i];
        if (is_str_type(s)) continue; /* str already emitted */
        fprintf(out, "typedef struct fc_slice_");
        emit_slice_elem_ident(s->slice.elem, out);
        fprintf(out, "_s fc_slice_");
        emit_slice_elem_ident(s->slice.elem, out);
        fprintf(out, ";\n");
    }

    /* Function typedefs. A function-pointer typedef tolerates incomplete
       by-value struct/union/option/slice params and return (C only requires
       those complete at a call or definition, not at the pointer-type
       declaration), and every struct/union/option/slice is forward-declared
       above, so all fn typedefs can be emitted here in one pass, before the
       struct/union defs. The `fns` set is dependency-ordered inner-first
       (collect_types_in_type recurses param/return before adding the outer fn),
       so a fn type used by value inside another fn's signature is declared first. */
    for (int i = 0; i < fns.count; i++) {
        Type *f = fns.types[i];
        fprintf(out, "typedef struct { ");
        emit_type(f->func.return_type, out);
        fprintf(out, " (*fn_ptr)(");
        for (int j = 0; j < f->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            emit_type(f->func.param_types[j], out);
        }
        if (f->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "void*); void* ctx; } ");
        emit_type(f, out);
        fprintf(out, ";\n");
    }

    /* Slice bodies. The fn typedefs above are complete, so a slice of a
       function type can reference it by pointer. */
    for (int i = 0; i < slices.count; i++) {
        Type *s = slices.types[i];
        if (is_str_type(s)) continue;
        fprintf(out, "struct fc_slice_");
        emit_slice_elem_ident(s->slice.elem, out);
        fprintf(out, "_s { ");
        emit_elem_type(s->slice.elem, out);
        fprintf(out, "* ptr; fc_len_t len; };\n");
    }

    /* Emit option/result bodies whose inner types are already complete:
       primitives, pointers, slices, and (since the fn typedefs above are done)
       function types, plus any ?!-composition over those. Wrappers over
       struct/union/stub inners are deferred to the def interleave below, where
       each newly-emitted def unlocks the wrappers embedding it. */
    WrapEmit we = {
        .options = &options, .results = &results,
        .opt_done = calloc(options.count > 0 ? (size_t)options.count : 1, sizeof(bool)),
        .res_done = calloc(results.count > 0 ? (size_t)results.count : 1, sizeof(bool)),
        .defs_done = NULL, .defs_done_count = 0,
        .out = out,
    };
    emit_wrapper_bodies(&we);

    /* One topological sort of all struct/union definitions, top-level
       (non-generic) decls and monomorphized instances (including synthesized
       tuples) together, so by-value dependencies emit before their dependents
       no matter which kind references which (a top-level struct holding a tuple
       by value, or a generic instance holding a top-level struct by value). */
    SuDef *defs = NULL;
    int def_count = 0, def_cap = 0;
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_generic_decl(d)) continue;
        if (d->kind == DECL_STRUCT || d->kind == DECL_UNION) {
            SuDef s = { decl_su_name(d), d, NULL };
            DA_APPEND(defs, def_count, def_cap, s);
        }
    }
    for (int m = 0; m < mono->count; m++) {
        MonoInstance *inst = &mono->entries[m];
        if (!inst->concrete_type) continue;
        if (inst->decl_kind == DECL_STRUCT || inst->decl_kind == DECL_UNION) {
            SuDef s = { inst->mangled_name, NULL, inst };
            DA_APPEND(defs, def_count, def_cap, s);
        }
    }
    int *def_state = calloc((size_t)(def_count > 0 ? def_count : 1), sizeof(int));
    int *def_order = malloc(sizeof(int) * (size_t)(def_count > 0 ? def_count : 1));
    int def_order_count = 0;
    for (int i = 0; i < def_count; i++)
        topo_visit_sudef(defs, def_count, i, def_state, def_order, &def_order_count);
    free(def_state);

    /* Emit definitions in dependency order, interleaving option/result bodies
       that wrap each newly-defined struct/union/tuple. */
    const char **defs_done = malloc(sizeof(char*) * (size_t)(def_count > 0 ? def_count : 1));
    we.defs_done = defs_done;
    for (int oi = 0; oi < def_order_count; oi++) {
        SuDef *s = &defs[def_order[oi]];
        const char *def_name = s->name;
        if (s->decl) {
            Decl *d = s->decl;
            if (d->kind == DECL_STRUCT) {
                if (!d->struc.is_extern)
                    emit_struct_def(d->struc.name, d->struc.fields, d->struc.field_count, out);
            } else if (d->kind == DECL_UNION) {
                emit_union_def(d->unio.name, d->unio.variants, d->unio.variant_count, out);
            }
        } else if (s->mi) {
            MonoInstance *inst = s->mi;
            Type *ct = inst->concrete_type;
            if (inst->decl_kind == DECL_STRUCT)
                emit_struct_def(inst->mangled_name, ct->struc.fields, ct->struc.field_count, out);
            else if (inst->decl_kind == DECL_UNION)
                emit_union_def(inst->mangled_name, ct->unio.variants, ct->unio.variant_count, out);
        }
        if (!def_name) continue;
        defs_done[we.defs_done_count++] = def_name;
        emit_wrapper_bodies(&we);
    }
    free(def_order);
    free(defs);
    free(defs_done);
    free(we.opt_done);
    free(we.res_done);

    /* Emit eq function forward declarations and definitions */
    for (int i = 0; i < eqs.count; i++)
        emit_eq_forward(eqs.types[i], out);
    for (int i = 0; i < eqs.count; i++)
        emit_eq_func(eqs.types[i], out);

    /* Emit enum_of membership-check helpers */
    for (int i = 0; i < enum_ofs.count; i++)
        emit_enum_of_helpers(enum_ofs.types[i], out);

    free(slices.types);
    free(options.types);
    free(results.types);
    free(fns.types);
    free(eqs.types);
    free(enum_ofs.types);

    fprintf(out, "\n");
}

/* The file-level globals of the entry file, whose initializers run at the
 * start of C main (g_file_globals). */
static void collect_file_globals(Decl **all_decls, int all_count) {
    /* Collect file-level non-function globals for hoisted initialization in C
     * main. (The program has a main: pass1 requires one for a compile.) */
    g_file_global_count = 0;
    g_file_globals = NULL;
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_LET && !is_func_decl(d) && !d->let.is_module_member)
            g_file_global_count++;
    }
    if (g_file_global_count > 0) {
        g_file_globals = malloc(sizeof(Decl*) * (size_t)g_file_global_count);
        int idx = 0;
        for (int i = 0; i < all_count; i++) {
            Decl *d = all_decls[i];
            if (d->kind == DECL_LET && !is_func_decl(d) && !d->let.is_module_member)
                g_file_globals[idx++] = d;
        }
    }
}

/* Prototypes for every function and generic instance, so definitions may
 * appear in any order. */
static void emit_prototypes(FILE *out, Decl **all_decls, int all_count, MonoTable *mono) {
    /* Emit forward declarations for functions (with void* _ctx), skip generics.
     * main is emitted as fc_main with its str[] param (no _ctx). */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (is_func_decl(d) && is_entry_point(d) && !is_generic_decl(d)) {
            Expr *fn = d->let.init;
            fprintf(out, "%sint32_t fc_main(", g_fn_attr);
            emit_type(fn->func.params[0].type, out);
            fprintf(out, " %s);\n", param_c_name(&fn->func.params[0]));
            symmap_add("fc_main", "main", d->loc.filename, d->loc.line);
            continue;
        }
        if (is_func_decl(d) && !is_entry_point(d) && !is_generic_decl(d)) {
            const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            Expr *fn = d->let.init;
            emit_fn_signature(cname, d->let.resolved_type->func.return_type,
                              fn->func.params, fn->func.param_count, out);
            fprintf(out, ";\n");
            symmap_add(cname, d->let.name, d->loc.filename, d->loc.line);
        }
    }
    /* Forward declarations for monomorphized functions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        /* Set up substitution context */
        SubstCtx subst;
        enter_instance(inst, &subst);
        emit_fn_signature(inst->mangled_name, fn->type->func.return_type,
                          fn->func.params, fn->func.param_count, out);
        fprintf(out, ";\n");
        leave_instance();
        if (g_backtraces) {
            const char *disp = fmt_mono_display(g_arena, tmpl->let.name,
                                                inst->type_args, inst->type_param_count);
            symmap_add(inst->mangled_name, disp, tmpl->loc.filename, tmpl->loc.line);
        }
    }
    fprintf(out, "\n");
}

/* The lambdas to lift to C functions and the trampolines to emit for
 * functions passed where C expects a C function pointer. */
static void collect_code(EmittedCode *code, Decl **all_decls, int all_count,
                         MonoTable *mono) {
    /* Collect code->lambdas from all non-generic declarations.  Lambdas inside
     * generic bodies are collected per mono instance instead: they are emitted
     * once per instantiation, under that instance's SubstCtx and with the
     * instance's mangled name suffixed onto every lifted name (a capture,
     * param, or return typed by the enclosing type variable has no concrete
     * type outside an instantiation). */
    for (int i = 0; i < all_count; i++) {
        if (all_decls[i]->kind == DECL_LET && all_decls[i]->let.init &&
            !is_generic_decl(all_decls[i])) {
            collect_lambdas_expr(all_decls[i]->let.init, &code->lambdas);
        }
    }
    code->inst_lambdas = mono->count > 0
        ? calloc((size_t)mono->count, sizeof(LambdaSet)) : NULL;
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        if (!inst->template_decl->let.init ||
            inst->template_decl->let.init->kind != EXPR_FUNC) continue;
        collect_lambdas_expr(inst->template_decl->let.init, &code->inst_lambdas[mi]);
    }
    /* Record code->lambdas in symmap (for --backtraces frames) */
    if (g_backtraces) {
        for (int i = 0; i < code->lambdas.count; i++) {
            Expr *lam = code->lambdas.exprs[i];
            if (!lam->func.lifted_name) continue;
            /* The backtrace prints "defined at file:line" beside the name. */
            symmap_add(lam->func.lifted_name, "<lambda>", lam->loc.filename, lam->loc.line);
        }
        for (int mi = 0; mi < mono->count; mi++) {
            SubstCtx subst;
            enter_instance(&mono->entries[mi], &subst);
            for (int i = 0; i < code->inst_lambdas[mi].count; i++) {
                Expr *lam = code->inst_lambdas[mi].exprs[i];
                if (!lam->func.lifted_name) continue;
                symmap_add(lambda_c_name(lam), "<lambda>", lam->loc.filename, lam->loc.line);
            }
            leave_instance();
        }
    }

    /* Collect code->trampolines: FC functions handed to C as raw function
     * pointers. Walk all non-generic bodies and all monomorphized bodies. */
    for (int i = 0; i < all_count; i++) {
        if (all_decls[i]->kind == DECL_LET && all_decls[i]->let.init &&
            !is_generic_decl(all_decls[i])) {
            collect_trampolines_expr(all_decls[i]->let.init, &code->trampolines);
        }
    }
    /* Also walk monomorphized function bodies */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET || !inst->template_decl) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst;
        enter_instance(inst, &subst);
        for (int j = 0; j < fn->func.body_count; j++)
            collect_trampolines_expr(fn->func.body[j], &code->trampolines);
        leave_instance();
    }
}

/* Global data: the static backing arrays of slice literals in module
 * constants, then every non-function global. */
static void emit_globals(FILE *out, Decl **all_decls, int all_count) {
    /* Pre-pass: walk module-member inits and lift every EXPR_ARRAY_LIT into a
     * static backing array.  Nested array lits inside struct/variant/some
     * initializers are all collected; the use sites then emit slice headers
     * referencing these backings.  Only module members are walked: file-level
     * lets are initialized in C main and use the normal function-scope
     * emission. */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_LET && !is_func_decl(d) && d->let.is_module_member &&
            d->let.init && d->let.init->kind != EXPR_FUNC) {
            bool rodata = d->let.is_frozen;
            collect_const_backings(d->let.init, &rodata);
        }
    }

    /* Emit the static backing arrays themselves.  Each is `static T name[] =
     * { e0, e1, ... };`; elements are emitted in const context so nested
     * struct/variant/some stay on the aggregate-initializer path. */
    if (g_const_backing_count > 0) {
        g_const_context = true;
        for (int i = 0; i < g_const_backing_count; i++) {
            Expr *al = g_const_backings[i];
            /* A frozen module constant's backing is emitted `static const` so
             * it can live in read-only memory. pass2 rejects every write
             * through it, so the .ptr cast at the use site casts away a const
             * nothing can reach. */
            fprintf(out, al->array_lit.codegen_backing_rodata
                         ? "static const " : "static ");
            emit_elem_type(al->array_lit.elem_type, out);
            if (al->array_lit.elem_count == 0) {
                /* Empty form `T[N]{ }`: N zero-initialized elements. */
                fprintf(out, " %s[%" PRIu64 "] = {0};\n",
                        al->array_lit.codegen_backing_name,
                        al->array_lit.size_expr->int_lit.value);
            } else {
                fprintf(out, " %s[] = {", al->array_lit.codegen_backing_name);
                for (int j = 0; j < al->array_lit.elem_count; j++) {
                    if (j > 0) fprintf(out, ", ");
                    emit_expr(al->array_lit.elems[j], out);
                }
                fprintf(out, "};\n");
            }
        }
        g_const_context = false;
        fprintf(out, "\n");
    }

    /* Emit non-function global variable definitions.
     * Must come before lifted lambdas so they can reference globals.
     * Module members are emitted with const-expr initializers at C file scope.
     * File-level globals are declared without initializers here; their
     * initialization is hoisted into the C main wrapper.  Both carry a mangled
     * codegen_name (fc__name); the two are told apart by is_module_member. */
    for (int i = 0; i < all_count; i++) {
        Decl *d = all_decls[i];
        if (d->kind == DECL_LET && !is_func_decl(d)) {
            const char *cname = d->let.codegen_name ? d->let.codegen_name : d->let.name;
            emit_type(d->let.resolved_type, out);
            if (d->let.is_module_member) {
                /* Module member: emit with initializer (must be const expr).
                 * Flip const context so array/struct-with-fixed-array/etc.
                 * take the aggregate-initializer path.
                 *
                 * A read-only module constant takes C's `const` so the object
                 * itself can live in read-only memory. It is spelled east
                 * (`T const name`) because that is the one placement that is
                 * correct for every emitted type form: on a pointer constant
                 * `uint8_t *const` freezes the pointer while leaving the
                 * pointee writable, which is the MMIO rule
                 * (`let vga = (u8*) 0xA0000usize`); a leading `const` would
                 * freeze the pointee instead. */
                if (!d->let.is_mut) fprintf(out, " const");
                fprintf(out, " %s = ", cname);
                g_const_context = true;
                emit_expr(d->let.init, out);
                g_const_context = false;
                fprintf(out, ";\n");
            } else {
                /* File-level global: declare only, init hoisted into C main */
                fprintf(out, " %s;\n", cname);
            }
        }
    }
    fprintf(out, "\n");
}

/* The lifted lambdas (context structs, prototypes, definitions), every
 * function and generic instance, and the trampolines. Frees `code`. */
static void emit_functions(FILE *out, Decl **all_decls, int all_count,
                           MonoTable *mono, EmittedCode *code) {
    /* Emit context structs for capturing code->lambdas */
    emit_lambda_ctx_structs(&code->lambdas, out);
    for (int mi = 0; mi < mono->count; mi++) {
        if (!code->inst_lambdas || code->inst_lambdas[mi].count == 0) continue;
        MonoInstance *inst = &mono->entries[mi];
        SubstCtx subst;
        enter_instance(inst, &subst);
        emit_lambda_ctx_structs(&code->inst_lambdas[mi], out);
        leave_instance();
    }

    /* Emit forward declarations for lifted code->lambdas */
    emit_lambda_fwd_decls(&code->lambdas, out);
    for (int mi = 0; mi < mono->count; mi++) {
        if (!code->inst_lambdas || code->inst_lambdas[mi].count == 0) continue;
        MonoInstance *inst = &mono->entries[mi];
        SubstCtx subst;
        enter_instance(inst, &subst);
        emit_lambda_fwd_decls(&code->inst_lambdas[mi], out);
        leave_instance();
    }
    /* Emit forward declarations for C-boundary code->trampolines */
    for (int i = 0; i < code->trampolines.count; i++) {
        emit_trampoline_signature(&code->trampolines.entries[i], out);
        fprintf(out, ";\n");
    }
    fprintf(out, "\n");

    /* Emit lifted lambda function definitions */
    emit_lambda_defs(&code->lambdas, out);
    for (int mi = 0; mi < mono->count; mi++) {
        if (!code->inst_lambdas || code->inst_lambdas[mi].count == 0) continue;
        MonoInstance *inst = &mono->entries[mi];
        SubstCtx subst;
        enter_instance(inst, &subst);
        emit_lambda_defs(&code->inst_lambdas[mi], out);
        leave_instance();
    }
    free(code->lambdas.exprs);
    if (code->inst_lambdas) {
        for (int mi = 0; mi < mono->count; mi++) free(code->inst_lambdas[mi].exprs);
        free(code->inst_lambdas);
    }

    /* Emit function definitions (skip generics) */
    for (int i = 0; i < all_count; i++) {
        if (is_func_decl(all_decls[i]) && !is_generic_decl(all_decls[i])) {
            emit_func_decl(all_decls[i], out);
        }
    }

    /* Emit monomorphized function definitions */
    for (int mi = 0; mi < mono->count; mi++) {
        MonoInstance *inst = &mono->entries[mi];
        if (inst->decl_kind != DECL_LET) continue;
        Decl *tmpl = inst->template_decl;
        if (!tmpl || !tmpl->let.init || tmpl->let.init->kind != EXPR_FUNC) continue;
        Expr *fn = tmpl->let.init;
        SubstCtx subst;
        enter_instance(inst, &subst);
        emit_fn_signature(inst->mangled_name, fn->type->func.return_type,
                          fn->func.params, fn->func.param_count, out);
        fprintf(out, " {\n    (void)_ctx;\n");
        g_indent_level = 1;
        g_guards_suppressed = false;   /* generic-instance body: guards on (function boundary) */
        g_overflow_checked = false;    /* generic-instance body: unchecked (function boundary) */
        begin_hoisted_scope(fn->func.body, fn->func.body_count, out);
        defer_scope_push(false);
        emit_block_stmts(fn->func.body, fn->func.body_count, out, true, true);
        defer_scope_pop();
        end_hoisted_scope();
        g_indent_level = 0;
        fprintf(out, "}\n\n");
        leave_instance();
    }

    /* Emit C-boundary trampoline definitions */
    for (int i = 0; i < code->trampolines.count; i++) {
        TrampolineEntry *te = &code->trampolines.entries[i];
        Type *ft = te->type;
        emit_trampoline_signature(te, out);
        fprintf(out, " {\n");
        if (ft->func.return_type->kind == TYPE_VOID) {
            fprintf(out, "    %s(", te->name);
        } else {
            fprintf(out, "    return %s(", te->name);
        }
        for (int j = 0; j < ft->func.param_count; j++) {
            if (j > 0) fprintf(out, ", ");
            fprintf(out, "_p%d", j);
        }
        if (ft->func.param_count > 0) fprintf(out, ", ");
        fprintf(out, "NULL);\n");
        fprintf(out, "}\n");
    }
    free(code->trampolines.entries);
}

/* The backtrace helper body and the FC-symbol mapping table.
 * Emitted at the end of the TU so all collected names (top-level functions,
 * monomorphized instances, lifted lambdas) are present in g_symmap.
 *
 * The table is keyed by function address (taken with &name).  At program
 * startup a constructor sorts it by address; lookup is a binary search
 * for the largest entry with fn_addr <= return_addr.  It does not use the
 * dynamic symbol table, so `-rdynamic` is not required.  Entries with
 * fc_name == NULL mark runtime helpers (fc_oob, fc_oob_sub, ...) whose
 * frames are left out of the printed backtrace.
 *
 * The body is gated on __linux__/__APPLE__ via #if; on other platforms
 * the helper is a no-op stub and abort() still fires.  The forward
 * declaration is in the preamble. */
static void emit_backtrace_table(FILE *out) {
    fprintf(out, "\n#if defined(__linux__) || defined(__APPLE__)\n");
    fprintf(out, "#include <execinfo.h>\n");
    fprintf(out, "#endif\n");
    fprintf(out, "typedef struct { void *fn_addr; const char *fc_name; const char *file; int line; } fc_sym_entry;\n");
    fprintf(out, "static fc_sym_entry _fc_symtab[] = {\n");
    for (int i = 0; i < g_symmap_count; i++) {
        FcSymEntry *e = &g_symmap[i];
        fprintf(out, "    { (void*)&%s, ", e->c_name);
        if (e->fc_name) {
            fprintf(out, "\"");
            emit_c_escaped(e->fc_name, (int)strlen(e->fc_name), out);
            fprintf(out, "\"");
        } else {
            fprintf(out, "(void*)0");
        }
        fprintf(out, ", \"");
        emit_c_escaped(e->file, (int)strlen(e->file), out);
        fprintf(out, "\", %d },\n", e->line);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const int _fc_symtab_count = %d;\n", g_symmap_count);
    fprintf(out,
        "static int _fc_sym_cmp(const void *a, const void *b) {\n"
        "    void *pa = ((const fc_sym_entry*)a)->fn_addr;\n"
        "    void *pb = ((const fc_sym_entry*)b)->fn_addr;\n"
        "    if ((uintptr_t)pa < (uintptr_t)pb) return -1;\n"
        "    if ((uintptr_t)pa > (uintptr_t)pb) return  1;\n"
        "    return 0;\n"
        "}\n"
        "__attribute__((constructor)) static void _fc_symtab_init(void) {\n"
        "    qsort(_fc_symtab, (size_t)_fc_symtab_count, sizeof _fc_symtab[0], _fc_sym_cmp);\n"
        "}\n");
    fprintf(out,
        "__attribute__((cold, unused))\n"
        "static void fc_dump_backtrace(void) {\n"
        "#if defined(__linux__) || defined(__APPLE__)\n"
        "    void *_frames[64];\n"
        "    int _n = backtrace(_frames, 64);\n"
        "    if (_n < 2) return;\n"
        "    fprintf(stderr, \"backtrace:\\n\");\n"
        "    int _printed = 0;\n"
        "    for (int _i = 1; _i < _n; _i++) {\n"
        "        /* Binary search: largest entry with fn_addr <= frame addr. */\n"
        "        uintptr_t _a = (uintptr_t)_frames[_i];\n"
        "        int _lo = 0, _hi = _fc_symtab_count;\n"
        "        while (_lo < _hi) {\n"
        "            int _mid = _lo + (_hi - _lo) / 2;\n"
        "            if ((uintptr_t)_fc_symtab[_mid].fn_addr <= _a) _lo = _mid + 1;\n"
        "            else _hi = _mid;\n"
        "        }\n"
        "        if (_lo == 0) {\n"
        "            fprintf(stderr, \"  #%%d %%p\\n\", _printed++, _frames[_i]);\n"
        "            continue;\n"
        "        }\n"
        "        const fc_sym_entry *_e = &_fc_symtab[_lo - 1];\n"
        "        if (!_e->fc_name) continue;  /* internal helper: skip */\n"
        "        fprintf(stderr, \"  #%%d %%-24s defined at %%s:%%d\\n\",\n"
        "                _printed++, _e->fc_name, _e->file, _e->line);\n"
        "        if (strcmp(_e->fc_name, \"main\") == 0) break;\n"
        "    }\n"
        "#else\n"
        "    (void)_fc_symtab;\n"
        "    (void)_fc_symtab_count;\n"
        "#endif\n"
        "}\n");
}

void codegen_emit(Program *prog, FILE *out, MonoTable *mono,
                  Arena *arena, InternTable *intern_tbl, SymbolTable *symtab,
                  const CodegenOptions *opts) {
    g_mono = mono;
    g_arena = arena;
    g_intern = intern_tbl;
    g_symtab = symtab;
    g_backtraces = opts && opts->backtraces;
    /* Under --backtraces every non-tail-eliminated FC call must leave a real,
     * in-place frame so the execinfo walk can name it: `noinline` prevents frame
     * merging, and the GCC preamble pragma prevents cold-split mis-attribution.
     * TCO stays on (see g_fn_attr's doc comment); a frame it elides is simply
     * missing from the backtrace. */
    g_fn_attr = g_backtraces ? "static __attribute__((unused, noinline)) "
                             : "static __attribute__((unused)) ";
    symmap_reset();

    /* Collect from_libs and defines from extern module declarations */
    const char **from_libs = NULL;
    int from_lib_count = 0, from_lib_cap = 0;
    CDefine *defines = NULL;
    int define_count = 0, define_cap = 0;
    for (int i = 0; i < prog->decl_count; i++) {
        collect_from_libs(prog->decls[i], &from_libs, &from_lib_count, &from_lib_cap);
        collect_defines(prog->decls[i], &defines, &define_count, &define_cap);
    }

    /* Flatten module decls into a single array */
    Decl **all_decls;
    int all_count;
    collect_all_decls(prog, &all_decls, &all_count);

    /* Detect which feature-gated headers are needed. A narrow --len-repr
     * forces stdio: slice-construction capacity guards (fc_chk_len /
     * fc_len_cap) print a diagnostic before aborting, and they attach to
     * paths spread across the emitter (raw-parts literals, cstr to str, argv,
     * interpolation, runtime-sized alloca); gating each one separately would
     * be easy to get wrong for no practical gain on the targets that use it. */
    g_needs_stdio = g_len_repr < 64;
    g_needs_math = false;
    g_needs_float = false;
    g_needs_errno = false;
    g_uses_error_name = false;
    g_errname_emitted = false;
    for (int i = 0; i < all_count; i++)
        detect_features_decl(all_decls[i]);
    /* Also scan monomorphized template bodies */
    for (int mi = 0; mi < mono->count; mi++) {
        if (mono->entries[mi].decl_kind == DECL_LET && mono->entries[mi].template_decl)
            detect_features_decl(mono->entries[mi].template_decl);
    }

    emit_preamble(out, defines, define_count, from_libs, from_lib_count);
    free(from_libs);
    free(defines);
    emit_runtime_support(out);
    emit_types(out, all_decls, all_count, mono);
    collect_file_globals(all_decls, all_count);
    emit_prototypes(out, all_decls, all_count, mono);
    EmittedCode code = {0};
    collect_code(&code, all_decls, all_count, mono);
    emit_globals(out, all_decls, all_count);
    emit_functions(out, all_decls, all_count, mono, &code);
    if (g_backtraces) emit_backtrace_table(out);

    symmap_reset();
    free(g_file_globals);
    g_file_globals = NULL;
    g_file_global_count = 0;
    free(all_decls);
}
