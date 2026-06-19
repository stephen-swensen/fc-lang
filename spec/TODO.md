# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

---

## Atomic pointer publication — `T*` / `any*` pointees for the atomic builtins

`atomic_load_acquire` / `atomic_store_release` (shipped 2026-06-10, see archived TODO) accept
only integer and `bool` pointees. The one common lock-free idiom that restriction blocks is
**pointer publication**: release-store a `T*` into a shared cell and let the other thread
acquire-load it — the "swap in the new state and let the consumer pick it up" pattern
(double-buffer swaps, hot-reload handoffs). Workarounds exist and are honest — publish an index
into a known array (indices are integers), or pass the pointer through a one-slot SPSC ring as
ordinary slot payload — so this is not urgent, but it's the first gap a user is likely to hit.

Implementation is small (the same two builtins, one more pointee category; `__atomic_load_n`/
`__atomic_store_n` already handle pointer types). The deferred work is **escape analysis**:
a release-store of a pointer is a store through a pointer, so the provenance rules need answers —
minimally, reject storing a `PROV_STACK` pointer into a cell that may outlive the frame (mirror
the existing "cannot store stack pointer in heap struct" rule), and decide what provenance an
acquire-loaded pointer carries (`PROV_UNKNOWN` is the conservative answer). Don't ship the
pointee expansion without closing the escape rules — conservative-but-complete.

## Atomic `fetch_add` — first read-modify-write op (shared counters across threads)

The shipped acquire/release pair is complete for single-writer communication (SPSC rings,
flags, published values): a single writer can do plain read-modify-write on its own cell. The
model breaks the moment **two threads increment the same counter** — shared stats, refcounts on
buffers co-owned with an audio thread — where load+add+store loses increments. The only
workaround inside the current model is restructuring to single-writer ownership, which is often
the better design but not always available.

`atomic_fetch_add(p, v)` (returning the prior value) is the smallest RMW that closes this:
emit `__atomic_fetch_add(p, v, __ATOMIC_ACQ_REL)`, same builtin wiring pattern as the existing
pair, same integer pointees (`bool` excluded), same lock-free static assert. Spec work: extend
§Atomics & Memory Model with RMW semantics — an acq_rel RMW is both an acquire load and a
release store, and the modification order of a single cell is total. Naming should follow the
established explicit-ordering convention. CAS / `exchange` stay out of this item — see the
separate TODO below; don't bundle them in "while we're at it".

## Atomic exchange and compare-and-swap — complete the RMW set

FC is a general-purpose systems language, so the standard lock-free toolkit should eventually
be complete regardless of any one program's needs. These two close it out:

**`atomic_exchange(p, v)`** — unconditionally store `v`, return the prior value. Not a niche
op: it's the idiomatic **snapshot-and-reset counter** (`let hits = atomic_exchange(&counter, 0)`
— a stats thread drains the count while writers keep `fetch_add`-ing, no lost increments), the
one-shot claim flag (`if !atomic_exchange(&claimed, true) then /* we got here first */`), and —
once pointer pointees land (TODO above) — the pointer steal (`atomic_exchange(&queue_head,
null)`). Simpler than CAS (no comparison, no failure path: it's an unconditional acq_rel RMW)
and useful independently; it can ship with `fetch_add` rather than waiting for CAS.

**`atomic_compare_exchange(p, expected, desired)`** — conditional RMW, the primitive for
**multi-producer/multi-consumer structures** (several threads enqueueing into one ring,
lock-free freelists/stacks, claim-a-slot protocols) and any "only update if unchanged"
protocol. Design questions to settle:

- **Result shape.** Two viable signatures, both expressible in FC: mirror C by taking
  `expected: T*` and writing the observed value back through the pointer (FC pointers serve as
  out-params; returns `bool` success), or take `expected` by value and return the observed
  value, with the caller comparing to detect success. The pointer form is one call per retry
  loop iteration; the by-value form is the simpler signature. Pick one — shipping both is the
  kind of API surface FC avoids (no overloading).
- **Weak vs strong.** Spurious-failure weak CAS only matters as a perf refinement inside retry
  loops; ship strong-only (`__atomic_compare_exchange_n(..., false, ...)`), add weak later if
  ever demanded.
- **Failure-path ordering.** C11 lets the comparison-failed load use a weaker ordering; with
  FC's one-ordering-per-op convention, the failure load is simply acquire — document it, no knob.

Both follow the established builtin pattern (`__atomic_exchange_n` /
`__atomic_compare_exchange_n` with acq_rel orderings, integer pointees, lock-free static
assert). Spec work extends the RMW section from the `fetch_add` item: exchange and CAS-success
are acq_rel RMWs in the cell's total modification order; CAS-failure is an acquire load with
no store.

## Unchecked cast `(T!)` — opt out of float→int saturation in hot paths

The saturating float→int cast (rc.5, audit item 16) is the **only** UB-fix we shipped that
carries per-operation runtime overhead. Every other "define C's UB" cast decision is free at
runtime: pointer↔int and int-literal→pointer are *static* restrictions (the emitted cast is a
bare reinterpret), and int→float / narrowing int→int / float→float are already defined in C and
emit a bare cast. Only `float → int` routes through the `fc_f2*` helper family (NaN check + two
range branches before the truncate). Measured cost in a per-pixel loop (`-O3`): ~2.5× vs the
bare `cvttsd2si`, +56% instructions / +3 `comisd` / +4 branches per cast. In wolf-fc's raycaster
that's ~64k+ saturating conversions/frame in the wall-texturing band alone (see wolf-fc
`[render-fp]`), almost always on values the programmer already knows are in range and clamps
right after — the checks never fire but always cost.

