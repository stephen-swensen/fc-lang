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

## Slice/pointer provenance tracking

Stack-allocated memory (e.g. interpolated strings via `alloca`, `&local_var`), read-only memory (string literals), and heap memory (`alloc`) all produce the same pointer/slice types. The compiler currently has no way to distinguish provenance or warn about returning stack pointers, freeing read-only memory, etc. This is a known gap — same as C. Options to explore:
- Lightweight escape analysis (warn if stack-derived slice/pointer escapes the function)
- Provenance annotations or regions
- Runtime tagging (unlikely — conflicts with zero-cost philosophy)

Related concern: if a function returns a `str`, the caller can't distinguish heap-allocated (must free) from read-only (must not free/mutate). One option: make `free` on read-only memory a no-op (compiler or runtime check), so callers can always safely free a returned slice. But this doesn't solve the case where a caller tries to write into returned read-only memory. Needs more thought.

No urgency — the "trust the programmer" model works for now — but worth revisiting as the language matures.

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
- 389 tests covering all milestones M1–M9.
