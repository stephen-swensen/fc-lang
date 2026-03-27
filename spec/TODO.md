# Active

Open design questions and topics for future work.

## Standard Library

### Namespace and structure
- Standard library will live in a `std::` namespace (distinct from `global::` used by application code)
- Flat module structure — one module per file, no nesting — because FC does not allow cross-file module definition, so nested submodules would have to live in a single file
- Defined modules: `io` (file I/O — see spec §std::io), `sys` (system operations — see spec §std::sys)
- Likely modules: `text`, `math`, plus others TBD
- Import pattern: `import io from std::`, `import sys from std::`, etc.
- Resolved: explicit import required per file. No auto-imports — matches FC's "no magic" philosophy.

## Slice/pointer provenance and safety

### Current situation

All pointer/slice creation paths produce the same types with no provenance information:

| Source | Storage | Safe to return? | Safe to free? |
|--------|---------|-----------------|---------------|
| `alloc(T)`, `alloc(T, N)`, `alloc(expr)` | Heap | Yes | Yes |
| `"hello"`, `c"hello"` | Static read-only | Yes | **No** |
| `&x` on `let mut` | Stack | **No** | **No** |
| `T[N] { }` array literal | Stack | **No** | **No** |
| `"hello \{x}"` interpolation | Stack (`alloca`) | **No** | **No** |
| `(cstr)str_value` cast | Stack (`alloca`) | **No** | **No** |

This is the same situation as C, with one exception: C has `const` which provides partial protection (compiler rejects writes through `const char*`), while FC has no equivalent. FC also creates more implicit stack temporaries than typical C (interpolation, str→cstr casts, array literals) where the sugar hides the provenance.

The `alloc(slice)` deep-copy pattern mitigates this — `alloc(s)!` promotes any stack slice to heap-owned memory. But the programmer must know when to use it.

### Escape analysis — implemented (see Resolved section)

The lightweight compile-time escape checks are now implemented. See the Resolved section for details.

### Capturing lambda context lifetime (bug)

Capturing lambdas store their context in a compound literal: `&(_ctx_fn){ .captured_x = x }`. The compound literal has block scope in C11. This is safe when the lambda is used within the same function (the compound literal's scope covers the lambda's usage). However, FC allows returning capturing lambdas from functions, which creates a dangling context pointer — the compound literal's storage is reclaimed when the function returns.

GCC detects this at `-Werror -Wdangling-pointer` (C compilation fails), so it cannot produce a silently wrong binary. But FC should catch this at the FC level: extend escape analysis to track closure provenance and reject returning capturing lambdas.

Note: non-capturing lambdas are not affected — their `.ctx` is `NULL`.

### Stack array literal size must be compile-time literal (enforcement gap)

Per spec, `N` in `T[N] { }` must be a compile-time integer literal. The compiler currently accepts runtime expressions (e.g., `int32[n] { }`) without error — the failure only surfaces at C compilation. Pass2 should validate that the size expression is a constant (literal or constant-foldable expression) and emit a clear FC error otherwise. Low priority since the failure mode is a C compilation error, not silent wrong behavior.

### Branch widening in if/match (future)

Currently, if/match branches require exact type equality. Adding implicit widening would allow branches returning different-but-compatible types to unify automatically — e.g., `const str` + `str` → `const str`, or `int8` + `int32` → `int32`. This also affects loop return type unification via `break value`. Deferred because it opens a can of worms (numeric widening, const widening, loop types all interact). For now, users manually cast to unify: `(const str)non_const_expr`.

### `T**` and C interop

FC supports `T**` in the type system — the compiler, parser, and type checker all handle multi-level pointers. The main C use case for double pointers is out-parameters, where a function writes a pointer back through `T**`:

```c
int sqlite3_open(const char *filename, sqlite3 **ppDb);
long strtol(const char *str, char **endptr, int base);
// caller: sqlite3 *db; sqlite3_open("test.db", &db);
// the ** exists because &(T*) = T** — it's just an out-parameter
```

**In pure FC**, `T**` out-parameters work directly — a function can accept `T**` and write through it. However, idiomatic FC rarely needs this pattern because functions return values directly, and option types / struct returns handle the cases where C would use an out-parameter.

