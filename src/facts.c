#include "facts.h"
#include "pass1.h"
#include <string.h>

/* ---- Values ---- */

/* A null-sentinel option (T*?, any*?, cstr?) represents none as a null pointer,
 * so some(p) over a null p is indistinguishable from none. pass2 rejects a
 * provably-null payload and, in a constant initializer, any payload not
 * provably non-null; codegen elides the runtime null-guard for a provably
 * non-null one.
 *
 * Both predicates lean toward emitting the guard: anything uncertain (params,
 * field reads, call results, extern returns) is false from both. */
bool ptr_value_provably_nonnull(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_UNARY_PREFIX:
        /* &x / &fn: the address of a binding, element, or function is never
         * null. */
        return e->unary_prefix.op == TOK_AMP;
    case EXPR_UNARY_POSTFIX:
        /* p!: unwrap yields the some-payload, which for a pointer option is
         * non-null (some(null) is itself rejected or guarded), and alloc(...)!
         * is malloc-checked. Only reached here when the result is
         * pointer-typed. p? makes the same null test before yielding, so it
         * counts too. */
        return e->unary_postfix.op == TOK_BANG ||
               e->unary_postfix.op == TOK_QUESTION;
    case EXPR_CSTRING_LIT:
        /* c"..." points into static storage. */
        return true;
    case EXPR_CAST:
        /* (T*) <nonzero usize literal>, e.g. a fixed MMIO address. (A bare
         * fixed-width int literal never reaches here: pass2 rejects int<->ptr
         * casts that don't go through usize/isize.) */
        if (e->cast.operand && e->cast.operand->kind == EXPR_INT_LIT)
            return e->cast.operand->int_lit.value != 0;
        /* A widening / const-add cast preserves the operand's null-status. */
        return ptr_value_provably_nonnull(e->cast.operand);
    default:
        return false;
    }
}

bool ptr_value_provably_null(const Expr *e) {
    if (!e) return false;
    switch (e->kind) {
    case EXPR_DEFAULT:
        /* default(T*) / default(any*) zero-init to NULL. (default(T*?) is an
         * EXPR_DEFAULT with an option target: that is none, not a some, and
         * never reaches some().) */
        return e->default_expr.target &&
               (e->default_expr.target->kind == TYPE_POINTER ||
                e->default_expr.target->kind == TYPE_ANY_PTR);
    case EXPR_CAST:
        /* (T*) 0usize: a pointer-width integer-literal zero cast to pointer.
         * (Bare (T*) 0 is rejected in pass2; only the usize form reaches here.) */
        if (e->cast.operand && e->cast.operand->kind == EXPR_INT_LIT)
            return e->cast.operand->int_lit.value == 0;
        return ptr_value_provably_null(e->cast.operand);
    default:
        return false;
    }
}

const Expr *error_const_literal(const Expr *e) {
    if (!e) return NULL;
    const Symbol *s = NULL;
    if (e->kind == EXPR_FIELD) s = e->field.resolved_member;
    else if (e->kind == EXPR_IDENT) s = e->ident.resolved_sym;
    if (!s || s->kind != DECL_LET || !s->decl || !s->parent || !s->parent->decl)
        return NULL;
    const Decl *pd = s->parent->decl;
    if (pd->kind != DECL_MODULE || !pd->module.is_error_group) return NULL;
    return s->decl->let.init;  /* the assigned EXPR_INT_LIT (patched by pass1) */
}

/* The integer pair, for the err(T, code) zero-code guard: a code of 0 is the
 * ok tag, so it cannot be an error. Same conservatism as the pointer pair:
 * anything uncertain is false from both and gets a runtime guard. Negation
 * preserves zero-ness (including INT_MIN), so unary minus recurses. */
bool int_value_provably_nonzero(const Expr *e) {
    if (!e) return false;
    /* A declared error constant is non-zero: pass1 numbers them from
     * FC_ERROR_CODE_BASE (65536). */
    {
        const Expr *lit = error_const_literal(e);
        if (lit) return lit->int_lit.value != 0;
    }
    switch (e->kind) {
    case EXPR_INT_LIT: return e->int_lit.value != 0;
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_MINUS)
            return int_value_provably_nonzero(e->unary_prefix.operand);
        return false;
    case EXPR_CAST: return int_value_provably_nonzero(e->cast.operand);
    default: return false;
    }
}

bool int_value_provably_zero(const Expr *e) {
    if (!e) return false;
    {
        const Expr *lit = error_const_literal(e);
        if (lit) return lit->int_lit.value == 0;  /* never true once codes are assigned */
    }
    switch (e->kind) {
    case EXPR_INT_LIT: return e->int_lit.value == 0;
    case EXPR_DEFAULT:
        /* default(<integer type>) is 0 */
        return e->default_expr.target && type_is_integer(e->default_expr.target);
    case EXPR_UNARY_PREFIX:
        if (e->unary_prefix.op == TOK_MINUS)
            return int_value_provably_zero(e->unary_prefix.operand);
        return false;
    case EXPR_CAST: return int_value_provably_zero(e->cast.operand);
    default: return false;
    }
}

