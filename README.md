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

## Testing

```sh
make test               # build (if needed) and run all tests
make test-parallel      # build (if needed) and run all tests in parallel
```

Tests live in `tests/cases/`. Each test is a `.fc` file paired with one of:

- **`.expected_exit`** — expected exit code (e.g., `42`)
- **`.expected`** — expected stdout output (diff-compared)
- **`.error`** — expected compiler error message (substring match)

The test runner (`tests/run_tests.sh`) compiles each `.fc` file to C, compiles the C with `-Wall -Werror`, runs the binary, and checks the result. All intermediate files (generated C, binaries) go into a system temp directory that is automatically cleaned up on exit. Test names are prefixed by milestone (e.g., `m3_struct.fc`).

## Repository Layout

- **`src/`** — The compiler, written in C11. Pipeline: lexer → parser → pass1 (declaration collection) → pass2 (type checking) → monomorphization → codegen (C11 emission).
- **`stdlib/`** — Standard library modules (`std::io`, `std::sys`, etc.), written in FC.
- **`spec/`** — Language specification (`fc-spec.html`, best viewed in a browser), formal grammar, and compiler roadmap.
- **`tests/cases/`** — Integration tests organized by functional areas.

For code examples, see the full language specification in `spec/fc-spec.html`.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
