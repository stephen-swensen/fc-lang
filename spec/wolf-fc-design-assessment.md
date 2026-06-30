# FC Design Assessment — wolf-fc as the real-world case study

*Assessment date: 2026-06-30. Reviewer: Claude (Opus 4.8). Evidence base: `spec/examples.fc`
plus a cross-section of [wolf-fc](https://github.com/stephen-swensen/wolf-fc) — `src/sdl2.fc`
(SDL FFI), `src/opl2.fc` and `src/render.fc` (hot paths), `src/combat.fc` (game-logic unions),
`src/data.fc` (binary loaders). wolf-fc was ~17.2K lines of FC at the time of review.*

The question this answers: does FC keep close to C's spirit while offering ML-style niceties,
smoothing C's biggest footguns (slices, options) **without** going off the rails into Rust-style
abstraction?

---

## Verdict: compelling, and the niche is real

FC lands almost exactly where it aims: **C's semantic model with a 2010s skin**. The nearest
precedent is not Rust — it's **Zig**. Manual memory, monomorphized generics, no hidden control
flow, escape hatches over guarantees. But FC diverges from Zig in a coherent direction: it trades
Zig's `comptime` metaprogramming for **ML's pattern matching, real sum types, and bottom-up
inference**. That is a defensible, underserved position — "C with expression-orientation and
exhaustive `match`, minus the ceremony." Nobody else sits precisely there.

The strongest single argument is that **wolf-fc exists and reads well**: a software raycaster, a
from-scratch YM3812 (OPL2) FM-synth emulator, and an in-process audio mixer, cross-platform, in a
language with no training-data precedent — and `opl2.fc`/`render.fc` read like unusually
well-typed C, not like a fight with the language. That is the proof that matters.

---

## Where the "C spirit + niceties" thesis pays off

- **Sum types + exhaustive `match` are the killer feature.** `combat.fc`'s `enemy_kind`
  (18 variants), `enemy_state`, and `damage_source` are, in C, `int` enums plus a `switch` with no
  completeness check. In FC, adding a variant breaks the build at every match site — a contract the
  code documents explicitly at `combat.fc:90-95`. A genuine, earned safety win at zero runtime
  cost. This one feature likely removes more real bugs than everything else combined.

- **Slices are the right footgun fix, done the right way.** C's #1 bug — pointer + length passed
  separately — becomes one fat pointer with `.len` and a bounds check. Crucially the check is
  *removable with a paper trail*: `render.fc:41` and `:110-118` drop bounds checks on the
  per-pixel hot path with a written proof of the precondition. Safe-by-default,
  escape-hatch-with-receipts is exactly the right systems posture — neither C's "always unsafe"
  nor Rust's "prove it or rewrite it."

- **`checked`/`unchecked` ⟂ `guarded`/`unguarded` as orthogonal axes** is one of the most elegant
  things in the language. It is the "documented C-like costs over hidden machinery" philosophy made
  concrete: opt into wrapping, saturation, or abort locally, and the reader sees the cost.

- **Options + `defer` give Go/Zig-style resource discipline without RAII.** `data.fc`'s
  `match io.read_all(path) with | none | some(contents) -> defer free(contents)` is clean,
  explicit, and has no hidden destructors — the right fit for the audience.

- **C interop is mostly flat and honest.** `sdl2.fc`'s `extern ... as` rename FFI is about as
  low-friction as C binding gets, and `any*` for opaque handles is correct minimalism.

---

## Honest tensions to watch as it scales

The "off the rails" worry was about Rust-style *abstraction* creep. On that axis FC is clean — no
lifetimes, traits, borrows, or ownership types. But three other places are worth naming.

1. **The closure ceiling is the real cost of the minimalism.** Capturing closures can't be returned
   (stack context), so `examples.fc:98` and wolf-fc both inline composition instead. An honest
   tradeoff (no heap closure environments, no GC), but it means the functional toolkit is
   "HOF-as-argument only" — no partial application, no map/filter returning composed closures. For
   an ML-flavored language this is the most visible ergonomic ceiling, and it *is* a ceiling.

2. **Conceptual weight is low, but syntactic surface area is no longer small.** The subtler
   "off the rails" risk. `examples.fc` now carries tuples, `when`-guards, or-patterns, two
   checked/guarded axes, deep-`const`, atomics, hex floats, explicit type args, *two* stack-alloc
   homes (`%.16s` vs `alloca`) plus a heap one, greedy `let…in`, and `;`-sequencing. None of it is
   Rust-conceptually-heavy — but the feature *count* is creeping from "minimal" toward "rich." FC
   avoids Rust's conceptual tax cleanly; it is accumulating a syntax-surface tax. These are
   different axes and worth tracking separately, because the second one sneaks up.

3. **Inferred return types lean hard on tooling.** No return annotations is lovely for terseness,
   but in a 4.6K-line `main.fc` a function's contract isn't visible without running inference in
   your head — you're betting on LSP inlay hints to carry it. A legitimate TypeScript-ish stance,
   but a sharp departure from C's "everything is declared," and likely *why* the LSP work looms so
   large. A soft spot that grows with codebase size.

*Minor leaks:* the `->` triple-overload forcing `(p->x)` parens inside match guards
(`examples.fc:307`), and tagless-typedef interop needing the `any*` + mirror-struct dance
(`sdl2.fc:137-147`). Individually trivial; they accumulate.

---

## Bottom line

Compelling, and — rarer — *coherent*. FC keeps C's mental model intact (a C programmer reads the
SoA `chip` struct in `opl2.fc` and knows the exact memory layout), spends its novelty budget where
the payoff is highest (sum types, slices, options, exhaustiveness), and refuses the abstractions
that would have made it Rust. The two things to watch as it scales are not soundness or
scope-creep-into-Rust — they are the **closure ceiling** and the **read-without-tooling** problem
from inferred return types. Neither is fixable without giving something up, so the live question is
whether they stay as acceptable at 50K lines as they clearly are at 17K.
