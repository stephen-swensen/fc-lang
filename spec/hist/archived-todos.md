# Archived TODO Items

Resolved design decisions and implementation history, moved from TODO.md on 2026-03-27.

---

## `when` guards on match arms (implemented 2026-04-18)

Originally: allow a boolean predicate on a pattern, e.g. `| ek_dog when no_dogs -> continue`. The workaround was an outer `if` + separate `match`, or a dedicated arm with no predicate and a follow-up `if` inside the arm — both split logic that would read as a single guarded pattern. Came up in wolf-fc while gating dog spawns behind a CLI flag.

**Resolution:** `when <bool-expr>` is now a legal clause between a match arm's pattern and its `->`. The guard evaluates in the arm's scope so any pattern bindings (including destructured struct fields and variant payloads) are visible. When attached to an or-pattern, the guard applies to the whole pattern (every alternative shares it). Arms with a guard do not contribute to exhaustiveness — the Maranget pattern matrix skips them — so a final wildcard (or otherwise complete unguarded coverage) is required. Because `->` serves double duty as pointer-field access and arm separator, the parser blocks `->` as a top-level infix within a guard expression; pointer-field access inside a guard must be parenthesised (`| p when (p->value) > 0 -> ...`). Codegen switches from the flat if/else chain to a done-flag + `abort()`-on-fallthrough structure whenever any arm has a guard, so a guard-false arm cleanly falls through to the next. `tests/cases/pattern_matching/match_when_*` covers basic guards, destructured bindings, or-pattern guards, fallthrough, option/variant/bool patterns, pointer-field access, non-bool error, body-less error, and exhaustiveness error.

---

## Codegen: emit newlines inside large struct initializers (fixed 2026-04-16)

Originally: `alloc(SomeStruct { field1 = alloc(...)!, field2 = alloc(...)!, ... })!` lowered to one massive C line — every nested `alloc(...)!` expanded to a GCC statement-expression and they concatenated inline into the struct literal. With ~10+ fields the line crossed gcc's column-tracking limit (~4096 cols), causing `note: '-Wmisleading-indentation' is disabled from this point onwards, since column-tracking was disabled due to the size of the code/headers`. Wolf-fc hit this in `build_level` (~17 fields) and silenced it with `-Wno-misleading-indentation`.