/* ---- Interpolation formats ---- */

/* Parse width and precision from an InterpSegment format spec string.
 * Sets *width to the explicit width (0 if absent).
 * Sets *precision to the explicit precision (-1 if absent, 0 for ".0").
 *
 * Both accumulate in int64 and saturate one past INTERP_MAX_FIELD, so a digit
 * run long enough to overflow the accumulator (undefined behavior) instead
 * compares as too large. pass2 rejects over-limit specs (via interp_seg_spec),
 * so codegen sees only values in range. */
void parse_format_width_prec(const char *text,
                                     int64_t *width, int64_t *precision) {
    const char *fs = text;
    const int64_t sat = (int64_t)INTERP_MAX_FIELD + 1;
    *width = 0;
    *precision = -1;
    /* Skip flags */
    while (*fs == '-' || *fs == '+' || *fs == '0' ||
           *fs == '#' || *fs == ' ') fs++;
    /* Width */
    while (*fs >= '0' && *fs <= '9') {
        if (*width < sat) *width = *width * 10 + (*fs - '0');
        if (*width > sat) *width = sat;
        fs++;
    }
    /* Precision */
    if (*fs == '.') {
        fs++;
        *precision = 0;
        while (*fs >= '0' && *fs <= '9') {
            if (*precision < sat) *precision = *precision * 10 + (*fs - '0');
            if (*precision > sat) *precision = sat;
            fs++;
        }
    }
}

/* A leading `0` is the zero flag, not a width digit: C puts the flag prefix
 * ahead of the width, so `%08d` is flag `0` + width 8 and `%00d` is the flag
 * written twice. The flag loop below and parse_format_width_prec split at the
 * same point. */
void interp_seg_spec(const InterpSegment *seg, InterpSpec *out) {
    memset(out, 0, sizeof *out);
    out->precision = -1;
    if (seg->is_literal) return;
    const char *fs = seg->text;
    for (;; fs++) {
        bool *slot;
        switch (*fs) {
        case '-': slot = &out->minus; break;
        case '+': slot = &out->plus;  break;
        case ' ': slot = &out->space; break;
        case '#': slot = &out->hash;  break;
        case '0': slot = &out->zero;  break;
        default:  slot = NULL;        break;
        }
        if (!slot) break;
        if (*slot && !out->repeated) out->repeated = *fs;
        *slot = true;
    }
    parse_format_width_prec(seg->text, &out->width, &out->precision);
}

/* pass2's checked-overflow test and codegen's truncation check both ask here. */
int interp_seg_trunc_prec(const InterpSegment *seg) {
    if (seg->is_literal || seg->conversion != 's') return -1;
    int64_t width = 0, precision = -1;
    parse_format_width_prec(seg->text, &width, &precision);
    return (int)precision;
}

/* True when an integer conversion renders its operand as unsigned. `%u %x %X %o`
 * always do: they print the operand's bit pattern, negatives included.
 * `%d`/`%i` print the operand's value, so they render unsigned when the
 * operand's own type is unsigned. The spec's format table gives `%d` "all
 * integer types", and a u64 passed through `long long` would print `u64.max`
 * as -1 (by an implementation-defined conversion). The format-string emitter
 * and the argument emitter both ask here, so the conversion character and the
 * argument's cast agree. */
bool interp_conv_is_unsigned(char conv, Type *t) {
    switch (conv) {
    case 'u': case 'x': case 'X': case 'o': return true;
    case 'd': case 'i': return t && type_is_unsigned(t);
    default: return false;
    }
}

/* Bytes a literal segment contributes to the formatted output: each `%%` folds
 * to one `%`, and a backslash escape counts as the single byte it denotes. */
int interp_literal_len(InterpSegment *seg) {
    return decode_str_lit(seg->text, seg->text_length, NULL);
}

/* Upper bound on the bytes a non-string conversion can emit, for buffer sizing.
 * A field width is a minimum, not a maximum, so it can only widen the bound;
 * flags may add a sign, a space, or a `#` prefix. */
