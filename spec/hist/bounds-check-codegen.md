# Bounds-Check Codegen: Improvements

> **Disposition (2026-04):** Proposal 1 **shipped**. Proposal 2 **deferred** — see _Proposal 2: deferral rationale_ at the bottom for empirical measurements and the decision. This doc is retained as a historical reference.

Two proposals for improving the C code FC emits for slice bounds checks. Motivated by an audit of generated C from the wolf-fc project (a raycasting game with hot per-pixel loops), where bounds-check overhead was both the largest source of generated-source bloat and a real obstacle to autovectorization in tight loops.

Both proposals are codegen-only (no language-surface change, no new syntax, no test-reachable semantics change beyond byte-identical stderr output on failure). Proposal 1 is a mechanical rewrite of the emission site. Proposal 2 is a new flow-analysis pass with a narrow, high-value scope.

---

## Current state

`codegen.c:2108-2137` emits, for every `slice[index]`:

```c
(*({ <slice_type> _s<N> = <slice-expr>;
     int64_t _i<N> = (int64_t)<index-expr>;
     if (_i<N> < 0 || _i<N> >= _s<N>.len) {
         fprintf(stderr, "<file>:<line>: slice index out of range: "
                         "index=%lld len=%lld\n",
                 (long long)_i<N>, (long long)_s<N>.len);
         abort();
     }
     _s<N>.ptr + _i<N>;
}))
```

A similar shape is emitted for subslices at `codegen.c:2139+` (EXPR_SLICE).

A typical wolf-fc build produces ~550 such expansions, each spanning ~200 bytes of C source. The generated C for the whole project is 827KB / 10,278 lines — a significant fraction of that is bounds-check boilerplate. Every access sets up and (by lexical nesting) potentially blocks code motion around an inline `fprintf`+`abort` call, even though the call is on a statically-never-taken path at runtime.

---

## Proposal 1: Cold out-of-line failure helper

### What changes

Emit a single cold, noreturn helper once in the prologue:

```c
static void fc_oob(const char *file, int line, long long idx, long long len)
    __attribute__((cold, noreturn));
static void fc_oob(const char *file, int line, long long idx, long long len) {
    fprintf(stderr, "%s:%d: slice index out of range: index=%lld len=%lld\n",
            file, line, idx, len);
    abort();
}
```

And replace each bounds-check expansion with:

```c
(*({ <slice_type> _s<N> = <slice-expr>;
     int64_t _i<N> = (int64_t)<index-expr>;
     if (__builtin_expect((uint64_t)_i<N> >= (uint64_t)_s<N>.len, 0))
         fc_oob("<file>", <line>, (long long)_i<N>, (long long)_s<N>.len);
     _s<N>.ptr + _i<N>;
}))
```

Three independent changes are packaged here, each pulling its weight:

**a. Unsigned-compare fusion.** `(uint64_t)_i >= (uint64_t)_s.len` subsumes both the negative-index check and the overflow check in a single compare. When `_i < 0`, its two's-complement reinterpretation as `uint64_t` is a huge value that compares greater than any valid `len` (slice lengths are non-negative by invariant). `-O2` GCC already fuses the short-circuit form into a single `cmp/ja` for most cases, but emitting the unsigned form directly is robust across compilers, optimization levels, and debug builds (`-O0` debug binaries see the same number of instructions).

**b. Cold out-of-line helper.** The current inlined `fprintf(stderr, ...); abort();` is what most limits hot-path optimization. Even though the branch is statically never taken, the call to `fprintf` is a full external function call in the middle of the emitted function. The C compiler must conservatively assume it reads and writes memory, clobbers caller-saved registers, and sequences with surrounding loads/stores. This defeats:

- Vectorization of loops with slice accesses in the body (the compiler can't prove the memory state is consistent across all lanes past a call that observes memory).
- Reordering of unrelated loads/stores across the check.
- Register-allocation decisions: variables live across the "dead" call get spilled.

Replacing the inline call with a single call to a `cold, noreturn` helper:
- Tells the optimizer "this path is rare; schedule it out of line."
- Tells it "control does not return from here" — nothing after the call needs to be kept live for the cold path.
- Removes the inline I/O call graph from the hot function entirely, so vectorization and load/store reordering become legal again.

Clang treats `cold, noreturn` similarly; on MSVC (not supported today, but relevant for Windows builds), use `__declspec(noreturn)` and skip `cold`.

**c. `__builtin_expect(..., 0)`.** Nudges basic-block layout so the fast path is fall-through and the cold call is emitted after the function body. Independent of `cold` on the helper; they cooperate. Harmless on compilers that don't recognize it (it's standard GCC/Clang).

