# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

This is a compiler project for **FC** (version 1.0.0-rc.6), a systems programming language that transpiles to C11. The compiler (`./fcc`) is implemented in C and lives in `src/`. The language specification is in `spec/fc-spec.html`. For a quick reference of FC syntax and semantics, see `spec/examples.fc` — a runnable, commented program that demonstrates all core language features.

## Build & Run

- **`make`** — Release build (`-O2`). Produces `fcc` at `./build/<os>/fcc` (where `<os>` is `linux`/`windows`/`macos`; `.exe` suffix on Windows). The per-OS subdirectory keeps a shared source tree across two operating systems (e.g. WSL + MSYS2) from cross-execing each other's binaries.
- **`make print-bin`** — Echo the per-OS binary path. `run.sh`, `tests/run_tests*.sh`, and `demos/*/run.sh` use this instead of replicating OS detection.
- **`make dev`** — Clean rebuild at `-O0` for clearer diagnostics during dev iteration. Use this when assertion failures, debugger sessions, or sanitizer output need to be readable; `-O2` builds optimize in ways that can muddle line attribution.
- **`make check`** / **`make test-all`** — Run all tests with both gcc and clang. `check` is the GNU-canonical alias.
- **`make test-gcc`** / **`make test-clang`** — Test with a single compiler.
- **`make test-gcc FILTER=pattern`** — Run only tests matching a pattern (e.g., `FILTER=stdlib/data`, `FILTER=closures`).
- **`make install`** / **`make uninstall`** — Install `fcc` to `$(bindir)` and stdlib to `$(datadir)/fcc/stdlib/` per GNU conventions. Defaults to `PREFIX=/usr/local`; override `PREFIX`, `DESTDIR`, `bindir`, or `datadir` as needed.
- **`make install-vscode`** / **`make uninstall-vscode`** — Install/remove the VSCode extension (Linux) in `~/.vscode/extensions/`; vendors `vscode-languageclient` via `npm`. The extension drives `fcc --lsp`. See **Editor integration**.
- **`make test-lsp`** — Run the LSP server wire tests (`tests/lsp/`, needs `python3`). Kept out of `make check` so it can't regress the compiler suite.
- **`make clean`** — Remove `build/` (every OS subdirectory).
- **`make help`** — Print the full target reference.
- **`./run.sh file.fc`** — Compile, link with stdlib, run, and print exit code. Supports `--flag name` and multiple source files.

**Optimization (default = `-O2`):** plain `make` produces a release-optimized binary. Use `make dev` for the dev-iteration loop (`-O0`, clearer diagnostics), or override with e.g. `make OPT=-O0` or `make OPT="-O0 -fsanitize=address,undefined"`. Tests default to `-O0` for the transpiled C; `make test-gcc-O2` / `test-clang-O2` / `test-all-O2` re-run the suite at `-O2` to catch optimizer-surfaced UB. The compiler-build OPT and the test-C OPT are independent axes. `make clean` is required when switching `OPT` values since Make doesn't track CFLAGS changes — `make dev` takes care of this for you.

The compiler is built with `cc -std=c11 -Wall -Wextra -Wpedantic -g` (plus `$(OPT)`). Tests compile the generated C with `cc -std=c11 -Wall -Werror`, so the emitted C must be warning-clean. FC emits all helpers (e.g. imported stdlib functions) as `static __attribute__((unused))`, so `-Wunused-function` stays quiet even though many helpers are unreferenced in any given program — GCC/clang still DCE them at link time.

**Struct initialization**: Always declare stack-allocated structs with `= {0}` before calling their init function (e.g., `Parser parser = {0};`). This prevents uninitialized-field bugs when new fields are added but the init function isn't updated — the kind of bug that only manifests on some platforms. `arena_alloc` already zero-fills heap allocations.

## Git Workflow

