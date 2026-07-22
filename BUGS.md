# Bug hunt — 2026-07-18 (branch `bugsearch`)

Systematic negative-space / coverage-completeness search over the whole feature
surface (9 parallel probe sweeps, ~400 probe programs, all findings re-verified
against `fcc 1.0.0-rc.6` @ 3e17dec). **45 confirmed bugs, each demonstrated by
one new failing test** in `tests/cases/`. Baseline before adding them: 2063
passed, 0 failed (gcc). After: 2063 passed, **45 failed** — every failure below
is intentional and should flip to PASS as its bug is fixed.

**Status: all 45 fixed** (+ many found and fixed along the way: §1.5 during §1's
triage, two variant-construction holes during §2.5, three more — two uncovered
spellings of §2.3/§2.4 plus a `for a, a` collision — caught by an adversarial
review of the §2 diff, three during §4 (a wrong-length string *pattern* compare,
an option typedef missing behind any pointer, and §6.1 which the §4.8–4.11
root-cause fix closed outright), and three during §5: §5.7, a module member
named `main` (two `fc_main` definitions; a compiler segfault on the zero-param
form), and a `let` destructure in a match arm emitting a placeholder comment).
§1 (parser / lexer), §2 (pass2 judgments), §3 (escape analysis), §4 (codegen
emits invalid C), §5 (C-identifier hygiene) and §6 (string interpolation
semantics) all complete — see those sections for what landed. §8
(diagnostics-polish) is now cleared of every item fixable without a design
decision (three left open with a note on why). §7 was the decide-first list of
genuine design questions — **all ten now resolved** (each with a recorded
decision and, where the decision was to change behavior, tests). Current
suite: **2290 passed, 0 failed** (gcc + clang, -O0 and -O2; LSP wire tests
green).

Several **new bugs** were found while doing the work and fixed in the same
sessions — see §4.14, §5.7, and §5's diff-review section.

Conventions:

- Run one bug's test: `make test-gcc FILTER=<test_name>`.
- The failure mode encodes the bug class: `expected error but compilation
  succeeded` = missing diagnostic; `C compilation failed` = fcc exit 0 with
  invalid C; `fc compilation failed` = wrong rejection of legal code;
  `exit code: expected 0, got 134` = compiles clean but runs wrong.
- `.error` files contain *guessed substrings* of the future diagnostic
  (conservative — e.g. just `stack` or `error:`). When fixing, tighten the
  substring to the real message.
- A few entries have two defensible fix directions; those are flagged
  **fix-direction** — decide (and if needed re-shape the test) before fixing.
- All original probe repros (plus ASan logs and many additional variants per
  bug) are under the session scratchpad
  `/tmp/claude-1000/-home-ryan-Projects-fc-lang/6a3ba3cd-25ec-403f-929c-cc49f5ab3c7a/scratchpad/probes/`
  — ephemeral; everything needed to fix is restated here + in the tests.

---

## 1. Parser / lexer gaps — ✅ ALL FIXED (2026-07-18)

All four fixed on branch `bugsearch`, plus 1.5 (found during triage); suite 2083
passed / 41 failed (the remaining failures are §2–§6), gcc + clang, -O0 and -O2,
LSP wire tests green. 15 tests added beyond the four repros. Two shared root
causes turned out to be twins-in-the-parser, closed by one helper each.

### 1.1 `expressions/juxtaposed_stmts_err` — statement separator never enforced ✅ FIXED
`parse_block` loops `parse_block_item` with only `skip_separators` *between*
items — it never requires a NEWLINE/`;` after an item. So `assert(a) assert(b)`,
`let x = 5 6` (the `6` silently discarded), `let x = 5 let y = 6`, and
`continue 5` (parses as `continue; 5`) are all silently accepted.
grammar.bnf's `block` rule requires the separator. Fix in `parse_block`
(src/parser.c ~1130): demand NEWLINE/`;`/DEDENT before starting the next item.

**Fixed:** `parse_block` now reports "expected a newline or ';' between
statements" when an item stops short of `at_stmt_terminator` (the existing
predicate — NEWLINE/`;`/DEDENT/`else`/EOF). Cascade guard: the check is skipped
for a line that already produced an error, so recovery debris never earns a
second diagnosis (`line_errs`, reset whenever separators are consumed).
`parse_inline_seq` is deliberately *not* changed — its `let … in` form ends at
`)`/`,` — but leftovers from an inline body are caught by the enclosing block
anyway (`if c then continue 5` is flagged).

Also catches the **cross-line spelling**: an over-indented line after a complete
statement is a *continuation* (the layout pass suppresses the NEWLINE), so it
juxtaposes too. New test `expressions/juxtaposed_continuation_err`. This found
one real instance in the user's sibling project — **`wolf-fc/src/input.fc:67`**,
where `loop` is over-indented 4 spaces under `let mut event = …` (harmless in
effect, but now a compile error; the fix is dedenting that `loop` and its body).
euler-fc, all demos, `spec/examples.fc`, and the stdlib are clean.

Collateral: three pre-existing tests pinned *incidental* pass2 diagnostics that
were only reachable through juxtaposition — `(e) 1` (a cast-shaped
juxtaposition), `1e9i32`, and `1e`. The syntax error now precedes them.
`enums/err_type_as_value` was re-pointed at the direct spelling (`let x = e`) so
it still pins "'e' is a type, not a value"; the two malformed-float tests briefly
expected the separator error and now pin 1.5's direct messages.

### 1.2 `strings/raw_newline_in_str_err` — raw newline in string literal ✅ FIXED
The lexer's string scanner doesn't stop at `\n`; the literal is accepted and
codegen copies the newline byte verbatim into the C string literal, which then
spans two lines → gcc "missing terminating \" character". **Fix-direction:**
reject in the lexer (consistent with unterminated-string fatals; the test
expects this) — or, if multi-line strings are ever wanted, escape control bytes
in the emitter. Escaping control bytes is needed anyway (see 4.10–4.11).

**Fixed** in the lexer (the fix-direction the test expected): `scan_string_body`
stops at an unescaped `\n` and reports "unterminated string", so a missing
closing quote is a one-line error instead of swallowing the file. The same
one-line rule was missing in `scan_char_lit` (a raw newline was silently taken
as the byte) and is now applied there too. Covers plain, `c"…"`, and
interpolated literals (they share the scanner). Spec §Escape Sequences states
the rule. Escaping control bytes in the emitter is still wanted for 4.10–4.11.

### 1.3 `slices/const_str_slice_lit` — `const str[2] { ... }` unparseable ✅ FIXED
`parse_prefix` recognizes slice literals only from a bare type token; a
`const`-prefixed head falls to the error default ("unexpected token unknown").
Per grammar.bnf the head is a `type_expr` (which admits leading `const`), and
`const str[...]` is the *only* spelling that can hold string literals (element
type `str` correctly rejects `const str` values) — so slices of string
literals are currently inexpressible. Legal program, wrongly rejected.

**Fixed** together with 1.4 — see below.

### 1.4 `generics/const_arg_slice_lit_elem` — `wide<64>[2]{}` parse error ✅ FIXED
Twin drift in parser.c: the array-literal lookahead scan uses
`is_type_arg_token` (admits int literals), but the element-type parse of
`<...>` calls `parse_type` instead of `parse_type_arg` — so a *const-arg*
instantiation can't be a slice-literal element type while the type-arg twin
`box<i32>[2]{}` and the in-generic `wide<'n>[2]{}` both work.

**Fixed (1.3 + 1.4 together).** Rather than patching the twin, the two
hand-rolled element-type parsers (the `TOK_IDENT` and `TOK_TYPE_VAR` arms of
`parse_prefix`) were replaced by one shape test plus `parse_type`:
`scan_type_head` / `at_slice_literal` decide *whether* a `type_expr [ … ] {`
starts here, and `parse_type` — which already owns `const`, dotted names, type
*and* const arguments (`parse_type_arg`), and the `? * !` suffixes — parses the
element type. A `TOK_CONST` arm was added to `parse_prefix` for 1.3. The split
is safe because `[` is never part of a type in expression position (`T[]` before
`{` is declined by `parse_type_suffix`; fixed-array `T[N]` is struct-field-only),
so the first `[` past the head always opens the literal. Dropping the twin also
fixed an unreported latent bug: `any*[N]{}` built `any**` on the old path.

### 1.5 malformed numeric literals diagnosed only indirectly ✅ FIXED
Not from the original hunt — surfaced by 1.1's triage (above) and fixed on the
same go-ahead. A numeric literal owns its suffix: every valid one (`i8`…`usize`,
`f32`/`f64`, hex-float `p`) is consumed by the scanner, so an identifier
character still adjacent to the literal can only be a malformed suffix. It used
to split into a number plus an identifier, and the mistake surfaced wherever
that landed — as juxtaposition (`1e9i32`), or an undefined name.

`scan_number` now wraps the scanner body and rejects a trailing identifier
character ("invalid suffix on numeric literal": `1e9i32`, `42foo`, `0x1fz`,
trailing `1_`), and an uncommitted `e`/`E` exponent gets its own message
("float exponent requires at least one digit": `1e`, `1e+`). No legal program is
affected — FC has no juxtaposition rule that gives `1x` a meaning, and the
exponent commit rule (`1e3 - 2` stays a subtraction) is untouched. Spec
§Literals states the rule. Tests: `expressions/numeric_suffix_adjacent_err`,
`hex_suffix_adjacent_err`, `digit_separator_trailing_err`, and
`numeric_suffix_forms` (positive: every valid spelling, incl. `1e3 - 2`); the
two `float_scientific_*` tests now pin the direct messages instead of the
incidental ones they inherited.

**Tests added** (15; the four listed under 1.5 plus): `expressions/juxtaposed_let_err`,
`expressions/juxtaposed_continuation_err`,
`control_flow/juxtaposed_after_continue_err`, `expressions/stmt_separators`
(positive: every legal separator form incl. `;`, inline bodies, `(a; b)`,
`let … in`), `strings/raw_newline_in_interp_err`,
`strings/raw_newline_in_cstr_err`, `expressions/char_raw_newline_err`,
`strings/escaped_newline_ok` (positive), `slices/const_slice_lit_forms`
(sized / raw-parts / option element), `slices/const_slice_lit_nonptr_err`
(`const i32[2]` still rejected), `generics/const_arg_slice_lit_forms`
(const arithmetic, module const, mixed type+const args, in-generic `'n`),
`slices/slice_lit_head_negative_space` (comparisons and indexing keep their
readings). Spec: §Continuation, §Escape Sequences, §Literals, §Slice literals
updated.

**Incidental findings, triaged.** All four were verified against 812180c (the
pre-fix build) to separate "newly introduced" from "newly visible":

- **Symptom changed by 1.1, cause pre-existing** — `(t) x` where `t` is a
  *user-defined* type name is not read as a cast (the parser's cast heuristic
  fires only for built-in type names or a `* < ! ? .` suffix), so it now reports
  the separator error instead of pass2's "'t' is a type, not a value". Only
  reaches programs that are illegal either way — a bare cast to a struct/enum
  type is never legal, and `(mytype*) p` / `(mod.t*) p` already parse as casts.
  Diagnostics-only. → tracked in §8.
