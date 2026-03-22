# Active

Open design questions and topics for future work.

## Standard Library

### Namespace and structure
- Standard library will live in a `std::` namespace (distinct from `global::` used by application code)
- Flat module structure — one module per file, no nesting — because FC does not allow cross-file module definition, so nested submodules would have to live in a single file
- Defined modules: `io` (file I/O — see spec §std::io), `sys` (system operations — see spec §std::sys)
- Likely modules: `text`, `math`, plus others TBD
- Import pattern: `import io from std::`, `import sys from std::`, etc.
- Open: does stdlib require explicit import per file, or is any of it pre-imported?

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

### Escape analysis (low-hanging fruit)

Lightweight compile-time checks for the obvious cases:
- Returning a stack-derived pointer or slice from a function
- Storing a stack pointer/slice in a heap-allocated struct
- `free()` on a string literal or stack pointer

GCC/Clang do this for the simplest C cases (`-Wreturn-local-addr`). FC could do better since the compiler has more information about slice origins (it generates the `alloca` calls and array literal backing arrays).

### `const` qualifier (future)

Adding a `const` qualifier to pointer/slice types would let FC match C's convention where `const T*` / `const T[]` means "borrowed, don't modify, don't free" and `T*` / `T[]` means "you own this, you're responsible for it." This is widely used in C APIs:

```
const char *getenv(...)     // returns internal pointer — don't free
char *strdup(...)           // returns heap copy — caller must free
```

Core mechanic is straightforward: add `const` to the type system, reject writes through const pointers/slices in pass2, emit `const` in codegen. Key design questions:

- **Depth**: does `const int32*[]` mean the ints are const, the slice is const, or both? FC's simpler type system (no multi-level `const * const *` chains) should allow a cleaner design than C.
- **Inference**: should `let s = "hello"` infer `const str`? Probably yes for literals. Need coercion rules (non-const to const is safe, const to non-const requires explicit cast).
- **Transitivity**: if you have a const pointer to a struct, are the struct's fields const? (C says yes.)
- **Generics**: how does `box<const int32>` interact with monomorphization?
- **Cast-away**: C allows `(char*)const_ptr` to strip const. FC should probably allow this for C interop but could warn.

None of these are blockers — FC's type system is simpler than C's, so the design should be cleaner. Defer until after core milestones, then add as a focused feature.

**When const is added**, the `cstr` type alias can be split into `const cstr` (read-only, maps to `const char*`) and `cstr` (mutable, maps to `char*`). This would let extern declarations like `snprintf` use `cstr` for the mutable output buffer and `const cstr` for the format string — matching C's actual prototypes exactly. The compiler's automatic boundary casts would then emit `const char*` or `char*` depending on the const qualifier, eliminating the current need to use `any*` for mutable char buffer parameters. Spec examples and variadic extern tests should be updated accordingly.

#### Why `const` is simpler in FC than in C

In C, `const` can appear at every level of pointer indirection independently — `const int *`, `int *const`, `const int *const`, `const int **`, `int *const *`, etc. With N levels of indirection there are 2^N combinations, and the placement rules (`const` applies to whatever is left of it) are notoriously confusing.

FC's `const` would be simpler because:

1. **`let` vs `let mut` already handles reassignment.** C uses `const` for two things: "data is read-only" (`const int *p`) and "variable can't be reassigned" (`int *const p`). FC's `let`/`let mut` distinction covers the second case, so FC's `const` would only mean "the pointed-to data is read-only" — one meaning, not two.
2. **Multi-level pointers are rare in practice.** FC supports `T**` (the spec and compiler both allow it), but idiomatic FC code rarely needs it — option types, struct fields, and multiple return values via structs cover the common cases. So while `const` would technically need to compose through pointer levels, the N=1 case (`const T*`, `const T[]`) would cover the vast majority of real usage.

#### `T**` and C interop

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
    extern strtol: (cstr, cstr*, int32) -> int64

let mut end = default(uint8*)
let val = c.strtol((cstr)s, &end, 10)   // &end is uint8**, codegen emits (char**)
```

**Opaque handle out-parameters** (e.g. `sqlite3_open`'s `sqlite3**`) are also handled automatically. Since FC represents opaque C types as `any*`, the out-parameter type is `any**` — which emits as `void**` in C. But `void**` is not implicitly convertible to `sqlite3**` in C (only `void*` has that special property). The compiler handles this by emitting a `(void*)` cast at the extern boundary, collapsing `void**` to `void*` which C then implicitly converts to the target `T**`:

```fc
module sqlite from "sqlite3.h" =
    extern sqlite3_open: (cstr, any**) -> int32

let mut db = default(any*)
let rc = sqlite.sqlite3_open(c"test.db", &db)   // &db is any**, codegen emits (void*)
```

### What we're not doing

- Borrow checker / ownership system (Rust-style) — too complex, conflicts with "manual memory, maps to C" philosophy
- Runtime provenance tagging — conflicts with zero-cost philosophy
- Automatic reference counting — same

## Build targets (`--target`, embedded, bare-metal)

The compiler currently only supports hosted targets (`target_hosted` is always set). The design calls for:

- `--target <name>` CLI flag (e.g. `arduino-uno`, `esp32`, `bare-metal`)
- Built-in flags `target_embedded` and `target_bare_metal` set automatically
- `alloc`/`free` availability enforcement per target (compile error on bare-metal)
- `import libc` scoped to functions available on the target

The conditional compilation infrastructure (`#if`/`#else if`) is already in place and ready to use these flags once they are defined. The generated C is already portable — it can be compiled with embedded toolchains today by compiling the output manually. The `--target` flag would automate this and add compile-time enforcement.

---

# Resolved

## Native platform-width types `isize`/`usize` (resolved 2026-03-22)

Added `isize` (signed, pointer-width) and `usize` (unsigned, pointer-width) as opt-in types for C interop and embedded targets. FC's defaults remain fixed-width: `int32` for default integers, `int64` for `sizeof` and slice `.len`. The native types are escape hatches for when exact platform type matching matters.

- **Codegen**: `isize` → `ptrdiff_t`, `usize` → `size_t` (resolved by the C compiler, not FC)
- **Literal suffixes**: `42i` (isize), `42u` (usize) — bare `i`/`u` without width digits
- **No implicit widening**: explicit casts required in both directions between isize/usize and fixed-width types. `isize + int32` is a type error; write `(isize)x + y` or `(int32)x + y`
- **Type properties**: `.bits`, `.min`, `.max` are platform-dependent (emitted as C expressions like `PTRDIFF_MAX`, `SIZE_MAX`, `(int32_t)(sizeof(ptrdiff_t)*8)`)
- **Generics**: type variables can bind to isize/usize; monomorphization works normally
- **Operators**: arithmetic, comparison, bitwise, shifts all work between same-type operands; signed overflow wrapping and shift masking use platform-dependent expressions
- 548 tests passing (18 new native_types tests).

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
- 396 tests covering all milestones M1–M9.