- **Never auto-commit**
- **Never invoke `/commit`** — the user always runs this manually
- Default branch is `main`; experimental work may happen in other branches
- Working changes may be discarded and branches may be abandoned — follow the user's lead
- The user will always be in control of commiting changes, and may do so outside of the claude sessions itself, so don't count on the state of the git repository remaining static

## Language Overview

FC is a C-targeting language with these core design constraints:
- **Target**: C11 (using `int8_t`/etc. from `<stdint.h>`, `_Static_assert`, anonymous unions)
- **Memory model**: Manual, no GC, no borrow checker — follows C's philosophy
- **Syntax**: Indentation-based (offside rule, spaces only — tabs are a compile error)
- **Comments**: `//` line comments, `/* */` block comments (nestable). No `#` comments.
- **Type inference**: Directional (bottom-up, inside-out), never global unification
- **Generics**: Monomorphized at compile time, zero runtime cost
- **Completeness over partiality**: Prefer conservative-but-complete restrictions over partial solutions that only cover some cases. A feature that works correctly in all scenarios (even if limited) is better than one that works in common cases but has subtle unsound edge cases. Don't ship a partial fix — either solve the full problem space or keep the restriction until you can.

## Compiler Architecture

The compiler pipeline is: **source → lexer → parser → pass1 → pass2 → mono discovery → mono finalize → codegen → C file**.

### Source files (`src/`)

