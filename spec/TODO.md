# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Standard Library

### std::math module

sin, cos, sqrt, pow, abs, floor, ceil, round, etc. wrapping `math.h`. Tier 1 (C11 standard).

### std::text module

String manipulation utilities (split, join, trim, contains, starts_with, etc.). Design TBD.

### prelude.fc / types.fc status

`stdlib/prelude.fc` provides `print`, `println`, `freeze` — undocumented convenience functions. `stdlib/types.fc` provides generic `tuple1` through `tuple5` — undocumented. Both are experimental and only used in `tests/scratch.fc`. Need a decision before v1.0: formalize in spec or remove.

