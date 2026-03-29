# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## C Interop

### `const` types in generic type arguments

`const` is not recognized as a type modifier inside generic type argument lists (`<...>`). For example, `result<int32, const str>.err("fail")` fails to parse. The parser's type-argument path should be extended to handle `const` so that generic types can be instantiated with const-qualified type parameters.


