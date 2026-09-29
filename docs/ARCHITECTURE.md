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
one C file that the user compiles with any C11 compiler (the test suite uses
gcc and clang with `-Wall -Werror`).

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
3. **Collect generic names** across all token streams
   (`parser_collect_generic_names`). In expression position, `name<...>` is
   read as an instantiation only when `name` is a generic declaration, and any
   file may use a generic declared in any other, so this needs the whole
   program before parsing starts.
4. **Parse** each file into a `Program`. Syntax errors are reported and
   recovered from; if there are any, the driver stops here.
5. **Merge** the per-file programs (`program_merge`).
6. **pass1** (`pass1_collect`): declarations, layouts, signatures, imports.
7. **pass2** (`pass2_check`): type checking. It runs even when pass1 reported
   errors, so name errors and type errors come out together. The driver stops
   if anything was reported.
8. **Monomorphization**: `mono_discover_transitive` finds every generic
   instance reachable from the ones pass2 registered; `mono_finalize_types`
   resolves names and orders instances so by-value dependencies come first.
   Instance-level errors (a failed `static_assert`, a const-eval error, an
   infinite instance family) stop the driver.
9. **codegen** (`codegen_emit`) writes the C file.
10. If the program declares `error` groups, the driver writes the error-code
    map (`<output>.errcodes`) beside the C file.