**Keep saturating as the default** — it removes real UB (NaN/overflow → portable, deterministic
result instead of platform-dependent garbage or miscompiled surrounding code), consistent with
the rest of the language. Add an opt-in escape hatch for the hot sites instead of weakening the
default.

**Syntax: `(int32!) f`** — suffix `!` on the target type in a cast. Reads like option-unwrap's
postfix `!`: "I, the programmer, assert the precondition the compiler can't verify; don't make
me pay for the failure path." Emits the bare `(int32_t)f` — i.e. exactly the rc.4 codegen, so it
is **bit-identical to pre-rc.5 for every in-range value** (preserves wolf-fc's bit-stable golden
render path at marked sites; UB only on the out-of-range/NaN inputs the programmer is vouching
won't occur).

**Legal only where a runtime check exists to skip — i.e. a float source with an integer target.**
`(int32!) f`, `(uint8!) f`, `(usize!) f` are valid; `(int32!) someInt`, `(float64!) i`,
`(usize!) ptr` are **compile errors**: "redundant `!`: this cast inserts no runtime check." This
mirrors the existing rule that `x!` is rejected on a non-option — suffix `!` always *means
something* everywhere it's accepted, never silently inert. This is the answer to "does `!` extend
to other casts": no — float→int is the entire list, and the type checker enforces that.

**Spec note — trap vs. UB asymmetry, deliberate.** Option `x!` is *checked* (emits the tag test,
aborts on `none`); cast `(T!)` is *unchecked* (UB if out of range). Both read as "assert the
precondition," but they fail differently. This is intentional: a *trapping* cast would still emit
the range comparisons (just branching to `abort` instead of saturating) and would be no faster —
defeating the purpose. The whole value is removing the branches, so `(T!)` must be genuinely
unchecked. One explicit spec sentence so nobody expects `(int32!)` to abort like `x!`.

Implementation is small: parser accepts an optional `!` after the type name in cast position;
the type checker permits it only when the operand is float-typed and the target integral (error
otherwise); codegen emits the bare C cast instead of the `fc_f2*` helper. No new runtime, no
monomorph interaction.

**Optional complement — a cast-then-clamp peephole.** A `(int32) f` whose result flows directly
into a clamp to a sub-range (`if x < lo … if x > hi …`) has provably-dead saturation; the
compiler could elide the helper there with zero source churn and bit-identical output. Attractive
because it speeds up *existing* code (wolf's band loop already clamps to `[0,63]`), but it's
narrow and pattern-fragile — ship it as a bonus, not the primary mechanism. The explicit `(T!)`
is the general, predictable lever.

**Scope boundary — don't bundle.** This item is casts only. The other runtime-checked constructs
(slice indexing → bounds check, `/` and `%` → divide-by-zero + `INT_MIN/-1` guard) are
*operators*, not casts; extending the `!`-unchecked convention to them (`s[i!]`, `a /! b`) is a
separate, larger design question with its own soundness story. Note it here only so the map is
complete; resolve it on its own.

> Origin: rc.4→rc.5 wolf-fc codegen-regression audit (2026-06-19). The saturating cast was the
> one measured per-frame regression; fixed-point texcoords (wolf-fc `[render-fp]`) sidestep it
> for converted sites, and `(T!)` covers the float→int casts that remain (DDA setup, sprite
> scaling).

---
