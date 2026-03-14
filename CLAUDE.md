# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

This is an early-stage compiler project for **FC** (version 0.5 draft), a systems programming language that transpiles to C11. The compiler (`./fc`) is implemented in C and lives in `src/`. The language specification is in `spec/fc-spec.html`.

## Build & Run

- **`make`** — Build the compiler (produces `./fc` in the project root)
- **`make test`** — Build and run all tests
- **`make clean`** — Remove build artifacts
- **`./fc input.fc -o output.c`** — Compile a single FC file to C

The compiler is built with `cc -std=c11 -Wall -Wextra -Wpedantic -g`. Tests compile the generated C with `cc -std=c11 -Wall -Werror`, so the emitted C must be warning-clean.

## Git Workflow

- **Never auto-commit** — only commit when explicitly asked
- When asked to commit, always run `git diff HEAD` (or `git diff --cached` if already staged) and base the commit message **only on what the diff shows**. Do not rely on conversation memory — changes may have been made and reverted during the session. Also check `git log --oneline -3` to match the repository's commit message style
- Default branch is `main`; experimental work may happen in other branches — always check the current branch before acting
- Working changes may be discarded and branches may be abandoned — follow the user's lead
- No `Co-Authored-By` attribution in commit messages

## Language Overview

FC is a C-targeting language with these core design constraints:
- **Target**: C11 (using `int8_t`/etc. from `<stdint.h>`, `_Static_assert`, anonymous unions)
- **Memory model**: Manual, no GC, no borrow checker — follows C's philosophy
- **Syntax**: Indentation-based (offside rule, spaces only — tabs are a compile error)
- **Type inference**: Directional (bottom-up, inside-out), never global unification
- **Generics**: Monomorphized at compile time, zero runtime cost

## Compiler Architecture

The compiler pipeline is: **source → lexer → parser → pass1 → pass2 → codegen → C file**.

### Source files (`src/`)

- **`main.c`** — Entry point. Reads input file, runs the pipeline, writes output.
- **`lexer.c/h`** — Tokenizer. Scans source into tokens, then runs a layout pass that converts indentation into `INDENT`/`DEDENT`/`NEWLINE` tokens (offside rule). Also handles same-level match blocks where `|` acts as a delimiter.
- **`token.c/h`** — Token types and interning. All identifier/keyword strings are interned for pointer-equality comparison.
- **`parser.c/h`** — Pratt parser. Produces an AST of `Expr` and `Decl` nodes. Handles expressions (12 precedence levels), statements, patterns, and declarations.
- **`ast.h`** — AST node definitions. `Expr` (expressions, including `let` bindings), `Pattern` (match patterns), `Decl` (top-level declarations), `Program` (root).
- **`pass1.c/h`** — First pass. Walks declarations to collect top-level names, struct/union layouts, and function signatures into a `SymbolTable`. Enables forward references.
- **`pass2.c/h`** — Second pass (type checker). Walks expressions with a scope chain, infers types bottom-up, resolves identifiers, checks type compatibility, validates casts and widening. Assigns unique codegen names to local bindings for shadowing support.
- **`codegen.c/h`** — C code emitter. Walks the typed AST and emits C11 source. Handles typedef generation for slices/options, function declarations, struct/union definitions, and expression emission.
- **`types.c/h`** — Type representation and utilities. Defines `Type` (int8–uint64, float32/64, bool, str, cstr, pointer, slice, option, struct, union, function), plus helpers like `type_is_numeric()`, `type_eq()`, `type_name()`.
- **`diag.c/h`** — Diagnostics. `diag_fatal()` prints an error with source location and exits.
- **`common.c/h`** — Shared utilities. Arena allocator, dynamic array macro (`DA_APPEND`).

## Key Language Design Decisions

### Type System
- No explicit type annotations on bindings — always inferred from RHS
- Function parameter types are always required (they anchor inference)
- No function overloading — names always resolve to exactly one function
- Implicit widening only where lossless (e.g., `int32` → `int64`, NOT int → float)
- Integer-to-float always requires an explicit cast
- No `null` in the language — option types (`T?`) replace nullable values

### Compilation Model
- **Two-pass**: First pass collects top-level names/types/layouts; second pass type-checks expressions
- Top-level declarations can reference each other regardless of order
- Local bindings resolve left-to-right within function bodies