The language server runs steps 2-7 through `analyze()` in `src/analyze.c`; see
[Language server](#language-server).

## Source files

| File | Responsibility |
|------|----------------|
| `main.c` | CLI driver (above). |
| `args.c/h` | `@response` expansion and argument parsing, shared with the language server. Paths inside a response file resolve against that file's directory, and globs in it are expanded. |
| `token.c/h` | Token kinds and the intern table. Identifier and keyword strings are interned, so names compare by pointer. |
| `lexer.c/h` | Tokenizer, the `#if` evaluator, and the layout pass that turns indentation into `INDENT`/`DEDENT`/`NEWLINE` tokens (the offside rule; a leading `\|` of a match arm is a same-level delimiter). |
| `ast.c/h` | AST node types (`Expr`, `Pattern`, `Decl`, `Program`) and the shared child visitors `expr_for_each_child`, `expr_any_child` and `pattern_for_each_child`. |
| `parser.c/h` | Pratt parser with error recovery. Produces the AST, including desugared forms (an `error` group becomes a module of constants). |
| `types.c/h` | The `Type` representation, equality, printing (`type_name`), substitution, the context-free const-expression evaluator used by const generics, and type-name mangling. `str` and `cstr` are true aliases of `u8[]` and `u8*`: the `Type` keeps the alias spelling for messages only. |
| `pass1.c/h` | Symbol tables. Collects every top-level and module-level name, type layout and function signature so declarations can refer to each other in any order; resolves imports; assigns C names; numbers error codes; checks that no two declarations claim one C name. |
| `pass2.c/h` | The type checker: inference, name resolution, widening, casts, match exhaustiveness, provenance (escape) analysis, constant folding, generic validation, and registration of generic instances. |
| `facts.c/h` | Facts about values that pass2 judges by and codegen emits by, so the two agree: whether a pointer or integer is provably null/non-zero, error-constant values, and how interpolated-string format specs are read and sized. |
| `monomorph.c/h` | The instance table (`mono_register`), transitive discovery, and the finalize step that resolves and orders instances. |
| `codegen.c/h` | The C emitter. |
| `diag.c/h` | Error reporting. In server mode it reports to a sink and turns a fatal error into a `longjmp`. |
| `common.c/h` | Arena allocator, growable arrays (`DA_APPEND`), exact-size string builders, file reading. |
| `analyze.c/h` | The non-fatal front end used by the language server. |
| `lsp.c/h`, `json.c/h` | The language server and its JSON-RPC value model. |
| `builtin_docs.inc` | Hover documentation for built-in intrinsics (user-facing markdown). |
| `platform.c/h` | Host detection (`os`/`arch`/`env` flags) and path canonicalization. |
| `version.c/h` | `--version` output; the build writes the git and compiler details into a generated header. |

## Front end

### Lexer

The lexer scans the source, then runs a layout pass that converts indentation
into block tokens. Tabs are a compile error. Unrecoverable lexical errors
(tabs, an unterminated string or comment, inconsistent indentation) are the
only fatal errors in the compiler: they call `diag_fatal`, which exits in the
CLI and aborts the analysis in the server.

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
  resynchronize at the next declaration (`recover_to`).

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
visible only to the file that wrote them. `resolve_symbol` in pass2 is the one
implementation of this order.

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
  Each instantiation is registered with the mono table and checked against
  the concrete rules (see [Invariants](#invariants)).

### Monomorphization

`mono_register` is the single place a generic instance enters the table. It
mangles the instance name, evaluates the instance's `static_assert`s, and caps
the number of instances per template: a template that instantiates itself
with a growing const argument (`f<'n + 1>` inside `f`) would otherwise never
terminate. After pass2, discovery walks instantiated bodies to a fixpoint,
and finalize resolves every type name to its mangled C name and sorts
instances so a by-value struct field's type is emitted before the struct.

### Code generation

`codegen_emit` writes, in order: the preamble (includes and defines), runtime
support helpers, type definitions, prototypes, globals, function bodies, and
the backtrace table when `--backtraces` is on. Generic functions and types
are emitted once per instance, with a substitution context that maps type
variables to the instance's concrete types.

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
- Any error channel that stashes an error to report later (const evaluation
  inside instantiation is the main one) must be drained at the end of the
  pipeline. The worst failure is `fcc` exiting 0 with broken output.

### Single resolution

Each name is resolved once, in pass2, and the result is stored on the AST
(`EXPR_IDENT.resolved_sym`, `companion_module`, `EXPR_FIELD.resolved_member`,
and the call's resolved callee). Later code reads the stored pointer instead of
looking the name up again. Resolving twice in different contexts is how a
parameter that shares a name with a module ends up meaning the module.

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
  enumerators), `fc_slice_*`, `fc_option_*`, `fc_result_*`. A derived name is
  never built by suffixing a user name, and every join of two names must be
  shown injective: an FC identifier can start or end with `_`, so `a__b` can
  split two ways. (That is why tag enumerators put the variant first.) A name
  derived from a type must distinguish exactly what the emitted C type
  distinguishes; `type_ident_eq` in codegen is that relation.
- `_l_<name>_<id>` for every function-local binding (let, parameter, loop
  variable, pattern binding), assigned by `local_c_name` in pass2, plus
  `_<temp><n>` for codegen temporaries. Source names never reach C directly,
  so a local can be named after a libc function or a C keyword.

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
  platforms.
- Signed arithmetic goes through unsigned to define overflow:
  `(int32_t)((uint32_t)a + (uint32_t)b)`. Shift counts are masked
  (`a << (b & 31)`). Integer division aborts on a zero divisor and handles
  signed `MIN / -1` explicitly (C leaves it undefined).
- Slice indexing is bounds-checked, and unwrapping an option or result checks
  its tag first; a failed check aborts.
- Struct and union equality compiles to generated comparison functions.
- `const` is emitted after the type it applies to (east const), so it binds to
  the pointee even when the pointee is itself a pointer (`int32_t* const*`).

### Walkers

Every whole-tree walk over expressions goes through `expr_for_each_child` or
has a `switch` with no `default`. Adding an `ExprKind` therefore produces a
`-Wswitch` warning at every site that must handle it, and the build warns
until they all do.

### Const generics

A generic parameter may be an integer value as well as a type. Const
arguments ride the same `Type **` type-argument arrays as type arguments
(`TYPE_CONST_INT` for a value, `TYPE_CONST_EXPR` for an unevaluated
expression), and a fixed array's size can be symbolic until substitution
folds it with `const_type_eval`. Parameter kinds (`GenParamKind`) sit in
arrays parallel to `type_params`.

## Language server

`fcc --lsp` speaks the Language Server Protocol over stdio. `lsp_main` in
`src/lsp.c` is the message loop. The server is single-threaded and analyzes
lazily: edits only mark documents dirty, and the dirty units are re-analyzed
when the input queue drains or a type-aware request arrives, so a burst of
keystrokes costs one analysis.

### Analysis

`analyze()` runs the lexer, parser, pass1 and pass2 over in-memory sources and
keeps the typed AST, symbols and arena alive in an `AnalysisResult` for
queries. It differs from the CLI in four ways: diagnostics go to a collector,
a lexer fatal aborts only this analysis, pass2 runs past recoverable parse and
pass1 errors (so a half-typed line doesn't blank the rest of the file), and it
stops after pass2. It does not require a `main`, so library files can be
analyzed on their own.

Unchanged feed files are not re-lexed on every keystroke: a session-scoped lex
cache keeps their tokens, keyed by path, content hash and flag set.

### Compilation units

Every open document belongs to a unit, and the server keeps one analysis per
unit, shared by all its open documents (`unit_key`):

- **With `lsp.rsp`.** The server walks up from the document's directory for a
  file named `lsp.rsp`. If it finds one, the unit is exactly what `fcc
  @lsp.rsp` would compile: its inputs (globbed, resolved against the response
  file's directory), its `--flag`s and its `--len-repr`. Nothing else is
  merged, so the editor resolves names exactly as the CLI does. A broken
  `lsp.rsp` falls back to the rule below and adds one diagnostic saying why.
- **Without it.** The unit is the document's directory: the document, its
  sibling `.fc` files and the installed standard library (`FCC_STDLIB_DIR`,
  else the install data directory, else `./stdlib`). A stdlib file that is
  also open or a sibling is merged once, matched by canonical path or by
  identical content.

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

If the lexer aborted (a stray tab, an unterminated string), the fresh
analysis has no types. Queries then use the unit's last analysis that did
type-check (`query_result`), whose positions may be off for the line being
edited; diagnostics always come from the fresh analysis.

The editor extension in `editors/vscode/` is a thin client that starts
`fcc --lsp`.
