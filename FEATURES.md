# FC Features

A complete inventory of the language and its tooling. For the rationale behind any of it — and for code examples — see the language specification in [`spec/fc-spec.html`](spec/fc-spec.html), best viewed in a browser. [`spec/examples.fc`](spec/examples.fc) is a runnable quick reference covering the core syntax in one file.

## Syntax & lexical structure

- **Offside rule** — indentation defines blocks; spaces only, tabs are an error. Brackets suppress layout; `;` sequences expressions inline
- **Comments** — `//` line, `/* */` block (nestable)
- **String interpolation** — `"sum=%d{x + y}"` with `%d`/`%x`/`%f`/`%s` over arbitrary expressions; stack-allocated by default, `alloc(s)!` promotes to the heap
- **Naming** — lowercase `snake_case` throughout; no sigils on ordinary names

## Types

- **Fixed-width primitives** — `i8`–`i64`, `u8`–`u64`, `f32`/`f64`, `bool`, `char` (a `u8` with character-literal syntax), plus pointer-width `isize`/`usize`
- **Structs** — named fields, struct literals, structural equality
- **Tagged unions** — one payload per variant, exhaustively matched
- **Enums** — closed sets over an integer representation; `enum_of` is the only checked way in
- **Tuples** — anonymous products with indexing, destructuring, and pattern matching
- **Options** — `T?` replaces null; `some`/`none`, `.is_some`, checked unwrap `x!`
- **Results** — `T!` for fallible operations, `ok`/`err`, propagation with `x?`, `void!` for the payload-less case, and named `error` groups with stable codes. Results cannot be silently discarded
- **Slices & strings** — `T[]` fat pointers with `.len`/`.ptr`; `str` = `u8[]`, `cstr` = `u8*` for C interop
- **Fixed-size inline arrays** — `T[N]` for struct layout and C interop
- **Pointers** — `T*`, `const T*`, address-of, pointer arithmetic, `any*` (opaque `void*`)
- **Function types** — `(i32) -> i32`; functions and closures are first-class values
- **Type-associated modules** — a module may share a type's name and act as its namespace of operations
- **Static type properties** — `i32.min`, `u8.bits`, `f64.epsilon`/`nan`/`inf`, `sizeof`, `alignof`, `bitcast`, `default`

## Bindings & inference

- **Directional inference** — bottom-up and inside-out, never global unification. Bindings never carry type annotations; function parameters always do
- **`let` / `let mut`** — immutability governs reassignment, not content; shadowing is allowed; `let … in` is the expression form
- **Struct destructuring** in both bindings and patterns
- **Implicit widening only where lossless** — `i32` → `i64` yes, int → float never
- **Module constants are read-only** — a module-level non-function `let` is deeply frozen and lands in `.rodata`

## Control flow

- **`if`, `match`, and `loop` are expressions** — `loop` yields a value via `break value`
- **`for`** in three forms — `for i in 0..n`, `for x in slice`, `for i, x in slice`
- **Exhaustive pattern matching** — wildcards, nested patterns, or-patterns, `when` guards, struct destructuring
- **`defer`** — block-scoped, LIFO, correctly unwound by `return`/`break`/`continue`
- **Early return** — `return`, `break`, and `continue` are void-typed expressions usable in expression position

## Functions & closures

- **Return types are always inferred** — `->` introduces the body, never a return type
- **No overloading** — a name resolves to exactly one function
- **Closures capture by copy** of immutable bindings; `alloc` promotes a capturing closure to the heap so it can outlive its creator

## Generics

- **Monomorphized at compile time** — zero runtime cost, no boxing, no dictionaries
- **Type variables** (`'a`) on functions, structs, and unions, with implicit or explicit type arguments
- **Const parameters** (`'n`) — compile-time integer arguments that parameterize layout, e.g. `wide<256>`
- **`static_assert(cond, "msg")`** — per-instantiation predicates in type and function bodies
- **No traits or interfaces** — constraints are built-in (numeric, ordering, bitwise) and checked at instantiation; user-defined constraints are just passed functions

## Memory & runtime safety

