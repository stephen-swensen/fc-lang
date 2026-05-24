# Compile-time evaluation of constant expressions

*Design plan for the change that taught module-level const initializers to
evaluate fixed-width integer/bool subexpressions to literals (commit `3649e05`).
Line numbers below reference the pre-change source.*

## Context

Module-level `let` bindings whose initializer is a "constant expression" are emitted as
C file-scope variables **with a const-expr initializer** (`codegen.c:5144`). That requires
the folded initializer to be a valid C compile-time constant. Integer `/` and `%` are
emitted as a runtime zero-check **statement expression** (`codegen.c:1651`), which is *not*
a valid C file-scope initializer — so `is_const_expr` rejects integer div/mod
(`pass2.c:5489`). The result:

```fc
module m =
    let x = 20
    let y = 40 / x      // ERROR: "top-level initializer for 'y' must be a constant expression"
```

even though the divisor is obviously nonzero. (Note: file-level / non-module globals are a
separate, more permissive tier — they are declared uninitialized and initialized at runtime
inside the generated C `main`, `codegen.c:5153`, so division already works there. Only
module members are affected.)

**Fix:** teach the const-fold pass to *evaluate* fixed-width integer/bool/char subexpressions
to a single literal AST node at compile time, mirroring FC's runtime arithmetic semantics
exactly. `40 / x` folds `x`→`20`, then evaluates `40 / 20`→ literal `2`, which is a valid C
initializer. Div-by-zero becomes a **compile-time error** (strictly safer than today's
runtime abort). `isize`/`usize` and float are deliberately *not* host-evaluated (see below).

Decisions settled with the user:
- Evaluate everything we can (fixed-width int/bool/char) — single source of truth.
- `isize`/`usize`: leave to the target C compiler, never host-evaluate (cross-compilation safety; consistent with the existing shift-mask precedent at `codegen.c:1626` and `shift_mask_for` returning `-1` for isize/usize at `codegen.c:1144`).
- Div-by-zero: emit a specific diagnostic, "division by zero in constant expression".

## Why fixed-width only (cross-compilation)

