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
| `make OPT=-O3` | Any other flags. Run `make clean` first when changing `OPT`; Make does not track flag changes. |
| `make asan` | `fcc` built with AddressSanitizer and UBSan, in its own directory (`build/<os>-asan`), so it does not disturb the normal build. |
| `make print-bin` | Print the binary's path. Scripts use this instead of hard-coding it. |
| `make install` / `make uninstall` | Install `fcc` and the stdlib under `PREFIX` (default `/usr/local`); `DESTDIR`, `bindir` and `datadir` work as usual. The stdlib path the language server uses is compiled in from `datadir`, and changing it rebuilds what embeds it. |
| `make install-vscode` | Build and install the VSCode extension. |
| `make help` | List every target. |

The compiler is plain C11 and builds warning-free with
`-std=c11 -Wall -Wextra -Wpedantic` under both gcc and clang;
`make check-warnings` enforces it. The C it emits is C11 plus the GNU
extensions listed in the spec (§Why C as a target), so it needs GCC or Clang.

`./run.sh file.fc` compiles a program with the stdlib, runs it, and prints the
exit code.

## Testing

| Command | What it does |
|---------|--------------|
| `make check` | Everything below that runs in a few minutes: the source checks, `test-all`, `test-all-len16` and `test-lsp`. Run this before sending a change. CI (`.github/workflows/check.yml`) runs it and `test-vscode` on every push. |
| `make check-ascii`, `check-warnings`, `check-keywords`, `check-examples` | The source checks: `src/` is ASCII; the compiler builds warning-free under gcc and clang; every hand-kept keyword list (token names, the spec, both editor grammars) matches the lexer's table, and every built-in operator has hover text (`tools/check-keywords.py`); `spec/examples.fc` compiles and runs. |
| `make test-all` | The suite under gcc and clang, in parallel. |
| `make test-gcc` / `make test-clang` | One compiler. |
| `make test-all-O2` (and `test-gcc-O2`, `test-clang-O2`) | Compile the generated C at `-O2`, which surfaces undefined behavior the optimizer exploits. |
| `make test-all-len16` (and `test-gcc-len16`, `test-clang-len16`) | The whole suite with 16-bit stored slice lengths (`--len-repr 16`). Keep these green when touching slice code. |
| `make test-lsp` | Language server tests (needs `python3`). |
| `make test-asan` | The compiler suite (gcc) and the language server tests against the `make asan` build. Slower; run it after changing memory handling. |
| `make test-vscode` | The VSCode extension against a real server, with the VSCode API mocked (needs `node`). Run it after changing `editors/vscode/extension.js`. |

Add `FILTER=pattern` to run the tests whose `category/name` matches an awk
(extended) regular expression: `make test-gcc FILTER=closures`,
`make test-gcc FILTER='^stdlib/data'`. `JOBS=1` runs the suite serially.

The runner (`tests/run_tests.sh`, run from the repository root) compiles each test to C with `fcc`,
compiles the C with `-std=c11 -Wall -Werror`, runs it and checks the result.
Because of `-Werror`, every test also checks that `fcc`'s output compiles
warning-free. Its environment variables are listed at the top of the script;
the useful ones besides `FILTER` and `JOBS`:

- `FCC=path` tests another build of the compiler, such as an old one to
  confirm a new test fails on it.
- `KEEP=1` keeps the work directory, with every test's C file and output.
- `FC_TEST_MEM_CAP_KB=0` lifts the memory cap on `fcc` (3 GB by default, so a
  runaway instantiation fails instead of swapping). A sanitizer build needs
  it; `make test-asan` sets it.

A failing test prints the `fcc` or C compiler command that failed, rewritten
to write under `/tmp`, so it can be rerun by hand.

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
| `fcc_args` | Literal extra `fcc` arguments, one per line; `#` starts a comment line. For example `--backtraces`, or `@tests/cases/<category>/<test>/file.rsp` (paths resolve from the repository root). |
| `expected_stderr_contains` | Lines that must each appear in the program's stderr (fixed-string); `#` starts a comment line. For output whose exact layout varies, such as backtraces. |
| `*.h` | C headers the test's `extern` declarations include; the test's directory is on the C include path. |
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
- **Report user errors in pass1 or pass2 when you can.** The language server
  stops after pass2, so an error raised by monomorphization or codegen fails
  the command-line build but never reaches the editor.
- **Update the spec** when a feature adds or changes a language rule.

