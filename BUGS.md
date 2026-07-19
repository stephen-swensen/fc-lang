# Bug hunt — 2026-07-18 (branch `bugsearch`)

Systematic negative-space / coverage-completeness search over the whole feature
surface (9 parallel probe sweeps, ~400 probe programs, all findings re-verified
against `fcc 1.0.0-rc.6` @ 3e17dec). **45 confirmed bugs, each demonstrated by
one new failing test** in `tests/cases/`. Baseline before adding them: 2063
passed, 0 failed (gcc). After: 2063 passed, **45 failed** — every failure below
is intentional and should flip to PASS as its bug is fixed.

**Status: 35 / 45 fixed** (+9 found and fixed along the way: §1.5 during §1's
triage, two variant-construction holes during §2.5, three more — two uncovered
spellings of §2.3/§2.4 plus a `for a, a` collision — caught by an adversarial
review of the §2 diff, and three during §4: a wrong-length string *pattern*
compare, an option typedef missing behind any pointer, and §6.1 which the §4.8–
4.11 root-cause fix closed outright). §1 (parser / lexer), §2 (pass2
judgments), §3 (escape analysis) and §4 (codegen emits invalid C) complete —
see those sections for what landed. §5–§8 untouched; current suite: 2180
passed, 10 failed (gcc + clang, -O0 and -O2; LSP wire tests green).

One **new bug** was found while writing §4's tests and is recorded as a failing
test rather than fixed — see §4.14.

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
failures are §5, §6, and the new §4.14), gcc + clang, -O0 and -O2, LSP wire
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

### 4.14 `generics/nested_instance_arg_in_generic` — ⏸️ NEW, NOT FIXED

Found while writing 4.4's test; **pre-existing** (verified against d5cfe7b), and
left as a failing test rather than fixed — it is a monomorphization-discovery
bug, not a codegen one, and out of scope for this session's batch.

A **generic** function whose body instantiates another generic function at a
*generic-instance* argument type never gets the resulting struct instance
registered: for `let bx2 = (v: 'a) -> bx(bx(v))`, the emitted C references an
undefined `fc__box__14_fc__box__3_i32` (`box<box<i32>>`), and the callee's own
instance is mangled `fc__bx__7_fc__box` — a bare `box` with no arguments, so the
two names do not even agree. The identical nesting from a **non-generic** caller
(`bx(bx(5))` in `main`) works, which localizes it to substituting the caller's
`'a` into an already-generic argument type. The `let`-bound spelling
(`let inner = bx(v)` then `bx(inner)`) fails the same way, so it is not about
call nesting. Note `discover_in_expr`'s EXPR_CALL arm only registers a callee
when `type_arg_count > 0` (explicit type args); an *inferred* generic call in a
generic body reaches instantiation by another route, which is where to start.
The test pins the working non-generic twin alongside the failing case.

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
`nested_instance_arg_in_generic` (**failing** — see 4.14).

## 5. C-identifier hygiene / mangling collisions

- **5.1 `c_hygiene/module_named_fc`** — module/namespace named `fc`: member
  mangling `fc__<name>` collides with the top-level user-decl prefix
  `fc__<name>`. Globals silently MERGE (compiles clean, wrong values); struct
  twins produce invalid C. Make the schemes disjoint (or reject `fc` as a
  module/namespace name — test expects the mangling fix, i.e. both asserts
  pass).
- **5.2 `c_hygiene/internal_name_collisions`** — params, for-vars, and match
  bindings keep their source spelling while codegen mints `_ctx`, `_sg*`,
  `_fe*`, `_subj*`, `_match*`, `_fc_back_*`, `_l_name_N` in the same scopes.
  Worst shapes are SILENT wrong-runtime: param `_sg0_0` in interpolation emits
  self-initialization `int32_t _sg0_0 = _sg0_0;` (legal C11, garbage value);
  binding `_match1` shadows the match result var (result uninitialized).
  For-var `_fe0` self-compares the loop bound; binding `_subj0` collides with
  the subject temp; param `_ctx`/`_l_x_0`/`_fc_back_0` are invalid C. Give
  params/for-vars/pattern bindings the same unique-name treatment locals get.
