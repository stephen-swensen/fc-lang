# FC Spec TODO

Open design questions and topics for future discussion.

---

## Standard Library

### Namespace and structure
- Standard library will live in a `std::` namespace (distinct from `global::` used by application code)
- Flat module structure — one module per file, no nesting — because FC does not allow cross-file module definitions, so nested submodules would have to live in a single file
- Defined modules: `io` (file I/O — see spec §std::io), `sys` (system operations — see spec §std::sys)
- Likely modules: `text`, `math`, plus others TBD
- Import pattern: `import io from std::`, `import sys from std::`, etc.
- Open: does stdlib require explicit import per file, or is any of it pre-imported?

---

## alloc / free / sizeof / default

### Should these move to a sys module? (discussed, deferred)
- Original motivation: reserved identifier status prevents users from defining `free` in their own modules
- Resolved by convention: `drop` is the idiomatic name for user-defined deep cleanup functions; `free` is reserved for raw deallocation (wrapping C's `free`). Convention sidesteps the conflict.
- A `sys::` namespace for built-ins would need to be a second implicit namespace (like `global::`), which is non-trivial spec work
- Also discussed regularizing all four as generic functions in a `sys` module (e.g. `sys.alloc<T>()`, `sys.free(p)`) to make them feel like normal language constructs. Decided against it for several reasons: the initialized alloc form (`alloc(expr)`) would have to be dropped to avoid overloading; slice allocation requires an awkward separate name (`alloc_slice`) due to the no-overloading rule; and `sys` would not be a real module but compiler magic pretending to be one, which undermines the framing.
- **Decision: no change for now.** Revisit if the `drop` convention proves insufficient in practice.

---

## No `size_t` equivalent — extern declarations hardcode uint64

FC has no `size_t` type. Extern declarations for C functions that take or return `size_t` (e.g., `fwrite`, `fread`, `malloc`) must use a fixed-width integer — currently `uint64`. This is correct on 64-bit platforms but wrong on 32-bit, where `size_t` is `uint32`.

**Current workaround**: Since FC doesn't emit its own extern forward declarations (the real C headers provide correct prototypes via `#include`), the C compiler sees the correct `size_t`-width parameters and implicitly narrows the `uint64` values FC passes. This is sub-optimal (unnecessary 64→32 bit operations) but valid — the same pattern as FC's general approach of using `int64` arithmetic on 32-bit platforms where `int32` is native. The values involved in I/O sizes always fit in 32 bits in practice.

**If FC adds 32-bit target support**: Consider adding a `usize`/`isize` type that maps to the target's pointer width, or a `size_t` type alias that resolves at codegen time. Until then, `uint64` is the pragmatic choice.

---

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

**Opaque handle out-parameters** (e.g. `sqlite3_open`'s `sqlite3**`) are a different case. Since FC represents opaque C types as `any*`, the out-parameter would need `any**` — but `void**` is not implicitly convertible to `sqlite3**` in C. The pragmatic workaround is declaring the param as `any*` and casting: `(any*)&db`. A future improvement could teach the extern boundary to handle this, but it's low priority since most opaque-handle APIs are wrapped in higher-level FC modules that hide the out-parameter pattern.

### What we're not doing

- Borrow checker / ownership system (Rust-style) — too complex, conflicts with "manual memory, maps to C" philosophy
- Runtime provenance tagging — conflicts with zero-cost philosophy
- Automatic reference counting — same

---

## Resolved

### Implicit widening in generic function calls (resolved 2026-03-15)
- Generic function arguments with concrete parameter types now auto-widen, matching non-generic call behavior.
- e.g. `get(list, 0)` works when `index: int64` — the int32 literal widens to int64.
- Widening only applies to parameters that contain no type variables. Type variable binding via unification still requires exact matches.

### Implicit widening in struct literals and variant constructors (resolved 2026-03-15)
- Struct literal fields and union variant payloads now auto-widen, matching function call argument behavior.
- e.g. `point { x = 10 }` works when `x: int64`, and `holder.val(42)` works when `val(int64)`.

### Structural equality, codegen safety, spec alignment (resolved 2026-03-15)
- Structural `==`/`!=` implemented for all types: structs (field-by-field), unions (tag + payload), slices (element-wise), str/str32 (len + memcmp), options (has_value + inner), func (fn_ptr + ctx). Generated `fc_eq_T` comparison functions.
- Codegen safety: signed overflow wrapping via cast-through-unsigned for `+`, `-`, `*`, unary `-`; shift amount masking (`& 31` for 32-bit, etc.); integer division/modulo by zero emits `abort()` check (float unaffected per IEEE 754).
- str32 type parsing fixed (wrong length in lookup table, missing `type_str32()` constructor).
- Pointer ordering (`<`, `>`, `<=`, `>=`) now accepted (was incorrectly limited to numeric types).
- Address-of (`&x`) now rejects immutable `let` bindings; `&f` on top-level functions still works.
- String literal pattern matching in `match` implemented (was a no-op in codegen).
- 326 tests covering all milestones M1–M8 plus cross-cutting features.

### File handles are `any*`, not a built-in type (resolved 2026-03-11)
- The `file` built-in type was removed. File handles are `any*` — the same opaque pointer type used for sqlite handles, pthread handles, and any other C resource.
- No reason to privilege file I/O with a special type when all other C libraries use `any*`.
- File operations moved from built-in `file.open`/`file.close`/etc. to `std::io` module (`io.open`, `io.close`, etc.).

### Extern declarations, std::io module, print→io.write migration (resolved 2026-03-16)
- `extern` declarations implemented: parse, pass1 registration, pass2 type-checking, codegen (no `_ctx` parameter).
- `module ... from "lib"` syntax for C library source metadata.
- `stdlib/io.fc` written as a physical FC file wrapping C stdio via extern declarations.
- `stdin`/`stdout`/`stderr` are now built-in globals typed `any*` (not module members).
- `str→cstr` cast implemented via `(cstr)expr` — emits alloca+memcpy stack copy with null terminator.
- `print`/`eprint`/`fprint` removed as compiler operators. All I/O now uses `io.write(s, f)`.
- Null-sentinel optimization extended to `any*?` and `cstr?` (not just `T*?`).

### True type aliases for str/cstr/str32, import-as alias propagation (resolved 2026-03-17)
- `TYPE_STR`, `TYPE_CSTR`, `TYPE_STR32` removed from `TypeKind` enum. `str` is now `TYPE_SLICE{uint8}`, `cstr` is `TYPE_POINTER{uint8}`, `str32` is `TYPE_SLICE{uint32}` — with a `const char *alias` field on `Type` for display names.
- `str` and `uint8[]` are fully interchangeable (same for `cstr`/`uint8*`, `str32`/`uint32[]`). Eliminated ~15 duplicate `TYPE_SLICE || TYPE_STR` code paths in pass2 and codegen.
- `const char*` emission confined to extern call boundaries only; within FC-generated code, `cstr` emits as `uint8_t*`.
- `import T as alias from M` now propagates the alias name to diagnostics and type signatures via shallow-copied `Type` with `alias` set. Multiple aliases for the same type are independent and interchangeable.
- Spec updated with alias name propagation semantics in §Static Type Properties and §Importing.
- 395 tests covering all milestones M1–M9.
