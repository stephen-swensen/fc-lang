# FC vs Zig: Comprehensive Comparison

## Feature Completeness Overview

**Zig is substantially more feature-complete as a production language.** FC is a well-designed 1.0 with clear opinions and a focused scope, but it covers less surface area. That said, FC has some genuine design wins over Zig in specific areas.

---

## Where FC Wins

### 1. Readable Transpilation Target
FC emits human-readable C11. You can inspect, debug, and understand the generated code. Zig targets LLVM IR (opaque). This is a real advantage for:
- Debugging: `gdb` on the C output is natural
- Portability: anywhere a C11 compiler runs, FC runs
- Auditability: the compiler's output is reviewable
- Bootstrap simplicity: FC's compiler is ~8k lines of C

### 2. Algebraic Data Types & Pattern Matching
FC has proper tagged unions with exhaustive pattern matching, struct destructuring, nested patterns, and wildcard binds. Zig has tagged unions and `switch`, but:
- Zig's `switch` on unions is more verbose
- No nested pattern destructuring in Zig
- No struct field patterns in Zig
- FC's exhaustiveness checking across bool/int/char/struct/union/option is thorough (74 dedicated tests)

### 3. Option Types as First-Class
`T?` is baked into FC's type system with `some`/`none`, `.is_some`/`.is_none`, `!` unwrap, and pattern matching. Pointer options `T*?` use null representation (zero overhead). Zig has optionals too, but FC's integration with pattern matching is tighter -- you can nest `option` inside union variants and destructure in one `match`.

### 4. String Interpolation
FC has `"hello %s{name}, n=%d{x + y}"` with arbitrary expressions inside `%spec{expr}`. Zig has `std.fmt.format` which is compile-time checked but far more verbose. FC's approach is ergonomic for the common case.

### 5. Structural Equality for All Types
Every FC type supports `==`/`!=` structurally -- structs, unions, slices, options, nested types. The compiler generates comparison functions automatically. In Zig, you must implement `std.meta.eql` or write manual equality; `==` on structs is a compile error.

### 6. Simpler Mental Model
FC has fewer concepts. No allocators threading through every API, no `comptime` to reason about, no `errdefer`/`defer` ordering, no `@` builtins namespace. The tradeoff is less power, but the learning curve is meaningfully lower.

### 7. Indentation-Based Syntax
Subjective, but FC's offside rule eliminates brace/semicolon noise. The `match` arm alignment with `|` pipes is clean. Zig's C-like braces are familiar but verbose.

### 8. F#-Inspired Functional Stdlib
FC's `data.fc` provides `fold_left`, `fold_right`, `scan`, `map`, `filter`, `iter`, `reduce`, `retain`, `any`, `all` on array_list, slice, linked_list, hash_dict, and hash_set. Zig's stdlib has none of this -- you write loops. For the functional-programming-inclined, FC's stdlib is more expressive out of the box.

---

## Where Zig Wins

### 1. Comptime -- Zig's Killer Feature
Zig's `comptime` is a full compile-time execution engine. You can:
- Run arbitrary code at compile time
- Generate types programmatically
- Write generic data structures without a separate generics system
- Implement compile-time format string checking
- Build compile-time hash maps, parsers, state machines

FC has `sizeof`, `alignof`, `default`, and conditional compilation (`#if`/`#end`). That's it. No compile-time evaluation, no type-level computation, no compile-time reflection.

**Impact**: Zig libraries can do things FC fundamentally cannot -- e.g., compile-time regex compilation, automatic serialization, protocol parsers generated from schemas.

### 2. Error Handling
Zig has a first-class error system:
- Error sets (named error enums)
- `try` / `catch` for propagation
- `errdefer` for cleanup on error paths
- Error return traces for debugging
- Error unions `!T` that compose naturally

FC's stdlib currently uses `T?` for fallible operations, which loses error information. However, FC's existing generics and tagged unions already support handrolled result types — `union result = | ok('a) | err('b)` with exhaustive pattern matching works today (see `spec/examples.fc`). Domain-specific error unions (e.g., `io_error` with `not_found`, `permission_denied`, etc.) compose naturally with this. C-style errno is also accessible via FC's extern system. The stdlib could adopt result types without any language changes.

The gap vs Zig is ergonomic, not capability: Zig's `try` keyword auto-propagates errors in one token, while FC requires explicit match-and-return at each call site. This is more verbose but also more explicit (Go made the same tradeoff).

### 3. Memory Management Sophistication
Zig provides:
- Custom allocators (passed explicitly, swappable)
- `defer`/`errdefer` for deterministic cleanup
- `GeneralPurposeAllocator` with leak detection
- Arena allocators, fixed-buffer allocators, page allocators
- `@memcpy`, `@memset` builtins

FC has: `alloc` (-> `calloc`), `free`, and intraprocedural escape analysis. No custom allocators, no `defer`, no leak detection. The escape analysis catches dangling returns and bad frees, but can't enforce "you forgot to free this."

**Impact**: FC programs will leak memory silently. Zig programs can catch leaks in tests via `std.testing.allocator`.