- **5.3 `c_hygiene/param_shadows_runtime`** — params named `abort`, `fc_str`,
  `snprintf` (etc.: `memcmp`, `fprintf`, any `fc_*` helper/typedef) shadow
  file-scope names the generated body references. `is_c_reserved`
  (src/common.c:138) covers only C keywords; it needs the libc names codegen
  emits calls to plus the whole `fc_*` runtime namespace.
- **5.4 `c_hygiene/variant_named_tag`** — payload variant named `tag`
  duplicates the injected discriminant member inside the anonymous union
  ("duplicate member 'tag'").
- **5.5 `c_hygiene/union_tag_struct_collision`** — the tag-enum typedef
  `fc__<union>_tag` lives inside the user-reachable `fc__<name>` space; a user
  struct named `shape_tag` collides. Derived names need a reserved spelling
  (double-underscore separator is unreachable since `__` is banned in FC
  identifiers).
- **5.6 `c_hygiene/pattern_binding_c_keyword`** — pattern bindings are the one
  binding form not run through `c_safe_ident`: `| circle(int) ->` emits
  `int32_t int = ...`. (Params, for-vars, fields, variants already escape.)
  Overlaps with 5.2's fix if bindings get unique names.

## 6. String interpolation semantics

- **6.1 `strings/pct_escape_static`** — ✅ **FIXED 2026-07-19** as part of §4's
  root cause A (the decode/re-encode of string literals) — see there. In a
  *static* (segment-free) literal, `%%` was not collapsed in the emitted bytes
  but `.len` was computed as if it were: `"50%% off"` → content `50%% of` with
  len 7. The cstr twin had the raw bytes too. Both now fold, from the single
  decoder that also computes the length. Test `strings/pct_escape_forms`.
- **6.2 `strings/interp_width_overflow_err`** — width digits accumulate
  through 32 bits unvalidated: `%99999999999d` wraps to ~1.2 GB backing array
  → SIGSEGV at frame setup; `%2147483648d` emits C that fails
  `-Werror=format-truncation`; `%4294967297d` silently ignores the width.
  Also consider capping the hoisted buffer (a non-wrapping `%9999999d` = 10 MB
  stack array). Expect a compile error for unrepresentable widths.
- **6.3 `strings/interp_precision_overflow_err`** — precision wraps the same
  way: `%.4294967297s` becomes precision 1 → silent data loss ("hello" → "h").
- **6.4 `strings/interp_unsigned_decimal`** — `%d` maps unconditionally to
  `"%lld"` + `(long long)` cast: `%d{u64.max}` prints `-1` (and the
  conversion is implementation-defined). The `%u`/`%x` paths already switch on
  signedness; `%d` should too. Spec: `%d` covers all integer types.

## 7. Design questions / spec contradictions (no tests — decide first)

1. **Discarded results smuggled in aggregates**: `{ fallible(), 9 }` or
   `some(fallible())` in statement position compiles — the discard check
   guards only top-level `TYPE_RESULT`. Letter-of-the-rule vs. its intent;
   probably worth guarding any discarded value *containing* a result.
2. **`return`/`break`/`continue` in grammar's `primary_expr`** but rejected in
   general expression positions (call args, match subject) — parser only
   allows them as block/inline-sequence items. Current semantics are
   defensible; then grammar.bnf should stop listing them under `primary_expr`.
3. **Explicit type args on struct literals** (`box<i32> { ... }`,
   `pt<4> { ... }`): grammar.bnf line ~922 sanctions them, the parser never
   consumes them, and the fallback misparse yields nonsense diagnostics
   ("tuple literal requires at least 2 elements"). Accept per grammar, or fix
   grammar + add a targeted diagnostic.
4. **Spec §Tuples' own examples are uncompilable**: `{i32, str}` can never
   receive `{ 1, "a" }` ("expected {i32, str}, got {i32, const str}") because
   tuple types are synthesized structurally and the literal's `const` leaks
   in. Either the spec examples change to `const str`, or tuples get
   elementwise nonconst→const acceptance (repr-preserving widening precedent
   exists: `i32*` → `const i32*`).
