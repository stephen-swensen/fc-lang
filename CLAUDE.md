# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Status

This is an early-stage compiler project for **FC** (version 0.7 draft), a systems programming language that transpiles to C11. The compiler (`./fc`) is implemented in C and lives in `src/`. The language specification is in `spec/fc-spec.html`.

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
- **Comments**: `//` line comments, `/* */` block comments (nestable). No `#` comments.
- **Type inference**: Directional (bottom-up, inside-out), never global unification
- **Generics**: Monomorphized at compile time, zero runtime cost
- **Completeness over partiality**: Prefer conservative-but-complete restrictions over partial solutions that only cover some cases. A feature that works correctly in all scenarios (even if limited) is better than one that works in common cases but has subtle unsound edge cases. Don't ship a partial fix — either solve the full problem space or keep the restriction until you can.

## Compiler Architecture

The compiler pipeline is: **source → lexer → parser → pass1 → pass2 → mono discovery → codegen → C file**.

### Source files (`src/`)

- **`main.c`** — Entry point. Reads input file, runs the pipeline, writes output.
- **`lexer.c/h`** — Tokenizer. Scans source into tokens, then runs a layout pass that converts indentation into `INDENT`/`DEDENT`/`NEWLINE` tokens (offside rule). Also handles same-level match blocks where `|` acts as a delimiter.
- **`token.c/h`** — Token types and interning. All identifier/keyword strings are interned for pointer-equality comparison.
- **`parser.c/h`** — Pratt parser. Produces an AST of `Expr` and `Decl` nodes. Handles expressions (12 precedence levels), statements, patterns, and declarations.
- **`ast.h`** — AST node definitions. `Expr` (expressions, including `let` bindings), `Pattern` (match patterns), `Decl` (top-level declarations), `Program` (root).
- **`pass1.c/h`** — First pass. Walks declarations to collect top-level names, struct/union layouts, and function signatures into a `SymbolTable`. Enables forward references.
- **`pass2.c/h`** — Second pass (type checker). Walks expressions with a scope chain, infers types bottom-up, resolves identifiers, checks type compatibility, validates casts and widening. Assigns unique codegen names to local bindings for shadowing support. Registers monomorphized generic instances and resolves their concrete types. Performs intraprocedural escape analysis: tags pointer/slice expressions with provenance (stack/heap/static/unknown) and rejects returning stack pointers, freeing non-heap memory, or storing stack pointers in heap structs. Uses `diag_error` (not `diag_fatal`) so multiple type errors are reported in a single compilation; erroneous expressions receive the poison type `TYPE_ERROR` which propagates silently to suppress cascading false positives.
- **`monomorph.c/h`** — Monomorphization table and utilities. Tracks generic instantiations (functions, structs, unions) with their mangled names and concrete types. `mono_discover_transitive()` runs after pass2 to find all transitive instantiations (generic functions calling other generic functions) via a fixpoint AST walk. `mono_resolve_type_names()` resolves nested generic type references (e.g., self-referential struct fields) to mangled C identifiers.
- **`codegen.c/h`** — C code emitter. Walks the typed AST and emits C11 source. Handles typedef generation for slices/options, function declarations, struct/union definitions, and expression emission. Emits monomorphized copies of generic functions/structs using a substitution context (`SubstCtx`) that replaces type variables with concrete types at emit time.
- **`types.c/h`** — Type representation and utilities. Defines `Type` (int8–uint64, isize, usize, float32/64, bool, str, cstr, pointer, slice, option, struct, union, function), plus helpers like `type_is_numeric()`, `type_eq()`, `type_name()`.
- **`diag.c/h`** — Diagnostics. `diag_error()` reports an error with source location and continues (used by pass2 for error accumulation); `diag_fatal()` reports and exits immediately (used by lexer/parser for unrecoverable errors). `diag_error_count()` gates progression to later pipeline stages.
- **`common.c/h`** — Shared utilities. Arena allocator, dynamic array macro (`DA_APPEND`).

## Key Language Design Decisions

### Type System
- No explicit type annotations on bindings — always inferred from RHS
- Function parameter types are always required (they anchor inference)
- No function overloading — names always resolve to exactly one function
- Implicit widening only where lossless (e.g., `int32` → `int64`, NOT int → float)
- Integer-to-float always requires an explicit cast
- No `null` in the language — option types (`T?`) replace nullable values
- **Type aliases are true aliases**: `str`, `cstr`, `str32` are desugared to their underlying types (`uint8[]`, `uint8*`, `uint32[]`) internally with a `const char *alias` field on `Type` for display purposes. They are fully interchangeable with their underlying types — `str` and `uint8[]` are the same type, `cstr` and `uint8*` are the same type. The alias only affects `type_name()` output in diagnostics, never type equality or semantics. C interop details (e.g., `const char*` for C string functions) are handled only at extern call boundaries in codegen, not in the type system.

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
- Platform-width types: `isize` (signed, `ptrdiff_t`), `usize` (unsigned, `size_t`); suffixes `42i`, `42u`
- No implicit widening to/from `isize`/`usize` — explicit casts required
- String types: `str` = `uint8[]` (fat pointer), `cstr` = `uint8*` (null-terminated, C interop), `str32` = `uint32[]`
- `any*` = opaque pointer (`void*`), cannot be dereferenced