- **Manual memory** — `alloc` returns an option, `free` releases; no GC, no borrow checker, no hidden allocation
- **Explicit stack allocation** — `alloca` and bounded forms like `(cstr[N])` make stack cost visible at the call site
- **Escape analysis** — returning a stack pointer, freeing non-heap memory, or storing a stack pointer into the heap are compile errors
- **Defined behavior by default** — bounds-checked slice access, divide-by-zero checks, float→int saturation, wrapping signed overflow, masked shift amounts, deterministic left-to-right evaluation
- **Two orthogonal opt-out axes** — `unguarded`/`guarded` toggles precondition checks; `checked`/`unchecked` toggles overflow detection. A marker that would change nothing is a compile error
- **`assert`** and single-line abort diagnostics on any runtime failure
- **Atomics** — `atomic_load_acquire` / `atomic_store_release` through plain pointers, for sharing memory with C-created threads

## Program structure

- **Modules and namespaces** — arbitrary nesting, `private` members, explicit imports that must appear first, and a well-defined name-resolution order
- **Whole-program compilation** — all sources compile together into one self-contained C translation unit; declaration order never matters
- **Conditional compilation** — `#if` / `#else if` / `#else` / `#end` over `--flag` names and auto-detected `os`/`arch`/`env`, resolved at the token level

## C interop

- **`extern` functions, structs, unions, and constants** bound to real headers via `from <header.h>`
- **Automatic boundary casts**, variadic externs, C enum mapping, feature-test macro defines, and `extern … as` for reserved C spellings
- **Error protocols** — map a C function's failure convention onto `T!` at the declaration site
- **`any*`** for opaque handles, plus `str` ⇄ `cstr` conversions with explicit ownership

## Standard library

Eight modules, written in FC ([`stdlib/`](stdlib/)):

- **`std::io`** — files, directories, stdin/stdout/stderr, read/write/seek
- **`std::sys`** — environment, exit, time, sleep, pid, temp/home directories
- **`std::math`** — the usual `<math.h>` surface plus `min`/`max`/`is_nan`/`is_finite`
- **`std::text`** — strict whole-string parsing to `T!`, search, trim, case, split/join
- **`std::data`** — `array_list`, `linked_list`, `hash_dict`, `hash_set` (generic, reference-semantics handles) and a `slice` module of `map`/`filter`/`fold`-style operations
- **`std::net`** — TCP, UDP, and raw ICMP sockets; address construction and DNS resolution
- **`std::random`** — LCG and PCG generators
- **`std::wideint`** — const-generic `uwide<'n>` / `iwide<'n>` fixed-width big integers from 128 to 4096 bits

## Tooling

- **`fcc`** — the compiler: multiple inputs, `-o`, `--flag name[=value]`, `--no-auto-detect`, `--version`/`-V`
- **Response files** — `fcc @project.rsp` with recursive includes, comments, globs, and file-relative path resolution, so a project file is relocatable
- **Diagnostics with exactly one severity: error** — no warnings, no `-W…` family. Errors accumulate (a poison type suppresses cascading noise) so one run reports as many as it can
- **`--backtraces`** — opt-in FC-level stack traces on abort, with no linker flags and zero cost when off
- **`.errcodes`** — a name/code map for `error` declarations emitted alongside every build
- **`fcc --lsp`** — an in-process language server: project-wide live diagnostics, hover with doc comments, go-to-definition, scope-aware completion, and inferred-type inlay hints or CodeLens. A project pins its own compilation unit with an `lsp.rsp` file
- **Editor support** — a VSCode extension in [`editors/vscode/`](editors/vscode/) (`make install-vscode`) and a Vim syntax file at [`spec/fc.vim`](spec/fc.vim)
- **`run.sh`** — compile, link with the stdlib, run, and report the exit code in one shot
- **Test runner** — `make check` builds every case with both gcc and clang under `-Wall -Werror`; the emitted C is warning-clean by contract

## Platform contract

- **Requires a GCC-compatible C11 compiler** (GCC, Clang with the GNU driver, MinGW-w64, or a GCC-derived cross-compiler). MSVC is not supported
- **Runtime dependency is six libc symbols** — `malloc`, `free`, `abort`, `memcmp`, `strlen`, `snprintf` — which covers desktop, mobile, and libc-bearing embedded targets (Cortex-M, RISC-V, ESP32, AVR)
- **Emitted C is int-width-agnostic**, so 16-bit-`int` targets are in scope
