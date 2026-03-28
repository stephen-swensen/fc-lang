# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Core Language

### Capturing lambda context lifetime (bug)

Returning a capturing lambda creates a dangling pointer — the compound literal context (`&(_ctx_fn){ .captured_x = x }`) has block scope in C11, so its storage is reclaimed when the function returns.

GCC catches this at `-Werror -Wdangling-pointer` (C compilation fails), so it cannot produce a silently wrong binary. But FC should catch this at the FC level: extend escape analysis to track closure provenance and reject returning capturing lambdas.

Non-capturing lambdas are not affected — their `.ctx` is `NULL`.

Priority: medium (safety net exists at C level, but violates FC's "catch errors early" philosophy).

### Stack array literal size enforcement

Per spec, `N` in `T[N] { }` must be a compile-time integer literal. The compiler currently accepts runtime expressions (e.g., `int32[n] { }`) without error — the failure only surfaces at C compilation. Pass2 should validate that the size expression is a constant (literal or constant-foldable) and emit a clear FC error.

Priority: low (fails at C level, not silent).

### Remove str32

`str32` (alias for `uint32[]`) parses and type-checks but has no runtime support — no str32 literals, no interpolation, no str↔str32 conversion. Remove from spec, grammar, and compiler. Can re-add when Unicode support is properly designed.

## Type System

### Generic mixed type-var arithmetic (soundness)

Binary operations on different type variables (`'a + 'b`, `'a > 'b`) are allowed at template time, but the result type is unsound when widening is involved. The type-var early return in pass2 picks one operand's type arbitrarily. When instantiated with types that widen (e.g., `'a = int32`, `'b = int64`), the inferred return type doesn't match the computed type.

Fix: restrict mixed type-var binary operations — require both operands to be the same type variable (`'a + 'a` ok, `'a + 'b` error at template time, concrete + `'a` ok since it pins `'a`). Conservative-but-complete.

### Branch widening in if/match (deferred)

if/match branches require exact type equality. Adding implicit widening would allow compatible types to unify — e.g., `const str` + `str` → `const str`, or `int8` + `int32` → `int32`. Also affects loop return type unification via `break value`.

Deferred — significant work, numeric widening / const widening / loop types all interact. Users manually cast to unify: `(const str)non_const_expr`.

## Standard Library

### std::math module

sin, cos, sqrt, pow, abs, floor, ceil, round, etc. wrapping `math.h`. Tier 1 (C11 standard).

### std::text module

String manipulation utilities (split, join, trim, contains, starts_with, etc.). Design TBD.

### prelude.fc / types.fc status

`stdlib/prelude.fc` provides `print`, `println`, `freeze` — undocumented convenience functions. `stdlib/types.fc` provides generic `tuple1` through `tuple5` — undocumented. Both are experimental and only used in `tests/scratch.fc`. Need a decision before v1.0: formalize in spec or remove.

## Spec Sync

### Stdlib signatures need const qualifiers

The spec's function signatures are stale — missing `const` qualifiers the implementation correctly uses:

| Spec says | Should be |
|-----------|-----------|
| `io.write(s: str, f: any*)` | `io.write(s: const str, f: any*)` |
| `io.open(path: str, mode: str)` | `io.open(path: const str, mode: const str)` |
| `sys.env(name: str) -> str?` | `sys.env(name: const str) -> const str?` |

## Usability & Diagnostics

### type_name() for generic function types

`%T` on generic functions doesn't show explicit type parameters — shows `((int32, int32) -> int32) -> 'b` instead of `<'b, 'c>((int32, int32) -> int32) -> 'b`. Would require carrying type param info on `TYPE_FUNC`. Low priority — `%T` is most useful on concrete values.

## Test Coverage

### Missing %p interpolation test

No test exercises the `%p` pointer format specifier in string interpolation. The implementation handles it correctly, but test coverage is missing.

## Deferred

Items explicitly out of scope for now. Full analysis in `spec/hist/archived-todos.md`.

- **Closures at extern boundaries** — wrapper function pattern for passing closures to C callback APIs. Current non-capturing restriction is conservative-but-complete.
- **Unreachable pattern detection** — warning-only feature. Maranget infrastructure supports it as the dual of exhaustiveness. Small addition when there's demand.
- **Field access on type variables** — structural generics (`(p: 'a) -> p.x + p.y`). High complexity, trivial workaround exists (pass fields as separate parameters).
- **Inline assembly, volatile, bitfield structs** — outside FC's platform contract. Users write C wrapper files.
- **Eager type resolution** — not viable. Blocked by self-referential struct cycles. On-demand `resolve_type()` is the correct approach.
