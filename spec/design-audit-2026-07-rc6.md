# FC Language Design Audit — 2026-07-02 (v1.0.0-rc.6)

Scope: **language design only** — robustness, completeness, frictions, and highlights.
Not an implementation audit (for that, see `spec/hist/audit-2026-06-rc4.md`). Based on a
full read of `spec/fc-spec.html` (all 10 parts), `spec/examples.fc`, and `spec/TODO.md`.

---

## Overall verdict

FC is unusually coherent for a language at rc stage. The core bet — ML surface, C cost
model, "nothing clever in between" — is not just stated but actually enforced by the
design: nearly every restriction traces back to either the directional-inference theorem
or the no-hidden-costs rule, and the spec almost never has to say "this is arbitrary."
The robustness story is genuinely strong; the biggest weaknesses are ergonomic,
concentrated in two places: **error propagation** and **closures that can't escape**.

---

## Delights

- **The inference "design theorem" holds all the way down.** No annotations → no
  overloading → no context-dependent literals → monomorphized generics. Most languages
  state a philosophy and then leak exceptions; FC's one bounded exception (post-inference
  widening) is explicitly fenced. This is the spec's intellectual backbone and it's sound.
- **The two orthogonal context axes** (`checked`/`unchecked` × `guarded`/`unguarded`) are
  better factored than the precedents: Rust scatters this across `wrapping_add`/
  `checked_add` method families, Zig across `+%`/`@setRuntimeSafety`. FC's insight that
  *the operation's spelling states intent; the context states whether the precondition is
  verified* — with the guard axis governing C-UB preconditions and the overflow axis
  governing defined-but-lossy results — is a clean, teachable dividing line. The
  **redundant-marker compile error** is the kicker: a marker that survives compilation
  always changes the emitted code. That's a rare property.
- **"No implicit unbounded stack"** — the three-homes rule (bound it / `alloc` /
  `alloca`) for runtime-sized strings and `(cstr)` casts. Making the *compile error point
  at the menu of homes* turns a footgun into a teaching moment, and hoisted fixed buffers
  make bounded forms loop-safe. No other known language draws this line this precisely.
- **The `some()` null-payload guard.** The `T*?` null-sentinel optimization is standard;
  noticing that `some(null)` would silently alias `none` and closing it (compile error
  when provable, elided runtime guard otherwise) is the kind of edge-case honesty that
  separates a designed language from an accreted one.
- **Defined behavior everywhere C has UB, at stated cost**: wrapping signed arithmetic
  via cast-through-unsigned, masked shifts, saturating float→int, trapping division,
  fixed left-to-right evaluation order, and the `min / -1` decomposition across the two
  axes. The sub-`int` promotion handling (correct even on 16-bit-`int` targets) and the
  `fc_to_size`/`fc_to_int` narrowing asserts show the spec thinks about targets most
  languages ignore.
- **Tuples as synthesized structs** — equality, `default`, patterns, generics,
  monomorphization dedup all fall out for free, and the literal-index rationale ("wanting
  a computed index is the signal you want an array") is exactly right.
- **Small touches**: `%T` in interpolation; `none(i32)` as the symmetric partner to
  `some(3)`; hex floats; `void()` vs diverging `return`; per-iteration defer queues;
  bitwise-tighter-than-comparison precedence; the no-warnings stance (severity is binary,
  hygiene belongs to the LSP) stated with full conviction.
- **The spec itself**: transpilation notes at every feature, rationale sections, and
  honest hedging ("observed under configurations we test" for TCO) instead of
  overpromising.

---

## Glaring frictions

### 1. Error propagation (the big one)

FC leans on options and `result`-style unions, but offers no propagation affordance. The
unwrap-or-bail idiom is a 3-line `match` with a diverging arm, *per fallible call*.
Worse, generic union construction requires full explicit type args —
`result<i32, str>.err("fail")` — so the error path is the *most* verbose path in the
language. Deep call chains of fallible operations (exactly what parsers, I/O, and network
code look like) will be dominated by match ceremony.

This doesn't require traits or runtime machinery to fix: a Rust-`?`-style postfix
("unwrap or return the none/err upward") is a purely local desugar to the match you'd
have written — it fits the cost-transparency rule the same way `for` is sugar over
`loop`. Zig's `try` is the closest precedent in spirit. **Single highest-leverage
ergonomic gap in the language.**

### 2. Closures are stack-bound with no escape hatch — ✅ RESOLVED 2026-07-03

Capturing closures can't be returned, can't be stored in heap structs, and `let mut`
can't be captured at all. The workarounds (capture a pointer, heap-allocate a context
struct manually and thread it C-style) are honest but mean callback-oriented designs —
event handlers, `hash_dict` storing comparators beyond the creating frame, any registry
of behaviors — regress to C's `void* user_data` pattern. The transpilation already has
everything needed for an *explicit* heap-closure form (e.g. `alloc(closure)` producing a
heap context the caller frees, symmetric with how `alloc(s)` promotes a stack string).
Right now the feature cliff is steep: closures work beautifully until the moment they
need to outlive a frame, then vanish entirely.

