# Archived TODO Items

Resolved design decisions and implementation history moved out of TODO.md. The first
batch moved on 2026-03-27; later entries were added at the top, newest first, and each
later batch starts with a line giving its move date.

---

Moved from TODO.md on 2026-09-29.

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

## As-if elision of provably-dead bounds guards — TRIED AND REVERTED 2026-09-05

Sibling of the for-counter narrowing follow-up above — same as-if family, same
range-analysis machinery. Purely an optimization by definition: a guard may be omitted
only when pass2 proves it can never fire, so observable behavior (including which abort a
program hits) is unchanged and no spec change is needed.

Implemented in full on 2026-09-05 (value-range lattice over the typed AST — literals,
`& mask`, `%`, range-form `for` variables, if/match refinement, widening casts; constant
length from fixed-array fields, slice literals, and module `let mut` slices never
reassigned program-wide; frozen module slice headers folded onto their backing arrays)
and measured against the C optimizer on wolf-fc before being reverted uncommitted,
because the numbers said the analysis duplicates the C compiler:

- Guards surviving the C optimizer, old C → new C: gcc 13 -O1 522 → 512, -O2 522 → 518,
  -O3 617 → 610; clang 18 -O2 662 → 657 (textual sites 1009 → 803). Value-range
  propagation already proves ~98% of the same guards — including the module `let mut`
  never-reassigned case this item called "the case no C compiler can ever recover":
  gcc treats a never-written, non-address-taken static as read-only (ipa-reference), so
  it recovers exactly that fact from the single emitted C file. The residue only pass2
  proves is FC-level type knowledge: enum-typed indices into module tables
  (`fire_rate[g.weapon]`), four sites in wolf-fc.
- Runtime at -O0: frame 1.80 → 1.21 ms, raycaster 1.44 → 0.87 ms, OPL2 474 → 357
  ns/sample. gcc -O1/-O3 raycaster: unchanged. gcc -O2/-O3 OPL2: 2–5% *slower*, traced
  (guard-stripping control on the old C) to the direct `c->field[i]` emission form, not
  to the missing checks. clang -O3 raycaster 0.198 → 0.148 ms — also emission form
  (clang's surviving guard set was identical): dropping the slice-header copy the guarded
  statement expression makes around every access.

Verdict: on any compiler with value-range propagation the analysis is redundant, and the
shipping gcc -O3 build got slower. A wrongly elided guard is a silent out-of-bounds in
code the programmer believes is checked; ~800 lines of interval analysis plus a
whole-program mutation flag that every future global-mutation path must stamp is a poor
trade for four guards. A to-C compiler's leverage is the *shape* of the emitted C, not
re-proving what the C optimizer proves (next item). Reopen only against a real
measurement on a compiler without range propagation (Watcom/Borland class — the DOS build
goes through djgpp, which is gcc and already covered); the -O0 numbers above are the
proxy for what such a target would gain.

The out-of-scope boundary still holds if reopened: guards whose index is loaded from a
heap field (`mult_val[c.op_mult[op]]`) — the per-site answers are a use-site mask
(`[x & 15]`) or `unguarded`; no field-invariant tracking.

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

## `&` on `let` bindings yielding `const T*` — read-only address-of

*Status when archived (2026-09-29): implemented 2026-07-24; the TODO entry had not
been marked. Spec §Address-of states the rule, the `&f`-on-`let` exception and
address-of through a const path; tests include `const/ro_addr_of` and
`closures/addr_of_let_capture`; the implementation record is
`spec/hist/readonly-address-of-plan.md`.*

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

## Editor / LSP server — residual parser `diag_fatal` sites (resolved)

*Status when archived (2026-09-29): resolved by the maintainability plan's phase 3
(item B14), which turned the parser's remaining `diag_fatal` calls into `diag_error`
plus an error node; `src/parser.c` has none left. The backlog entry as it stood:*

- **Residual parser `diag_fatal` sites** — error-recovery parsing (done — see
  `spec/hist/archived-todos.md`) left `diag_fatal`s in `parser.c` (17 as of
  2026-07-21; the bugsearch mangling fixes added the extern-reserved-root checks) for
  validation *inside* matched delimiters (slice/tuple/`alloc`/`cstr[N]` literals,
  fixed-array size), string interpolation, the inline `let…in` form, and the
  extern-reserved-name check. These still abort the analysis (LSP falls back to
  `last_good`, no crash); converting them to `diag_error` + a best-effort node
  would close the last in-band recovery gaps. Low value (rare mid-typing).

---

Entries below were archived before 2026-09-29.

## `text.parse_*` trailing-garbage acceptance (resolved 2026-07-14 — whole-string strict)

`parse_i32("99xyz")` returned `ok(99)`: the parsers followed strtoll's leading-token
contract, silently skipping leading whitespace and accepting trailing garbage (surfaced
2026-07-08 when furl accepted the URL port `99xyz` as 99; re-raised by the std::wideint code
review, whose `wideint.parse` shipped whole-string strict). Decision: adopt the Rust/Zig
school across the stdlib — **every byte of the input must belong to the number**, else
`parse.invalid`; no lenient form. Implementation in `stdlib/text.fc`: a shared
`strict_reject` pre-check rejects the cases the C parsers absorb silently (empty input,
leading whitespace, embedded NUL — which truncates the cstr copy), and an
endptr-at-the-terminator check after the `strto*` call catches every other trailing
character. An explicit `+` sign stays accepted (part of the validated grammar, Rust
precedent); floats keep C's full grammar (exponent, hex-float, inf/nan) under whole-string
consumption. `text.trim` is the documented escape for whitespace-carrying input. The three
demo high-score loaders were the only lenient-dependent callers (they parsed an entire
zero-padded 32-byte buffer — a latent bug the strict contract exposed); fixed to slice to
bytes-read + trim. wolf-fc needed no changes (208/208 green — it parses clean sliced
tokens). Spec §std::text updated; strict-matrix tests added to `stdlib/text_parse`.

## Bit reinterpretation — `bitcast(T, x)` builtin (resolved 2026-07-04)

Closed the design-audit "bit reinterpretation" gap (`spec/design-audit-2026-07-rc6.md`):
inspecting a value's raw bits across the int/float boundary (hashing an `f32`, serializing
a float, picking apart sign/exponent/mantissa) had no first-class form. The spec's old
workaround — `*(u32*)&f` on a `let mut` temporary — was both noisy and **strict-aliasing UB**
that `-O2` may miscompile.

Shipped as a built-in `bitcast(T, x)`, wired exactly like `sizeof`/`alignof` (`TOK_BITCAST`,
`EXPR_BITCAST` with a `Type *target` + `Expr *operand`; parser reads a type arg then a
comma + value). Design (settled in conversation the same day):

- **Scalars only, equal size.** Both sides must be fixed-width scalars — a fixed-width
  integer, float, or `char` — of equal byte width (`bitcast_scalar_bytes` in `src/pass2.c`
  encodes eligibility + width in one helper). A size mismatch or an ineligible type is a
  **compile error**, not a runtime check.
- **Excludes `isize`/`usize`** (target-defined width would make the size match
  target-dependent) **and `bool`** (a byte other than 0/1 is not a valid bool, so bitcasting
  *to* bool could fabricate an invalid value). Every accepted type has the property that all
  bit patterns of its width are valid — which makes `bitcast` **statically total**: no
  runtime failure mode, hence **no `checked`/`unguarded` variant** (it sits on neither the
  guard nor the overflow axis; the distinguishing case vs. the value cast `(u32) f`, which
  *does* carry a saturation guard).
- **Lowering:** a C11 union compound-literal type-pun
  (`(((union { F from; T to; }){ .from = x }).to)`) — the *defined* pun (unlike the pointer
  cast), which GCC/clang fold to a register move. No `memcpy`/header dependency; no bare
  `int`, so it is int-width-agnostic for 16-bit targets. Verified defined at `-O2
  -fstrict-aliasing` on gcc+clang.
- **Aggregates punted** (padding-indeterminacy hazard). The motivating aggregate uses are
  covered without a value bitcast: pointer overlay (`(header*) buf.ptr`) for buffers,
  explicit shift-packing for struct→int. Revisit behind a no-padding check only if a
  concrete need appears.

Walk sites updated across pass1/pass2/monomorph/codegen/lsp (mirroring `EXPR_CAST`); a
`bitcast` `BUILTIN_DOCS` hover entry + keyword-completion entry added. Spec: §Casts points
to a new §bitcast; `spec/examples.fc` gains a demo. Tests in `tests/cases/casts_widening/`:
`bitcast_float_int`, `bitcast_int_reinterpret`, `bitcast_special_floats`, plus six `.error`
cases (size mismatch, bool source/target, `usize`, pointer, slice). A type-var operand in a
generic body is cleanly rejected (not supported; not a crash). 1762 tests green gcc+clang.

## Latent bug — lambdas with type-var types in generic bodies (resolved 2026-07-03)

Discovered 2026-07-03 while implementing heap closures. A lambda inside a generic function
that captured a binding (or declared a param) whose type involved the enclosing type
variable compiled without diagnostics but emitted broken C
(`typedef struct { /* TODO: type 22 */ x; } _ctx__fn_N;`) — lifted lambdas were collected
and emitted **once**, with no substitution context.

**Resolved by per-instantiation lambda emission** (the complete option, not the
reject-at-validation interim). In `src/codegen.c`: lambdas are collected per mono instance
(from the template body) instead of from generic decls; a `g_lambda_suffix` global —
active alongside `g_subst` during all generic-instance emission (ctx structs, forward
decls, definitions, mono function bodies, trampoline collection, backtraces symmap) —
makes `lambda_c_name()` mangle every lifted name to `_fn_N__<instance>` so each
instantiation gets its own correctly-typed lambda copy and `_ctx_` struct. Concrete-typed
lambdas in generic bodies are now also duplicated per instance (static, DCE'd — same cost
model as the rest of monomorphization).

A same-shaped adjacent hole closed at the same time, in pass2: a lambda whose param types
contain a type variable **not bound by an enclosing generic function** (e.g.
`let id = (v: 'a) -> v` inside `main`, or `(v: 'b) -> v` inside a generic over `'a`) also
compiled to broken C — lambdas are never generic templates, so this is now a compile
error ("type variable %s is not bound by an enclosing generic function"), as is an
explicit `<'t>` prefix on a lambda. `CheckCtx.active_type_vars` carries the enclosing
top-level function's bound vars (param vars + explicit `<>` vars) through the body so the
legal uses stay legal. Spec §Generic Functions states the rule; tests
`generics/lambda_capture_typevar*`, `generics/lambda_typevar_param`,
`generics/lambda_*_err`, `closures/heap_closure_typevar_capture`.

---

## Error-recovery parsing + ungated pass2 (resolved 2026-06-28)

Two paired LSP follow-ups, implemented together because each enables the other's payoff.
The throughline (from rust-analyzer/Clang/Roslyn): the front end should be **resilient** —
always return a spanned tree plus a side list of errors, never abort — so the *fresh*
analysis stays usable on the well-formed parts of a file the user is mid-editing.

**1. Error-recovery (panic-mode) parsing.** The parser's `expect`/`expect_typearg_gt`/
`expect_extern_c_name` were `diag_fatal` (→ `longjmp` in the server, `exit` in the CLI), so
any syntax error discarded the whole AST. They are now **non-fatal**: report via `diag_error`
and return the current token without consuming, so an enclosing loop can resynchronize.
Mechanics (all in `src/parser.c`):
- New AST placeholders `EXPR_ERROR` / `PAT_ERROR` / `DECL_ERROR` (`ast.h`), carrying only
  kind+loc. pass2 types `EXPR_ERROR` as `type_error()` silently (fixing the fatal `default:`
  in `check_expr_inner`); `PAT_ERROR` is treated like a wildcard; error nodes never reach
  codegen (a `-Wswitch` sweep + an `assert(0)` tripwire in `emit_expr` enforce this).