### Language server tests

`tests/lsp/lsp_test.py` drives `fcc --lsp` over stdio. Each scenario builds a
list of JSON-RPC messages, runs them through one server process with
`run_session`, and asserts on the responses with `check(name, cond)`. To add a
case, add a scenario at the end in the same style: write any files it needs
into a `tmpdir(...)` directory, and give each request a unique id. The script
has no single-scenario mode; it runs the whole file in a few seconds.

Every session also checks that the server survived: a session whose process
crashed, or printed a sanitizer report, fails the run at the end even if its
assertions passed. `make test-asan` runs the file against the sanitizer build,
which is how memory errors in the server are found.

### Refactoring

A refactor that shouldn't change behavior should leave the emitted C
unchanged. `tools/emit-corpus.sh OUTDIR` compiles every test and demo to C
and captures `fcc`'s error output for the error tests; run it before and after
the change and compare the two directories (`diff -r`). Every difference
needs an explanation. Extra programs can be added with a response file
argument (`tools/emit-corpus.sh OUTDIR @../other/program.rsp`). Paths and
line numbers embedded in the output change when the sources move, so change
one thing at a time.

### Debugging a wrong build

- **`fcc` crashes.** Build `make asan` and rerun the command with
  `build/<os>-asan/fcc`; the report points at the bad access. Set
  `ASAN_OPTIONS=detect_leaks=0` (as `make test-asan` does): the compiler
  leaves its memory to process exit, so the leak report is noise.
- **An internal compiler error.** `internal compiler error: ...` (codegen) or
  `internal: ...` / `unsupported expression kind` (pass2) means a node reached
  a pass in a shape an earlier pass should have rejected or rewritten. The fix
  is usually in the earlier pass: find where that shape should have been
  caught.
- **Wrong output.** Keep the C (`KEEP=1`, or the rerun command) and read it;
  the emitted C mirrors the source closely. Compile it with
  `-O0 -g -fsanitize=address,undefined` to catch undefined behavior in the
  generated code. If only a generic or only a concrete version misbehaves,
  compare the C for the two.

### Adding a built-in

A keyword-shaped built-in (`sizeof`, `alloc`, `static_assert`, an atomic
operation, ...) touches these places:

1. `token.h` (the token kind), `lexer.c` (its `KW` entry) and `token.c`
   (`token_kind_name`).
2. The parser: its prefix-parse case, and `parse_type_arg`'s list if it may
   appear as a generic argument.
3. The expression kind, if it gets one (see the next section).
4. The language server: a `BUILTIN_DOCS` entry in `src/builtin_docs.inc`,
   keyed by the spelling, and a `consider_builtin` call in `lsp.c`'s node
   finder (`find_in_expr`) so hovering the keyword shows it; add the keyword
   to the built-in hover probes in `tests/lsp/lsp_test.py`.
5. The spec: a section for it, and the word in its "reserved identifiers"
   list (built-in operators; syntax keywords go in "reserved words"). Both
   editor grammars (`editors/vscode/syntaxes/fc.tmLanguage.json`,
   `editors/vim/fc.vim`). `make check-keywords` fails until these match the
   lexer and the hover entry exists.
6. `FEATURES.md`, the feature inventory. If the emitted C uses a GNU
   extension the spec's "GNU extensions required" list lacks, add it there.
   Runtime helpers the C calls go in `emit_runtime_support` (codegen); a
   helper that prints to stderr goes in its `STDIO_HELPERS` table. The
   preamble includes what the emitted body turns out to use.

Tests for an operator or built-in go in `tests/cases/expressions/`, with its
generic behavior in `generics/`.

### Adding an expression, pattern, declaration or type kind

1. Add the kind to its enum (`ExprKind`, `PatternKind`, `DeclKind` in
   `ast.h`; `TypeKind` in `types.h`) and its payload to the node.
2. Build. Every `switch` over the kind that has no `default` now warns
   (`-Wswitch`), and those are the ones that must handle every kind: the
   child visitors in `ast.c`, the two dispatchers (`check_expr_inner` in
   pass2, `emit_expr` in codegen), the LSP's node finder, the whole-tree
   predicates in pass2 (such as `expr_may_yield_stack`), the self-recursion
   flow (`sr_flow`: whether control can complete through the kind, or leave
   the function from inside it), codegen's evaluation-order predicates
   (`expr_has_side_effects`, `expr_structurally_equal`), and for a type kind
   printing, equality, substitution and mangling in `types.c` and `emit_type`
   / `emit_type_ident` / `type_ident_eq` in codegen, which must agree on what
   distinguishes two types. Handle each one; a kind that cannot reach a
   dispatcher gets an explicit internal error there, not a silent default.
