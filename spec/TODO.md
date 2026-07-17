# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

---

## Const generics (value parameters) — IMPLEMENTED 2026-07-17 on branch `n-const-generics`; evaluation open

Generic parameters over compile-time integers, motivated by std::wideint's hand-enumerated
width families. Same `'x` sigil as type variables, kind inferred from occurrence position
(`'n` in a size/value slot = const param); `struct wide = limbs: u32['n / 32]` instantiates
as `wide<128>`/`wide<256>` from one definition; functions infer const params by unification
(`(a: wide<'n>)`) or take them via the explicit `<'n>` prefix; const args admit literals,
const params, named consts, bare `+ - * / %` (parens for shifts); `mul_wide`-style width
doubling works (`wide<'n * 2>`). Instantiation-time checking (C++/Zig school) with
instantiation-chain diagnostics; value recursion (`f<'n + 1>`) rejected as an infinite
family (no compile-time branch pruning). Spec §Const Parameters; grammar `const_expr`;
tests `tests/cases/generics/const_*` + `wideint_proto` (acceptance: generic wide with
add/mul_wide at 128/256 from one definition).

2026-07-17: **decision resolved — kept**; std::wideint rewritten over `uwide<'n>`/`iwide<'n>`
(see §std::wideint above), 2826 → 739 lines with the same API surface and richer abort
messages (width interpolated per instance).

2026-07-17 (same day): **`static_assert(cond, "msg")` added** — compile-time instantiation
predicates in struct/union bodies and statement position, replacing the initial
divide-by-zero width-contract hack in wideint's size expressions with a readable one-liner
(`static_assert('n % 32 == 0 && 'n >= 64, "...")`). Judgmental, never generative — the
comptime fence is spec'd as a law: *compile-time evaluation may decide whether an
instantiation exists, never what it contains*; no calls in constant expressions, message
must be a string literal. Checked per instance at the mono_register choke point (all
instantiation paths), immediately for concrete conditions. Spec §Const Parameters → Static
assertions; tests `generics/static_assert_*`.

Open (not blocking):
- Struct literals for const-param structs: `wide { limbs = ... }` cannot infer `'n` from a
  slice-typed field value; construction is via `default(wide<N>)` + mutation or companion
  constructors. Consider size inference from array-literal field values later.
- Named consts inside *field* size slots (`limbs: u32[cfg.words]`) — const args in `< >`
  fold named consts, field sizes accept only literals/const-param expressions today.
- LSP: hover shows `wide<256>` via type_name; `'n` hovers as `i32`. No dedicated const-param
  hover docs yet.

---

## std::wideint (né fixint) wide integers — IMPLEMENTED 2026-07-14; REWRITTEN over const generics 2026-07-17

Wide fixed-width integers as by-value limb structs with companion-module operations over a
shared private `u32[]` core: wrapping arithmetic, carry/borrow and `mul_wide` reporting
forms, `checked_*` abort forms, div/rem/divmod, bitwise/shifts, signed two's-complement
ops, `min()`/`max()`, strict `parse`/`parse_hex` (→ `uwide<'n>!`/`iwide<'n>!`),
`to_str`/`to_hex`. Spec §std::wideint; tests in `tests/cases/stdlib/wideint_*`.

2026-07-17: **rewritten from 12 hand-enumerated width families (u128–u4096, i128–i4096,
2826 lines) to two const-generic definitions (`uwide<'n>`, `iwide<'n>`, 739 lines)** — the
motivating case for the const-generics feature, closing its evaluation gate. Any width
that is a multiple of 32 and ≥ 64 now instantiates (uwide<192> works; the width contract
is enforced at instantiation by a `static_assert` in the struct body). The `mul_wide`
ladder is now unbounded (`uwide<4096>.mul_wide` →
`uwide<8192>`). Abort messages carry the concrete width via string interpolation in the
assert message (`uwide.checked_mul: overflow past 256 bits` — evaluated only on the abort
path). Width-inferring ops (`uwide.add(a, b)`) vs explicitly-named constructors
(`uwide<128>.from_u64(x)`, `uwide<256>.max()`). History: 2026-07-14 enumerated impl;
2026-07-16 1024/2048/4096 families + abort messages + companion docs (all superseded by
the generic rewrite, which preserved the same API surface, docs, and abort-message
content). By-value/heap line unchanged: fixed width = value struct; a future
arbitrary-precision `std::bignum` is the heap tool.

Follow-ups (deliberately additive, not blocking):
- **Cross-width conversions**: widening (`uwide<128>`→`uwide<256>`), truncating, and
  signed↔unsigned reinterpretation at the same width. Today the only cross-width paths are
  `mul_wide` or a heap round-trip through `to_hex`/`parse_hex`. (With const generics these
  can now be written ONCE: `widen(a: uwide<'m>) -> uwide<'n>` needs 'n inferred from the
  call context or named explicitly — `uwide<256>.widen_from(a)`.)