### Control Flow
- `if`, `match`, `loop` are expressions
- `return`, `break`, `continue` are void-typed expressions (enable early-return in expression positions)
- `loop` produces a value via `break value`; `for` is always void
- `match` is exhaustive; wildcard `_` satisfies exhaustiveness

### Function Syntax
- Functions/lambdas do **not** have return type annotations — the return type is always inferred
- Correct: `let f = (x: int32) -> x * 2` or with a block body: `let f = (x: int32) ->\n    x * 2`
- **Wrong**: `let f = (x: int32) -> int32 = x * 2` — this is not valid FC syntax
- The `->` token introduces the function body (or separates param types in function type syntax like `(int32) -> int32`)

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
- Type keywords are lowercase: `int32`, `float64`, `bool`, `str`
- Test `.fc` files must follow these conventions

## Testing

Tests live in `tests/cases/`, organized into subdirectories by functional category:

- `expressions/` — arithmetic, bitwise, boolean, comparison, literals, overflow, shifts, div/mod, comments, int range checks
- `bindings/` — shadow, void_bind, let_void, unused_local
- `functions/` — func definitions, recursion, early return, func types, top_level_as_value
- `control_flow/` — if, else, loop, for, break, continue
- `structs/` — struct creation, destructuring, field mutation, omitted fields
- `unions/` — union creation, variants, mixed/no payload
- `pattern_matching/` — match syntax and patterns (struct, union, option, wildcard, complex)
- `exhaustiveness/` — Maranget algorithm (bool, int, option, union, struct, nested, generic)
- `equality/` — structural equality codegen (struct, union, slice, option, string, widen, generic)
- `casts_widening/` — cast, widen (type conversions)
- `options/` — option, unwrap, none, nested options
- `pointers/` — pointer, deref, ptr arithmetic/ordering, addr_of, ptr_to_ptr
- `slices/` — slice, subslice, stack_array, bounds checks
- `strings/` — string, cstr, str interop, interpolation, alloc_str
- `memory/` — alloc, free, sizeof, default, linked_list
- `modules/` — module, import, namespace, private (single-file tests as `.fc` files, multi-file tests as subdirectories)
- `closures/` — capture, lambda, closure semantics, globals
- `generics/` — generic functions/structs/unions, monomorphization, dedup
- `type_properties/` — static type props (int32.min, float64.nan) + typevar props ('a.bits)
- `native_types/` — isize/usize literals, arithmetic, casts, generics, error cases
- `extern/` — extern declarations, conditional compilation (#if/#else/#end), variadic extern calls
- `escape/` — stack escape analysis (return stack ptr/slice, free non-heap, alloc struct with stack fields)
- `io/` — print, io read/write, eprint, stdin/stdout, sys, main_args

Each **single-file test** is an `.fc` file plus one of:
- `.expected_exit` — expected exit code (0–255); the test compiles and runs
- `.error` — substring expected in compiler stderr; the test must fail to compile

Each **multi-file test** is a subdirectory containing:
- Multiple `.fc` files (e.g. `main.fc`, `lib.fc`) — all compiled together
- `expected_exit` or `error` (no dot prefix) — the expected result
- `deps` (optional) — one path per line (relative to project root) for external dependencies like `stdlib/io.fc`

Run with `make test`. The test runner (`tests/run_tests.sh`) compiles FC→C with `./fc`, then C→binary with `cc -std=c11 -Wall -Werror`. Test names display as `modules/cross_ns_import`, etc. Every test file (including `.error` tests) must have a valid `let main` function — error tests put the bad code inside `main`'s body, not at top level. The generated C is compiled with `-Werror`, so all variables must be used.

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

Correctness is imperative in compiler implementation — exhaustive testing is not optional. Every new feature, bug fix, or spec change must be accompanied by a thorough test suite that covers not just the happy path but all meaningful combinations and edge cases.

When adding a feature or fixing a bug, add tests that exercise:
- The happy path (feature works as specified)
- All syntax forms and variations (e.g., every import form the spec defines)
- Edge cases and boundary conditions (empty inputs, single elements, maximum values)
- Error cases (invalid input produces a clear compile error, type mismatches, ordering violations)
- Interactions between features (e.g., shadowing + mutability, match + options + unions, generics + equality, nested types)
- Multiple type combinations (e.g., if a feature works on structs, unions, slices, options, and strings — test all of them, not just one)
- Generic/monomorphized variants of the feature

Aim for dozens of test cases per feature, not a handful. A bug caught by a test during development costs minutes; a bug discovered in user code costs hours.

Exit codes are mod 256 — keep expected values under 256 to avoid confusion.

## Workflow

- Run `make test` to confirm all tests pass before presenting a summary of changes

## Spec Reference

The `spec/` folder contains:
- **`fc-spec.html`** — Full language specification. Self-contained HTML with embedded markdown rendered by `marked.js`. Open in a browser to read.
- **`grammar.bnf`** — BNF grammar for the language syntax.
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