3. Then go through the switches that do have a `default`, since the compiler
   can't point you at those. They answer a question about a few kinds and
   default the rest; decide whether the new kind belongs in the few. For a new
   expression kind, these are the ones that usually matter:
   - `validate_generic_expr` (pass2): the per-instance check of a generic
     body. A kind whose rule depends on its operand's type needs a case here,
     or an instance at a type the rule rejects reaches codegen unchecked.
   - `const_fold_expr`, `is_init_expr` and `const_clone_expr` (pass2):
     whether the kind is a compile-time constant and may initialize a module
     constant. The three must agree.
   For a type or pattern kind: `grep -n 'case TYPE_' src/*.c` (or `PAT_`).
4. In pass2, check a child whose value the new node consumes (an operand, an
   argument, an element) with `check_operand`, not `check_expr`: it rejects a
   self-recursive call with no base case at that point. A child whose value
   becomes the node's own (a branch, a block's last statement) uses
   `check_expr`.
5. Write the tests (above), including the error paths.

### Code conventions

- Declare stack structs with `= {0}` before calling their init function
  (`Lexer lexer = {0};`), or with a designated initializer, so a field added
  later starts zeroed. `arena_alloc` and `xcalloc` already zero-fill.
- Allocate with `xmalloc`, `xcalloc` and `xrealloc` (`common.h`), which exit
  with a message when memory runs out, and with the arena for anything that
  lives as long as the AST. `DA_APPEND` grows arrays with `xrealloc`.
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
- Codegen trusts the checker but does not guess: an emitter that meets a form
  it cannot emit reports it with `internal_error` rather than writing
  something plausible.

### The standard library

The stdlib (`stdlib/*.fc`) is FC code compiled with each program, one file
per module, all in `namespace std::`. Conventions:

- A module's documentation is the comment between the `namespace` line and
  `module x =`; it is what the language server shows on hover. Each public
  definition has a doc comment directly above it: a capitalized sentence,
  not prefixed with the name. Examples go in a fenced ```` ```fc ```` block.
- Failures return results (`T!`). A module names the failure conditions a
  caller can act on in an `error` group (`io.file.not_found`) and passes
  other platform codes through raw.
- Tests go under `tests/cases/stdlib/`, as multi-file tests whose `deps` file
  lists the stdlib files they use.

### Editing the spec

`spec/fc-spec.html` is one Markdown document inside a
`<script type="text/markdown">` tag, rendered in the browser; edit the
Markdown. State rules in the present tense, as rules of the language rather
than consequences of how the compiler works, and keep the whole rule in the
spec rather than pointing to design notes elsewhere. `make check-keywords`
checks its reserved-word lists against the lexer.

### Releases

The version is `VERSION` (a SemVer string such as `1.0.0-rc.8`); `fcc
--version` adds the commit hash and date at build time. A release updates
`VERSION` and every other spelling of the old version, which `README.md` and
`spec/fc-spec.html` repeat (`grep -rn` for it).

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
  `int`. No 16-bit compiler is in the test matrix, so this is a review step:
  read the new C asking what each literal, promotion and shift does when
  `int` is 16 bits. Arithmetic on types narrower than `int` goes through
  `unsigned` (the emitted `(unsigned)(uint16_t)a * (unsigned)(uint16_t)b`),
  and sizes narrow to `size_t` or `int` through `fc_to_size` / `fc_to_int`.
- **Completeness over partiality.** A restriction that is right in every case
  is better than a feature that works in common cases and is unsound at the
  edges. Don't ship a partial fix: solve the whole problem, or keep the
  restriction until you can.
- **FC is general-purpose.** Justify features by general systems-programming
  patterns, not by one program that wants them.
- **Look at precedents first.** Before deciding a language question, lay out
  how C, Rust, Zig and similar languages handle it, and check the spec for an
  existing rule that already settles it or constrains the answer.
- **The spec and the compiler are the only authorities** on what FC is. There
  is no grammar file. When a question isn't settled by the spec, settle it
  and write the answer into the spec, stated as a rule rather than as a
  consequence of how the compiler happens to work.
