# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Never/bottom type for `return`, `break`, `continue`**: These expressions should unify with any type, not just `void`. Currently `let x = match opt with | some(v) -> v | none -> return 1` fails because the match arms have types `int32` vs `void`. The `none` arm never produces a value (it exits the function), so it should be compatible with any type.

- **Test runner: targeted test execution**: Add support for running a subset of tests by name or pattern (e.g., `make test FILTER=stdlib/data_array_list`) for faster iteration during development. The full suite is growing and takes ~10s.

- **stdlib data: sort for array_list and linked_list, insert for linked_list**: `array_list` and `linked_list` should have `sort` methods (taking a comparison function). `linked_list` should also have an `insert` operation (at a given node position). Insert requires exposing node handles/cursors; sort requires a comparison function since generics have no trait constraints.

- **Codegen: slices of module-internal generic structs**: When a module defines a generic struct (e.g., `struct entry = key: 'a value: 'b state: int32`) and another struct in the same module has a slice of it (`buckets: entry<'a, 'b>[]`), the generated C has two issues: (1) the entry struct typedef is emitted after the slice typedef that references it, and (2) bounds-checked slice indexing generates a statement-expression whose result can't be used for field access. This blocks using a single entry struct for hash_dict/hash_set buckets; the current workaround uses parallel arrays (struct-of-arrays).

- **Default hash/eq operators for hash_dict/hash_set**: Currently `hash_dict.make<int32>(hash_fn, eq_fn)` requires explicit hash and equality functions. A future enhancement could provide built-in `hash` and `eq` operators (based on byte representation of the type) so users can write `hash_dict.make<int32, str>()` without explicit function arguments.
