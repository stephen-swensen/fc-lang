# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Test coverage gaps

- **Escape analysis + struct field pointers** — storing stack pointers into struct fields at heap/static scope; pointer provenance transitivity through heap-allocated structs with pointer fields.
- **isize/usize edge cases** — bitwise ops on platform-width ints, type-property queries against generic constraints involving isize/usize, mixed-width widening rules at boundaries.
- **String interpolation edge cases** — nested function calls inside `%d{...}`, empty format strings, interpolation inside closure captures (compile-time vs runtime ordering).
