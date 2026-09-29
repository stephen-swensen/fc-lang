# TODO

Open items for the FC compiler and specification. Finished work, including the full
history behind the follow-ups below, is in `spec/hist/archived-todos.md`.

---

## Slice length representation: follow-ups

`--len-repr` itself is done (spec §Length representation). Full record in the archive
under "Slice length representation `--len-repr` — IMPLEMENTED 2026-08-22; follow-ups open".

**As-if narrowing of range-form `for` counters.** `for i in 0..s.len` binds a
user-visible `i64` and emits an `int64_t` counter, the main remaining 64-bit cost in
idiomatic loops on 16-bit targets (element-form counters already run at `fc_len_t`).
Semantics are deterministic, so the emitter may narrow the counter whenever all uses
provably fit (endpoints bounded by a stored len or by narrow constants) — pure as-if,
no spec change (§Length representation already grants the latitude). Do it only after
real gcc-ia16/djgpp measurements show it matters, and check first what the C optimizer
already recovers (see the guard-elision note below).

**The freestanding profile (Lane 1 gates).** The dependency half of small-target
support: what the emitted C assumes about its runtime (stdio-printing guards, malloc,
snprintf, …) and how each assumption becomes a hook or a compile-error gate. Planned in
**`spec/freestanding.md`** — eight items (`fc_trap` keystone → allocator hook →
float/atomics/backtraces gates → freestanding interpolation formatter → stdlib layering
→ `--profile <name>` bundles that imply a `--len-repr`, gcc `-O2`-style; `--len-repr`
stays the single primitive knob). Per-target needs and ordering live there.

## Not doing: as-if elision of provably-dead bounds guards

Tried and reverted: a pass2 value-range analysis that omitted bounds guards it could
prove never fire. On wolf-fc, gcc and clang value-range propagation already removed about
98% of the same guards (four enum-indexed table accesses were left for pass2), and the
gcc -O3 build got slower. A wrongly elided guard is a silent out-of-bounds, so ~800 lines
of interval analysis for four guards is a poor trade. Reopen only against a measurement
on a C compiler without range propagation (Watcom/Borland class). Full record in the
archive under "As-if elision of provably-dead bounds guards — TRIED AND REVERTED 2026-09-05".

## Guarded-access emission shape

A codegen-only change found by the guard-elision experiment, with no analysis and no
soundness exposure. The guarded index access copies the whole slice header into a
temporary — `T _s = obj; int64_t _i = idx; if (…) fc_oob(…); _s.ptr + _i` — even when
`obj` and `idx` are pure (a local, a parameter, a field path, a literal). Clang does not
see through that copy in wolf-fc's per-column raycaster loop (`bd_*[k]`): the direct form
was worth 0.198 → 0.148 ms per frame at -O3. Try: when both operands are pure
(`expr_has_side_effects` false), check against `obj.len` and index `obj.ptr[idx]` (or the
fixed-array field / literal backing array directly) with no header copy, keeping the
guard. Keep it only if it wins or holds on *both* gcc and clang: the direct
fixed-array-field form measured 2–5% slower on gcc -O2/-O3 in the OPL2 emulator
(`c->op_stage[i]`-style accesses; mechanism not pinned), so measure that path
specifically (`wolf-fc --test audiobench:30`, interleaved old/new runs, equal N — the
song gets denser with N). Byte-identical C is not the oracle here; wolf-fc's golden suite
(`make check` there) plus the perf numbers are.

## Const generics: follow-ups

Const generics and `static_assert` are done (spec §Const Parameters). Full record in the
archive under "Const generics (value parameters) — IMPLEMENTED 2026-07-17 …".

