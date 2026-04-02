# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Never/bottom type for `return`, `break`, `continue`**: These expressions should unify with any type, not just `void`. Currently `let x = match opt with | some(v) -> v | none -> return 1` fails because the match arms have types `int32` vs `void`. The `none` arm never produces a value (it exits the function), so it should be compatible with any type.

- **Test runner: targeted test execution**: Add support for running a subset of tests by name or pattern (e.g., `make test FILTER=stdlib/data_array_list`) for faster iteration during development. The full suite is growing and takes ~10s.


- **stdlib data: audit for in-place / mutating variants**: Review all data module functions to identify candidates for in-place mutation instead of (or in addition to) allocating new collections. Examples: `filter` on linked_list could unlink/free non-matching nodes in place (`retain`); `map` could overwrite values when the type doesn't change (`map_in_place: ('a) -> 'a`); `reverse` on linked_list could rewire pointers; `sort` already mutates. The tradeoff is API symmetry vs. efficiency — since `copy` is available for users who need it, we may prefer mutating as the default where it's clearly more efficient, with clear doc comments noting ownership semantics. This may lead to asymmetric APIs across data structures (e.g., linked_list.filter mutates, slice.filter allocates) which is acceptable if well-documented.

- **Default hash/eq operators for hash_dict/hash_set**: Currently `hash_dict.make<int32>(hash_fn, eq_fn)` requires explicit hash and equality functions. A future enhancement could provide built-in `hash` and `eq` operators (based on byte representation of the type) so users can write `hash_dict.make<int32, str>()` without explicit function arguments.