- **Symptom changed by 1.1, cause pre-existing** — malformed numeric literals
  (`1e`, `1e9i32`) lex as two tokens and so are diagnosed as juxtaposition. The
  messages they used to get ("undefined name 'e'", "'i32' is a type, not a
  value") were equally incidental. → **fixed as 1.5 below.**
- **Pre-existing, made visible by 1.3** — slice-literal elements are checked
  with bare `type_eq`: no widening of any kind. `const i32*[2] { &a, &b }`
  errors "expected const i32*, got i32*", and so does `i64[2] { 1, 2 }`
  ("expected i64, got i32") — both identically before 1.3, which only made the
  const spelling reachable. The sibling aggregate positions *do* widen
  (`peek(&a)` into a `const i32*` param, `holder { p = &a }` into a
  `const i32*` field). Extending widening to this position is a language
  decision, not a repair. → tracked as §7.9.
- **Pre-existing and unchanged** — a juxtaposition inside a *match arm* body
  (`| 3 -> n = 1 n = 2`) yields a 3-error "expected '|'" cascade instead of the
  separator message (arm bodies go through `parse_inline_seq`, untouched by
  1.1). It was already an error before, just a noisy one. → tracked in §8.

**Also found while fixing (pre-existing, minor):** a slice literal whose element
type is *itself* a slice can't be spelled directly — `i32[][2] { a, a }` fails
("unexpected token ']'"), because the head scan stops at the first `[` and
requires the `{` to follow its match. Parenthesizing works —
`(i32[])[2] { a, a }` compiles — and the aliased form (`str[2] { … }`) was never
affected, so this is an ergonomics nit, not an expressiveness gap. → §8.

## 2. Missing type-checker (pass2) judgments — ✅ ALL FIXED (2026-07-18)

All ten fixed on branch `bugsearch`, plus two more found while fixing 2.5
(both silent wrong-runtime — see there); suite 2122 passed / 31 failed (the
remaining failures are §3–§6), gcc + clang, -O0 and -O2, LSP wire tests green.
30 tests added beyond the ten repros. `spec/examples.fc`, the stdlib, all five
demos, and both sibling projects (wolf-fc, euler-fc) still compile clean under
the new judgments.

Two entries were decided against what the repro test originally assumed; both
are called out below (2.8 reversed, 2.6 confirmed). Spec updated: §Data Types
(void), §loop, §Match Expressions, §Allocation (slice-literal length),
§Generic structs and unions, §Const Parameters.

### 2.1 `control_flow/loop_mixed_break_err` — bare `break` + `break v` in one loop ✅ FIXED
A value loop mixing `break` and `break 5` passes pass2; the emitted
`_loop_result` is unassigned on the bare-break path → clang
`-Wsometimes-uninitialized` rejects the C; gcc binaries read garbage (observed
wrong values at -O0). Loop-result unification treats a valueless `break` as
non-contributing instead of anchoring the loop type to void and reporting the
mismatch (the arm-disagreement diagnostic "break type mismatch" already
exists — extend it here).

**Fixed** exactly that way: EXPR_BREAK now computes `vt = value ? check(value)
: void` and runs the *same* unification for both, so a valueless break anchors
the loop to void instead of abstaining. A loop is therefore uniform — every
break carries a value of one type, or none does. The void-vs-value pairing
gets its own wording ("this loop mixes `break` with `break <value>`") since
"expected void" would name a type the programmer never wrote; the
value-vs-value case keeps the existing message verbatim. `for` is unaffected
(`break value` was already rejected there, so all its breaks are void and
agree). Tests: `loop_mixed_break_reverse_err` (value break seen first),
`loop_mixed_break_nested_err` (per-loop tracking — an inner void loop must not
license a mixed outer one), `loop_break_forms` (positive: all-bare, all-valued
with several breaks, nested, and `for` with a bare break).

### 2.2 `pattern_matching/match_void_subject_err` — void match subject ✅ FIXED
`match f() with` where `f` returns void emits `void _subj0 = fc__f(NULL);`.
pass2 rejects void in let bindings and call args but never checks the match
subject. Also reachable via `match (n = 5) with` (assignment is void).

**Fixed** in `check_match`, right after the subject's type resolves: the
subject is a value position (codegen binds it to a temporary), so void is
rejected there like anywhere else. Tests cover all three spellings — a void
call, `match (n = 5)` (assignment), and the literal `match void()`.

### 2.3 `generics/bare_generic_type_err` — bare generic name as a type ✅ FIXED
A generic struct name with no type args in a field type, `default(box)`, or
`alloc(box)!` sails through to codegen → `fc__box` referenced, never defined
(fcc exit 0, invalid C). The name never reaches `mono_register`'s choke point,
so the existing "requires explicit type arguments" judgment never fires —
classic "a choke point only covers what flows through it". Cover the bare-name
case in pass1/pass2 type resolution. (Related papercuts: bare generic *param*
type `(b: box)` is accepted silently if never called; slice-literal element
form errors with the odd "expected box, got box<i32>".)

**Fixed** at every place that turns a written type name into a type —
`resolve_type` (expression and annotation positions) and
`canonicalize_field_stubs` (struct and union member types, which never go
through `resolve_type` at all, which is why the field spelling was the one
that reached codegen). One shared helper, `reject_bare_generic_type`, so they
can't drift. Both related papercuts are fixed by the same helper and are now
tested (`bare_generic_param_err`, `bare_generic_slice_elem_err`), along with
`default`, `alloc`, and the generic *union* twin.

**One spelling was still leaking after the first fix** (caught by an
adversarial review of the diff, not by the suite): inside a **module**, pass1
has already resolved and mangled a field's type by the time pass2 walks it, so
an unqualified sibling reference (`inner: box` within `module m`) arrives as a
resolved `TYPE_STRUCT` with `type_arg_count == 0` — not a `TYPE_STUB` — and
sailed straight past a check written for stubs. Same hole through `box*`,
`box[]`, `box?`, and for a generic union. The helper now takes the pieces
(symbol, display name, arg count) instead of a stub node, and
`canonicalize_field_stubs` judges the resolved `TYPE_STRUCT`/`TYPE_UNION` arms
too. Tests `bare_generic_module_sibling_err` (all four field shapes) and
`bare_generic_module_union_err`. The lesson is the standing one: a judgment
added to one representation of a name has to cover every representation the
pipeline produces.

### 2.4 `generics/void_typearg_err` — `void` instantiates `'a` ✅ FIXED
`idv(v())` with `v` void-returning monomorphizes `idv<void>` and emits
`void x` as a parameter → invalid C. Generic argument unification lacks the
void-as-value guard the non-generic call path has.

**Fixed** at three levels, because the written and inferred spellings reach
instantiation by different routes. `resolve_generic_arg` — the shared entry
point for explicit call type arguments *and* stub type arguments — rejects a
void argument, which covers `idv<void>(…)`, `box<void>`, and (after routing
the three union-variant sites through it instead of bare `resolve_type`)
`may<void>.nope`. The generic call-argument loop rejects a void *argument
expression* at the argument's own location, which covers the inferred case. A
backstop in `mono_register` catches anything that reaches instantiation
without passing either, so no void instance can ever reach codegen; it reports
at the template's declaration with the mangled name, which is deliberately the
worse message — it should never be the one a user sees. (It *is* the one a
void inferred into a generic **struct literal** — `box { value = v() }` — gets
today, since that inference path has no use-site check of its own. Correctness
holds; the location is poor. → §8.) The spec already said void "cannot be
passed as an argument" and "remains invalid as a generic type argument"; only
enforcement was missing.

**A fourth route was still open after the first fix** (found by the same
adversarial diff review): `inner: box<void>` as a **struct field type** passes
neither use-site check — field types never reach `resolve_type` — and never
reaches `mono_register` either, because no instance is ever registered for a
field type that names one. So the emitted C referenced `fc__box`, undefined.
`canonicalize_field_stubs` — the field-type counterpart to `resolve_type` —
now carries the void judgment too, sharing `reject_void_type_arg` with
`resolve_generic_arg`. Test `generics/void_typearg_field_err`.

### 2.5 `unions/unknown_variant_construction_err` — unknown no-payload variant ✅ FIXED
`u.zzz` on a union with no variant `zzz` is silently accepted (types as `u`)
and emits undeclared `fc__u_tag_zzz`. The call path `u.zzz(5)` checks
("union 'u' has no variant 'zzz'"); the no-payload field-access path doesn't.
Also makes `u.count` on a union "work". Non-call twin of a call-shaped check.

**Fixed** by giving both union-variant-construction branches of the field
checker the existence check the call path already had, via a
`reject_bad_no_payload_variant` helper, and reusing the call path's message
verbatim. `u.zzz(5)` does not double-report: the field check runs first (the
call reads its callee's type) and poisons, so the call path returns early on
TYPE_ERROR. Tests cover the no-payload spelling, the property-shaped
`u.count`, the module-qualified `shapes.u.zzz`, and the generic `may<i32>.zzz`.

**Two more holes in the same family, found while fixing this and closed with
it** — both were silent wrong-runtime, not invalid C, so nothing downstream
would ever have caught them:

- **A *payload* variant named without its payload.** `let y = u.a` where
  `a(i32)` emitted `(fc__u){ .tag = fc__u_tag_a }` — the variant with a
  zero-filled payload, exit 0. The call path had the mirror check ("variant
  '%s' takes no payload") but the non-call path judged neither direction. Now
  rejected with "variant 'a' requires a payload: write u.a(value)", suppressed
  in callee position (where the node *is* `u.a` of `u.a(5)`) via a new
  `CheckCtx.in_value_position` — derived by `check_expr` from the one-shot
  callee/reflection flags it already consumes, so the per-kind checker can see
  what the wrapper decided. Test `unions/payload_variant_no_call_err`.
- **Field access on a union *value*.** The second variant-construction branch
  fired on any object of union type, value or not, so `x.b` on a `u` value
  built a fresh `u.b` and discarded `x`. The branch now requires the object to
  name the type (`expr_is_type_ref`, already used two checks below), and a
  union value falls through to a tailored member-access message instead of the
  generic "field access on non-struct type". Test
  `unions/union_value_field_access_err`.

`unions/variant_construction_forms` pins the negative space for all three:
payload and no-payload variants, bare and module-qualified, and a generic
union's explicit-type-arg spelling of both. `unions/imported_companion_variant/`
adds the cross-namespace import of a name that is both a union and its
companion module — a distinct resolution path through the same branch.

### 2.6 `pattern_matching/match_dup_binding_err` — duplicate binding in one pattern ✅ FIXED
`| { a, a } ->` emits two `int32_t a = ...;` in one C scope → redefinition
error. **Fix-direction:** the test expects a "duplicate binding" diagnostic
(Rust/OCaml precedent); the alternative is unique codegen names with
last-wins. Note the let-destructure twin `let { a, a } = { 1, 2 }` currently
*compiles* (last wins) — whichever direction is chosen must pin both paths.

**Fix-direction decided: the diagnostic**, and applied to every binding form —
match arm, `let` destructure, and `for` destructure. Rust, OCaml, F#, and
Swift all reject rather than pick a winner, and last-wins in a destructure is
a silent footgun with no upside. Implemented as `check_dup_pattern_bindings`,
which reads the locals the pattern just added to its scope rather than
re-walking the pattern (`check_dup_bindings`): that keeps one source of truth
(no second walker to drift from the checker) and it necessarily runs *after*
the PAT_BINDING→PAT_VARIANT rewrite, so a no-payload variant name repeated in
two field positions is correctly not a binding at all. That negative space is
pinned by `dup_binding_negative_space` (repeated variant names in one pattern,
the same name bound by different arms, a pattern binding shadowing an outer
one, binding-plus-wildcard).

Reading the scope also turned out to reach a collision **no** pattern walk
would have seen, which the same review surfaced: `for a, a in s` binds the
element and the index var separately, and emitted two declarations of `a` in
one C scope (a pre-existing bug, not a regression — the `for` header just was
not anywhere in scope of the old rule). The check now runs once over the whole
freshly-created loop scope after the header finishes binding, so it covers the
pattern-internal case and the element-vs-index case together. Test
`control_flow/for_index_var_dup_err`.

### 2.7 `const_eval/slice_lit_negative_size_err` — negative concrete slice-literal size ✅ FIXED
`u8[-4]{}` (also `u8[1 - 5]{}`, negative named const) accepted; emits
`uint8_t _fc_back_0[18446744073709551612]`. Spec: "A statically-negative
length is therefore a compile error." Only the deferred per-instance generic
path checks `sz <= 0`; the concrete EXPR_ARRAY_LIT path (src/pass2.c ~7284)
reads the folded u64 and never checks. The concrete twin of a generic check.

**Fixed:** the concrete EXPR_ARRAY_LIT path reads the folded length as a
signed 64-bit value and rejects it when negative, before the element-count
check (so the negative case is diagnosed at the length, not as a bogus
element-count mismatch). An unsigned literal above `INT64_MAX` gets its own
"too large" message rather than being rendered as a negative.

### 2.8 `const_eval/slice_lit_zero_size_err` — zero concrete slice-literal size ⚠️ FIXED, DIRECTION REVERSED
Same hole as 2.7: `u8[0]{}` compiles and runs while the generic twin
`zeros<0>()` is mandated to fail (existing test
`generics/const_slice_size_zero_err`: "slice literal length must be positive").

**The concrete path was right and the generic one was wrong** — the opposite
of what this entry assumed. Evidence: `i32[0] { }` is an established
empty-slice idiom already used by five pre-existing tests including two stdlib
ones (`stdlib/data_slice`, `memory/module_array_lit`, `generic_nested_depth2`,
`results/prop_generic`); the spec forbids only a statically-**negative**
length (§Slice construction from raw pointer) and says nothing against zero;
and codegen handles it cleanly (`alloca(0)` — no zero-length C array is ever
declared). Rejecting zero would have broken working code to satisfy a rule
nobody wrote.

So the *generic* per-instance check was relaxed from `sz <= 0` to `sz < 0`,
and `generics/const_slice_size_zero_err` became the positive test
`generics/const_slice_size_zero` (a `<0>` instance yields an empty slice; a
`<3>` instance still yields three). The negative half it used to carry is now
its own test, `generics/const_slice_size_negative_err`, and the concrete zero
case is pinned by `const_eval/slice_lit_zero_size`. Spec §Allocation now
states the rule explicitly in both directions.

### 2.9 `const_eval/i64_literal_const_arith` — i64 literals rejected in const-arg arithmetic ✅ FIXED
Const args are spec'd to evaluate in the i64 domain, and a *lone* literal
`f<4000000000>` is accepted — but the same literal inside const-arg
arithmetic (`wide<4000000000 / 10 + 0>`) goes through the normal i32-literal
check and errors, then evaluates as 0 producing a bogus "fixed array size must
be positive, got 0" cascade (missing poison suppression — fix that too).

**Fixed at the source rather than by suppressing the cascade:** the i64 domain
now fixes the *type* of an unsuffixed integer literal written inside a const
expression, so the range check never fires and there is no bogus 0 to
propagate. A new `Parser.in_const_expr` flag marks the const-expression slots
— a generic `<...>` argument and a fixed-array size, which share one grammar —
and `parse_int_type_in` gives an unsuffixed literal `i64` there instead of the
`i32` expression default. A written suffix still wins. The flag is set for the
whole extent of `parse_const_arith`, so it also covers the parenthesized
escape hatch (`wide<(5000000000 - 1000000000) / 10>`), which re-enters the
general expression grammar and would otherwise have kept the asymmetry one
level down. Spec §Const Parameters states the rule.

Not covered (and correctly so): an ordinary module-level `let big =
4000000000 / 1000000000` is not a const-expression slot — it is a normal
binding whose inferred type is `i32`, and the literal is out of range there.

### 2.10 `enums/module_enum_count_size` — `gfx.mode.count` not const in size slots ✅ FIXED
The 3-part module-qualified spelling is rejected ("must be a compile-time
constant", duplicated diagnostic) while bare `mode.count`, imported forms, and
deep module-const paths all fold. The const-size dotted-path folder doesn't
descend into an enum reached through a module component. Spec: `E.count` is an
i32 constant expression usable in slice-literal lengths.

**Fixed** in `const_fold_type_property`, which required the property's object
to be an `EXPR_IDENT` — true for `mode.count`, false for `gfx.mode.count`
(whose object is itself an EXPR_FIELD). pass2 already typed the object
correctly in both cases, so the fold now reads the object's **type** rather
than its node shape, and every spelling folds regardless of nesting depth.
The test was widened to cover a two-level module path (`gfx.deep.chan.count`),
arithmetic over two such counts, both size positions (struct field and
slice-literal length), and the ordinary expression positions.

### Tests added (30)

`generics/`: `const_slice_size_negative_err`, `void_typearg_explicit_err`,
`void_typearg_struct_err`, `void_typearg_union_err`, `bare_generic_default_err`,
`bare_generic_alloc_err`, `bare_generic_param_err`, `bare_generic_union_err`,
`bare_generic_slice_elem_err`, `bare_generic_module_sibling_err`,
`bare_generic_module_union_err`, `void_typearg_field_err`
(+ `const_slice_size_zero` repurposed positive).
`const_eval/`: `slice_lit_zero_size` (positive).
`pattern_matching/`: `match_void_subject_assign_err`, `match_void_lit_subject_err`,
`match_dup_binding_nested_err`, `dup_binding_negative_space` (positive).
`bindings/`: `let_destructure_dup_binding_err`.
`control_flow/`: `for_destructure_dup_binding_err`, `loop_mixed_break_reverse_err`,
`loop_mixed_break_nested_err`, `loop_break_forms` (positive),
`for_index_var_dup_err`.
`unions/`: `unknown_variant_property_err`, `unknown_variant_module_qualified_err`,
`unknown_variant_generic_err`, `payload_variant_no_call_err`,
`union_value_field_access_err`, `variant_construction_forms` (positive),
`imported_companion_variant/` (positive, multi-file).
Widened: `enums/module_enum_count_size`, `const_eval/i64_literal_const_arith`.

## 3. Escape-analysis holes + one wrong-reject — ✅ ALL FIXED (2026-07-18)

All eight fixed on branch `bugsearch`; suite 2151 passed / 23 failed (the
remaining failures are §4–§6), gcc + clang, -O0 and -O2, LSP wire tests green.
20 tests added beyond the eight repros. `spec/examples.fc`, the stdlib, all
five demos, and both sibling projects (wolf-fc, euler-fc) still compile clean.
Spec updated: §Escape analysis (binding-form propagation, the new
§§Containers and their contents), §Heap closures.

The eight split into two root causes, and 3.8 turned out to be the *same* root
cause as 3.3/3.7 rather than a rule in tension with them — see below.

### Root cause A: binding forms laundered provenance (3.1–3.4) ✅ FIXED

Every binding introduced by something other than a plain `let` went through
`scope_add` (hardcoded `PROV_UNKNOWN`) instead of `scope_add_prov`, and the
loop was the one value-producing control expression that never joined its
result's provenance.

- **3.1 `escape/match_binding_stack_ptr_err`** — `| some(p) -> p` returned a
  dead-frame pointer (the unwrap twin `some(&x)!` was caught). Also laundered
  `free` and heap stores.
- **3.2 `escape/tuple_destructure_stack_ptr_err`** — `let {p, n} = {&x, 1}`
  then return `p`.
- **3.3 `escape/for_elem_stack_ptr_err`** — `for p in ptrs do return p`.
- **3.4 `escape/loop_break_stack_ptr_err`** — `break &x` produced a
  `PROV_UNKNOWN` loop value.

**Fixed** by one channel rather than four patches. `CheckCtx.bind_prov` carries
"the provenance the value being taken apart has" across the pattern checkers,
which the three callers set and restore around their call (match subject, `let`
destructure init, `for` element); `bound_prov` applies it at each of the four
`scope_add` sites, gated on `type_has_provenance` so a scalar destructured out
of a stack tuple stays untagged. `EXPR_LOOP` grew `loop_break_prov` alongside
the existing `loop_break_type` — the parallel channel, so a break contributes
its provenance exactly where it already contributes its type. Nesting,
depth-2 destructures, the index-var form (`for i, p in ptrs`), and the
destructuring-`for` header all follow for free.

### Root cause B: `alloc`'s one-level copy skipped its contents (3.5–3.7) ✅ FIXED

A heap promotion copies one level and whatever it copies keeps pointing where
it always did. The struct-literal field check knew this; the three sibling
shapes did not.

- **3.5 `escape/alloc_closure_stack_capture_err`** — `alloc(lambda)` promoted
  the context without looking at the captures.
- **3.6 `escape/alloc_union_stack_payload_err`** — `alloc(u.held(&x))!`; the
  union constructor already set `PROV_STACK`, the alloc path never read it.
- **3.7 `escape/alloc_slice_stack_ptr_elems_err`** — `alloc(ptrs)!` copied the
  elements verbatim; the copied *pointer values* still targeted the dead frame.

**Fixed** with a check per shape at the one site that performs the store.
3.6 reads the constructor's own `prov` (it was already correct — only the
consumer was missing). 3.5 records each capture's provenance on the `Capture`
node at capture time, which is the one place the outer binding is in scope;
`reject_stack_captures` then judges both `alloc(<lambda>)` and `alloc(f)`, and
it catches a captured *capturing closure* too, since such a closure's own
context is stack memory. 3.7 needed root cause C.

### Root cause C: container provenance conflated backing with contents (3.7, 3.8) ✅ FIXED

- **3.8 `memory/free_heap_via_stack_slice_elem`** (wrong-reject) — a value
  loaded from a stack slice element inherited the slice's stack provenance, so
  a heap slice or closure parked in a stack container could never be freed,
  not even via a local copy.

3.8 reads as the opposite of 3.3/3.7 — one wants element loads *not* to
inherit, the other two want them to. Taking 3.8's suggested fix literally
(element load → `PROV_UNKNOWN`, only `&fs[0]` inherits) would have reopened
`escape/return_indexed_stack_ptr_err`, a deliberate existing test. They are
instead the same bug: **`prov` on a container was doing two jobs.** For a
struct it means "holds stack data"; for a slice it means "the backing store is
stack" — and a slice literal's backing is *always* stack (an alloca) however
static or heap its contents are. So the tag was simultaneously too strong for
element loads and too weak for `alloc`'s deep copy.

**Fixed** by splitting the second job onto its own axis: `Expr.elem_prov` /
`LocalBinding.elem_prov`, "the provenance of the values this thing holds",
defaulting to `PROV_UNKNOWN` (not tracked). Slice literals compute it by
merging their elements'; it rides through subslices, struct and tuple
literals and their field loads, option/result unwrap, casts and widens,
if/match/block joins, `alloc`'s copy, and `c[i] = v` (which taints the
container, in the loop pre-taint sweep too, since `c` may be re-read a later
iteration). A slice element load then reads `elem_prov` where it used to read
`prov`, and `alloc(slice)` judges `elem_prov` for 3.7. `&c[i]` still reads the
backing and is still rejected.

This makes the collection pattern work — `let bufs = (i32[])[2] { }`, fill with
`alloc`, `free(bufs[i])` — including through a `for` element, a subslice, a
struct field, and for heap closures. The rewritten
`memory/free_heap_via_stack_slice_elem` pins it, and
`memory/heap_elems_in_stack_container` pins every read form.

**What stays rejected, and deliberately:** provenance merges monotonically
toward stack, so a container seeded with a stack value keeps that tag for every
later read even after an element is overwritten with a heap one — which is
exactly the shape the original 3.8 repro used (`(i32[])[2] { e, e }` where `e`
is a stack slice, then `ss[0] = alloc(…)`). Under flow-insensitive analysis
that rejection is correct, not a residual bug: `ss[0]` may still be `e`. It is
pinned as `escape/free_stack_slice_elem_err`, and the idiomatic seed (`{ }`)
carries no taint.

### Tests added (20)

`escape/`: `match_binding_free_err`, `match_binding_heap_store_err`,
`struct_destructure_stack_ptr_err`, `nested_destructure_stack_ptr_err`,
`for_index_elem_stack_ptr_err`, `for_destructure_stack_ptr_err`,
`loop_break_nested_stack_ptr_err`, `alloc_closure_named_capture_err`,
`alloc_closure_stack_ptr_capture_err`,
`alloc_closure_stack_closure_capture_err`,
`alloc_union_stack_slice_payload_err`, `alloc_slice_stack_slice_elems_err`,
`struct_field_elem_stack_ptr_err`, `subslice_elem_stack_ptr_err`,
`match_slice_elem_stack_ptr_err`, `nested_container_elem_stack_ptr_err`,
`free_stack_slice_elem_err`,
`binding_prov_forms` (positive: heap and static values through every binding
form), `alloc_container_elem_forms` (positive: scalar / static / heap /
closure elements all survive promotion).
`memory/`: `heap_elems_in_stack_container` (positive), and
`free_heap_via_stack_slice_elem` rewritten to the collection pattern.
Every `.error` substring was tightened to the real diagnostic.

**Known limit, deliberate:** contents are tracked one level deep. A container
loaded out of another inherits its holder's element tag as the bound on its
own (`nested_container_elem_stack_ptr_err`), which keeps the nested case
conservative rather than precise — a heap container parked two levels inside a
stack-tainted one is rejected. The alternative is per-level element tracking,
which the intraprocedural analysis has no way to make sound anyway.

**Adversarial diff review** (the same pass that caught the leftover spellings
in §2.3/§2.4) found no further holes: `&arr[0]`, storing into a heap slice's
element, capturing a stack closure, a container held in a struct field, a
match on an element load, a subslice of one, and the loop-order case are all
rejected; eight legal-code shapes (passing a stack slice to a function,
iterating static strings, freeing through a parameter, destructuring a call
result, a closure capturing a parameter) are all accepted and ASan-clean.

## 4. Codegen emits invalid C for legal programs — ✅ ALL FIXED (2026-07-19)

All thirteen fixed on branch `bugsearch`, plus §6.1 and two more found while
fixing (both restated below); suite 2180 passed / 10 failed (the remaining
failures are §5 and §6), gcc + clang, -O0 and -O2, LSP wire
tests green. 15 tests added beyond the thirteen repros. `spec/examples.fc`, the
stdlib, all five demos, and both sibling projects (wolf-fc, euler-fc) still
compile clean; the new codegen paths are ASan/UBSan-clean.

The thirteen collapsed into five root causes. Spec updated: §Fixed arrays
(the outermost-type rule), §String Interpolation escape rules (`%%` folds in
every literal form).

### Root cause A: string literals were echoed, never decoded (4.8–4.11) ✅ FIXED

A string literal reaches codegen as its *source* text — the bytes between the
quotes, escapes unprocessed — and codegen printed that text into the emitted C
literal. That is wrong in both directions: FC and C disagree on what an escape
denotes (FC's `\x` takes exactly two hex digits, C's is greedy), and C reads
sequences FC never writes (trigraphs). Every symptom below is the same bug seen
from a different position.

- **4.8 `strings/trigraph_literal`** — `??` runs emitted raw →
  `-Werror=trigraphs`, and a conforming C11 translator would rewrite the bytes.
- **4.9 `strings/hex_escape_adjacent_digit`** — `"\x411"` is `{0x41, '1'}` in
  FC but one out-of-range escape in C.
- **4.10 `strings/quote_in_interp`** — `\"` in an *interpolated* string's static
  text emitted `\\"` (escaped backslash + raw quote), closing the C format
  literal early.
- **4.11 `strings/nul_in_interp`** — `\0` in interpolated static text landed as
  a real NUL inside the snprintf format (`-Werror=format-contains-nul`, and at
  runtime every later segment dropped).

**Fixed** with one decode and one encode, shared by every position.
`decode_str_lit` turns source text into the bytes it denotes (FC's escape set is
closed and the lexer has already rejected the rest, so the decode is total) and
doubles as the length function, so no consumer can compute a length that
disagrees with the bytes. `emit_c_byte` re-encodes for C: **three-digit octal**
rather than `\x` (self-delimiting, so 4.9's adjacent digit stays its own byte),
`\?` unconditionally (4.8), and a `fmt` flag for the one position that is a
printf format string. The seven emission sites — static `str`, `cstr`, string
*pattern*, both `alloc` heap copies, the interpolation format string, and
`emit_c_escaped`'s diagnostic text — all route through it. 4.11's NUL is the one
byte a format string cannot carry, so it is written by the format as `%c` with
argument `0`: snprintf copies it like any other byte and counts it in the
return, no segment splitting needed.

**Two more fixed by the same change:**

- **§6.1 `strings/pct_escape_static`** (listed under §6) — `%%` was folded in the
  computed `.len` but not in the emitted bytes, so `"50%% off"` yielded
  `50%% of`. The decoder folds it, so length and bytes now come from one place.
  The `cstr` twin, which had never folded `%%` at all, is fixed with it. New test
  `strings/pct_escape_forms` pins all four spellings.
- **A string *pattern* compared at the wrong length** (found while fixing, not
  from the hunt) — `| "a\nb" ->` emitted
  `fc_eq_fc_str(s, (fc_str){"a\nb", 4})`: the *source* length, 4, against a
  3-byte C literal, so the comparison read one byte past the literal. Silent,
  and no test would have caught it. Pinned in `strings/escape_encoding_forms`.

### Root cause B: nested pattern types were never resolved (4.2, 4.3) ✅ FIXED

- **4.2 `enums/nested_enum_pattern_union`** — an enum-variant pattern inside a
  union payload emitted tagged-union codegen for the enum
  (`_subj0.chosen.tag == fc__color_tag_red` on an int typedef).
- **4.3 `pattern_matching/nested_union_binding`** — a binding at depth 2 of a
  nested union pattern (`| wrap(chosen(c)) ->`) was type-checked but never
  declared ("'c' undeclared").

Only the *outermost* type in a pattern comes from a checked expression; every
type reached by descending is the name-only `TYPE_STUB` the declaration wrote.
Both pattern emitters dispatched on `->kind` without resolving it, so an enum
payload looked like a union (4.2) and a nested union's variant table was never
found, leaving `payload_type` NULL and the binding silently unemitted (4.3).
**Fixed** by resolving the stub once at the entry of `emit_pat_predicate` and
`emit_pat_bindings` — a no-op for an already-resolved type, so every level below
the first now sees the definition. Test `nested_pattern_depth_forms` covers the
enum-in-union, or-pattern, depth-2 binding, depth-2 tag test, through-an-option
and through-a-struct-field shapes.

### Root cause C: the typedef-discovery walk (4.5, 4.7, and 4.4) ✅ FIXED

- **4.7 `options/alloc_nested_option`** — the walk never descends into a
  **pointer's pointee**, so a nested option whose only occurrence is behind one
  gets no typedef. `alloc(T)`'s `T*?` result is that shape, and so is a plain
  `i32??*` parameter or field — a more general hole than the entry assumed
  ("skips alloc's type argument"). Fixed by descending into `TYPE_POINTER`; a
  pointer's C declarator names its pointee, so the typedef must exist. Structs
  are not collected here at all, so a self-referential `next: node*` still
  terminates. Test `options/option_behind_pointer_forms` covers the alloc,
  parameter, and field spellings over option, result, and slice pointees.
- **4.5 `generics/fn_typedef_instance_field_dup`** — the emitted typedef *name*
  resolves a stub to its mangled definition but `type_eq` does not, so
  `(wide<64>) -> i32` written as a struct field's type and the identical
  signature of a declared function hash apart and both emit
  `fc_fn_fc__wide__5___k64__int32_t`. Fixed by canonicalizing stub param/return
  types in the `TYPE_FUNC` arm — the same treatment the option and result arms
  already applied to their inner types. Test `fn_typedef_instance_forms` adds
  the type-arg (`box<i32>`) and generic-union twins.
- **4.4 `generics/const_arith_transitive_fn_typedef`** — name capture: the walk
  substituted the *caller's* `'n` into a callee's signature, so inside `mk2<'n>`
  the call `mk<'n * 2>()` emitted a typedef over the phantom `wide<64>` that is
  never instantiated. Fixed at the cause rather than the substitution: a
  **direct** call emits its callee by name and needs no function-type typedef at
  all, so the walk no longer collects one (an indirect call, whose callee really
  is a function value, still does). This also removes the latent hazard in
  std::wideint's `mul_wide` shape, which worked only because every width
  instance happened to exist.

### Root cause D: match arms after a catch-all (4.1) ✅ FIXED

**4.1 `pattern_matching/match_catchall_nonfinal`** — an unguarded catch-all in
non-final position tests nothing, so it emitted a bare `{...}` and the next
arm's `else` had no `if`. **Fixed** by dropping the later arms: under
first-match-wins they are unreachable, and FC permits such redundant arms
(`exhaustiveness/exhaust_union_dup_variant`), so there is nothing to diagnose.
Guarded catch-alls are unaffected — they route through the done-flag form, and a
guard means the arm is *not* unconditional. `match_catchall_nonfinal_forms` pins
the wildcard, binding and all-wildcard-struct spellings, mid-chain position, the
void match, the guard-elsewhere form, and the guarded-catch-all negative case.

### Root cause E: a fixed array nested in another type (4.6) ✅ FIXED

**4.6 `structs/nested_fixed_array_field_err`** — **fix-direction decided:
reject**, which is what the test expected. C spells a fixed array as an
inside-out declarator (`uint8_t m[3][2]`), which no other type constructor here
composes with, and supporting it would mean auditing every consumer of the
fixed-array repr — the partial-fix shape FC's completeness rule exists to avoid.
The check went in `parse_type_suffix`, where the nesting is formed, so it covers
the whole family rather than the one reported spelling: `u8[2][3]`, `u8[2][]`,
`u8[2]*`, `u8[2]?` and `u8[2]!` are all "a fixed array must be a field's
outermost type". The negative space — a fixed array *of* a composite
(`i32[][3]`, `i32*[2]`, `i32?[2]`) — is unaffected and pinned by
`structs/fixed_array_outermost_ok`. Spec §Fixed arrays states the rule.

### Warning-clean C (4.12, 4.13) ✅ FIXED

- **4.12 `expressions/xor_literal_pow`** — `10 ^ 6` trips
  `-Werror=xor-used-as-pow`, and a *hexadecimal* operand is the only thing that
  silences gcc and clang (parentheses do not). FC's `^` is exclusive-or and has
  no other reading, so a direct integer-literal operand of `^` is emitted in
  hex. A negative value keeps its decimal spelling (hex digits would not denote
  the same value, and the warning never fires there). `xor_literal_forms` pins
  every literal type, both sides, negative operands, and the compound-operand
  case that keeps decimal.
- **4.13 `equality/self_compare`** — `x == x` trips
  `-Werror=tautological-compare`. A no-op cast does *not* silence gcc (it strips
  the conversion and re-equates), so the fix reuses the existing sequencing
  machinery: `binary_is_self_compare` extends `seq_needed`'s condition, hoisting
  the left operand into a `_sq` temp, which neither compiler equates. Detection
  is `expr_structurally_equal`, deliberately answering true only for
  side-effect-free shapes — the same set the C compilers flag, since they never
  equate operands they cannot prove pure. Types that route through a generated
  `fc_eq_*` helper are excluded (a call is never tautological).
  `self_compare_forms` covers the local, field, pointer, deref, constant and
  variable index, compound arithmetic, enum, bool and float shapes, and asserts
  that a side-effecting operand is still evaluated once per occurrence.

### 4.14 `generics/nested_instance_arg_in_generic` — ✅ FIXED (2026-07-19)

Found while writing 4.4's test; **pre-existing** (verified against d5cfe7b), and
not one of the original 45. Filed as a failing test first, then fixed in the
same session once scoping showed it was tractable.

**Symptom.** A *generic* function whose body instantiates another generic
function at a *generic-instance* argument type emitted C naming things that were
never defined: for `let bx2 = (v: 'a) -> bx(bx(v))`, an undefined
`fc__box__14_fc__box__3_i32` (`box<box<i32>>`), and a callee mangled
`fc__bx__7_fc__box` — a bare `box` with no arguments, so the name the caller
emitted and the name the definition carried did not even agree. The identical
nesting from a **non-generic** caller (`bx(bx(5))`) always worked.

**One rule, missing at six sites.** A mangled C name is spelled from its type
arguments' *names*, so an argument that is itself a generic instance must
already carry its own mangled name — otherwise the outer name is built over a
bare template name and matches no definition. Inside a generic body that is not
true by construction: pass2 only ever sees the abstract `box<'a>`, so the
instance is discovered late, and *every* site that mangles from type arguments
has to canonicalize them the same way. Each site that missed it produced a
differently-wrong name, which is why the symptom moved every time one was fixed.

The rule is now a single exported helper, `mono_canonical_type_arg`
(monomorph.h), applied at:

- `resolve_generic_types_in_ret` (pass2) — the bindings it mangles from.
- `discover_in_expr`'s EXPR_CALL arm (mono) — which also has to *register* the
  argument's instance via `discover_nested_types`; nothing else reaches it.
- `discover_nested_types` itself (mono) — it mangled from `type_args` while
  descending only into *fields*, so the arguments were never visited at all.
  This was the last one, and the only producer that survived all the others.
- `mono_resolve_type_names` (mono) — same shape: recursed into fields, not args.
- `mangle_generic_with_subst` and the deferred-call mangler (codegen).

Plus a **choke point** at emission, `generic_instance_c_name` (codegen): a node
that carries type arguments but whose name is not itself a registered instance
is spelled from its arguments. Patching producers one at a time was whack-a-mole
— emission is the one point every name must pass through, so `emit_type`,
`emit_type_ident`, and the three union derived-name sites (tag enum, variant
construction) settle it there. Registration stays at discovery; *naming* is
settled at emission.

**This is the third or fourth recurrence of this family** (§4.4 and §4.5 were
the same shape), and the §4.14 fix reduces the blast radius without removing the
hazard: the C name of an instance is still *stored state on the type node*, so
sites can still disagree about it. Scoping afterwards found the actual root
cause — `mangle_type_name`'s `TYPE_STUB` arm already computes a name from
structure, and its comment describes this exact bug, but the `TYPE_STRUCT` and
`TYPE_UNION` arms read the stored name instead. The durable fix is much smaller
than a rewrite: see **`MANGLING-PLAN.md`** at repo root.

**Test** `generics/nested_instance_arg_in_generic` covers the family, not the
repro: the original two-level shape, three levels, the instance reaching the
argument through a wrapper constructor (`bx(some(bx(v)))`), a generic *union*
instance as the argument, the const-generic twin (`bx(mkw<'n>())`), and the
non-generic twin that always worked — pinned so both stay honest.

### Tests added (15)

`strings/`: `escape_encoding_forms` (positive: every escape through static str,
cstr, interpolated text, both alloc'd heap copies, and a string pattern),
`pct_escape_forms` (positive: `%%` in all four literal forms),
`trigraph_in_assert_msg` (a `??` run through the assert message *and* the
asserted expression's embedded source text).
`expressions/`: `xor_literal_forms` (positive).
`equality/`: `self_compare_forms` (positive).
`pattern_matching/`: `match_catchall_nonfinal_forms` (positive),
`nested_pattern_depth_forms` (positive).
`structs/`: `fixed_array_slice_elem_err`, `fixed_array_ptr_err`,
`fixed_array_option_err`, `fixed_array_outermost_ok` (positive).
`options/`: `option_behind_pointer_forms` (positive).
`generics/`: `const_arith_transitive_chain` (positive: a deeper const chain plus
the type-parameter twin), `fn_typedef_instance_forms` (positive),
`nested_instance_arg_in_generic` (positive — see 4.14).

## 5. C-identifier hygiene / mangling collisions — ✅ ALL FIXED (2026-07-19)

All six fixed on branch `bugsearch`, plus one more found while fixing (5.7,
below) and two pre-existing ones surfaced by the diff review (see there);
suite 2202 passed / 3 failed (the remaining failures are §6), gcc +
clang, -O0 and -O2, LSP wire tests green. 16 tests added beyond the six repros.
`spec/examples.fc`, the stdlib, all five demos, and both sibling projects
(wolf-fc 208/208, euler-fc) still compile clean; an ASan/UBSan `fcc` over 1909
test compilations plus `examples.fc` and wolf-fc reports zero hits.

The six collapsed into **one design question with three answers** — *which
namespace does this generated name live in?* FC identifiers cannot contain
`__`, and that is the whole lever: it makes three name spaces separable, and
every one of the six bugs is a name that landed in the wrong one. The rule is
now stated as an invariant in CLAUDE.md → **C name namespaces**; the three
spaces are `fc__…` (user declarations), `fc_<kind>_…` (compiler-derived), and
`_l_<name>_<id>` / `_<temp><n>` (function-local).

### Root cause A: the user namespace wasn't rooted deep enough (5.1) ✅ FIXED

**5.1 `c_hygiene/module_named_fc`** — a module or namespace named `fc` mangled
its members to `fc__<name>`, the same spelling file-scope declarations use.
Two globals silently merged into one C object.

**Fixed** by rooting the *whole* declaration path at `fc__` rather than only
file-scope decls (`mangle_root`, pass1.c): a module member is `fc__mod__name`,
a namespaced one `fc__ns__mod__name`, so `fc` is just another path component
(`fc__fc__counter`). That also buys the converse the other fixes rely on —
since no user name can start with `fc__`, any derived spelling that does *not*
is unreachable from source by construction. Tests
`module_named_fc_forms` (every declaration kind duplicated between module `fc`
and file scope: struct, union, enum, generic struct, function, global) and
`namespace_named_fc/` (multi-file, the namespace half).

### Root cause B: bindings kept their source spelling (5.2, 5.3, 5.6) ✅ FIXED

Three symptoms of one gap: `let` was the only binding form pass2 gave a unique
codegen name. Params, for-loop variables (element and index) and pattern
bindings reached C with their source spelling, escaped only for C keywords.

- **5.2 `c_hygiene/internal_name_collisions`** — a source name spelling a
  codegen temp. Worst shapes were silent: param `_sg0_0` in an interpolation
  emitted `int32_t _sg0_0 = _sg0_0;` (legal C11, garbage); binding `_match1`
  shadowed the match result var.
- **5.3 `c_hygiene/param_shadows_runtime`** — params named `abort`, `fc_str`,
  `snprintf` shadowed file-scope names the emitted body calls.
- **5.6 `c_hygiene/pattern_binding_c_keyword`** — `| circle(int) ->` emitted
  `int32_t int = …`.

**Fixed** with one channel rather than three escapes: `local_c_name` (pass2.c)
mints `_l_<name>_<id>` for *every* binding form, and params, for-vars and
pattern bindings now carry it on the AST (`Param.codegen_name`,
`for_expr.var_codegen_name` / `index_codegen_name`,
`Pattern.binding.codegen_name`) the way `let` already did. All three symptoms
follow: the `_l_` prefix keeps every source name out of file scope, so no list
of borrowed libc/runtime symbols has to be maintained (5.3); because *every*
binding is rewritten, a name spelling a temp becomes `_l__subj0_7` and cannot
reach it (5.2); and `_l_int_3` is not a keyword (5.6). `c_safe_ident` is now
used only for *member* names, which live in per-type namespaces.

The destructure paths, which used to overwrite `binding.name` with the codegen
name, were switched to the new field — so the source spelling survives for
diagnostics and the LSP, and re-checking a pattern is idempotent. Tests
`internal_name_collision_forms` (every binding form named after a temp —
`_ctx`, `_sg0_0`, `_subj0`, `_match0`, `_fe0`, `_fs0`, `_loop_result`, `_sq0`,
`_fc_back_0`, `_l_x_0` — plus a capture of one), `binding_shadows_runtime_forms`
(libc and `fc_*` names through params, `let`, for-vars, match and destructure
bindings), `pattern_binding_name_forms` (keyword/libc/temp names through
variant, nested-variant, option, result, struct, tuple and for-header patterns).

### Root cause C: derived names built by suffixing a user name (5.4, 5.5) ✅ FIXED

- **5.4 `c_hygiene/variant_named_tag`** — a payload variant named `tag`
  duplicated the injected discriminant, because C gives an *anonymous* union's
  members the enclosing struct's namespace.
- **5.5 `c_hygiene/union_tag_struct_collision`** — the tag enum's typedef
  `fc__<union>_tag` and its enumerators `fc__<union>_tag_<variant>` sat inside
  the user-reachable `fc__<name>` space, so a user struct named `shape_tag`
  collided.

**Fixed** by moving each derived name out of the namespace it was borrowing.
5.4: the payload union is now *named* (`… union { … } u;`), so the outer struct
declares exactly `tag` and `u` — nothing derived from source — and no variant
name reaches that namespace at all. 5.5: the tag enum is `fc_tag_<U>` and its
enumerators `fc_tv_<V>__<U>` (variant first), both outside `fc__`.

The two kinds take **different prefixes**, and the variant is joined with `__`
rather than `_`, because a single `_`-joined space is ambiguous both ways: union
`a` variant `b_c` would spell like union `a_b` variant `c`, and union `shape`
variant `circle` like union `shape_circle`'s typedef. The enumerator puts the
variant *first* because only that order is injective — the union-first join
aliases across a `_` boundary; see the diff-review section below for the case
that forced the order. Tests `union_derived_name_forms` (variants named `tag` *and* `u`, user
types named `thing_tag` / `thing_tag_blank` / union `thing_tag_more`, through
equality, options and slices) and `union_derived_name_generic_forms` (the
monomorphized and module-scoped twins, which emit through separate code paths).

### 5.7 declaration paths flatten ambiguously — ✅ FIXED (found while fixing 5.1)

Not from the original hunt; **pre-existing** (the same collision spelled
`a__b__counter` before 5.1's reroot). Rooting the path at `fc__` does not make
the scheme injective, because FC has *two* hierarchies — namespaces and module
nesting — and both flatten onto the same `__`. `namespace a:: module b` and
`module a = module b` spell one prefix, and their members were emitted as a
single C object: `assert(b.counter == 111)` and `assert(a.b.counter == 222)`
read the same global. A component may also start or end with `_`, so module
`a_` member `b` and module `a` member `_b` both spell `fc__a___b`.

Making the join unambiguous costs every generated name its readability
(length-prefixed components, as the monomorphizer's type mangling uses), so the
**claim is checked instead**: `check_c_name_collisions` (end of pass1) walks
every declaration that reaches C file scope and reports a second claimant of a
name, quoting both sites. Complete by construction — it judges the names
actually emitted, not the shapes that were anticipated — and it is the backstop
the whole §5 family was missing: any future scheme change that reintroduces a
collision fails loudly instead of silently merging two globals. Extern
declarations are exempt (they are emitted under their C tag; two FC spellings of
one C type are the point), and the check only judges an otherwise-clean program
— a redefinition necessarily collides too, so running anyway would just diagnose
a reported mistake twice. Tests `path_flatten_collision_err/` (multi-file,
namespace-vs-nesting), `underscore_boundary_collision_err` (the `_` boundary
case), and `c_name_claim_negative_space` (positive: names differing only by
where an underscore falls, a type and its companion module, a module member
whose tail matches a file-scope decl, two error groups sharing member names, and
a generic template at several instances).

### Adversarial diff review — 3 regressions caught and fixed, 2 pre-existing bugs found

A review pass over the §5 diff (the same shape §2 and §3 used) found three
defects the fix itself introduced. All three are the *same mistake*: a rule
stated for one join was not applied to the next one along.

- **The enumerator join was not injective.** `fc_tv_<U>__<V>` aliases, because
  an FC identifier may *begin* with `_`: union `a_` variant `b` and union `a`
  variant `_b` both spell `fc__a___b` — gcc "redeclaration of enumerator", fcc
  exit 0. This is precisely the hazard `mangled_tail` (pass1.c) documents and
  defends against, one join further along, and the baseline spelling did not
  have it. **Fixed** by putting the variant *first*: `fc_tv_<V>__<U>` is
  injective because `V` contains no `__` and `U` always starts with `fc__`, so
  a shorter split would have to end its variant one `_` short and then read
  `_f` where it needs `__`. Test `union_derived_name_underscore_forms`.
- **The pattern emitters' fixed `char[256]` paths became silently wrong.**
  Naming the payload union (5.4) costs `.u.` per level where a level used to
  cost `.`, and `snprintf` truncation is invisible. A cut landing inside a
  member name can hit a **shorter member of the same union** — a leaf
  `| ab(f32) | abq(i32)` matched at `abq` emits `.u.ab`, which compiles clean
  under `-Wall -Werror` and reads an `f32` as an `i32`. Verified: exit 0 on the
  pre-§5 build, exit 134 after. **Fixed** at the cause — the six path buffers in
  `emit_pat_predicate` / `emit_pat_bindings` and the two in `emit_value_eq` are
  now arena-backed (`path_cat`), so no depth truncates. Test
  `deep_pattern_path_forms` (the prefix-sibling union at depth 5 with 58-char
  names, plus the equality helper's long field paths).
- **`extern` can spell a name inside the `fc__` root.** The C-name position is
  the one place `__` is legal (it is where the implementation-reserved namespace
  lives), so `extern fc__m__counter as c` and module member `m.counter` were
  emitted as one object — the §5.1 failure mode, through the door the reroot
  opened. **Fixed** by claiming extern C names in the backstop; two externs
  naming one symbol stay legal (that is how a header symbol gets a second
  alias). Tests `extern_name_claims_decl_err/` and
  `extern_alias_negative_space`.

Two **pre-existing** bugs surfaced by the same pass, both fixed here since the
backstop is what exposed them:

- **A module member named `main`.** Codegen decided "this is the entry point"
  from the source name alone, so `module m = let main = …` emitted a second
  `fc_main` *and* a second `int main(int, char**)` wrapper (fcc exit 0, invalid
  C) — and the zero-parameter form **segfaulted the compiler**, since the
  entry-point path reads `params[0]` unconditionally. Only a *file-scope*
  `let main` is the entry point (pass2's signature check does not apply to a
  member either), so `is_entry_point` now says so and a member is an ordinary
  function. Test `module_member_named_main`.
- **A `let` destructure in a match arm** emitted `/* TODO: expr kind 44 */` —
  fcc exit 0, invalid C. The arm loop emits its statements itself instead of
  delegating to `emit_block_stmts`, and the twin knew only about plain `let`.
  De-twinned into `emit_let_destruct_stmt`, called from both. Test
  `destructure_in_match_arm`.

The review also confirmed clean: every `param_count` loop in codegen routes
through `param_c_name`; all six `.tag` sites and every payload access carry
`.u`; the two remaining `binding.name` readers are correct; nested
modules/namespaces root at `fc__` exactly once; and `unmangled_name` (lsp.c) is
algorithmically identical to the new `mangled_tail`.

### Tests added (16)

`c_hygiene/`: `module_named_fc_forms`, `namespace_named_fc/` (multi-file),
`internal_name_collision_forms`, `binding_shadows_runtime_forms`,
`pattern_binding_name_forms`, `union_derived_name_forms`,
`union_derived_name_generic_forms`, `union_derived_name_underscore_forms`,
`deep_pattern_path_forms`, `path_flatten_collision_err/` (multi-file),
`underscore_boundary_collision_err`, `c_name_claim_negative_space`,
`extern_name_claims_decl_err/` (multi-file), `extern_alias_negative_space`,
`module_member_named_main`, `destructure_in_match_arm`.
Widened: `param_keywords` (generic, higher-order/trampoline and defer paths).

## 6. String interpolation semantics — ✅ ALL FIXED (2026-07-19)

- **6.1 `strings/pct_escape_static`** — ✅ **FIXED 2026-07-19** as part of §4's
  root cause A (the decode/re-encode of string literals) — see there. In a
  *static* (segment-free) literal, `%%` was not collapsed in the emitted bytes
  but `.len` was computed as if it were: `"50%% off"` → content `50%% of` with
  len 7. The cstr twin had the raw bytes too. Both now fold, from the single
  decoder that also computes the length. Test `strings/pct_escape_forms`.
- **6.2 `strings/interp_width_overflow_err`** — ✅ **FIXED 2026-07-19.** Width
  digits accumulated through 32 bits unvalidated: `%99999999999d` wrapped to a
  ~1.2 GB backing array → SIGSEGV at frame setup; `%2147483648d` emitted C that
  failed `-Werror=format-truncation`; `%4294967297d` silently ignored the width.
  Both axes now accumulate in `int64` and *saturate* (`parse_format_width_prec`,
  codegen.c), and pass2 rejects anything over `INTERP_MAX_FIELD` = **32767**.
  The limit is derived, not picked: a width and a precision are passed to the C
  formatter as `int`, and C11 guarantees `int` only to 32767 — the same 16-bit
  floor the emitted arithmetic already honors (§FC targets 16-bit-int
  platforms). That also caps what one segment can demand of the hoisted buffer
  (~32 KB), which closes the "consider capping the hoisted buffer" note.
- **6.3 `strings/interp_precision_overflow_err`** — ✅ **FIXED 2026-07-19**, same
  mechanism (`%.4294967297s` had wrapped to precision 1 → "hello" → "h").
- **6.4 `strings/interp_unsigned_decimal`** — ✅ **FIXED 2026-07-19.** `%d`
  mapped unconditionally to `"%lld"` + `(long long)`, so `%d{u64.max}` printed
  `-1` through an implementation-defined conversion. `%d`/`%i` now emit `%llu` +
  `(unsigned long long)` when the operand's own type is unsigned. One predicate
  (`interp_conv_is_unsigned`, codegen.c) drives the conversion character, the
  argument cast, and the sign-byte budget, so the three cannot drift.

### Found while fixing 6.4: format modifiers were never judged ✅ FIXED (2026-07-19)

Codegen copies a format spec into the emitted C format string **verbatim**, so
FC accepted every flag/conversion pair C11 leaves undefined — and the C it
emitted failed `-Wall` (which the project compiles with `-Werror`) on all of
them, on both gcc and clang: `'+'` with `%u`/`%s`/`%c`, `'#'` with `%d`/`%s`/`%c`,
`'0'` with `%s`/`%p`, precision with `%c`/`%p`, plus the *nullified* forms C
requires the formatter to ignore (`%--d`, `%-08d`, `%0.5d`, `%+ d`). Every
modifier on `%T` was silently dropped as well, since the type name is spliced at
compile time and never reaches the formatter. Pre-existing, but 6.4 widened it:
`%+d{u32.max}` now remaps to `%+llu`, which gcc rejects.

Fixed with one rule — **every modifier a spec carries must be honored** —
judged in pass2 (`check_interp_spec_mods`) off the shared spec reading
(`interp_seg_spec` → `InterpSpec`, codegen.c). A modifier fails that either by
not applying to the conversion (C11's flag table) or by being nullified by
another modifier. Spec §Format specifiers → Modifiers states the rule. All 7
flag-carrying specs in the existing tests/stdlib/demos/spec sources stay legal;
no *working* program breaks, since every rejected form already emitted C that
would not compile under the project's own flags.

### Found while fixing the above: integer precision under-allocated the buffer ✅ FIXED (2026-07-19)

`interp_numeric_bound` sized integer segments from the type's magnitude alone and
ignored the precision, but a precision on an integer conversion is a *minimum
digit count*: `%.20d{5}` writes twenty digits from an `i32` budgeted at eleven,
so snprintf silently clipped the result to `00000000005` — a direct violation of
the spec's "buffer size guaranteed to be sufficient". The bound now takes
`max(magnitude, precision + sign)`, where the sign byte is budgeted only for a
conversion that actually renders signed (`%.20x` of a negative prints a bit
pattern with no sign). Test `strings/interp_int_precision`.

### Tests added (17)

Positive space — `interp_flags_valid` (every C11-defined flag/conversion pair,
guarding the new judgment against over-rejecting), `interp_int_precision`
(integer precision as a minimum digit count, incl. the exact-fit `%#.20x`
budget), `interp_unsigned_decimal_widths` (`%d`/`%i` across every unsigned type
+ usize, signed operands unchanged, `%u` still a bit pattern),
`interp_field_limit_boundary` (32767 still works on both axes).

Negative space — `interp_width_over_limit_err`, `interp_precision_over_limit_err`
(one past the limit), `interp_width_wrap_forms_err` (the other 32-bit wrap
shapes), and one per modifier rule: `interp_flag_sign_unsigned_conv_err`,
`interp_flag_sign_unsigned_operand_err`, `interp_flag_sign_str_err`,
`interp_flag_hash_err`, `interp_flag_zero_str_err`, `interp_flag_zero_ptr_err`,
`interp_flag_repeated_err`, `interp_flag_zero_minus_err`,
`interp_flag_zero_precision_err`, `interp_flag_plus_space_err`,
`interp_precision_char_err`, `interp_precision_ptr_err`,
`interp_type_spec_mods_err`.

## 7. Design questions / spec contradictions (no tests — decide first)

1. **Discarded results smuggled in aggregates**: `{ fallible(), 9 }` or
   `some(fallible())` in statement position compiles — the discard check
   guards only top-level `TYPE_RESULT`. Letter-of-the-rule vs. its intent;
   probably worth guarding any discarded value *containing* a result.
   **✅ DECIDED — no change** (2026-07-19). The rule guards the *result*, and a
   result placed inside an aggregate has been consumed by that construction:
   the value built from it is what statement position then discards, which is
   the ordinary C-style "a plain value you may not need" case (§Results cannot
   be silently ignored draws exactly that line). Deepening the check to any
   discarded value *containing* a result would make the diagnostic depend on a
   type's interior rather than on what the expression produces.
2. **`return`/`break`/`continue` in grammar's `primary_expr`** but rejected in
   general expression positions (call args, match subject) — parser only
   allows them as block/inline-sequence items. Current semantics are
   defensible; then grammar.bnf should stop listing them under `primary_expr`.
   **✅ FIXED — semantics kept, grammar corrected** (2026-07-19). They are
   statement items now: `block_expr_item` gains `return_expr`, `break_expr`,
   `"continue"` (and `ignore_stmt`, which the grammar had never carried) and
   `primary_expr` loses them. Deleting them from `primary_expr` alone would
   have made `if c then f() else return` underivable, because the body
   positions were spelled `expr_or_block` while the parser reads all of them
   (lambda, if/else, match arm, loop, for) as item sequences — so those now
   share one `body_or_block = INDENT block DEDENT | inline_body`, and
   `expr_or_block` is left to the positions that really do take a single value
   (a `let` initializer, a guard operand). Also found while checking the
   grammar against the lexer: the `keyword` production was missing `do`,
   `ignore`, and `guarded`/`unguarded`/`checked`/`unchecked`.
3. **Explicit type args on struct literals** (`box<i32> { ... }`,
   `pt<4> { ... }`): grammar.bnf line ~922 sanctions them, the parser never
   consumes them, and the fallback misparse yields nonsense diagnostics
   ("tuple literal requires at least 2 elements"). Accept per grammar, or fix
   grammar + add a targeted diagnostic.
   **✅ FIXED — not allowed** (2026-07-19, decided). A struct literal's type
   arguments are always determined by its field values, so an explicit list
   adds nothing; `struct_literal` in grammar.bnf drops the `[ "<" …">" ]` (and
   gains the module-qualified name it always accepted), and spec §Generic
   Structs states the rule. The parser claims the form (`struct_lit_typearg_scan`
   in parser.c) and reports it at the `<`, then consumes the arguments and
   parses the literal as if they were absent so nothing cascades. The claim
   needs no generic-name gate: `<…>` followed by struct-literal brace shape
   (`{}` or `{ ident =`) can never close a comparison chain, since a tuple
   literal needs two elements. The three expression-position readings of
   `name<…>` (generic call, bare instantiation, this) now share one
   `typearg_scan` and differ only in what must follow it — the two older scans
   were byte-identical twins. Tests: `generics/struct_lit_type_args_err`,
   `_nested_err`, `_module_err`, `_const_err` (const argument), `_nongeneric_err`
   (the form is wrong whether or not the name is generic), and
   `_negative_space` (a tuple of comparisons keeps its comparison reading; the
   argument-free literal still infers).
4. **Spec §Tuples' own examples are uncompilable**: `{i32, str}` can never
   receive `{ 1, "a" }` ("expected {i32, str}, got {i32, const str}") because
   tuple types are synthesized structurally and the literal's `const` leaks
   in. Either the spec examples change to `const str`, or tuples get
   elementwise nonconst→const acceptance (repr-preserving widening precedent
   exists: `i32*` → `const i32*`).
   **✅ FIXED — spec examples corrected** (2026-07-19, decided). The premise of
   the second option was wrong: the mismatch here runs `const str` value into a
   `str` slot, so accepting it would *strip* const, not add it. That direction
   is rejected everywhere in FC, tuples included — plain `g("hello")` against
   `(s: str)` fails identically — and §Slice literals already states the rule
   for the same "aggregate literal element" shape (`const str[3] { "a", … }`,
   "a string literal is a `const str` and does not narrow to `str`"). So the
   examples were simply wrong about the literal's type: §Tuples now says
   `{ 4, "hello" }` is `{i32, const str}`, the parameter example is
   `(x: {i32, const str})`, `t[1]` is documented `const str`, `flip`'s result
   is `{const str, i32}`, and a sentence in §Tuple values states that elements
   infer `const` exactly as struct fields do. Every code block in §Tuples was
   compiled and run as one program to confirm (exit 0, gcc `-Wall -Werror`
   clean). Tests: `tuples/const_str_element` (the corrected spelling, through
   parameter/return/destructure/slice-literal positions) and
   `tuples/const_str_strip_err` (the spec's old spelling, now pinned as an
   error).
   **✅ The residual is now decided too — no elementwise widening** (2026-07-19).
   Tuples do no elementwise widening in the **safe** direction either
   (`{i32, str}` into a `{i32, const str}` parameter), and that stays. The
   governing rule, now stated in spec §Implicit Widening → "How deep widening
   reaches": **widening reinterprets a value, never rebuilds one; it descends
   only into the option niche, where it is provably free.** Every widen either
   leaves the bits alone (const-add, `T*` → `any*`) or rewrites one scalar in
   place, so it reaches a value's own type and no further — a tuple's elements,
   a struct's fields, and a result's payload never widen, because converting
   them means constructing a second aggregate and copying the parts across.
   Verified concretely: `{i32, str}` and `{i32, const str}` emit as two
   distinct C typedefs with byte-identical bodies, so the conversion would need
   a field-wise rebuild in a statement-expression — the very thing the spec
   already cites as its reason for rejecting `i32?` → `i64?`. It would also
   apply to tuples but never to the equivalent named structs (nominal), which
   is a wart. The precedents agree: C allows `T*` → `const T*` at top level and
   famously not `T**` → `const T**`; Rust's coercions do not propagate into
   tuples; Zig coerces at top level and into optionals — which is exactly where
   FC coerces today. This is not tension with §7.9: building and converting are
   different acts, and a literal's elements are *placed into* slots whose type
   is written right there. Tests `tuples/widen_no_descent_err` (struct field),
   `_opt_err` (option payload), `_assign_err` (assignment), `_elem_err` (slice
   -literal element) reach the rule through four indirect paths.
5. **Spec §Tuples contradicts itself on `t[0] = v`**: line ~2742 says element
   rebinding "needs `let mut`", but §One-rule-three-knobs says contents are
   always assignable — and the compiler follows the latter. Fix the spec
   prose (or change the rule).
   **✅ FIXED — spec prose corrected** (2026-07-21, decided). The rule stands as
   §One rule, three knobs and §Address-of already state it; §Tuples was simply
   wrong in two places, both the same mistake of reading an element access as
   binding-level. `t[0] = 7` is a content mutation and needs no `let mut`
   (verified), so that parenthetical is gone; the passage now says `let` vs
   `let mut` governs only whole-tuple reassignment (`t = { … }`) and cites the
   struct-field analogue. The second spot — `&t[1] // (when t is let mut)` —
   was wrong for the same reason: `&t[0]` and `&p.x` and `&b[0]` are all
   *content* addresses and all compile on a `let` binding; only `&t` itself
   needs `let mut`. §Address-of gained that general statement (it previously
   named only field *assignment* as the binding/value distinction), and the
   §Inherited-behavior comment `b[0] = 99 // element mutation on a let mut
   binding` no longer implies `mut` is what admits it. Test:
   `tuples/let_content_mutation` (element assign, element address + write
   through it, nested tuple and struct elements, a pointer element, and the
   one form that does need `let mut`).
6. **Closure captured-struct field mutation** mutates a per-call temporary
   (`f()` sees its own fresh copy each call, writes never persist) — identical
   for stack and heap closures. Consistent, but the spec's "capture by copy
   (at creation)" reads as one persistent copy. Needs a spec sentence.
   **✅ FIXED — spec sentence, semantics kept** (2026-07-21, decided). The
   framing in the item (and in the "reject the write" proposal below) was wrong
   about what the write *is*. A capture is a by-value copy, and FC already has
   by-value copies with exactly these semantics: **a by-value parameter**.
   Verified side by side — `p.x = v` on a `(p: point)` parameter, `&p.x` into
   it, a fixed-array field write, and mutate-and-return
   (`(p: point) -> (p.x = p.x + 1; p)`) all compile, all discard at return, and
   all leave the caller's value intact, matching the captured `point` case in
   every shape. So the write is not a no-op at all: it is observable for the
   rest of the call and through whatever the body returns — only the *enclosing
   binding* is unaffected, which is what by-value means. Rejecting it would
   have banned the capture spelling of a pattern FC allows for parameters, and
   would have needed a special rule for captures alone. `let mut` governs the
   root binding; `const` is the separate mechanism for read-only *through a
   reference*; neither has anything to say here.
   Spec §Closures & Capture gains **What a capture copies**: the copy is taken
   at creation, each call works on a fresh copy of it, and a captured name
   behaves as if the body began with `let <name> = <the value copied at
   creation>` — so every rule that applies is one that already existed. It also
   states the one genuinely surprising consequence, which falls out of the same
   sentence: the copy is one level deep, so a post-creation write to the
   captured value's own storage is *not* seen by the closure while a write
   through a reference it holds *is* (both verified). §Heap closures' "captures
   are still immutable copies" now reads "copies of immutable bindings" — the
   binding is immutable, the copy's contents are not. Every code block added
   was compiled and run as one program. Test:
   `closures/capture_copy_semantics` (struct, tuple and fixed-array captures;
   the parameter twin; `&base.x`; captured slice backing and captured pointer
   pointee; and the creation-time snapshot vs. shared-reference split).

   **Investigation notes 2026-07-21 (what the emitted C does).** It makes the
   two copies explicit: closure *creation* stores one copy in the context
   (`_fc_back_0._l_c_1 = _l_c_1`, or the malloc'd twin under `alloc`), and each
   *call* opens with `fc__counter _l_c_1 = _c->_l_c_1;` — a fresh local copy of
   that copy. That is the per-call value the body reads and writes; the write
   reaches nothing outside the call. What *does* persist is any write that
   crosses a reference, since the copy is shallow: through a captured pointer
   (`pc.n = pc.n + 1`) and into a captured slice's backing
   (`buf[0] = buf[0] + 1`) both accumulate across calls. Tuple elements behave
   as struct fields do; reassigning the captured binding itself is already
   "cannot assign to immutable binding".

   **Rejecting the write was considered and declined.** A prototype (an
   `is_capture` bit on `EXPR_IDENT` plus a capture-rooted-lvalue check beside
   the immutable-binding and self-assignment checks in `EXPR_ASSIGN`) was
   written and measured: zero sites in 2272 tests, `spec/examples.fc`, all five
   demos, wolf-fc and euler-fc — nobody writes this. It was also *incomplete*:
   `&c.f` and a fixed-array field (`h.arr[0] = v`, which decays to a slice and
   slips a syntactic check) reach the same copy, so a complete rule would have
   had to const-qualify or provenance-tag those too. Both facts were moot once
   the parameter parallel surfaced — the sites are rare because mutating a
   by-value copy is rare, not because the spelling is a trap. The precedent
   survey that motivated the proposal (C++ `operator()` is const unless
   `mutable`; ObjC `__block`; Swift immutable captures; F# FS0407) is about
   languages whose captures are *closure state*; FC's per-call re-copy makes a
   capture a per-call value, which is the parameter case, and every one of
   those languages allows mutating a by-value parameter.
7. **Const-generic argument slots reject `dir.count` / `i32.bits`**
   (`buf<dir.count>` errors) while the same expressions fold in *size* slots
   (`u8[dir.count]` works). The spec's const-arg grammar technically excludes
   them; the asymmetry looks unintended.
   **✅ FIXED — a const argument only has to fold** (2026-07-21, decided). The
   two slots share one constant-expression grammar, so what folds in a size
   slot is a const argument. The hole was never in the folder — arithmetic
   forms already worked (`buf<dir.count * 2>`, `buf<(i32.bits)>`), since
   `normalize_const_expr` folds named consts, enum counts and type properties
   alike. Only the *bare* dotted spelling missed, in two different ways:
   - `buf<i32.bits>` never parsed. `parse_type` consumed `i32` as a built-in
     and stopped at the `.`, so the whole argument list failed its claim and
     backtracked ("expected ')', got '<'"). `ident_arg_is_const_expr` now takes
     the value reading when a built-in type name is followed by a member — the
     one dotted case the parser *can* settle, since no type continues past
     `i32`. The ambiguous case stays deferred: `m.point` is a type and
     `dir.count` is a value, and only pass2 knows which.
   - `buf<dir.count>` parsed as a type stub and died as "unknown type name
     'dir.count'". `try_named_const_arg` only accepted a stub naming a
     `DECL_LET`; it now falls back to `try_type_property_const_arg`, which
     commits to the value reading exactly when the name's prefix denotes a type
     (a built-in, or an enum reached bare or through modules), rebuilds the
     dotted expression, and hands it to `check_const_type_expr` — the same
     const path the size slot uses, so no property or folding rule is
     duplicated. Prefix isn't a type ⇒ NULL ⇒ the type reading and its kind
     gate report as before.

   `grammar.bnf`'s `const_atom` gains `type_property_access` (its
   `IDENT , { "." , IDENT }` already covered `dir.count`, but not a
   `builtin_type` head) with a note that *which* names fold is semantic, not
   syntactic. Spec §Const arguments now leads with the general rule ("any
   expression that folds to a compile-time integer") and names the property
   forms in its list, and the size-slot paragraph says outright that the two
   slots accept the same expressions. Tests: `generics/const_type_property_arg`
   (enum count bare / module-qualified / two levels deep, `i32.bits`, `u8.max`,
   arithmetic over them, explicit `<…>` prefix, and the same expressions in
   size slots), `_float_err`, `_unknown_err`, `_variant_err`,
   `_target_width_err` (`isize.bits` is target-defined and folds in neither
   slot), and `_negative_space` (a module-qualified type argument still reads
   as a type; `n < i32.bits` still reads as a comparison).
8. **User types named after builtins** (`struct i32`, `enum str`,
   `module i32`) are accepted but unreachable (primitive/property lookup wins,
   diagnostics like "expected i32, got i32"). Spec disclaims support;
   rejecting the declaration would be kinder.
   **✅ FIXED — reject the type/module, keep the value binding** (2026-07-21,
   decided). A built-in type name is resolved before any user declaration in
   every type and module position, so a user *type or module* named after one
   is permanently unreachable — the actual bug. Those declarations are now
   rejected: `struct`, `union`, `enum` (incl. extern C structs/unions),
   `module`, error groups, `namespace`, and a type/module `import` alias whose
   name is a built-in type name are compile errors. The check is one recursive
   pre-pass in pass1 (`check_builtin_type_name_decls`, before mangling so it
   reads source-spelling names) over the whole declaration tree, plus a guard
   at each import-resolution site where the imported symbol's kind is known;
   the single predicate is `type_from_name` — exactly the lookup every type
   position consults, so "is this a built-in name" can never drift from "does
   this win the lookup".
   **Value bindings are deliberately exempt.** A `let`, parameter, loop
   variable, value import alias, or extern function/constant named `i32`/`any`/
   `char` lives in the value namespace, never collides with the type (`: i32`
   still means the primitive), and is reachable and correct — so rejecting it
   would be a hygiene check, which FC does not do. It would also break real
   code: the standard library gives every container an `any` combinator
   (`array_list.any`, `linked_list.any`, …) and `spec/examples.fc` binds
   `let i8`/`let u64`/`let f32`. The "basically reserved words" framing thus
   holds only for the type/module namespace, not universally — which the spec's
   own taxonomy already anticipated by separating "binding" from "user-defined
   type" names. Member names (struct fields, union/enum variants) are likewise
   untouched: they live in per-type namespaces and cannot shadow a primitive.
   Spec §Identifiers rewrites the built-in-type-names paragraph to state the
   split. Tests: `naming/` (new category) — `struct`/`union`/`enum`/`module`/
   `error_group`/`namespace`/`nested_type`/`extern_struct`/`extern_union`
   `_named_builtin_err`, `import_alias_type`/`_module`/`_module_scoped_builtin_err`
   (all three import paths), and the negative space `value_binding`/
   `toplevel_value`/`member_named`/`combinator_any`/`value_import_alias_builtin_ok`.
9. **Slice-literal elements admit no widening at all** (found while fixing
   §1.3): the element check is a bare `type_eq`, so `i64[2] { 1, 2 }` errors
   "expected i64, got i32" and `const i32*[2] { &a, &b }` errors "expected
   const i32*, got i32*" — while the sibling aggregate positions accept both
   (a `const i32*` *parameter* takes `&a`; a `const i32*` struct *field* takes
   `&a`). The spec's widening list (binary expressions, comparisons, call
   arguments, slice indices, for-range endpoints) does not name this position,
   so today's behavior is defensible-by-omission — but the asymmetry with
   struct-literal fields, which are the same "aggregate literal element"
   position, looks unintended. Deciding it also settles the direction for §7.4
   (tuples). Extending widening here is a language change and needs a call.
   **✅ FIXED — slice-literal elements now widen** (2026-07-19, decided). A
   survey of every widening position (60-odd probe programs) found this to be
   the only genuinely accidental hole: the element type is *written at the
   literal*, so it is an anchor by FC's own definition, and the sibling
   struct-literal-field check twenty lines away in `check_expr` is the same
   shape plus the three lines that try widening. No test asserted the old
   rejection and neither the stdlib nor the demos worked around it. All three
   widening kinds now reach it — numeric (`i64[3] { 1, 2, 3 }`,
   `f64[2] { 1.5f32, 2.5f32 }`), const-add (`const str[2] { buf, "lit" }`),
   and `T*` → `any*` (`any*[2] { &a, &b }`) — on the `alloc` path too. The
   one-directional half is unchanged and now pinned: narrowing, `const`
   stripping, `const T*` → `any*`, and int→float stay errors. A **type-variable
   element type still admits only itself** — the widen attempt is skipped
   rather than the check relaxed, since the generic body's exact-match check is
   the only one that instance ever gets (`generics/generic_slice_lit_elem_err`
   guards the near-regression). Tests: `slices/slice_lit_widen`,
   `slice_lit_widen_repr`, `slice_lit_narrow_err`, `slice_lit_const_strip_err`,
   `slice_lit_anyptr_const_err`, `slice_lit_int_float_err`,
   `memory/alloc_slice_lit_widen`, and `casts_widening/widen_anchor_sites`
   (every anchored position in one program — the executable form of the spec's
   canonical list).
10. **An extern C name can spell a monomorphized instance name** (found in
    review of §5): `check_c_name_collisions` runs at the end of pass1, but
    instance names (`fc__pair__4__i32`, `fc__wide__6___k256`) only exist after
    mono. User *paths* can never reach them (a path component cannot start
    with a digit), but an extern C name is verbatim, so
    `extern fc__pair__3_i32 as p` silently merges with the instance — the
    §5.1 failure mode through the one remaining door. The mandatory `as` alias
    does not help: it fixes the FC-side spelling (no FC identifier can contain
    `__`), while the collision is on the C-side name, emitted verbatim.
    Verified: `extern fc__id__3_i32 as ext_id` next to generic `id` compiles
    clean (fcc exit 0, gcc -Wall -Werror clean) and the extern call binds to
    the static instance in the same TU.
    **✅ FIXED** (2026-07-19, decided: reject the root outright). Any extern C
    name beginning with `fc__` is rejected at the declaration
    (`extern_c_name_in_reserved_root`, parser.c) — functions/constants and
    struct tags alike, alias or not: no header can legitimately export a
    symbol there, so such an extern could only alias a compiler-emitted one.
    The pass1 extern claims stay as defense in depth. Spec §C Interop states
    the rule (and its §Unions transpilation sketch was updated to the named
    payload union while there). Tests `extern_fc_root_err` (every extern
    form, incl. the instance-name spelling), `extern_name_claims_decl_err`
    (repurposed — the module-member spelling is now rejected at the extern
    itself), and `extern_alias_negative_space` extended with the ban's
    negative space: `__` names not under the root, names *containing* `fc__`
    elsewhere, and the single-underscore `fc_` prefix all stay legal.
    Residual, deliberately open: the few derived names containing no `__` at
    all (`fc_main`, `fc_str`, `fc_trunc`) can still be extern-aliased
    silently — banning the whole `fc_` prefix would block a real library that
    happens to use it, so that sliver stays until it earns a rule.

## 8. Diagnostics-polish observations (fix opportunistically)

**Status: the fixable-without-a-design-decision items are done (2026-07-19,
branch `bugsearch`).** 12 tests added; suite 2238 passed / 0 failed (gcc +
clang, -O0 and -O2; LSP wire tests green). `spec/examples.fc`, the stdlib, all
five demos, and both sibling projects (wolf-fc, euler-fc) still compile clean;
the new codegen path is ASan/UBSan-clean. Three items are left open with a note
on why — each needs a language/design call or a substantial new pass, not a
localized fix.

- **Diagnostics leak internal type spellings** — ✅ **RESOLVED as no-change**
  (2026-07-19), now that the blocking decision is made. "return type mismatch:
  expected str, got **const str**" reads as noise only if `const str` is an
  internal spelling that ought to unify with `str`. It isn't: §7.4/§7.9 settled
  that `const` is never stripped implicitly, in any position, so the two types
  named in that message genuinely differ and the difference *is* the error.
  Hiding the qualifier would turn a precise diagnostic into "expected str, got
  str". The message is load-bearing and stays.
- ✅ **FIXED** — *(added while fixing §2.4)* A void inferred into a generic
  **struct literal** — `box { value = v() }` — was caught only by
  `mono_register`'s backstop, reported at the *template declaration* with a
  mangled name. The generic struct-literal path now carries its own use-site
  check (pass2, right before unification): a field value typed void is rejected
  at the **field**, "field 'value': void cannot be a generic type argument". The
  non-generic path already rejected it via the field's concrete type; only the
  type-var field slipped through. Test `generics/void_struct_literal_err`.
- ✅ **FIXED** — *(added while fixing §2.4)* The three union/struct-member
  construction sites passed `GP_TYPE` to `resolve_generic_arg` rather than the
  declared parameter kind, so a **named** const argument was refused at variant
  construction (`maybe_wide<cfg.n>.nothing`) while `wide<cfg.n>` in a type slot
  worked. All three now read `sym->param_kinds[k]`. Test
  `generics/const_union_named_arg` (no-payload variant, bare and arithmetic
  named-const args, with the type-position control). *Residual, left open:* the
  **payload-call** spelling `maybe_wide<cfg.n>.got(x)` resolves the union's type
  args through a separate call-based path that still refuses a named const arg —
  a distinct, deeper site (adjacent to §7.7's kind-inference gap), not one of
  the three field-access sites this fix covers.
- *(added while fixing §2.7)* The concrete negative-slice-length check reads
  the folded literal as signed, so a length that wraps at an **unsigned**
  width slips through: `i32[0u32 - 1u32] { }` still emits
  `int32_t _fc_back_0[4294967295]`. *Left open* — arguably correct under FC's
  wrapping semantics (the value genuinely is 4294967295); catching it (or not)
  is a semantic call, not a repair.

*(Also found while settling §2.8, and **fixed** — a zero-size heap request
reported allocation failure.* `alloc(T, 0)`, `alloc(T[0] { })`, `alloc("")`,
and a heap copy of an empty slice all emitted `malloc(0)` / `calloc(0, n)`,
which C11 7.22.3p1 leaves implementation-defined: the allocator may return
null, which FC's option reads as exhaustion, so `alloc(i32, 0)!` aborted on
some libcs and succeeded on others. glibc returns a unique pointer, so the
suite never saw it. Codegen now routes every count that can legitimately be
zero through a `fc_alloc_n` helper that requests one unit instead — the same
idiom C programmers write by hand — so the pointer is unique and freeable
everywhere while the FC-level length stays 0. It folds away for a constant
count. Reachable at runtime too, since the spec allows `alloc(T[N])` with a
runtime `N`. Test `memory/alloc_zero_size` (all five forms, literal and
runtime zero, ASan-clean); spec §Heap Allocation states the rule.)
- ✅ **FIXED** — Transitive const-eval diagnostics could print a mangled name
  ("in instantiation of 'fc__inner'") and dropped the arguments entirely.
  `fmt_type_inst` (pass2) now renders the source name (mangling stripped to its
  last `__`-separated component — a user name can never contain `__`) plus the
  concrete arguments, so the message reads `inner<8>`, matching the richer form
  the static_assert path already used. Test `generics/const_eval_inst_name_err`.
- ✅ **FIXED** — The oversized-`'n`-in-expression diagnostic suggested "cast the
  use site", but `(i64) 'n` triggers the same error (the check is on the *read*
  of `'n`, before any cast) — unfulfillable advice. The message now states the
  actual rule: "a const parameter is an i32 where it is read as a value; a value
  this large is usable only in a type or size position." Test
  `generics/const_expr_cast_no_help_err`.
- ✅ **FIXED** — "every path through this expression returns" fired even when the
  divergence was a `continue` (or `break`). Both the `let`- and destructure-bind
  messages now read "diverges (returns, breaks, or continues)". Test
  `control_flow/never_bind_continue_err` (the divergence is by `continue`/`break`).
- ✅ **FIXED** — Empty `error g =` / zero-field `struct` / empty `union` / empty
  `enum` produced 9–25-error cascades. All four decl parsers now detect the
  missing indented body (`decl_body_present`), report one clean "must declare at
  least one field/variant/member" (suppressed if the header already failed), and
  bail with a `DECL_ERROR` instead of running the member loop against the next
  declaration. Tests `structs/empty_struct_body_err`,
  `unions/empty_union_body_err`, `enums/empty_enum_body_err`,
  `errors/empty_error_group_err`.
- ✅ **FIXED** — Bounds-abort printed a huge usize index as `index=-1` (the
  index was rendered through a signed `int64_t`). Codegen now chooses the
  reported format from the index operand's own signedness — a new `fc_oob_u`
  (`%llu`) for an unsigned index, the existing `fc_oob` (`%lld`) for a signed one
  — and always prints `len` unsigned. A huge usize prints its true magnitude; a
  genuine signed `-1` still prints `-1`. Tests `slices/oob_index_unsigned`,
  `slices/oob_index_signed` (multi-file, `expected_stderr_contains`).
- ✅ **FIXED** — `defer break` / `defer continue` / `defer return` were rejected
  with a bare "unexpected token 'break' in expression". `parse_block_item`'s
  defer arm now reports "cannot defer a control-flow expression (break,
  continue, or return)" and parses the transfer as an ordinary statement so no
  cascade follows. Tests `defer/{break,continue,return}_in_defer_err` (existing
  `.error` substrings tightened to the purposeful message).
- `f<g>(x)` where `f` isn't generic, `identity<i32> == identity<i32>`, and
  `(identity<i32>)(5)` all get incidental diagnostics (comparison/cast
  misreadings) rather than curated ones. Same family: `(t) x` for a
  *user-defined* type name `t` isn't read as a cast at all (the heuristic wants
  a built-in name or a `* < ! ? .` suffix), so it lands on the juxtaposition
  error. *Left open* — the fix is a pre-pass collecting declared type names (the
  shape `parser_collect_generic_names` already uses for the `<` gate); that is a
  new resolution pass and a disambiguation-strategy decision, not a localized
  polish edit.
- ✅ **FIXED** — A juxtaposition inside an inline match-arm body
  (`| 3 -> n = 1 n = 2`) produced a 3-error "expected '|'" cascade. The arm loop
  now applies the same separator check `parse_block` uses (skipped if the body
  already failed), reporting one clean "expected a newline or ';' between
  statements" and resyncing to the next arm. Test
  `pattern_matching/match_arm_juxtaposition_err`.
- ✅ **RESOLVED as no-change, rule now stated** — *(the third finding of the
  §7.4/§7.9 widening survey)* A parameter whose type *contains* a type variable
  gets no widening (`pass2.c` gates on `!type_contains_type_var(pt)`), which
  looked like a generics gap. Sweeping every shape that can hold a type
  variable against its concrete twin found **exactly one** real asymmetry, and
  it is not composites: `unify` already handles const-add and `T*` → `any*`
  itself, and a concrete parameter beside a generic one always widened; the
  composite cases (`{'a, i64}`) reject in the generic *and* concrete forms
  alike, for §7.4's reason. The survivor is the bare type variable —
  `choose(a: 'a, b: 'a)` rejects `(1i64, 2i32)` where `(a: i64, b: i64)` takes
  it. That one stays rejected: a type variable is bound by the first argument
  mentioning it (verified — `choose(1i64, 2i32)` says "expected i64",
  `choose(2i32, 1i64)` says "expected i32"), so widening would make a call's
  meaning depend on argument **order**, and removing that order-dependence
  means a common type across every site mentioning `'a` — whole-program
  unification, which FC's directional inference rules out. It is therefore the
  same symmetric position as an `if`/`match`/`return` join, and the existing
  rule already governs it; it only *looked* arbitrary because the spec's list
  of symmetric positions never mentioned type variables. §Implicit Widening now
  carries that bullet plus the order-dependence rationale. Tests
  `generics/typevar_symmetric` (positive, the cast bridge, the concrete twin's
  order-freedom, and a concrete param beside a generic one) and
  `typevar_symmetric_err`.
- ✅ **FIXED** — *(found while surveying widening for §7.4/§7.9)* A **generic**
  call's argument mismatch reported a bare `"argument 2: type mismatch"` with
  no types at all, while the non-generic path printed `expected … got …`. It
  now prints both, with the bindings established by earlier arguments
  substituted in — so `choose(42, true)` says "expected **i32**, got bool"
  rather than "expected `'a`", and `add(wide<4>, wide<8>)` says "expected
  **wide<4>**, got wide<8>". The substitution is message-only and drains any
  const-eval error it might trip (the `codegen.c:789` idiom) so it cannot
  consume a failure another site owns. Tests `generics/type_mismatch` and
  `generics/const_fn_arg_mismatch` updated to the informative text.
- A slice literal whose element type is itself a slice needs parens:
  `i32[][2] { a, a }` fails, `(i32[])[2] { a, a }` works. The head scan takes
  the first `[` as the literal's, so an unparenthesized `[]` element suffix is
  never seen. *Left open* — an ergonomics nit (parens express it), and the head
  scan is load-bearing for the §1.3/§1.4 slice-literal grammar; reshaping it is
  not a localized fix.

## Areas swept clean (for the record)

Results/errors/`?`/discard/defer (~55 probes: all indirect discard paths,
anchoring, LIFO unwind ordering, error-code determinism, extern protocols,
escape-through-`ok`) came back **zero bugs**. Also clean: checked/unchecked
boundaries incl. INT_MIN/-1 and shift masking; float→int saturation; enum_of
edges; interpolation format/argument *portability* (all `%lld`/`%llu` +
explicit casts — 16-bit-int safe); atomics; module resolution order and
file-import isolation; `<` disambiguation negative space (comparisons keep
their meaning); static_assert placement rules; capture-by-copy runtime
semantics; slice bounds/equality; option nesting (construct/match/unwrap);
`.errcodes` lifecycle.