*Resolution: `alloc(lambda)` → `F?` copies the context struct to the heap (Apple
Blocks' `Block_copy` precedent; option like every other alloc, `none` on failure); the
result has heap provenance so all escape restrictions lift through the existing
machinery. Operand must be a capturing lambda literal or an immutable `let` bound
directly to one (`alloc(f)` — needed because an indented block body can't appear inside
parens; the binding form memcpys the live context, sound because captures are immutable
copies). Anything else — parameter, `let mut`, conditional init — is rejected: a fat
pointer carries no context size (same constraint as Rust's `Box::new`). `free(f)` emits
`free(f.ctx)`; freeing a stack closure is a compile error; copies share one context.
Capture semantics unchanged (`let mut` still uncapturable — shared mutation stays
"heap cell + captured pointer", which now composes with escaping closures). Extern
boundary unchanged. Spec §Heap closures; tests `closures/heap_closure_*` (10 positive
incl. self-recursive, generic-body, nested-capture promotion; 6 error). All ASan-clean.*

### 3. A mutability-model seam

`let p = point{...}; p.x = 10` is legal (content mutation), but `&p` is not (only
`let mut` is addressable). So you can *write* a field you cannot *point at*. The "let
controls reassignability, not content" rule is internally consistent and the spec
explains it, but it will read as a contradiction to both camps FC borrows from: ML people
expect `let` to freeze contents; C people expect anything writable to be addressable. It
also interacts oddly with `const` being the *actual* content-immutability mechanism but
only existing for pointers/slices — there is no way to declare a struct binding whose
fields can't be assigned. Not a change proposal so much as a flag: this is the part of
the core model most likely to generate recurring user confusion, and the spec might
benefit from confronting the `p.x = 10` / `&p` asymmetry head-on rather than in separate
sections.

*✅ RESOLVED 2026-07-03 (documentation): new spec passage §let and let mut → "One rule,
three knobs" confronts the asymmetry in one place — the keyword governs the binding,
contents are always assignable; addressability tracks reassignability (`*pp = v` is
reassignment through an alias) and capturability is its complement (a copy of a
reassignable binding would go stale). Reframed per user: the audit's "ML people expect
`let` to freeze contents" was overstated — F# is the precedent, and its triple matches
FC exactly (`let`/`let mutable` reassignment split, `&x` requires mutable, mutable
locals uncapturable/FS0407, record `mutable` fields assignable through immutable
bindings). FC's real divergence from ML is only the flipped default: no per-field
immutability opt-in; `const T*`/`const T[]` views recover read-only-ness at access-path
boundaries. Acknowledged in-spec as a deliberate omission (no frozen-contents value
binding); `const` value bindings noted here as the natural extension point if demand
ever materializes — declined for now (no demand, rc-stage). Also fixed the misleading
"Value-level immutability is handled by `let`" line in §The const qualifier and added
the reassignment-through-alias rationale to §Address-of. No rule changes.*

### 4. Truncation-consistency wobble

FC's general stance is "trap on data loss" (fixed-array field assignment *aborts* when
`src.len > N`; `checked` traps narrowing casts), but `(cstr[N])` and `%.Ns` *silently
truncate*. The printf-semantics justification is real, but a byte-slice assignment
aborting while a string cast clips is two answers to the same question. Worth a
deliberate look — even if the resolution is just documenting why strings get strlcpy
semantics and arrays don't.

*✅ RESOLVED 2026-07-03: `checked` now governs the two truncating string forms —
`checked (cstr[N]) s` and a `checked` interpolation with a `%.Ns` segment abort
(`fc_trunc`, "string truncation in …: len=… max=…") instead of clipping; defaults
unchanged (silent clip, printf/strlcpy semantics). This closes the real gap: string
clipping was the only *defined* data loss the overflow axis didn't govern (narrowing
casts already sat there). The remaining default asymmetry is now documented as
principled (spec §Fixed arrays → Assignment): operations that spell their bound at
the use site clip to it; `=` into a fixed-array field spells no bound and a clipped
zero-filled binary copy would be indistinguishable from a short source — so its
length check is a guard, like slice bounds; clip deliberately via subslice
(`p.data = msg[0..n]`). Spec §Checked arithmetic; tests `checked/trunc_*`,
`generics/generic_checked_trunc*`.*

