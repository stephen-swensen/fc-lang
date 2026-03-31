# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Parser

### `const` types in generic type arguments

`const` is not recognized as a type modifier inside generic type argument lists (`<...>`). For example, `result<int32, const str>.err("fail")` fails to parse. The parser's type-argument path should be extended to handle `const` so that generic types can be instantiated with const-qualified type parameters.

## Pass1

### Piecemeal type stub resolution

Pass1 resolves type stubs (references to sibling extern types within the same `from` module) in a piecemeal fashion — one loop for extern function signatures, another for struct/union field types. Each time a new "escape path" for unresolved stubs is discovered, another resolution loop must be added. A more robust approach would be a single comprehensive pass that walks every type tree in every symbol within the module's member table after registration, resolving all stubs at once. This would close the door on future escape paths (e.g., generic type arguments, nested modules) rather than handling them case-by-case.

