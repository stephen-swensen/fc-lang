/* Hover documentation for FC's built-in intrinsics and globals, included by
 * lsp.c. The strings are user-facing markdown and keep their typography, so
 * this is the one file in src/ exempt from the ASCII-only check. */

/* Separates a type's hover section from its companion module's. Drawn with
 * U+2500 text rather than markdown `---` because VSCode colors a markdown <hr>
 * with the theme's editorHoverWidget.border, which is invisible in themes that
 * draw borderless hovers. */
#define HOVER_RULE "────────────────────────────────"

static const BuiltinDoc BUILTIN_DOCS[] = {
    { "alloc", "alloc(T) -> T*?",
      "Allocates `T` on the **heap**, zero-initialized, and returns an option that is "
      "`none` when the OS allocation fails — so a successful pointer is checked, not assumed.\n\n"
      "Forms:\n"
      "- `alloc(T)` → `T*?` — one zeroed `T`\n"
      "- `alloc(T[n])` → `T[]?` — a slice of `n` zeroed elements\n"
      "- `alloc(T, n)` → `T*?` — a raw buffer of `n` elements\n"
      "- `alloc(expr)` → `T?` — a heap copy of a struct/union/string value\n\n"
      "Unwrap with `!` once checked, and release with `free` (commonly `defer free(p)`). "
      "Lowers to C `calloc`/`malloc`." },

    { "alloca", "alloca(T) -> T*",
      "Allocates `T` on the **dynamic stack** (`__builtin_alloca`), zero-initialized. "
      "Unlike `alloc` it cannot fail, returns the value directly (no option), and is "
      "reclaimed automatically when the enclosing function returns — never `free` it.\n\n"
      "Forms:\n"
      "- `alloca(T)` → `T*`\n"
      "- `alloca(T[n])` → `T[]`\n"
      "- `alloca(T, n)` → `T*` — a raw buffer\n\n"
      "The result has stack provenance: escape analysis forbids returning it or storing it "
      "in heap/static memory. **Beware loops** — each iteration grows the frame; promote a "
      "buffer that must persist or repeat to the heap with `alloc(...)`." },

    { "free", "free(p: T*) -> void",
      "Releases heap memory obtained from `alloc` (lowers to C `free`; for a slice it frees "
      "the backing `.ptr`).\n\n"
      "Only heap pointers may be freed: escape analysis rejects freeing stack (`alloca`, "
      "`&local`), static (string-literal), or `const` memory. After freeing, the pointer is "
      "dangling — don't read it again. Pairs naturally with `defer free(p)`." },

    { "default", "default(T) -> T",
      "Produces the **zero value** of any type `T`, with no initializer to spell out:\n"
      "- numbers → `0` / `0.0`\n"
      "- booleans → `false`\n"
      "- pointers and options → null / `none`\n"
      "- structs, unions, slices, tuples → every field zeroed\n\n"
      "Useful for an explicit empty or initial value. `none(T)` is shorthand for `default(T?)`." },

    { "some", "some(x: T) -> T?",
      "Wraps a present value in an option, yielding a `T?` that holds `x`.\n\n"
      "For a pointer option (`T*?`, which uses null as its empty sentinel) a value known to be "
      "non-null is stored directly; an unprovable one is null-checked at runtime and aborts if "
      "null. Pair with `none(T)` for the empty case, and read the value back with `!` or `match`." },

    { "none", "none(T) -> T?",
      "The **empty** option of inner type `T` — a `T?` holding no value. Exactly equivalent to "
      "`default(T?)`.\n\n"
      "The type argument is required so the element type is known: write `none(i32)`, not a bare "
      "`none`. Build the present case with `some(x)`, and test for empty with a `none` match arm "
      "(or `.is_none`)." },

    { "ok", "ok(x: T) -> T!",
      "Wraps a success value in a result, yielding a `T!` that holds `x` with error code 0.\n\n"
      "A result is `ok(T) | err(i32)` — repr `{ err: i32, value: T }` where `err == 0` means ok "
      "(C's \"0 on success\"). Pair with `err(T, code)` for the failure case, and read the value "
      "back with `match` or `!` (which aborts with the code on `err`)." },

    { "err", "err(T, code: i32) -> T!",
      "The **failure** result of payload type `T`, carrying a non-zero `i32` error code.\n\n"
      "The type argument is required so the success type is known: write `err(i32, 2)`, not a "
      "bare `err`. Code 0 is the ok tag and therefore unrepresentable — a provably-zero code is "
      "a compile error, an unprovable one is checked at runtime and aborts if zero. Build the "
      "success case with `ok(x)`, match failure with an `err(e)` arm (literal codes like "
      "`err(2)` work too), or test with `.is_err`." },

    { "error_name", "error_name(e: i32) -> str?",
      "The fully-qualified name of a declared error code — `some(\"file_io.not_found\")` for a "
      "constant declared in an `error` group, `none` for anything else (reserved-range platform "
      "passthrough codes like errno/Win32, and negative codes).\n\n"
      "Backed by a static name table emitted only when `error_name` is used (or unconditionally "
      "under `--backtraces`, where unwrap aborts also print the name) — a static cost, no runtime "
      "machinery. Format the numeric fallback yourself: codes below 65536 belong to the platform." },

    { "sizeof", "sizeof(T) -> i64",
      "The size of type `T` in bytes, as an `i64` (lowers to `(int64_t)sizeof(T)`). Works on any "
      "type — primitives, pointers, slices, structs, unions. Computed by the C compiler, so it "
      "reflects the target's real layout and padding." },

    { "alignof", "alignof(T) -> i64",
      "The alignment requirement of type `T` in bytes, as an `i64` (lowers to "
      "`(int64_t)_Alignof(T)`). The address of any `T` value is a multiple of this." },

    { "bitcast", "bitcast(T, x) -> T",
      "Reinterprets the raw bytes of `x` as type `T` — e.g. inspect an `f32`'s bits as a "
      "`u32` for hashing or serialization (`bitcast(u32, x)`), or vice versa.\n\n"
      "`T` and `x`'s type must both be **equal-size fixed-width scalars** (a fixed-width "
      "integer, float, or `char`); a size mismatch is a compile error, and `isize`/`usize` "
      "(target-defined width) and `bool` are not accepted. Unlike the value cast `(T) x`, no "
      "conversion happens — the bits are unchanged. Every bit pattern is a valid result, so "
      "there is no runtime failure (no `checked`/`unguarded` variant). Lowers to a C11 union "
      "type-pun that compilers fold to a register move (zero cost); it is *not* the "
      "strict-aliasing UB of a pointer-cast reinterpret." },

    { "enum_of", "enum_of(E, x) -> E?",
      "The membership-checked integer\u2192enum conversion \u2014 and the **only** one: a plain "
      "cast `(E) x` is a compile error. Accepts any integer type for `x` and yields `E?`: "
      "`some` of the matching variant when `x` equals a declared value, `none` otherwise.\n\n"
      "This is the boundary operation for C-shaped input (bytes from a data file, an int from "
      "an extern call): the check happens once at the edge, so every value of an enum type is "
      "a declared variant and matches over enums stay exhaustive without `_`. Lowers to a "
      "small static `switch` over the declared values (a range check when they are "
      "contiguous)." },

    { "assert", "assert(cond: bool[, msg: str]) -> void",
      "Checks `cond` at runtime; if it is false, prints the source file, line, and the failing "
      "condition text (plus the optional `msg`) to stderr and calls `abort()`.\n\n"
      "Forms:\n"
      "- `assert(cond)`\n"
      "- `assert(cond, \"message\")`\n\n"
      "For invariants that must hold — a violated assert ends the program. Always active; there "
      "is no disable switch." },

    { "atomic_load_acquire", "atomic_load_acquire(p: T*) -> T",
      "Atomically reads `*p` with **acquire** ordering: writes another thread released before "
      "storing to `*p` become visible afterward. `T` must be a lock-free integer or `bool` "
      "(enforced by a `_Static_assert`). Lowers to `__atomic_load_n(p, __ATOMIC_ACQUIRE)`.\n\n"
      "Pair with `atomic_store_release` to hand data between threads without a lock." },

    { "atomic_store_release", "atomic_store_release(p: T*, v: T) -> void",
      "Atomically writes `v` into `*p` with **release** ordering: writes this thread made before "
      "it are visible to any thread that later acquire-loads `*p`. `T` must be a lock-free integer "
      "or `bool`. Lowers to `__atomic_store_n(p, v, __ATOMIC_RELEASE)`.\n\n"
      "The reading side uses `atomic_load_acquire`." },

    { "stdin", "stdin: any*",
      "The process's standard input stream as an opaque `any*` (a C `FILE*`). Pass it to extern C "
      "I/O functions that expect a `FILE*`; it cannot be dereferenced in FC." },

    { "stdout", "stdout: any*",
      "The process's standard output stream as an opaque `any*` (a C `FILE*`). Pass it to extern C "
      "I/O functions that expect a `FILE*`; it cannot be dereferenced in FC." },

    { "stderr", "stderr: any*",
      "The process's standard error stream as an opaque `any*` (a C `FILE*`). Pass it to extern C "
      "I/O functions that expect a `FILE*`; it cannot be dereferenced in FC." },
};