### 5. Cast noise at the i64/usize boundary

`slice.len` as `i64` is well-argued — but the spec's own examples show the cost: the
SPSC ring needs `(usize) r.buf.len` twice in six lines, and the canonical `strlen`
interop needs `(i64)`. Every program that touches both slices and C size types pays this
tax on nearly every line of the boundary. A knowing, documented trade — flagged only
because the friction is visible even in the spec's showcase examples, which suggests
users will feel it constantly.

### 6. Loop-condition inversion

With no `while`, the idiom is `loop / if not_ready() then break / ...` — every condition
reads negated relative to intent. `for` covers the common cases and the one-construct
argument is coherent, but this is the most-typed pattern in imperative code and it reads
backwards. (Compound assignment is not re-litigated here — that trade is stated and
priced in the spec.)

---

## Completeness gaps

- **Atomics RMW set** — already in `spec/TODO.md`; the analysis there (fetch_add →
  exchange → CAS, pointer publication gated on escape rules) is correct and correctly
  sequenced.
- **Bit reinterpretation.** ✅ RESOLVED 2026-07-04 (shipped). `f32`↔`u32` bit inspection
  required the pointer-cast-through-temporary dance. For a systems language this is a common
  need (hashing floats, serialization); a `bitcast`-style operation would be zero-cost and
  in-spirit. The spec acknowledges the gap; it's worth an eventual answer.
  *Resolution: shipped a builtin `bitcast(T, x)` (mirrors `sizeof`/`alignof`), scalars-only
  and fixed-width, with a **static** equal-size check (a mismatch is a compile error, not a
  runtime guard) and a C11 union type-pun lowering — which also fixes a real bug: the old
  pointer-cast workaround was strict-aliasing UB that `-O2` may miscompile. `usize`/`isize`
  and `bool` are excluded (target-dependent width / no invalid-representation guarantee);
  aggregate bitcast is punted (padding hazard; covered by pointer overlay / explicit
  shift-packing). Because a size-matched reinterpretation is statically total, `bitcast`
  sits on neither the checked/unchecked nor the bounded/unbounded axis. See §bitcast in the
  spec; implementation record in `spec/hist/archived-todos.md`.*
- **No C-style enum / integer exhaustiveness.** The "module of i32 constants" pattern for
  C enums is fine at the boundary, but matching on such constants can't be
  exhaustiveness-checked — you always need `_`. Pure-FC code has unions, so this only
  bites interop-heavy code; acceptable, but one of FC's headline safety features
  (exhaustive match) goes dark exactly where C values enter.
- **Numeric option bridging.** ✅ RESOLVED 2026-07-03 (example fixed; limitation kept).
  `i32?` not widening to `i64?` is representation-honest,
  but the prescribed bridge (`if x.is_some then some((i64) x!) else none(i64)`) is clunky
  enough that people will write helper functions per type pair. Genuinely rare, but the
  spec's own workaround is the least pleasant line in it.
  *Resolution: the limitation stands, but the spec's bridge example now shows the
  idiomatic form — `match x with | some(v) -> some((i64) v) | none -> none(i64)`.
  The `is_some`/`!` chain is an anti-pattern where a match does the job; `!` is for
  cases where matching is genuinely not an option.*
