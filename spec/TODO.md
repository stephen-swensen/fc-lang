# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Never/bottom type for `return`, `break`, `continue`**: These expressions should unify with any type, not just `void`. Currently `let x = match opt with | some(v) -> v | none -> return 1` fails because the match arms have types `int32` vs `void`. The `none` arm never produces a value (it exits the function), so it should be compatible with any type.

- **Test runner: targeted test execution**: Add support for running a subset of tests by name or pattern (e.g., `make test FILTER=stdlib/data_array_list`) for faster iteration during development. The full suite is growing and takes ~10s.

- **stdlib data: sort for array_list and linked_list, insert for linked_list**: `array_list` and `linked_list` should have `sort` methods (taking a comparison function). `linked_list` should also have an `insert` operation (at a given node position). Insert requires exposing node handles/cursors; sort requires a comparison function since generics have no trait constraints.

- **Default hash/eq operators for hash_dict/hash_set**: Currently `hash_dict.make<int32>(hash_fn, eq_fn)` requires explicit hash and equality functions. A future enhancement could provide built-in `hash` and `eq` operators (based on byte representation of the type) so users can write `hash_dict.make<int32, str>()` without explicit function arguments.

- **Compiler crash: self-referential generic structs inside modules**: A generic struct with a self-referential field (e.g., `struct node = next: node<'a>*?`) crashes the compiler (segfault) when defined inside a module, whether namespaced or not. The same pattern works fine at the top level (outside any module). Repro: `module m = struct node = value: 'a next: node<'a>*? let make = (v: 'a) -> alloc(node { value = v })!` then call `m.make(42)`. Workaround: use struct-of-arrays with index-based linking instead of pointer-based self-referential types.

- **Compiler crash: multiple generic structs in one module where one references the other**: When a module contains two generic structs and one references the other (e.g., `struct node = value: 'a` and `struct list = head: node<'a>*?`), instantiating the outer struct crashes the compiler. Defining them without cross-reference is fine. Workaround: use struct-of-arrays where all node data is stored in parallel arrays within a single struct.

- **Forward reference error in namespaced module functions**: Within a module in a namespaced file, calling a function defined later in the same module triggers error "function 'X' has no explicit type parameters; type arguments are inferred from call arguments". This does NOT occur in non-namespaced files. Workaround: ensure all callee functions are defined before their callers (definition order matters in namespaced modules).

- **Generic function pointer field calls require split read/call**: Calling a function pointer field on a generic struct directly (e.g., `d->hash(key)` where `hash: ('a) -> int64`) fails with "cannot resolve generic function '?'". Workaround: split into two steps: `let fn = d->hash; fn(key)`.

- **Generic function calls from parent to child module scope fail**: A private generic helper defined at the parent module level cannot be called from a nested child module, even though the name resolves. Error: "cannot resolve generic function 'helper'". Workaround: define helpers within the same module that calls them, or inline the logic.