- **`main.c`** — Entry point. Reads input file, runs the pipeline, writes output.
- **`lexer.c/h`** — Tokenizer. Scans source into tokens, then runs a layout pass that converts indentation into `INDENT`/`DEDENT`/`NEWLINE` tokens (offside rule). Also handles same-level match blocks where `|` acts as a delimiter.
- **`token.c/h`** — Token types and interning. All identifier/keyword strings are interned for pointer-equality comparison.
- **`parser.c/h`** — Pratt parser. Produces an AST of `Expr` and `Decl` nodes. Handles expressions (12 precedence levels), statements, patterns, and declarations.
- **`ast.h`** — AST node definitions. `Expr` (expressions, including `let` bindings), `Pattern` (match patterns), `Decl` (top-level declarations), `Program` (root).
- **`pass1.c/h`** — First pass. Walks declarations to collect top-level names, struct/union layouts, and function signatures into a `SymbolTable`. Enables forward references. For module-scoped types: mangles declaration names (e.g., `inner` → `m__inner`), canonicalizes generic stub names in field types to use mangled forms, and registers module types in the global symtab under mangled names so they're findable from any context.
- **`pass2.c/h`** — Second pass (type checker). Walks expressions with a scope chain, infers types bottom-up, resolves identifiers, checks type compatibility, validates casts and widening. Assigns unique codegen names to local bindings for shadowing support. Registers monomorphized generic instances. Uses `ModuleScopeChain` for arbitrary-depth parent module symbol lookup with interleaved import/parent resolution: at each module level, members are checked then that level's imports before moving to the parent, so a child's import can shadow a parent's member. `scope_lookup_capture` stops at the current module boundary (first `is_global` scope); parent resolution is handled by the interleaved loop. `resolve_symbol`/`resolve_symbol_kind` encode this interleaved order as the canonical lookup for all name resolution. **Single-resolution invariant:** EXPR_IDENT stores `resolved_sym` (the Symbol it resolved to) and `companion_module` (for struct/union names with a companion module) directly on the AST node. EXPR_FIELD, EXPR_CALL, and `find_callee_symbol` read these stored pointers instead of re-resolving — this is the architectural guarantee that prevents name shadowing bugs where a parameter/local shares a name with a module or type. Performs intraprocedural escape analysis: tags pointer/slice expressions with provenance (stack/heap/static/unknown) and rejects returning stack pointers, freeing non-heap memory, or storing stack pointers in heap structs. Uses `diag_error` (not `diag_fatal`) so multiple type errors are reported in a single compilation; erroneous expressions receive the poison type `TYPE_ERROR` which propagates silently to suppress cascading false positives.
- **`monomorph.c/h`** — Monomorphization table and utilities. Tracks generic instantiations (functions, structs, unions) with their mangled names and concrete types. Three phases run after pass2: (1) `mono_discover_transitive()` finds all transitive instantiations (generic functions calling other generic functions) via a fixpoint AST walk, using `resolved_callee`/`resolved_sym` from pass2 for module-scoped symbol lookup. (2) `mono_finalize_types()` discovers nested struct types referenced only in field types (not directly constructed), resolves all type names via `mono_resolve_type_names()`, and topologically sorts entries so by-value struct dependencies are emitted before their dependents. (3) `mono_resolve_type_names()` canonicalizes generic struct/union names in a type tree to mangled C identifiers, using the global symtab to resolve module-scoped names.
- **`codegen.c/h`** — C code emitter. Walks the typed AST and emits C11 source. Handles typedef generation for slices/options, function declarations, struct/union definitions, and expression emission. Emits monomorphized copies of generic functions/structs using a substitution context (`SubstCtx`) that replaces type variables with concrete types at emit time.
- **`types.c/h`** — Type representation and utilities. Defines `Type` (i8–u64, isize, usize, f32/64, bool, str, cstr, pointer, slice, option, struct, union, function), plus helpers like `type_is_numeric()`, `type_eq()`, `type_name()`.
- **`diag.c/h`** — Diagnostics. `diag_error()` reports an error with source location and continues (used by pass2 for error accumulation); `diag_fatal()` reports and exits immediately (used by lexer/parser for unrecoverable errors). `diag_error_count()` gates progression to later pipeline stages. **No warnings:** FC has exactly one diagnostic severity — error. Don't add a warning channel, a `-W…` family, or an opt-in advisory pass. Style/hygiene checks (unused bindings, shadowed names, unused imports) are deliberately out of scope for the compiler; they belong to the programmer (and possibly to a future LSP/editor integration). A diagnostic is either important enough to fail the build or it isn't emitted. **Server mode:** `diag_set_sink()` redirects diagnostics to a collector (vs. stderr) and `diag_set_abort_jmp()` makes `diag_fatal*` `longjmp` (vs. `exit(1)`) so the in-process LSP survives mid-typing fatals; both default off, leaving the CLI path byte-for-byte unchanged.
- **`common.c/h`** — Shared utilities. Arena allocator, dynamic array macro (`DA_APPEND`).
- **`analyze.c/h`**, **`json.c/h`**, **`lsp.c/h`** — The in-process language server (`fcc --lsp`), independent of the normal compile path. `analyze()` runs lexer→parser→pass1→pass2 over an in-memory source (merging the installed stdlib so `std::` resolves), collecting diagnostics and keeping the typed AST alive for queries; `json.c` is a minimal JSON-RPC value model; `lsp.c` is the stdio message loop + handlers (diagnostics, hover, definition, completion, and a type-above CodeLens). See **Editor integration** below. **Note:** `pass2_check` now takes the AST `Arena*` (the type nodes it synthesizes are AST-referenced and freed with that arena) — this fixed a latent leak and is what lets a long-running server reclaim each analysis.

## Key Language Design Decisions

### Type System
- No explicit type annotations on bindings — always inferred from RHS
- Function parameter types are always required (they anchor inference)
- No function overloading — names always resolve to exactly one function
- Implicit widening only where lossless (e.g., `i32` → `i64`, NOT int → float) — applies in binary expressions, comparisons, function call arguments, slice indices, and for-range endpoints. An `i32` literal like `0` widens to `i64` when compared to `.len` or used as a slice bound.
- Integer-to-float always requires an explicit cast
- No `null` in the language — option types (`T?`) replace nullable values
- **Type aliases are true aliases**: `str` and `cstr` are desugared to their underlying types (`u8[]` and `u8*`) internally with a `const char *alias` field on `Type` for display purposes. They are fully interchangeable with their underlying types — `str` and `u8[]` are the same type, `cstr` and `u8*` are the same type. The alias only affects `type_name()` output in diagnostics, never type equality or semantics. C interop details (e.g., `const char*` for C string functions) are handled only at extern call boundaries in codegen, not in the type system.