- **Unicode posture is implicit.** ✅ RESOLVED 2026-07-04. `str` is bytes, `char` is a byte — a defensible
  systems-language answer (it's C's), but the spec never *says* "FC strings are byte
  strings; UTF-8 is a convention of the data, not the type." One paragraph would prevent
  a class of user assumptions.
  *Resolution: added a "Strings are byte strings" subsection to §Slices & Strings that
  states the byte-string model outright — `str.len` is a byte count, FC decodes/validates/
  normalizes nothing, UTF-8 works under byte ops because it is ASCII-compatible, and the
  encoding lives in the data not the type (arbitrary-offset slicing can split a code point;
  code-point/grapheme/case/normalization are §std::text concerns).*
- **Spec gap — context markers × generics.** ✅ RESOLVED 2026-07-03. The redundancy rule says
  `unguarded (a / b)` on floats is a compile error, and generic-body validation is
  deferred to monomorphization. So what happens to `unguarded (a / b)` in a generic body
  instantiated at both `i32` (guard exists) and `f64` (marker is redundant)?
  Per-instantiation redundancy errors would make the marker unusable in generic code;
  silently waiving the rule for type variables is probably right but is currently
  unspecified. Same question for `checked`. Small, but it's a real hole in an otherwise
  airtight pair of sections.
  *Resolution: waived for type variables — the redundancy scan accepts a marker when
  some admissible instantiation would govern an operation in its body; instances where
  none is governed emit the plain op (no per-instantiation errors). A marker meaningless
  under every instantiation (`unguarded (a == b)`) is still rejected. Spec §Redundancy
  "Generic bodies" paragraph; `type_maybe_integer`/`type_maybe_signed` in pass2's
  governed-op predicates; tests `generics/generic_{unguarded_div,checked_add,
  checked_float_inst,marker_redundant,checked_redundant}`. Investigating this uncovered —
  and fixed — a far larger latent codegen bug: every defined-behavior emit decision
  (wrap casts, shift masks, div/mod guards, min/-1, checked traps, sub-int `~`) keyed on
  the raw type variable instead of the substituted type, so monomorphized generic bodies
  emitted bare UB-carrying C operators (`a << b` unmasked, no divide-by-zero guard, UB
  signed overflow). Fixed via `subst_resolve` at the EXPR_BINARY/EXPR_UNARY_PREFIX emit
  sites (equality already did this); tests `generics/generic_{wrap_semantics,
  div_zero_guard}`.*

---

## Robustness notes

The robustness ledger is impressive: non-uniform recursive generics rejected at
definition, infinite-instantiation detection, negative slice lengths rejected/trapped,
no-base-case self-recursion rejected, or-pattern binding-freedom, guards excluded from
exhaustiveness, escape-analysis taint that survives `let mut` reassignment and
loop-carried flows, and — critically — the *intraprocedural limitation documented with a
worked counterexample* rather than implied away.

The `default(T)` holes (null function values, null raw-pointer fields) are the one place
"no null" is soft, and the spec is candid about it; that candor is the right call given
the zero-init cost model, though it does mean the safety pitch needs the asterisk: FC
removes null from the *surface*, not the *memory*.

One residual: escape analysis trusting call boundaries means the
`stash(holder { p = &local })` dangle compiles. That's the documented, honest line — but
as the stdlib grows callback-taking APIs, this is where real-world dangling pointers will
actually come from. If ever revisited, an opt-in per-parameter annotation ("this callee
may store this pointer") would be more in FC's spirit than whole-program analysis.

---

## Ranked priorities

1. **Error-propagation sugar** — pure desugar, huge ergonomic payoff, zero cost-model
   violation.
2. **An explicit heap-closure escape hatch** — the `alloc` symmetry already exists in the
   language's vocabulary. ✅ RESOLVED 2026-07-03 (see item 2 above).
3. **The markers-in-generics spec gap** — cheap to specify now, annoying to retrofit.
4. **The truncation-consistency question and the mutability-seam documentation.**
   ✅ Both halves RESOLVED 2026-07-03 (see items 3 and 4 above).

Everything else on the friction list is priced-in trade-offs that the spec defends
adequately. The language's identity — "the discipline required to take the ML surface
without sacrificing the cost model" — is intact everywhere examined; the gaps are where
discipline shades into austerity for the two workflows (fallible call chains, escaping
callbacks) that real programs do constantly.
