# Contributing to FC

This covers building and testing the compiler, the test layout, the
conventions the code follows, and the principles behind design decisions.
`docs/ARCHITECTURE.md` explains how the compiler is put together;
`spec/fc-spec.html` defines the language.

## Building

| Command | What it does |
|---------|--------------|
| `make` | Release build (`-O2`) of `build/<os>/fcc`. |
| `make dev` | Clean rebuild at `-O0`, for debugging and readable sanitizer output. |
| `make OPT="-O0 -fsanitize=address,undefined"` | Any other flags. Run `make clean` first when changing `OPT`; Make does not track flag changes. |
| `make print-bin` | Print the binary's path. Scripts use this instead of hard-coding it. |
| `make install` / `make uninstall` | Install `fcc` and the stdlib under `PREFIX` (default `/usr/local`); `DESTDIR`, `bindir` and `datadir` work as usual. |
| `make install-vscode` | Build and install the VSCode extension. |
| `make help` | List every target. |

The compiler is plain C11 and builds warning-free with
`-std=c11 -Wall -Wextra -Wpedantic`. Keep it that way.

`./run.sh file.fc` compiles a program with the stdlib, runs it, and prints the
exit code.

## Testing

| Command | What it does |
|---------|--------------|
| `make check` | The ASCII check on `src/`, then `test-all`. Run this before sending a change. |
| `make test-all` | The suite under gcc and clang, in parallel. |
| `make test-gcc` / `make test-clang` | One compiler. |
| `make test-all-O2` (and `test-gcc-O2`, `test-clang-O2`) | Compile the generated C at `-O2`, which surfaces undefined behavior the optimizer exploits. |
| `make test-gcc-len16` / `make test-clang-len16` | The whole suite with 16-bit stored slice lengths (`--len-repr 16`). Keep these green when touching slice code. |
| `make test-lsp` | Language server tests (needs `python3`). Not part of `check`. |

Add `FILTER=pattern` to run the tests whose `category/name` matches a grep
pattern: `make test-gcc FILTER=closures`, `make test-gcc FILTER=stdlib/data`.
`JOBS=1` runs the suite serially.

The runner (`tests/run_tests.sh`) compiles each test to C with `fcc`,
compiles the C with `-std=c11 -Wall -Werror`, runs it and checks the result.
Because of `-Werror`, a test program must use every variable it declares.

### Test layout

Tests live in `tests/cases/<category>/`. Browse the categories to find where a
test belongs; standard library tests are under `stdlib/`.

A **single-file test** is `name.fc`, optionally with:

- `name.error`: the test must fail to compile, and this text must appear in
  the compiler's output (a fixed-string match). Keep it to one line.
- `name.expected_exit`: the expected exit code. Without it the program must
  exit 0. Most tests use `assert` instead, which aborts (exit 134) on failure.

A **multi-file test** is a directory holding several `.fc` files, compiled
together, plus any of these files (note: no leading dot):

| File | Meaning |
|------|---------|
| `error` | As `.error` above. |
| `expected_exit` | As `.expected_exit` above. |
| `deps` | Extra source files to compile with the test, one path per line, relative to the repository root (for example `stdlib/io.fc`). Use this rather than copying stdlib files. |
| `flags` | Conditional-compilation names, one per line; each becomes `--flag <name>`. |
| `fcc_args` | Literal extra `fcc` arguments, one per line; `#` starts a comment line. For example `--backtraces`, or `@file.rsp`. |
| `expected_stderr_contains` | Lines that must each appear in the program's stderr (fixed-string); `#` starts a comment line. For output whose exact layout varies, such as backtraces. |
| `skip_windows` | Skip on Windows (MSYS2/UCRT). The backtrace tests use it: frames come from `execinfo`, which that platform lacks. |
| `skip_o2` | Skip in the `-O2` runs. For assertions that only hold unoptimized, such as a full backtrace that tail-call optimization would shorten. |
| `only_o2` | Run only in the `-O2` runs. |

Every test, including error tests, needs a `let main`; put the bad code inside
`main`'s body. Exit codes are taken mod 256. Test programs follow FC naming
conventions: lowercase `snake_case` for every name.

### What to test

Every feature, bug fix or spec change comes with tests of the happy path, the
edge cases, the error cases, and interactions with existing features. In
particular:

- **A bug fix starts with a test that fails on the current compiler**, then
  sweeps sibling code for the same bug. When the fix lands in shared code,
  test the general behavior it now guarantees, in the category it belongs to,
  not only the case that exposed it.
- **The territory a feature takes over.** When new syntax or resolution
  claims a form that used to mean something else, test that the old readings
  still work next to it.
- **Errors reached indirectly.** Trigger each new error through indirect paths
  too (through a generic instantiation, an import, a nested module), not only
  at the direct site; indirect paths are where errors get lost.
- **Every type shape.** Exercise new type or codegen machinery with options,
  results, unions, nested structs and fixed arrays, not only the shape it was
  built for.
- **Generic and concrete twins.** Whichever of the two you developed on, the
  other is the untested one.

## Changing the compiler

### Adding a feature

- **Extend the existing structures; don't add a parallel one.** New data rides
  existing channels (const generic arguments ride the type-argument arrays),
  and new checks extend existing walkers and tables. Two copies of a walker
  drift apart.