5. **Spec §Tuples contradicts itself on `t[0] = v`**: line ~2742 says element
   rebinding "needs `let mut`", but §One-rule-three-knobs says contents are
   always assignable — and the compiler follows the latter. Fix the spec
   prose (or change the rule).
6. **Closure captured-struct field mutation** mutates a per-call temporary
   (`f()` sees its own fresh copy each call, writes never persist) — identical
   for stack and heap closures. Consistent, but the spec's "capture by copy
   (at creation)" reads as one persistent copy. Needs a spec sentence.
7. **Const-generic argument slots reject `dir.count` / `i32.bits`**
   (`buf<dir.count>` errors) while the same expressions fold in *size* slots
   (`u8[dir.count]` works). The spec's const-arg grammar technically excludes
   them; the asymmetry looks unintended.
8. **User types named after builtins** (`struct i32`, `enum str`,
   `module i32`) are accepted but unreachable (primitive/property lookup wins,
   diagnostics like "expected i32, got i32"). Spec disclaims support;
   rejecting the declaration would be kinder.
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

## 8. Diagnostics-polish observations (no tests; fix opportunistically)

- Diagnostics leak internal type spellings: "return type mismatch: expected
  str, got **const str**" where `const str` isn't user-annotatable in that
  position; "string pattern on non-str type const cstr".
- *(added while fixing §2.4)* A void inferred into a generic **struct
  literal** — `box { value = v() }` — is caught only by `mono_register`'s
  backstop, which reports at the *template declaration* with the mangled name
  ("cannot instantiate 'fc__box' with void") rather than at the literal. Same
  family as the mangled-name item below. The struct-literal inference path
  needs a use-site check of its own; correctness is not at risk (no such
  instance reaches codegen).
- *(added while fixing §2.4)* The three union-variant construction sites pass
  `GP_TYPE` to `resolve_generic_arg` rather than the parameter's declared
  kind, so a **const** generic argument is still refused there:
  `maybe_wide<cfg.n>.nothing` fails while `wide<cfg.n>` in a type position
  works. Pre-existing (the previous `resolve_type` call had the same effect);
  the fix is `usym->param_kinds[k]`, and it is adjacent to §7.7's asymmetry.
- *(added while fixing §2.7)* The concrete negative-slice-length check reads
  the folded literal as signed, so a length that wraps at an **unsigned**
  width slips through: `i32[0u32 - 1u32] { }` still emits
  `int32_t _fc_back_0[4294967295]`. Arguably correct under FC's wrapping
  semantics (the value genuinely is 4294967295), but it is the one spelling
  of "negative length" the rule does not catch.

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
- Transitive const-eval diagnostics can print mangled names
  ("in instantiation of 'fc__inner'") where the static_assert path prints
  `inner<8>`.
- The oversized-`'n`-in-expression diagnostic suggests "cast the use site",
  but `(i64) 'n` triggers the same error — unfulfillable advice.
- "every path through this expression returns" fires for paths that
  `continue`.
- Empty `error g =` and zero-field `struct` produce 9–25-error parse cascades.
- Bounds-abort prints a huge usize index as `index=-1` (signed rendering).
- `defer break` / `defer return` rejected with generic "unexpected token"
  rather than a purposeful message.
- `f<g>(x)` where `f` isn't generic, `identity<i32> == identity<i32>`, and
  `(identity<i32>)(5)` all get incidental diagnostics (comparison/cast
  misreadings) rather than curated ones. Same family: `(t) x` for a
  *user-defined* type name `t` isn't read as a cast at all (the heuristic wants
  a built-in name or a `* < ! ? .` suffix), so it lands on the juxtaposition
  error. A pre-pass collecting declared type names — the shape
  `parser_collect_generic_names` already uses for the `<` gate — would let all
  of these get semantic answers instead of token-shape guesses.
- A juxtaposition inside a match-arm body (`| 3 -> n = 1 n = 2`) produces a
  3-error "expected '|'" cascade; block bodies report one clean "expected a
  newline or ';' between statements". The arm loop could apply the same check
  after `parse_body`.
- A slice literal whose element type is itself a slice needs parens:
  `i32[][2] { a, a }` fails, `(i32[])[2] { a, a }` works. The head scan takes
  the first `[` as the literal's, so an unparenthesized `[]` element suffix is
  never seen.

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