### Generated C Patterns
- Signed integer arithmetic uses cast-through-unsigned to define overflow: `(int32_t)((uint32_t)a + (uint32_t)b)`
- Shift amounts are masked to avoid C UB: `a << (b & 31)` for 32-bit types
- Bounds checks emit `abort()` before slice accesses
- Option unwrap (`x!`) emits a tag check before value read
- Struct/union equality emits generated comparison functions

### Types and Literals
- Default integer: `int32`; default float: `float64`
- Suffixed literals: `42i8`, `42u64`, `3.14f32`
- String types: `str` = `uint8[]` (fat pointer), `cstr` = `uint8*` (null-terminated, C interop), `str32` = `uint32[]`
- `any*` = opaque pointer (`void*`), cannot be dereferenced

### Control Flow
- `if`, `match`, `loop` are expressions
- `return`, `break`, `continue` are void-typed expressions (enable early-return in expression positions)
- `loop` produces a value via `break value`; `for` is always void
- `match` is exhaustive; wildcard `_` satisfies exhaustiveness

### Bindings
- `let`: immutable binding, not addressable, capturable in closures (by copy)
- `let mut`: mutable binding, addressable (`&x` → `T*`), not capturable in closures
- Both `let` and `let mut` allow field/element mutation — `let` controls reassignability, not content mutation
- Shadowing is allowed (any combination of `let`/`let mut`)

### Operators
- No compound assignment (`+=`, `-=`, etc.) — only plain `=`
- Bitwise operators bind tighter than comparison (fixes C's `x & mask == 0` wart)
- `!` is postfix option-unwrap AND prefix boolean-not (context-dependent)
- `->` means: pointer field access, function type arrow, OR match arm separator (context-dependent)

### Naming Conventions (FC code)
- All user-defined names use **lowercase snake_case**: `let my_func`, `struct my_point`, `union my_shape`, `module my_module`
- This applies to struct names, union names, variant names, function names, module names, variable names
- Type keywords are lowercase: `int32`, `float64`, `bool`, `str`
- Test `.fc` files must follow these conventions

## Testing

Tests live in `tests/cases/`. Each test is an `.fc` file plus one of:
- `.expected_exit` — expected exit code (0–255); the test compiles and runs
- `.error` — substring expected in compiler stderr; the test must fail to compile

Run with `make test`. The test runner (`tests/run_tests.sh`) compiles FC→C with `./fc`, then C→binary with `cc -std=c11 -Wall -Werror`.

### Naming convention

Tests are prefixed by milestone: `m1_` (expressions/operators), `m2_` (control flow/functions), `m3_` (structs/unions/match), `m4_` (types: options, slices, pointers, casts, widening), `m5_` (memory: alloc, free, sizeof, default), `m6_` (modules, imports, namespaces, multi-file).

### Multi-file tests

For tests that compile multiple `.fc` files together, use a `_part2.fc` suffix for companion files. The test runner automatically pairs `foo.fc` with `foo_part2.fc` when both exist. Only the base file should have `.expected_exit` or `.error`.

### Test coverage philosophy

Strive for near-complete code coverage. Every feature described in the spec must have corresponding tests. When adding a feature or fixing a bug, add tests that exercise:
- The happy path (feature works as specified)
- All syntax forms and variations (e.g., every import form the spec defines)
- Edge cases and boundary conditions
- Error cases (invalid input produces a clear compile error)
- Interactions between features (e.g., shadowing + mutability, match + options + unions)

Each milestone must fully implement all spec features for that area — no deferring aspects to later milestones.

Exit codes are mod 256 — keep expected values under 256 to avoid confusion.

## Spec Reference

The `spec/` folder contains:
- **`fc-spec.html`** — Full language specification. Self-contained HTML with embedded markdown rendered by `marked.js`. Open in a browser to read.
- **`grammar.bnf`** — BNF grammar for the language syntax.
- **`fc-compiler-plan.md`** — Milestone-based compiler implementation roadmap (M1–M10).
- **`TODO.md`** — Outstanding spec/compiler tasks.

Spec sections are organized as:
- Part 1 — Foundations (types, literals, operators, let/mut, inference)
- Part 2 — Control flow (if, match, loop, for)
- Part 3 — Functions (lambdas, closures, capture)
- Part 4 — Type system (structs, unions, options, pointers, slices, function types)
- Part 5 — Memory management (alloc, free, stack/heap)
- Part 6 — I/O, system & formatting (std::io module, std::sys module, print/eprint/fprint/sprint)
- Part 7 — Program structure (modules, namespaces, imports, conditional compilation)
- Part 8 — C interop (extern, any*, variadics)
- Part 9 — Generics (type variables, monomorphization)
