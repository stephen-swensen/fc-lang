# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

---

## Slice length representation `--len-repr` — IMPLEMENTED 2026-08-22; follow-ups open

Resolution of the i64-length / small-target tension (rc.6 audit item 5's deeper half;
niche.md structural blocker 1). The settled position: **the semantic domain of every
length is `i64` on every target, forever; only the stored representation is a build
knob.** `fcc --len-repr <16|32|64>` (default 64 — historical behavior) sizes `fc_len_t`,
every slice/str len field, and every bounds compare (emitted at
`max(index static width, len width)` — a single native compare for narrow indices).
Soundness invariant: every stored len proven in `[0, FC_LEN_MAX]` at construction —
compile-time lens (string/slice literals, fixed-array sizes incl. per-instance
const-generic folds, const-context computed lens) judged statically in pass2; runtime
lens guarded at the construction sites (raw-parts `fc_chk_len`, runtime `alloca` cap,
cstr→str strlen, argv, interpolation `_flen` cap), with `alloc(T[n] { })` over capacity
answering `none` (an allocation that cannot succeed). `&s.len` is a compile error (the
stored field can't be an honest `i64*`). Reads widen (`(int64_t)s.len`) — free at 64.
Spec §Length representation; tests `slices/len_repr*`, `generics/len_repr16_const_inst_err`;
`make test-{gcc,clang}-len16` runs the whole suite at 16 on the host (green from day one,
both compilers, -O0 and -O2).

**Follow-up — as-if narrowing of range-form `for` counters.** `for i in 0..s.len` binds
a user-visible `i64` and emits an `int64_t` counter, the main residual 64-bit cost in
idiomatic loops on 16-bit targets (element-form counters already run at `fc_len_t`).
Semantics are deterministic, so the emitter may narrow the counter whenever all uses
provably fit (endpoints bounded by a stored len or by narrow constants) — pure as-if,
no spec change (§Length representation already grants the latitude). Do after real
gcc-ia16/djgpp measurements show it matters.

**Follow-up — the freestanding profile (Lane 1 gates).** The dependency half of
small-target support: what the emitted C assumes about its runtime (stdio-printing
guards, malloc, snprintf, …) and how each assumption becomes a hook or a compile-error
gate. Audited and planned in **`spec/freestanding.md`** (2026-08-22) — eight items
(`fc_trap` keystone → allocator hook → float/atomics/backtraces gates → freestanding
interpolation formatter → stdlib layering → `--profile <name>` bundles that imply a
`--len-repr`, gcc `-O2`-style; `--len-repr` stays the single primitive knob). Per-target
needs and ordering live there; the djgpp wolf-fc experiment needs none of it and comes
first.

## As-if elision of provably-dead bounds guards

Sibling of the for-counter narrowing follow-up above — same as-if family, same
range-analysis machinery, implement together. Purely an optimization by definition: a
guard may be omitted only when pass2 proves it can never fire, so observable behavior
(including which abort a program hits) is unchanged and no spec change is needed.

Two facts prove a guard dead, and both are FC-visible where a C compiler is blind:

- **Index range**: a small value-range lattice over the typed AST — literals, `& mask`,
  `%`, range-form `for` variables, if/match branch refinement, widening casts. (The
  counter-narrowing item needs exactly this lattice.)
- **Length is constant**: fixed-size struct array fields (declared `N`), frozen module
  slice literals, and — the case no C compiler can ever recover — a module `let mut`
  slice whose *root is never reassigned* anywhere in the program (one whole-program
  scan; FC compiles whole-program, so global knowledge is cheap — the semantic-`<`-gate
  precedent). Locals bound directly to slice literals qualify the same way.

With both proven, emit the bare access; for a frozen module slice additionally fold the
header — its `.ptr`/`.len` are compile-time constants, so the emitted C can reference
the backing array directly instead of loading the slice struct (the form handwritten C
takes).

Honest sizing, measured on the opl2 emulator bench (2026-08-30, host gcc -O2, 20M
samples): of 221 emitted guards, gcc's own VRP+inlining already eliminated all but 16;
freezing the tables (pregen migration) auto-killed the two hot survivors whose *index*
was provable but whose *len* was a mut-global load; the residual FC-vs-C gap (~8%) was
emission shape, not guards. So the value of this item is **not** host `-O2` speed — it
is `-O0`/debug builds, retro toolchains that do far less VRP (djgpp gcc, gcc-ia16 — the
targets the niche cares about), and smaller emitted C. Same gating as the counter item:
do after real djgpp/ia16 measurements show it matters.

Out of scope, deliberately: guards whose index is loaded from a heap field
(`mult_val[c.op_mult[op]]` — the store-side `val & 15` invariant is a program-level
fact this analysis does not track). The per-site answers there already exist: a
use-site mask (`[x & 15]`, which makes the range locally provable and *earns* elision)
or `unguarded` (the explicit spelling; the opl2 hot path measured both). No
field-invariant tracking as part of this item — conservative-but-complete.

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
instantiation paths), immediately for concrete conditions. Placement (final,
after three iterations): anywhere among a type body's members and any straight-line
statement position in a function body — position is documentation (co-locate with what
the assert protects; contract-first is a stated convention, not an error — FC has no
style diagnostics). A prologue *requirement* for const-param asserts was implemented and
then dropped: it contradicted the co-location utility and promoted style to a compile
error against FC's own philosophy. The one enforced restriction is semantic: no nesting
inside if/match/loop/for/defer/lambda (the assertion is unconditional — no branch
pruning — so nesting would promise conditionality that cannot exist). Spec §Const Parameters → Static assertions; tests
`generics/static_assert_*`.

