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

## Resolved

### File handles are `any*`, not a built-in type (resolved 2026-03-11)
- The `file` built-in type was removed. File handles are `any*` — the same opaque pointer type used for sqlite handles, pthread handles, and any other C resource.
- No reason to privilege file I/O with a special type when all other C libraries use `any*`.
- File operations moved from built-in `file.open`/`file.close`/etc. to `std::io` module (`io.open`, `io.close`, etc.).
- `stdout`/`stderr` moved from built-in constants to `io.stdout`/`io.stderr`.
- `print`/`eprint` remain reserved compiler operators — they don't require `std::io` import.
- `fprint` destination changed from `file` to `any*`.
