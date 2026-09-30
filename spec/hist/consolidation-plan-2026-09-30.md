# Consolidation plan

Finished 2026-09-30: every group below is merged (see Status). One copy is
left on purpose, pending a language decision: the language server's
`companion_of_type_sym` does not pair a type with a same-named `error` group,
while pass2's `companion_of` does, and the spec speaks only of modules pairing
with structs and unions.

## Goal

Goal 3 of the maintainability plan (`spec/hist/maintainability-plan-2026-09-29.md`)
is that a maintainer can change one piece of logic without a copy of it
elsewhere silently going stale. A fresh-eyes review on 2026-09-30 found that
it is not met: many rules still live in two to five hand-kept copies, and
about ten of those copies had already drifted into user-visible bugs. This
plan merges each group into one implementation.

Done when every group below is merged into a single source of truth (a
function, a table, or a switch the compiler checks for completeness), or has
a comment at both sites saying why the two differ on purpose.

## Working rules

- One group at a time. First confirm the duplication by reading both copies.
- A refactor leaves the emitted C and the error output byte-identical
  (`tools/emit-corpus.sh` over the tests, demos, wolf-fc and euler-fc, against
  the baseline taken after phase A). Every difference must be a drift bug the
  merge fixes. Each such bug gets a test that fails on the pre-B compiler.
- `make check` stays green after every group.
- A bug that the merge does not fix, or a language question, is reported and
  asked about, not fixed in passing.

## Groups

### B1. Operations governed by `checked` and `unguarded`

pass2's `expr_node_is_governed_guard` and `expr_node_is_governed_overflow`
decide whether a marker is redundant; about ten codegen sites decide, again,
whether to emit each guard or overflow check (`emit_binary`, `emit_cast`,
`emit_index`, `emit_slice`, the interpolation and `(cstr[N])` paths, unary
minus), and `detect_features_expr` a third time. A prose list in pass2 has
already drifted (it omits subslicing). Merge into one classification in
`facts.c` that both passes call.

Also fix here: float `/` (and unsigned `/`, `%` under `unguarded`) is emitted
without operand sequencing, `(f(&a) / f(&a))`, which breaks left-to-right
evaluation.

### B2. Which headers the C needs

`detect_features_expr` predicts, before emission, which headers each emitter
will need, and has to be kept in step with every emitter by hand (the
generic-division bug of phase A came from it). Derive the preamble from what
is actually emitted instead: emit the body first, record each need where the
code that creates it is written, then write the preamble.

### B3. The constant-expression grammar

The operator set appears in pass2's `const_expr_node_ok`, `types.c`'s
evaluator and `const_expr_print`; the node kinds are hard-coded in about ten
walkers with a `default`; `const_type_eval`'s type-variable arm repeats
`const_expr_eval`'s; the parser's `starts_const_atom` lacks `sizeof`/`alignof`
where its three siblings have them (a test exists to prevent the resulting
error cascade, and `wide<'n * sizeof(i32)>` still produces it). One
description of the grammar, used by each consumer.

### B4. Format specs and escapes

The printf format-spec grammar is parsed in five places (lexer, three in
`facts.c`, codegen); the conversion set in two (lexer, and a pass2 `default`
that can never run); the escape set in two (lexer, `common.c`'s decoder, whose
`default` would silently mis-decode an escape added only to the lexer). One
parser and one table each.

### B5. Generic and concrete twins in pass2

- The unary operator checks, concrete and per-instance.
- `check_let` and `check_decl_let`.
- Module-context setup, repeated at five sites.

### B6. Names

- A generic instance's C name is computed two ways (`mangle_generic_name` in
  monomorph and `mangle_type_name` in types).
- The type-to-companion-module lookup appears at three pass2 sites and in the
  language server, whose copy already disagrees about error groups.
- The enum `count` property is special-cased at four sites outside
  `TYPE_PROPERTIES`, the table that calls itself the one list.

### B7. Length rules

Slice-literal and fixed-array length checks (negative, over `--len-repr`,
element count) are written at several pass2 sites and in the parser, with
different messages.

### B8. Parser

- The four declaration-body member loops have drifted (after `static_assert`,
  a union body skips the end-of-line check).
- `token_starts_prefix_expr` hand-copies `parse_prefix`'s cases and misses
  two.