Open (not blocking):
- Struct literals for const-param structs: `wide { limbs = ... }` cannot infer `'n` from a
  slice-typed field value; construction is via `default(wide<N>)` + mutation or companion
  constructors. Consider size inference from array-literal field values later. (Explicit
  type args on struct literals — `wide<128> { … }` — were rejected 2026-07-19,
  `spec/hist/bugs-2026-07-21.md` §7.3.) 2026-07-22 adequacy/additivity check: the status
  quo constructs everything — `default(T)` is total (zero-filled memory is the default of
  every FC type, bare pointer fields included), so `default(wide<N>)` + mutation and `<'n>`
  companion constructors cover every const-param struct (the std::wideint pattern), and
  `'n` already infers from a *typed* field value (a `w: wide<'n>` field unifies against
  the value's type) — only the shape where `'n` appears solely in size slots lacks a
  literal spelling. Both future avenues stay additive: inferred and explicit args each
  occupy today-error space ("could not infer type variable 'n" / the §7.3 rejection), so
  admitting either later changes no compiling program's meaning — and the parser already
  claims `name<…> { }` in order to reject it (`struct_lit_typearg_scan`), so re-admitting
  the explicit form would flip a claimed parse, not introduce new grammar ambiguity.
- ✅ RESOLVED — Named consts inside *field* size slots (`limbs: u32[cfg.words]`): landed
  with the 2026-07-17 hardening (concrete size slots fold named consts; tests
  `structs/fixed_array_named_const_*`); module-qualified enum counts in size slots fixed
  2026-07-18 (`spec/hist/bugs-2026-07-21.md` §2.10); and the general rule — size slots and
  const-arg slots accept the same constant expressions — settled 2026-07-21 (ibid. §7.7,
  spec §Const arguments).
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
- **Field address-of:** ✅ DECIDED 2026-07-22 — stays `F*`, unchanged. `let` vs `let mut`
  governs exactly one thing: the *root variable* — its reassignability, and therefore
  whole-value address-taking (`&p` yields a pointer whose `*pp = v` is reassignment through
  an alias). One level deep, `let` and `let mut` are semantically identical — `p.field = v`
  and `&p.field` → `F*` are both already legal on a `let` (spec §Address-of,
  `spec/hist/bugs-2026-07-21.md` §7.5). F# school: mutability is a property of the variable,
  never the value; shadowing and lexical scoping are the semantic tools for evolving values,
  and a capture stays a plain value snapshot. The derived rule for this feature: each
  address-of grants through the pointer exactly what its direct spelling allows — `p = v` is
  illegal on `let`, so `&p` yields `const T*`; `p.field = v` is legal, so `&p.field` stays
  `F*`. The whole-binding view being stricter than the field view is accepted (deep const is
  the one tool that forecloses `*pp = v`; the precise remedy for wanting a writable field
  pointer is to spell it: `&p.field`). Existing carve-outs stay (`&s.fixed_array` error,
  packed/bit-field restrictions).
- **Function bindings:** `&f` (C function pointer extraction) keeps its own rule — `let mut` +
  non-capturing (§Address-of). A `const`-qualified C function pointer isn't a meaningful
  interop artifact; decide explicitly that `&f` on a `let` lambda stays an error rather than
  falling through to the new rule.
- **Capturability interaction:** the binding stays capturable (that's half the point), and the
  resulting `const T*` is itself an ordinary pointer value a closure may capture by copy —
  confirm the capture-a-pointer idiom composes (it should: same as capturing any `let` pointer,
  programmer owns the lifetime).

## Module constants are read-only — IMPLEMENTED 2026-07-25; no new keyword

Surfaced 2026-07-24 while stress-testing the const/`let`/`let mut` model against an F#
value-vs-variable lens: a module-level `let config = point{…}` permitted `config.x = 3` from
anywhere, so a "constant" was distinguishable from `let mut` only by rebindability, and its
bytes sat in writable memory for the life of the program. The sketch then was a `const x`
modifier at global scope (Zig/Rust precedent). **Shipped instead: module-level non-function
`let` simply *is* frozen.** Spec §Module constants are read-only; tests
`tests/cases/const/ro_*`.

Three findings decided the shape:

- **Representation-only was not available.** Module-level `let` already requires a
  constant-expression initializer, so "any constant-foldable module binding" is *every*
  module binding — there is no subset to select. And `.rodata` placement while `cfg.x = 3`
  still compiles is a clean build that segfaults: emitting C `const` **is** the decision to
  reject the writes.
- **A keyword would mark what the declaration form already guarantees.** `let mut` was
  already the module-scope spelling for mutable global state (`stdlib/net.fc` —
  `let mut wsa_initialized`), and at global scope it carries no capture penalty. A third
  form bought nothing and would have left plain `let` constants still patchable — adding a
  spelling rather than closing the edge.
- **The break was empirically free.** Nothing in `stdlib/`, `demos/`, or `../wolf-fc` wrote
  a module constant's contents; all three built unchanged. wolf-fc moved 18 tables /
  2016 bytes and 512 constant objects from `.data` to `.rodata`.

The cost owned: §One rule, three knobs gains a module-scope clause. Locals and **file-level**
top-level bindings keep initialize-then-patch — the entry-point file is the script zone
(looser init gate, hoisted into `main`) and stays as permissive as possible. The freeze is
deep through the constant's own storage but **stops at an address it merely holds**, so
`let vga = (u8*) 0xA0000usize` and `u8[] { ptr = …, len = … }` stay writable *through* —
memory-mapped I/O is exactly why. A constant is placed in read-only memory only when every
byte it occupies is compiler-emitted; that judgment is per declaration, so a constant mixing
a table with a raw address is read-only in its own storage without its backing being frozen
(conservative and complete, rather than proving which subobject an access path reaches).

Incidental fix: qualified reassignment of a module member (`m.n = 5`) was never rejected —
the immutable-binding check only inspected bare-identifier targets — so a module-level `let`
was silently rebindable through its qualified name. The read-only root check closes it.

## BUG: `alloc`/`alloca` of a module-qualified *value* is misread as a type — FIXED 2026-07-25

Found 2026-07-25 while stress-testing module constants; **pre-existing** (reproduces
identically on the 2026-07-21 build), and unrelated to the read-only change. Fixed the same
day, together with a second crash the investigation surfaced (sizeless `alloca(T)`, below).

`alloc(expr)`/`alloca(expr)` decide between the `alloc(T)` and `alloc(expr)` readings in the
**parser**, on syntax alone (`parser.c`, the `try_type` branch): a leading identifier
followed by `.` is committed to `parse_type` as a possible module-qualified type name, and
whenever that parse reaches `)` the type reading wins. Nothing afterwards checks that the
dotted name actually *denotes* a type, and the saved backtrack position is never used on
that path. A module-qualified **value** therefore silently becomes `alloc(T)`:

```fc
module k =
    let p = point { x = 3, y = 4 }
    let mut q = point { x = 5, y = 6 }

