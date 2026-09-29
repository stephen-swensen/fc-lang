# FC Code Generation: Quality Analysis

A study of the C and `-O2` assembly (gcc and clang) emitted by `fcc`, using a
small but representative program — a recursive, closure-capturing prime
factorization routine that accumulates into a dynamic array. The goal is to
characterize the *general* qualities of FC's code generation, not any one
library.

## The program

The reference program factors an integer into its primes, accumulating the
factors into a generic dynamic array (`array_list`). It is small but exercises a
deliberately abstraction-heavy slice of the language: nested lambdas that
capture an enclosing binding, mutual and self recursion, bounds-checked slice
access, option unwrap, integer division/modulo, and a generic container.

```fc
import array_list from std::data

module prelude =
    let factorize = (num:int64) ->
        let mut factors = array_list.make<int64>()
        let p = &factors                       // pointer captured by the closures

        let find_odd = (candidate:int64, largest_factor:int64) ->
            if candidate > largest_factor / 2 then
                array_list.add(p, largest_factor)
            else if largest_factor % candidate == 0 then
                array_list.add(p, candidate)
                find_odd(candidate, largest_factor / candidate)   // self-recursion
            else
                find_odd(candidate + 2, largest_factor)

        let find = (largest_factor:int64) ->
            if largest_factor == 1 then
                array_list.add(p, 1i64)
            else if largest_factor % 2 == 0 then
                array_list.add(p, 2i64)
                find(largest_factor / 2)                          // self-recursion
            else
                find_odd(3, largest_factor)                       // calls sibling lambda

        find(num)
        factors
```

`find` strips factors of two, then hands off to `find_odd`, which walks odd
candidates. Both append through `p`, a `let`-bound pointer to the `let mut
factors` — the idiom that lets the closures mutate an enclosing binding (a
`let mut` cannot be captured directly, but a pointer value can). The two
references the analysis returns to repeatedly are this **captured-pointer
closure** and the **recursive helpers**.

## Method

The emitted C was inspected directly; the same C was then compiled at `-O2` with
both gcc and clang and the assembly read function by function. To separate
genuine steady-state codegen from constant folding, the driving input was also
fed from a runtime source so the optimizer could not fold the whole computation
away (a constant argument lets the backend evaluate the entire factorization at
compile time — itself a notable result, but not a measure of steady-state code
quality).

## Headline finding

**FC's abstractions are paid for at compile time.** The C that `fcc` emits is
verbose and safety-laden, but it is verbose in precisely the way that optimizes
well. At `-O2` an abstraction-heavy FC program lowers to what one would
hand-write in C — stack-only closures, direct (devirtualized) calls,
recursion turned into iteration, strength-reduced arithmetic, and safety checks
relegated to cold paths.

## Core characteristics

### 1. The emitted C is "optimizer-friendly ugly"

The generated source is not meant to be read. Every slice access is a
bounds-checked GNU statement-expression; every `/` and `%` carries a
divide-by-zero guard; bindings are emitted as `T x = {0}; x = <init>;` with
trailing `(void)x;` markers. None of this survives optimization. The codegen
leans on idioms that gcc and clang see through cleanly:

- `static` on every helper, giving the optimizer whole-program visibility and
  license to inline and clone aggressively.
- `__builtin_expect(...)` on every safety branch, steering the abort paths out
  of the hot path.
- Cast-through-unsigned for signed arithmetic (defined overflow) and masked
  shift amounts — patterns the backends recognize and fold.
- Statement-expressions for checked operations, which inline transparently.

The `= {0}` pre-initialization plus immediate reassignment, the `(void)x`
suppressions, and redundant re-inits leave **no trace** in the output.

### 2. Closures are zero-cost when they don't escape

FC lowers every function value to a uniform fat pointer (code pointer + context
pointer), and capturing lambdas get a context struct allocated on the creator's
stack frame. For non-escaping closures this entire mechanism evaporates at
`-O2`:

- Capture context structs collapse to **a single stack slot** (or nothing) —
  **no heap allocation** for closures. In the reference program the context for
  `find_odd` carries only the captured pointer `p`; at `-O2` it becomes one
  stack slot, passed by address.
- The indirect fat-pointer dispatch is **devirtualized to direct calls** when
  the optimizer can see which function a pointer holds, which is the common case
  for locally-created lambdas. `find`'s call to `find_odd` lowers to a plain
  `call`, not an indirect jump through the fat pointer.
