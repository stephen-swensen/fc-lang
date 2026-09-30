# FC compiler architecture

This document describes how `fcc` is put together: the pipeline, what each
source file is responsible for, the invariants the code relies on, and the
design of the language server. It names functions only where they are the
entry point to a subsystem; for anything finer, read the code.

The language itself is defined by `spec/fc-spec.html`; `spec/examples.fc` is a
runnable tour of it. For building, testing and the contribution checklist, see
`CONTRIBUTING.md`.

## Overview

FC is a systems language that compiles to C11. `fcc` is a whole-program
compiler written in C11 with no dependencies beyond the C library: it reads
every source file of a program at once, type-checks them together, and writes
one C file that the user compiles with GCC or Clang. The emitted C is C11 plus
a few GNU extensions (statement expressions, `__attribute__`, `__builtin_*`),
listed in the spec under "Why C as a target"; the test suite compiles it with
gcc and clang under `-Wall -Werror`.

The same binary is also the language server (`fcc --lsp`), which runs the front
end in-process on every edit.

## Pipeline

```
args -> lexer -> parser -> pass1 -> pass2 -> mono discovery -> mono finalize -> codegen
```

The command-line driver is `main()` in `src/main.c`:

1. **Arguments.** `args_expand` splices `@response` files into a flat token
   stream; `args_parse` interprets it (inputs, `-o`, `--flag`, `--len-repr`,
   `--backtraces`, host auto-detection of `os`/`arch`/`env` flags).
2. **Lex** every input file. Conditional compilation (`#if`) is evaluated here,
   against the flag set.
3. **Parse** (`parse_files`, which also does steps 4 and 5). First it
   collects generic names across all token streams
   (`parser_collect_generic_names`): in expression position, `name<...>` is
   read as an instantiation only when `name` is a generic declaration, and any
   file may use a generic declared in any other, so this needs the whole
   program before parsing starts.
4. Each file is parsed into a `Program`. Syntax errors are reported and
   recovered from; if there are any, the driver stops after step 5.
5. The per-file programs are merged into one (`program_merge`).
6. **pass1** (`pass1_collect`): declarations, layouts, signatures, imports.
7. **pass2** (`pass2_check`): type checking. It runs even when pass1 reported
   errors, so name errors and type errors come out together. The driver stops
   if anything was reported.
8. **Monomorphization**: `mono_discover_transitive` finds every generic
   instance reachable from the ones pass2 registered; `mono_finalize_types`
   resolves every instance's type names to their C names.
   Instance-level errors (a failed `static_assert`, a const-eval error, an
   infinite instance family) stop the driver.
9. **codegen** (`codegen_emit`) writes the C file.
10. If the program declares `error` groups, the driver writes the error-code
    map beside the C file: the output path with `.errcodes` in place of its
    extension.