### Compilation Model
- **Two-pass**: First pass collects top-level names/types/layouts; second pass type-checks expressions
- Top-level declarations can reference each other regardless of order
- Local bindings resolve left-to-right within function bodies
- **Name resolution order**: local scope → current module members → current module imports → parent members → parent imports → *(repeat for each ancestor)* → file-level imports → global declarations. At each module level, members are checked then imports before moving to the parent (interleaved resolution). A child's import can shadow a parent's member.
- **File-level imports are per-file**: each file's imports are visible only to modules defined in that file, never to other files (even in the same namespace). They sit at the bottom of the import chain, after all module-level imports.
- **Imports must come first**: imports must appear at the top of a file (after any `namespace` declaration) and at the top of a module body, before all other declarations. The compiler enforces this.

### Generated C Patterns
- Signed integer arithmetic uses cast-through-unsigned to define overflow: `(int32_t) ((uint32_t) a + (uint32_t) b)`
- Shift amounts are masked to avoid C UB: `a << (b & 31)` for 32-bit types
- Bounds checks emit `abort()` before slice accesses
- Option unwrap (`x!`) emits a tag check before value read
- Struct/union equality emits generated comparison functions

### Types and Literals
- Default integer: `i32`; default float: `f64`
- Suffixed literals: `42i8`, `42u64`, `3.14f32`
- Platform-width types: `isize` (signed, `ptrdiff_t`), `usize` (unsigned, `size_t`); suffixes `42isize`, `42usize`
- No implicit widening to/from `isize`/`usize` — explicit casts required
- String types: `str` = `u8[]` (fat pointer), `cstr` = `u8*` (null-terminated, C interop)
- String interpolation: `%spec{expr}` where `expr` is any arbitrary FC expression — e.g., `"sum=%d{x + y}"`, `"len=%d{(i32) buf.len}"`. Format specifiers: `%d`/`%x` (int), `%f` (float, width/precision optional), `%s` (str/cstr). Stack-allocated via `alloca`; use `alloc(s)!` to promote to heap.
- `any*` = opaque pointer (`void*`), cannot be dereferenced

### Control Flow
- `if`, `match`, `loop` are expressions
- `return`, `break`, `continue` are void-typed expressions (enable early-return in expression positions). `return` is the idiomatic way to produce void in an else branch: `if x > 3 then f() else return`
- `loop` produces a value via `break value`; `for` is always void
- `for` has three forms: `for i in 0..n` (range, exclusive end), `for x in slice` (element), `for i, x in slice` (index + element). Loop variable type is inferred from endpoints/slice via widening.
- `match` is exhaustive; wildcard `_` satisfies exhaustiveness
- `defer <expr>` schedules an expression to run at block scope exit (LIFO order). Block-scoped: runs when the enclosing function body, loop/for body, if/else block, match arm, or nested block exits. `return` unwinds all defers to function scope; `break`/`continue` unwind to loop boundary. Each loop iteration gets a fresh defer queue. Return value of deferred expression is discarded.

### Union Syntax
- Variant declarations require `|` before each variant: `| circle(i32)`, not `circle(i32)`
- Variant **construction** requires the union type name: `shape.circle(5)`, `shape.empty`
- Variant **pattern matching** uses bare names: `| circle(r) -> ...`, `| empty -> ...`
- Each variant carries zero or one payload (not multiple — use a struct for compound data)
```fc
union shape =
    | circle(i32)       // one payload
    | rect(point)         // struct payload for compound data
    | empty               // no payload

let s = shape.circle(5)  // construction: qualified
match s with
| circle(r) -> r * r     // pattern: bare name
| rect(p) -> p.x + p.y
| empty -> 0
```