- **Division core**: `limbs_divmod` is binary long division (O(bits) iterations — correct,
  simple); a Knuth-D core could replace it without touching any caller if wide division
  ever becomes hot.

## Enum declarations — IMPLEMENTED 2026-07-10

Closed sets of named integer constants over a declared fixed-width repr (`enum door_lock of u8 =
| normal_a | gold_key = 1 …`): union-style exhaustive matching (sound — `enum_of(E, x) -> E?` is
the only integer→enum path), qualified construction, same-enum ordering, direct slice indexing,
total cast out to any numeric type, `E.count` const-foldable type property, mandatory zero
variant (`default(E)` = zero-filled ≡ default invariant). Transpiles to a fixed-width integer
typedef (never a C `enum` — impl-defined width; 16-bit-int targets). First-class
`DECL_ENUM`/`TYPE_ENUM` through the whole pipeline incl. LSP. Tests in `tests/cases/enums/` +
`exhaustiveness/exhaust_enum_*`; spec §Enums (Part 4), §Static Type Properties (`count`),
Part 8 §Mapping C enums rewritten. Slice-literal lengths were generalized from "integer
literal" to "const-foldable constant expression" to admit `E.count` (also unlocks `i32.bits`,
`1 + 2`). Resolves the rc.6 audit item "No C-style enum / integer exhaustiveness".

Follow-ups (deliberately additive, not blocking):
- Reflection-lite: `enum_name(e)` (static name table, pay-when-used — the `error_name`
  design) and variant iteration; wanted for logging and CLI/config parsing.
- `extern enum` verification: FC-side redeclaration with emitted `_Static_assert` against
  the C header's values. C-owned sets stay extern-constant modules until then.
- Flags remain integers by design (a flag combination is outside any closed set); if
  embedded/driver work ever needs bit-precise register fields, that is a packed-struct
  feature, not an enum feature.

---

## Result type `T!` — IMPLEMENTED 2026-07-06 (branch `result-type`); follow-ups open

The carrier is in: built-in `T!` = `ok('a) | err(i32)` with intrinsic constructors `ok(v)` /
`err(T, code)` (hard keywords, like `some`/`none`), the `err == 0 ⇔ ok` repr, and full parity
with options — `x!` unwrap printing the error code, literal-code patterns, `.is_ok`/`.is_err`,
`err(T, 0)` compile-error/runtime-guard mirroring `some(null)`, composition `T?!`/`T!?`,
generics `'a!`, `default(T!) = ok(default(T))`, equality, LSP hover/completion. Tests in
`tests/cases/results/` + `exhaustiveness/exhaust_result_*`; spec §Result Types + Part 5
`## result`; grammar Rule 6 documents the `(x!)` cast-vs-unwrap resolution (cast only when `)` is
followed by an expression start). **`spec/result-type-design.md` remains the single source of
truth**; don't re-litigate here. Remaining follow-ups, in order:

1. **Propagation operator `x?`** — ✅ IMPLEMENTED 2026-07-06 (designed same day; see design
   doc §Propagation operator incl. implementation notes): postfix `?` on both carriers —
   unwrap on success, return the failure (err verbatim / none) from the enclosing function on
   failure; pure local desugar (defers unwind free), top type layer only. **No inference
   contribution**: the enclosing function must already return the matching carrier, anchored
   by explicit ok/err/some/none (bare `ok` for void!) on its success paths — an implicit
   return-type lift was implemented, then rejected same day as an explicitness violation
   (recorded under the design doc's rejected alternatives; don't re-propose). Errors at the
   `?` site otherwise (subsumes kind mixing); no `?` in defer (walk also closed the
   pre-existing return-in-loop-in-defer hole), none in `main`/top level; recursion works
   (base case anchors the carrier). Tests `results/prop_*` + `options/prop_option_*`; spec
   §Propagation; grammar postfix `?`.
2. **Stdlib error-code convention** — ✅ IMPLEMENTED 2026-07-06: compiler-owned code space via
   `error` declarations (hard keyword; groups desugar to pseudo-modules of i32 consts;
   deterministic assignment from 65536, [1, 65535] reserved platform passthrough; qualified
   constant patterns; `error` i32 display alias in type position; `error_name(e) -> str?`
   intrinsic; named `x!` aborts under `--backtraces`; automatic `<output>.errcodes` map on
   every successful compile that declares errors).
   Design record in `spec/result-type-design.md` §Error-code organization; spec §Named error
   codes; tests `tests/cases/errors/` + `backtraces/err_unwrap_named`.
