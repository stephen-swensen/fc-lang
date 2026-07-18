# Bug hunt — 2026-07-18 (branch `bugsearch`)

Systematic negative-space / coverage-completeness search over the whole feature
surface (9 parallel probe sweeps, ~400 probe programs, all findings re-verified
against `fcc 1.0.0-rc.6` @ 3e17dec). **45 confirmed bugs, each demonstrated by
one new failing test** in `tests/cases/`. Baseline before adding them: 2063
passed, 0 failed (gcc). After: 2063 passed, **45 failed** — every failure below
is intentional and should flip to PASS as its bug is fixed.

**Status: 4 / 45 fixed.** §1 (parser / lexer) complete — see the section header
for what landed. §2–§8 untouched; current suite: 2079 passed, 41 failed (gcc).

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

All four fixed on branch `bugsearch`; suite 2079 passed / 41 failed (the
remaining failures are §2–§6), gcc + clang, -O0 and -O2, LSP wire tests green.
11 tests added beyond the four repros (below). Two shared root causes turned out
to be twins-in-the-parser, closed by one helper each.

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
it still pins "'e' is a type, not a value"; the two malformed-float tests now
expect the separator error.

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

**Tests added** (11): `expressions/juxtaposed_let_err`,
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
readings). Spec: §Continuation, §Escape Sequences, §Slice literals updated.

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
  value") were equally incidental. A lexer rule rejecting an identifier
  character adjacent to a numeric literal ("invalid suffix on numeric literal")
  would name the actual problem — same family as 1.2, ~10 lines. → **proposed
  as 1.5, awaiting go-ahead.**
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

## 2. Missing type-checker (pass2) judgments — most emit broken C or run wrong

### 2.1 `control_flow/loop_mixed_break_err` — bare `break` + `break v` in one loop
A value loop mixing `break` and `break 5` passes pass2; the emitted
`_loop_result` is unassigned on the bare-break path → clang
`-Wsometimes-uninitialized` rejects the C; gcc binaries read garbage (observed
wrong values at -O0). Loop-result unification treats a valueless `break` as
non-contributing instead of anchoring the loop type to void and reporting the
mismatch (the arm-disagreement diagnostic "break type mismatch" already
exists — extend it here).

### 2.2 `pattern_matching/match_void_subject_err` — void match subject
`match f() with` where `f` returns void emits `void _subj0 = fc__f(NULL);`.
pass2 rejects void in let bindings and call args but never checks the match
subject. Also reachable via `match (n = 5) with` (assignment is void).

### 2.3 `generics/bare_generic_type_err` — bare generic name as a type
A generic struct name with no type args in a field type, `default(box)`, or
`alloc(box)!` sails through to codegen → `fc__box` referenced, never defined
(fcc exit 0, invalid C). The name never reaches `mono_register`'s choke point,
so the existing "requires explicit type arguments" judgment never fires —
classic "a choke point only covers what flows through it". Cover the bare-name
case in pass1/pass2 type resolution. (Related papercuts: bare generic *param*
type `(b: box)` is accepted silently if never called; slice-literal element
form errors with the odd "expected box, got box<i32>".)

### 2.4 `generics/void_typearg_err` — `void` instantiates `'a`
`idv(v())` with `v` void-returning monomorphizes `idv<void>` and emits
`void x` as a parameter → invalid C. Generic argument unification lacks the
void-as-value guard the non-generic call path has.

### 2.5 `unions/unknown_variant_construction_err` — unknown no-payload variant
`u.zzz` on a union with no variant `zzz` is silently accepted (types as `u`)
and emits undeclared `fc__u_tag_zzz`. The call path `u.zzz(5)` checks
("union 'u' has no variant 'zzz'"); the no-payload field-access path doesn't.
Also makes `u.count` on a union "work". Non-call twin of a call-shaped check.

### 2.6 `pattern_matching/match_dup_binding_err` — duplicate binding in one pattern
`| { a, a } ->` emits two `int32_t a = ...;` in one C scope → redefinition
error. **Fix-direction:** the test expects a "duplicate binding" diagnostic
(Rust/OCaml precedent); the alternative is unique codegen names with
last-wins. Note the let-destructure twin `let { a, a } = { 1, 2 }` currently
*compiles* (last wins) — whichever direction is chosen must pin both paths.