### Function Syntax
- Functions/lambdas do **not** have return type annotations — the return type is always inferred
- `->` introduces the function **body**, never a return type — this applies to all functions including void-returning ones
- Correct: `let f = (x: i32) -> x * 2` or with a block body: `let f = (x: i32) ->\n    x * 2`
- Correct void function: `let greet = (name: str) ->\n    print(name)`
- **Wrong**: `let f = (x: i32) -> i32 = x * 2` — this is not valid FC syntax
- **Wrong**: `let f = (x: i32) -> void` — `void` is not a return type annotation; what follows `->` is the body
- The `->` token introduces the function body (or separates param types in function type syntax like `(i32) -> i32`)

### Bindings
- `let`: immutable binding, not addressable, capturable in closures (by copy)
- `let mut`: mutable binding, addressable (`&x` → `T*`), not capturable in closures
- Both `let` and `let mut` allow field/element mutation — `let` controls reassignability, not content mutation
- Shadowing is allowed (any combination of `let`/`let mut`)
- Self-assignment (`x = x`) is a compile error — it's always a no-op

### Operators
- No compound assignment (`+=`, `-=`, etc.) — only plain `=`
- Bitwise operators bind tighter than comparison (fixes C's `x & mask == 0` wart)
- `!` is postfix option-unwrap AND prefix boolean-not (context-dependent)
- `->` means: pointer field access, function type arrow, OR match arm separator (context-dependent)

### Match Arm Indentation
Match arms (`|` pipes) align with the `match` keyword, **not** indented under it. The lexer's layout pass treats `|` as a same-level delimiter:
```fc
match x with
| some(v) -> use(v)
| none -> fallback()
```

### Naming Conventions (FC code)
- All user-defined names use **lowercase snake_case**: `let my_func`, `struct my_point`, `union my_shape`, `module my_module`
- This applies to struct names, union names, variant names, function names, module names, variable names
- Type keywords are lowercase: `i32`, `f64`, `bool`, `str`
- Test `.fc` files must follow these conventions

## Testing

Tests live in `tests/cases/`, organized into subdirectories by functional category (expressions, bindings, functions, control_flow, structs, unions, pattern_matching, exhaustiveness, equality, casts_widening, options, pointers, slices, strings, memory, modules, closures, generics, type_properties, native_types, extern, const, escape, io). Standard library tests live in `tests/cases/stdlib/` as multi-file tests with `deps` files pointing to `stdlib/*.fc`. Browse the directories to see what's covered.

Each **single-file test** is an `.fc` file optionally paired with:
- `.expected_exit` — expected exit code (0–255). If omitted, the expected exit code is 0.
- `.error` — substring expected in compiler stderr; the test must fail to compile.

Most tests use `assert` (which calls `abort()`, exit code 134) for correctness checks and omit `.expected_exit`, so a passing test simply exits 0.

Each **multi-file test** is a subdirectory containing:
- Multiple `.fc` files (e.g. `main.fc`, `lib.fc`) — all compiled together
- `expected_exit` or `error` (no dot prefix) — the expected result
- `deps` (optional) — one path per line (relative to project root) for external dependencies like `stdlib/io.fc`

Run with `make check` (or `make test-all`). The test runner compiles FC→C with `./fcc`, then C→binary with both `gcc` and `clang` using `-std=c11 -Wall -Werror`. Test names display as `modules/cross_ns_import`, etc. Every test file (including `.error` tests) must have a valid `let main` function — error tests put the bad code inside `main`'s body, not at top level. The generated C is compiled with `-Werror`, so all variables must be used.

### Multi-file tests

Multi-file tests each get their own subdirectory within a category dir, making it clear which files belong together. For example:
```
modules/cross_ns_import/
    main.fc          # file with main, imports from lib
    lib.fc           # file defining the namespace/module
    expected_exit    # expected exit code
```
The test runner discovers all `.fc` files in the subdirectory and compiles them together. If a `deps` file exists, the listed files are also included in compilation. Use `deps` instead of copying stdlib files into test directories — e.g., a `deps` file containing `stdlib/io.fc` for tests that use `std::io`.

### Test coverage philosophy

Every new feature, bug fix, or spec change must include tests covering the happy path, edge cases, error cases, and feature interactions. Aim for thorough coverage — not just one type or one syntax form, but all meaningful combinations. Exit codes are mod 256 — keep expected values under 256.

## Workflow

- During development, use `make test-gcc FILTER=pattern` for fast iteration on relevant tests
- Run `make test-all` to confirm all tests pass (both gcc and clang) before presenting a final summary of changes
- Skip tests for changes that only touch `demos/` or `spec/` (except `spec/examples.fc`) — demo apps and spec documents don't affect compiler tests

## Editor integration

`fcc --lsp` runs an in-process LSP server (JSON-RPC over stdio) — no separate process or runtime. The VSCode extension lives in `editors/vscode/` (plain JS, launches `fcc --lsp` via `vscode-languageclient`); its TextMate grammar is mirrored from `spec/fc.vim`.

- **Pipeline reuse:** `analyze()` (`src/analyze.c`) re-runs lexer→parser→pass1→pass2 per edit (full-document sync) and keeps the typed AST + symbols alive for queries. Features: live diagnostics (severity always Error), hover (`type_name` of the node, plus a doc comment scanned from source — the contiguous `//` run directly above the definition and the `//` trailing a struct/union field; see `extract_doc_comment` in `src/lsp.c`), go-to-definition (top-level/module symbols via `resolved_sym->decl`, module members `mod.member` via `EXPR_FIELD.resolved_member` set in pass2, struct-literal type names, union variant constructors `u.variant` → the union decl, **block-locals — params/`let`s/`for`-vars/match bindings — via `EXPR_IDENT.resolved_local_loc` stamped in pass2 from each binding's `def_loc`, and plain struct fields `s.field` → the field's `StructField.loc`**), completion (keywords + symbols + `.`/`::` members), and a CodeLens showing each `let`'s inferred type on the line above. Field-name hover/definition is now positionally exact for any object shape (`a.b`, `a.b.c`, `s . field`) via `EXPR_FIELD.name_loc` recorded by the parser. The `FindCtx` carries a `def_loc` (drives go-to-def when there's no Symbol) and a separate `doc_loc`/`doc_is_field` (drives the hover doc comment) so a variant constructor can keep go-to-def on the union while its doc reads the variant's own line.
- **stdlib feed:** the installed stdlib (`FCC_STDLIB_DIR`, else the baked-in `$(datadir)/fcc/stdlib`, else `./stdlib`) is merged into every analysis; diagnostics are filtered to the open file. When the open document (or a sibling) *is* a feed file — most commonly because Go To Definition opened a stdlib module — the matching feed entry is dropped so the file isn't analyzed twice (which would trip a pass1 redefinition that silently halts pass2: no diagnostics *and* no hover/CodeLens). The dedup (in `analyze_doc`) keys on **canonical absolute path** (`realpath`, via `canon_path`) **OR byte-identical content**: path catches the same on-disk file however its path was spelled (incl. an open buffer with unsaved edits — the live buffer must win); content catches a *separate identical copy at a different path* (e.g. the repo's `stdlib/data.fc` vs the installed `/usr/local/share/.../data.fc`, whose realpaths differ). The content check is length-gated, so a full `memcmp` runs only for a genuine same-length candidate. Two *different* files that merely share a basename — a project's own `data.fc` vs the stdlib's — match neither key and are correctly kept. (`src/lsp.c` defines `_DEFAULT_SOURCE` to expose `realpath` under `-std=c11`.) **Safety net:** if pass2 is gated anyway (a pass1 error in a merged file the dedup couldn't collapse, e.g. an *edited* divergent copy, or a mid-parse abort) and no diagnostic lands on the open file, `analyze()` surfaces one file-level "analysis incomplete" diagnostic on the open document naming the offending include — so the editor never goes silently blank.
- **Library mode (no entry point):** `pass1_collect` takes a `require_main` flag — `true` for the CLI, `false` for `analyze()`. The server analyzes library code (a stdlib module, a file of helpers) that has no `let main`, so it suppresses the "no entry point" error and the entry-point-file restriction on top-level `let`. The `main` *signature* check (pass2) is unaffected: it only fires when a `main` actually exists. The CLI still requires `main`.
- **Robustness:** the lexer/parser's many `_Noreturn diag_fatal` sites are caught via `setjmp`/`longjmp` (`diag_set_abort_jmp`) so a mid-typing fatal aborts one analysis, not the server. The wire test in `tests/lsp/` asserts this survival.
- **Stale-overlay retention:** type-aware queries (hover, definition, completion, CodeLens) read `query_result(doc)` (`src/lsp.c`), which returns the fresh analysis when it actually type-checked (`AnalysisResult.typed`, set from `pass2_ran`) and otherwise the last one that did, retained on `LspDoc.last_good`. Without this, the two *unrecoverable* states you pass through while typing — a parse abort (`program == NULL`, e.g. `let r2 = ` / a trailing `.`) and a pass1 error gating pass2 (every node's type `NULL`) — blank *all* overlays, so type info flickers off every other keystroke. **Diagnostics deliberately bypass this** (`publish_diagnostics` always uses the fresh `result`), so the squiggle stays live even while overlays are served stale. `result` and `last_good` may alias (when the freshest analysis is the good one); `analyze_doc` retires the previous result with `!=`-guarded frees and `doc_free_results` frees both, so nothing is freed twice (verified clean under ASan across a clean→broken→clean edit cycle). The retained AST carries positions from an earlier revision, so a line you haven't touched still resolves while the line under edit may miss (never a crash) — this drift is the cost that the **error-recovery parser** and **ungating pass2** follow-ups (`spec/TODO.md`) would retire by keeping the *fresh* analysis usable.
- **Per-analysis leak closed (was ~13KB/keystroke):** the long-running server now reclaims everything each analysis. pass1's *referenced* Type nodes and generic `type_params` arrays are arena-allocated (`intern->arena`, which is the AST arena freed by `analysis_free`) — see `arena_dup_names` and the `arena_alloc(intern->arena, …)` sites in `src/pass1.c`. pass2's self-recursion placeholder is arena-backed, and the per-lambda `LambdaCtx.returns`/`entries` scratch arrays are freed (captures are arena-copied into the AST). Verified clean: a 40-edit ASan session importing `std::` reports zero leaks. (CLI behavior is byte-for-byte unchanged — it just frees the arena at exit instead of relying on process teardown.)
- **Open items / known limits:** the single source of truth is `spec/TODO.md` → "Editor / LSP server" (resolved items move to `spec/hist/archived-todos.md`). Don't re-enumerate them here or in the VSCode README.

## Spec Reference

The `spec/` folder contains:
- **`fc-spec.html`** — Full language specification. Self-contained HTML with embedded markdown rendered by `marked.js`. Open in a browser to read.
- **`examples.fc`** — Runnable quick reference demonstrating all core syntax and semantics. Read this first for a fast overview of the language.
- **`grammar.bnf`** — BNF grammar for the language syntax.
- **`TODO.md`** — Outstanding spec/compiler tasks.
- **`hist/`** — Historical design artifacts and analysis documents.

Spec sections are organized as:
- Part 1 — Foundations (types, literals, operators, let/mut, inference)
- Part 2 — Control flow (if, match, loop, for)
- Part 3 — Functions (lambdas, closures, capture)
- Part 4 — Type system (structs, unions, options, pointers, slices, function types)
- Part 5 — Generics (type variables, monomorphization)
- Part 6 — Program structure (modules, namespaces, imports, conditional compilation)
- Part 7 — Memory management (alloc, free, stack/heap)
- Part 8 — C interop (extern, any*, variadics)
- Part 9 — Standard library (std::io, std::sys, std::math, std::text, std::net, std::data)