- The parenthesized `;` sequence is a separate copy of the block-statement
  parser, so `(ignore f(); 1)` fails although the spec says `;` is exactly a
  same-level newline.

### B9. Codegen internal copies

- The trampoline rule, four times, plus a pass2 copy.
- The null-sentinel option rule; one copy misses `any*` and emits a dead
  `fc_option_void_ptr`.
- `emit_type` and `emit_type_ident`.
- `zero_brace_extra` and `zero_agg_depth`.
- `emit_self_parens` and `_bseq`.
- The union payload lookup, three ways.

### B10. Walks with their own child lists

`expr_has_side_effects`, `expr_structurally_equal` (codegen) and `sr_flow`
(pass2) list children themselves and end in a `default`, so a new kind is
silently mishandled. Move them onto the visitor or make them exhaustive.

### B11. Small ones

- The module-cycle message is built twice, and one copy says "through
  imports" when there are none.
- Teardown in `lsp.c` and `main.c`/`analyze.c`.
- The `--len-repr` default of 64, set in three places.
- Pinning an undecided generic parameter's kind, four times.
- The generic-validation memo key, twice.
- `pass1.c`'s two module-member registration loops.
- DeclKind to completion kind, twice (language server).
- The struct and union arms of `discover_nested_types`.
- `normalize_type_sizes` and `canonicalize_field_stubs` both fold sizes.

## Status

- B1 done. `facts_guard_governs`/`facts_overflow_governs` (facts.c) are the
  one list; pass2's redundancy check and every codegen gating site call them.
  `binary_needs_seq`/`int_divmod_guards` replace the two copies of the
  sequencing condition; float `/` and unguarded unsigned `/`/`%` are now
  sequenced (test expressions/div_operand_order; latent, so it cannot fail
  on the pre-B compiler: gcc and clang happen to evaluate left to right).
- B2 done. The body is emitted first and scanned (`scan_body_needs`);
  `detect_features_*` deleted. The stdio helpers are one table
  (`STDIO_HELPERS`). Output change: seven type-variable-property tests no
  longer include an unneeded `<float.h>`.
- B3 done. The operator set is const_binary_op/const_unary_op (types.c);
  const_binary_op_supported/const_unary_op_supported derive from them, and
  const_expr_node_ok, try_eval_const and const_expr_eval all use them, as
  does const_cast_value. const_param_value is the one const-param lookup.
  const_expr_print spells operators with token_kind_name. The tree walkers
  that only search (expr_refs_const_param, expr_mentions_type_var,
  const_expr_walk_vars) use the ast.c visitor. The parser's starts_const_atom
  is the one "can begin a const atom" list and now includes sizeof/alignof
  (test generics/const_arg_sizeof_in_arith_err). Output byte-identical.
- B4 done. interp_spec_scan (common.c) is the one format-spec reader
  (lexer, facts, codegen), interp_conv_class the one conversion table (the
  lexer's accepted set and pass2's operand check, now an exhaustive switch
  over the class), simple_escape_byte the one escape table (lexer and
  decode_str_lit). Output byte-identical.
- B5 done. unary_operand_error is the one prefix-operator rule (concrete and
  per-instance; a generic `-a` at an enum now gets the enum message, test
  generics/generic_unary_enum_err). check_let_init and bindable_type are
  shared by local and declared lets, which fixes `let x = void()` at module
  or file level (fcc exited 0 with broken C; test bindings/decl_let_void_err).
  enter_module_scope (renamed from enter_module_scope_on_demand) is also the
  in-order walk's module entry. Output byte-identical otherwise.
- B6 done. instance_base_name (types.c) is the one rule for the template name
  an instance is spelled from; mangle_type_name, discovery and codegen use it.
  discovery's struct and union arms are one discover_aggregate, which records a
  looked-up template symbol on the node. A stub already renamed in place is now
  discovered under its real name (it was double-mangled, so its instance was
  silently never registered); in the infinite-family test the depth cap now
  names the family, the dangling backstop no longer repeats an error already
  reported, and the message names the source type ('b' rather than 'fc__b').
  companion_of is the one type/module companion lookup in check_ident.
  type_enum_count_is is the one enum `count` rule (pass2 checks and folding,
  LSP completion).