### Cost

- About 5 new lines in the emitted prologue (one helper, one forward decl).
- Per-site expansion drops from ~200 bytes to ~100 bytes.
- Expected ~40–50% shrink of generated C for slice-heavy programs (wolf-fc: ~827KB → ~500KB estimated; not measured).

### Semantic compatibility

The existing error message format (`<file>:<line>: slice index out of range: index=N len=M`) is preserved byte-for-byte. The tests that assert on stderr — `slices/slice_bounds`, `slices/subslice_bounds_abort`, and any others that grep stderr output — continue to pass without modification.

One behavioral subtlety: `fprintf` is now called from a helper frame rather than the calling function. Stack-trace output under a debugger gains one frame (`fc_oob`). Not observable from within FC or from tests.

### Implementation

Mechanical. Two sites in `codegen.c`:

1. `EXPR_INDEX` at ~line 2108 — replace the block from `fprintf(out, "; if (_i%d < 0 || _i%d >= _s%d.len) { fprintf(stderr, ..."` through the `abort();` block with the new single-line form.
2. `EXPR_SLICE` at ~line 2139 — same rewrite for the two (lo/hi) bounds checks emitted for subslices. These are independent checks in the current codegen (`if (_lo < 0 || _hi > s.len || _lo > _hi)` — three-way); keep the failure-message wording the same, move the fail path to the helper.

