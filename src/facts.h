#pragma once
#include "ast.h"

/* Facts about expressions that pass2 judges by and codegen emits by. Each is
 * computed here, in one place, so both passes get the same answer: pass2 does
 * not reject what codegen would emit, and codegen does not elide a guard pass2
 * relied on. */

/* The operations the two markers govern. `unguarded` switches off the
 * value-precondition guards: float-to-int saturation, the integer
 * divide/modulo zero check, and slice indexing and subslice bounds.
 * `checked` switches on the data-loss traps: integer `+ - *`, signed `/` (at
 * MIN / -1), signed negation, a lossy integer narrowing cast, and the two
 * truncating string forms (a `(cstr[N])` cast, a `%.Ns` interpolation
 * segment). pass2 rejects a marker whose body has none of the operations it
 * governs as redundant; codegen gates exactly these on the marker, so the two
 * cannot disagree.
 *
 * `resolve` gives a node's type in the current context: NULL in pass2, where
 * a generic body's type variable counts as governed when some instantiation
 * would be (`a / b` has a zero guard at i32 and none at f64, so a marker there
 * is accepted, and is a no-op in the instances where nothing is governed);
 * codegen's substitution in an instance, where the type is concrete. */
bool facts_guard_governs(const Expr *e, Type *(*resolve)(Type *));
bool facts_overflow_governs(const Expr *e, Type *(*resolve)(Type *));

/* True if an interpolated string's buffer size is not a compile-time constant:
 * it has a %s segment (str or cstr) with no explicit precision, so its byte
 * budget depends on a runtime string length. Such an interpolation needs an
 * explicit home (a precision, alloc, or alloca); pass2 rejects a bare one.
 * Defined as the negation of interp_const_buffer_size, which codegen uses to
 * size the buffer. */
bool interp_is_runtime_sized(const Expr *e);

/* Explicit truncating precision of a `%s` format segment (>= 0), or -1 when the
 * segment is literal, not %s, or has no precision. A precision caps the
 * segment's bytes (printf semantics), so these segments are governed by the
 * overflow axis (`checked` aborts instead of clipping). */
int interp_seg_trunc_prec(const InterpSegment *seg);

/* A format segment's spec (interp_spec_scan); all zero, with precision -1,
 * for a literal segment. */
void interp_seg_spec(const InterpSegment *seg, InterpSpec *out);

/* Pointer-value null-status predicates for null-sentinel options (T*?, any*?,
 * cstr?), where none is represented by a null pointer. provably_nonnull is true
 * only when a value can never be null (codegen elides the some() null-guard);
 * provably_null is true only when it is always null (pass2 rejects some(p) of
 * it). Both are false for anything uncertain, which gets a runtime guard. The
 * integer pair does the same for err(T, code), whose code may not be zero. */
bool ptr_value_provably_nonnull(const Expr *e);
bool ptr_value_provably_null(const Expr *e);
bool int_value_provably_nonzero(const Expr *e);
bool int_value_provably_zero(const Expr *e);

/* Whether function value `e` is code known at compile time, with no context,
 * which C can call as a raw function pointer through a trampoline: a function
 * named at top level or in a module, a non-capturing lambda literal, or a
 * local `let` bound to one (Expr.ident.fn_literal, which pass2 sets). pass2
 * accepts only such a value at `&f` and at an extern call's function
 * parameter; codegen emits a trampoline for exactly these. */
bool fn_value_is_context_free(const Expr *e);

/* If e is a resolved reference to a declared error constant (a member of an
 * `error` group, reached as `group.member`/`mod.group.member` or through an
 * import as a bare name), return the member's assigned EXPR_INT_LIT; else
 * NULL. */
const Expr *error_const_literal(const Expr *e);

/* The helpers the interpolation facts are built from, which codegen's buffer
 * emitter also uses. */
bool interp_conv_is_unsigned(char conv, Type *t);
int interp_literal_len(InterpSegment *seg);

/* One segment's share of an interpolated string's byte budget: `bytes` when
 * constant; when `runtime` (a %s of str or cstr with no precision), the
 * string's length, at least `bytes` (its field width). */
typedef struct {
    bool runtime;
    int64_t bytes;
} InterpSegBudget;

InterpSegBudget interp_seg_budget(InterpSegment *seg, Type *t);
bool interp_const_buffer_size(Expr *e, Type *(*resolve)(Type *), int64_t *out_size);