static int64_t interp_numeric_bound(char conv, Type *t, const char *flags_text,
                                    int64_t explicit_width, int64_t explicit_prec) {
    int64_t bound;
    switch (conv) {
    case 'd': case 'i': case 'u':
        switch (t ? t->kind : 0) {
        case TYPE_INT8: bound = 4; break;
        case TYPE_UINT8: bound = 3; break;
        case TYPE_INT16: bound = 6; break;
        case TYPE_UINT16: bound = 5; break;
        case TYPE_INT32: bound = 11; break;
        case TYPE_UINT32: bound = 10; break;
        case TYPE_INT64: bound = 20; break;
        case TYPE_UINT64: bound = 20; break;
        default: bound = 20; break;
        }
        break;
    case 'x': case 'X':
        switch (t ? t->kind : 0) {
        case TYPE_INT8: case TYPE_UINT8: bound = 2; break;
        case TYPE_INT16: case TYPE_UINT16: bound = 4; break;
        case TYPE_INT32: case TYPE_UINT32: bound = 8; break;
        case TYPE_INT64: case TYPE_UINT64: bound = 16; break;
        default: bound = 16; break;
        }
        break;
    case 'o':
        switch (t ? t->kind : 0) {
        case TYPE_INT8: case TYPE_UINT8: bound = 3; break;
        case TYPE_INT16: case TYPE_UINT16: bound = 6; break;
        case TYPE_INT32: case TYPE_UINT32: bound = 11; break;
        case TYPE_INT64: case TYPE_UINT64: bound = 22; break;
        default: bound = 22; break;
        }
        break;
    case 'f': {
        /* A %f field width is a minimum, not a maximum: the integer part can
         * have as many digits as the type's largest value (DBL_MAX is about
         * 1.8e308, so 309 digits; FLT_MAX is about 3.4e38, so 39). The budget
         * covers sign + integer digits + '.' + fraction (default precision 6). */
        int int_digits = (t && t->kind == TYPE_FLOAT32) ? 39 : 309;
        int64_t prec = explicit_prec >= 0 ? explicit_prec : 6;
        bound = 1 + int_digits + 1 + prec;
        break;
    }
    case 'e': case 'E': case 'g': case 'G': {
        /* Scientific/shortest forms are bounded by precision, not the exponent:
         * sign + leading digit + '.' + prec mantissa digits + 'e+ddd' (or
         * 'e-ddd'). */
        int64_t prec = explicit_prec >= 0 ? explicit_prec : 6;
        bound = 9 + prec;
        break;
    }
    case 'c': bound = 1; break;
    case 'p': bound = 18; break;
    default: bound = 24; break;
    }
    /* On an integer conversion a precision is a minimum digit count, so it can
     * push the output past what the type's magnitude alone needs: `%.20d{5}`
     * writes twenty digits from an i32 whose own bound is 11. (The float cases
     * above already include their precision; pass2 rejects a precision on
     * %c/%p.) Without this the buffer is too small and snprintf clips the
     * result.
     *
     * The per-type bounds above are whole-field widths that include a minus
     * sign where one can appear, so a precision that replaces them must budget
     * that byte itself: `%.20d{-5}` writes 21. Only a signed rendering needs
     * it; `%.20x` of a negative prints the bit pattern with no sign. */
    switch (conv) {
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
        if (explicit_prec > 0) {
            int64_t need = explicit_prec + (interp_conv_is_unsigned(conv, t) ? 0 : 1);
            if (need > bound) bound = need;
        }
        break;
    default: break;
    }
    const char *flags = flags_text;
    while (*flags == '-' || *flags == '+' || *flags == '0' || *flags == '#' || *flags == ' ') {
        if (*flags == '+' || *flags == ' ') bound++;
        if (*flags == '#') bound += 2;
        flags++;
    }
    if (explicit_width > bound) bound = explicit_width;
    return bound;
}

/* One segment's share of an interpolation's byte budget. `t` is the segment
 * operand's type as the caller sees it: codegen passes the instance's, so a %T
 * of a type variable budgets the bound type's name. */
InterpSegBudget interp_seg_budget(InterpSegment *seg, Type *t) {
    if (seg->is_literal)
        return (InterpSegBudget){ false, interp_literal_len(seg) };
    if (seg->conversion == 'T')
        return (InterpSegBudget){ false, (int64_t)strlen(type_name(t)) };
    int64_t width = 0, prec = -1;
    parse_format_width_prec(seg->text, &width, &prec);
    if (seg->conversion == 's' && t && (is_str_type(t) || is_cstr_type(t))) {
        /* Precision is a hard maximum; width is a minimum field. */
        if (prec >= 0) return (InterpSegBudget){ false, prec > width ? prec : width };
        return (InterpSegBudget){ true, width };
    }
    return (InterpSegBudget){ false, interp_numeric_bound(seg->conversion, t, seg->text,
                                                         width, prec) };
}

/* If the interpolation's byte budget is a compile-time constant (no segment
 * contributes a runtime string length), store it in *out_size and return true.
 * It is the sum the emitted _flen computes, since both take each segment's
 * share from interp_seg_budget. `resolve` maps an operand's type to the one
 * the caller sees (NULL: as written). */
bool interp_const_buffer_size(Expr *e, Type *(*resolve)(Type *), int64_t *out_size) {
    int64_t total = 0;
    for (int i = 0; i < e->interp_string.segment_count; i++) {
        InterpSegment *seg = &e->interp_string.segments[i];
        Type *t = seg->is_literal ? NULL : seg->expr->type;
        if (t && resolve) t = resolve(t);
        InterpSegBudget b = interp_seg_budget(seg, t);
        if (b.runtime) return false;
        total += b.bytes;
    }
    *out_size = total;
    return true;
}

bool interp_is_runtime_sized(const Expr *e) {
    int64_t dummy = 0;
    return !interp_const_buffer_size((Expr *)e, NULL, &dummy);
}