- **A single enforcement point covers only what passes through it.** Before
  relying on one (`mono_register`, `resolve_symbol`), list what never reaches
  it and handle those cases.
- **The concrete case is part of the feature.** Machinery built for generics
  must also handle the fully concrete form (`u8[4 * 2]` in a non-generic
  struct).
- **A generic instance means what the same concrete code means.** Same result
  types, same widening, same checks.
- **Errors reported later need a guaranteed drain.** Anything stashed to
  report later must be turned into a diagnostic by the end of the pipeline.
  `fcc` exiting 0 with broken output is the worst failure the compiler has.
- **Decide semantic questions semantically.** The compiler sees the whole
  program, so don't approximate a name-dependent decision with token
  lookahead.
- **Update the spec** when a feature adds or changes a language rule.

### Refactoring

A refactor that shouldn't change behavior should leave the emitted C
unchanged. Compile the test suite, the demos and any other FC code you have to
C (and capture `fcc`'s error output for the error tests) before and after the
change, and compare the two sets byte for byte. Every difference needs an
explanation. Paths and line numbers embedded in the output change when the
sources move, so change one thing at a time.

### Adding an expression, pattern, declaration or type kind

1. Add the kind to its enum (`ExprKind`, `PatternKind`, `DeclKind` in
   `ast.h`; `TypeKind` in `types.h`) and its payload to the node.
2. Build. Every `switch` over the kind that has no `default` now warns
   (`-Wswitch`): the child visitors in `ast.c`, the LSP's node finder, and the
   other whole-tree walkers. Handle each one.
3. Then go through the switches that do have a `default`, since the compiler
   can't point you at those: `grep -n 'case EXPR_' src/*.c` (or `TYPE_`,
   `PAT_`, `DECL_`). The ones that matter most:
   - `check_expr_inner` in pass2 fails with "unsupported expression kind" on
     a kind it doesn't handle, so a test catches it;
   - `emit_expr` in codegen silently emits nothing for an unhandled kind, so
     check it by hand;
   - for a type kind: printing (`type_name`), equality, substitution and
     mangling in `types.c`, and `emit_type`, `emit_type_ident` and
     `type_ident_eq` in codegen, which must agree on what distinguishes two
     types.
4. Write the tests (above), including the error paths.

### Code conventions

- Declare stack structs with `= {0}` before calling their init function
  (`Parser parser = {0};`), so a field added later starts zeroed.
  `arena_alloc` already zero-fills.
- Build any string that embeds a name, type spelling, path or diagnostic with
  the exact-size helpers in `common.h` (`str_sprintf`, `arena_sprintf`,
  `str_appendf`, `intern_sprintf`), never a fixed buffer. See "No fixed
  buffers for names" in `docs/ARCHITECTURE.md`.
- New names in the emitted C must land in one of the three C name spaces
  (ARCHITECTURE.md, "C name spaces"), and new function-local bindings must get
  their C name from `local_c_name`.
- Source files are plain ASCII; `make check` enforces it. The exception is
  `src/builtin_docs.inc`, which holds user-facing markdown.
- Comments say what the code does and why, in the present tense. History
  belongs in commit messages, and a comment should not promise that two pieces
  of code stay in sync; if they must, make the compiler or a test check it.
- Diagnostics are errors; the compiler has no warnings. A check either
  matters enough to fail the build or isn't made.

### Commits

Write the subject as a short imperative sentence ("Make the module cycle check
count every reference"), and use the body to say why. One logical change per
commit.

## Engineering principles

These are the values behind past design decisions. The language rules they
led to are in the spec; these are the reasons to reach for when deciding
something new.

- **Explicit over implicit.** Inference never invents a type or a
  construction the programmer didn't write. When an explicit form is
  verbose, answer with a better diagnostic, not with rewriting (the spec's
  rejection of auto-wrapping for `x?` is the model case).
- **Restrictive but explicit over a hidden cost.** FC has no garbage
  collector to pay for convenience, so a feature that is more restrictive is
  better than one with an invisible cost. Escape hatches exist, but they are
  spelled out.
- **Static costs over runtime machinery.** If a compile-time bound is wrong,
  fix the bound; don't replace it with runtime bookkeeping. A documented,
  C-like cost beats hidden state in the generated code.
- **Measure before optimizing.** gcc and clang at `-O2` already remove most
  redundant checks. Before building an optimization into FC, measure what
  survives the C compiler on real programs, on both compilers. The leverage
  FC has is the shape of the C it emits.
- **The generated C must not assume the width of `int`.** FC targets include
  16-bit-`int` platforms; check codegen changes against both 16- and 32-bit
  `int`.
- **Completeness over partiality.** A restriction that is right in every case
  is better than a feature that works in common cases and is unsound at the
  edges. Don't ship a partial fix: solve the whole problem, or keep the
  restriction until you can.
- **FC is general-purpose.** Justify features by general systems-programming
  patterns, not by one program that wants them.
- **The spec and the compiler are the only authorities** on what FC is. There
  is no grammar file. When a question isn't settled by the spec, settle it
  and write the answer into the spec, stated as a rule rather than as a
  consequence of how the compiler happens to work.
