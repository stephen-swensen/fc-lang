# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## C Interop

### Extern unions with struct-typed fields

Extern unions cannot have fields whose type is a struct (e.g., C unions like SDL_Event where each variant is itself a struct with nested sub-structs). This forces workarounds using raw byte buffers and pointer arithmetic when interacting with complex C unions. Consider allowing struct-typed fields in extern unions. Additionally, consider allowing partial field declarations for extern unions/structs (declare only the fields you need), which would make it practical to import large C unions like SDL_Event with only the variants you use.

### Private module name conflicts across files

A `private module c from "stdio.h"` inside `std::io` (in stdlib/io.fc) conflicts with a top-level `module c from "stdlib.h"` in another file when both are compiled together. Private modules should be scoped to their enclosing module and not clash with same-named modules in other scopes. Currently requires workaround: rename one of the modules.

## Standard Library

### prelude.fc / types.fc status

`stdlib/prelude.fc` provides `print`, `println`, `freeze` — undocumented convenience functions. `stdlib/types.fc` provides generic `tuple1` through `tuple5` — undocumented. Both are experimental and only used in `tests/scratch.fc`. Need a decision before v1.0: formalize in spec or remove.

