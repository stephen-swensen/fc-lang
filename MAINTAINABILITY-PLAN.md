# Maintainability plan

## Goal

If every AI tool disappeared tomorrow, a skilled engineer should be able to
take ownership of this codebase and feel comfortable doing it. Concretely,
they should be able to:

1. find the design written down where they would look for it (not in an
   assistant's instruction file or in memory outside the repo);
2. trust that comments and docs describe the code as it is today;
3. change one piece of logic without a copy of it elsewhere silently going
   stale;
4. rely on the build, not on prose, to enforce the invariants that matter;
5. read any function without scrolling through hundreds of lines of
   unrelated cases.

This is not about hiding AI involvement. The README's disclosure stays.
Items are here because they get in the way of ownership. Many of them are
also recognizable AI habits (history told in comments, code copied and
adapted instead of shared), and fixing them serves both purposes.

The review behind this plan (2026-09-28, at commit 270a131) found the
architecture sound: the Pratt parser with error recovery, the
single-resolution invariant, provenance tracking, and the exhaustiveness
checker are all good work. The problems are duplicated code that has
drifted apart (and is now causing bugs), oversized functions, comments that
narrate history or have gone wrong, and documentation that lives only in
CLAUDE.md.

## Working rules

- Phases are ordered. Tests come first, then structure, then comments. Don't
  rewrite the comments on code that is about to move or merge.
- Every phase ends with `make test-all`, `make test-gcc-len16` and
  `make test-clang-len16` green.
- For pure refactors (phases 2, 4, 5), use the byte-identical C check: emit
  C for the test suite, `demos/` and wolf-fc before and after the change, and
  compare. Any difference must be explained.
- Every bug fix gets a test that fails on the current compiler, and a sweep
  for the same bug in sibling code.
- Line numbers are as of 270a131 and will drift. Function names are the
  stable reference.
- When this plan is finished, move it to `spec/hist/`.

## Status

- **Phases 1 and 2: done (2026-09-28).** `src/ast.c` provides
  `expr_for_each_child`, `expr_any_child` and `pattern_for_each_child`. Every
  whole-tree walker listed in phase 2 now uses them. B1-B10 are fixed and
  tested, and the module-cycle check has moved to the end of pass2.
- **Phase 3: done (2026-09-28).** B11-B20 are fixed and tested, and member
  completion after `->` is gone (it never produced results in practice) along
  with the `>` trigger character. B21 was fixed in phase 4 with the removal
  of `pending_decls` (LSP test: comma-list import diagnostics in source
  order).
  Two more things came out of it:
  - The token name table missed `'...'` as well as `'const'`. It is now a
    switch, so `-Wswitch` catches a token kind with no name.
  - **B27, fixed (option A):** arithmetic on a type variable is typed as the
    type variable, but the same expression with concrete types widened, so
    `x + 300` at `'a = u8` wrapped to 49 where concrete code gives the i32
    305. An instantiation is now rejected when the concrete rule would give an
    operation a different type than the generic body did (spec: Scope of
    operations on type variables, "Result types"). No existing code was
    affected.
  - **B28, fixed:** a generic comparison whose instance needed widening was
    emitted as a bare C comparison (`x > 300` with `x` a `uint8_t`), where
    concrete code casts to the common type first, so clang's
    `-Wtautological-constant-out-of-range-compare` rejected the C. Instances
    now get the casts concrete code gets (`widen_for_instance`).
  - **B29, fixed:** a value whose type involves a type variable, passed to a
    non-generic function, function value or extern, was never checked per
    instance: `u64` reached an `i64` parameter as -1, `1.5` as 1, `true` as
    1. The argument is now checked for each instance by the concrete rule.
  - B27-B29 are one principle, now in the spec (§Errors at monomorphization)
    and in CLAUDE.md's feature checklist: a generic instance means exactly
    what the same code with concrete types means. A sweep of every
    concrete-path widening site found no other place where a generic body
    defers a check.
- **Phase 4: done (2026-09-29).** Every step was checked against byte-identical
  emitted C and diagnostics over every test, demo, wolf-fc and euler-fc; the
  deliberate differences are the bug fixes listed under "Found while doing
  phase 4" (B21, B31, B33-B36), one diagnostic wording fix, one unified
  negative-literal message, and dropping the duplicate generic-instance
  prototypes. Left undone, both optional: routing match arms through
  `emit_block_stmts` with a tail-mode enum, and having `ImportRef` hold a
  `Symbol *`. The pass2 `resolved_member` item waits for phase 5's
  `EXPR_FIELD` split. The seven pass2 instance sites share
  `register_aggregate_instance` but still resolve their type arguments in
  their own ways. New files: `src/facts.c/h`, the facts pass2 judges by and
  codegen emits by (null/zero provability, error constants, interpolation
  format reading).
- **Phase 5: done (2026-09-29).** Pure moves, each checked against
  byte-identical output. The big dispatch switches (`check_expr_inner`,
  `emit_expr`, `parse_prefix`) now hold small cases; their large cases are
  named functions (`check_call`, `check_field` and its three parts,
  `emit_binary` and its helpers, which removed the last `goto`, and so on).
  `codegen_emit` and `pass1_collect` are short drivers over named steps.
  `emit_interp_string` was left as it is (nothing natural fell out).
- **Phase 6: done (2026-09-29).** Every comment in `src/` was rewritten by the
  phase 6 rules, verified comment-only by comparing each file with comments
  stripped against its pre-rewrite copy (the one code change is moving
  `BUILTIN_DOCS` and the hover rule into `src/builtin_docs.inc`). `src/` is
  ASCII apart from that file, and `make check` now enforces it
  (`check-ascii`). Diagnostics are ASCII (three `.error` expectations and one
  spec quote changed). Test headers lost their `§7.8`/`§8.x` labels, and the
  stdlib headers were trimmed. The rewrite found the bugs listed under "Found
  while doing phase 6".
- **Phase 7: done (2026-09-29).** New `docs/ARCHITECTURE.md` and
  `CONTRIBUTING.md` (test markers including `skip_o2`/`only_o2`, the new-kind
  checklist, conventions, and the engineering principles that lived only in
  assistant memory); CLAUDE.md is down to 58 lines of pointers, assistant
  rules and FC syntax pitfalls. Working notes and `fc-vs-zig.md` moved to
  `spec/hist/`; `fc.vim` moved to `editors/vim/`; the three design records
  were curated (`result-type-design.md` 1000 -> 415 lines); `spec/TODO.md`
  keeps only open items (777 -> 278 lines, history in `hist/archived-todos.md`).
  One test runner (`tests/run_tests.sh`, no `.expected` support), one
  `test-all` recipe shared with `test-all-O2`, `fcc --help`, README and editor
  docs fixed. The 33 `bugsearch` test headers were rewritten too. Left undone,
  optional: restructuring `tests/lsp/lsp_test.py`.
- Still open: B30 (phase 4), B39 and B40 (phase 6), and the struct-copy
  const question. When they are settled, move this plan to `spec/hist/`.
- Moving the walkers found five more bugs of the same kind, now fixed and
  tested (listed under B22-B26 below).
- **Module cycles: complete rule (decided 2026-09-28).** Every reference
  written anywhere inside a top-level module counts: identifiers, types in
  annotations, fields, payloads, type arguments, casts and literals, and
  imports, including from nested modules. The error names the location of
  each reference in the cycle. The only existing code this rejected was
  wolf-fc (`enemies` <-> `projectiles`, and `enemies` -> `death_cam`). wolf-fc
  was restructured to match: `projectiles` is nested in `enemies`, the dog
  bite moved to `enemies.ai`, and the death cam starts from
  `enemies.ai.start_death_cam`. Its 287 regression tests pass.

## Phase 1: Tests for the confirmed bugs (about 1/2 day)

Write each program below as a test in the matching `tests/cases/` category.
All of them misbehave on the current compiler.

### fcc exits 0 but the generated C does not compile

This is the worst failure mode a compiler can have. Every case is a
hand-written tree walker with `default: break;` that never learned about a
newer AST kind.

B1. A lambda in a tuple literal. `_fn_N` is undeclared, because
`collect_lambdas_expr` (codegen.c) has no `EXPR_TUPLE_LIT` case.

```fc
let main = (args: str[]) ->
    let t = { (x: i32) -> x + 1, 2 }
    let f = t[0]
    assert(f(41) == 42)
    0
```

B2. A lambda inside string interpolation. Same walker, no
`EXPR_INTERP_STRING` case.

```fc
let main = (args: str[]) ->
    let s = "v=%d{((x: i32) -> x * 2)(21)}"
    assert(s.len == 4)
    0
```

B3. An option type that appears only inside a tuple literal.
`fc_option_uint16_t` is unknown, because `collect_types_expr` has no
`EXPR_TUPLE_LIT` case.

```fc
let main = (args: str[]) ->
    let t = { some(3u16)!, 2 }
    assert(t[0] == 3u16)
    0
```

B4. The same through a destructuring `let`. `collect_types_expr` has no
`EXPR_LET_DESTRUCT` case.

```fc
let pair = (x: u16) -> { x, 2 }
let main = (args: str[]) ->
    let { a, b } = pair(some(3u16)!)
    assert(a == 3u16)
    assert(b == 2)
    0
```

B5. A function reference in a tuple literal. `fc_ctramp_fc__g` is
undeclared, because `collect_trampolines_expr` has no `EXPR_TUPLE_LIT` case.

```fc
let g = (x: i32) -> x + 1
let main = (args: str[]) ->
    let t = { &g, 1 }
    assert(t[1] == 1)
    0
```

B6. A generic call in a `for` range end. The instance is never discovered,
so C sees an implicit declaration. Cause: `discover_in_expr` (monomorph.c)
does not walk the range end.

```fc
let count = (x: 'a) -> 3
let f = (x: 'a) ->
    let mut s = 0
    for i in 0..count(x) do
        s = s + i
    s
let main = (args: str[]) ->
    assert(f(1u8) == 3)
    0
```

B7. A generic call in a match `when` guard. Same walker, which does not walk
the guard.

```fc
let pos = (x: 'a) -> true
let f = (x: 'a, o: i32?) ->
    match o with
    | some(v) when pos(x) -> v
    | _ -> 0
let main = (args: str[]) ->
    assert(f(1u8, some(4)) == 4)
    0
```

B8. A slice literal inside a tuple literal inside a loop falls back to
`__builtin_alloca` on every iteration, so the stack grows with the loop. It
misses the function-entry backing array that other slice literals get. Test
it by inspecting the generated C, or with a loop long enough to overflow the
stack.

```fc
let main = (args: str[]) ->
    for i in 0..3 do
        let t = { i32[2]{ i, i }, 3 }
        assert(t[0][1] == i)
    0
```

### Wrong diagnostics

B9. A false "circular reference between modules 'b' and 'a'". The module
cycle check (pass1 phase 4) compares raw name strings, so a parameter named
`b` looks like a reference to module `b`. This is exactly the shadowing bug
the single-resolution invariant exists to prevent.

```fc
module a =
    let f = (b: i32) -> b + 1
module b =
    let h = (x: i32) -> a.f(x)
let main = (args: str[]) ->
    assert(b.h(1) == 2)
    0
```

B10. A real module cycle goes undetected when it passes through a tuple
literal (spec: "Circular references between modules are not allowed"). This
should be an error test.

```fc
module a =
    let f = (x: i32) -> { b.g(x), 1 }
module b =
    let g = (x: i32) -> x + 1
    let h = (x: i32) -> a.f(x)
let main = (args: str[]) ->
    0
```

B11. A spurious "redefinition of 'point'" when a companion module comes
before its struct inside a module. At top level either order works. The
struct arm in `register_module_members` (pass1.c) looks the name up without
checking kind.

```fc
module geo =
    module point =
        let origin_x = 0
    struct point =
        x: i32
let main = (args: str[]) ->
    let p = geo.point { x = geo.point.origin_x }
    p.x
```

B12. Generic ordering on pointers is rejected: "ordering comparison requires
numeric or pointer types, got i32* and i32*". The same comparison written
directly compiles. `validate_generic_body` (pass2.c) re-implements the
`EXPR_BINARY` ordering rule and leaves out pointers. The spec and
`spec/generics-constraint-model.md` both allow pointers.

```fc
let less = (a: 'a, b: 'a) -> a < b
let main = (args: str[]) ->
    let xs = i32[2] { 1, 2 }
    assert(less(&xs[0], &xs[1]))
    0
```

B13. A file-level `let` may contain a function call if the call returns a
union. The rule is "no function calls". `is_file_init_expr` recognizes
variant constructors by "the result is a union and the callee is a field",
while its twin `is_const_expr_ex` uses `is_variant_constructor`. Nothing is
miscompiled (the program gets a runtime init), but the rule is not enforced.

```fc
union shape =
    | circle(i32)
    | empty
module m =
    let make = (r: i32) -> shape.circle(r)
let g = m.make(3)
let main = (args: str[]) ->
    match g with
    | circle(r) -> r
    | empty -> 0
```

B14. The parser still has 17 `diag_fatal` calls (parser.c, e.g. the `none`
without a type argument, a tuple literal with fewer than two elements, and
the `alloc` forms). Any of them ends the whole parse, so later syntax errors
are never reported and the LSP loses the analysis while you type. This
breaks the recovery contract stated in parser.c and CLAUDE.md. Test: two
independent syntax errors in one file, where the first is one of these
sites; both must be reported.

B15. A stray `const` in an expression reports "unexpected token unknown",
because `TOK_CONST` is missing from the names table in token.c.

B16. A trailing comma is accepted in `i32[3] {1,2,3,}` but rejected in
`alloc(i32[3] {1,2,3,})`. The two element loops have drifted (parser.c,
around 1675 and 2648). Decide which behavior is correct and test both
forms.

### Runtime and LSP

B17. Overflow when assigning to a fixed-array field calls plain `abort()`
(codegen.c, around 5725), so `--backtraces` prints no backtrace. The
struct-literal version of the same check uses `FC_ABORT()`.

B18. Go to Definition on a field of a generic struct (`b.v` where `b` is a
`box<i32>`) returns null. `type_substitute` and `type_deep_copy` (types.c)
copy each field's name and type but drop its `loc`. Add a test in
`tests/lsp/`.

B19. Go to Definition builds its URI with a bare `"file://%s"` (lsp.c,
`handle_definition`), while diagnostics use `path_to_uri(canon_path(...))`.
Paths with spaces or `..` segments probably open a second, misspelled
editor tab. Add a test in `tests/lsp/` with a space in the path.

B20. Three fixed 64-entry arrays in codegen.c silently drop entries past 64:
`from_libs[64]` and `defines[64]` in `codegen_emit`, and `deps[64]` around
line 7457. Test with 65 extern libraries if that is practical; otherwise
just fix it.

B21. `import a, b, c from m` produces declarations in the order a, c, b,
because `pending_decls` is drained from the back (parser.c, the module-body
and top-level decl loops). No visible effect is known beyond diagnostic
order. It is fixed as part of phase 4 (removing `pending_decls`).

### Found while doing phase 2 (all fixed)

Each of these was a hand-written walker that skipped a child the visitor now
covers.

- B22. An `unguarded` or `checked` marker whose only governed operation was
  in a match arm's `when` guard was rejected as redundant.
  (`unguarded/match_guard_index`, `checked/match_guard_overflow`)
- B23. A `defer` whose expression used `?` inside a match guard was accepted,
  and the compiler then segfaulted. (`defer/propagate_in_match_guard_err`)
- B24. An operation on type variables in a match guard was never checked per
  instance, so fcc exited 0 and the C failed to compile.
  (`generics/match_guard_op_err`)
- B25. A recursive function whose self-call sat inside a tuple or struct
  literal was reported as "never returns". The wrong branch-ordering decision
  came from `expr_refs_self` not searching literals.
  (`functions/rec_order_literal`)
- B26. A slice literal inside a tuple literal in a module constant was
  emitted with a null pointer, and reading it segfaulted.
  (`tuples/module_const_slice_elem`)

### Found while doing phase 4

- B30 (open, not fixed). A misplaced comma-list import reports "imports must
  appear at the top" once per name: `import a, b, c from m` after another
  declaration gives three identical errors. Each name is its own `DECL_IMPORT`
  and both placement checks in pass1 run per declaration. A fix would report
  once per statement (consecutive imports sharing a location are one
  statement).
- B31 (fixed). A slice literal whose element type has a parenthesized const
  argument (`wide<(256 >> 1)>[2] {}`, the form a shift needs) did not parse:
  `scan_type_head` carried its own copy of the type-argument scan, without
  `typearg_scan`'s parenthesis handling. It now calls `typearg_scan`.
  (`generics/const_arg_slice_lit_paren`)
- B32 (fixed). A radix prefix with no digits was accepted as an integer
  literal: `0x`, `0b`, `0o` and `0xu8` compiled as 0. The sibling `0x_ff` was
  accepted too, though the spec allows `_` only between digits. Each prefix
  now requires a digit of its base, and `scan_digits` accepts `_` only after a
  digit. Spec: Literals. (`expressions/radix_prefix_*`,
  `expressions/digit_separator_after_prefix_err`,
  `expressions/radix_literal_forms`)
- B33 (fixed; the module-side twin of B11). Inside a module, a second nested
  module sharing its name with a companion type was accepted silently, and its
  members vanished: the duplicate check looked only at the first symbol of
  that name, which was the type. The top level reported it. Nested module and
  type registration now use the mirrored rules `module_name_taken` /
  `type_name_taken`. (`modules/companion_duplicate_module_in_module_err`,
  `modules/companion_duplicate_module_after_enum_err`)
- B34 (fixed; the plan's `mono_instantiate` item). A generic struct literal
  registered the instance made of the body's type variables in order, not the
  instance its type names. `holder { w = dbl(a) }` has type `holder<'n * 2>`,
  but at `'n = 5` monomorphization registered `holder<5>`. A correct program
  was rejected when that phantom instance failed a `static_assert`, and in
  general an unused instance was emitted (`box<i32>` beside `box<i32?>`). The
  literal now goes through `discover_in_type`, like every other type operand.
  (`generics/struct_lit_instance_from_type`,
  `generics/struct_lit_instance_assert_err`)
- B35 (fixed; the `unsigned_counterpart` item). An interpolated `isize` or
  `usize` under an unsigned conversion (`%x`, `%u`) was reinterpreted through
  `uint64_t` rather than at its own width, so on a 32-bit target a negative
  `isize` printed 16 hex digits instead of 8. Invisible on a 64-bit host, so
  there is no host test; the emitted C now casts through `size_t`.
- B36 (fixed; the "stdin flags" item). A binding named `stdin`, `stdout` or
  `stderr` of type `any*` was miscompiled: codegen recognized the C stream by
  name and type, so `let stdout = (any*) 0usize; let p = stdout` gave `p` the
  C stream. pass2 now marks the identifier that resolved to the built-in
  (`ident.is_std_stream`) and codegen reads that. (`bindings/shadow_std_stream`)
- Diagnostic: "type variable ''a' cannot be used as a value" quoted a name that
  already starts with `'`. It now reads "type variable 'a ...", like the other
  type-variable messages.

### Found while doing phase 6

The comment rewrite checked every comment against the code, which turned up
these. B37, B38, B41 and B42 are fixed; B39 and B40 are open.

- B37 (fixed). `~` was rejected in a top-level initializer where `-` and `!`
  are accepted: `module m = let a = ~5isize` reported "must be a constant
  expression". `is_init_expr` now allows it, as the spec already said.
  (`const_eval/unary_ops_unfoldable`)
- B38 (fixed). fcc exited 0 with C that didn't compile when a module
  constant or `let mut` global gave a fixed-array field a longer slice
  literal, at any nesting depth. Siblings on the same path: a raw-parts slice
  and `default(T[])` as the field value were emitted as slice headers. A static
  initializer has no run time to abort in, so `const_fold_expr` now checks the
  length at compile time and requires a slice literal (or the empty slice,
  which codegen emits as zeros). File-level and function-body copies still
  abort at run time. Spec: Fixed-Size Inline Arrays, Struct Literals.
  (`structs/fixed_array_module_const*`, `structs/fixed_array_module_mut_overflow_err`,
  `structs/fixed_array_file_level_overflow`, `extern/extern_struct_fixed_array_const*`)
- B39. Completion never offers `static_assert`: lsp.c keeps its own copy of
  the keyword list, which has drifted from the lexer's. The fix is to have
  the lexer export its keyword table and completion read it, not to add the
  one name.
- B40 (minor). Go-to-definition into another file uses the byte column as the
  UTF-16 column, so the range is off on lines with non-ASCII text before the
  name. Same-file definitions convert correctly.
- B41 (fixed, found while testing B38). A body type-checked before a top-level
  generic struct's or union's field types were canonicalized (any module
  member, or a top-level function above the declaration) instantiated it from
  the raw stub name: `outer<'a>` with `b: box<'a>` emitted an unrooted
  `box__3_i32` beside `fc__box__3_i32`, and the C failed to compile. (On
  `main` these programs were rejected with a false "infinite generic
  instantiation" error; the phase 4 commit turned that into broken C.) Top-level
  field types are now canonicalized in pass 0, before any body; the one oracle
  change removed an unused unrooted struct `generics/linked_list` had been
  emitting all along. (`generics/instance_field_generic_module`,
  `generics/instance_field_generic_before_decl`)
- B42 (fixed). A read-only slice could not be copied into a fixed-array
  field (`box { data = "ab" }` was `str` vs `const str`), though the copy only
  reads it. The rule now is the element as the source hands it out must
  convert to the field's element type without changing representation
  (`fixed_array_copy_ok`); module constants also accept a string literal. The
  same "element as read" rule was missing in three siblings, each a const
  hole: a generic fixed-array field bound `'a = str` from a `const str[]`, a
  `for` loop over a read-only slice of strings bound a writable `str`, and
  `alloc` of a `const str[]` returned a `str[]`. All now use
  `type_slice_elem_read`, as indexing did. Spec: Fixed-Size Inline Arrays
  (Assignment), Deep const, and the `alloc` note. (`structs/fixed_array_const_source*`,
  `structs/fixed_array_module_const_string_overflow_err`,
  `generics/fixed_array_generic_const_source_err`,
  `control_flow/for_*const_slice*`, `memory/alloc_const_slice_*`)
- Open design question (not a bug fix): const is not carried through a
  struct copied out of a read-only slice. `let q = ro[0]` with
  `ro: const p[]` and `p` holding a `s: str` gives a writable `q.s`, as in
  C's shallow const, while `ro[0].s[0] = x` is rejected. The spec says an
  element "cannot be laundered into a writable view"; whether that should
  reach reference fields of a copied struct is for the user to decide.
- Dead code (removed): the "update imported symbols' types" loop in
  `pass2_check` could never match, since module lets are only in member
  tables, never in the global table it searched. An instrumented build ran it
  on 830 corpus inputs without a match, and removing it left the emitted C
  and diagnostics unchanged.

## Phase 2: One shared expression visitor (about 2-3 days)

This is the fix for B1-B8 and B10. It is the most valuable single change in
this plan.

There are about 15 hand-written whole-tree walkers: 7 in codegen.c,
`discover_in_expr` in monomorph.c, the module-cycle walker in pass1.c, 5 in
pass2.c (`pretaint_walk`, `validate_generic_body`, `ccf_walk`,
`subtree_has_governed_effect`, `expr_refs_self`), and 3 in lsp.c
(`find_in_expr`, `lens_expr`, `harvest_expr`). Each one re-lists the
children of every expression kind and ends in `default: break;`. When a new
kind is added, every walker that forgets it skips that subtree without any
error.

- Add `src/ast.c` with `expr_for_each_child(Expr *e, fn, void *ctx)` (and a
  pattern equivalent if the walkers need it). Its switch lists every
  `ExprKind` and has **no `default`**, so `-Wswitch` (already on through
  `-Wall`) reports any kind that is not handled.
- Move each walker onto it. A walker keeps a switch only for the kinds it
  treats specially and delegates everything else to the visitor.
- Move the module-cycle check to after pass2, working from `resolved_sym`
  and `resolved_member` instead of name strings (B9, B10). The spec says
  "The compiler detects cycles during the first pass" (fc-spec.html, around
  line 4663). Reword it to state the rule without the implementation
  strategy.
- Bring `lens_expr` and `harvest_expr` in lsp.c up to full coverage. Today a
  lambda inside a struct literal gets no inlay hint, and completion does not
  offer `for` variables or pattern bindings, although `complete_scope`'s
  comment promises them.
- Check: B1-B8 and B10 pass, and the emitted C is byte-identical on
  everything else.

## Phase 3: The remaining bug fixes (about 2 days)

- B14: change the 17 parser `diag_fatal` calls to `diag_error` and return an
  error node. Then correct the claims in CLAUDE.md, `analyze.c`,
  `analyze.h` and `lsp.c` that only the lexer can abort.
- B15: add `TOK_CONST` to `token_names`.
- B11: this falls out of `register_type_decl` in phase 4. Do it here if
  phase 4 is delayed.
- B12: share the binary-operator typing rules between `EXPR_BINARY` and
  `validate_generic_body` instead of patching the copy.
- B13: merge `is_file_init_expr` into the const-expr walker as a mode.
- B16: share the element-list loop.
- B17: use `FC_ABORT()`.
- B18: copy the whole field or variant struct, then overwrite its `type`.
- B19: use `path_to_uri(canon_path(...))`.
- B20: replace the arrays with `DA_APPEND`.
- **Decision needed:** completion after `p->` still offers struct fields
  (lsp.c `complete_members`, its `arrow` parameter, and the `>` trigger
  character in `editors/vscode/extension.js`). `->` is not member access in
  FC. Proposal: drop the member lookup and the trigger, but keep detecting
  `->` so it still suppresses the bare-name list.

## Phase 4: Merge the duplicated code (about 4-5 days)

Each item below is logic that exists in several copies, several of which
have already drifted. Merge them into one function. Risk is noted where it
is above low.

### common.c

- Add `str_dup`, `str_ndup` and `read_file`, and delete the local copies:
  `str_dup` in types.c, `dup_cstr` in lsp.c, `dupn` in args.c, about five
  inline malloc+memcpy copies, and three file readers (`read_file` in
  main.c, `read_file_or_null` in args.c, `read_whole_file` in lsp.c).
- Replace every `snprintf(NULL, 0, ...)` + malloc pair with
  `arena_sprintf`, `str_sprintf` or `intern_sprintf`. These include
  `make_local_name` in pass2.c, six sites in pass1.c, `intern_fmt2` and
  `lambda_c_name` in codegen.c, and the `buf[64]` + memcpy name builders in
  pass2.c.
- Delete `#define msgf str_sprintf` in args.c.

### Pipeline (main.c, analyze.c)

- main.c and analyze.c each run their own copy of the front end (lex all,
  collect generic names, parse, merge programs, pass1, pass2) and of the
  teardown. Extract a shared `merge_programs` and a shared teardown.
- `lex_feed_cached` repeats `lex_one`'s lexer setup. Split out a common
  core.
- `analyze_unit` writes the global `g_len_repr`. Pass `len_repr` into
  `analyze()` so the save and restore happen in one place.

### parser.c and lexer.c

- Replace the `pending_decls` side channel with `parse_import_decl`
  appending to the caller's list. This fixes B21 and removes the "every decl
  loop must drain the queue" coupling.
- Shared helpers for the repeated loops:
  - `parse_type_arg_list`: 4 copies.
  - `parse_call_args`: 3 copies.
  - "wrap in `EXPR_BLOCK` if there is more than one statement": 7 copies.
  - `arena_dup` for the memcpy+free pattern: about 25 copies.
  - `at_struct_lit_brace`: 3 copies.
  - the `( ... ) [ ... ] {` lookahead: 3 copies. `scan_type_head` also
    re-implements `typearg_scan` without its paren handling.
  - `str { ptr, len }` vs `T[] { ptr, len }`.
  - source-text capture: 2 copies.
  - or-pattern flattening: 4 copies.
  - import decl construction: 4 copies.
  - the struct field-line loop: 2 copies.
  - the lexer's integer-suffix scan: 4 copies.
- Delete `decl_error_node` (it duplicates `alloc_decl_error`, minus the
  filename). Merge `parse_int_type` with `parse_int_type_in`.
- Replace `is_type_name` with `type_from_name(...) != NULL`. Replace
  `parse_char_value`'s escape handling with `decode_str_lit`. Use
  `type_type_var()` and `type_uint8()` instead of hand-built types.
- Use `tok_loc()` at the 29 sites that set `.filename = p->filename` by
  hand.
- Save and restore `allow_fixed_array` the way `in_const_expr` and
  `block_arm_arrow` already are.
- Delete: the `generic_gate` flag (always true), `if (!m) break;` after
  `expect` (which never returns NULL), the unreachable `case '\''` in
  lexer.c, and memsets after `arena_alloc` (which already zero-fills).

### pass1.c, monomorph.c, types.c

- Add one `register_type_decl(tab, d, mangle_prefix, qualified, ns)` to
  replace the nine copies of "allocate the Type, add the symbol, add the
  mangled twin" (struct, union and enum, each in `register_*_sym`, in the
  module arms, and in the namespaced phase-2 arms). Its redefinition check
  must look at the symbol's kind (B11). Make `symtab_add` return a
  `Symbol *`, which removes the 20 uses of `&tab->symbols[tab->count - 1]`.
- Merge the module-registration block that appears twice in
  `register_module_members`.
- Add `mono_instantiate(sym, args, n, kind)` to replace the five copies of
  "register, substitute, deep-copy, rename, resolve" in monomorph.c. Check
  whether the struct-literal arm becomes `discover_in_type(e->type)`; that
  would also stop it emitting an unused `box<i32>` next to `box<i32?>`.
  Medium risk.
- Merge `type_eq` and `type_eq_ignore_const`, which are about 50 lines
  each.
- Merge the four const-expression variable collectors
  (`const_expr_collect_vars`, `ck_collect_expr`, `kind_walk_expr`,
  `type_collect_vars`/`ck_collect`).
- Move `gen_inst_type_depth` and `fmt_type_inst` from pass2.c into types.c.
  monomorph.c has identical copies.
- Replace `mangle_cat` with `str_appendf`.
- Delete:
  - `pass1.c:2005-2025`, which can never run;
  - `TYPE_CHAR`, which is never constructed but has about 8 `case` labels;
  - the unused `intern` parameter of `process_member_import`;
  - the `(void)a` in `mangle_generic_name` and `tuple_canonical_name`.
- Optional: `ImportRef` copies five fields of generic metadata from
  `Symbol`; it could hold a `Symbol *`. This reaches into pass2.

### pass2.c

- `EXPR_IDENT` binds symbols in four near-identical blocks (module members,
  imports, parents, globals), each with its own on-demand cycle check, and
  they have drifted. Replace them with `bind_resolved_symbol()` plus
  `ensure_let_checked()` on top of `resolve_symbol`. Medium risk; the
  module tests cover it.
- Add `instantiate_generic_aggregate(ctx, sym, raw_args, n, loc)` to
  replace about seven copies, which now disagree on how they resolve type
  arguments and on how they set `mi->concrete_type`. Standardize on the
  deep-copy version. Medium risk, because codegen reads `concrete_type`.
- Three places re-walk a module path by name (the `EXPR_FIELD` chain
  handling and the union re-resolution by type name). The object's
  `resolved_member` is already set on the node. Use it; a companion pair
  needs an `EXPR_FIELD` equivalent of `companion_module`. Medium risk. Do it
  after `EXPR_FIELD` is extracted in phase 5.
- The const-expression grammar is written three times
  (`normalize_size_expr`, `normalize_const_expr`, `sa_shape_check`). Merge
  them. Medium risk; this touches const generics.
- `const_fold_expr` rebuilds children in four hand-written loops. Replace
  them with `fold_children`. Merge the two copies of the
  VISITING/DONE/FAILED state machine.
- `is_const_expr_ex` / `is_const_expr`: use one function with a nullable
  out-parameter.
- `scope_add` / `scope_add_prov` / `scope_add_prov2`: use one function.
  The five scope-chain lookups should use `scope_find_binding`; they
  currently disagree on where to stop at `is_global`.
- One `lookup_type_symbol` for the struct/union/enum lookup cascade, which
  appears 7 times.
- `resolve_generic_types_in_ret` re-implements `resolve_type`'s recursion.
- The negative-literal fold spells out five per-type messages, although
  `check_int_literal_range` already takes a `negative` argument.
- Smaller helpers:
  - `file_imports_for()`: 5 copies of the lookup loop.
  - a submodule enter/leave helper built on `SavedCtxScope`.
  - one shared "return of stack-allocated value" diagnostic.
  - `gen_inst_diag` should accept a NULL frame, so that
    `check_inst_sizes_frame` does not write every message twice.
- Group the seven `pending_*` one-shot fields of `CheckCtx` into one struct,
  so `EXPR_LET` and `check_decl_let` each save and restore them with a
  single assignment. Move `g_gen_xbody_depth`, `g_gen_inf_reported` and
  `g_gen_seen*` into a struct passed to `validate_generic_body`.
- Optional but cheap: `return poison(e);` in place of the 189 copies of
  `e->type = type_error(); return e->type;`.

### codegen.c

- One `emit_fn_signature()` in place of 7 copies. Delete the second
  prototype loop, which emits generic-instance prototypes twice.
- The generic-instance struct, union and tag-enum emission re-implements
  `emit_struct_def`, `emit_union_def` and `emit_union_tag_enum`. Call those
  instead.
- `aggregate_c_name(t)` for the choice between `mangle_generic_with_subst`
  and `generic_instance_c_name`, which is repeated at 7 sites.
- Use `subst_resolve` at the 4 places that inline it. Use `type_is_subint`
  and `unsigned_counterpart` where they are re-implemented inline.
- Duplicated emission code:
  - `alloc(struct_lit)` and `alloc(union)` are byte-identical: merge them.
  - the evaluation-order temp setup: 3 copies.
  - the three option-unwrap branches.
  - "put the value in a temp, run defers, return it": 5 copies.
  - `emit_loc_args()` for the file/line fallback and escaping: 17 copies.
  - enum sign-extension: 2 copies.
- Add `enter_instance()` and `leave_instance()` for the nine hand-written
  set/clear pairs on `g_subst` and `g_lambda_suffix`. Make `g_int_lit_hex`
  a parameter instead of a global. Give `indent_level` and `temp_counter`
  the `g_` prefix the other globals have.
- Unknown-kind fallbacks currently emit a C comment and carry on, which
  risks exit 0 with bad output. Replace them with one `internal_error()`
  that calls `diag_error`; main.c already checks the error count after
  codegen.
- Replace `strcmp(name, "stdin")` (2 sites) and `strstr(codegen_name,
  "NAN")` / `"FLT_"` (the header selection) with flags set in pass2.
- Delete the synthetic `main`, which is unreachable because the CLI
  requires `main`. Delete the empty branches in `emit_func_decl`. Replace
  `fmt_lambda_display`, which ignores its parameters, with the literal it
  returns. Rename `emit_interp_string_impl` (it has no non-`_impl` sibling).
- Rename the shadowed `is_last` in the match-arm emission. It has two
  different meanings in one function.
- Layering: pass2 calls analysis predicates that are declared in ast.h but
  defined in codegen.c (`ptr_value_provably_*`, `int_value_provably_*`,
  `error_const_literal`, `interp_seg_spec`, `interp_seg_trunc_prec`,
  `interp_is_runtime_sized`). Move them to their own `.c/.h`.
- Medium risk, and optional: match arms emit their own statements instead
  of using `emit_block_stmts`, and a comment records that this duplication
  already caused one bug. Replace `emit_block_stmts`'s two bools with a
  tail-mode enum (value / discard / return / assign) so match arms can use
  it. Watch the defer interaction.

### lsp.c

- Make `consider()` return whether it won. That removes the 8 "did the
  winner land here" re-checks. Make `consider_builtin` call `consider()`
  instead of being a hand copy, which has drifted: it does not clear
  `type_ref_sym`, `companion` or `decl_site`.
- One predicate for "is a struct, union, enum (or module)", which is tested
  in about 10 places. One `decl_in_file()`; today a NULL filename passes
  the check at some sites and fails it at others.
- Split `analyze_unit` into its three phases, and merge the
  "open buffer else disk" block, which is copied verbatim. Merge the two
  "find primary doc for key" loops in `flush_dirty`.
- In `complete_members`, the enum-variants-plus-`count` block appears
  twice; extract it. In `handle_hover`, the doc-comment read at a site
  appears twice; extract it.
- Use `hex_digit_val` (common.c) instead of `hexval`, `path_dir` (args.c)
  instead of three `strrchr` + `%.*s` re-derivations, and `id_char`
  instead of the 7 inline copies.
- `lens_emit` builds a fake `FindCtx` only to call `let_name_col`. Make
  that helper take `(idx, src)`.
- Delete `LspDoc.version` and `LspServer.initialized` (written, never
  read), the unreachable `continue` in `flush_dirty`, and the `if (p)
  free(p)` guards.
- `platform_detect_flags` repeats the three `#if` ladders of the
  `platform_get_*` functions. Write it in terms of them.
- Use one include-guard style across the headers (`#pragma once` is the
  majority).

## Phase 5: Split the oversized functions (about 3 days)

These are pure moves along seams that already exist. A big switch over AST
kinds is normal in a compiler; case bodies of 300 to 600 lines are not. The
goal is a dispatch switch whose large cases are named functions.

| Function | Lines | Extract |
|---|---|---|
| `check_expr_inner` (pass2.c) | 4,465 | `EXPR_FIELD` (about 600: module member / type-name member / value field), `EXPR_CALL` (about 490: variant constructor / generic / plain), `EXPR_FUNC` (about 360: return derivation, `?` propagation, return validation), `EXPR_IDENT` (about 350; small after phase 4), `EXPR_ALLOC` (about 340: alloca / `alloc(T)` / `alloc(expr)`), and the address-of branch (about 120) |
| `emit_expr` (codegen.c) | 2,710 | `emit_binary` (about 280; with helpers for integer arithmetic, shifts and div/mod, which removes the `goto _binary_done`), `emit_alloc` (275), `emit_match` (211), `emit_struct_lit` (171), `emit_call` (142), `emit_field` (131), `emit_unwrap` (122), `emit_cast` (102) |
| `codegen_emit` (codegen.c) | 1,142 | One function per existing comment banner: preamble, runtime helpers, type collection, type declarations, globals, functions, backtrace table |
| `parse_prefix` (parser.c) | 1,093 | `parse_alloc` (160), paren prefix plus cast (155), `parse_ident_prefix` (92), `parse_interp_string` (68), float literal (58), `parse_for_expr` (58), tuple literal (54), `parse_assert` (36); also split `parse_infix`'s 165-line `TOK_LT` case into `parse_generic_suffix` |
| `pass1_collect` (pass1.c) | 685 | A short driver calling named functions, replacing the phase numbers 0, 0.5, 0.6, 1, 2, 3a, 3b, 4, "Final" and "Last" |
| `register_module_members` (pass1.c) | 330 | Small after `register_type_decl` |
| `emit_interp_string_impl` (codegen.c) | 328 | Only if something natural falls out; otherwise leave it |

Also in `validate_generic_body`: extract the call branch (about 90 lines).
In lsp.c, extract `find_in_expr`'s 70-line `EXPR_FIELD` case.

## Phase 6: Comments and diagnostics (about 3-4 days)

Do this one file at a time, after that file's phase 4 and 5 work. Fix the
comments that are wrong first; a wrong comment is worse than none.

### Comments that are wrong today (fix first)

- analyze.c, analyze.h and CLAUDE.md say only the lexer can abort the
  analysis. The parser can too (B14). This stays true until phase 3.
- `diag.c:37` says the message goes into a fixed buffer. It uses
  `str_vsprintf`.
- lsp.c describes pass2 as gated on pass1 ("=> pass2 never runs",
  "pass1-gated pass2"). That gate no longer exists.
- lsp.c says a lone `>` falls through to ordinary completion. It returns
  empty.
- About eight comments across ast.h, parser.c, parser.h and lsp.c still
  describe `->` as pointer field access (for example, the `EXPR_DEREF_FIELD`
  comment in ast.h reads `x->f`).
- codegen.c says lambdas display as `<lambda at f.fc:N>`.
  `fmt_lambda_display` returns `"<lambda>"`.
- codegen.c calls the `alloca((cstr) s)` path "retained only as a fallback
  during staging". It is the live path for that form.
- `lexer.c:948` "(live until line ~1052)" is stale. `lexer.c:912` says
  "Find the last #if", but the code uses the last token.
- parser.c comments cite "the grammar", meaning the deleted `grammar.bnf`.
- pass2.c cites "§Dynamic stack allocation" and "§Slice construction". The
  real spec headings differ.
- Orphaned comments, left behind when new code was inserted between a
  comment and its function:
  - pass2.c: 1177, 1552 and 2808 have no function below them; the comments
    at 1370, 4268 and 4493 document functions further down.
  - pass1.c:992 sits above `make_qualified`, not `register_module_members`.
  - codegen.c:744, `lsp.c:2720`, and `analyze.c:195` (a section header with
    no code under it).

### Rules for the rewrite

1. **Present tense.** Describe what the code does and why. Delete history:
   "used to", "no longer", "previously", "now", "was fixed", "crashed the
   compiler", "fcc exit 0". That belongs in commit messages.
2. **No references a reader can't follow.** Drop "audit item 14", "item 7",
   "§7.8", "Item 2", dates, and names of demos or incidents. Keep the
   reason, not the tag. Spec references are fine when they use a real
   heading.
3. **Keep the invariant, cut the argument.** A 20-line block usually hides
   a 3-line invariant. Keep the invariant, and add an example only when it
   is what makes the invariant clear. The good "why" comments stay, trimmed:
   union-tag injectivity, east-const, the div/mod overflow-versus-guard
   decomposition, the provenance and address-of tables, the error-recovery
   contract, `mangle_type_name` injectivity, and the pre-taint loop example.
4. **No promises the code doesn't keep.** About ten comments say "so the
   two can never drift" next to two copies that did drift. After phase 4
   either the copies are gone or the promise is false; delete it either way.
   Something that must stay in sync should be enforced by the compiler or a
   test, not by a comment.
5. **Plain ASCII.** No em-dashes, arrows, `≡`, ellipsis characters,
   superscripts, `*emphasis*`, or CAPITALS for emphasis ("NON-EMPTY",
   "NOT", "ONCE"). Drop the tic words: "belt-and-braces" or "suspenders",
   "load-bearing", "crucially", "deliberately", "exactly", "by
   construction", "defense in depth". The one exception is `BUILTIN_DOCS`
   in lsp.c, which is user-facing markdown.
6. **Don't restate the code.** One-line signposts ("check arg count") are
   fine in long functions. Paragraphs explaining a one-line wrapper are not.
7. Rename identifiers that encode tracker labels: the `a5_*` functions in
   pass2.c are named after audit item A5.

Scale: about 1,100 lines in `src/` contain non-ASCII characters, about 440
comment blocks run 5 lines or more, and 89 run 10 or more. Rewrite by hand
while doing the trim. A blanket sed replaces em-dashes with worse
punctuation and leaves the prose as it was.

### Diagnostics

- About 50 user-facing diagnostic strings (mostly in pass2.c) contain
  em-dashes. Make them ASCII; about 3 `.error` test expectations change.
- Add a `make check` step that fails on non-ASCII bytes in `src/*.c` and
  `src/*.h`, with an exemption for `BUILTIN_DOCS` if it keeps its
  typography. Then rule 5 is enforced by the build instead of by memory.

### Tests and stdlib

- About 30 test files begin with `§7.8` or `§8.x` labels that point into
  `spec/hist/bugs-2026-07-21.md`. Replace the labels with a plain sentence
  saying what the test covers. Keep the "Regression: X used to crash"
  headers; that is normal in compiler test suites.
- stdlib: `net.fc` and `io.fc` headers list every function by name, and
  those lists will drift, so cut them to one sentence each. `data.fc` gives
  the reference-semantics argument twice; keep one. In `io.fc`, cut the
  "because" clauses on parameter docs. Otherwise stdlib is good as it is.

## Phase 7: Put the knowledge where a human will look (about 2 days)

Today the architecture documentation exists only in CLAUDE.md, a 57 KB file
written for an AI assistant. It reads partly as a changelog ("was
~13KB/keystroke", "measured on wolf-fc", "before pass2 was ungated"),
names functions that no longer exist (`analyze_doc`, `doc_free_results`,
`publish_diagnostics`, `LspDoc.last_good`), and is wrong about the parser.
`spec/TODO.md` says "Architecture lives in CLAUDE.md". Engineering values
and design rationale also live in the assistant's memory directory, outside
the repository, and would disappear with it.

- **Write `docs/ARCHITECTURE.md`** (about 300-400 lines) at the level of
  modules, not functions:
  - the pipeline, and what each source file is responsible for;
  - an **Invariants** section: C name spaces (`fc__`, `fc_<kind>_`,
    `_l_`), exact-size name building (no fixed buffers for names), the
    `--len-repr` construction invariant, the single-resolution invariant,
    one diagnostic severity plus recovering parsing, and the generated-C
    conventions (overflow via unsigned casts, shift masking, bounds and
    unwrap checks);
  - the LSP design: units, `lsp.rsp`, the stdlib feed, project-wide
    diagnostics, and stale-result retention. One or two paragraphs each,
    with no timings or leak history.

  Name functions only where they are the entry point to a subsystem, since
  function-level detail is what drifted in CLAUDE.md.
- **Write `CONTRIBUTING.md`:**
  - build and test targets, including `FILTER`, O2 and len16;
  - the test layout and **every** marker (`.error`, `.expected_exit`,
    `deps`, `flags`, `fcc_args`, `expected_stderr_contains`,
    `skip_windows`, `skip_o2`, `only_o2`). `skip_o2` and `only_o2` are
    documented nowhere today;
  - a checklist for adding a new AST kind or Type kind. After phase 2 most
    of it is "the compiler will tell you";
  - the feature-addition discipline and the test-coverage philosophy from
    CLAUDE.md, rewritten as a checklist without the review anecdotes;
  - the `= {0}` struct-init rule, FC naming conventions, and commit style.
- **Audit the assistant's memory for knowledge the repo lacks.** Most
  language decisions are already in the spec. What is missing is the
  engineering values. Put them in an "Engineering principles" section of
  CONTRIBUTING.md, or in the spec where they are language rules:
  - measure what gcc and clang already do before building an FC-side
    optimization;
  - prefer static costs over runtime machinery;
  - prefer restrictive but explicit over hidden cost;
  - no implicit type rewriting in inference;
  - emitted C must not assume the width of `int` (16-bit-int targets);
  - a bug fix comes with a failing test and a sweep of its siblings, and a
    fix in shared code gets tests of the general behavior.

  Check each against the spec before writing it down.
- **Shrink CLAUDE.md to about 50 lines:** pointers to ARCHITECTURE.md,
  CONTRIBUTING.md, `spec/examples.fc` and the spec; the assistant-only
  rules (hands off git, run `make test-all` before reporting, the spec plus
  the compiler are the only authority); and the short list of FC syntax
  mistakes a model tends to make (no return-type annotations, how match
  arms line up, `->` is never a return type). Its "Key Language Design
  Decisions" section duplicates the spec and can go.
- **Tidy `spec/`:**
  - Move working notes to `spec/hist/`: `readonly-address-of-plan.md`,
    `design-audit-2026-07-rc6.md`, `lsp-test-dogfooding-assessment.md`,
    `module-system-evaluation.md`, `code-gen-analysis.md`,
    `wolf-fc-design-assessment.md`.
  - Keep the design records but curate them, removing status banners,
    branch names and suite counts: `auto-deref-decision.md`,
    `generics-constraint-model.md`, and `result-type-design.md` (1,000
    lines; trim hard).
  - Your call: `fc-vs-zig.md` is an AI self-assessment. Delete it or move
    it to `hist/`.
  - Move `spec/fc.vim` to `editors/vim/`, next to the TextMate grammar
    that is mirrored from it.
  - `spec/TODO.md`: move the sections marked IMPLEMENTED or FIXED to
    `hist/archived-todos.md`, as its own header says, and keep open items
    short.
- **README:**
  - it names `run_tests.sh` as the test runner;
  - it promises a "formal grammar" that was deleted;
  - it should link to ARCHITECTURE.md and CONTRIBUTING.md.
- **Test tooling:**
  - Delete `tests/run_tests.sh` and the `make test` / `test-parallel`
    targets that use it. It has drifted: it reads `CC_OPT` to pick the O2
    skip rules but never passes it to the C compile, and it lacks the
    memory cap. `JOBS=1` already gives a serial run.
  - Delete the unused `.expected` stdout-diff support; no test uses it.
  - Merge the duplicated `test-all` / `test-all-O2` blocks in the
    Makefile.
  - Drop the legacy `src/*.o` sweep in `make clean` and the matching
    `.gitignore` lines.
  - Keep the per-OS build-directory explanation only in the Makefile;
    `run.sh`, the test runner, README and CLAUDE.md all repeat it.
- **Editor:**
  - `extension.js:5` describes `make install-vscode` as a plain copy, but
    it packages a `.vsix`;
  - `editors/vscode/README.md` says only the open file's diagnostics are
    shown, which is no longer true;
  - CLAUDE.md says the extension depends on `vscode-languageclient`, but it
    has no dependencies.
- **fcc usage:** there is no `--help`, and the usage line omits `--lsp`
  and `--version`.
- Optional, next time it is touched: `tests/lsp/lsp_test.py` (2,300 lines)
  grew by appending, with about 20 families of per-section helpers and
  mid-file imports. Restructure it as one session helper plus test
  functions.

## Leave alone

A human owner would not touch these, and the plan does not:

- **The large-switch design** of the checker, parser and emitter. Only the
  oversized cases move out.
- **File-scope state in codegen.** It is a single-pass emitter. Group the
  globals (phase 4), but don't thread a context struct through every call.
- **Pairs that look duplicated but differ on purpose:**
  - `try_eval_const` (pass2, at the type's width, wraps like the emitted C)
    vs `const_expr_eval` (types.c, i64 domain, where `i64.min / -1` is an
    error);
  - `emit_type` / `emit_type_ident` / `type_ident_eq` (documented as
    deliberate);
  - the per-purpose type walkers, whose descent rules genuinely differ.
- **Code that is already good:**
  - the Maranget exhaustiveness engine;
  - `json.c`, `args.c` and its Windows glob shim;
  - the lexer's layout pass and `#if` evaluator;
  - the parser's error-recovery machinery (`expect` contract,
    `recover_to`, `recover_progress`) and its `>>` splitting;
  - `mono_register` as the single choke point;
  - the `CNameClaims` set and the `TN_PUBLISH` ring;
  - `fc-spec.html`, `spec/examples.fc`, `spec/hist/`, and the stdlib code.
- **Renames in ast.h** (`struc`, `unio`, `enu`, `let_expr.let_name`). The
  churn outweighs the gain; rename only lines you are already changing.
- **The LSP location fields on AST nodes.** They are normal practice.
- **Swapping the const-eval error global for an out-parameter.** It would
  touch 36 `type_substitute` calls for no gain in correctness.
- **Splitting lsp.c.** 3,800 lines is within C norms. If it is split later,
  completion is the most self-contained part (about 1,000 lines with its
  own types), so move it first.
- **Recording name and annotation locations in the parser** to replace
  lsp.c's text-scanning helpers (`consider_type_annotation`,
  `kw_name_col`, `read_ident_at`). It is the right direction, but do it
  only when LSP work next touches that area.
- **Git history, the README disclosure, and "used to crash" comments in
  regression tests.**

## Done when

- B1-B21 are fixed and covered by tests, and the whole suite is green on
  gcc and clang, at O0 and O2, and with len16.
- Every whole-expression walker goes through `expr_for_each_child` or has a
  switch with no `default`. Adding an `ExprKind` produces `-Wswitch`
  warnings at every site that must handle it.
- No function listed in phase 5 has a case body longer than about 150
  lines.
- `make check` fails on non-ASCII in `src/`.
- This returns nothing, apart from any hits that have been reviewed and
  kept:
  `grep -nE 'used to|no longer|previously|audit item|item [0-9]|§[0-9]|20[0-9]{2}-[0-9]{2}' src/*.[ch]`
- `docs/ARCHITECTURE.md` and `CONTRIBUTING.md` exist, and nothing a human
  maintainer needs lives only in CLAUDE.md or in assistant memory.
- **Cold-read test:** someone who has not worked on the compiler adds a
  small feature, such as a new expression kind or a new diagnostic, using
  only the docs and the compiler's warnings, without asking anyone.
