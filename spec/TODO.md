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

## Capturing-lambda context outlives its stack scope when the lambda is a block tail

A capturing lambda's environment is emitted as the address of a **stack compound literal**
(`codegen.c` `EXPR_FUNC`, ~line 4408): `.ctx = &(_ctx_NAME){ .cap = cap, ... }`. A C compound
literal inside a function has **block-scope lifetime** — it lives only to the end of the
innermost `{}` enclosing it. That is correct when the lambda is assigned directly at function
scope (literal and closure value share the function-body block), but **wrong whenever the lambda
is produced as the tail expression of a nested block** (`let…in`, and by the same mechanism an
`if`/`match` arm used as a value). FC emits such a block as a GCC statement-expression
`({ …; closure; })`, so the context literal gets the *inner* block's lifetime; the closure
struct is copied out into the outer binding, the inner block exits, the literal is destroyed,
and the surviving `.ctx` now dangles. Any later call through that closure reads freed stack.

Minimal self-contained repro (no euler-fc dependency):

```fc
let apply = (f: (int32) -> int32, x: int32) ->
    f(x)

let main = (args: str[]) ->
    let base = 10
    let adder =                       // lambda is the TAIL of a let…in block
        let b = base
        (x: int32) -> x + b
    let r = apply(adder, 5)           // .ctx already dangles here
    assert(r == 15)
    0
```

`fcc` emits the context literal inside the statement-expression:

```c
fc_fn_int32_t__int32_t _l_adder_1 = ({
    int32_t _l_b_2 = _l_base_0;
    (fc_fn_int32_t__int32_t){ .fn_ptr = _fn_3, .ctx = &(_ctx__fn_3){ ._l_b_2 = _l_b_2 } };
});   // inner block ends — _ctx__fn_3 temporary destroyed; _l_adder_1.ctx now dangles
```

Compiling the generated C with `cc -std=c11 -Wall -Werror -O2` fails:
`error: using dangling pointer '_l_b_2' to an unnamed temporary [-Werror=dangling-pointer=]`
(plus a companion `-Werror=uninitialized`, same root cause). The diagnostic only fires at
`-O2`: it needs inlining to trace the context pointer across the call into the lambda body. At
`-O0` GCC can't see it and the dead stack slot usually still holds the right bytes, so it
compiles and "works" by luck. **This is why the suite misses it** — tests default to `-O0`; the
`make test-*-O2` variants would catch a regression test for this pattern. The flattened form
(`let b = base` then `let adder = (x) -> x + b` as sibling statements) puts the literal at
function-body scope and is clean — that is the euler-fc workaround already applied in
`prelude.fc::nth_prime`.

Note the closure does **not** escape its creating function here (it's only called within the
same body), so this is purely a too-narrow-lifetime bug, not a true escape — escape analysis is
not the lever. The fix is in codegen: when a capturing lambda is the tail of a block expression,
the context must get the lifetime of the binding it flows into, not the inner block's. The
honest options are (a) **hoist** the `_ctx_NAME` literal to a named local declared at the
destination binding's scope (out of the statement-expression), or (b) restrict the pattern —
**not** heap-allocation, which would add a hidden alloc and a leak FC's manual-memory model
rejects (see "Static costs over runtime machinery"). The design choice is *where* to hoist and
how to handle deeper nesting (block-tail inside block-tail, lambda passed directly as a call
argument). Land it with `-O2` regression tests for `let…in`, `if`-arm, and `match`-arm valued
capturing lambdas.

---