3. **C-interop extern result mapping** — ✅ IMPLEMENTED 2026-07-06 (designed same day; see
   design doc §C interop incl. implementation notes): extern declarations returning `T!` take
   a mandatory `from <protocol>` tail; closed protocol set `errno(-1)`/`errno(null)`/`status`/
   `neg_errno`/`hresult`/`last_error(<s>)`/`wsa_error(-1)`; payload-ness declared by the
   return type (`i32!` vs `void!`); `void!` legalized as the payload-less result (bare `ok`
   expression + pattern, repr = lone `int32_t`, `status` is a repr identity); raw code
   passthrough, no arithmetic; call sites wrapped inline (no adapter fns, variadics free).
   Tests `extern/proto_*` + `results/void_result_*`; spec §Extern error protocols + §`void!`.
4. **Stdlib migration** — ✅ IMPLEMENTED 2026-07-07 (design record: design doc §Stdlib error
   contract): operations return `T!`/`void!` via protocol externs, predicates stay `bool`,
   absence stays `T?`; wrappers map branchable platform codes to curated named conditions
   (`io.file.*`, `net.conn.*`, `net.dns.*`, `text.parse.*` — the latter all-named, fixing the
   invisible-overflow and i64-truncation holes in `parse_*`) with raw passthrough for the
   uncurated tail; `read_char` is the `u8?!` showcase; `read`/`write` stay raw counts. The
   errno accessor shim works via a scoped lexer/parser change: the extern C-name position
   admits `__` identifiers (with mandatory `as` alias) so `__errno_location`/`__error` are
   declarable (grammar note + spec §Reserved identifiers; tests `extern/dunder_*`). Spec
   Part 9 updated; demos migrated; tests `stdlib/*` reworked + `stdlib/net_error_paths`.

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

## Discarded pure value as a no-op error — extend the self-assignment rule

Surfaced 2026-07-12 by a hand-written `factorial` whose `loop` had no `break`: the arm
`| 0 -> product` was intended to exit the loop with `product`, but a `loop`'s value comes only
from `break v` (`pass2.c:6632`), so the arm's value was silently discarded, the loop typed
`void`, and the function inferred `-> void`. The compiler caught it only at the *use* site
(`cannot bind void expression to 'f'`); at the definition it was silent, because a breakless
`loop` is the blessed intentional-infinite-loop idiom (`pass2.c:720`) and a void-returning
function is legal. No "did you forget `break`?" heuristic is wanted — that is warning-shaped,
and FC has exactly one diagnostic severity (`diag.c` / CLAUDE.md).

There is, however, a *sound* error consistent with FC's existing precedent: **self-assignment
`x = x` is an error because it is provably a no-op** (`pass2.c`, "self-assignment of 'x' has no
effect"). A **discarded, statically-effect-free value** is the same category — `| 0 -> product`
computes a value and throws it away. Today discards are legal C-style for *any* non-result type
(only `T!` results are guarded — `check_result_ignore`, `pass2.c:1208`, comment at :1207); the
proposal narrows that to reject discards that are *provably pure*, so the mistake fails the
build while side-effecting discards (a byte count, a call) stay legal.

Not a one-liner — three things to settle before it's sound:

- **Purity predicate.** Sound only over statically-effect-free expressions: literals,
  identifiers, field access, casts, `&x`, wrapping arithmetic/comparison. Everything that can
  abort or mutate is *not* pure and must stay legal discarded — `x!` (unwrap abort), `arr[i]`
  (bounds abort), `checked …`/guards (overflow abort), assignment, and any call.
- **Per-arm, with discard propagation.** The whole `match` in the factorial is *not* pure — its
  `| _ ->` arm mutates `n` via `defer` — so a whole-expression purity check wouldn't fire. The
  check must propagate "this position is discarded" into match/if arm tails and flag the pure
  arm (`| 0 -> product`) individually. That plumbing is the bulk of the work.
- **Diagnostic + the intentional-no-op escape.** An arm that deliberately does nothing is
  written `void(...)` (the blessed void-typing spelling — *not* bare `()`); the message should
  name it: "value computed but discarded; wrap in `void(...)` for an intentional no-op arm, or
  `break`/return it." Mirror the self-assignment wording.

Blast radius is a new error class over shared pass2 code — validate across the full suite +
stdlib + the sibling euler-fc before trusting the false-positive rate. Rare-mistake / fiddly-win,
hence backlog, not blocking. (The `never`/bottom-type alternative — typing a breakless loop
`never` à la Rust `!` / Zig `noreturn` — is a much larger type-system change and, absent
unreachable-code detection, would make this *more* silent, not less; not pursued.)

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