The language server runs steps 2-7 through `analyze()` in `src/analyze.c`; see
[Language server](#language-server).

## Source files

| File | Responsibility |
|------|----------------|
| `main.c` | CLI driver (above). |
| `args.c/h` | `@response` expansion and argument parsing, shared with the language server. Paths inside a response file resolve against that file's directory, and globs in it are expanded. |
| `token.c/h` | Token kinds and their display names (`token_kind_name`). |
| `lexer.c/h` | Tokenizer, the `#if` evaluator, and the layout pass that turns indentation into `INDENT`/`DEDENT`/`NEWLINE` tokens (the offside rule; a leading `\|` of a match arm is a same-level delimiter). |
| `ast.c/h` | AST node types (`Expr`, `Pattern`, `Decl`, `Program`) and the shared child visitors `expr_for_each_child`, `expr_any_child` and `pattern_for_each_child`. |
| `parser.c/h` | Pratt parser with error recovery. Produces the AST, including desugared forms (an `error` group becomes a module of constants). |
| `types.c/h` | The `Type` representation, equality, printing (`type_name`), substitution, the context-free const-expression evaluator used by const generics, and type-name mangling (`instance_base_name` is the one rule for the template an instance is named from). Also the rules pass2 and codegen share about types: the constant operators (`const_binary_op`, `const_unary_op`, `const_cast_value`), what a type embeds by value (`type_byval_aggregate`), and which options are null-sentinel pointers (`option_inner_is_null_sentinel`). `str` and `cstr` are true aliases of `u8[]` and `u8*`: the `Type` keeps the alias spelling for messages only. |
| `pass1.c/h` | Symbol tables. Collects every top-level and module-level name, type layout and function signature so declarations can refer to each other in any order; resolves imports; assigns C names; numbers error codes; checks that no two declarations claim one C name. |
| `pass2.c/h` | The type checker: inference, name resolution, widening, casts, match exhaustiveness, provenance (escape) analysis, constant folding, generic validation, and registration of generic instances. `front_end_free` tears down what the front end built, for both fcc and the language server. |
| `facts.c/h` | Facts about expressions that pass2 judges by and codegen emits by, so the two agree: which operations `checked` and `unguarded` govern, which functions are static (`fn_static_target`: constants, and the only functions that cross to C), whether a pointer or integer is provably null/non-zero, error-constant values, and how interpolated-string segments are sized. |
| `monomorph.c/h` | The instance table (`mono_register`), transitive discovery, and the finalize step that resolves instance type names. |
| `codegen.c/h` | The C emitter. |
| `diag.c/h` | Error reporting. In server mode it reports to a sink and turns a fatal error into a `longjmp`. |
| `common.c/h` | The arena allocator; the `xmalloc`/`xcalloc`/`xrealloc` wrappers, which exit on out-of-memory; growable arrays (`DA_APPEND`); exact-size string builders; the intern table (the parser interns every name, so names compare by pointer); file reading; name helpers shared by several passes (`c_safe_ident`, `is_mangled_root_name`, `mangled_source_name`, `ns_display`); string-literal decoding and the one escape table (`simple_escape_byte`); the one format-spec reader and conversion table (`interp_spec_scan`, `interp_conv_class`), shared by the lexer, facts and codegen; and `g_len_repr`, the `--len-repr` setting. |
| `analyze.c/h` | The non-fatal front end used by the language server. |
| `lsp.c/h`, `json.c/h` | The language server and its JSON-RPC value model. |
| `builtin_docs.inc` | Hover documentation for built-in intrinsics (user-facing markdown). |
| `platform.c/h` | Host detection (`os`/`arch`/`env` flags) and path canonicalization. |
| `version.c/h` | `--version` output; the build writes the git and compiler details into a generated header. |

## Front end

### Lexer

The lexer scans the source, then runs a layout pass that converts indentation
into block tokens. Tabs are a compile error. Every lexical error (a tab, an
unterminated string or comment, inconsistent indentation, a bad `#if`) is
fatal: it calls `diag_fatal`, which exits in the CLI and aborts the analysis
in the server. The only other fatal errors are I/O failures in the driver,
running out of memory, and internal errors: a node reaching a pass in a shape
an earlier pass should have ruled out.

### Parser

The parser is a Pratt parser for expressions plus recursive descent for
statements, patterns and declarations. It never aborts:

- `expect` reports a mismatch and returns without consuming, so the caller
  keeps going with what it has.
- An unparseable region becomes an `EXPR_ERROR`, `PAT_ERROR` or `DECL_ERROR`
  node. These exist only when an error has been reported, so they never reach
  codegen.
- Progress is guaranteed by the leaf rule (the prefix and pattern-atom
  parsers consume the offending token unless it is a hard stop) and by a
  progress check in every item loop. The top-level and module-body loops also
  resynchronize at the next declaration (`recover_to_decl`); `DECL_PARSERS`
  is the one table of declaration-starting keywords.
- Where the parser tries one reading and may back out (a parenthesized type
  that might be a cast, a `<` that might open type arguments), the attempt runs
  inside `diag_speculate_begin`/`diag_speculate_end`: its errors are held and
  dropped if the parser backtracks, so an abandoned reading reports nothing.
  Held errors are not in `diag_error_count()`, so code inside a speculation
  that compares error counts sees none of them.

### pass1: declarations

pass1 walks declarations only. It builds the global `SymbolTable` and one
member table per module, records struct/union/enum layouts and function
signatures, and resolves `import` statements into import tables (one per
module body, one per file). Imports must precede other declarations in a file
or module body.

Name resolution, which pass1 sets up and pass2 carries out, looks in this
order: local scopes, then for the current module its members and then its
imports, then the same for each enclosing module in turn, then the importing
file's file-level imports, then global declarations. File-level imports are
visible only to the file that wrote them. `resolve_name` in pass2 is the one
implementation of this order; it also reports where it found the name, so a
caller looking for the companion half of a type/module pair looks in the same
place.

### pass2: type checking

Types flow bottom-up: every binding's type comes from its initializer, and
function parameter types anchor inference. There is no global unification.
pass2 walks each function body with a scope chain and annotates the AST in
place: each expression gets its `Type`, each identifier the `Symbol` it
resolved to, and each binding a function-local C name.

An expression that fails to type-check gets `TYPE_ERROR`, which is accepted
silently everywhere, so one mistake produces one diagnostic rather than a
cascade.

Besides typing, pass2 does:

- **Widening and casts.** Implicit widening only where lossless; everything
  else needs a cast.
- **Exhaustiveness** of `match`, with a Maranget-style usefulness check that
  names a missing case.
- **Provenance.** Pointer and slice expressions carry where they point
  (stack, heap, static, unknown), which rejects returning a pointer to the
  stack, freeing non-heap memory, and storing stack pointers in heap objects.
- **Constant folding** for top-level initializers and const-generic arguments.
- **Generics.** A generic body is checked once, with type variables abstract.
  Each instantiation is registered with the mono table, and the checks that
  depend on the concrete types (operator operands, sizes, const arguments) are
  made per instance by `validate_generic_expr`, which walks the body under
  that instance's bindings and reports through the chain of instantiations
  that led to it (see [Invariants](#invariants)).

### Monomorphization

`mono_register` is the single place a generic instance enters the table. It
mangles the instance name, evaluates the instance's `static_assert`s, and caps
the number of instances per template: a template that instantiates itself
with a growing const argument (`f<'n + 1>` inside `f`) would otherwise never
terminate. Once any error has been reported it registers nothing, so in a
program (or an editor buffer) that already has an error, no further instance
is checked. After pass2, discovery walks instantiated bodies to a fixpoint,
and finalize resolves every type name to its mangled C name. Codegen's
`emit_types` sorts every struct and union definition, instances and
top-level declarations together, so a by-value field's type is defined before
the struct that holds it.

### Code generation

`codegen_emit` writes, in order: the preamble (includes and defines), runtime
support helpers (`emit_runtime_support`), type definitions, prototypes,
globals, function bodies, and the backtrace table when `--backtraces` is on.
Generic functions and types are emitted once per instance, with a
substitution context that maps type variables to the instance's concrete
types: `subst_resolve` gives a node's type in the current instance, and
`instance_type` also resolves a generic stub to the instance's struct. The
body is written first, into the output file itself; the headers the preamble
includes and the runtime helpers it defines are then chosen by what the body
uses (`scan_body_needs`), and the file is rewritten as preamble, runtime
support, body. No emitter reports what it needs.

## Invariants

These are the rules the code depends on. Breaking one tends to produce C that
compiles and does the wrong thing, so they are worth knowing before changing
anything nearby.

### Errors

- There is one diagnostic severity: error. There are no warnings and no
  advisory passes; a check is important enough to fail the build or it is not
  made.
- Codegen never runs after an error. pass2 does run after recoverable parse
  and pass1 errors in the language server, and after pass1 errors in the CLI.
- Codegen reports a form it cannot emit with `internal_error`, which is an
  ordinary error: the driver checks the count after codegen and deletes the C
  file (and its `.errcodes` map), so a compiler bug never leaves broken output
  behind.
- Any error channel that stashes an error to report later must be drained by
  a caller that owns a diagnostic site. The main one is the const evaluator
  (`const_type_eval` in `types.c`), which runs inside type substitution with no
  diagnostic context and stashes its first failure; `const_eval_take_error`
  reports it and `const_eval_error_pending` asks whether one is waiting. The
  worst failure is `fcc` exiting 0 with broken output.

### Single resolution

Each name is resolved once, in pass2, and the result is stored on the AST
(`EXPR_IDENT.resolved_sym` and `companion_module`, `EXPR_FIELD.resolved_member`
and `companion_module`, and the call's resolved callee). Later code reads the
stored result, through `expr_symbol` and `expr_module` in pass2, instead of
looking the name up again. Resolving twice in different contexts is how a
parameter that shares a name with a module ends up meaning the module.

After pass2 every type name carried by a `Type` is canonical: the key it has
in the global symbol table. Mono and codegen look types up there by that key,
which is a table read, not a second resolution through scopes.

### C name spaces

FC identifiers cannot contain `__`. That keeps three name spaces in the
emitted C apart:

- `fc__...` for user declarations: `fc__` plus the FC path joined by `__`
  (`fc__name`, `fc__mod__name`, `fc__ns__mod__name`). Rooting the whole path
  keeps a module named `fc` from colliding with the prefix. The scheme is not
  injective (namespaces and nested modules flatten onto one separator), so
  pass1 reports any second declaration that claims an emitted name. Extern C
  names are emitted as written, so the parser rejects one that starts with
  `fc__`.
- `fc_<kind>_...` for names the compiler derives: `fc_str`, `fc_main`,
  `fc_eq_*`, `fc_fn_*`, `fc_tag_*` (a union's tag enum), `fc_tv_*` (its
  enumerators), `fc_slice_*`, `fc_option_*`, `fc_result_*`. The kind prefix
  is what keeps these apart from user names, and every join of two names must
  be shown injective: an FC identifier can start or end with `_`, so `a__b`
  can split two ways. (That is why tag enumerators put the variant first.) A
  suffix is added only after a prefixed name, where it stays inside that
  kind's space (`fc_enum_of_<E>_i`). A name derived from a type must
  distinguish exactly what the emitted C type distinguishes; `type_ident_eq`
  in codegen is that relation.
- `_l_<name>_<id>` for every function-local binding (let, parameter, loop
  variable, pattern binding), assigned by `local_c_name` in pass2, plus
  `_<temp><n>` for codegen temporaries. Compiler-made helpers share the `_`
  prefix: a lifted lambda `_fn_<n>` (suffixed `__<instance>` inside a
  generic instance) and its context struct `_ctx_<lambda>`, a function-entry
  backing slot `_fc_back_<n>` (for a slice literal, an interpolation buffer, a
  `(cstr[N])` buffer or a closure's context), and a constant's backing array
  `_fc_const_backing_<n>`. Source names never reach C directly, so a local
  can be named after a libc function or a C keyword.

Struct fields and union payloads keep their FC names (escaped by
`c_safe_ident` if they are C keywords); they live in per-type name spaces.

### No fixed buffers for names

Text that embeds an identifier, a qualified name, a type spelling, a
diagnostic, or a path is sized to its content, using `str_sprintf`,
`arena_sprintf`, `str_appendf` or `intern_sprintf` from `common.h`. A
truncated name is not an error; it is a different valid name (a member path
cut to a shorter member, two namespaces merged onto one symbol, a number
missing digits), and it fails silently. A fixed buffer is fine only for
content bounded by construction, such as an integer rendering or a
compiler-generated counter name.

`type_name()` returns a string from a small ring of rotating slots. Copy it if
you need to hold more than a couple at once.

### Slice lengths (`--len-repr`)

`.len` is an `i64` in the language on every target. `--len-repr 16|32|64`
changes only how wide the stored length is (`fc_len_t`) and the width of
bounds-check comparisons. Soundness rests on one rule: every stored length is
proven to lie in `[0, FC_LEN_MAX]` when the slice is constructed, either
statically in pass2 (literals, fixed arrays, const-generic sizes) or by a
runtime check in the emitted C (raw parts, `alloca`, `cstr` to `str`,
`argv`, interpolation). An `alloc` of more elements than fit returns `none`.
Any new way to construct a slice must go through this rule, and
`make test-gcc-len16` must stay green.

### Generic instances mean what concrete code means

An instance of a generic function or type behaves exactly as the same code
written with the concrete types would: the same result types, the same
widening, the same checks. A generic body may not type an operation
differently from its concrete counterpart, and a check the generic body
cannot make is made per instance by the concrete rule.

### Generated C

- The output compiles cleanly under `-std=c11 -Wall -Werror` with gcc and
  clang. Helpers are emitted `static __attribute__((unused))`, so unused ones
  don't warn and the C compiler drops them.
- It must not assume the width of `int`; FC targets include 16-bit-int
  platforms. Arithmetic on types narrower than `int` goes through `unsigned`,
  and lengths narrow to `size_t` or `int` through `fc_to_size`/`fc_to_int`.
  Those two and `fc_alloc_n` check with C `assert`, so compiling the output
  with `-DNDEBUG` removes the checks (the spec allows it); the bounds and
  length guards that carry FC semantics are never compiled out.
- Signed arithmetic goes through unsigned to define overflow:
  `(int32_t)((uint32_t)a + (uint32_t)b)`. Shift counts are masked
  (`a << (b & 31)`). Integer division aborts on a zero divisor and handles
  signed `MIN / -1` explicitly (C leaves it undefined).
- Slice indexing is bounds-checked, and unwrapping an option or result checks
  its tag first; a failed check aborts.
- Struct and union equality compiles to generated comparison functions.
- A function value is `{ fn_ptr, ctx }`, and every FC function's code takes a
  trailing `void* _ctx`. At the C boundary two kinds of wrapper bridge the
  difference, collected by `collect_boundary_code`: a trampoline
  `fc_ctramp_<name>` drops the context so C can call a static FC function,
  and `fc_cwrap_<C name>` adds it so an extern can be an FC value (spec
  §Static functions).
- `const` is emitted after the type it applies to (east const), so it binds to
  the pointee even when the pointee is itself a pointer (`int32_t* const*`).

### Walkers

A walk that must handle every kind has a `switch` with no `default`, so adding
a kind produces a `-Wswitch` warning at every such site, and
`make check-warnings` fails until each one handles it. These are the child
visitors in `ast.c` (`expr_for_each_child` and friends), the dispatchers
`check_expr_inner` (pass2) and `emit_expr` (codegen), the whole-tree
predicates in pass2 (such as `expr_may_yield_stack`, which decides whether a
loop's value can point into its own frame) and the self-recursion flow
(`sr_flow`), codegen's evaluation-order predicates (`expr_has_side_effects`,
`expr_structurally_equal`), the language server's node finder
(`find_in_expr`), and the type-kind walkers in `types.c` and codegen
(including `type_ident_eq`). A walk that treats most kinds alike lists them
anyway, and reaches their children through the visitor rather than naming
them itself. A kind that must never reach one of them gets an
explicit case that reports an internal error. A `switch` with a `default`
answers a question about a few kinds; CONTRIBUTING.md lists the ones a new
expression kind usually has to be added to.

Facts that pass2 and codegen both need (a type property's result type, a
built-in member's type, the width of a fixed-size integer, an interpolation
segment's size budget, which operations a marker governs) live in one table or
function, in `types.c` or `facts.c`, that both read. The same holds within a
pass: a rule that several checks or emitters apply (the length limits in
pass2's `static_length_error`, codegen's `union_variant_payload` and
`needs_eq_func`, the parser's `parse_body_lines`) is one function they call.

### Memory and re-entry

Everything that lives as long as one compilation (the AST, types, interned
names, diagnostic text) is allocated from the arena and freed with it; the
analysis result in the language server owns its arena. Symbol tables, growable
arrays, token arrays and source buffers are `malloc`ed (through the `x*`
wrappers) and freed by their owner.

The language server runs the front end many times in one process, so a
module-level static in the front end must not carry state from one analysis
into the next. Each one is reset where its pass starts (pass1's error-code
table, the diagnostic counters in `diag_reset_counts`), is drained by its
reader (the const-eval stash), or is a counter whose value only needs to be
unique (`local_c_name`'s id).

### Const generics

A generic parameter may be an integer value as well as a type. Const
arguments ride the same `Type **` type-argument arrays as type arguments
(`TYPE_CONST_INT` for a value, `TYPE_CONST_EXPR` for an unevaluated
expression), and a fixed array's size can be symbolic until substitution
folds it with `const_type_eval`. Parameter kinds (`GenParamKind`) sit in
arrays parallel to `type_params`.

Constant expressions are evaluated in two places. pass2 folds a module
constant's initializer (`fold_module_let`) or a concrete size, argument or
`static_assert` condition (`fold_to_literal`) in context with
`const_fold_expr`, which evaluates each operator with `try_eval_const`, with
names resolved and diagnostics at hand. `const_type_eval`
evaluates a `TYPE_CONST_EXPR` with no context during substitution, in the
`int64_t` domain of const arguments and sizes, and stashes its errors (see
[Errors](#errors)); `const_eval_typed` is its mode for a function body's
`static_assert` condition, which evaluates each node at the type pass2 gave it,
as the concrete condition is folded. Both places compute every operator with
`const_binary_op`, so they agree with each other and with the emitted C.

## Language server

`fcc --lsp` speaks the Language Server Protocol over stdio. `lsp_main` in
`src/lsp.c` is the message loop. The server is single-threaded and analyzes
lazily: edits only mark documents dirty, and the dirty units are re-analyzed
when the input queue drains or any request arrives, so a burst of keystrokes
costs one analysis.

### Analysis

`analyze()` runs the lexer, parser, pass1 and pass2 over in-memory sources and
keeps the typed AST, symbols and arena alive in an `AnalysisResult` for
queries. It differs from the CLI in four ways: diagnostics go to a collector,
a fatal error aborts only this analysis, pass2 runs past recoverable parse and
pass1 errors (so a half-typed line doesn't blank the rest of the file), and it
stops after pass2. An error that only monomorphization or codegen reports
therefore never reaches the editor. It does not require a `main`, so library files can be
analyzed on their own.

Unchanged feed files are not re-lexed on every keystroke: a session-scoped lex
cache keeps their tokens, keyed by path, content hash and flag set.

### Compilation units

Every open document belongs to a unit, and the server keeps one analysis per
unit, shared by all its open documents (`unit_key`). Which unit a document
belongs to depends only on the files on disk, never on the order documents
were opened in:

- **With `lsp.rsp`.** The server walks up from the document's directory for a
  file named `lsp.rsp`. If it finds one that lists the document, the unit is
  exactly what `fcc @lsp.rsp` would compile: its inputs (globbed, resolved
  against the response file's directory), its `--flag`s and its
  `--len-repr`. Nothing else is merged, so the editor resolves names exactly
  as the CLI does. A broken `lsp.rsp` falls back to the rule below and adds
  one diagnostic saying why.
- **Without it.** The unit is the document's directory: the document, its
  sibling `.fc` files and the installed standard library (`FCC_STDLIB_DIR`,
  else the install data directory, else a `stdlib` directory under the
  server's working directory). A stdlib file that is also open or a sibling is
  merged once, matched by canonical path or by identical content.
- **Not listed by the lsp.rsp above it.** The document is analyzed by the
  directory rule, in a unit of its own, and gets one diagnostic saying the
  rsp does not list it. That unit reports nothing about the files the rsp
  does list, so their diagnostics come only from the rsp's unit.

An open buffer always wins over its file on disk, however an `lsp.rsp` spells
the path (documents are matched by canonical path, `store_find_by_path`).

When a document changes, `flush_dirty` re-analyzes every unit that contains
it, not just the unit it keys: an `lsp.rsp` can list files from other
directories, so one file can belong to several units. Each unit records the
files its last analysis merged for this purpose. Units with no open documents
are dropped.

### Diagnostics

Diagnostics are published for every file in a unit, open or not
(`publish_project_diagnostics`), so breaking a file shows errors in the files
that depend on it. A file that goes clean or leaves the unit is cleared with
an empty publish. Standard library files are never reported unless the user
opens one.

### Queries

Hover, go-to-definition, completion, CodeLens and inlay hints all read the
typed AST. Because pass2 records resolved symbols and definition locations on
the nodes, most queries are a position lookup followed by reading what pass2
stored. Completion is scope-aware (`complete_scope`), handles member access
(`complete_members`), and treats an `import` statement as its own context
(`complete_import`). Built-in intrinsics have no declaration to read, so their
hover text comes from `builtin_docs.inc`.

The AST records a location for each node but not for every token in it, so
a few queries read the source text around a node: the type name inside a
written annotation (`consider_type_annotation`), the name after a declaration
keyword (`kw_name_col`, `let_name_col`), the keyword a built-in was spelled
with, and the statement being typed during `import` completion. Every such
scan is bounded by the line index (`line_start`), because the AST may come
from an older version of the text than the one being scanned.

If the lexer aborted (a stray tab, an unterminated string), the fresh
analysis has no types. Queries then use the unit's last analysis that did
type-check (`query_result`), whose positions may be off for the line being
edited; diagnostics always come from the fresh analysis.

On Windows, the server does not discover `lsp.rsp` files, sibling files or
the installed standard library (those need POSIX directory walking), so each
document is its own unit.

The editor extension in `editors/vscode/` is a thin client that starts
`fcc --lsp` and restarts it if it exits.