- **Struct literals for const-param structs.** `wide { limbs = ... }` cannot infer `'n`
  from a slice-typed field value, so construction goes through `default(wide<N>)` plus
  mutation, or `<'n>` companion constructors (the std::wideint pattern). That covers
  every const-param struct: `default(T)` is total, and `'n` already infers from a *typed*
  field value (a `w: wide<'n>` field unifies against the value's type). Only the shape
  where `'n` appears solely in size slots lacks a literal spelling. Explicit type args on
  struct literals (`wide<128> { … }`) are rejected (`spec/hist/bugs-2026-07-21.md` §7.3).
  Both ways to add a literal later — inferring `'n` from array-literal field values, or
  admitting explicit args — are additive: each occupies today-error space ("could not
  infer type variable 'n" / the §7.3 rejection), and the parser already claims
  `name<…> { }` in order to reject it (`struct_lit_typearg_scan`), so re-admitting the
  explicit form flips a claimed parse rather than adding grammar ambiguity.
- **LSP hover for const params.** Hover shows `wide<256>` via `type_name`, but `'n`
  hovers as `i32`; there are no dedicated const-param hover docs.

## std::wideint: follow-ups

std::wideint is done as the const-generic `uwide<'n>`/`iwide<'n>` (spec §std::wideint).
Full record in the archive under "std::wideint (né fixint) wide integers — …".

- **Cross-width conversions**: widening (`uwide<128>`→`uwide<256>`), truncating, and
  signed↔unsigned reinterpretation at the same width. Today the only cross-width paths
  are `mul_wide` or a heap round-trip through `to_hex`/`parse_hex`. With const generics
  each can be written once: a generic widen from `uwide<'m>` to `uwide<'n>` needs `'n`
  inferred from the call context or named explicitly (`uwide<256>.widen_from(a)`).
- **Division core**: `limbs_divmod` (`stdlib/wideint.fc`) is binary long division,
  O(bits) iterations — correct and simple. A Knuth-D core could replace it without
  touching any caller if wide division ever becomes hot.

## Enums: follow-ups

Enums are done (spec §Enums). Full record in the archive under "Enum declarations —
IMPLEMENTED 2026-07-10".

- **Reflection-lite**: `enum_name(e)` (a static name table, paid for only when used —
  the `error_name` design) and variant iteration; wanted for logging and CLI/config
  parsing.
- **`extern enum` verification**: an FC-side redeclaration of a C enum with emitted
  `_Static_assert`s against the C header's values. Until then, C-owned sets stay
  extern-constant modules.

Bit flags stay integers by design (a flag combination is outside any closed set).
Bit-precise register fields belong to a packed-struct feature (see "Direct hardware
access" below), not to enums.

---

## Atomic pointer publication — `T*` / `any*` pointees for the atomic builtins

`atomic_load_acquire` / `atomic_store_release` (archive: "Concurrency primitives") accept
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

## Discarded pure value as a no-op error — extend the self-assignment rule

A `loop` arm meant to exit with a value but written without `break` — `| 0 -> product` in
a hand-written `factorial` — silently discards the value. A loop's value comes only from
`break v` (the `EXPR_LOOP` case in `pass2.c`), so the loop types as `void` and the
function infers `-> void`. The compiler catches it only at the *use* site (`cannot bind
void expression to 'f'`). At the definition it is silent, because a breakless `loop` is
the intended spelling of an infinite loop (see the self-recursion check, `sr_flow` in
`pass2.c`) and a void-returning function is legal. A "did you forget `break`?" heuristic
is not wanted: that is warning-shaped, and FC has exactly one diagnostic severity
(`docs/ARCHITECTURE.md` → Invariants → Errors).

There is, however, a *sound* error consistent with FC's existing precedent: **self-assignment
`x = x` is an error because it is provably a no-op** (`pass2.c`, "self-assignment of 'x' has no
effect"). A **discarded, statically-effect-free value** is the same category — `| 0 -> product`
computes a value and throws it away. Today discards are legal C-style for any non-result type
(only `T!` results are guarded, by `check_result_ignore` in `pass2.c`); the proposal narrows
that to reject discards that are *provably pure*, so the mistake fails the build while
side-effecting discards (a byte count, a call) stay legal.

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
  written `void()` (the one no-op spelling — *not* bare `()`); the message should name it:
  "value computed but discarded; use `void()` for an intentional no-op arm, or
  `break`/return it." Mirror the self-assignment wording. Decide whether `ignore expr` (today
  the explicit ignore for results) also silences it.

Blast radius is a new error class over shared pass2 code — validate across the full suite +
stdlib + the sibling euler-fc before trusting the false-positive rate. Rare mistake, fiddly
win: backlog, not blocking. (The `never`/bottom-type alternative — typing a breakless loop
`never` à la Rust `!` / Zig `noreturn` — is a much larger type-system change and, absent
unreachable-code detection, would make this *more* silent, not less; not pursued.)

---

## Direct hardware access — `volatile`, inline asm, packed layouts

FC's model of the machine is otherwise explicit (exact-width ints, defined two's-complement
wrap, `sizeof`/`alignof`, `bitcast`, unchecked pointer arithmetic, acquire/release atomics),
but the three tools for touching hardware *directly* are reachable only through C interop
today. For the retro/embedded niche (`spec/niche.md`) — MMIO registers, framebuffers,
interrupt handlers, port I/O — these are the first gaps a driver-level program hits. All
three are open design questions; lay out the C/Rust/Zig precedents before deciding.

- **`volatile`** — no FC spelling exists. Today MMIO goes through an `extern` C helper or a
  C-side `volatile` declaration, and a plain FC load/store through a held address may be
  merged, hoisted, or elided by the C optimizer. Questions: type qualifier (C/Zig
  `volatile T*`, composing with the existing `const` pointer machinery and its
  `type_ident_eq` typedef-naming rules) vs. access intrinsics (Rust
  `read_volatile`/`write_volatile` — no new type axis, every access spelled at the use site);
  interaction with the module-constant freeze (which already stops at a held address) and
  with escape analysis/provenance.
- **Inline assembly** — none. Needed for port I/O (`in`/`out`), `cli`/`sti`, CPU-specific
  instructions, and ISR prologues. Questions: GCC extended-asm passthrough (operands,
  clobbers — ties FC to the gcc/clang dialect, which djgpp shares) vs. a narrower intrinsic
  set; how operands name FC bindings given `_l_<name>_<id>` local mangling; whether it can
  appear in `unguarded`-style lexical form only; portability across the C toolchains FC
  targets (`spec/niche.md` dialect audit).
- **Packed layouts / bit fields** — FC has no native `packed struct` and no bit fields; both
  work only by declaring the type in a C header and mirroring it as an `extern struct`
  (spec §Packed extern structs). Questions: a native `packed` modifier (the `&field`
  restriction would move from the C compiler's `-Waddress-of-packed-member` to FC), native
  bit-field declarations vs. leaving register fields to shift/mask code (the enum notes
  above already route bit-precise register fields here, not to enums), and whether FC should
  state struct field order/padding normatively rather than inheriting it from C.

---

## Editor / LSP server (`fcc --lsp`)

The server's architecture is described in `docs/ARCHITECTURE.md` → "Language server";
this section is the open-item backlog. None of these block release.

- **Completion ignores source order inside a function.** `complete_scope` (`src/lsp.c`)
  offers the enclosing module's members and imports, the enclosing function's locals, file
  imports and top-level names. Inside a function body, `harvest_expr` collects every local
  regardless of source order, so a `let` declared textually *after* the cursor is still
  offered. A line filter on the harvest closes it. Low value (a name you're about to type
  showing up a few lines early is mild).
- **Variant-constructor go-to-definition granularity** — lands on the union declaration,
  not the specific variant. Deliberate today, but `UnionVariant.loc` is recorded (hover
  already reads it for the variant's doc comment via `variant_decl_loc`), so refining it
  is a small change plus a wire-test update.
- **Stdlib AST re-parse per keystroke** — unchanged feed sources (stdlib + `lsp.rsp`
  files) are lex-cached across analyses and edit bursts coalesce into one analysis, but
  `analyze()` still re-*parses* the cached tokens into a fresh AST each time. Caching the
  parsed AST (token→AST) is the remaining, lower-value win now that lexing — the dominant
  cost — is cached.
- **Install targets are Linux-only** — `make install` / `install-vscode` assume a Linux
  layout; Windows/macOS packaging is unwritten.
