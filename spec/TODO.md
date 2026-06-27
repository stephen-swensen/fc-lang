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

---

## Editor / LSP server (`fcc --lsp`)

Architecture lives in `CLAUDE.md` → "Editor integration"; this section is the
open-item backlog. None of these block release.

### `data.fc` CodeLens is empty under `-O0` (optimization-level divergence)

Opening `stdlib/data.fc` returns an empty `textDocument/codeLens` response when
`fcc` is built at `-O0` (`make dev`, or any `-fsanitize` build), but is correct
at `-O2` (the default `make`, and what `make test-lsp` / `make check` build). So
the wire test `tests/lsp/lsp_test.py` ("type CodeLens still provided") passes in
CI and fails only on debug builds.

This **predates** the 2026-06-27 LSP work — it reproduces on a pristine checkout,
so it is not a regression from the doc-comment / go-to-def / leak changes. The
divergence-by-optimization-level smells like undefined behavior or an
uninitialized read that `-O2` happens to mask; ASan (a `-O0` build) reports no
memory error for the session, so it is a *logic* divergence, not a detectable
overflow/UAF. `data.fc` is `namespace std:: / module data` with generics
(`array_list<'a>`); `lens_decls`→`lens_emit` finds zero emittable `let` lenses
while analysis reports no diagnostics for the file. First thing to check: whether
pass2 is being silently gated — a pass1 error in a *sibling* merged stdlib file
suppresses all resolved types yet stays invisible because diagnostics are
filtered to the open file — and whether that gating differs by opt level.

### Smaller follow-ups (non-blocking)

- **Completion over-offers** — no flow-sensitive local scoping, so it lists
  names not yet in scope at the cursor. Needs a scope-at-position filter.
- **Variant-constructor go-to-definition granularity** — lands on the union
  declaration, not the specific variant. Deliberate today, but `UnionVariant.loc`
  is now recorded, so refining it is a small change plus a wire-test update.
- **Stdlib re-parse per keystroke** — `analyze()` re-lexes/parses the whole
  merged stdlib on every edit; caching the parsed stdlib would cut per-keystroke
  CPU (the leak it also caused is already fixed).
- **Install targets are Linux-only** — `make install` / `install-vscode` assume
  a Linux layout; Windows/macOS packaging is unwritten.

---
