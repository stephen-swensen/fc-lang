#pragma once
#include "ast.h"

/* Facts about expressions that pass2 judges by and codegen emits by. Each is
 * computed here, in one place, so both passes get the same answer: pass2 does
 * not reject what codegen would emit, and codegen does not elide a guard pass2
 * relied on. */

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

/* Largest field width or precision a format spec may carry. Both are passed to
 * the C library as `int`, and C11 guarantees only that `int` reaches 32767 (the
 * same 16-bit floor the emitted arithmetic assumes), so a larger value cannot
 * be represented on every target. pass2 rejects specs over the limit rather
 * than letting the digits wrap into an arbitrary field. The cap also bounds the
 * hoisted buffer a single segment can demand. */
#define INTERP_MAX_FIELD 32767

/* The modifiers a format spec carries, as written. Codegen copies the spec
 * verbatim into the emitted C format string, so this is also what the C
 * formatter sees, and pass2 judges the spec from this reading. */
typedef struct InterpSpec {
    bool minus, plus, space, hash, zero;  /* flags present */
    char repeated;                        /* a flag written twice (that flag), else 0 */
    int64_t width;                        /* explicit field width, 0 when absent */
    int64_t precision;                    /* explicit precision, -1 when absent */
} InterpSpec;

/* Read a format segment's modifiers. Width and precision saturate one past
 * INTERP_MAX_FIELD so an over-long digit run is reported as too large instead of
 * overflowing the accumulator. */
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

/* If e is a resolved reference to a declared error constant (a member of an
 * `error` group, reached as `group.member`/`mod.group.member` or through an
 * import as a bare name), return the member's assigned EXPR_INT_LIT; else
 * NULL. */
const Expr *error_const_literal(const Expr *e);

/* The helpers the interpolation facts are built from, which codegen's buffer
 * emitter also uses. */
void parse_format_width_prec(const char *text, int64_t *width, int64_t *precision);
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
