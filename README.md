# FC

FC is a systems programming language that transpiles to C11. It aims to combine modern language ergonomics with C's performance and memory model — no garbage collector, no borrow checker, just direct control.

https://fc-lang.dev

## Development Transparency

FC was developed with heavy AI assistance — primarily **Anthropic's Claude Opus 4.6** (and earlier Opus/Sonnet models) driven through **[Claude Code](https://claude.com/claude-code)**. The human author (Stephen Swensen) designed the language, authored the specification, set architectural direction, reviewed every change, drove design decisions, and took responsibility for correctness, style, and the licensing posture. The AI generated the bulk of the C compiler implementation, standard library, test suite, and specification prose under that direction.

This is disclosed up front because FC is also intended as a demonstration of what a well-directed human/AI collaboration can produce on a non-trivial language-design and compiler-construction task, and because readers evaluating the compiler — or considering FC as a reference for their own language work — deserve to know how it was built.

## Key Features

- **C11 target** — generates portable C using `<stdint.h>` types and `_Static_assert`
- **Indentation-based syntax** — offside rule, spaces only
- **Type inference** — directional (bottom-up, inside-out), no global unification
- **Monomorphized generics** — zero runtime cost
- **Manual memory management** — follows C's philosophy
- **No null** — option types (`T?`) replace nullable values
- **Expressions everywhere** — `if`, `match`, and `loop` produce values

## Status

The compiler implements all features described in the language specification (v1.0-draft). The spec and implementation are validated by a comprehensive test suite.

## Building

Requires a C11 compiler (GCC, Clang, etc.).

```sh
make          # build the fc compiler
make clean    # remove build artifacts
```

This produces the `fc` binary in the project root.

## Usage

```sh
./fc input.fc                # compile to input.c
./fc input.fc -o output.c    # compile to a specific output file
```

The compiler transpiles `.fc` source to a `.c` file. To build and run the result:

```sh
./fc hello.fc -o hello.c
cc -std=c11 -o hello hello.c
./hello
```

## Quick Run

`run.sh` compiles an FC file, links it with the standard library, runs the binary, and prints the exit code:

```sh
./run.sh hello.fc                       # compile + run
./run.sh --flag debug hello.fc          # compile with a flag enabled
./run.sh main.fc lib.fc                 # compile multiple files together
```

## Testing

```sh
make test-all                       # run all tests with both gcc and clang
make test-gcc                       # run all tests with gcc
make test-clang                     # run all tests with clang
make test-gcc FILTER=closures       # run only tests matching a pattern
make test-gcc FILTER=stdlib/data    # patterns match against category/test_name
```

Tests live in `tests/cases/`, organized into subdirectories by category (e.g., `expressions/`, `structs/`, `generics/`, `modules/`, etc.).

**Single-file tests** are an `.fc` file optionally paired with:

- **`.expected_exit`** — expected exit code (0–255). If omitted, the expected exit code is 0.
- **`.error`** — expected compiler error message (substring match); the test must fail to compile.

Most tests use `assert` (which calls `abort()`, exit code 134) for correctness checks and omit `.expected_exit`, so a passing test simply exits 0.

**Multi-file tests** use a subdirectory within a category dir, containing multiple `.fc` files plus an optional `expected_exit` or `error` file (no dot prefix), and an optional `deps` file listing external dependencies (one per line, e.g., `stdlib/io.fc`). Use subdirectories for tests that need multiple source files or dependencies.

The test runner (`tests/run_tests.sh`) compiles each FC file to C, compiles the C with `-Wall -Werror`, runs the binary, and checks the result. All intermediate files go into a system temp directory that is automatically cleaned up on exit.

## Repository Layout

- **`src/`** — The compiler, written in C11. Pipeline: lexer → parser → pass1 (declaration collection) → pass2 (type checking) → monomorphization → codegen (C11 emission).
- **`stdlib/`** — Standard library modules (`std::io`, `std::sys`, etc.), written in FC.
- **`spec/`** — Language specification (`fc-spec.html`, best viewed in a browser) and formal grammar.
- **`tests/cases/`** — Integration tests organized by functional areas.

For code examples, see the full language specification in `spec/fc-spec.html`.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