- Helper lambdas inline into their caller wholesale, leaving only the genuinely
  recursive entry points as standalone functions. Here `factorize`, `find`,
  `make`, and the container helpers all inline into the entry point; gcc keeps
  just one standalone helper (`find_odd`) plus `main`, and clang inlines even
  `find_odd` away.

This is the practical payoff of the capture model: the abstraction is free
exactly where the language already guarantees the closure cannot outlive its
frame. The captured-pointer idiom (`let p = &factors`) costs nothing beyond the
pointer it names.

### 3. Recursion becomes iteration

Both tail-recursive and accumulator-style recursive helpers are converted to
loops by the backend. In the reference program `find`'s factor-of-two stripping
and `find_odd`'s odd-candidate walk both become plain loops, and `find_odd`'s
terminal `array_list.add(p, largest_factor)` — the tail of the recursion —
lowers to a `jmp` into the append routine (a true tail call), so deep recursion
does not grow the stack. The language's documented tail-call behavior is
realized by the C backends rather than requiring special codegen — the emitted
C is shaped so that ordinary compiler TCO applies.

### 4. Arithmetic is strength-reduced as expected

The reference program is dense with division and modulo, and the lowering is
exactly what hand-written C would produce. `find`'s `largest_factor % 2 == 0`
lowers to a bit test (`and`/`test`) and `largest_factor / 2` to the standard
signed-halve idiom — no actual division instructions where the divisor is a
known constant. `find_odd`'s `largest_factor % candidate`, where the divisor is
a runtime value, emits a real `idiv`, which is unavoidable. The divide-by-zero
guards FC inserts around every `/` and `%` do not obstruct any of this: they
fold away when the divisor is a nonzero constant (the `/ 2` cases) and otherwise
become a single predicted-not-taken branch.

### 5. Safety checks are real but cold

FC's safety guarantees (bounds checks, option-unwrap tag checks, div-by-zero,
allocation-failure aborts) are preserved, but their cost in the hot path is
close to nil:

- Each check is a single compare + conditional branch, predicted not-taken.
  Every `array_list.add` in the reference program carries a bounds-checked store
  and an option-unwrap on allocation, yet the steady-state append is a handful
  of instructions with the failure paths elsewhere.
- Every `abort()` epilogue is emitted into a `.text.unlikely` / `.cold`
  section, off the hot instruction stream.

Redundant safety work that the *source* expresses repeatedly is cleaned up:
a helper called twice in a loop condition and body is computed once; per-element
bounds checks hoist to a single pre-loop check; the loop body reduces to the
essential work.

### 6. The limits of the cleanup

The one class of residue worth knowing about: **a safety check the optimizer
cannot prove dead because an opaque call sits between the guaranteeing
condition and the check.** When a function establishes an invariant (e.g.
"there is now room"), then calls out to another function, then performs a
checked operation that the invariant makes safe, the backend can't carry the
fact across the call boundary and keeps the check. The append path in the
reference program is the concrete instance: it guards "is the array full?",
calls a grow routine if so, then does the bounds-checked store — and the store's
check is provably dead, but the intervening grow call hides that from the
optimizer, so the compare survives. The cost is a single predicted-not-taken
compare and a few bytes of code — negligible at runtime — but it is the spot
where FC's "always check" policy leaves a mark the optimizer can't erase.
Structuring hot internal operations to avoid an intervening opaque call (or
providing an unchecked internal primitive after the guard) is the lever if such
a path ever becomes measurable.

Separately, costs that originate in **library or program source** rather than
codegen survive as written — the transpiler faithfully lowers an inefficient
algorithm to inefficient C. These are not codegen weaknesses; they are fixed at
the FC source level.

## Verdict

FC's code generation is high quality. The strategy — emit obvious, safe,
heavily-annotated C and let the C backend optimize it — works: the closure,
generic, and slice machinery imposes no runtime overhead in the cases the
language is designed to make safe, and the safety checks that remain are
arranged so that the hot path pays almost nothing for them. The transpiler does
not try to out-optimize gcc/clang; it emits C shaped so they don't have to be
told twice. The residual imperfections are small, well-understood, and largely
attributable to safety policy composed with call-boundary opacity, or to source
the transpiler is faithfully translating — not to the code generator itself.