- A **leaf-bump rule** (`parse_prefix` / `parse_pattern_atom` defaults consume the offending
  token unless it's a hard stop) plus a **no-progress watchdog** (`recover_progress`) in every
  item loop guarantee forward progress — the LSP can never hang.
- **Scope-aware recovery** (`recover_to` + `DECL_START` sync set, Dragon-Book hierarchical
  anchoring) in `parse_program` / `parse_module_decl`; block bodies resync on layout
  (NEWLINE/DEDENT) via leaf-bump + watchdog. Match arms handle `PAT_ERROR`/guard/unterminated-
  or-pattern by resyncing to the next `|` without corrupting the or-pattern buffer.
- The **CLI now reports all syntax errors in one run** (a post-parse gate in `main.c` reports
  and exits non-zero before pass1, so error nodes never reach codegen). The lexer/layout
  errors (tabs, unterminated string/comment, inconsistent indentation) **stay fatal** — the
  `setjmp`/`longjmp` backstop in `analyze()` remains the single unrecoverable path (recovering
  the indentation stack mid-stream was deliberately out of scope).

**2. Ungated pass2.** `analyze.c` and `main.c` gated `pass2_check` behind
`diag_error_count()==0`, so one recoverable pass1 error (a duplicate name in a *merged*
sibling, an unresolvable type) blanked every node's type. pass2 already accumulates with
`diag_error` and propagates the `TYPE_ERROR` poison, so the gate is simply removed: pass2 now
runs past recoverable parse/pass1 errors. The LSP keeps hover/definition/CodeLens **live on
the well-formed lines** while a broken line is typed, and a broken merged sibling no longer
blanks the open file. The CLI surfaces name and type errors together in one compile.

A planned `pass1_seal_incomplete` boundary-poison and extra pass2 NULL-guards turned out to be
**unnecessary** and were skipped (no dead defensive code): an ASan sweep — a direct `analyze()`
harness over ~20 adversarial single-file + merged-sibling inputs, plus the wire tests run
against an ASan-instrumented server with `abort_on_error=1` — found pass2's existing guards
(`EXPR_IDENT` `!sym->type`, module `!members`, `TYPE_ERROR` propagation) already sufficient.
`make_mangled` never returns NULL, and pass1 already skips the second `symtab_add` on a
redefinition, so the realistic NULL-deref surface was empty.

The old **safety-net** "analysis incomplete" diagnostic (`analyze.c`) now fires only on a hard
lexer abort (`!pass2_ran` ⇔ `aborted`), not on ordinary recoverable errors — its two wire
tests were rewritten to assert the new payoff. Stale-overlay retention (`last_good`) is kept
as the fallback for the lexer-abort case but is no longer the primary path.

Tests: `tests/cases/recovery/` (10 cases — leaf/block/decl/match recovery, a watchdog
no-hang case, and a `pass2-runs-after-pass1-error` CLI guard); new `tests/lsp/` wire tests for
the recovery payoff (live hover/CodeLens with a broken line) and the rewritten sibling-error
tests. 1691 tests green (gcc+clang), LSP wire tests green (incl. ASan), `examples.fc` clean.
(Drive-by: `run.sh` globbed `stdlib/*`, which scooped up `stdlib/lsp.rsp`; narrowed to
`stdlib/*.fc`.)

Remaining (still in TODO.md): lexer-level recovery is intentionally not done (layout errors
stay fatal). A handful (~14) of parser `diag_fatal` sites also remain by choice — validation
*inside* matched delimiters (slice/tuple/`alloc`/`cstr[N]` literals, fixed-array size),
string-interpolation, the inline `let…in` form, and the extern-reserved-name check; these are
rare mid-typing and the LSP falls back to `last_good` for them (no crash), so converting them
was deferred as low-value. And error recovery is a *prerequisite* for — but not itself —
parsed-file caching / incremental reparsing.

---

## LSP CodeLens/hover went silently blank on stdlib files (resolved 2026-06-27)

> **The "`-O0`-only empty-CodeLens divergence" framing was a misdiagnosis.**
> Optimization level had nothing to do with it; the variable was how the server
> resolved the stdlib feed versus where the opened file lived. All four
> combinations of `{-O0, -O2} × {feed path matches open file, feed path differs}`
> were tested: opt level never changed the outcome — only the path match did.

**Root cause.** `analyze_doc` (`src/lsp.c`) deduped the stdlib feed against the open
document by **canonical path** (`realpath`). When the opened file was a stdlib source
whose on-disk copy sat at a *different path* than the feed copy — e.g. opening the
repo's `stdlib/data.fc` while the server's feed resolved to the **installed**
`/usr/local/share/fcc/stdlib` (which happens whenever `FCC_STDLIB_DIR` is unset and FC
is installed) — the path dedup missed even though the two files were byte-identical.
The module was then merged twice → pass1 raised a redefinition → `analyze()` gated
`pass2_check` (so every `resolved_type` was NULL → empty CodeLens *and* dead hover) →
the redefinition diagnostic was attributed to the feed copy's path (≠ the open file),
so `publish_diagnostics` filtered it out. Net: the editor went silently blank.

The original observer correlated the failure with `-O0`/`make dev` because whichever
way they ran the `-O2` test happened to point the feed at the repo stdlib (the
`make test-lsp` wrapper sets `FCC_STDLIB_DIR`), while the `-O0` run resolved the feed
to the installed copy — a spurious correlation with the build, not the optimization.

**Fix (two parts, conservative-but-complete).**
1. **Content-identity dedup** in `analyze_doc`, OR'd with the existing path dedup: a
   feed entry is dropped if it matches the open document or a sibling by canonical
   path *or* by byte-identical content. The content check is gated on an equal length
   first, so a full `memcmp` runs only for a genuine same-length candidate — never for
   an ordinary edit (cheaper than the `realpath` syscalls already performed). The two
   keys are complementary: path catches the same file with unsaved edits (live buffer
   must win); content catches a separate identical copy at a different path.
2. **Safety-net diagnostic** in `analyze()` (`src/analyze.c`): when type-checking is
   gated (a pass1 error in a merged sibling/feed file, or a mid-parse abort) yet no
   diagnostic lands on the open file, a single file-level diagnostic is surfaced on the
   open document naming the first offending include ("analysis incomplete: an error in
   an included file (…) halted type checking …"). This makes the residual cases the
   dedup heuristic can't catch (an *edited* divergent copy) visible instead of a
   mysteriously dead editor, and fixes the whole class of silent gating (any merged
   sibling error used to blank the open file with no explanation). LSP-only; the CLI
   pipeline (`main.c`) never calls `analyze()` and is unchanged.

Tests: `tests/lsp/lsp_test.py` — "identical stdlib copy at a different path: type
CodeLens still provided" and the safety-net pair ("a merged sibling's error surfaces
an 'analysis incomplete' diagnostic …"). Both fail on the pre-fix binary.

## Unchecked cast `(T!)` — opt out of float→int saturation (resolved 2026-06-19)

> **Superseded 2026-06-21 by `unguarded`/`guarded` blocks.** The suffix-`!` cast
> overloaded `!` (already checked-unwrap and boolean-not) for the opposite
> *unchecked* meaning. It was retired in favor of a single general mechanism —
> `unguarded`/`guarded` block expressions — that govern all three runtime guards
> (float→int saturation, integer divide/modulo, slice bounds) without touching `!`.
> See §Unguarded blocks in the spec. The motivation and trap-vs-UB analysis below
> still hold; only the spelling changed (`(int32!) f` → `unguarded (int32) f`).

The saturating float→int cast (rc.5, audit item 16) is the only UB-fix that carries
per-operation runtime overhead: `float → int` routes through the `fc_f2*` helper family (NaN
check + two range branches before the truncate), measured at ~2.5× the bare `cvttsd2si` in a
per-pixel loop. Other "define C's UB" cast decisions are free (static restrictions or
already-defined C casts). In wolf-fc's raycaster that's ~64k+ saturating conversions/frame,
almost always on values already known in range.

**Shipped:** an opt-in escape hatch, suffix `!` on the target type in a cast — `(int32!) f`.
Saturation stays the **default**; `(T!)` emits the bare `(int32_t)f` (exactly the rc.4 codegen,
bit-identical for every in-range value; UB only on the out-of-range/NaN inputs the programmer
vouches won't occur). Reads like option-unwrap's postfix `!`: "I assert the precondition the
compiler can't verify; don't make me pay for the failure path."

Legal **only** on a float→integer cast (the one cast with a runtime check to skip). On any other
cast — `(int32!) someInt`, `(float64!) i`, `(usize!) ptr`, identity — it is a compile error
("redundant '!': this cast inserts no runtime check"), mirroring the rule that `x!` is rejected
on a non-option. Deliberate trap-vs-UB asymmetry vs option `x!`: `x!` is *checked* (aborts on
`none`), `(T!)` is *unchecked* (UB out of range) — a trapping cast would still emit the range
comparisons and be no faster, defeating the purpose.

Implementation: a `bool unchecked` flag on the `EXPR_CAST` AST node — parser accepts the optional
`!` in cast position (`parser.c`, `parse_prefix`); pass2 rejects it anywhere but float→int
(`pass2.c`, `EXPR_CAST`); codegen guards the saturating branch so unchecked falls through to the
bare cast (`codegen.c`, `EXPR_CAST`). No new runtime, no monomorph interaction. Spec: §Casting,
`examples.fc`, `grammar.bnf` (`cast_expr` gains optional `"!"`). Tests:
`casts_widening/unchecked_cast*` (happy path across integer targets from float64/float32; five
`.error` cases for int/float-target/identity/bool/pointer sources).

**The optional cast-then-clamp peephole** sketched in the original proposal was *considered and
dropped*: detecting a `(T) f` whose result flows into a sub-range clamp is a fragile
multi-statement AST pattern match, and — unlike the explicit `(T!)` — it would reintroduce UB on
NaN/overflow automatically, without programmer opt-in, in tension with FC's "remove real UB"
stance. The explicit `(T!)` is the general, predictable lever; the peephole was not worth its
fragility. The `s[i!]` / `a /! b` extension to *operators* (bounds check, divide guards) remains
a separate, unaddressed design question.

> Origin: rc.4→rc.5 wolf-fc codegen-regression audit (2026-06-19).

## Generic call as a non-first operand mis-parses (resolved 2026-06-10)

Surfaced while fixing the qualified-type-argument scan (next entry): an explicit-type-argument
call `name<T>(...)` only parsed when `name` was the *leftmost* token of its expression. As any
later operand it fell apart — with simple type args, no dots involved:

- `assert(qo >= size_of<int32>())` — `unexpected token ')' in expression`
- `let x = 1 + size_of<int32>()` — same
- `assert(!is_big<int32>())` — same (prefix operand, not just binary RHS)

Root cause was *where* the disambiguation lived. The "generic call vs comparison" scan ran only
in the `TOK_LT` infix handler and only fired when `left` was a bare `EXPR_IDENT`/`EXPR_FIELD`.
When the callee appeared as the right operand of `>=`/`+`/`!`/etc., precedence climbing had
already grabbed the bare name as that operator's operand (the `<` is at comparison precedence,
too low to bind in the recursive call), so by the time the loop saw `<`, `left` was the whole
compound expression and the scan never ran — the tokens then parsed as a chained comparison and
died on `>(`.

Fix: the scan was factored into `generic_call_scan()` (type-arg tokens between `<` and `>`,
then `(` or `.`), and the Pratt loop in `parse_expr` now bumps a `<` that passes the scan to
`PREC_POSTFIX` when `left` is a bare ident/field — a generic call *is* a call, so its `<` binds
at call precedence like an ordinary call's `(`. Since the infix handler consumes the entire
`name<T>(...)` form once entered, the bump only adds entry points and cannot change existing
parses; the comparison fallback stays for everything the scan rejects. Same disambiguation rule
as before (spec §Generic Functions, Parsing), now applied wherever a call can appear. Test:
`generics/explicit_call_operand_position.fc` (binary RHS at several precedences, prefix
operands, module-qualified callees, nested type args, variant construction in operand position,
plain comparisons unaffected).

## Generic call with a qualified type argument mis-parses (resolved 2026-06-10)

A parser gap in the explicit-type-argument call form `name<Type>(...)`, surfaced building a
generic lock-free ring (`spsc.ring<'a>`) in wolf-fc: a **qualified** type argument
(`module.type`) failed the `<` disambiguation scan. The tentative scan that decides "generic
call vs comparison" (`is_type_arg_token` in `parser.c`, shared by the call-position scan and
the array-literal scan) accepted identifiers, `*`, `?`, `[]`, etc. — but not `.`. Any dotted
name inside `<...>` aborted the scan and the expression fell through to comparison parsing.
Type *annotations* accepted these fine (`cmd_ring: spsc.ring<audio_q.cmd>*` parsed, and
`parse_type` itself handles qualified names); only the call/value position failed — a
parser-only, not type-system, issue.

The one root cause showed up as two different downstream errors:

1. **Plain qualified argument read as comparison.** `spsc.make<audio_q.cmd>(64)` parsed as
   `spsc.make < audio_q.cmd > (64)` — a chained ordering comparison — giving the baffling
   `ordering comparison requires numeric or pointer types, got <'a>(int64) -> spsc.ring<'a>*
   and audio_q.cmd`.

2. **Qualified pointer argument died on the `*`.** `spsc.make<imf.player*>(16)` also fell
   through to comparison parsing, where `imf.player * ` is a multiplication missing its right
   operand — `unexpected token '>' in expression`. Not a separate pointer gap: a bare pointer
   type argument (`make<int32*>(16)`) always parsed fine; the dot was again the culprit.

**Resolution:** added `TOK_DOT` to `is_type_arg_token` so qualified names pass the
type-argument scan at both shared scan sites (generic call/variant-construction position and
array-literal position). `parse_type` already handled qualified names, so no other change was
needed. This extends the spec's disambiguation rule ("`name<type>()` is always a generic
call") to dotted names — `a < m.b > (c)` is now a generic call, which costs nothing because
the chained comparison it would otherwise be is a bool-ordering type error anyway. Spec
§Generic Functions **Parsing** paragraph updated to enumerate the accepted type-argument
spellings (qualified names, `*`/`?`/`[]` suffixes) and the rule's precedence. Four regression
tests added under `tests/cases/generics/qualified_*` covering qualified args on module and
top-level callees, mixed multi-args, pointer/option/slice suffixes, bare-pointer args, generic
union variant construction (`maybe<m.cmd>.just(v)`), and array literals (`slot<m.cmd>[2]
{...}`).

Noted while fixing, left open (pre-existing, unrelated to the dot gap): a generic call only
parses when the callee is the *leftmost* token of its expression — as a later operand
(`assert(qo >= size_of<int32>())`, `1 + size_of<int32>()`, `!is_big<int32>()`) precedence
climbing grabs the callee name as the operator's operand before the `<` disambiguation can see
it. Tracked as its own TODO ("Generic call as a non-first operand mis-parses").

## Concurrency primitives — atomics + a memory model (resolved 2026-06-10)

FC had no concurrency story: no atomics and no multi-threaded memory model, so the only way to
talk to threads created by C libraries (e.g. SDL's audio callback in wolf-fc) was bound host
primitives or blocking locks. Resolved with two builtin operators scoped to the lock-free
single-producer/single-consumer case:

- **`atomic_load_acquire(p)` / `atomic_store_release(p, v)`** — keywords like `alloc`/`free`,
  operating through a pointer to a *plain* object (no atomic type, following the Linux kernel
  `smp_load_acquire`/`smp_store_release` and Zig builtin school rather than the C11 `_Atomic` /
  Rust `AtomicT` type-based school). Pointee must be an integer type or `bool`; the store value
  widens implicitly; loads through `const` pointers are allowed, stores are not. Type-variable
  pointees are rejected (conservative-but-complete).
- **Memory model**: the acquire/release subset of C11, specified in §Atomics & Memory Model
  (Part 7) along with the no-tearing guarantee and the data-race rules. Codegen emits
  `__atomic_load_n`/`__atomic_store_n` plus a per-use
  `_Static_assert(__atomic_always_lock_free(...))` so non-lock-free targets fail the C build
  instead of degrading to a locked implementation.

Deliberately excluded (new names if ever added, semantics of these two never change):
read-modify-write ops (`fetch_add`, CAS, `exchange`), standalone fences, relaxed/seq_cst
orderings, pointer-pointee atomics, and any thread spawn/join — SPSC publication needs none of
them. Tests in `tests/cases/atomics/` including a full SPSC ring exercise.

## Diverging expressions lack a bottom type in match-arm / if-branch unification (resolved 2026-06-02)

Originally: `return`/`break`/`continue` were typed `void` inside a match arm or if branch
rather than a bottom type, so they wouldn't unify with a sibling that yields a value. This
blocked the idiomatic "unwrap-or-bail" form (`| none -> return 1` alongside `| some(w) -> w`
errored `match arms have different types: any* vs void`), as well as `let x = if c then v
else return`. Tracing also turned up a related gap: a function body ending in `return value`
was rejected, because the tail's type was `void`.

**Resolution:** added a bottom type `TYPE_NEVER` (`types.c`) and typed `return`/`break`/
`continue` as `never` (`pass2.c`). A `unify_branch` helper absorbs `never` into its sibling
at the `if` and `match` unification sites; the loop's `break` *value* still flows through the
separate `loop_break_type` channel, unchanged. When a function body's tail diverges, the
return type is derived from the `return` statements instead of the (valueless) tail — so a
body ending in `return expr` (or in a `match` whose every arm returns) now type-checks.
Codegen emits a diverging branch/arm as statements rather than assigning a result temp (no
ternary; `if`/`match` value forms use a statement-expression with a result temp that only the
value branch writes). Binding a fully-diverging expression is rejected with `cannot bind 'x':
every path through this expression returns, so it has no value`.

Scope decisions (with the user): all three keywords (`return`/`break`/`continue`) were
included, since they share the identical bug and the loop-value channel is separate. The
bottom type is internal — not surfaced to users by name. `noreturn` *functions* (`sys.exit`,
`abort`) and treating an infinite `loop` as `never` were left out of scope. 11 regression
tests added under `tests/cases/control_flow/never_*`; full suite green on gcc and clang at
`-O0` and `-O2`. Spec updated (Part 2 Control Flow Expressions; Early-return rule). Surfaced
while adding SDL handle null-checks in wolf-fc.

---

## Stdlib test coverage — std::sys and std::io gaps (resolved 2026-05-03)

Originally: from the 2026-04-20 stdlib audit, `std::sys` had zero automated test coverage (`env`, `time`, `sleep`, `get_pid`, `temp_dir`, `home_dir`, `exit`) and `std::io` was missing tests for `read_all`, `read_char`, `exists`, `can_read`, `can_write`. The code existed and was being used by real programs (wolf-fc, demos), but nothing in the suite asserted against it.

**Resolution:** added nine multi-file tests under `tests/cases/stdlib/`:

- `sys_env` — happy path on `PATH`; negative path on a name guaranteed-unset.
- `sys_time_sleep` — covers `time` and `sleep` together: snapshot clock, sleep 50ms, snapshot again, assert delta is in `[30ms, 10s)` (range tuned for Win32 Sleep's ~15.6ms granularity floor).
- `sys_get_pid` — pid > 0 and stable across calls within the same process.
- `sys_temp_dir` — returns some, path is non-empty, and is actually a writable directory (round-trips a probe file).
- `sys_home_dir` — returns some with non-empty path.
- `sys_exit` — calls `sys.exit(42)`, expected exit code 42; an `assert(false)` guards the unreachable path so a no-op `exit` would fail loudly instead of silently returning 0.
- `io_read_all` — round-trip a known payload, byte-compare, free; missing path returns none.
- `io_read_char` — read two bytes one at a time, then EOF returns none.
- `io_exists_access` — missing path: all three predicates false; existing file: all true; existing directory: `exists` true.

All run on gcc and clang; full suite 1250 tests passing (was 1241). No stdlib code changes were needed — the audit was right that the code worked, just unverified. Both `TODO.md` bullets are now closed and the file is empty of stdlib items.

---

## Stdlib expansion bullets — sprintf, io_error, bit utils, encodings (deferred past 1.0, 2026-05-03)

The 2026-04-20 stdlib audit produced four "candidate work" bullets that lived on `TODO.md`: (1) `sprintf`-style formatted output beyond string interpolation; (2) an `io_error` union or thread-local `last_errno` so `io`/`sys`/`net` failures don't lose `errno`; (3) bit utilities (`popcount`, `leading_zeros`, `trailing_zeros`, `rotl`/`rotr`, `byteswap`); (4) encoding helpers (`base64`, `hex`, URL).

**Resolution:** deferred past 1.0. None of these block a release; all are feature growth dressed up as gaps.

- **`sprintf`-style formatted output** — string interpolation already covers the everyday case and is type-checked at compile time, which `snprintf` will never be. Code that needs raw `snprintf` can `extern` it directly today. Pure feature growth.
- **`io_error` / `last_errno`** — this is API design, not a missing feature. The current `bool` / `option` return shape works for everyday use; swapping it in later is a breaking change either way. Doing the redesign under release pressure to "make 1.0 feel complete" is the wrong reason. Ship the simple model; revisit when a real consumer demands actionable errno.
- **Bit utilities** — half a dozen tiny wrappers around `__builtin_popcount` etc. Trivial to add at any point post-1.0; nobody is currently blocked on them.
- **Encoding helpers** — application-level conveniences, not part of the language-runtime contract. Belong in user libraries or a 1.x stdlib expansion.

The two stdlib items kept on `TODO.md` are pure test-debt against code that already exists and ships (`std::sys` and the `std::io` gaps from the same audit) — those are real release-confidence blockers; these four are not.

---

## Unused-binding warning (retired 2026-05-03)

Originally proposed: an opt-in `-Wunused` for file-scope / module-scope `let` bindings that are declared but never referenced. Surfaced during the wolf-fc subsystem-extraction refactor (2026-04), where hand-rolled shell scripts turned up `tile_area`, `tex_size`, `alt_elevator_tile`, and `hud.draw_vsep` — dead code that survived the original file split. The same check would naturally extend to same-scope shadowing.

**Resolution:** retired as won't-implement, and codified into a broader stance: **FC's diagnostics surface has no warnings — every diagnostic is either an error or it isn't emitted at all.** An unused binding is not wrong in the strict sense; it's a style call that belongs to the programmer. A warning that doesn't fail the build adds noise and trains people to ignore output, and a warning that *does* fail the build is just an error wearing a hat. If we ever want to surface this kind of advisory information, the right channel is an LSP/editor integration where it can be presented contextually without coupling to compilation success. Spec's §Diagnostics and `CLAUDE.md` now document the no-warnings stance.

---

## Cycle diagnostics — shortest call-graph path (retired 2026-05-03)

Originally proposed: when an import or module reference cycle is detected, print the shortest symbol-to-symbol path that forms the cycle, not just the file/module pair at the endpoints. Surfaced during a wolf-fc refactor that accidentally introduced a transitive cycle through `overlay → save → intermission → overlay`; the error named the modules but not the specific edges, and tracing by hand took a few minutes.

**Resolution:** retired. The pain has not recurred since, and the user no longer recalls the specific case as load-bearing. Not worth holding 1.0 for. Revisit if the same shape of confusion shows up again under real use.

---

## Const-expr propagation through module-level name references (resolved 2026-04-23)

Originally: module-level `let` initializers required pure literals for element expressions. The 2026-04 loosening (commit `a8c1221`) accepted array literals, `some(...)`, and variant constructors as initializers, but elements still had to be literals. Named constants like
```fc
module music =
    let getthem = 3
    let searchn = 11
    let songs = int32[60] { getthem, searchn, getthem, ..., pacman }
```
were rejected with "must be a constant expression," forcing wolf-fc to inline raw integers with apology comments.

**Resolution:** extended the module-level const-expr gate in `check_module_members` to fold identifier references through `const_fold_expr`. Folded values substitute into the init tree at pass2 time, so codegen sees only literal forms; no changes to `codegen.c`. Uses the `resolved_sym` already stored on `EXPR_IDENT` for lookup (the single-resolution invariant CLAUDE.md calls out), and a four-state (UNVISITED/VISITING/DONE/FAILED) marker on `Decl.let` for memoization plus an internal reentry guard. `let mut` targets and file-level lets are rejected; scope follows FC's normal lookup (same module, imports, parent modules), so forward and cross-module references are both fine. Arithmetic over folded idents (`let n = base + 1`) composes for free via the existing `EXPR_BINARY` const-expr path. Value cycles are caught by pass2's existing on-demand type-check cycle detector (`circular dependency: 'X' depends on itself`), which type-checks the same ident paths fold would walk — the VISITING state exists only as an internal-invariant reentry guard (fires `diag_fatal` if ever observed). Regression and feature tests under `tests/cases/memory/module_ident_*` (backward, forward, transitive, in-some/variant/struct/slice, arithmetic, cross-module, cycle, mut-ref).

---

## Typed enum-indexed arrays (retired 2026-04-22)

Originally: in wolf-fc, several 60-entry tables are semantically `music`-valued, `palette-index`-valued, or `par-seconds`-valued, but their type is bare `int32[60]` (e.g. `let songs = int32[60] { 3, 11, 9, 12, ... }`). Proposed that if `music` were a real int-tagged enum, `music[60]` could be a first-class type: every literal element checked against the variant set, and the table index itself (`songs[level_num]`) yielding a `music`, not a raw `int32` needing a cast. A range-typed index (`level_index` guaranteed `0..59`) would let the lookup be total and eliminate the runtime `if level_num >= 0 && level_num < 60` guards consumers write today.

**Resolution:** retired as won't-implement for 1.0. This wants three things FC doesn't have: (1) int-tagged enums as a distinct kind — today zero-payload unions fill the "named constants" role but aren't designed to be int-indexed or interchangeable with `int32`; (2) `enum[N]` as a first-class array type with element-wise variant-set checking; (3) range-typed indices (`0..59` proved at the construction site), which is dependent-types-lite and would reshape the type system. Legitimate feature, but it's a 2.0-scale design conversation, not a 1.0 paper cut. The existing workaround (`int32[60]` plus one runtime bounds guard at the read site) is ugly but not broken and doesn't block any real code. Revisit if/when FC takes on a broader "refinement types" direction.

---

## Extended filesystem — stat/symlinks/mmap (retired 2026-04-22)

Originally: listed under stdlib gaps as future work — `stat`/metadata, symlink handling, memory mapping. Already flagged "Explicitly out of scope today; worth naming as future stdlib work."

**Resolution:** retired as won't-implement for 1.0. Each of these is a non-trivial cross-platform surface (POSIX vs. Windows `GetFileAttributesEx`/`CreateFileMapping`/reparse points) that adds substantial stdlib API without solving a currently-hit problem in FC code. The existing `std::io` primitives (open, read, write, exists, mkdir, list_dir) cover the everyday filesystem needs; programs that need stat/mmap today can reach through `extern` to the C APIs. Keep as a known gap; not a tracked item.

---

## Cross-namespace nested-type identity leak through user modules (resolved 2026-04-22)

Originally: a stdlib type at a nested module path (e.g. `std::random.pcg_random`, declared as `module random = struct pcg_random + module pcg_random = ...`) got a different internal name depending on where it was *referenced*. When a user module called a stdlib function whose signature mentioned the nested type, pass2 ran the callee's `check_decl_let` on-demand but only set `ctx.module_symtab`, leaving `parent_modules`, `import_scope`, and `current_ns` reflecting the caller's scope. Bare stub names in the callee's own signatures (like `pcg_random*` in `stdlib/random.fc :: next_uint32`) then failed to resolve, the parameter stayed as `TYPE_STUB`, and unification against the caller's fully-resolved `std::random.pcg_random*` argument produced `expected pcg_random*, got std::random.pcg_random*`. The workaround was to keep every function touching the type at entry-point file scope, which blocked module-boundary refactoring in wolf-fc.

**Resolution:** added `struct Symbol *parent` to `Symbol` (set in a final pass1 phase after all symtab mutations, so pointers stay stable). Added `enter_module_scope_on_demand` / `restore_scope` helpers in pass2 that rebuild the full `ModuleScopeChain`, `ImportScope` stack, `module_symtab`, and `current_ns` from the callee module's parent chain, plus file-level imports from its declaring file. Wired into the on-demand field-access path at `pass2.c` around line 3291. `CheckCtx` now carries `file_scopes` so the rebuild can reach per-file import tables. Regression tests in `tests/cases/modules/`: `cross_ns_nested_type_via_user_module/` (mirrors the TODO reproducer), `cross_ns_nested_type_transitive/` (chains through three user-module peers so multiple on-demand resolutions happen in succession), and `cross_ns_nested_type_no_import_err/` (confirms missing import still surfaces as a clean "different namespace" error). All 1206 tests pass on gcc and clang.

---

## Escape analysis — interprocedural gap (retired 2026-04-20)

Originally: the intraprocedural escape analysis (commit `38e6461`) catches stack-ptr in struct/union construction, field/index access into stack-provenance aggregates, global assignment of stack-provenance values, and heap-struct field assignment (`h->f = &stack`). What remained was **interprocedural** — if a function receives a struct-with-pointer by value, the parameter is `PROV_UNKNOWN` inside the callee, so writing it to a global / heap field there isn't caught at the call site.

Example that slips through:
```fc
let stash = (h: holder) -> g = h                   // h is PROV_UNKNOWN inside stash
let leak  = () -> stash(holder { p = &local })
```

Two fix directions were considered: (a) a per-function escape summary pass propagating "parameter carries stack provenance and escapes" bits across the call graph via fixpoint; (b) forbid passing provenance-carrying aggregate values by value to non-local functions.

**Resolution:** retired as won't-implement. The gap requires a specific three-step sequence (build aggregate carrying a stack pointer → pass it by value → callee escapes it) that is uncommon in practice. The intraprocedural pass already catches the common mistakes — direct `&local` return, storing into a heap struct, returning a struct with a stack-pointer field, freeing non-heap memory. Approach (a) would add roughly 400 LOC of fixpoint analysis for a narrow additional catch and complicates handling of indirect calls, generics, and closures. Approach (b) would reject legit code, most visibly any indirect call or callback receiving a pointer-in-struct argument. FC's design stance is manual memory management in C's tradition; escape analysis is a sanity net, not a safety proof, and extending it across call boundaries starts to conflict with that stance. Use-after-free and other lifetime-shape problems are already outside the analysis's remit. The spec's `§Pointers and References → Escape analysis` section now documents the interprocedural gap explicitly with an example so users know where the analysis stops.

---

## `&fn` does not emit a raw C function pointer (resolved 2026-04-20)

Originally: `&fn` on a non-capturing function was emitted as `&(fc_fn_..._..){.fn_ptr = fn, .ctx = NULL}` — the address of a fat-pointer compound literal instead of a raw C function pointer. Storing that into a C struct field (e.g. `SDL_AudioSpec.callback`) handed C code a pointer to a two-word struct, which it then tried to call as a function — segfault. The trampoline mechanism (`_ctramp_*`) only fired when a function was passed as an argument to an extern call with a `TYPE_FUNC` parameter; struct field assignments, casts to `any*`, and any other `&fn` context slipped through.

**Resolution:** took fix direction 1 from the original TODO. Changes:

- `codegen.c`: `collect_trampolines_expr` now registers a trampoline for `EXPR_UNARY_PREFIX(TOK_AMP, ...)` when the operand is a top-level function ident (non-local `EXPR_IDENT` with `TYPE_FUNC`), a module-qualified function field, or a non-capturing inline lambda (`EXPR_FUNC` with `capture_count == 0`). `emit_expr` for the same patterns now emits the raw `_ctramp_<name>` symbol instead of `&(fat-struct){...}`.
- `pass2.c`: `&f` on a function-typed operand now yields `any*` with `PROV_STATIC` (was `TYPE_FUNC*` with `PROV_STACK`). This makes `&fn` an opaque C-interop handle — it can be passed through `any*` parameters and stored in `any*` struct fields, but FC-side dereference/call is a type error. Also added: `&(inline lambda)` rejected at compile time if the lambda captures.
- Tests: rewrote `pointers/addr_of_func.fc` (its `let p = &double; (*p)(21)` pattern relied on the old buggy behavior), added `pointers/addr_of_func_c_invoke/` (multi-file — C helper invokes `&fn` through `void*` and through a struct field), `pointers/addr_of_func_deref_err.fc` (rejects `*p` on the `any*` result), `closures/addr_of_inline_capturing_err.fc` (rejects inline capturing lambda), and `escape/return_addr_of_func.fc` (confirms `&fn` has static provenance so it can be returned).

All 1203 tests pass on gcc and clang.

---

## `-Walloc-size-larger-than=` warning at `monomorph.c:532` (resolved 2026-04-18)

Originally: under `-O2 -flto`, GCC flagged `calloc((size_t)t->count, sizeof(int))` with `-Walloc-size-larger-than=`. LTO range analysis couldn't prove `t->count >= 0`, and a negative `int` cast to `size_t` becomes a high-range value that tripped the heuristic. Surfaced during wolf-fc's switch to building its local FC compiler copy with `-O2 -flto`. Not a real bug — `t->count` only grows from 0 via `DA_APPEND` — but worth documenting the invariant.

**Resolution:** added `assert(t->count >= 0)` before the `calloc` at `monomorph.c:532` (plus `#include <assert.h>`). The assert gives GCC LTO the range information it needed, silences the warning at `-O2 -flto`, and makes the invariant explicit. Considered but rejected: changing `count` from `int` to `size_t`/`uint32_t` — would touch 17 call sites in `monomorph.c` for no functional gain. Build is clean at `-O0`, `-O2`, and `-O2 -flto`; all 1152 tests pass.

---

## Windows/MSYS2 test failures (resolved 2026-04-18)

Originally: 37 of 987 tests failed on MSYS2 UCRT64 (gcc), grouped into abort/signal handling (different exit code from POSIX), POSIX-dependent IO/stdio, and POSIX-dependent networking. The failure list was captured on 2026-04-04, before Windows branches were added to `stdlib/sys` (sleep/time/pid) and `stdlib/io` (mkdir/list_dir); by the time the work landed, net.fc already had a full Winsock branch.

**Resolution:** on UCRT64, all 987 tests now pass.

- `stdlib/io.fc`: added a Windows branch routing `access()` through `_access` in `<io.h>` (MinGW's `<unistd.h>` wrapper was unreliable enough under UCRT64 that just going direct was simpler).
- `stdlib/sys.fc`: `temp_dir()` now falls back to `/tmp` on POSIX when `TMPDIR`/`TMP`/`TEMP` are all unset (common on Linux; Windows always sets `TEMP`). This lets tests use `sys.temp_dir()!` unconditionally.
- `src/codegen.c`: emit `#ifdef _WIN32` constructor calling `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)` before main. Without it, every abort-triggering test popped a Watson dialog and hung; with it, abort() exits cleanly with status 3.
- `tests/run_tests*.sh`: on MSYS2/MinGW, treat exit code 3 as equivalent to 134 for tests whose `.expected_exit` hardcodes the POSIX abort value. Avoids duplicating every exit file per platform.
- `tests/cases/io/*` and `tests/cases/stdlib/io_*`: replaced hardcoded `/tmp/...` and `/dev/null` paths with `sys.temp_dir()!` + `%s{tmp}/...` interpolation, dropped POSIX-only assertions (e.g. "/dev/null is readable"), and gave `io_mkdir` / `io_list_dir` a Windows branch for `_rmdir` from `<direct.h>`.

Out of scope and left alone: the `SOCKET` (uint64 on x64) vs `int32` declared return type in `stdlib/net.fc`'s Winsock externs. It's technically ABI-incorrect for handles above 2^31 but Windows assigns small socket values in practice, and the tests pass. Noted as a latent issue, not a blocker.

The net-test "C compilation failed" entries the user reported mid-investigation turned out to be environmental (likely AV interference with the linker); they cleared without touching `stdlib/net.fc`.

---

## `when` guards on match arms (implemented 2026-04-18)

Originally: allow a boolean predicate on a pattern, e.g. `| ek_dog when no_dogs -> continue`. The workaround was an outer `if` + separate `match`, or a dedicated arm with no predicate and a follow-up `if` inside the arm — both split logic that would read as a single guarded pattern. Came up in wolf-fc while gating dog spawns behind a CLI flag.

**Resolution:** `when <bool-expr>` is now a legal clause between a match arm's pattern and its `->`. The guard evaluates in the arm's scope so any pattern bindings (including destructured struct fields and variant payloads) are visible. When attached to an or-pattern, the guard applies to the whole pattern (every alternative shares it). Arms with a guard do not contribute to exhaustiveness — the Maranget pattern matrix skips them — so a final wildcard (or otherwise complete unguarded coverage) is required. Because `->` serves double duty as pointer-field access and arm separator, the parser blocks `->` as a top-level infix within a guard expression; pointer-field access inside a guard must be parenthesised (`| p when (p->value) > 0 -> ...`). Codegen switches from the flat if/else chain to a done-flag + `abort()`-on-fallthrough structure whenever any arm has a guard, so a guard-false arm cleanly falls through to the next. `tests/cases/pattern_matching/match_when_*` covers basic guards, destructured bindings, or-pattern guards, fallthrough, option/variant/bool patterns, pointer-field access, non-bool error, body-less error, and exhaustiveness error.

---

## Codegen: emit newlines inside large struct initializers (fixed 2026-04-16)

Originally: `alloc(SomeStruct { field1 = alloc(...)!, field2 = alloc(...)!, ... })!` lowered to one massive C line — every nested `alloc(...)!` expanded to a GCC statement-expression and they concatenated inline into the struct literal. With ~10+ fields the line crossed gcc's column-tracking limit (~4096 cols), causing `note: '-Wmisleading-indentation' is disabled from this point onwards, since column-tracking was disabled due to the size of the code/headers`. Wolf-fc hit this in `build_level` (~17 fields) and silenced it with `-Wno-misleading-indentation`.

**Resolution:** `EXPR_STRUCT_LIT` emission now breaks fields onto their own lines when the literal has 2+ fields, using the standard `indent_level` machinery. The `alloc(struct_lit)` and `alloc(union_variant)` lowerings also wrap the `if (_ap) *_ap = ...;` assignment in explicit braces so multi-line struct literals don't trip clang's `-Wmisleading-indentation` either. Reproducer (17-field struct alloc'd with nested `alloc(int32)!`) dropped from a 3723-col line to 635 cols; all 1134 tests pass on gcc and clang.

---

## `!` boolean-not precedence in nested if/else chains (investigated 2026-04-14)

Originally filed after writing the wolf-fc push-wall code: `else if !pushwall_tiles[idx] then false` inside a deeply-nested if/else chain appeared to parse incorrectly — the condition seemed to not trigger as expected. The speculation was that `!` might be mis-parsed as postfix option-unwrap on the slice-index expression, or that there was a precedence interaction with `else if`.

**Resolution:** could not reproduce. The parser handles all the relevant forms correctly:

- `!arr[i]` parses as `!(arr[i])` — postfix `[i]` binds tighter than prefix `!`, then prefix `!` applies to the indexed result. Works in assertions, if-conditions, and arbitrarily deep else-if chains.
- `!obj.field`, `!ptr->field[i]`, `!fn(x)`, `!!x`, and `!(expr)` all parse as expected.
- Deeply nested `if / else if / else if / ... / else if !arr[i] then ...` chains evaluate the `!arr[i]` branch correctly.

The original symptom was most likely a local indentation or logic mistake in the surrounding push-wall code, not a parser bug. Regression test: `tests/cases/expressions/bool_not_precedence.fc` locks in the correct behavior for slice index, field access, pointer-field chain, call, double-negation, and nested else-if forms.

---

## OR-patterns with bindings (retired 2026-04-14)

Originally listed as a possible v2 extension: or-pattern alternatives are currently binding-free — `| some(x) | none -> x` is rejected. Lifting this would require the same-bindings-at-same-types rule that Rust/OCaml enforce, plus a different codegen strategy than the current `(a || b)` predicate (e.g. per-alternative if-branches that set bindings and `goto` a shared arm body).

**Resolution:** not doing this. The binding-free form already covers the useful cases; the workaround (repeat the arm body, or bind in a single pattern and match further inside) is acceptable for the rare cases where it comes up.

---

## SIMD / vector types (retired 2026-04-10)

Originally listed as an evaluation item: investigate first-class vector types for SIMD operations (e.g., `float32x4`, `int32x8`), likely emitted as GCC/Clang `__attribute__((vector_size(N)))` typedefs that get arithmetic operators for free.

**Resolution:** out of scope for 1.0. GCC and Clang already auto-vectorize many numeric loops, so most FC programs get SIMD "for free" through the C compiler without any language changes. Explicit vector types only pay off when auto-vectorization fails or guaranteed vectorization is required — a narrow enough niche that code needing it can drop to C via `extern` for hand-written intrinsics. The spec's Platform contract section now documents this as a deliberate non-feature, pointing users at the auto-vectorizer and `extern` as the two escape hatches. Worth revisiting only if FC targets performance-critical numeric workloads where the auto-vectorizer's coverage becomes a bottleneck in practice.

---

## Packed structs and bit-level layout control (resolved 2026-04-10)

Originally listed as "Packed structs and bit-level layout control" — add a `packed struct` keyword so FC could emit `__attribute__((packed))` and disallow `&field` to keep `-Werror` clean, plus first-class bit field syntax for hardware register layouts. Use cases: memory-mapped I/O, binary protocol parsing, compact on-disk formats, register maps.

**Resolution:** no native features needed for either packed structs or bit fields. Both are accessible through `extern struct` against a C header that defines the layout, with zero FC compiler changes.

For **packed structs**, the C header declares the type with `__attribute__((packed))`, the FC side mirrors the field layout as an `extern struct`, and every extern-struct operation (construction, field read/write, `default`, `sizeof`, structural equality, by-value and `T*` parameter passing, nesting) works transparently. The C compiler handles the unaligned-access codegen — FC just emits ordinary field references. The single restriction is that taking `&packed_field` triggers `-Waddress-of-packed-member`, which fails under `-Werror`; the workaround is to copy the field to a local first.

For **bit fields**, the same pattern applies with no width annotation on the FC side. FC declares each bit field as its underlying integer type (`unsigned int : 3` → `uint32`), and the C compiler emits the shift/mask for every read and write automatically because it owns the struct definition from the header. Empirically verified against gcc and clang: `default`, designated-initializer construction, field read/write, whole-struct copy, and structural equality all work. Taking `&bit_field` is rejected at C compile time with a clear error (`cannot take address of bit-field 'name'`). As a bonus, the C compiler catches literal writes that don't fit the declared bit width (`field = 9` into a 3-bit field) under `-Werror=overflow`, giving FC free compile-time range checking for constants without FC tracking bit widths at all.

Regression tests:

- `tests/cases/extern/extern_struct_packed/` — packed `tcp_header` with a `_Static_assert` on its 13-byte size; exercises construction, default, field read/write, whole-struct copy, and the copy-to-local workaround for `&field`.
- `tests/cases/extern/extern_bit_field/` — `struct reg` with 1-, 3-, 4-, and 24-bit fields mirrored as plain `uint32`; exercises the same operations plus structural equality across bit-field fields.

The C-interop section of the spec documents both patterns under "Packed extern structs" and "Bit fields."

**Tradeoffs accepted:**

- Users maintain a C header alongside their FC code (a few lines per type).
- No layout cross-checking between the C side and the FC mirror — drift shows up at runtime. Mitigated by `_Static_assert(sizeof(...) == N, ...)` in the header for size checks and explicit range asserts in FC for bit field width expectations. Field reordering is still on the user.
- Runtime (non-literal) values that overflow a bit field still truncate silently, matching C bit field semantics.

In practice, nearly every real use case for packed structs and bit fields already starts from an external definition — network RFCs, file format specs, vendor hardware headers, kernel ABI — so the extern C header is the natural integration point, not an imposition. Native FC syntax for either feature would only be worth revisiting if self-hosted FC code ends up with many such types where the C-header overhead becomes annoying duplication, or if FC pursues goals that disallow C-header dependencies.

---

## Platform flags and cross-platform support model (resolved 2026-04-09)

The original `--flag windows` approach conflated OS with toolchain/environment — MSYS2 UCRT64 and native MSVC both look like "Windows" but use different ABIs. Resolved by introducing a structured taxonomy along three axes (`os`, `arch`, `env`), auto-detected from the host C compiler.

**Resolution:**

- `#if` expression evaluator (commit 5cf85e3) — added `!`, `&&`, `||`, parentheses, and string equality (`flag == "value"`), so flags can carry values via `--flag name=value`. This made the structured taxonomy expressible at the language level.
- Platform auto-detection (`src/platform.c`) — uses compile-time `#ifdef` checks against the C compiler's predefined macros to bake `os`/`arch`/`env` values into the `fc` binary at build time. Initially implemented as a runtime `popen("cc -dM -E ...")` probe; switched to compile-time detection after profiling showed the subprocess cost dominated `fc` startup (~6 ms per invocation, ~10 seconds across the test suite). Defaults the env to `gnu` on Linux (musl detection deferred); sets `gnu` for MinGW Windows; leaves env unset on macOS/FreeBSD where it is implied by the OS.
- User override semantics — `--flag os=...` replaces an auto-detected entry rather than appending alongside it. This gives cross-compilation for free with no separate `--target` flag.
- `--no-auto-detect` CLI option — disables the compiler probe entirely, for reproducible builds and testing.
- Reserved-but-not-enforced built-in names — users can shadow `os`/`arch`/`env` if they want, since the override mechanism works the same way for any name.

**Deferred:**

- Musl detection on Linux. Requires header probing (`#include <features.h>`) and is genuinely tricky because musl defines `__GLIBC__` for compat in some headers. Defer until a real consumer needs to differentiate.
- Freestanding / embedded support (`os=freestanding`). Reserved in the documented taxonomy but not detected, since stdlib currently assumes a hosted environment (`abort`, `malloc`, `alloca`, etc.).
- `--target x86_64-linux-gnu` shorthand. Per-axis `--flag` works fine for cross-compilation; sugar can come later if it proves valuable.
- Native MSVC support is **out of scope**. FC's stdlib is built around the MinGW/MSYS2 environment on Windows, and native MSVC would need separate header bindings, separate detection logic (the `cc -dM -E` probe assumes a GCC/Clang-style toolchain), and a different stdlib implementation in many places. The platform.c detection rules still recognize `_MSC_VER` and would set `env=msvc` if it ever fired, but no stdlib branch uses that value and `env=msvc` is not a documented part of the spec.

---

## Design Decisions (from Active section)

### Platform contract

FC targets **C11 hosted implementations with heap allocation** — specifically, GCC or Clang with a libc that provides heap allocation and standard string/formatting functions. This covers:

- Desktop/server (Linux, macOS, Windows/MinGW)
- Mobile (iOS, Android NDK)
- "Rich" embedded (Raspberry Pi, ESP32 with ESP-IDF, Arduino with avr-libc, ARM Cortex-M with newlib/picolibc)

The common thread: these all provide `malloc`, `free`, `abort`, `memcmp`, `strlen`, `snprintf`, and stack-dynamic allocation (`__builtin_alloca`). That's FC's platform contract.

**Not supported:** C11 freestanding (no libc), static-memory-only bare metal, custom allocator-only environments. Those users are writing C directly — FC adds no value there because too much of the core language (alloc, string interpolation, bounds-check abort) depends on a hosted-like environment.

### Core language headers

The compiler emits C code that depends on a small set of C headers. These are split into two tiers:

**Always emitted** (FC's platform contract — every FC program needs these):
```c
#include <stdint.h>     // FC's fixed-width type system (int8_t, uint64_t, etc.)
#include <stddef.h>     // isize/usize → ptrdiff_t/size_t, NULL
#include <stdbool.h>    // bool
#include <stdlib.h>     // malloc, free, abort
#include <string.h>     // memcmp, strlen
```

**Feature-gated** (emitted only when the program uses the relevant feature):
```c
#include <stdio.h>      // string interpolation (snprintf)
#include <math.h>       // float type properties (NAN, INFINITY)
#include <float.h>      // float type properties (FLT_MAX, DBL_MAX)
```

Note: `inttypes.h` is not needed in emitted C — string interpolation uses `%lld`/`%llu` with `(long long)` casts rather than `PRId64`/`PRIu64` macros.

A pure-integer program with no string interpolation and no floats gets a 5-header preamble. These 5 headers are available on essentially every platform with a libc.

**`alloca` portability:** Replace all `alloca()` calls in codegen with `__builtin_alloca()`. GCC and Clang both support this intrinsic on every target. This eliminates the `<alloca.h>` header, which is not in the C standard and has inconsistent availability across platforms.

### C functions the core language emits

| Function | Used by | Header |
|----------|---------|--------|
| `malloc` | `alloc` expressions | stdlib.h |
| `free` | `free` expressions | stdlib.h |
| `abort` | bounds checks, div-by-zero, option unwrap failure | stdlib.h |
| `memcmp` | structural equality for slices/strings | string.h |
| `strlen` | `(str)cstr` cast (wrap pointer with length) | string.h |
| `__builtin_alloca` | string interpolation buffers, `(cstr)str` cast | (compiler builtin, no header) |
| `snprintf` | string interpolation formatting | stdio.h (feature-gated) |

### Stdlib is just FC files

The FC standard library (`stdlib/`) is ordinary FC source code that uses extern declarations to wrap C functions. It receives **no special compiler treatment**. Users include it by passing the files to the compiler:

```bash
# No stdlib — pure computation, only core headers emitted
./fc main.fc -o out.c

# With I/O — io.fc's `module c from "stdio.h"` pulls in <stdio.h>
./fc main.fc stdlib/io.fc -o out.c

# Full stdlib
./fc main.fc stdlib/io.fc stdlib/sys.fc -o out.c
```

The extern `from_lib` declarations in stdlib files (and any user-written extern modules) drive additional `#include` emissions. If you don't compile a stdlib file, you don't get its headers. This is the intended mechanism, not a workaround.

### Stdlib surface area

The stdlib wraps C APIs organized by portability tier:

**Tier 1 — C11 standard** (works everywhere FC's platform contract holds):
- `io` — fopen, fread, fwrite, fclose, fflush (stdio.h)
- `math` — sin, cos, sqrt, pow, etc. (math.h) — future
- `text` — string manipulation utilities — future

**Tier 2 — POSIX** (Linux, macOS, BSDs, most embedded with newlib/picolibc):
- `sys` — getenv, exit (C11 portion), clock_gettime, nanosleep (POSIX portion)
- Future: `fs` (filesystem operations), `thread` (pthreads)

**Tier 3 — Platform-specific** (user-provided, not part of FC's stdlib):
- Windows APIs, vendor SDKs, hardware-specific libraries — users write their own extern bindings

Tier 1 modules should work on every platform FC targets. Tier 2 modules work on POSIX systems. Users choose what to include by which files they pass to the compiler.

### `define` annotation on extern modules

C libraries sometimes require feature-test macros (`_POSIX_C_SOURCE`, `_GNU_SOURCE`, `_WIN32_WINNT`, etc.) to be `#define`d before their headers are included. Rather than requiring users to know which flags each library needs at the build command level, this information lives with the extern declaration:

```fc
module t from "time.h" define "_POSIX_C_SOURCE" "200809L" =
    extern clock_gettime: (int32, any*) -> int32
    extern nanosleep: (any*, any*?) -> int32
```

Codegen emits `#define _POSIX_C_SOURCE 200809L` before `#include <time.h>`. Rules:
- Multiple modules defining the same macro with the same value: deduplicated silently
- Multiple modules defining the same macro with different values: compile error
- Defines are emitted before all `#include` lines (feature-test macros must precede headers per POSIX spec)

This replaces the current `_POSIX_C_SOURCE` auto-detection hack for `time.h`. The define is co-located with the declaration that needs it — passing `sys.fc` to the compiler brings everything it needs with it.

### Conditional compilation

The `#if`/`#else if`/`#else`/`#end` system and `--flag` CLI option remain for **user-defined** conditional compilation:

```bash
./fc main.fc --flag posix stdlib/io.fc stdlib/sys.fc -o out.c
```
```fc
#if posix
    import sys from std::
    let t = sys.time()
#end
```

The previously planned `--target` flag with automatic `target_embedded` / `target_bare_metal` built-in flags is dropped. The `target_hosted` built-in flag is also dropped — since every FC program targets a hosted environment per the platform contract, the flag is always true and therefore meaningless. Platform-specific behavior is controlled by user-defined flags and by which files are passed to the compiler.

### Implementation changes required — all complete

| Current state | Target state | Status |
|---------------|--------------|--------|
| 10 headers always emitted in preamble | 5 core headers always + feature-gated | Done |
| `alloca()` + `<alloca.h>` | `__builtin_alloca()`, no header | Done |
| `_POSIX_C_SOURCE` auto-detected for `time.h` | `define` annotation on extern module declarations | Done |
| `target_hosted` built-in flag always set | Drop (platform contract makes it redundant) | Done |
| Planned `--target` with `target_embedded`/`target_bare_metal` | Drop entirely | Done (never existed) |
| Stdlib implicitly expected | Stdlib is explicitly passed as source files | Done |

### Standard library namespace and structure

- Standard library lives in a `std::` namespace (distinct from `global::` used by application code)
- Flat module structure — one module per file, no nesting — because FC does not allow cross-file module definition
- Defined modules: `io` (file I/O — see spec §std::io), `sys` (system operations — see spec §std::sys)
- Import pattern: `import io from std::`, `import sys from std::`, etc.
- Resolved: explicit import required per file. No auto-imports — matches FC's "no magic" philosophy.

### Slice/pointer provenance and safety — current situation

All pointer/slice creation paths produce the same types with no provenance information:

| Source | Storage | Safe to return? | Safe to free? |
|--------|---------|-----------------|---------------|
| `alloc(T)`, `alloc(T, N)`, `alloc(expr)` | Heap | Yes | Yes |
| `"hello"`, `c"hello"` | Static read-only | Yes | **No** |
| `&x` on `let mut` | Stack | **No** | **No** |
| `T[N] { }` array literal | Stack | **No** | **No** |
| `"hello \{x}"` interpolation | Stack (`alloca`) | **No** | **No** |
| `(cstr)str_value` cast | Stack (`alloca`) | **No** | **No** |

The `alloc(slice)` deep-copy pattern mitigates this — `alloc(s)!` promotes any stack slice to heap-owned memory. But the programmer must know when to use it.

### `T**` and C interop

FC supports `T**` in the type system — the compiler, parser, and type checker all handle multi-level pointers. The main C use case for double pointers is out-parameters, where a function writes a pointer back through `T**`:

```c
int sqlite3_open(const char *filename, sqlite3 **ppDb);
long strtol(const char *str, char **endptr, int base);
```

**In pure FC**, `T**` out-parameters work directly. However, idiomatic FC rarely needs this pattern because functions return values directly, and option types / struct returns handle the cases where C would use an out-parameter.

**At the C boundary**, `cstr*` (uint8**) is automatically cast to `char**` by the compiler, following the same pattern as the `cstr` → `const char*` cast for single-level pointers.

**Opaque handle out-parameters** (e.g. `sqlite3_open`'s `sqlite3**`) are also handled automatically. Since FC represents opaque C types as `any*`, the out-parameter type is `any**` — which emits as `void**` in C. The compiler handles this by emitting a `(void*)` cast at the extern boundary.

### What we're not doing

- Borrow checker / ownership system (Rust-style) — too complex, conflicts with "manual memory, maps to C" philosophy
- Runtime provenance tagging — conflicts with zero-cost philosophy
- Automatic reference counting — same

### Closures at extern boundaries — wrapper function pattern (future)

Currently, only top-level functions and non-capturing lambdas can be passed to extern C functions. The compiler generates static trampolines (`_ctramp_*`) that strip the `void* _ctx` parameter and call the known function directly with `NULL`. This is conservative but complete — it is correct for all C callback APIs.

The limitation shows up when writing FC stdlib wrappers. A wrapper like `let sort = (arr: int32[], compare: (int32, int32) -> int32) -> ...` receives `compare` as a fat pointer `{fn_ptr, ctx}`. When forwarding to the extern `qsort`, it's rejected because it's a local function value, not a top-level function or literal lambda.

#### Design space explored

C callback APIs fall into three categories:

1. **Synchronous callbacks** (`qsort`, `bsearch`) — callback is invoked before the extern returns. A thread-local stash approach works: stash the fat pointer's `fn_ptr` and `ctx` before the extern call, generate a static trampoline that reads from the stash. Sound because the stash is valid for the duration of the call.

2. **Deferred callbacks with user-data** (`pthread_create`, most event loops) — the C API provides a `void*` parameter that gets passed through to the callback. The context could be threaded through this parameter without a stash. Fully sound but requires different codegen than category 1.

3. **Deferred callbacks without user-data** (`signal`, `atexit`) — no mechanism to carry context. Closures fundamentally cannot work here without runtime code generation. The non-capturing restriction is the only correct answer.

#### Why this is deferred

Implementing only category 1 (stash approach) would be a partial solution — it handles `qsort`-style wrappers but not `pthread_create`-style wrappers. Per the project's completeness principle, a partial solution that covers some APIs but silently breaks on others is worse than a conservative restriction that is uniformly correct.

#### Possible approaches if revisited

- **Thread-local stash for synchronous callbacks**: per-callsite `_Thread_local` fn_ptr/ctx slots with a static trampoline that reads from them.
- **Inferred restriction propagation**: if pass2 detects that a function parameter flows to an extern call, mark that parameter as "must be non-capturing" and enforce at call sites transitively.
- **Explicit `extern` function type annotation**: e.g. `compare: extern (int32, int32) -> int32` to mark a parameter as a bare C function pointer.
- **User-data threading for deferred callbacks**: for APIs like `pthread_create` that provide a `void*` arg, the compiler could pack the fat pointer context into that parameter.

A complete solution would need to handle at least categories 1 and 2 together. Category 3 always keeps the non-capturing restriction.

---

## C interop and embedded: remaining gaps (2026-03-22, updated 2026-03-23)

Overview of what's solved and what's still missing for full C interop and embedded platform support.

**Solved:**
- Extern declarations with automatic boundary casts (cstr → `const char*`, any* → `void*`, cstr* → `char**`, any** → `void*`)
- Variadic extern functions (printf, snprintf, etc.) with C default argument promotions
- `isize`/`usize` for platform-native types (size_t, ptrdiff_t)
- Opaque pointers (`any*`) for C handles (FILE*, sqlite3*, etc.)
- `c"..."` literals for null-terminated strings; `str` ↔ `cstr` casts
- Raw pointer arithmetic as escape hatch from slice overhead
- Function pointer params in extern (non-capturing lambdas extract fn_ptr automatically)
- Extern struct: C struct layout import via `extern struct C_NAME [as fc_name]` in `from` modules
- Extern union: C untagged union import via `extern union C_NAME [as fc_name]` in `from` modules, with memcmp-based equality
- Conditional compilation (`#if`/`#else`/`#end`) with built-in and user-defined flags
- Escape analysis: compile-time detection of returning stack pointers/slices, freeing non-heap memory, storing stack pointers in heap structs
- `const` qualifier for pointer/slice types: deep const, `const cstr` → `const char*` vs `cstr` → `char*` at extern boundaries, string/cstring literals infer const, write/free/address-of rejection through const
- Extern constants: `extern C_NAME [as fc_name]: type` imports C `#define` constants with type validation (scalar/pointer/cstr only) and automatic cstr boundary casts
- Fixed-size inline array fields (`T[N]`) in structs and extern structs

**Deferred gaps (outside FC's platform contract):**

- **No inline assembly** — can't emit platform-specific instructions. For GPIO toggling, interrupt handlers, etc., need a C wrapper file.
- **No `volatile`** — relevant for memory-mapped I/O registers on embedded.
- **No bitfield structs** — C bitfields (`uint32_t flags : 4`) are common in hardware register definitions.

---

## Resolved Items

### Extern constants — `#define` interop (resolved 2026-03-27)

C constants defined as `#define FOO 42` can now be imported using the existing `extern` syntax with a non-function type: `extern FOO: int64`. The mechanism requires no special codegen — the C macro name is emitted directly into the generated code, and the C preprocessor expands it after the `#include`. The `as` clause works for renaming: `extern SDL_INIT_VIDEO as sdl_init_video: int64`.

Type validation rejects types with no C `#define` equivalent: slices, options, structs, unions, void. For `cstr`-typed constants, codegen emits a `(const uint8_t*)` cast to bridge the `char*` / `uint8_t*` signedness difference at the C boundary.

Function-like macros (`#define MAX(a,b) ...`) are code transformations, not constant values, and remain outside the scope of this feature. Users wrap them in C helper functions or reimplement in FC.

### Function pointer trampolines at extern boundaries (resolved 2026-03-22)

FC functions internally carry an extra `void* _ctx` parameter for closure support. When a non-capturing function or lambda is passed to a C extern expecting a plain function pointer, the compiler now generates a static trampoline that drops the `_ctx` and matches the C calling convention. This happens automatically — the programmer just passes the function. Documented in spec §C interop with a `qsort` example.

### `const` qualifier for pointer/slice types (resolved 2026-03-22)

Added `const` as a type qualifier for pointer and slice types. `const` means "can't write through this indirection" — orthogonal to `let`/`let mut` which controls binding reassignment.

#### Design
- **Syntax**: `const` prefix on pointer/slice types only: `const int32*`, `const str`, `const int32[]`
- **Deep const**: accessing pointer/slice fields through a const pointer gives const versions. If `const node*` and node has `next: node*`, then `p->next` is `const node*`. Value fields are simply not writable through const (no type change needed). Chains naturally: `const_ptr->next->next` propagates.
- **Literal inference**: `"hello"` → `const str`, `c"hello"` → `const cstr` (data is in read-only .rodata)
- **Coercion**: non-const → const implicit (safe direction via `type_can_widen`). Const → non-const requires explicit cast: `(str)const_str_value`
- **Casts both directions**: `(const int32*)ptr` to freeze, `(int32*)const_ptr` to strip. Regular cast syntax.
- **Struct fields**: can be declared `const` — `struct config = name: const str`
- **`.ptr` on const slice**: gives `const T*`
- **Write rejection**: assignment through const pointer/slice, address-of through const, free of const — all rejected with `diag_error`
- **Generics**: type variables can bind to const types; `type_substitute` preserves `is_const`; unification allows non-const → const
- **Extern boundaries**: `const cstr` → `const char*`, non-const `cstr` → `char*`
- **Equality**: constness ignored for eq function generation/dedup
- **Orthogonal to provenance**: const = write permission, provenance = storage location

#### What const is NOT
- No `const` at binding sites (FC has no type annotations on `let` — always inferred from RHS)
- No branch widening (if/match branches with `const T*` and `T*` require explicit cast — deferred to future branch widening feature)
- No `const` on bare value types (`const int32` is a parse error)

#### Implementation
- `bool is_const` field added to `struct Type` in `types.h`
- Singleton const types: `type_const_str()`, `type_const_cstr()` for literal inference
- `type_make_const()` helper: shallow-copies pointer/slice with `is_const=true`
- `type_eq()` checks `is_const` for pointer/slice; `type_eq_ignore_const()` for eq dedup
- `type_can_widen()` extended with non-const → const widening and option inner widening
- `TOK_CONST` keyword, `apply_const()` parser helper (handles pointer/slice/option-wrapping)
- `is_write_through_const()` recursive checker in pass2 for assignment targets
- `mangle_type_name()` prefixes `const_` for mangled names
- `emit_type()` emits `const ` prefix on C pointers; same slice typedefs for const/non-const
- 21 new tests in `tests/cases/const/` (13 success, 8 error)
- 614 total tests passing

### Stack escape analysis (resolved 2026-03-22)

Lightweight intraprocedural escape analysis added to pass2. Every expression that produces a pointer or slice type is tagged with a **provenance** (`PROV_UNKNOWN`, `PROV_STACK`, `PROV_HEAP`, `PROV_STATIC`) tracking where its backing storage lives. The analysis catches three classes of bugs at compile time:

1. **Returning stack-derived pointers/slices** — `return &local`, returning array literals, interpolated strings, `(cstr)str` casts, subslices of stack arrays, or any of these through let bindings, if/match branches, blocks, some-wrapping, pointer arithmetic, or pointer casts.
2. **Freeing non-heap memory** — `free("hello")` (static), `free(&x)` (stack), `free(array_lit)` (stack), `free(interp_string)` (stack).
3. **Storing stack pointers in heap structs** — `alloc(node { data = &local })` where a struct field is a stack-derived pointer or slice.

#### Provenance sources
- `PROV_STACK`: `&x` (address-of local), array literals, interpolated strings, `(cstr)str` cast (alloca copy), subslice of stack
- `PROV_HEAP`: all forms of `alloc()` — including `alloc(stack_slice)` which deep-copies to heap
- `PROV_STATIC`: string literals `"..."`, cstring literals `c"..."`
- `PROV_UNKNOWN`: function parameters, call return values, extern results (no interprocedural analysis)

#### Propagation
Provenance flows through: let bindings, identifiers, if/match branches (conservative merge — any STACK branch → STACK), blocks (last expression), some/unwrap, pointer arithmetic, pointer casts, subslices, `.ptr` on slices.

#### Design decisions
- **Intraprocedural only**: function parameters are `PROV_UNKNOWN`. A function receiving a pointer is allowed to return it — the caller is responsible for correctness.
- **Conservative on reassignment**: `let mut` reassignment does not update provenance (tracks initial binding only). This may produce false positives if a stack pointer is later reassigned to heap, but is safe.
- **`alloc(stack_data)` produces PROV_HEAP**: the whole point of `alloc(s)!` is to promote stack data to heap. The provenance of the input is not propagated.
- **Option types checked recursively**: `some(&x)` returns `int32*?` which carries `PROV_STACK` — the option wrapper doesn't hide the provenance.

#### Implementation
- `Provenance` enum and `prov` field added to `Expr` in `ast.h`
- `LocalBinding` extended with `prov` in pass2; `scope_add_prov()` propagates through let bindings
- Three check points in pass2: explicit `return`, implicit return (last expression in function body), `free()`, and `alloc(struct_lit)` fields
- 36 new tests in `tests/cases/escape/` (18 error tests, 18 success tests)
- 593 total tests passing

### Native platform-width types `isize`/`usize` (resolved 2026-03-22)

Added `isize` (signed, pointer-width) and `usize` (unsigned, pointer-width) as opt-in types for C interop and embedded targets. FC's defaults remain fixed-width: `int32` for default integers, `int64` for `sizeof` and slice `.len`. The native types are escape hatches for when exact platform type matching matters.

- **Codegen**: `isize` → `ptrdiff_t`, `usize` → `size_t` (resolved by the C compiler, not FC)
- **Literal suffixes**: `42i` (isize), `42u` (usize) — bare `i`/`u` without width digits
- **No implicit widening**: explicit casts required in both directions between isize/usize and fixed-width types.
- **Type properties**: `.bits`, `.min`, `.max` are platform-dependent (emitted as C expressions)
- **Generics**: type variables can bind to isize/usize; monomorphization works normally
- **Operators**: arithmetic, comparison, bitwise, shifts all work between same-type operands
- 548 tests passing (18 new native_types tests).

### `char32` type and `str32` status (2026-03-24)

FC defines `char` as an alias for `uint8` with character literal syntax (`'a'`, `'\n'`, `'\x41'`). `str32` (alias for `uint32[]`) was partially implemented: it parses and type-checks, but has no runtime support (no `str32` literals, no `str32` interpolation, no `str32`↔`str` conversion). Decision: remove `str32` until Unicode support is properly designed. Removed 2026-03-27.

### Generic type variable soundness — mixed type-var arithmetic (2026-03-24)

#### Problem
Binary operations on different type variables (`'a + 'b`, `'a > 'b`) are currently allowed at template time, but the result type is unsound when widening is involved. During template checking, the type-var early return (pass2.c) picks one operand's type arbitrarily as the result. When instantiated with types that widen (e.g., `'a = int32`, `'b = int64`), the inferred return type (`int32`) doesn't match the actual computed type (`int64`).

#### Proposed fix
Restrict mixed type-var binary operations: require both operands to have the same type variable (e.g., `'a + 'a` ok, `'a + 'b` error at template time, concrete + `'a` ok since it pins `'a`). This is conservative-but-complete.

#### Also noted
`type_name()` for generic function types doesn't show explicit type parameters. Low priority.

### Field access on type variables — structural generics (explored, deferred 2026-03-22)

Explored allowing field access on bare type variables, e.g. `let sum = (p: 'a) -> p.x + p.y`, where `'a` is resolved to a concrete struct at monomorphization.

#### What was prototyped
- A new type kind `TYPE_FIELD_OF(base_type, field_name)` representing "the type of field F of type T", resolved during `type_substitute` when the base type becomes a concrete struct.
- 17 tests passing: basic access, arithmetic, comparison, let binding, nested access (3 levels deep), multiple struct types, chained generic calls, multi-type instantiation.

#### Why it was deferred
- **High complexity relative to value.** Completing it properly would require changes to `unify()`, match exhaustiveness, unary operators, and monomorphization-time error reporting.
- **Incomplete error reporting.** Invalid field access was not caught until C compilation.
- **Trivial workaround exists.** Instead of `let sum = (p: 'a) -> p.x + p.y`, write `let sum = (x: 'a, y: 'a) -> x + y`.
- **Rare in practice.** "Any struct with field X" is more natural in TypeScript or Go than in a C-targeting systems language.

### Multiline struct literals, function calls, and array literals (resolved 2026-03-21)
Bracket depth tracking added to the lexer layout pass. When inside `()`, `[]`, or `{}`, `INDENT`/`DEDENT`/`NEWLINE` tokens are suppressed. Multiline struct literals, function calls with many arguments, and array literals all parse naturally. Trailing commas permitted. No parser changes needed.

### alloc/free/sizeof/default placement — no change (resolved 2026-03-21)
Considered moving to a `sys` module. Resolved by convention: `destroy` is the idiomatic name for user cleanup; `free` stays reserved for raw deallocation. Regularizing as generic functions in a module was rejected because `alloc(expr)` can't coexist with `alloc<T>()` under no-overloading, and `sys` would be compiler magic pretending to be a module.

### Unreachable pattern detection — deferred (resolved 2026-03-21)
Low priority since it's a warning, not a correctness issue. The Maranget infrastructure is in place; unreachable arm detection is the dual of exhaustiveness (call `find_witness` against preceding arms). Small addition when needed.

### Module-scoped imports — implemented (2026-03-25)

Imports now follow lexical scoping rules, matching `let` binding semantics. Implemented and reverted once (2026-03-24), then re-implemented with a new architecture (2026-03-25) using ImportTable/ImportRef with live source references instead of the previous dual-registration approach.

**New architecture:** Uses `ImportTable` with `ImportRef` entries that store lightweight references (local_name, source_name, source_members pointer) instead of copying symbol data. Lookups go through the source module's live member table, avoiding the stale NULL-type problem. An `ImportScope` linked list (stack-allocated, pushed/popped on module entry/exit) provides arbitrarily deep nested visibility — child modules inherit parent imports with shadowing.

**Key design points:**
- `import * from M` only exports M's own declarations (`members`), never M's imports (non-forwarding)
- File-level imports are scoped per-file via `FileImportScopes`, preventing cross-file leakage
- Module-level imports are stored in `Symbol.imports`, separate from `members`
- Lookup chain: local scope → module members → import scope chain → global declarations
- Later imports shadow earlier imports (same scope level); inner scopes shadow outer
- On-demand type checking for imported functions uses source_members context switch

### Eager type resolution — not viable (deferred indefinitely, 2026-03-21)
Resolving struct field types and union variant payloads in-place on registered types is blocked by self-referential structs. A struct like `node { next: node*? }` creates a cyclic type graph when its field type is resolved. The existing on-demand `resolve_type()` calls in pass2 and `resolve_struct_stub()` in codegen remain the correct approach.

### Codegen: nested option/slice typedef ordering (resolved 2026-03-20)
- `collect_types_in_type()` now recurses into inner types before adding the outer type to the typeset, ensuring dependency typedefs are emitted first in the generated C.
- Fixed for both option types (`int32??`, `int32???`) and slice types.

### M9: std::sys module, main args as str[], conditional compilation, cstr→str cast (resolved 2026-03-17)
- `std::sys` module (`stdlib/sys.fc`): `env`, `exit`, `time`, `sleep` — pure FC wrapping C stdlib via extern declarations.
- Main function signature changed from `(args: int32)` to `(args: str[])`. Codegen emits `fc_main(fc_slice_fc_str args)` for user code plus a C `main` wrapper that converts `argc`/`argv` to `str[]` via `alloca`.
- Conditional compilation: `#if`/`#else if`/`#else`/`#end` directives implemented as a token-level filter.
- `(str)cstr` cast implemented: wraps existing pointer with `strlen`-derived length (no copy).
- 413 tests passing.

### Implicit widening in generic function calls (resolved 2026-03-15)
- Generic function arguments with concrete parameter types now auto-widen, matching non-generic call behavior.
- Widening only applies to parameters that contain no type variables. Type variable binding via unification still requires exact matches.

### Implicit widening in struct literals and variant constructors (resolved 2026-03-15)
- Struct literal fields and union variant payloads now auto-widen, matching function call argument behavior.

### Structural equality, codegen safety, spec alignment (resolved 2026-03-15)
- Structural `==`/`!=` implemented for all types: structs (field-by-field), unions (tag + payload), slices (element-wise), str (len + memcmp), options (has_value + inner), func (fn_ptr + ctx). Generated `fc_eq_T` comparison functions.
- Codegen safety: signed overflow wrapping via cast-through-unsigned; shift amount masking; integer division/modulo by zero emits `abort()` check.
- Pointer ordering (`<`, `>`, `<=`, `>=`) now accepted.
- Address-of (`&x`) now rejects immutable `let` bindings.
- String literal pattern matching in `match` implemented.
- 326 tests covering all milestones M1–M8.

### File handles are `any*`, not a built-in type (resolved 2026-03-11)
- The `file` built-in type was removed. File handles are `any*` — the same opaque pointer type used for any C resource.
- File operations moved from built-in `file.open`/`file.close`/etc. to `std::io` module.

### Extern declarations, std::io module, print→io.write migration (resolved 2026-03-16)
- `extern` declarations implemented: parse, pass1 registration, pass2 type-checking, codegen.
- `module ... from "lib"` syntax for C library source metadata.
- `stdlib/io.fc` written as a physical FC file wrapping C stdio via extern declarations.
- `stdin`/`stdout`/`stderr` are now built-in globals typed `any*`.
- `str→cstr` cast implemented via `(cstr)expr`.
- `print`/`eprint`/`fprint` removed as compiler operators. All I/O now uses `io.write(s, f)`.
- Null-sentinel optimization extended to `any*?` and `cstr?`.

### Stdlib completeness pass (2026-03-28)
Extended all three non-experimental stdlib modules:
- **io.fc**: added `seek`, `tell`, `eof`, `remove`, `rename`, and seek origin constants (`seek_set`/`seek_cur`/`seek_end`).
- **sys.fc**: added `parse_int32`, `parse_int64`, `parse_float32`, `parse_float64` (wrapping atoi/atoll/strtof/atof).
- **math.fc**: added `asin`, `acos`, `atan` (inverse trig), `fmod`, `hypot`, `trunc`, and pure-FC `is_nan`/`is_inf`/`is_finite` using type properties.

### std::math module (resolved 2026-03-27)
Added `stdlib/math.fc` wrapping C11 `math.h`. 2 constants (`pi`, `e`) and 14 functions: `sqrt`, `abs`, `pow`, `min`, `max`, `floor`, `ceil`, `round`, `sin`, `cos`, `tan`, `atan2`, `exp`, `log`, `log2`, `log10`. Float64 only — consistent with FC's explicit-cast philosophy. Also fixed float literal codegen precision (was `%g` / 6 digits, now `%.17g` for float64, `%.9g` for float32) and added `-lm` to test runners and `run.sh`. Tests in `tests/cases/stdlib/`.

### type_name() for generic function types (resolved 2026-03-27)
`type_name()` (used by `%T` interpolation and all diagnostic error messages) now shows explicit type parameters on generic function types. Added `type_params` and `type_param_count` fields to `TYPE_FUNC` in `types.h`, populated from `EXPR_FUNC.explicit_type_vars` during pass2, and propagated through `resolve_type()` and `type_substitute()`. Output: `<'a>() -> 'a` instead of `() -> 'a`.

### Capturing lambda context lifetime (resolved 2026-03-27)
Returning a capturing lambda created a dangling pointer — the compound literal context (`&(_ctx_fn){ .captured_x = x }`) has block scope in C11. The direct case (`return (x) -> x + captured`) was already caught by a pattern match on `EXPR_FUNC`, but the indirect case (`let f = (x) -> x + captured; f`) was not, because the return value was an `EXPR_IDENT` with `PROV_UNKNOWN`. Fix: capturing lambdas now receive `PROV_STACK` provenance, and `TYPE_FUNC` is included in `type_has_provenance()`. The general provenance-based escape check now catches all paths: direct return, indirect via let, conditional branches (conservative merge), and storing in heap structs. Non-capturing lambdas retain `PROV_UNKNOWN` since their `.ctx` is `NULL`. Tests: 4 error cases (indirect, explicit indirect, branch, alloc struct), 2 success cases (non-capturing return, local use).

### Generic mixed type-var arithmetic (resolved 2026-03-27)
Binary operations on different type variables (`'a + 'b`, `'a > 'b`) were unsound — the result type was picked arbitrarily, which produced wrong types when widening was involved (e.g., `'a = int32`, `'b = int64`). Fix: pass2 now rejects binary operators on different type variables at template time. Same type var (`'a + 'a`) and concrete+typevar (`int32 + 'a`) remain allowed. Conservative-but-complete — no partial fix that covers some cases but breaks others. Tests: updated 4 existing error tests, added `concrete_typevar_arith.fc`, `mixed_typevar_bitwise_err.fc`, `mixed_typevar_logical_err.fc`.

### Branch widening in if/match (deferred 2026-03-27)
if/match branches require exact type equality. Implicit widening would allow compatible types to unify (e.g., `const str` + `str` → `const str`, `int8` + `int32` → `int32`). Also affects loop return type unification via `break value`. Deferred permanently — significant complexity (numeric widening, const widening, loop types all interact) for marginal benefit. Users manually cast to unify: `(const str)non_const_expr`. This is consistent with FC's design: no implicit widening across branches avoids a class of subtle bugs.

### Stack array literal size enforcement (resolved 2026-03-27)
Pass2 now validates that the size expression in `T[N] { }` is `EXPR_INT_LIT`, rejecting runtime expressions with "array size must be a compile-time constant". Previously this only failed at C compilation.

### Remove str32 (resolved 2026-03-27)
`str32` (alias for `uint32[]`) had no runtime support — no literals, no interpolation, no conversion. Removed from compiler (`TYPE_STR32` eliminated, `str32` no longer parses as a builtin type). Can re-add when Unicode support is properly designed. See also: "char32 type and str32 status (2026-03-24)" and "True type aliases" entries below.

### Missing %p interpolation test (resolved 2026-03-27)
Added `tests/cases/strings/interp_ptr.fc` — exercises `%p` format specifier on pointer values in string interpolation.

### Stdlib signatures const qualifiers (resolved 2026-03-27)
Spec function signatures for `io.write`, `io.open`, and `sys.env` updated to include `const` qualifiers matching the implementation: `io.write(s: const str, f: any*)`, `io.open(path: const str, mode: const str)`, `sys.env(name: const str) -> const str?`.

### Deferred items archived (2026-03-27)
The following items were explicitly moved out of the active TODO as deferred/out-of-scope:
- **Closures at extern boundaries** — wrapper function pattern for passing closures to C callback APIs. Current non-capturing restriction is conservative-but-complete.
- **Unreachable pattern detection** — warning-only feature. Maranget infrastructure supports it as the dual of exhaustiveness. Small addition when there's demand.
- **Field access on type variables** — structural generics (`(p: 'a) -> p.x + p.y`). High complexity, trivial workaround exists (pass fields as separate parameters).
- **Inline assembly, volatile, bitfield structs** — outside FC's platform contract. Users write C wrapper files.
- **Eager type resolution** — not viable. Blocked by self-referential struct cycles. On-demand `resolve_type()` is the correct approach.

### True type aliases for str/cstr/str32, import-as alias propagation (resolved 2026-03-17)
- `TYPE_STR`, `TYPE_CSTR`, `TYPE_STR32` removed from `TypeKind` enum. `str` is now `TYPE_SLICE{uint8}`, `cstr` is `TYPE_POINTER{uint8}`, `str32` is `TYPE_SLICE{uint32}` — with a `const char *alias` field on `Type` for display names.
- `str` and `uint8[]` are fully interchangeable (same for `cstr`/`uint8*`, `str32`/`uint32[]`).
- `const char*` emission confined to extern call boundaries only.
- `import T as alias from M` now propagates the alias name to diagnostics and type signatures.