### 2.7 `const_eval/slice_lit_negative_size_err` — negative concrete slice-literal size
`u8[-4]{}` (also `u8[1 - 5]{}`, negative named const) accepted; emits
`uint8_t _fc_back_0[18446744073709551612]`. Spec: "A statically-negative
length is therefore a compile error." Only the deferred per-instance generic
path checks `sz <= 0`; the concrete EXPR_ARRAY_LIT path (src/pass2.c ~7284)
reads the folded u64 and never checks. The concrete twin of a generic check.

### 2.8 `const_eval/slice_lit_zero_size_err` — zero concrete slice-literal size
Same hole as 2.7: `u8[0]{}` compiles and runs while the generic twin
`zeros<0>()` is mandated to fail (existing test
`generics/const_slice_size_zero_err`: "slice literal length must be positive").

### 2.9 `const_eval/i64_literal_const_arith` — i64 literals rejected in const-arg arithmetic
Const args are spec'd to evaluate in the i64 domain, and a *lone* literal
`f<4000000000>` is accepted — but the same literal inside const-arg
arithmetic (`wide<4000000000 / 10 + 0>`) goes through the normal i32-literal
check and errors, then evaluates as 0 producing a bogus "fixed array size must
be positive, got 0" cascade (missing poison suppression — fix that too).

