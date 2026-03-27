# FC

FC is a systems programming language that transpiles to C11. It aims to combine modern language ergonomics with C's performance and memory model — no garbage collector, no borrow checker, just direct control.

## Key Features

- **C11 target** — generates portable C using `<stdint.h>` types and `_Static_assert`
- **Indentation-based syntax** — offside rule, spaces only
- **Type inference** — directional (bottom-up, inside-out), no global unification
- **Monomorphized generics** — zero runtime cost
- **Manual memory management** — follows C's philosophy
- **No null** — option types (`T?`) replace nullable values
- **Expressions everywhere** — `if`, `match`, and `loop` produce values

## Status

The compiler is under active development.

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
make test               # build (if needed) and run all tests
make test-parallel      # build (if needed) and run all tests in parallel
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
- **`spec/`** — Language specification (`fc-spec.html`, best viewed in a browser), formal grammar, and compiler roadmap.
- **`tests/cases/`** — Integration tests organized by functional areas.

For code examples, see the full language specification in `spec/fc-spec.html`.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
