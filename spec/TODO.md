# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Codegen

### Field access on union-typed struct fields misidentified as variant constructor

In `codegen.c` around line 1698, the no-payload variant constructor path checks `e->type && e->type->kind == TYPE_UNION` on any `EXPR_FIELD`. This matches both actual variant constructors (`color.green`) and ordinary field accesses that return a union type (`cfg.bg` where `bg: color`). The latter incorrectly emits `(color){ .tag = color_tag_bg }` instead of the field access `cfg.bg`.

Reproducer:
```fc
union color =
    | red
    | green
    | blue

struct config =
    bg: color

let main = (args: str[]) ->
    let cfg = config { bg = color.green }
    let r = match cfg.bg with
    | red -> 1
    | green -> 2
    | blue -> 3
    r
```

The generated C references `color_tag_bg` (doesn't exist) instead of accessing the struct field. The fix needs to distinguish "variant constructor on a union type" from "field access on a value that happens to have union type" — likely by checking whether the object expression is a type/module reference vs a value.

### Cross-module function call inlined as variant constructor

When a module function returns a union variant (e.g., `shape.circle(r)`) and is called from another file via `module.func(arg)`, the codegen inlines the call as a variant literal using the **function name** as the variant tag instead of emitting a function call. For example, `helper.make_circle(5)` emits `(shape){ .tag = shape_tag_make_circle, .make_circle = 5 }` instead of `helper__make_circle(5, NULL)`.

Reproducer:
```fc
// main.fc
union shape =
    | circle(int32)
    | square(int32)

let main = (args: str[]) ->
    let s = helper.make_circle(5)
    match s with
    | circle(r) -> r * r
    | square(side) -> side * side
```

```fc
// helper.fc
module helper =
    let make_circle = (r: int32) -> shape.circle(r)
```

The generated C for `main` contains `shape_tag_make_circle` (doesn't exist) and `.make_circle = 5` (not a union member) instead of calling `helper__make_circle`. The helper function itself generates correct C. This is likely related to the variant-constructor-vs-field-access issue above — the codegen's `EXPR_FIELD` handling sees a union return type and treats the call as a variant construction.

### GNU statement expressions in generated C

The codegen emits GNU statement expressions (`({ ... })`) for indirect calls, `alloc(T[N])`, array literals, and other constructs. These are supported by GCC and Clang but are not part of the C11 standard. If strict C11 compliance is ever needed (e.g., MSVC support), the codegen would need to be refactored to emit temporary variables and sequential statements instead.

## Parser

### `const` types in generic type arguments

`const` is not recognized as a type modifier inside generic type argument lists (`<...>`). For example, `result<int32, const str>.err("fail")` fails to parse. The parser's type-argument path should be extended to handle `const` so that generic types can be instantiated with const-qualified type parameters.

## Pass1

### Piecemeal type stub resolution

Pass1 resolves type stubs (references to sibling extern types within the same `from` module) in a piecemeal fashion — one loop for extern function signatures, another for struct/union field types. Each time a new "escape path" for unresolved stubs is discovered, another resolution loop must be added. A more robust approach would be a single comprehensive pass that walks every type tree in every symbol within the module's member table after registration, resolving all stubs at once. This would close the door on future escape paths (e.g., generic type arguments, nested modules) rather than handling them case-by-case.