Prologue emission: add the `fc_oob` definition next to the existing `fc_str` / `fc_slice_*` typedefs near the top of the output. Only emit if at least one bounds check was emitted in the translation unit (not critical — it's cold and tiny, no harm in always emitting).

Also applies to: EXPR_INDEX on pointer-to-fixed-array (the `(fc_slice_T){ .ptr = ..., .len = N }` adapter pattern seen in wolf-fc's `opl2__sample`), which flows through the same EXPR_INDEX path.

---

## Proposal 2: Elide bounds checks when the index is provably in range

### Motivation

The dominant pattern in real FC code is:

```
for i in 0..s.len
    ... s[i] ...
```

The index is mechanically, trivially in range. The emitted check is pure overhead — not just the runtime cost (one compare + predicted-not-taken branch), but the *optimization barrier* cost: the conditional branch with a call destination that the compiler cannot prove unreachable prevents autovectorization of the surrounding loop. For pixel-fill loops like wolf-fc's `upscale_2x` and the ceiling/floor fills in `render_walls`, this is the difference between scalar and SIMD code — measurably 2–4× on the hot path.

Elision is also the correct thing to do on principle: FC's bounds checks exist to catch mistakes. When the mistake is mechanically impossible, the check has no defensive value and is purely a tax.

### Scope: start narrow, widen later

There is a spectrum of sophistication here, from pattern-match on specific AST shapes to full abstract interpretation. The proposal is a two-phase approach: ship the narrow version first, revisit if real code shows the general version is worth the complexity.

**Phase 1 — Direct loop pattern (high value, small implementation):**

Match exactly this shape at emit time:

```
for <i> in 0..<expr-L>:
    ...
    <expr-S>[<i>]      // elide check
    ...
```

Elide the check when:
- The for-loop's low bound is the literal `0`.
- The for-loop's high bound is an `EXPR_FIELD` / `EXPR_DEREF_FIELD` reading `.len` on some slice expression `E_L`.
- At the index site, the slice expression `E_S` is *structurally identical* to `E_L` (same AST modulo source locations).
- `E_L` is a "stable path": a chain of variable lookups and struct-field accesses, with no function calls, no index expressions, no arithmetic. (A variable, `v.a.b`, `p->a->b` — yes; `xs[i].inner`, `f().len` — no.)
- The slice variable at the root of `E_L` is not reassigned between the loop header and the index site. A conservative, cheap check: the root is bound by a `let` (not `let mut`), or if by `let mut`, no assignment to it appears in the for-loop body.
- The index expression at the use site is exactly `<i>` (not `<i>+1`, not `<i>-1`).

This covers the overwhelming majority of loop accesses in wolf-fc, the stdlib, and the FC test suite. It is syntactic pattern-matching over the AST, no dataflow required.

**Phase 2 — Narrowing analysis (higher value, larger implementation):**

Add a simple per-function interval pass that tracks, for integer bindings, conservative `[lo, hi]` ranges:

- For-loop variables over `L..H`: range `[L, H-1]` if `L` and `H` are constants or stable-path `.len` reads.
- After `if (i >= 0 && i < s.len)`: range narrows within the true branch.
- Across unknown operations: range widens to `[INT64_MIN, INT64_MAX]`.

Then EXPR_INDEX asks the analyzer: "is `idx` proven to be in `[0, s.len - 1]`?" If yes, skip the check.

This generalizes Phase 1 to include:

```
for i in 1..s.len - 1   // range [1, s.len - 2], still safe
    s[i + 1]             // range [2, s.len - 1], still safe

if i >= 0 && i < xs.len
    xs[i]                // narrowed
```

Cost: one pass over each function body, interval dictionary keyed by symbol, bounded by function size. No fixed-point iteration needed if we only narrow along linear control flow and widen at joins.

Phase 2 should only ship if Phase 1 is measured to miss meaningful cases. For a game engine where the dominant pattern is `for i in 0..s.len`, Phase 1 alone is likely sufficient.

### What elision emits

When the check is elided, emit:

```c
<slice-expr>.ptr[<index-expr>]
```

No temporaries, no statement expression, no wrapping. This is also what the compiler will likely produce from the checked form with enough optimization, but emitting it directly means:

- The autovectorizer sees a clean loop with no conditional branches in the body.
- `-O0` debug builds are also fast — currently the check-heavy form is a real slowdown at `-O0`, which hurts the edit/run loop.
- Generated C is readable — a pixel loop looks like a pixel loop, not a wall of statement expressions.

### Correctness

An elided check must be provably safe. A false positive (eliding where the index might actually go out of bounds) is a memory-safety bug. The conditions in Phase 1 are purposely strict to make correctness inspectable:

- "Stable path" + "no reassignment in body" ensures `E_L.len` evaluated at loop header equals `E_S.len` evaluated at each use.
- Index expression == loop variable ensures the index is in `[0, E_L.len - 1]`.
- Slice lengths are non-negative by invariant (slices are built by codegen; FC has no way to produce a slice with negative length), so `i >= 0` is implied by `i in 0..len`.

Property tests in the FC test suite should include: a loop where the body does `s = ...` reassignment to `s` — the check must NOT be elided. Add one per phase.

### Implementation

For Phase 1:

- In codegen, when entering a `for i in LO..HI` loop, push a frame onto a small stack: `{ loop_var: symbol, bound_slice: AST-node-or-null }`. `bound_slice` is non-null iff `LO` is literal `0` and `HI` is an `EXPR_FIELD(.len, <stable-path>)`. Also record whether the stable-path root is immutable for the body.
- Walk the loop body. For nested assignments to the stable-path root, clear `bound_slice` for the rest of the body (conservative: once it could have changed, don't elide).
- At `EXPR_INDEX`, check: is the object a stable path structurally equal to any active frame's `bound_slice`? Is the index expression a reference to that frame's `loop_var`? If yes, emit the unchecked form.
- Pop the frame on loop exit.

Structural equality on AST nodes: small helper, recursive, ignores source locations. ~30 lines.

Stable-path predicate: predicate walks an AST node, accepts EXPR_VAR / EXPR_FIELD / EXPR_DEREF_FIELD, rejects everything else. ~15 lines.

"Root is immutable for body" check: walk the body once, record whether any EXPR_ASSIGN targets the root symbol. Can be deferred: if implementing this is awkward, fall back to "root is bound by `let` (immutable)" and accept that `let mut` loops don't get elision. The `let mut` case is rare enough to punt.

Total implementation: ~150–200 lines in codegen.c, one feature flag for easy rollback while the test suite is exercised.

For Phase 2: a new file, `bounds_analysis.c` + header, run in pass2 before codegen. A symbol → interval map, threaded through statements. Flat, no fixed-point. ~400–600 lines.

### Measurement

Before/after on wolf-fc:

- Generated C line count (expect drop).
- `size(1)` on the release binary (expect drop: dead fprintf call sites removed).
- Benchmark the `render_walls` + `upscale_2x` hot path (expect speedup, magnitude depends on vectorization unlock).
- Test suite pass/fail (expect no regression; bounds-check tests still fire on genuinely-out-of-range indices).

---

## Interaction and rollout

The two proposals are independent and can land in either order. Proposal 1 is mechanical and safe to ship first — it reduces the cost of every check that remains, so it's pure win even if Proposal 2 never happens. Proposal 2 reduces the *number* of checks emitted, so it multiplies the benefit of Proposal 1 (less of an already-cheaper thing).

Recommended order:

1. Ship Proposal 1 behind no flag — pure codegen quality improvement, no semantic change, test-compatible.
2. Measure generated-C size and hot-path performance on wolf-fc as a benchmark.
3. Ship Proposal 2 Phase 1 behind a compiler flag initially (e.g., `--elide-safe-bounds`), default off. Let it bake for a release.
4. Promote Phase 1 to default-on after wolf-fc and the full test suite demonstrate stability.
5. Decide on Phase 2 based on measured gaps.

---

## Proposal 2: deferral rationale (2026-04)

After shipping Proposal 1, we measured the vectorization story on a canonical shape — `for i in 0..xs.len: acc = acc + xs[i]` compiled with `-O2` and `-O3 -march=x86-64-v3` under both GCC 13 and Clang 18:

| Compiler / flags                | P1-checked form                | Unchecked form (what P2 would emit) |
|---------------------------------|--------------------------------|-------------------------------------|
| Clang `-O2`                     | vectorized (SSE, 5× `paddd`)   | vectorized (5× `paddd`)             |
| Clang `-O2 -march=x86-64-v3`    | vectorized (AVX2, 10× `vpaddd`)| vectorized (10× `vpaddd`)           |
| Clang `-O3 -march=x86-64-v3`    | vectorized (AVX2, 10× `vpaddd`)| vectorized (10× `vpaddd`)           |
| GCC `-O2` / `-O3`               | scalar                         | scalar                              |
| GCC `-O3 -march=x86-64-v3`      | **scalar** (check blocks it)   | vectorized (AVX2, 4× `vpaddd`)      |

**Clang already vectorizes P1-checked code identically to unchecked code.** The `cold+noreturn` pattern from Proposal 1 gives Clang's vectorizer everything it needs to prove the cold path is unreachable and emit a clean SIMD body. P2 would deliver zero value to Clang users.

**GCC does not vectorize the P1-checked form** and does not accept any of the `__builtin_unreachable`-based hints we tested to fix that (precheck-hoisting, `if (!(i < n)) __builtin_unreachable()` before the access, etc. — all measured, all still scalar). GCC's vectorizer appears to bail at the presence of a conditional call, regardless of `noreturn` or `cold` or hints that would make the condition provably false. Fixing this would require actually removing the branch from the emitted source, i.e. P2 as originally specified — no cheap middle-ground codegen trick worked.

Weighing that against P2's real cost — a correctness-critical AST pass with subtle edge cases around closures, defers, pointer aliasing, monomorphization, nested shadowed loops, and mutable slice reassignment, where any false-positive elision is a memory-safety hole — the value only shows up for a narrow audience: users who specifically care about autovectorized SIMD on hot slice loops, compile with GCC, and build at `-O3 -march=...`. For everyone else (Clang users, GCC users at `-O2`, users not on ISA-specific tunings), P1's VRP-based check elimination already reduces the runtime bounds check to effectively zero cost.

### Decision

**Defer P2 indefinitely.** Revisit only if one of these materializes:

- A concrete FC program's profile shows a GCC-built hot loop measurably bottlenecked by non-vectorization, with no reasonable workaround (e.g., switching to Clang, hand-unrolling, or using `any*` to bypass bounds checking on a specific proven-safe path).
- A future GCC release improves its vectorizer to see through cold+noreturn-guarded branches, at which point P2 becomes zero-value against that version too.
- FC adopts an `--unsafe-no-bounds-checks` escape hatch for specific hot paths (a coarser, more conservative alternative: user-chosen per-function, not compiler-inferred).

P2's AST machinery is not on the roadmap until one of the above forces the question.
