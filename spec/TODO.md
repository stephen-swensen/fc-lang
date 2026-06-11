# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

---

## Generic call as a non-first operand mis-parses

Surfaced while fixing the qualified-type-argument scan (see archived TODO, resolved
2026-06-10): an explicit-type-argument call `name<T>(...)` only parses when `name` is the
*leftmost* token of its expression. As any later operand it falls apart — with simple type
args, no dots involved:

- `assert(qo >= size_of<int32>())` — `unexpected token ')' in expression`
- `let x = 1 + size_of<int32>()` — same
- `assert(!is_big<int32>())` — same (prefix operand, not just binary RHS)

Root cause is *where* the disambiguation lives. The "generic call vs comparison" scan runs in
the `TOK_LT` infix handler (`parser.c`, `case TOK_LT`) and only fires when `left` is a bare
`EXPR_IDENT`/`EXPR_FIELD`. When the callee appears as the right operand of `>=`/`+`/`!`/etc.,
precedence climbing has already grabbed the bare name as that operator's operand (the `<` is at
comparison precedence, too low to bind in the recursive call), so by the time the loop sees
`<`, `left` is the whole compound expression and the scan never runs — the tokens then parse as
a chained comparison and die on `>(`.

Workarounds are clean and always available: bind the call with `let` first, or parenthesize the
call itself (`1 + (size_of<int32>())`, `!(is_big<int32>())`). Call-argument position
(`f(size_of<int32>())`) is unaffected since a bracketed expression restarts precedence.

Likely fix: hoist the generic-call disambiguation out of the infix `<` handler into postfix
position, so `name<T>(...)` binds at call precedence like an ordinary call `name(...)` — a
generic call *is* a call, and call arguments already restart precedence. The existing scan
(type-arg tokens between `<` and `>`, then `(` or `.`) transplants as-is; the infix handler's
comparison fallback stays for everything the scan rejects. Same disambiguation rule as today
(spec §Generic Functions, Parsing), just applied wherever a call can appear.

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