`size_t`/`ptrdiff_t` width is target-defined; a cross-compiler folds size_t constants at the
*target's* width. Baking a host-width literal would be wrong for a future 16/32-bit target.
Fixed-width types (`int8..uint64`, with 2's-complement wrap) are identical on every target,
so evaluating them in `fcc` is target-independent. Float is also fixed-width but is *not*
evaluated: it already compiles fine as a C constant (no runtime check ever), and host vs.
target float rounding is a needless risk for zero functional gain — so float subexpressions
are left as folded C, same deferral philosophy as isize/usize.

**Evaluability rule (recursive):** a subtree evaluates to a literal only if its result type
*and every operand type within it* are fixed-width integer/bool/char. Any `isize`/`usize`- or
float-typed node short-circuits evaluation of itself and its ancestors; the unevaluated tree
is left for the existing const-expr gate + codegen to handle as before.

## Implementation

All changes are in **`src/pass2.c`**, additive to the existing const-fold machinery
(`const_fold_expr` at `pass2.c:5257`, gate in `check_module_members` at `pass2.c:5687`).
**No changes to `is_const_expr` are required** — evaluated subtrees become
literal nodes it already accepts; any div/mod node that *remains* (isize/usize, or a
non-literal operand like an extern const) is still correctly rejected by the unchanged
`is_const_expr` div/mod gate (`pass2.c:5489`).

### 1. New evaluator: `try_eval_const(CheckCtx*, Expr*) -> Expr*`

Takes a node whose children have *already* been const-folded. Returns a freshly-allocated
literal `Expr` (`EXPR_INT_LIT` / `EXPR_BOOL_LIT`) if the node is evaluable,
else `NULL` (caller keeps the folded tree). Operates on `e->type` (populated — fold runs
after the init is type-checked).

Handles, only when result + operand types are fixed-width int/bool/char:
- **Binary** `+ - * / % << >> & | ^` and comparisons `< > <= >= == !=` (→ bool), logical `&& ||` (→ bool).
- **Unary prefix** `-` (negate), `~` (bitwise not), `!` (bool not).
- **Cast** where *both* operand and target are integer (truncation / sign-extension). int→float / float→int casts are left unevaluated (valid C constants; avoids float-range UB).

Does **not** evaluate (returns NULL → left as-is): any `isize`/`usize` or float node, `sizeof`/`alignof`/`default` (target-decided), extern consts, aggregates (struct/array/slice/some/variant — their *elements* still get individually evaluated by the recursive fold).

### 2. Evaluation semantics (must mirror codegen exactly)

Compute in `uint64_t` (two's-complement, mod 2^64), then **normalize** to the result type:
mask to width (8/16/32/64), and for signed types sign-extend back to 64 bits. Store that
64-bit value in `int_lit.value` (so existing literal codegen emits a clean
signed/unsigned decimal — sign-extended negatives emit as e.g. `-3`, not `253`).

- **`+ - * << & | ^`**: compute in uint64, normalize. (Signedness only affects final representation.) Matches the cast-through-unsigned wrap in `codegen.c:1605`.
- **`>>`**: signed result → arithmetic shift (sign-extended operand); unsigned → logical.
- **Shift amount**: mask to width−1 (7/15/31/63), matching `shift_mask_for` (`codegen.c:1140`).
- **`/ %`** (signedness-dependent):
  - If divisor evaluates to **0** → `diag_error(node->loc, "division by zero in constant expression")` and return NULL. The module gate's `new_errs == 0` guard (`pass2.c:5701`) suppresses the redundant generic message.
  - Guard `INT_MIN / -1` (and `% -1`) to avoid UB in `fcc` itself: for signed `/` with divisor −1, result = negate(dividend) computed in uint64 (wraps to dividend for MIN); for `%` with divisor −1, result = 0. Otherwise do signed (sign-extended) or unsigned division per type.
- **Comparisons**: signed vs unsigned per operand type → `EXPR_BOOL_LIT`.
- **`&& ||`**: evaluate both bool operands (pure), combine → `EXPR_BOOL_LIT`.
- **Casts (int→int)**: read operand per its signedness, normalize to target width/signedness.

Reading an operand literal: interpret `int_lit.value` per the operand's own type signedness
(sign-extend from its width if signed, zero-extend if unsigned). Lexer literals are always
non-negative magnitudes (unary `-` is a separate node), so this read is uniform for both
lexer-produced and evaluator-produced literals.

### 3. Hook into `const_fold_expr`

In the `EXPR_BINARY`, `EXPR_UNARY_PREFIX`, and `EXPR_CAST` cases, after building the
children-folded node `n`, attempt `Expr *v = try_eval_const(ctx, n); if (v) return v;` before
returning `n`. Identifier folding already substitutes module-let values, so `40 / x` arrives
as `40 / 20`.

### 4. Edge to verify: INT64_MIN emission

The evaluator can newly produce `INT64_MIN`/large 64-bit values (e.g. `1i64 << 63`) that the
lexer never emits directly. Confirm the `INT64_C(...)` path (`codegen.c`, `case EXPR_INT_LIT`)
compiles clean under `-Werror`; if `INT64_C(-9223372036854775808)` warns (literal overflow
before negation), emit via the `(-9223372036854775807LL - 1)` idiom. Set
`int_lit.out_of_range = false` on produced literals (value is in range by construction after
normalization).

## Spec & examples updates

(FC convention: update the spec when a semantic rule is new — see
`spec/hist/archived-todos.md` for the const-expr identifier-folding precedent.)

- **`spec/fc-spec.html`**, "Statements vs. expressions" section: prose stating that constant expressions are **evaluated at compile time** for fixed-width integer/bool types; integer `/`, `%`, shifts, bitwise, comparisons, and int→int casts are all permitted in module-level const-exprs and are folded to literals. **Division/modulo by zero in a constant expression is a compile-time error.** `isize`/`usize` and float subexpressions are folded by the C compiler, not `fcc` (so `isize`/`usize` div/mod remain disallowed in module-member initializers). Note the two-tier model: module members are compile-time C constants; file-level globals are runtime-initialized and therefore more permissive.
- **`spec/examples.fc`**: extend the module-level const-expr example to show integer division/modulo/shift in a module-level `let`.

## Tests

Existing error tests that **flip to passing** (limitation lifted):
- `tests/cases/memory/module_div_const_err` → `module_div_const` (asserts the divided value).
- `tests/cases/memory/module_mod_const_err` → `module_mod_const`.

New tests under `tests/cases/const_eval/` covering happy path, edges, errors, interactions:
- **Div/mod**: via module-let divisor; chained; `INT_MIN / -1` wrap; `% -1` → 0; unsigned div.
- **Div-by-zero (error)**: `40 / 0` literal, and via a divisor that folds to zero.
- **Shifts**: amount masking, signed (arithmetic) vs unsigned (logical) right shift, `1i64 << 63`.
- **Bitwise / comparisons / casts / wrap semantics.**
- **isize/usize stays blocked**: div/mod → `.error`; `+`/`-`/`*` still compile.
- **Interactions**: evaluated value inside struct field, array element, `some()`, union payload; referenced by a later module-let.

## Verification

1. `make dev` (clean `-O0` build).
2. `make test-gcc FILTER=memory` / `FILTER=const_eval` for fast iteration.
3. Inspect generated C for a div case to confirm it emits a literal, not a statement expression.
4. `make test-all` (gcc + clang); `make test-all-O2` for optimizer-surfaced UB.

## Outcome

Implemented as planned, with these notes:

- **INT64_MIN edge materialized.** `INT64_C(-9223372036854775808)` does trip `-Werror`
  (`integer constant is so large that it is unsigned`); codegen now emits the
  `(-9223372036854775807LL - 1)` idiom for `INT64_MIN`. `UINT64_MAX` via `UINT64_C(...)`
  was already fine.
- **A third test flipped.** `tests/cases/memory/module_slice_lit_div_err` (integer division
  inside an array-literal element) also became valid once elements fold to literals; it was
  converted to a passing `module_slice_lit_div`.
- **Array *type* sizes left out of scope.** `int32[N]` size expressions are checked at
  type-check time (`pass2.c`, `EXPR_ARRAY_LIT`) and require a literal `EXPR_INT_LIT` there —
  a separate, stricter path that runs *before* initializer const-fold, so even `int32[2+1]`
  fails today. Lifting it is a distinct feature and was not touched.
- **No `is_const_expr` or general codegen changes needed** beyond the INT64_MIN literal fix —
  evaluated subtrees become literals the existing gate and emitter already handle.
- **Result:** the full suite (1277 tests) passes under gcc and clang at both `-O0` and `-O2`.
  Committed as `3649e05`.
