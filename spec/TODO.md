# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## C Interop

### Extern unions with struct-typed fields

Extern unions cannot have fields whose type is a struct (e.g., C unions like SDL_Event where each variant is itself a struct with nested sub-structs). This forces workarounds using raw byte buffers and pointer arithmetic when interacting with complex C unions. Consider allowing struct-typed fields in extern unions. Additionally, consider allowing partial field declarations for extern unions/structs (declare only the fields you need), which would make it practical to import large C unions like SDL_Event with only the variants you use.

### `const` types in generic type arguments

`const` is not recognized as a type modifier inside generic type argument lists (`<...>`). For example, `result<int32, const str>.err("fail")` fails to parse. The parser's type-argument path should be extended to handle `const` so that generic types can be instantiated with const-qualified type parameters.