### 4. Concurrency & Async
Zig has (or had -- it's been redesigned) async/await with stackless coroutines, and the stdlib supports event loops and I/O. FC has **nothing** -- no threads, no async, no channels, no atomics. The networking stdlib (`net.fc`) is blocking-only.

**Impact**: FC cannot build concurrent servers, parallel pipelines, or anything requiring non-blocking I/O without dropping to C FFI.

### 5. Build System & Package Manager
Zig includes:
- `build.zig` -- a full build system written in Zig itself
- Package manager (`zig fetch`, `build.zig.zon`)
- Cross-compilation to dozens of targets out of the box
- Bundled C/C++ compiler (can compile C code too)

FC has: a Makefile, `./fcc input.fc -o output.c`, and `./run.sh`. No package manager, no build system, no cross-compilation support (beyond "use a different C compiler").

### 6. Safety Features
Zig provides:
- Runtime safety checks (overflow, null dereference, use-after-free detection in debug)
- `@intCast` with runtime bounds checking
- Undefined behavior detection in debug mode
- Stack protector support
- Address sanitizer integration

FC has: bounds-checked slices, option unwrap checks, escape analysis for dangling pointers, and defined integer overflow (wrapping). But no overflow detection in debug mode, no use-after-free detection, no sanitizer integration.

### 7. Inline Assembly
Zig has `asm` blocks for inline assembly with proper register constraints. FC has no way to write assembly -- you'd need to go through C FFI to an assembly file.

### 8. SIMD & Vector Types
Zig has first-class `@Vector` types with SIMD operations. FC has no vector types or SIMD support.

### 9. Packed Structs & Bit-Level Control
Zig has `packed struct` for exact bit layout, `@bitCast`, `@alignCast`, and control over struct padding. FC emits C structs with C's layout rules -- no packed structs, no bit fields, no alignment control beyond what C provides.

### 10. Testing Built Into the Language
Zig has `test "name" { ... }` blocks in the source, run with `zig test`. Tests live alongside code and have access to the testing allocator for leak checking. FC tests are separate `.fc` files that rely on `assert` + exit codes.

### 11. Ecosystem & Community
Zig has a substantial package ecosystem, corporate backing (Zig Software Foundation), real-world production users (Uber, Tigerbeetle), and thousands of contributors. FC is a solo project at its 1.0 release.

---

## Feature-by-Feature Matrix

| Feature | FC | Zig |
|---|---|---|
| **Type inference** | Local, bottom-up, directional | Limited (less inference, more explicit) |
| **Generics** | `'a` type vars, monomorphized | `comptime` types, monomorphized |
| **Tagged unions** | First-class with pattern matching | First-class with `switch` |
| **Pattern matching** | Exhaustive, nested, struct destructure | `switch` only, less expressive |
| **Option types** | `T?` built-in | `?T` built-in |
| **Error handling** | `T?` + abort | Error unions, `try`/`catch`, `errdefer` |
| **Closures** | Stack-allocated, copy capture | Function pointers only (no closures) |
| **String interpolation** | `%d{expr}` syntax | `std.fmt` (verbose) |
| **Memory model** | Manual, alloc/free | Manual, custom allocators, defer |
| **Escape analysis** | Intraprocedural, compile-time | None (relies on conventions) |
| **Bounds checking** | Always (slices) | Debug mode (configurable) |
| **Integer overflow** | Defined (wrapping) | Debug: trapped; release: wrapping |
| **Compile-time execution** | `#if` flags only | Full `comptime` evaluation |
| **Metaprogramming** | None | `comptime` + `@` builtins |
| **Inline assembly** | No | Yes |
| **SIMD** | No | `@Vector` types |
| **Async/concurrency** | No | Redesigned (was async/await) |
| **Build system** | Makefile | `build.zig` |
| **Package manager** | No | Yes |
| **Cross-compilation** | Via C compiler | Built-in, dozens of targets |
| **Target** | C11 source | Machine code (LLVM) |
| **Self-hosted** | No (compiler is C) | Yes |
| **Structural equality** | All types, automatic | Manual |
| **Defer/cleanup** | No | `defer` + `errdefer` |
| **Conditional compilation** | `#if`/`#end` | `comptime if` (type-safe) |
| **Testing** | External files + exit codes | Built-in `test` blocks |
| **Stdlib breadth** | math, io, text, net, data | Extensive (crypto, http, json, etc.) |

---

## Honest Assessment

**FC is roughly at the "usable hobby language" stage.** It has a complete compiler pipeline, a working stdlib with networking and data structures, 1,223 tests, and a clean spec. The core language design is sound -- the type inference, pattern matching, escape analysis, and C interop story are all coherent.

FC lacks error propagation, custom allocators, defer/cleanup, concurrency primitives, a build system, and compile-time metaprogramming. But so does C -- and C is the most successful systems language in history. These are features Zig added *on top of* the C-level baseline. FC isn't competing in Zig's niche; it's competing in C's niche, with modern ergonomics.

**FC's real strength is how much expressiveness it delivers for how simple the language is.** Pattern matching, structural equality, string interpolation, closures, generics, and a functional stdlib -- all without requiring the programmer to understand allocator threading, comptime, error sets, defer ordering, or a `@` builtins namespace. It's a good power-to-weight ratio, and it's a more pleasant language to *read and write* than Zig for many tasks. The C transpilation is a genuine architectural advantage for portability and debuggability.

**FC's positioning is "better C with modern ergonomics"** -- not "compete with Zig/Rust for systems programming." Against that benchmark, it's well-positioned: it keeps C's simplicity and mental model while adding the things C programmers actually miss (sum types, pattern matching, type inference, generics, no header files). The Zig comparison highlights features FC *could* grow into over time, not gaps that need closing before FC is useful.
