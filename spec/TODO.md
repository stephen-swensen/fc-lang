# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Standard Library

### io.read_all

Read an entire file into a heap-allocated string. API design TBD: return type (`str?` vs `str` with error), max size limits, error handling strategy.

### std::text — string manipulation utilities

`std::text` exists with parse functions (`parse_int32`, `parse_int64`, `parse_float32`, `parse_float64`). Remaining: split, join, trim, contains, starts_with, etc. Design TBD.

### prelude.fc / types.fc status

`stdlib/prelude.fc` provides `print`, `println`, `freeze` — undocumented convenience functions. `stdlib/types.fc` provides generic `tuple1` through `tuple5` — undocumented. Both are experimental and only used in `tests/scratch.fc`. Need a decision before v1.0: formalize in spec or remove.

