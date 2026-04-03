# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Test runner: targeted test execution**: Add support for running a subset of tests by name or pattern (e.g., `make test FILTER=stdlib/data_array_list`) for faster iteration during development. The full suite is growing and takes ~10s.


- **Default hash/eq operators for hash_dict/hash_set**: Currently `hash_dict.make<int32>(hash_fn, eq_fn)` requires explicit hash and equality functions. A future enhancement could provide built-in `hash` and `eq` operators (based on byte representation of the type) so users can write `hash_dict.make<int32, str>()` without explicit function arguments.