**At the C boundary**, `cstr*` (uint8**) is automatically cast to `char**` by the compiler, following the same pattern as the `cstr` → `const char*` cast for single-level pointers. This covers common C APIs like `strtol`:

```fc
module c from "stdlib.h" =
    extern strtol: (const cstr, cstr*, int32) -> int64

let mut end = default(uint8*)
let val = c.strtol((const cstr)s, &end, 10)   // &end is uint8**, codegen emits (char**)
```

**Opaque handle out-parameters** (e.g. `sqlite3_open`'s `sqlite3**`) are also handled automatically. Since FC represents opaque C types as `any*`, the out-parameter type is `any**` — which emits as `void**` in C. But `void**` is not implicitly convertible to `sqlite3**` in C (only `void*` has that special property). The compiler handles this by emitting a `(void*)` cast at the extern boundary, collapsing `void**` to `void*` which C then implicitly converts to the target `T**`:

```fc
module sqlite from "sqlite3.h" =
    extern sqlite3_open: (const cstr, any**) -> int32

let mut db = default(any*)
let rc = sqlite.sqlite3_open(c"test.db", &db)   // &db is any**, codegen emits (void*)
```

### What we're not doing

- Borrow checker / ownership system (Rust-style) — too complex, conflicts with "manual memory, maps to C" philosophy
- Runtime provenance tagging — conflicts with zero-cost philosophy
- Automatic reference counting — same

## Platform contract, core language, and stdlib

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

## Closures at extern boundaries — wrapper function pattern (future)

Currently, only top-level functions and non-capturing lambdas can be passed to extern C functions. The compiler generates static trampolines (`_ctramp_*`) that strip the `void* _ctx` parameter and call the known function directly with `NULL`. This is conservative but complete — it is correct for all C callback APIs.

The limitation shows up when writing FC stdlib wrappers. A wrapper like `let sort = (arr: int32[], compare: (int32, int32) -> int32) -> ...` receives `compare` as a fat pointer `{fn_ptr, ctx}`. When forwarding to the extern `qsort`, it's rejected because it's a local function value, not a top-level function or literal lambda. This prevents the idiomatic pattern of wrapping C APIs with nicer FC signatures.

### Design space explored

C callback APIs fall into three categories:

1. **Synchronous callbacks** (`qsort`, `bsearch`) — callback is invoked before the extern returns. A thread-local stash approach works: stash the fat pointer's `fn_ptr` and `ctx` before the extern call, generate a static trampoline that reads from the stash. Sound because the stash is valid for the duration of the call.

2. **Deferred callbacks with user-data** (`pthread_create`, most event loops) — the C API provides a `void*` parameter that gets passed through to the callback. The context could be threaded through this parameter without a stash. Fully sound but requires different codegen than category 1.

3. **Deferred callbacks without user-data** (`signal`, `atexit`) — no mechanism to carry context. Closures fundamentally cannot work here without runtime code generation. The non-capturing restriction is the only correct answer.

### Why this is deferred

Implementing only category 1 (stash approach) would be a partial solution — it handles `qsort`-style wrappers but not `pthread_create`-style wrappers. Per the project's completeness principle, a partial solution that covers some APIs but silently breaks on others is worse than a conservative restriction that is uniformly correct. The current restriction (category 3 behavior for all extern boundaries) is complete.

### Possible approaches if revisited

- **Thread-local stash for synchronous callbacks**: per-callsite `_Thread_local` fn_ptr/ctx slots with a static trampoline that reads from them. Simple codegen, works for the common case. Unsound if the C function stores the pointer for later or the same callsite is reentered.

- **Inferred restriction propagation**: if pass2 detects that a function parameter flows to an extern call, mark that parameter as "must be non-capturing" and enforce at call sites transitively. No new syntax, but requires cross-function dataflow analysis.

- **Explicit `extern` function type annotation**: e.g. `compare: extern (int32, int32) -> int32` to mark a parameter as a bare C function pointer. Simple local checking, visible in the API, better error messages. Adds a new concept to the type system.

- **User-data threading for deferred callbacks**: for APIs like `pthread_create` that provide a `void*` arg, the compiler could pack the fat pointer context into that parameter and generate a trampoline that unpacks it. Requires recognizing the pattern (which parameter is the user-data?) — possibly via annotation on the extern declaration.

A complete solution would need to handle at least categories 1 and 2 together. Category 3 always keeps the non-capturing restriction.

---

---

# Notes

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

**Addressed by platform contract / core / stdlib redesign (see Active TODO):**
- ~~No `--target` flag~~ — dropped. Platform contract is "C11 with libc and heap." No bare-metal support. Conditional compilation uses user-defined `--flag` only.
- ~~No preprocessor define control~~ — solved by `define` annotation on extern module declarations. Replaces the `_POSIX_C_SOURCE` auto-detection hack.

**Remaining gaps (active):**

1. **No `#define` / macro interop** — C constants defined as `#define FOO 42` can't be imported. Must redeclare manually in FC. Platform-dependent values (e.g., `O_RDONLY`) make manual redeclaration error-prone. Needed for any non-trivial C library (SDL has hundreds of constants).

2. **No fixed-size array fields in structs** — C structs and unions often contain fixed-size arrays (`uint8_t addr[16]`, `char name[256]`). FC has no way to declare a field that occupies `N * sizeof(T)` bytes inline. Design direction: `T[N]` syntax in struct field declarations as a storage annotation — the field is stored as a C array but accessed as a `T[]` slice (the compiler creates a fat pointer view on access). Slice assignment to a fixed-size field would emit a bounded `memcpy`. Works for both extern and first-class FC structs. Not blocking for SDL2 (whose types are mostly scalar/struct fields) but needed for networking, crypto, and other C APIs that use inline arrays.

Items 1–2 are needed for full C interop coverage. Item 1 is the SDL2 blocker.

**Deferred gaps:**

4. **No inline assembly** — can't emit platform-specific instructions. For GPIO toggling, interrupt handlers, etc., need a C wrapper file. Outside FC's problem space per the platform contract.

5. **No `volatile`** — relevant for memory-mapped I/O registers on embedded. Outside FC's problem space per the platform contract.

6. **No bitfield structs** — C bitfields (`uint32_t flags : 4`) are common in hardware register definitions. Outside FC's problem space per the platform contract.

---

# Resolved

## Function pointer trampolines at extern boundaries (resolved 2026-03-22)

FC functions internally carry an extra `void* _ctx` parameter for closure support. When a non-capturing function or lambda is passed to a C extern expecting a plain function pointer, the compiler now generates a static trampoline that drops the `_ctx` and matches the C calling convention. This happens automatically — the programmer just passes the function. Documented in spec §C interop with a `qsort` example.

## `const` qualifier for pointer/slice types (resolved 2026-03-22)

Added `const` as a type qualifier for pointer and slice types. `const` means "can't write through this indirection" — orthogonal to `let`/`let mut` which controls binding reassignment.

### Design
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

### What const is NOT
- No `const` at binding sites (FC has no type annotations on `let` — always inferred from RHS)
- No branch widening (if/match branches with `const T*` and `T*` require explicit cast — deferred to future branch widening feature)
- No `const` on bare value types (`const int32` is a parse error)

### Implementation
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

## Stack escape analysis (resolved 2026-03-22)

Lightweight intraprocedural escape analysis added to pass2. Every expression that produces a pointer or slice type is tagged with a **provenance** (`PROV_UNKNOWN`, `PROV_STACK`, `PROV_HEAP`, `PROV_STATIC`) tracking where its backing storage lives. The analysis catches three classes of bugs at compile time:

1. **Returning stack-derived pointers/slices** — `return &local`, returning array literals, interpolated strings, `(cstr)str` casts, subslices of stack arrays, or any of these through let bindings, if/match branches, blocks, some-wrapping, pointer arithmetic, or pointer casts.
2. **Freeing non-heap memory** — `free("hello")` (static), `free(&x)` (stack), `free(array_lit)` (stack), `free(interp_string)` (stack).
3. **Storing stack pointers in heap structs** — `alloc(node { data = &local })` where a struct field is a stack-derived pointer or slice.

### Provenance sources
- `PROV_STACK`: `&x` (address-of local), array literals, interpolated strings, `(cstr)str` cast (alloca copy), subslice of stack
- `PROV_HEAP`: all forms of `alloc()` — including `alloc(stack_slice)` which deep-copies to heap
- `PROV_STATIC`: string literals `"..."`, cstring literals `c"..."`
- `PROV_UNKNOWN`: function parameters, call return values, extern results (no interprocedural analysis)

### Propagation
Provenance flows through: let bindings, identifiers, if/match branches (conservative merge — any STACK branch → STACK), blocks (last expression), some/unwrap, pointer arithmetic, pointer casts, subslices, `.ptr` on slices.

### Design decisions
- **Intraprocedural only**: function parameters are `PROV_UNKNOWN`. A function receiving a pointer is allowed to return it — the caller is responsible for correctness.
- **Conservative on reassignment**: `let mut` reassignment does not update provenance (tracks initial binding only). This may produce false positives if a stack pointer is later reassigned to heap, but is safe.
- **`alloc(stack_data)` produces PROV_HEAP**: the whole point of `alloc(s)!` is to promote stack data to heap. The provenance of the input is not propagated.
- **Option types checked recursively**: `some(&x)` returns `int32*?` which carries `PROV_STACK` — the option wrapper doesn't hide the provenance.

### Implementation
- `Provenance` enum and `prov` field added to `Expr` in `ast.h`
- `LocalBinding` extended with `prov` in pass2; `scope_add_prov()` propagates through let bindings
- Three check points in pass2: explicit `return`, implicit return (last expression in function body), `free()`, and `alloc(struct_lit)` fields
- 36 new tests in `tests/cases/escape/` (18 error tests, 18 success tests)
- 593 total tests passing

## Native platform-width types `isize`/`usize` (resolved 2026-03-22)

Added `isize` (signed, pointer-width) and `usize` (unsigned, pointer-width) as opt-in types for C interop and embedded targets. FC's defaults remain fixed-width: `int32` for default integers, `int64` for `sizeof` and slice `.len`. The native types are escape hatches for when exact platform type matching matters.

- **Codegen**: `isize` → `ptrdiff_t`, `usize` → `size_t` (resolved by the C compiler, not FC)
- **Literal suffixes**: `42i` (isize), `42u` (usize) — bare `i`/`u` without width digits
- **No implicit widening**: explicit casts required in both directions between isize/usize and fixed-width types. `isize + int32` is a type error; write `(isize)x + y` or `(int32)x + y`
- **Type properties**: `.bits`, `.min`, `.max` are platform-dependent (emitted as C expressions like `PTRDIFF_MAX`, `SIZE_MAX`, `(int32_t)(sizeof(ptrdiff_t)*8)`)
- **Generics**: type variables can bind to isize/usize; monomorphization works normally
- **Operators**: arithmetic, comparison, bitwise, shifts all work between same-type operands; signed overflow wrapping and shift masking use platform-dependent expressions
- 548 tests passing (18 new native_types tests).

## `char32` type and `str32` status (2026-03-24)

FC defines `char` as an alias for `uint8` with character literal syntax (`'a'`, `'\n'`, `'\x41'`). For symmetry, `str32` (alias for `uint32[]`) implies a corresponding `char32` alias for `uint32` — a single Unicode code point.

However, `str32` itself is only partially implemented: it parses and type-checks, but has no runtime support (no `str32` literals, no `str32` interpolation, no `str32`↔`str` conversion). Two options:

1. **Add `char32`** — alias for `uint32`, supports 32-bit character literals (e.g., `'😀'c32` or similar syntax). Only worth doing if `str32` gets full runtime support.
2. **Remove `str32`** — remove it from the type system, built-in type names list, and spec until Unicode support is actually designed and implemented. Avoids shipping a half-implemented feature.

Leaning toward option 2 — remove `str32` until it's real.

## Generic type variable soundness — mixed type-var arithmetic (2026-03-24)

### Problem
Binary operations on different type variables (`'a + 'b`, `'a > 'b`) are currently allowed at template time, but the result type is unsound when widening is involved. During template checking, the type-var early return (pass2.c) picks one operand's type arbitrarily as the result. When instantiated with types that widen (e.g., `'a = int32`, `'b = int64`), the inferred return type (`int32`) doesn't match the actual computed type (`int64`).

Example:
```fc
let ff = <'b, 'c>(x: (int32, int32) -> int32) -> x(32, 32) + default('b) + default('c)
```
Template infers return type = `'b`. But `ff<int32, int64>(...)` computes `int32 + int32 + int64` = `int64` via widening, while the return type says `int32`.

### Proposed fix
Restrict mixed type-var binary operations: require both operands to have the same type variable (e.g., `'a + 'a` ok, `'a + 'b` error at template time, concrete + `'a` ok since it pins `'a`). This is conservative-but-complete — prevents silent type mismatches without requiring trait bounds or return type re-inference.

### Also deferred
`type_name()` for generic function types doesn't show explicit type parameters (e.g., shows `((int32, int32) -> int32) -> 'b` instead of `<'b, 'c>((int32, int32) -> int32) -> 'b`). Would require carrying type param info on `TYPE_FUNC`. Low priority — `%T` is most useful on concrete values.

## Field access on type variables — structural generics (explored, deferred 2026-03-22)

Explored allowing field access on bare type variables, e.g. `let sum = (p: 'a) -> p.x + p.y`, where `'a` is resolved to a concrete struct at monomorphization. This would enable duck-typed generic functions that operate on any struct with matching field names — similar to C++ templates or Go structural interfaces.

### What was prototyped
- A new type kind `TYPE_FIELD_OF(base_type, field_name)` representing "the type of field F of type T", resolved during `type_substitute` when the base type becomes a concrete struct.
- Pass2 deferred field access validation on `TYPE_TYPE_VAR` and `TYPE_FIELD_OF`, creating `TYPE_FIELD_OF` nodes instead of erroring.
- Binary operator deferral extended to handle `TYPE_FIELD_OF` operands.
- Codegen resolved `TYPE_FIELD_OF` through the substitution context during monomorphized emission.
- 17 tests passing: basic access, arithmetic, comparison, let binding, nested access (3 levels deep), multiple struct types, chained generic calls, multi-type instantiation.

### Why it was deferred
- **High complexity relative to value.** `TYPE_FIELD_OF` threads through `type_substitute`, `type_eq`, `type_contains_type_var`, `type_collect_vars`, `type_name`, `mangle_type_name`, `emit_type`, and `emit_type_ident`. The first 80% was clean, but completing it properly would require changes to `unify()`, match exhaustiveness, unary operators, and monomorphization-time error reporting — roughly as much work again.
- **Incomplete error reporting.** Invalid field access (wrong field name, non-struct type) was not caught until C compilation. Proper monomorphization-time error reporting would be a significant addition to the compiler.
- **Unification gaps.** Constructing a generic struct from deferred field values (`pair { x = p.y, y = p.x }`) fails because `unify` can't bind `'a` to `TYPE_FIELD_OF('a, "x")` without creating a self-referential binding.
- **Trivial workaround exists.** Instead of `let sum = (p: 'a) -> p.x + p.y`, write `let sum = (x: 'a, y: 'a) -> x + y`. Passing fields as parameters is idiomatic in systems programming and already fully supported.
- **Rare in practice.** "Any struct with field X" is a dynamic/structural typing pattern more natural in TypeScript or Go than in a C-targeting systems language. Real-world generic code (containers, algorithms, utilities) works through operators, function parameters, and built-in constructs — all already supported.

### If revisited
The `TYPE_FIELD_OF` approach is viable. Key remaining work: (1) extend `unify()` to handle `TYPE_FIELD_OF` in struct literal construction, (2) add monomorphization-time error reporting for invalid field access, (3) handle match/unwrap on deferred field types, (4) extend unary operator deferral. Consider whether the complexity is justified by real user demand before proceeding.

## Multiline struct literals, function calls, and array literals (resolved 2026-03-21)
Bracket depth tracking added to the lexer layout pass. When inside `()`, `[]`, or `{}`, `INDENT`/`DEDENT`/`NEWLINE` tokens are suppressed. Multiline struct literals, function calls with many arguments, and array literals all parse naturally. Trailing commas permitted. No parser changes needed.

## alloc/free/sizeof/default placement — no change (resolved 2026-03-21)
Considered moving to a `sys` module. Resolved by convention: `drop` is the idiomatic name for user cleanup; `free` stays reserved for raw deallocation. Regularizing as generic functions in a module was rejected because `alloc(expr)` can't coexist with `alloc<T>()` under no-overloading, and `sys` would be compiler magic pretending to be a module.

## Unreachable pattern detection — deferred (resolved 2026-03-21)
Low priority since it's a warning, not a correctness issue. The Maranget infrastructure is in place; unreachable arm detection is the dual of exhaustiveness (call `find_witness` against preceding arms). Small addition when needed.

## Module-scoped imports — implemented (2026-03-25)

Imports now follow lexical scoping rules, matching `let` binding semantics. Implemented and reverted once (2026-03-24), then re-implemented with a new architecture (2026-03-25) using ImportTable/ImportRef with live source references instead of the previous dual-registration approach.

**New architecture:** Uses `ImportTable` with `ImportRef` entries that store lightweight references (local_name, source_name, source_members pointer) instead of copying symbol data. Lookups go through the source module's live member table, avoiding the stale NULL-type problem. An `ImportScope` linked list (stack-allocated, pushed/popped on module entry/exit) provides arbitrarily deep nested visibility — child modules inherit parent imports with shadowing.

**Key design points:**
- `import * from M` only exports M's own declarations (`members`), never M's imports (non-forwarding)
- File-level imports are scoped per-file via `FileImportScopes`, preventing cross-file leakage
- Module-level imports are stored in `Symbol.imports`, separate from `members`
- Lookup chain: local scope → module members → import scope chain → global declarations
- Later imports shadow earlier imports (same scope level); inner scopes shadow outer
- On-demand type checking for imported functions uses source_members context switch

## Eager type resolution — not viable (deferred indefinitely, 2026-03-21)
Resolving struct field types and union variant payloads in-place on registered types (the high-value part of the proposal) is blocked by self-referential structs. A struct like `node { next: node*? }` creates a cyclic type graph when its field type is resolved from `option(pointer(stub))` to `option(pointer(full_node))` — the full node's `next` field then points back to itself. Functions like `type_contains_type_var()`, `type_eq()`, and other recursive type walkers traverse struct fields and infinite-loop on these cycles. Resolving only AST annotations (param types, cast targets, etc.) is possible but adds ~170 lines of AST walker to eliminate ~10 one-line `resolve_type()` calls — not worth the complexity. The existing on-demand `resolve_type()` calls in pass2 and `resolve_struct_stub()` in codegen remain the correct approach.

## Codegen: nested option/slice typedef ordering (resolved 2026-03-20)
- `collect_types_in_type()` now recurses into inner types before adding the outer type to the typeset, ensuring dependency typedefs are emitted first in the generated C.
- Fixed for both option types (`int32??`, `int32???`) and slice types (latent bug for nested slices).
- Matches the existing `TYPE_FUNC` pattern which already recursed first.

## M9: std::sys module, main args as str[], conditional compilation, cstr→str cast (resolved 2026-03-17)
- `std::sys` module (`stdlib/sys.fc`): `env`, `exit`, `time`, `sleep` — pure FC wrapping C stdlib via extern declarations. `time`/`sleep` use a private `timespec` struct passed to C via `any*` casts. `_POSIX_C_SOURCE` emitted only when `time.h` is used.
- Main function signature changed from `(args: int32)` to `(args: str[])`. Codegen emits `fc_main(fc_slice_fc_str args)` for user code plus a C `main` wrapper that converts `argc`/`argv` to `str[]` via `alloca`. Pass2 validates main takes exactly `str[]` and returns `int32`.
- Conditional compilation: `#if`/`#else if`/`#else`/`#end` directives implemented as a token-level filter between raw tokenization and layout pass. Directives must appear at column 1. `target_hosted` is a built-in flag; user flags via `--flag name`. Inactive branches are stripped (syntax-checking deferred to M10).
- `(str)cstr` cast implemented: wraps existing pointer with `strlen`-derived length (no copy). Complements existing `(cstr)str` cast (stack copy + null terminator).
- Extern boundary casts extended to `cstr*` returns (`uint8_t**` ↔ `char**`).
- Parser fix: `parse_if_expr` now restores position when no `else` found, preventing `(cast)expr` after void `if` from being misparsed as a function call.
- 413 tests passing.

## Implicit widening in generic function calls (resolved 2026-03-15)
- Generic function arguments with concrete parameter types now auto-widen, matching non-generic call behavior.
- e.g. `get(list, 0)` works when `index: int64` — the int32 literal widens to int64.
- Widening only applies to parameters that contain no type variables. Type variable binding via unification still requires exact matches.

## Implicit widening in struct literals and variant constructors (resolved 2026-03-15)
- Struct literal fields and union variant payloads now auto-widen, matching function call argument behavior.
- e.g. `point { x = 10 }` works when `x: int64`, and `holder.val(42)` works when `val(int64)`.

## Structural equality, codegen safety, spec alignment (resolved 2026-03-15)
- Structural `==`/`!=` implemented for all types: structs (field-by-field), unions (tag + payload), slices (element-wise), str/str32 (len + memcmp), options (has_value + inner), func (fn_ptr + ctx). Generated `fc_eq_T` comparison functions.
- Codegen safety: signed overflow wrapping via cast-through-unsigned for `+`, `-`, `*`, unary `-`; shift amount masking (`& 31` for 32-bit, etc.); integer division/modulo by zero emits `abort()` check (float unaffected per IEEE 754).
- str32 type parsing fixed (wrong length in lookup table, missing `type_str32()` constructor).
- Pointer ordering (`<`, `>`, `<=`, `>=`) now accepted (was incorrectly limited to numeric types).
- Address-of (`&x`) now rejects immutable `let` bindings; `&f` on top-level functions still works.
- String literal pattern matching in `match` implemented (was a no-op in codegen).
- 326 tests covering all milestones M1–M8 plus cross-cutting features.

## File handles are `any*`, not a built-in type (resolved 2026-03-11)
- The `file` built-in type was removed. File handles are `any*` — the same opaque pointer type used for sqlite handles, pthread handles, and any other C resource.
- No reason to privilege file I/O with a special type when all other C libraries use `any*`.
- File operations moved from built-in `file.open`/`file.close`/etc. to `std::io` module (`io.open`, `io.close`, etc.).

## Extern declarations, std::io module, print→io.write migration (resolved 2026-03-16)
- `extern` declarations implemented: parse, pass1 registration, pass2 type-checking, codegen (no `_ctx` parameter).
- `module ... from "lib"` syntax for C library source metadata.
- `stdlib/io.fc` written as a physical FC file wrapping C stdio via extern declarations.
- `stdin`/`stdout`/`stderr` are now built-in globals typed `any*` (not module members).
- `str→cstr` cast implemented via `(cstr)expr` — emits alloca+memcpy stack copy with null terminator.
- `print`/`eprint`/`fprint` removed as compiler operators. All I/O now uses `io.write(s, f)`.
- Null-sentinel optimization extended to `any*?` and `cstr?` (not just `T*?`).

## True type aliases for str/cstr/str32, import-as alias propagation (resolved 2026-03-17)
- `TYPE_STR`, `TYPE_CSTR`, `TYPE_STR32` removed from `TypeKind` enum. `str` is now `TYPE_SLICE{uint8}`, `cstr` is `TYPE_POINTER{uint8}`, `str32` is `TYPE_SLICE{uint32}` — with a `const char *alias` field on `Type` for display names.
- `str` and `uint8[]` are fully interchangeable (same for `cstr`/`uint8*`, `str32`/`uint32[]`). Eliminated ~15 duplicate `TYPE_SLICE || TYPE_STR` code paths in pass2 and codegen.
- `const char*` emission confined to extern call boundaries only; within FC-generated code, `cstr` emits as `uint8_t*`.
- `import T as alias from M` now propagates the alias name to diagnostics and type signatures via shallow-copied `Type` with `alias` set. Multiple aliases for the same type are independent and interchangeable.
- Spec updated with alias name propagation semantics in §Static Type Properties and §Importing.