- B7 done. static_length_error is the one length rule (slice literals,
  raw-parts lens and their const-context fold, fixed-array sizes including
  per instance, string literals via check_string_lit_length). The parser
  keeps only the unreadable-literal case for a fixed-array size; zero is
  judged in pass2 with every other size. Raw-parts messages now use one noun
  ("slice literal length") and give the value.
- B8 done. parse_body_lines is the one declaration-body loop (struct,
  union, enum, error group), each body supplying a line function;
  body_static_assert is the one static_assert line for struct and union
  bodies. A union body's static_assert line now gets the end-of-line check
  (test unions/static_assert_line_trailing_tokens_err). token_starts_prefix_expr
  gains `const` and `error`, and tools/check-keywords.py checks it against
  parse_prefix's cases. The parenthesized sequence is parse_inline_seq, the
  block item parser, so `(ignore f(); 1)` parses (test
  expressions/paren_sequence_block_items). Output byte-identical otherwise.
- B9 done. fn_value_is_context_free (facts.c) is the one rule for a function
  value C can call through a trampoline (pass2's extern-argument check, and
  codegen's extern argument, `&f` and trampoline collection, which name it
  with trampoline_target); a module-qualified function passed to an extern
  (`qsort(..., cmp.asc)`) is now accepted like an imported or bare one (test
  extern/module_fn_to_extern). option_inner_is_null_sentinel (types.c) is the
  one null-sentinel option rule (codegen's is_null_sentinel, option typedef
  collection, type_needs_eq_func, pass2's constant `some`); codegen's
  needs_eq_func applies it under the instance substitution. This fixes `==`
  on `any*?`, and on a generic `'a?` instantiated at a pointer, both of which
  emitted C that failed to compile (tests equality/eq_any_ptr_option,
  equality/eq_generic_option_ptr), and drops a dead `fc_option_void_ptr`
  typedef. emit_type_ident spells only the kinds whose C type is not one
  identifier and defers the rest to emit_type; c_scalar_name is the one
  scalar spelling table. zero_brace_extra derives from zero_agg_depth.
  union_variant_payload is the one variant-payload lookup (pattern hoisting,
  predicates and bindings). Also merged, though not listed above:
  type_byval_aggregate (types.c) is the one by-value containment walk (pass2's
  cycle check, codegen's definition order), and pass1 records a generic
  function on its declaration (Decl.let.is_generic), which codegen reads
  instead of re-deriving the rule. Output byte-identical otherwise.
- B10 done. expr_has_side_effects and sr_flow list every kind and reach the
  children of the kinds they treat alike through the ast.c visitor;
  expr_structurally_equal, a pairwise comparison, lists every kind. Output
  byte-identical. Classifying sr_flow's former default fixed a spec
  violation: a base case reached by a `return` nested in an operand (a let's
  initializer, an assignment, a call argument, an if condition) was missed,
  and the function was rejected as "never returns" (test
  functions/rec_nested_exit_base). ARCHITECTURE.md and CONTRIBUTING.md list
  the three with the exhaustive walkers.
- B11 done.
  - report_let_cycle is the one let-cycle message; a qualified member
    reference (`m.b`) no longer says "through imports" (test
    modules/member_cycle_err; the runner's substring match cannot tell the
    old message from the new, so it passes on the pre-B compiler too).
  - front_end_free (pass2.c) is the one front-end teardown (fcc and
    analysis_free); doc_free and unit_free are the language server's.
  - FC_LEN_REPR_DEFAULT (common.h) is the one `--len-repr` default.
  - pin_fn_param_kind is the one kind pinning for an enclosing function's
    generic parameter (size positions, generic arguments, `'a.property`, a
    const parameter as a value).
  - gen_seen_add builds the generic-validation memo key from
    inst_arg_depth; both entry points call it.
  - set_type_resolved_syms also serves finish_module_members.
  - sym_kind_to_cik is the one completion kind, imports included: a
    module-body import of a function now completes as a Function (test in
    tests/lsp/lsp_test.py, fails on the pre-B compiler).
  - discover_nested_types' struct and union arms: merged in B6.
  - normalize_type_sizes is the one field-size fold; canonicalize_field_stubs
    only renames stubs.
  Output byte-identical.
