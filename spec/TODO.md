# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## C Interop

### Extern unions with struct-typed fields

Extern unions cannot have fields whose type is a struct (e.g., C unions like SDL_Event where each variant is itself a struct with nested sub-structs). This forces workarounds using raw byte buffers and pointer arithmetic when interacting with complex C unions. Consider allowing struct-typed fields in extern unions. Additionally, consider allowing partial field declarations for extern unions/structs (declare only the fields you need), which would make it practical to import large C unions like SDL_Event with only the variants you use.

### `const` types in generic type arguments

`const` is not recognized as a type modifier inside generic type argument lists (`<...>`). For example, `result<int32, const str>.err("fail")` fails to parse. The parser's type-argument path should be extended to handle `const` so that generic types can be instantiated with const-qualified type parameters.

### Private module name conflicts across files

A `private module c from "stdio.h"` inside `std::io` (in stdlib/io.fc) conflicts with a top-level `module c from "stdlib.h"` in another file when both are compiled together. Private modules should be scoped to their enclosing module and not clash with same-named modules in other scopes. Currently requires workaround: rename one of the modules.

### Forward references for functions with inferred return types

Top-level `let` function bindings cannot be referenced before their definition in the same file if their return type is inferred. The spec states "Top-level declarations can reference each other regardless of order", but pass2 cannot resolve the type of a function whose body hasn't been type-checked yet. Currently requires workaround: order functions so callees come before callers.