**Resolution:** `EXPR_STRUCT_LIT` emission now breaks fields onto their own lines when the literal has 2+ fields, using the standard `indent_level` machinery. The `alloc(struct_lit)` and `alloc(union_variant)` lowerings also wrap the `if (_ap) *_ap = ...;` assignment in explicit braces so multi-line struct literals don't trip clang's `-Wmisleading-indentation` either. Reproducer (17-field struct alloc'd with nested `alloc(int32)!`) dropped from a 3723-col line to 635 cols; all 1134 tests pass on gcc and clang.

---

## `!` boolean-not precedence in nested if/else chains (investigated 2026-04-14)

Originally filed after writing the wolf-fc push-wall code: `else if !pushwall_tiles[idx] then false` inside a deeply-nested if/else chain appeared to parse incorrectly — the condition seemed to not trigger as expected. The speculation was that `!` might be mis-parsed as postfix option-unwrap on the slice-index expression, or that there was a precedence interaction with `else if`.

**Resolution:** could not reproduce. The parser handles all the relevant forms correctly:

- `!arr[i]` parses as `!(arr[i])` — postfix `[i]` binds tighter than prefix `!`, then prefix `!` applies to the indexed result. Works in assertions, if-conditions, and arbitrarily deep else-if chains.
- `!obj.field`, `!ptr->field[i]`, `!fn(x)`, `!!x`, and `!(expr)` all parse as expected.
- Deeply nested `if / else if / else if / ... / else if !arr[i] then ...` chains evaluate the `!arr[i]` branch correctly.

The original symptom was most likely a local indentation or logic mistake in the surrounding push-wall code, not a parser bug. Regression test: `tests/cases/expressions/bool_not_precedence.fc` locks in the correct behavior for slice index, field access, pointer-field chain, call, double-negation, and nested else-if forms.

---

## OR-patterns with bindings (retired 2026-04-14)

Originally listed as a possible v2 extension: or-pattern alternatives are currently binding-free — `| some(x) | none -> x` is rejected. Lifting this would require the same-bindings-at-same-types rule that Rust/OCaml enforce, plus a different codegen strategy than the current `(a || b)` predicate (e.g. per-alternative if-branches that set bindings and `goto` a shared arm body).

**Resolution:** not doing this. The binding-free form already covers the useful cases; the workaround (repeat the arm body, or bind in a single pattern and match further inside) is acceptable for the rare cases where it comes up.

---

## SIMD / vector types (retired 2026-04-10)

Originally listed as an evaluation item: investigate first-class vector types for SIMD operations (e.g., `float32x4`, `int32x8`), likely emitted as GCC/Clang `__attribute__((vector_size(N)))` typedefs that get arithmetic operators for free.

**Resolution:** out of scope for 1.0. GCC and Clang already auto-vectorize many numeric loops, so most FC programs get SIMD "for free" through the C compiler without any language changes. Explicit vector types only pay off when auto-vectorization fails or guaranteed vectorization is required — a narrow enough niche that code needing it can drop to C via `extern` for hand-written intrinsics. The spec's Platform contract section now documents this as a deliberate non-feature, pointing users at the auto-vectorizer and `extern` as the two escape hatches. Worth revisiting only if FC targets performance-critical numeric workloads where the auto-vectorizer's coverage becomes a bottleneck in practice.

---

## Packed structs and bit-level layout control (resolved 2026-04-10)

Originally listed as "Packed structs and bit-level layout control" — add a `packed struct` keyword so FC could emit `__attribute__((packed))` and disallow `&field` to keep `-Werror` clean, plus first-class bit field syntax for hardware register layouts. Use cases: memory-mapped I/O, binary protocol parsing, compact on-disk formats, register maps.

**Resolution:** no native features needed for either packed structs or bit fields. Both are accessible through `extern struct` against a C header that defines the layout, with zero FC compiler changes.

For **packed structs**, the C header declares the type with `__attribute__((packed))`, the FC side mirrors the field layout as an `extern struct`, and every extern-struct operation (construction, field read/write, `default`, `sizeof`, structural equality, by-value and `T*` parameter passing, nesting) works transparently. The C compiler handles the unaligned-access codegen — FC just emits ordinary field references. The single restriction is that taking `&packed_field` triggers `-Waddress-of-packed-member`, which fails under `-Werror`; the workaround is to copy the field to a local first.

For **bit fields**, the same pattern applies with no width annotation on the FC side. FC declares each bit field as its underlying integer type (`unsigned int : 3` → `uint32`), and the C compiler emits the shift/mask for every read and write automatically because it owns the struct definition from the header. Empirically verified against gcc and clang: `default`, designated-initializer construction, field read/write, whole-struct copy, and structural equality all work. Taking `&bit_field` is rejected at C compile time with a clear error (`cannot take address of bit-field 'name'`). As a bonus, the C compiler catches literal writes that don't fit the declared bit width (`field = 9` into a 3-bit field) under `-Werror=overflow`, giving FC free compile-time range checking for constants without FC tracking bit widths at all.

Regression tests:

- `tests/cases/extern/extern_struct_packed/` — packed `tcp_header` with a `_Static_assert` on its 13-byte size; exercises construction, default, field read/write, whole-struct copy, and the copy-to-local workaround for `&field`.
- `tests/cases/extern/extern_bit_field/` — `struct reg` with 1-, 3-, 4-, and 24-bit fields mirrored as plain `uint32`; exercises the same operations plus structural equality across bit-field fields.

The C-interop section of the spec documents both patterns under "Packed extern structs" and "Bit fields."

**Tradeoffs accepted:**

- Users maintain a C header alongside their FC code (a few lines per type).
- No layout cross-checking between the C side and the FC mirror — drift shows up at runtime. Mitigated by `_Static_assert(sizeof(...) == N, ...)` in the header for size checks and explicit range asserts in FC for bit field width expectations. Field reordering is still on the user.
- Runtime (non-literal) values that overflow a bit field still truncate silently, matching C bit field semantics.

In practice, nearly every real use case for packed structs and bit fields already starts from an external definition — network RFCs, file format specs, vendor hardware headers, kernel ABI — so the extern C header is the natural integration point, not an imposition. Native FC syntax for either feature would only be worth revisiting if self-hosted FC code ends up with many such types where the C-header overhead becomes annoying duplication, or if FC pursues goals that disallow C-header dependencies.

---

## Platform flags and cross-platform support model (resolved 2026-04-09)

The original `--flag windows` approach conflated OS with toolchain/environment — MSYS2 UCRT64 and native MSVC both look like "Windows" but use different ABIs. Resolved by introducing a structured taxonomy along three axes (`os`, `arch`, `env`), auto-detected from the host C compiler.

**Resolution:**

- `#if` expression evaluator (commit 5cf85e3) — added `!`, `&&`, `||`, parentheses, and string equality (`flag == "value"`), so flags can carry values via `--flag name=value`. This made the structured taxonomy expressible at the language level.
- Platform auto-detection (`src/platform.c`) — uses compile-time `#ifdef` checks against the C compiler's predefined macros to bake `os`/`arch`/`env` values into the `fc` binary at build time. Initially implemented as a runtime `popen("cc -dM -E ...")` probe; switched to compile-time detection after profiling showed the subprocess cost dominated `fc` startup (~6 ms per invocation, ~10 seconds across the test suite). Defaults the env to `gnu` on Linux (musl detection deferred); sets `gnu` for MinGW Windows; leaves env unset on macOS/FreeBSD where it is implied by the OS.
- User override semantics — `--flag os=...` replaces an auto-detected entry rather than appending alongside it. This gives cross-compilation for free with no separate `--target` flag.
- `--no-auto-detect` CLI option — disables the compiler probe entirely, for reproducible builds and testing.
- Reserved-but-not-enforced built-in names — users can shadow `os`/`arch`/`env` if they want, since the override mechanism works the same way for any name.

**Deferred:**

- Musl detection on Linux. Requires header probing (`#include <features.h>`) and is genuinely tricky because musl defines `__GLIBC__` for compat in some headers. Defer until a real consumer needs to differentiate.
- Freestanding / embedded support (`os=freestanding`). Reserved in the documented taxonomy but not detected, since stdlib currently assumes a hosted environment (`abort`, `malloc`, `alloca`, etc.).
- `--target x86_64-linux-gnu` shorthand. Per-axis `--flag` works fine for cross-compilation; sugar can come later if it proves valuable.
- Native MSVC support is **out of scope**. FC's stdlib is built around the MinGW/MSYS2 environment on Windows, and native MSVC would need separate header bindings, separate detection logic (the `cc -dM -E` probe assumes a GCC/Clang-style toolchain), and a different stdlib implementation in many places. The platform.c detection rules still recognize `_MSC_VER` and would set `env=msvc` if it ever fired, but no stdlib branch uses that value and `env=msvc` is not a documented part of the spec.

---

## Design Decisions (from Active section)

### Platform contract

FC targets **C11 hosted implementations with heap allocation** — specifically, GCC or Clang with a libc that provides heap allocation and standard string/formatting functions. This covers:

- Desktop/server (Linux, macOS, Windows/MinGW)
- Mobile (iOS, Android NDK)
- "Rich" embedded (Raspberry Pi, ESP32 with ESP-IDF, Arduino with avr-libc, ARM Cortex-M with newlib/picolibc)

The common thread: these all provide `malloc`, `free`, `abort`, `memcmp`, `strlen`, `snprintf`, and stack-dynamic allocation (`__builtin_alloca`). That's FC's platform contract.

**Not supported:** C11 freestanding (no libc), static-memory-only bare metal, custom allocator-only environments. Those users are writing C directly — FC adds no value there because too much of the core language (alloc, string interpolation, bounds-check abort) depends on a hosted-like environment.

### Core language headers

The compiler emits C code that depends on a small set of C headers. These are split into two tiers:

**Always emitted** (FC's platform contract — every FC program needs these):
```c
#include <stdint.h>     // FC's fixed-width type system (int8_t, uint64_t, etc.)
#include <stddef.h>     // isize/usize → ptrdiff_t/size_t, NULL
#include <stdbool.h>    // bool
#include <stdlib.h>     // malloc, free, abort
#include <string.h>     // memcmp, strlen
```

**Feature-gated** (emitted only when the program uses the relevant feature):
```c
#include <stdio.h>      // string interpolation (snprintf)
#include <math.h>       // float type properties (NAN, INFINITY)
#include <float.h>      // float type properties (FLT_MAX, DBL_MAX)
```

Note: `inttypes.h` is not needed in emitted C — string interpolation uses `%lld`/`%llu` with `(long long)` casts rather than `PRId64`/`PRIu64` macros.

A pure-integer program with no string interpolation and no floats gets a 5-header preamble. These 5 headers are available on essentially every platform with a libc.

**`alloca` portability:** Replace all `alloca()` calls in codegen with `__builtin_alloca()`. GCC and Clang both support this intrinsic on every target. This eliminates the `<alloca.h>` header, which is not in the C standard and has inconsistent availability across platforms.

### C functions the core language emits

| Function | Used by | Header |
|----------|---------|--------|
| `malloc` | `alloc` expressions | stdlib.h |
| `free` | `free` expressions | stdlib.h |
| `abort` | bounds checks, div-by-zero, option unwrap failure | stdlib.h |
| `memcmp` | structural equality for slices/strings | string.h |
| `strlen` | `(str)cstr` cast (wrap pointer with length) | string.h |
| `__builtin_alloca` | string interpolation buffers, `(cstr)str` cast | (compiler builtin, no header) |
| `snprintf` | string interpolation formatting | stdio.h (feature-gated) |

### Stdlib is just FC files

The FC standard library (`stdlib/`) is ordinary FC source code that uses extern declarations to wrap C functions. It receives **no special compiler treatment**. Users include it by passing the files to the compiler:

```bash
# No stdlib — pure computation, only core headers emitted
./fc main.fc -o out.c

# With I/O — io.fc's `module c from "stdio.h"` pulls in <stdio.h>
./fc main.fc stdlib/io.fc -o out.c

# Full stdlib
./fc main.fc stdlib/io.fc stdlib/sys.fc -o out.c
```

The extern `from_lib` declarations in stdlib files (and any user-written extern modules) drive additional `#include` emissions. If you don't compile a stdlib file, you don't get its headers. This is the intended mechanism, not a workaround.

### Stdlib surface area

The stdlib wraps C APIs organized by portability tier:

**Tier 1 — C11 standard** (works everywhere FC's platform contract holds):
- `io` — fopen, fread, fwrite, fclose, fflush (stdio.h)
- `math` — sin, cos, sqrt, pow, etc. (math.h) — future
- `text` — string manipulation utilities — future

**Tier 2 — POSIX** (Linux, macOS, BSDs, most embedded with newlib/picolibc):
- `sys` — getenv, exit (C11 portion), clock_gettime, nanosleep (POSIX portion)
- Future: `fs` (filesystem operations), `thread` (pthreads)

**Tier 3 — Platform-specific** (user-provided, not part of FC's stdlib):
- Windows APIs, vendor SDKs, hardware-specific libraries — users write their own extern bindings

Tier 1 modules should work on every platform FC targets. Tier 2 modules work on POSIX systems. Users choose what to include by which files they pass to the compiler.

### `define` annotation on extern modules

C libraries sometimes require feature-test macros (`_POSIX_C_SOURCE`, `_GNU_SOURCE`, `_WIN32_WINNT`, etc.) to be `#define`d before their headers are included. Rather than requiring users to know which flags each library needs at the build command level, this information lives with the extern declaration:

```fc
module t from "time.h" define "_POSIX_C_SOURCE" "200809L" =
    extern clock_gettime: (int32, any*) -> int32
    extern nanosleep: (any*, any*?) -> int32
```

Codegen emits `#define _POSIX_C_SOURCE 200809L` before `#include <time.h>`. Rules:
- Multiple modules defining the same macro with the same value: deduplicated silently
- Multiple modules defining the same macro with different values: compile error
- Defines are emitted before all `#include` lines (feature-test macros must precede headers per POSIX spec)

This replaces the current `_POSIX_C_SOURCE` auto-detection hack for `time.h`. The define is co-located with the declaration that needs it — passing `sys.fc` to the compiler brings everything it needs with it.

### Conditional compilation

The `#if`/`#else if`/`#else`/`#end` system and `--flag` CLI option remain for **user-defined** conditional compilation:

```bash
./fc main.fc --flag posix stdlib/io.fc stdlib/sys.fc -o out.c
```
```fc
#if posix
    import sys from std::
    let t = sys.time()
#end
```

The previously planned `--target` flag with automatic `target_embedded` / `target_bare_metal` built-in flags is dropped. The `target_hosted` built-in flag is also dropped — since every FC program targets a hosted environment per the platform contract, the flag is always true and therefore meaningless. Platform-specific behavior is controlled by user-defined flags and by which files are passed to the compiler.

### Implementation changes required — all complete

| Current state | Target state | Status |
|---------------|--------------|--------|
| 10 headers always emitted in preamble | 5 core headers always + feature-gated | Done |
| `alloca()` + `<alloca.h>` | `__builtin_alloca()`, no header | Done |
| `_POSIX_C_SOURCE` auto-detected for `time.h` | `define` annotation on extern module declarations | Done |
| `target_hosted` built-in flag always set | Drop (platform contract makes it redundant) | Done |
| Planned `--target` with `target_embedded`/`target_bare_metal` | Drop entirely | Done (never existed) |
| Stdlib implicitly expected | Stdlib is explicitly passed as source files | Done |

### Standard library namespace and structure

- Standard library lives in a `std::` namespace (distinct from `global::` used by application code)
- Flat module structure — one module per file, no nesting — because FC does not allow cross-file module definition
- Defined modules: `io` (file I/O — see spec §std::io), `sys` (system operations — see spec §std::sys)
- Import pattern: `import io from std::`, `import sys from std::`, etc.
- Resolved: explicit import required per file. No auto-imports — matches FC's "no magic" philosophy.

### Slice/pointer provenance and safety — current situation

All pointer/slice creation paths produce the same types with no provenance information:

| Source | Storage | Safe to return? | Safe to free? |
|--------|---------|-----------------|---------------|
| `alloc(T)`, `alloc(T, N)`, `alloc(expr)` | Heap | Yes | Yes |
| `"hello"`, `c"hello"` | Static read-only | Yes | **No** |
| `&x` on `let mut` | Stack | **No** | **No** |
| `T[N] { }` array literal | Stack | **No** | **No** |
| `"hello \{x}"` interpolation | Stack (`alloca`) | **No** | **No** |
| `(cstr)str_value` cast | Stack (`alloca`) | **No** | **No** |

The `alloc(slice)` deep-copy pattern mitigates this — `alloc(s)!` promotes any stack slice to heap-owned memory. But the programmer must know when to use it.

### `T**` and C interop

FC supports `T**` in the type system — the compiler, parser, and type checker all handle multi-level pointers. The main C use case for double pointers is out-parameters, where a function writes a pointer back through `T**`:

```c
int sqlite3_open(const char *filename, sqlite3 **ppDb);
long strtol(const char *str, char **endptr, int base);
```

**In pure FC**, `T**` out-parameters work directly. However, idiomatic FC rarely needs this pattern because functions return values directly, and option types / struct returns handle the cases where C would use an out-parameter.

**At the C boundary**, `cstr*` (uint8**) is automatically cast to `char**` by the compiler, following the same pattern as the `cstr` → `const char*` cast for single-level pointers.

**Opaque handle out-parameters** (e.g. `sqlite3_open`'s `sqlite3**`) are also handled automatically. Since FC represents opaque C types as `any*`, the out-parameter type is `any**` — which emits as `void**` in C. The compiler handles this by emitting a `(void*)` cast at the extern boundary.

### What we're not doing

- Borrow checker / ownership system (Rust-style) — too complex, conflicts with "manual memory, maps to C" philosophy
- Runtime provenance tagging — conflicts with zero-cost philosophy
- Automatic reference counting — same

### Closures at extern boundaries — wrapper function pattern (future)

Currently, only top-level functions and non-capturing lambdas can be passed to extern C functions. The compiler generates static trampolines (`_ctramp_*`) that strip the `void* _ctx` parameter and call the known function directly with `NULL`. This is conservative but complete — it is correct for all C callback APIs.

The limitation shows up when writing FC stdlib wrappers. A wrapper like `let sort = (arr: int32[], compare: (int32, int32) -> int32) -> ...` receives `compare` as a fat pointer `{fn_ptr, ctx}`. When forwarding to the extern `qsort`, it's rejected because it's a local function value, not a top-level function or literal lambda.

#### Design space explored

C callback APIs fall into three categories:

1. **Synchronous callbacks** (`qsort`, `bsearch`) — callback is invoked before the extern returns. A thread-local stash approach works: stash the fat pointer's `fn_ptr` and `ctx` before the extern call, generate a static trampoline that reads from the stash. Sound because the stash is valid for the duration of the call.

2. **Deferred callbacks with user-data** (`pthread_create`, most event loops) — the C API provides a `void*` parameter that gets passed through to the callback. The context could be threaded through this parameter without a stash. Fully sound but requires different codegen than category 1.

3. **Deferred callbacks without user-data** (`signal`, `atexit`) — no mechanism to carry context. Closures fundamentally cannot work here without runtime code generation. The non-capturing restriction is the only correct answer.

#### Why this is deferred

Implementing only category 1 (stash approach) would be a partial solution — it handles `qsort`-style wrappers but not `pthread_create`-style wrappers. Per the project's completeness principle, a partial solution that covers some APIs but silently breaks on others is worse than a conservative restriction that is uniformly correct.

#### Possible approaches if revisited

- **Thread-local stash for synchronous callbacks**: per-callsite `_Thread_local` fn_ptr/ctx slots with a static trampoline that reads from them.
- **Inferred restriction propagation**: if pass2 detects that a function parameter flows to an extern call, mark that parameter as "must be non-capturing" and enforce at call sites transitively.
- **Explicit `extern` function type annotation**: e.g. `compare: extern (int32, int32) -> int32` to mark a parameter as a bare C function pointer.
- **User-data threading for deferred callbacks**: for APIs like `pthread_create` that provide a `void*` arg, the compiler could pack the fat pointer context into that parameter.

A complete solution would need to handle at least categories 1 and 2 together. Category 3 always keeps the non-capturing restriction.

---

## C interop and embedded: remaining gaps (2026-03-22, updated 2026-03-23)

Overview of what's solved and what's still missing for full C interop and embedded platform support.

**Solved:**
- Extern declarations with automatic boundary casts (cstr → `const char*`, any* → `void*`, cstr* → `char**`, any** → `void*`)
- Variadic extern functions (printf, snprintf, etc.) with C default argument promotions
- `isize`/`usize` for platform-native types (size_t, ptrdiff_t)
- Opaque pointers (`any*`) for C handles (FILE*, sqlite3*, etc.)
- `c"..."` literals for null-terminated strings; `str` ↔ `cstr` casts
- Raw pointer arithmetic as escape hatch from slice overhead
- Function pointer params in extern (non-capturing lambdas extract fn_ptr automatically)
- Extern struct: C struct layout import via `extern struct C_NAME [as fc_name]` in `from` modules
- Extern union: C untagged union import via `extern union C_NAME [as fc_name]` in `from` modules, with memcmp-based equality
- Conditional compilation (`#if`/`#else`/`#end`) with built-in and user-defined flags
- Escape analysis: compile-time detection of returning stack pointers/slices, freeing non-heap memory, storing stack pointers in heap structs
- `const` qualifier for pointer/slice types: deep const, `const cstr` → `const char*` vs `cstr` → `char*` at extern boundaries, string/cstring literals infer const, write/free/address-of rejection through const
- Extern constants: `extern C_NAME [as fc_name]: type` imports C `#define` constants with type validation (scalar/pointer/cstr only) and automatic cstr boundary casts
- Fixed-size inline array fields (`T[N]`) in structs and extern structs

**Deferred gaps (outside FC's platform contract):**

- **No inline assembly** — can't emit platform-specific instructions. For GPIO toggling, interrupt handlers, etc., need a C wrapper file.
- **No `volatile`** — relevant for memory-mapped I/O registers on embedded.
- **No bitfield structs** — C bitfields (`uint32_t flags : 4`) are common in hardware register definitions.

---

## Resolved Items

### Extern constants — `#define` interop (resolved 2026-03-27)

C constants defined as `#define FOO 42` can now be imported using the existing `extern` syntax with a non-function type: `extern FOO: int64`. The mechanism requires no special codegen — the C macro name is emitted directly into the generated code, and the C preprocessor expands it after the `#include`. The `as` clause works for renaming: `extern SDL_INIT_VIDEO as sdl_init_video: int64`.

Type validation rejects types with no C `#define` equivalent: slices, options, structs, unions, void. For `cstr`-typed constants, codegen emits a `(const uint8_t*)` cast to bridge the `char*` / `uint8_t*` signedness difference at the C boundary.

Function-like macros (`#define MAX(a,b) ...`) are code transformations, not constant values, and remain outside the scope of this feature. Users wrap them in C helper functions or reimplement in FC.

### Function pointer trampolines at extern boundaries (resolved 2026-03-22)

FC functions internally carry an extra `void* _ctx` parameter for closure support. When a non-capturing function or lambda is passed to a C extern expecting a plain function pointer, the compiler now generates a static trampoline that drops the `_ctx` and matches the C calling convention. This happens automatically — the programmer just passes the function. Documented in spec §C interop with a `qsort` example.

### `const` qualifier for pointer/slice types (resolved 2026-03-22)

Added `const` as a type qualifier for pointer and slice types. `const` means "can't write through this indirection" — orthogonal to `let`/`let mut` which controls binding reassignment.

#### Design
- **Syntax**: `const` prefix on pointer/slice types only: `const int32*`, `const str`, `const int32[]`
- **Deep const**: accessing pointer/slice fields through a const pointer gives const versions. If `const node*` and node has `next: node*`, then `p->next` is `const node*`. Value fields are simply not writable through const (no type change needed). Chains naturally: `const_ptr->next->next` propagates.
- **Literal inference**: `"hello"` → `const str`, `c"hello"` → `const cstr` (data is in read-only .rodata)
- **Coercion**: non-const → const implicit (safe direction via `type_can_widen`). Const → non-const requires explicit cast: `(str)const_str_value`
- **Casts both directions**: `(const int32*)ptr` to freeze, `(int32*)const_ptr` to strip. Regular cast syntax.
- **Struct fields**: can be declared `const` — `struct config = name: const str`
- **`.ptr` on const slice**: gives `const T*`
- **Write rejection**: assignment through const pointer/slice, address-of through const, free of const — all rejected with `diag_error`
- **Generics**: type variables can bind to const types; `type_substitute` preserves `is_const`; unification allows non-const → const
- **Extern boundaries**: `const cstr` → `const char*`, non-const `cstr` → `char*`
- **Equality**: constness ignored for eq function generation/dedup
- **Orthogonal to provenance**: const = write permission, provenance = storage location

#### What const is NOT
- No `const` at binding sites (FC has no type annotations on `let` — always inferred from RHS)
- No branch widening (if/match branches with `const T*` and `T*` require explicit cast — deferred to future branch widening feature)
- No `const` on bare value types (`const int32` is a parse error)

#### Implementation
- `bool is_const` field added to `struct Type` in `types.h`
- Singleton const types: `type_const_str()`, `type_const_cstr()` for literal inference
- `type_make_const()` helper: shallow-copies pointer/slice with `is_const=true`
- `type_eq()` checks `is_const` for pointer/slice; `type_eq_ignore_const()` for eq dedup
- `type_can_widen()` extended with non-const → const widening and option inner widening
- `TOK_CONST` keyword, `apply_const()` parser helper (handles pointer/slice/option-wrapping)
- `is_write_through_const()` recursive checker in pass2 for assignment targets
- `mangle_type_name()` prefixes `const_` for mangled names
- `emit_type()` emits `const ` prefix on C pointers; same slice typedefs for const/non-const
- 21 new tests in `tests/cases/const/` (13 success, 8 error)
- 614 total tests passing

### Stack escape analysis (resolved 2026-03-22)

Lightweight intraprocedural escape analysis added to pass2. Every expression that produces a pointer or slice type is tagged with a **provenance** (`PROV_UNKNOWN`, `PROV_STACK`, `PROV_HEAP`, `PROV_STATIC`) tracking where its backing storage lives. The analysis catches three classes of bugs at compile time:

1. **Returning stack-derived pointers/slices** — `return &local`, returning array literals, interpolated strings, `(cstr)str` casts, subslices of stack arrays, or any of these through let bindings, if/match branches, blocks, some-wrapping, pointer arithmetic, or pointer casts.
2. **Freeing non-heap memory** — `free("hello")` (static), `free(&x)` (stack), `free(array_lit)` (stack), `free(interp_string)` (stack).
3. **Storing stack pointers in heap structs** — `alloc(node { data = &local })` where a struct field is a stack-derived pointer or slice.

#### Provenance sources
- `PROV_STACK`: `&x` (address-of local), array literals, interpolated strings, `(cstr)str` cast (alloca copy), subslice of stack
- `PROV_HEAP`: all forms of `alloc()` — including `alloc(stack_slice)` which deep-copies to heap
- `PROV_STATIC`: string literals `"..."`, cstring literals `c"..."`
- `PROV_UNKNOWN`: function parameters, call return values, extern results (no interprocedural analysis)

#### Propagation
Provenance flows through: let bindings, identifiers, if/match branches (conservative merge — any STACK branch → STACK), blocks (last expression), some/unwrap, pointer arithmetic, pointer casts, subslices, `.ptr` on slices.

#### Design decisions
- **Intraprocedural only**: function parameters are `PROV_UNKNOWN`. A function receiving a pointer is allowed to return it — the caller is responsible for correctness.
- **Conservative on reassignment**: `let mut` reassignment does not update provenance (tracks initial binding only). This may produce false positives if a stack pointer is later reassigned to heap, but is safe.
- **`alloc(stack_data)` produces PROV_HEAP**: the whole point of `alloc(s)!` is to promote stack data to heap. The provenance of the input is not propagated.
- **Option types checked recursively**: `some(&x)` returns `int32*?` which carries `PROV_STACK` — the option wrapper doesn't hide the provenance.

#### Implementation
- `Provenance` enum and `prov` field added to `Expr` in `ast.h`
- `LocalBinding` extended with `prov` in pass2; `scope_add_prov()` propagates through let bindings
- Three check points in pass2: explicit `return`, implicit return (last expression in function body), `free()`, and `alloc(struct_lit)` fields
- 36 new tests in `tests/cases/escape/` (18 error tests, 18 success tests)
- 593 total tests passing

### Native platform-width types `isize`/`usize` (resolved 2026-03-22)

Added `isize` (signed, pointer-width) and `usize` (unsigned, pointer-width) as opt-in types for C interop and embedded targets. FC's defaults remain fixed-width: `int32` for default integers, `int64` for `sizeof` and slice `.len`. The native types are escape hatches for when exact platform type matching matters.

- **Codegen**: `isize` → `ptrdiff_t`, `usize` → `size_t` (resolved by the C compiler, not FC)
- **Literal suffixes**: `42i` (isize), `42u` (usize) — bare `i`/`u` without width digits
- **No implicit widening**: explicit casts required in both directions between isize/usize and fixed-width types.
- **Type properties**: `.bits`, `.min`, `.max` are platform-dependent (emitted as C expressions)
- **Generics**: type variables can bind to isize/usize; monomorphization works normally
- **Operators**: arithmetic, comparison, bitwise, shifts all work between same-type operands
- 548 tests passing (18 new native_types tests).

### `char32` type and `str32` status (2026-03-24)

FC defines `char` as an alias for `uint8` with character literal syntax (`'a'`, `'\n'`, `'\x41'`). `str32` (alias for `uint32[]`) was partially implemented: it parses and type-checks, but has no runtime support (no `str32` literals, no `str32` interpolation, no `str32`↔`str` conversion). Decision: remove `str32` until Unicode support is properly designed. Removed 2026-03-27.

### Generic type variable soundness — mixed type-var arithmetic (2026-03-24)

#### Problem
Binary operations on different type variables (`'a + 'b`, `'a > 'b`) are currently allowed at template time, but the result type is unsound when widening is involved. During template checking, the type-var early return (pass2.c) picks one operand's type arbitrarily as the result. When instantiated with types that widen (e.g., `'a = int32`, `'b = int64`), the inferred return type (`int32`) doesn't match the actual computed type (`int64`).

#### Proposed fix
Restrict mixed type-var binary operations: require both operands to have the same type variable (e.g., `'a + 'a` ok, `'a + 'b` error at template time, concrete + `'a` ok since it pins `'a`). This is conservative-but-complete.

#### Also noted
`type_name()` for generic function types doesn't show explicit type parameters. Low priority.

### Field access on type variables — structural generics (explored, deferred 2026-03-22)

Explored allowing field access on bare type variables, e.g. `let sum = (p: 'a) -> p.x + p.y`, where `'a` is resolved to a concrete struct at monomorphization.

#### What was prototyped
- A new type kind `TYPE_FIELD_OF(base_type, field_name)` representing "the type of field F of type T", resolved during `type_substitute` when the base type becomes a concrete struct.
- 17 tests passing: basic access, arithmetic, comparison, let binding, nested access (3 levels deep), multiple struct types, chained generic calls, multi-type instantiation.

#### Why it was deferred
- **High complexity relative to value.** Completing it properly would require changes to `unify()`, match exhaustiveness, unary operators, and monomorphization-time error reporting.
- **Incomplete error reporting.** Invalid field access was not caught until C compilation.
- **Trivial workaround exists.** Instead of `let sum = (p: 'a) -> p.x + p.y`, write `let sum = (x: 'a, y: 'a) -> x + y`.
- **Rare in practice.** "Any struct with field X" is more natural in TypeScript or Go than in a C-targeting systems language.

### Multiline struct literals, function calls, and array literals (resolved 2026-03-21)
Bracket depth tracking added to the lexer layout pass. When inside `()`, `[]`, or `{}`, `INDENT`/`DEDENT`/`NEWLINE` tokens are suppressed. Multiline struct literals, function calls with many arguments, and array literals all parse naturally. Trailing commas permitted. No parser changes needed.

### alloc/free/sizeof/default placement — no change (resolved 2026-03-21)
Considered moving to a `sys` module. Resolved by convention: `drop` is the idiomatic name for user cleanup; `free` stays reserved for raw deallocation. Regularizing as generic functions in a module was rejected because `alloc(expr)` can't coexist with `alloc<T>()` under no-overloading, and `sys` would be compiler magic pretending to be a module.

### Unreachable pattern detection — deferred (resolved 2026-03-21)
Low priority since it's a warning, not a correctness issue. The Maranget infrastructure is in place; unreachable arm detection is the dual of exhaustiveness (call `find_witness` against preceding arms). Small addition when needed.

### Module-scoped imports — implemented (2026-03-25)

Imports now follow lexical scoping rules, matching `let` binding semantics. Implemented and reverted once (2026-03-24), then re-implemented with a new architecture (2026-03-25) using ImportTable/ImportRef with live source references instead of the previous dual-registration approach.

**New architecture:** Uses `ImportTable` with `ImportRef` entries that store lightweight references (local_name, source_name, source_members pointer) instead of copying symbol data. Lookups go through the source module's live member table, avoiding the stale NULL-type problem. An `ImportScope` linked list (stack-allocated, pushed/popped on module entry/exit) provides arbitrarily deep nested visibility — child modules inherit parent imports with shadowing.

**Key design points:**
- `import * from M` only exports M's own declarations (`members`), never M's imports (non-forwarding)
- File-level imports are scoped per-file via `FileImportScopes`, preventing cross-file leakage
- Module-level imports are stored in `Symbol.imports`, separate from `members`
- Lookup chain: local scope → module members → import scope chain → global declarations
- Later imports shadow earlier imports (same scope level); inner scopes shadow outer
- On-demand type checking for imported functions uses source_members context switch

### Eager type resolution — not viable (deferred indefinitely, 2026-03-21)
Resolving struct field types and union variant payloads in-place on registered types is blocked by self-referential structs. A struct like `node { next: node*? }` creates a cyclic type graph when its field type is resolved. The existing on-demand `resolve_type()` calls in pass2 and `resolve_struct_stub()` in codegen remain the correct approach.

### Codegen: nested option/slice typedef ordering (resolved 2026-03-20)
- `collect_types_in_type()` now recurses into inner types before adding the outer type to the typeset, ensuring dependency typedefs are emitted first in the generated C.
- Fixed for both option types (`int32??`, `int32???`) and slice types.

### M9: std::sys module, main args as str[], conditional compilation, cstr→str cast (resolved 2026-03-17)
- `std::sys` module (`stdlib/sys.fc`): `env`, `exit`, `time`, `sleep` — pure FC wrapping C stdlib via extern declarations.
- Main function signature changed from `(args: int32)` to `(args: str[])`. Codegen emits `fc_main(fc_slice_fc_str args)` for user code plus a C `main` wrapper that converts `argc`/`argv` to `str[]` via `alloca`.
- Conditional compilation: `#if`/`#else if`/`#else`/`#end` directives implemented as a token-level filter.
- `(str)cstr` cast implemented: wraps existing pointer with `strlen`-derived length (no copy).
- 413 tests passing.

### Implicit widening in generic function calls (resolved 2026-03-15)
- Generic function arguments with concrete parameter types now auto-widen, matching non-generic call behavior.
- Widening only applies to parameters that contain no type variables. Type variable binding via unification still requires exact matches.

### Implicit widening in struct literals and variant constructors (resolved 2026-03-15)
- Struct literal fields and union variant payloads now auto-widen, matching function call argument behavior.

### Structural equality, codegen safety, spec alignment (resolved 2026-03-15)
- Structural `==`/`!=` implemented for all types: structs (field-by-field), unions (tag + payload), slices (element-wise), str (len + memcmp), options (has_value + inner), func (fn_ptr + ctx). Generated `fc_eq_T` comparison functions.
- Codegen safety: signed overflow wrapping via cast-through-unsigned; shift amount masking; integer division/modulo by zero emits `abort()` check.
- Pointer ordering (`<`, `>`, `<=`, `>=`) now accepted.
- Address-of (`&x`) now rejects immutable `let` bindings.
- String literal pattern matching in `match` implemented.
- 326 tests covering all milestones M1–M8.

### File handles are `any*`, not a built-in type (resolved 2026-03-11)
- The `file` built-in type was removed. File handles are `any*` — the same opaque pointer type used for any C resource.
- File operations moved from built-in `file.open`/`file.close`/etc. to `std::io` module.

### Extern declarations, std::io module, print→io.write migration (resolved 2026-03-16)
- `extern` declarations implemented: parse, pass1 registration, pass2 type-checking, codegen.
- `module ... from "lib"` syntax for C library source metadata.
- `stdlib/io.fc` written as a physical FC file wrapping C stdio via extern declarations.
- `stdin`/`stdout`/`stderr` are now built-in globals typed `any*`.
- `str→cstr` cast implemented via `(cstr)expr`.
- `print`/`eprint`/`fprint` removed as compiler operators. All I/O now uses `io.write(s, f)`.
- Null-sentinel optimization extended to `any*?` and `cstr?`.

### Stdlib completeness pass (2026-03-28)
Extended all three non-experimental stdlib modules:
- **io.fc**: added `seek`, `tell`, `eof`, `remove`, `rename`, and seek origin constants (`seek_set`/`seek_cur`/`seek_end`).
- **sys.fc**: added `parse_int32`, `parse_int64`, `parse_float32`, `parse_float64` (wrapping atoi/atoll/strtof/atof).
- **math.fc**: added `asin`, `acos`, `atan` (inverse trig), `fmod`, `hypot`, `trunc`, and pure-FC `is_nan`/`is_inf`/`is_finite` using type properties.

### std::math module (resolved 2026-03-27)
Added `stdlib/math.fc` wrapping C11 `math.h`. 2 constants (`pi`, `e`) and 14 functions: `sqrt`, `abs`, `pow`, `min`, `max`, `floor`, `ceil`, `round`, `sin`, `cos`, `tan`, `atan2`, `exp`, `log`, `log2`, `log10`. Float64 only — consistent with FC's explicit-cast philosophy. Also fixed float literal codegen precision (was `%g` / 6 digits, now `%.17g` for float64, `%.9g` for float32) and added `-lm` to test runners and `run.sh`. Tests in `tests/cases/stdlib/`.

### type_name() for generic function types (resolved 2026-03-27)
`type_name()` (used by `%T` interpolation and all diagnostic error messages) now shows explicit type parameters on generic function types. Added `type_params` and `type_param_count` fields to `TYPE_FUNC` in `types.h`, populated from `EXPR_FUNC.explicit_type_vars` during pass2, and propagated through `resolve_type()` and `type_substitute()`. Output: `<'a>() -> 'a` instead of `() -> 'a`.

### Capturing lambda context lifetime (resolved 2026-03-27)
Returning a capturing lambda created a dangling pointer — the compound literal context (`&(_ctx_fn){ .captured_x = x }`) has block scope in C11. The direct case (`return (x) -> x + captured`) was already caught by a pattern match on `EXPR_FUNC`, but the indirect case (`let f = (x) -> x + captured; f`) was not, because the return value was an `EXPR_IDENT` with `PROV_UNKNOWN`. Fix: capturing lambdas now receive `PROV_STACK` provenance, and `TYPE_FUNC` is included in `type_has_provenance()`. The general provenance-based escape check now catches all paths: direct return, indirect via let, conditional branches (conservative merge), and storing in heap structs. Non-capturing lambdas retain `PROV_UNKNOWN` since their `.ctx` is `NULL`. Tests: 4 error cases (indirect, explicit indirect, branch, alloc struct), 2 success cases (non-capturing return, local use).

### Generic mixed type-var arithmetic (resolved 2026-03-27)
Binary operations on different type variables (`'a + 'b`, `'a > 'b`) were unsound — the result type was picked arbitrarily, which produced wrong types when widening was involved (e.g., `'a = int32`, `'b = int64`). Fix: pass2 now rejects binary operators on different type variables at template time. Same type var (`'a + 'a`) and concrete+typevar (`int32 + 'a`) remain allowed. Conservative-but-complete — no partial fix that covers some cases but breaks others. Tests: updated 4 existing error tests, added `concrete_typevar_arith.fc`, `mixed_typevar_bitwise_err.fc`, `mixed_typevar_logical_err.fc`.

### Branch widening in if/match (deferred 2026-03-27)
if/match branches require exact type equality. Implicit widening would allow compatible types to unify (e.g., `const str` + `str` → `const str`, `int8` + `int32` → `int32`). Also affects loop return type unification via `break value`. Deferred permanently — significant complexity (numeric widening, const widening, loop types all interact) for marginal benefit. Users manually cast to unify: `(const str)non_const_expr`. This is consistent with FC's design: no implicit widening across branches avoids a class of subtle bugs.

### Stack array literal size enforcement (resolved 2026-03-27)
Pass2 now validates that the size expression in `T[N] { }` is `EXPR_INT_LIT`, rejecting runtime expressions with "array size must be a compile-time constant". Previously this only failed at C compilation.

### Remove str32 (resolved 2026-03-27)
`str32` (alias for `uint32[]`) had no runtime support — no literals, no interpolation, no conversion. Removed from compiler (`TYPE_STR32` eliminated, `str32` no longer parses as a builtin type). Can re-add when Unicode support is properly designed. See also: "char32 type and str32 status (2026-03-24)" and "True type aliases" entries below.

### Missing %p interpolation test (resolved 2026-03-27)
Added `tests/cases/strings/interp_ptr.fc` — exercises `%p` format specifier on pointer values in string interpolation.

### Stdlib signatures const qualifiers (resolved 2026-03-27)
Spec function signatures for `io.write`, `io.open`, and `sys.env` updated to include `const` qualifiers matching the implementation: `io.write(s: const str, f: any*)`, `io.open(path: const str, mode: const str)`, `sys.env(name: const str) -> const str?`.

### Deferred items archived (2026-03-27)
The following items were explicitly moved out of the active TODO as deferred/out-of-scope:
- **Closures at extern boundaries** — wrapper function pattern for passing closures to C callback APIs. Current non-capturing restriction is conservative-but-complete.
- **Unreachable pattern detection** — warning-only feature. Maranget infrastructure supports it as the dual of exhaustiveness. Small addition when there's demand.
- **Field access on type variables** — structural generics (`(p: 'a) -> p.x + p.y`). High complexity, trivial workaround exists (pass fields as separate parameters).
- **Inline assembly, volatile, bitfield structs** — outside FC's platform contract. Users write C wrapper files.
- **Eager type resolution** — not viable. Blocked by self-referential struct cycles. On-demand `resolve_type()` is the correct approach.

### True type aliases for str/cstr/str32, import-as alias propagation (resolved 2026-03-17)
- `TYPE_STR`, `TYPE_CSTR`, `TYPE_STR32` removed from `TypeKind` enum. `str` is now `TYPE_SLICE{uint8}`, `cstr` is `TYPE_POINTER{uint8}`, `str32` is `TYPE_SLICE{uint32}` — with a `const char *alias` field on `Type` for display names.
- `str` and `uint8[]` are fully interchangeable (same for `cstr`/`uint8*`, `str32`/`uint32[]`).
- `const char*` emission confined to extern call boundaries only.
- `import T as alias from M` now propagates the alias name to diagnostics and type signatures.
