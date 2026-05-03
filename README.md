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

**FC 1.0.0.** The compiler implements all features described in the language specification, validated by 1250+ tests passing on both gcc and clang across Linux and Windows (MSYS2/UCRT64).

The strongest evidence that the language is real, not just self-tested, is **[wolf-fc](https://github.com/stephen-swensen/wolf-fc)** — a ~14,000-line port of id Software's *Wolfenstein 3D* written entirely in FC. It exercises a meaningful slice of the language end-to-end: a real game loop, SDL bindings via `extern`, manual `alloc`/`free` with `defer`, modules and namespaces, structs and unions, slices, options, string interpolation, closures, and five of the seven stdlib modules (`io`, `sys`, `math`, `random`, `text`). If FC could not handle a non-trivial program, wolf-fc would not run; it does, on both Linux and Windows.

Smaller standalone programs in [`demos/`](demos/) cover the rest of the surface:

- **`fasteroids`** (~1300 lines) — vector-style Asteroids clone, SDL2.
- **`face-invaders`** (~1500 lines) — Space Invaders clone, SDL2.
- **`fibbles`** (~640 lines) — Snake/Nibbles clone, SDL2 graphics + audio.
- **`fing`** (~160 lines) — `ping` clone, exercises `std::net` (raw ICMP).
- **`furl`** (~220 lines) — `curl`-style HTTP client, exercises `std::net` (TCP).

`fing` and `furl` cover `std::net`, which wolf-fc doesn't touch. The only stdlib module not exercised by a demo or shipping program is `std::data`, which has its own coverage in `tests/cases/stdlib/`.

## Building

Requires a C11 compiler (GCC, Clang, etc.).

```sh
make              # build the fcc compiler (release, -O2)
make dev          # clean rebuild at -O0 for clearer diagnostics during development
make clean        # remove build artifacts
```

This produces the `fcc` binary at `./build/<os>/fcc` (where `<os>` is `linux`, `windows`, or `macos`; `fcc.exe` on Windows). The per-OS subdirectory lets a shared source tree across two operating systems — e.g. WSL Linux + MSYS2 on the same Windows box accessing the WSL filesystem via `\\wsl.localhost\...` — hold both binaries without one stomping the other. `make print-bin` echoes the path for scripts.

`make` defaults to `-O2`; override with `OPT=` (e.g. `make OPT=-O0` or `make OPT="-O0 -fsanitize=address,undefined"`). `make clean` is required when switching `OPT` values since Make doesn't track CFLAGS changes.

## Installing

`fcc` follows the GNU coding-standards install conventions:

```sh
sudo make install                       # installs to /usr/local by default
make install PREFIX=$HOME/.local        # user-local install (no sudo)
make uninstall                          # remove
```

The default install layout (with `PREFIX=/usr/local`):

```
/usr/local/bin/fcc                      # the compiler
/usr/local/share/fcc/stdlib/*.fc        # the standard library
```

`PREFIX`, `DESTDIR`, `bindir`, and `datadir` are all overridable per the GNU conventions, so distro/package builds (`PREFIX=/usr DESTDIR=/build/staging make install`) work out of the box.

> **Caveat:** until `fcc` grows automatic stdlib path resolution, you currently need to pass stdlib files explicitly on the command line (e.g. `fcc /usr/local/share/fcc/stdlib/*.fc your-program.fc`).

## Usage

After `make install`, `fcc` is on `$PATH`:

```sh
fcc input.fc                # compile to input.c
fcc input.fc -o output.c    # compile to a specific output file
fcc --version               # or -V — print version and build info
```

The compiler transpiles `.fc` source to a `.c` file. To build and run the result:

```sh
fcc hello.fc -o hello.c
cc -std=c11 -o hello hello.c
./hello
```

From the source tree before installing, run the just-built binary with `$(make -s print-bin) input.fc`, or use `./run.sh` to compile and execute in one shot (see Quick Run below).

### Version output

`fcc --version` (and the short form `fcc -V`) prints three lines: a SemVer-prefixed identifier with the commit hash and commit date, the auto-detected target triple, and the build environment.

```
fcc 1.0.0 (abcdef012345 26.05.03)
Target: linux x86_64 gnu
Built: 2026-05-03 with -O2 (cc 13.3.0)
```

The `1.0.0` prefix is hand-maintained in the `VERSION` file at the repo root. Everything in the parentheses is derived at build time from `git`: a 12-character commit hash, the commit date in UTC `yy.mm.dd` form, and a `-dirty` suffix when the working tree has uncommitted changes (the resulting binary is intentionally not commit-stable in that case). Outside a git checkout (tarball builds) the parenthetical falls back to `(nogit unknown)`.

## Quick Run

`run.sh` compiles an FC file, links it with the standard library, runs the binary, and prints the exit code:

```sh
./run.sh hello.fc                       # compile + run
./run.sh --flag debug hello.fc          # compile with a flag enabled
./run.sh main.fc lib.fc                 # compile multiple files together
```

## Testing

```sh
make check                          # run full test suite (alias of test-all)
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
