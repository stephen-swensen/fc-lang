# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Codegen

### Stack frame lifetime for inner-scope bindings

The spec says a pointer to an inner-scope binding remains valid for the entire function call (all local bindings share a single stack frame). The codegen uses GCC statement expressions (`({ ... })`) that scope inner variables, causing `-Werror=dangling-pointer` failures when a pointer to an inner-scope variable is used after the statement expression closes. Either the codegen needs to hoist declarations out of statement expressions, or the spec claim should be narrowed. Needs investigation.

## Standard Library

### prelude.fc / types.fc status

`stdlib/prelude.fc` provides `print`, `println`, `freeze` — undocumented convenience functions. `stdlib/types.fc` provides generic `tuple1` through `tuple5` — undocumented. Both are experimental and only used in `tests/scratch.fc`. Need a decision before v1.0: formalize in spec or remove.