### 2.10 `enums/module_enum_count_size` — `gfx.mode.count` not const in size slots
The 3-part module-qualified spelling is rejected ("must be a compile-time
constant", duplicated diagnostic) while bare `mode.count`, imported forms, and
deep module-const paths all fold. The const-size dotted-path folder doesn't
descend into an enum reached through a module component. Spec: `E.count` is an
i32 constant expression usable in slice-literal lengths.

## 3. Escape-analysis holes (all ASan-verified stack-use-after-return) + one wrong-reject

The spec claims provenance propagates through bindings, branches, and option
wrapping, and that returning/heap-storing stack pointers are compile errors.
Direct forms are caught; these paths launder the tag. Root pattern: bindings
introduced by anything other than a plain `let` are registered via `scope_add`
(hardcoded `PROV_UNKNOWN`) instead of `scope_add_prov`, and two `alloc` forms
skip the provenance walk. One fix sweep should close 3.1–3.4 together; 3.5–3.7
are separate alloc-path checks.

- **3.1 `escape/match_binding_stack_ptr_err`** — `| some(p) -> p` returns a
  dead-frame pointer (the unwrap twin `some(&x)!` IS caught). Also launders
  `free` (gcc even rejects the emitted C with `-Werror=free-nonheap-object`)
  and heap stores. src/pass2.c ~2725/2774/2797.
- **3.2 `escape/tuple_destructure_stack_ptr_err`** — `let {p, n} = {&x, 1}`
  then return `p`.
- **3.3 `escape/for_elem_stack_ptr_err`** — `for p in ptrs do return p`
  (indexed twin `ptrs[0]` IS caught: `escape/return_indexed_stack_ptr_err`).
  src/pass2.c ~2828/7551.
- **3.4 `escape/loop_break_stack_ptr_err`** — `break &x` → loop value has
  `PROV_UNKNOWN`; if/match joins merge provenance, loop doesn't
  (src/pass2.c ~7475).
- **3.5 `escape/alloc_closure_stack_capture_err`** — `alloc(lambda)` promotes
  the ctx without walking captured values' provenance; heap closure capturing
  a stack slice/pointer/stack-closure escapes. Explicit-store twins are
  rejected (spec cites both).
- **3.6 `escape/alloc_union_stack_payload_err`** — `alloc(u.held(&x))!`; the
  struct-literal twin `alloc(node { data = &x })` IS rejected. The union
  constructor sets `PROV_STACK` but the alloc path doesn't consult it.
- **3.7 `escape/alloc_slice_stack_ptr_elems_err`** — `alloc(ptrs)!` deep-copies
  the top level only; copied *pointer values* still target the dead frame.
  Needs a `type_has_provenance`-style element check on the alloc-promotion path.
- **3.8 `memory/free_heap_via_stack_slice_elem`** (wrong-reject) — a value
  *loaded* from a stack slice element inherits the slice's stack provenance,
  so a heap slice/closure stored in a stack slice can never be freed — not
  even via a local copy ("cannot free stack-allocated memory", and for
  closures a factually wrong "its context was not heap-allocated"). Freeing
  through a stack *struct field* or a fn param works. The element-load should
  yield the value's own (unknown) provenance; only `&fs[0]` should inherit the
  backing's.

## 4. Codegen emits invalid C for legal programs

- **4.1 `pattern_matching/match_catchall_nonfinal`** — an unguarded catch-all
  arm (`_` or binding) in non-final position emits a bare `{...}` block, so
  the next arm's `else` has no `if` → "'else' without a previous 'if'". FC
  deliberately allows redundant arms (first-match-wins; see
  `exhaustiveness/exhaust_union_dup_variant`). Guarded catch-alls and
  or-patterns containing `_` are unaffected (they still emit an `if`).
- **4.2 `enums/nested_enum_pattern_union`** — enum-variant pattern nested in a
  union payload emits tagged-union codegen for the enum:
  `_subj0.chosen.tag == fc__color_tag_red` on an int typedef. Or-pattern and
  union→option→enum forms too. Nested enum under bare option/result works —
  only the union-payload path lacks the enum (scalar-compare) branch.
- **4.3 `pattern_matching/nested_union_binding`** — a binding at depth 2 of a
  nested union pattern (`| wrap(chosen(c)) ->`) is type-checked but never
  declared in the C ("'c' undeclared"). Binding emission stops at depth 1.
- **4.4 `generics/const_arith_transitive_fn_typedef`** — name capture: when
  `mk2<'n>` calls `mk<'n * 2>`, the fn-type typedef collection substitutes the
  *caller's* `'n` into the callee's signature, emitting a typedef over the
  phantom `wide<64>` which is never instantiated. Renaming the callee's param
  to `'m` avoids it. **Latent hazard for std::wideint's `mul_wide` shape** —
  works there only because every width instance happens to exist.
- **4.5 `generics/fn_typedef_instance_field_dup`** — a struct field's function
  type over a monomorphized instance (`measure: (wide<64>) -> i32`) plus a
  function of the same signature emits the same typedef twice → C11
  "conflicting types". Repros with type-arg instances (`box<i32>`) too;
  fn-typedef dedup keys pre- vs post-mangling names inconsistently.
- **4.6 `structs/nested_fixed_array_field_err`** — `m: u8[2][3]` accepted,
  emits `uint8_t[2] m[3];` (not a C declarator). **Fix-direction:** the test
  expects rejection ("fixed array"); supporting it means emitting the
  inside-out C declarator `uint8_t m[3][2]` and auditing every consumer of
  fixed-array reprs.
- **4.7 `options/alloc_nested_option`** — a nested option type whose *only*
  occurrence is `alloc(T)`'s type operand never gets its typedef emitted
  (`fc_option_fc_option_int32_t` undeclared). The option-typedef discovery
  walk skips alloc's type argument.
- **4.8 `strings/trigraph_literal`** — `??` runs in string literals emitted raw
  → gcc/clang `-Werror=trigraphs`; a strictly conforming C11 translator would
  rewrite the bytes. Emit `?` escapes (`"?\?!"`). Also reachable via
  diagnostic text embedded in unwrap/assert messages (source text `i32??`).
- **4.9 `strings/hex_escape_adjacent_digit`** — FC's `\x` is exactly two
  digits; C's is greedy. `"\x411"` is passed through verbatim → "hex escape
  sequence out of range". Re-encode decoded bytes instead of echoing escapes.
- **4.10 `strings/quote_in_interp`** — `\"` in the static text of an
  *interpolated* string emits `\\"` (escaped backslash + raw quote),
  terminating the C format literal early. The static-only path handles it.
- **4.11 `strings/nul_in_interp`** — `\0` in interpolated static text lands as
  a real NUL inside the snprintf format (`-Werror=format-contains-nul`; at
  runtime everything after would be dropped). Static text with NULs can't ride
  a format string — needs segment splitting/memcpy like the static path.
- **4.12 `expressions/xor_literal_pow`** — `10 ^ 6` / `2 ^ 8` emitted verbatim
  → gcc/clang `-Werror=xor-used-as-pow`. Fold or re-spell literal operands.
- **4.13 `equality/self_compare`** — `x == x` emits `_l_x_0 == _l_x_0` →
  `-Werror=tautological-compare` (integers/bools only; floats are exempted by
  compilers for the NaN idiom). Spec's no-op rule covers only self-assignment.

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

- **6.1 `strings/pct_escape_static`** — in a *static* (segment-free) literal,
  `%%` is not collapsed in the emitted bytes but `.len` is computed as if it
  were: `"50%% off"` → content `50%% of` with len 7. A trailing `%%` only
  *looks* right because the len truncation chops the extra `%`. cstr twin has
  the raw bytes too. Only interpolated strings hit the snprintf path that
  collapses.
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
