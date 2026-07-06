# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

---

## Result type `T!` — design adopted, implementation pending (branch `result-type`)

The error-propagation carrier decision is made: a built-in result type `T!` = `ok('a) | err(i32)`
with intrinsic constructors `ok(v)` / `err(T, code)`, the `err == 0 ⇔ ok` repr, and full parity
with options (`x!` unwrap with code, literal-code patterns, `.is_ok`/`.is_err`, composition
`T?!`). Full rationale, rejected alternatives (incl. codes-on-`none` and the suffix bikeshed),
and the ordered follow-up list (implementation → propagation operator `x?` → stdlib code
convention → stdlib migration) live in **`spec/result-type-design.md`** — that document is the
single source of truth; don't re-litigate here. This supersedes the rc.6 audit's
"error-propagation sugar" item (the sugar lands *after* the carrier, as follow-up 2).

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

## `&` on `let` bindings yielding `const T*` — read-only address-of

Today `&p` requires `let mut` (§Address-of): a `T*` permits `*pp = v`, which is reassignment
through an alias — the very thing `let` forbids. But that argument doesn't apply to a pointer
that forecloses the write. The extension: **`&p` on a `let` binding yields `const T*`** (on a
`let mut` it stays `T*`, unchanged). This is Rust's `&`/`&mut` split minus the borrow checker,
and C++'s address-of-const, expressed with machinery FC already has.

**The friction it removes** (real demand — felt in practice) is the *false-`mut` tax*: needing
a pointer for plumbing reasons, not mutation, and having to declare `let mut` to get one. The
`mut` is dishonest in the source, and in FC it costs something concrete — `let mut` is
uncapturable, so promoting a binding just to take its address poisons its capturability for
every closure in scope. The cases that hit it:

- **Extern calls with `const T*` parameters** — the sharpest case; C fixed the signature
  (`nanosleep(const struct timespec*, …)`, `sigaction`, `setsockopt`, `init(const config*)`
  library entry points), so by-reference is not FC's choice to make.
- **Big-struct helpers** — `(cfg: const config*)` to skip the by-value copy; today the caller
  pays a copy into a `let mut` to get the pointer that avoids copies.
- **Constraint/comparator functions in generics** — the passed-function constraint style wants
  `(a: const T*, b: const T*) -> bool` for large `T`; only ergonomic if immutable data is
  pointable.
- **Read-only views at API boundaries** — hand `&table` to subroutines that consume it during
  the call; the single-value analog of the `const T[]` slice-view story.

**Why it's consistent and safe.** The "One rule, three knobs" derivation is untouched:
addressability tracked reassignability because of `*pp = v`, and `const` deep-rejects every
write through the pointer (§Deep const, §Write rejection), so nothing reachable through `&p`
can reassign or mutate `p`. Escape analysis needs nothing new — the result is `PROV_STACK`
like any address-of, same lifetime rules. `T*` already coerces to `const T*` (§Implicit
coercion), so callee signatures compose. Additive and non-breaking: `&p` on `let` goes from
compile error to `const T*`; no existing program changes meaning. Implementation is small:
pass2's address-of check produces a const pointer type instead of erroring; codegen's `&` is
unchanged.

**Design questions to settle before shipping:**

- **Observability, stated plainly in the spec:** `const T*` means no writes *through this
  pointer*, not "nobody writes" — a const view of `p` still observes `p.x = 10` performed
  through the binding. C's meaning of const, consistent with FC's existing const views.
- **Field address-of:** does `&p.field` on a `let` struct also yield `const F*`? Symmetry says
  yes (it's a smaller view of the same read-only aliasing); whatever the answer, the existing
  carve-outs stay (`&s.fixed_array` error, packed/bit-field restrictions).
- **Function bindings:** `&f` (C function pointer extraction) keeps its own rule — `let mut` +
  non-capturing (§Address-of). A `const`-qualified C function pointer isn't a meaningful
  interop artifact; decide explicitly that `&f` on a `let` lambda stays an error rather than
  falling through to the new rule.
- **Capturability interaction:** the binding stays capturable (that's half the point), and the
  resulting `const T*` is itself an ordinary pointer value a closure may capture by copy —
  confirm the capture-a-pointer idiom composes (it should: same as capturing any `let` pointer,
  programmer owns the lifetime).

---

## Editor / LSP server (`fcc --lsp`)

Architecture lives in `CLAUDE.md` → "Editor integration"; this section is the
open-item backlog. None of these block release.

### Smaller follow-ups (non-blocking)

- **Completion within-function flow-sensitivity** — the scope-at-position
  filter shipped: `complete_scope` walks the decl tree by line span and offers
  the enclosing module's members + imports, the enclosing function's locals,
  file imports, and top-level (no more global dump). One residual gap: inside a
  function body `harvest_expr` collects every local regardless of source order,
  so a `let` declared textually *after* the cursor is still offered. A line
  filter on the harvest closes it. Low value (a name you're about to type
  showing up a few lines early is mild).
- **Variant-constructor go-to-definition granularity** — lands on the union
  declaration, not the specific variant. Deliberate today, but `UnionVariant.loc`
  is now recorded, so refining it is a small change plus a wire-test update.
- **Residual parser `diag_fatal` sites** — error-recovery parsing (done — see
  `spec/hist/archived-todos.md`) left ~14 `diag_fatal`s in `parser.c` for
  validation *inside* matched delimiters (slice/tuple/`alloc`/`cstr[N]` literals,
  fixed-array size), string interpolation, the inline `let…in` form, and the
  extern-reserved-name check. These still abort the analysis (LSP falls back to
  `last_good`, no crash); converting them to `diag_error` + a best-effort node
  would close the last in-band recovery gaps. Low value (rare mid-typing).
- **Stdlib AST re-parse per keystroke** — the dominant half shipped: a session
  lex cache (commit `70a3331`) keeps unchanged feed sources (stdlib + `lsp.rsp`
  files) from being re-lexed each keystroke, and edit-burst coalescing
  (`263bff2`) collapses bursts into one analysis. `analyze()` still re-*parses*
  the cached tokens into a fresh AST each time; caching the parsed AST
  (token→AST) is the remaining, lower-value win now that lexing — the dominant
  cost — is cached.
- **Install targets are Linux-only** — `make install` / `install-vscode` assume
  a Linux layout; Windows/macOS packaging is unwritten.

---
