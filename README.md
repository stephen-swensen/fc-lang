# FC

FC is a systems programming language that transpiles to C11. It aims to combine modern language ergonomics with C's performance and memory model — no garbage collector, no borrow checker, just direct control.

## Key Features

- **C11 target** — generates portable C using `<stdint.h>` types and `_Static_assert`
- **Indentation-based syntax** — offside rule, spaces only
- **Type inference** — directional (bottom-up, inside-out), no global unification
- **Monomorphized generics** — zero runtime cost
- **Manual memory management** — follows C's philosophy
- **No null** — option types (`T?`) replace nullable values
- **Expressions everywhere** — `if`, `match`, and `loop` produce values

## Status

Early stage. The repository currently contains the language specification (v0.5 draft). The compiler is not yet implemented.

## Repository Layout

```
spec/fc-spec.html   Language specification (open in a browser to read)
spec/TODO.md        Open design questions
src/                Compiler source (empty — work has not started)
```

For code examples, see the full language specification in `spec/fc-spec.html`.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
