# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

- **Never/bottom type for `return`, `break`, `continue`**: These expressions should unify with any type, not just `void`. Currently `let x = match opt with | some(v) -> v | none -> return 1` fails because the match arms have types `int32` vs `void`. The `none` arm never produces a value (it exits the function), so it should be compatible with any type.