let hp = alloc(k.p)!     // emits calloc(1, sizeof(point)) — k.p is never read
let hq = alloca(k.q)     // segfaults the compiler
```

Two failure modes, both bad: `alloc` compiles clean and produces a zero-filled object (exit
0 with broken output), and `alloca` crashes the compiler. The bare-identifier twin is already
correct — `alloc(p)` on a local reports "alloc(expr) requires a literal or slice expression"
— so only the dotted form is affected.

This was the *semantic questions get semantic answers* rule (CLAUDE.md) unmet. **Resolution:**
rather than a token pre-pass, the dotted form was made to behave exactly like its
bare-identifier twin, which was already correct — the parser defers the ambiguous case and
pass2 answers it with real name resolution:

- **parser.c** (`TOK_ALLOC`/`TOK_ALLOCA`) applies the bare-identifier rule to dotted names:
  `[` and `,` still force the type reading (no `alloc(expr)` form starts that way), while a
  name followed by `)` stays an expression. `try_type`/backtracking is retained for the other
  tails (`<`).
- **pass2.c** settles it once at the head of `case EXPR_ALLOC`, before the stack/heap split so
  both operators share the judgment: flatten the `EXPR_FIELD` chain (`expr_dotted_name`,
  arena-backed — a fixed buffer would truncate silently into the wrong symbol), and take the
  type reading only when `resolve_dotted_name` yields a `DECL_STRUCT`/`DECL_UNION`/`DECL_ENUM`.
  `resolve_dotted_name` is non-erroring, so a value name falls through to the expression path.
  A local binding shadowing the module is checked first, matching the bare case's local-first
  order (the old heuristic got this wrong too: `let m = 5` did not stop `alloc(m.s)` from
  resolving module `m`'s type).

Two behaviors improved beyond the reported bug: `alloc(m.union.variant)` — an ordinary
expression the old heuristic reported as `unknown type name 'shapes.tag.b'` — now works, and
`alloc(m.undefined)` reports `module 'm' has no member` instead of `unknown type name`.

Tests: `memory/alloc_module_{type,variant}` (happy paths across struct/union/enum, nested
module paths, raw/slice/alloca forms) and `memory/{alloc,alloca}_module_{,mut_}value_err` +
`memory/alloc_module_shadowed_err`.

### Sub-bug found while fixing: sizeless `alloca(T)` crashed codegen — FIXED 2026-07-25

Verifying that `alloca(shapes.point)` "must keep working" showed it never did: **every**
spelling of the sizeless form crashed the compiler (`alloca(i32)`, `alloca(m.point)`,
`alloca('a)` — SIGSEGV in `emit_expr` on a NULL init expression). pass2 typed `alloca(T) → T*`
but codegen had no case for it, so the form existed only as a crash.

It is not a language form: §Dynamic stack allocation lists exactly four shapes
(`alloca(T, n)`, `alloca(T[n] { })`, an interpolation, a `(cstr)` cast) and states the reason —
`alloca` exists so that *runtime*-sized stack allocation is deliberate and visible. A
fixed-size stack object is what an ordinary `let` binding already is. So pass2 now rejects the
sizeless form instead of typing it; the bare-identifier spelling `alloca(point)` already
reported this via the `alloca(expr)` path, and the type spellings now agree. Tests
`memory/alloca_{bare,module}_type_err`.

## Fixed-buffer name/path truncation swept out of the compiler — FIXED 2026-07-25

Noted while fixing the `alloc`/`alloca` bug above: `parse_type` assembled a module-qualified
type name with `snprintf` into a `char buf[512]`. A sweep of `src/` found the same shape at
every layer, and it is a genuinely nasty failure mode rather than a cosmetic one — **a
truncated name is not invalid, it is a different valid name**. `snprintf` reports the cut only
in a return value that name-building code drops, and nothing downstream can tell.

Confirmed wrong against a build of the previous commit:

- **Module-qualified type names** (`parse_type`, and the struct-literal spelling in
  `parse_prefix`): a dotted name over ~500 chars became `unknown type name`.
- **Namespace paths** (`namespace a::b`, `from a::b::`): two namespaces agreeing in their
  first 510 characters mangled to one prefix and their same-named modules **merged** —
  invisibly, since both `from` clauses clipped identically and still resolved.
- **Numeric literals**: the digit string was copied into `char buf[72]` / `char buf[128]`
  before `strtoull`/`strtod`. The clipped prefix parses cleanly and sets no `ERANGE`, so
  `0x<90 zeros>42` silently evaluated to **0**, and a float clipped before its exponent to
  **0.0** with no underflow flag.
- **Instantiation descriptors** (`fmt_generic_inst`, mono's static_assert message,
  `gen_inst_diag`): long template names lost their `<args>` suffix and the message after it.
  `fmt_generic_inst`'s output also *keys the memo* that stops the generic-validation descent
  from re-walking an instantiation, so two instances clipping to the same text deduped to one.
- **`type_name()`**: rotating `char[256]` slots both truncated and — because each slot was
  claimed *before* its operands were formatted into it — let a nested call scribble on the
  partially-built name of the caller that invoked it (`pair<a<i32>, b<i32>, c<i32>, d<i32>>`
  wrapped the ring mid-accumulation). The slots now own exactly-sized heap strings and are
  published only once complete.
- **LSP**: go-to-definition's `file://` URI, the `@lsp.rsp` token, and the directory
  buffers behind unit keying and sibling discovery. The last of those silently split one
  project into per-file units past the cap.

`common.h` now provides `str_sprintf`/`str_vsprintf`/`arena_sprintf`/`str_appendf`/
`intern_sprintf`; the rule and the "bounded by construction" exemption are recorded in
CLAUDE.md. Buffers holding only compiler-generated text (`_fc_back_%d`, a `%g` rendering, a
mangling tag) were deliberately left alone.

Oracle: the emitted C is **byte-identical** to the previous commit on 1709 of 1710 single-file
test cases; the one difference is the new literal test, where the numbers are now right.
Tests `expressions/long_numeric_literals`, `modules/long_qualified_type`,
`modules/long_namespace_path`, `generics/static_assert_long_name_fail`,
`generics/generic_chain_long_name_err` — each verified to fail against the previous commit.

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

## BUG: `const T[N] { … }` unusable in every annotated position — FIXED 2026-08-20

Found 2026-08-19 while writing a `str[]` of playlist titles in
`demos/fuzzel-fobble/main.fc`, which worked around it with a plain `str[]` and
`text.copy` — a heap copy of static data to satisfy the type system.

§Slices & Strings → Allocation specifies both the form and the reason it exists:

> The element type may also be `const`-qualified — `const str[3] { "a", "bc", "def" }` is
> the spelling a slice of string literals takes, since a string literal is a `const str`
> and does not narrow to `str`.

The literal parsed and emitted, but could not be passed anywhere `const str[]` was
written — reporting `expected const str[], got const str[]`.

**Two types, one spelling.** `const` in an *annotation* is a prefix over the whole
type that follows (`parse_type`), so `const str[]` sets `is_const` on the **slice**.
A slice-literal head names an **element** type, so `const str[3] { … }` builds a
non-const slice whose **element** is const. Both are real, both are useful, and
`type_name` rendered both `const str[]` — which is where the unreadable diagnostic
came from.

**Resolution: keep both types; they were already in the language.** `const` in FC is a
view qualifier that attaches only to a reference type (`apply_const` rejects value
types), and reference types nest — a writable struct with a `name: const str` field is
already exactly "writable slot, read-only view", and `(const str)[]` is its slice twin.
Collapsing them would make slices the one container that erases a view nested inside
it, and would delete the only sound way to build a table of string literals. The
annotation spelling `(const str)[]` already parsed; what was missing was a name in
diagnostics, the widen, and the codegen to back it.

The three types form a lattice, and the edge that was missing is the safe one:

| Type | Slots | An element reads as | Widens to |
| --- | --- | --- | --- |
| `str[]` | writable | `str` | `const str[]` |
| `(const str)[]` | writable | `const str` | `const str[]` |
| `const str[]` | read-only | `const str` | — |

`str[] → (const str)[]` stays rejected: it is C's `char** → const char**` hole.

- **types.c `type_name`** parenthesizes a const inner under a `*`, `[]`, or `[N]`
  suffix, so the two print as `(const str)[]` and `const str[]` (and a pointer to a
  read-only view no longer prints `const const str*`). Options and results need no
  parens — `const` distributes into them, so `const str?` *is* the option of `const str`.
- **types.c `widen_repr_preserving`** lets a container's const-add absorb the element's
  own const (`elem_const_absorbed`). Adding const to the container is what permits
  dropping it from the element: the target forbids the slot write and deep const hands
  the element back const on every load, so it grants strictly less.
- **pass2.c `unify`** applies the same rule during generic inference (`absorbed_elem`),
  so a generic callee accepts what its non-generic twin accepts — without it,
  `(const i32*)[]` failed to bind `'a` against `const 'a*[]`.
- **pass2.c dereference** now propagates deep const: `*p` on a `const str*` is a
  `const str`. §Deep const already said "every pointer dereference in the chain
  preserves const" — indexing a `const T[]` did it, deref did not — and the pointer
  half of the widen is sound only because of it.
- **parser.c `alloc`** accepts a `const` target, so `alloc(const str[n] { })` reaches
  the runtime-length path instead of being parsed as a nested slice literal (which
  requires a compile-time length). This was the third reported symptom.
- **parser.c cast** admits a parenthesized element type: `((const T)[]) x`. `const`
  never starts an expression, so `((const` can only be a type; every other `((` stays
  an expression and is not probed.

**Pre-existing codegen bugs surfaced by the fix** (both reproduce on the previous build
through the `(const str[])` cast the entry itself documented as the workaround):

- **The C slice struct disagreed with its own name.** `emit_type_ident` never prints a
  qualifier, so `(const i32*)[]` and `const i32*[]` share `fc_slice_int32_t_ptr` — but
  the struct *body* spelled the element with `emit_type`, so whichever form was emitted
  first decided whether `ptr` was `const int32_t**`, and the other one's backing array
  no longer matched (`-Werror=incompatible-pointer-types`). Every position that spells
  a slice's element in C now goes through `emit_elem_type`, which drops the top-level
  const: the member, a literal's backing array, an alloca'd or calloc'd one. Nothing is
  lost — FC enforces const itself, and the C projection cannot model what FC means
  anyway, since the *read-only* slice is the one whose member comes out non-const.
  Stores into that storage strip the qualifier explicitly (`emit_elem_store_cast`), so
  no write goes through a differently-qualified lvalue.
- **A const pointer to a pointer emitted the wrong C type.** `const i32**` (a read-only
  pointer to an `i32*`) was spelled west as `const int32_t**`, which reads in C as
  "pointer to pointer to const int" — permitting the `*p = q` FC rejects and rejecting
  the `int32_t**` argument FC accepts. `emit_type` now spells pointer const east
  (`int32_t* const*`); for a value pointee `int32_t const*` is just `const int32_t*`
  the other way round.

Also emitted: a cast that only moves `const` around is now dropped for pointers as it
already was for slices — C performs that qualification change implicitly at every
assignment and argument.

Tests: `const/elem_const_slice` (both types named, slot write, both widens, element
reads back const), `elem_const_ptr_slice` and `elem_const_ptr_widen` (the non-`str`
twin, and the pointer-level widen), `elem_const_slice_alloc` (runtime-length heap and
stack tables, filled after allocation), `elem_const_option_widen`,
`elem_const_struct_field`, `elem_const_slice_uses` (for-loops, subslices, index),
`elem_const_slice_cast` (both cast directions, and `((a + b))` still an expression),
`elem_const_generic` / `elem_const_generic_err` (inference absorbs the element const
only where the container adds one), `elem_const_slice_widen_err` (the `char**` hole
stays closed), `elem_const_slice_strip_err` (the diagnostic that used to read
`expected const str[], got const str[]`), `elem_const_slice_write_err`,
`const/deref_deep_const{,_err}`, and `const/cast_strip_ptr` — a gap the work exposed:
`const/cast_strip` covered only the *slice* strip, whose C type is unchanged either
way, so nothing held the pointer strip's cast in place. `spec/examples.fc` shows the
pair under Const.

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
  `spec/hist/archived-todos.md`) left `diag_fatal`s in `parser.c` (17 as of
  2026-07-21; the bugsearch mangling fixes added the extern-reserved-root checks) for
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
