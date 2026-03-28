# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Core Language

### Capturing lambda context lifetime (bug)

Returning a capturing lambda creates a dangling pointer — the compound literal context (`&(_ctx_fn){ .captured_x = x }`) has block scope in C11, so its storage is reclaimed when the function returns.

GCC catches this at `-Werror -Wdangling-pointer` (C compilation fails), so it cannot produce a silently wrong binary. But FC should catch this at the FC level: extend escape analysis to track closure provenance and reject returning capturing lambdas.

Non-capturing lambdas are not affected — their `.ctx` is `NULL`.

Priority: medium (safety net exists at C level, but violates FC's "catch errors early" philosophy).

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

## Usability & Diagnostics

### type_name() for generic function types

`%T` on generic functions doesn't show explicit type parameters — shows `((int32, int32) -> int32) -> 'b` instead of `<'b, 'c>((int32, int32) -> int32) -> 'b`. Would require carrying type param info on `TYPE_FUNC`. Low priority — `%T` is most useful on concrete values.
