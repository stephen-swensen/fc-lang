# CLAUDE.md

Guidance for Claude Code in this repository. Everything a human maintainer
needs lives in the files below; this file adds only what is specific to an AI
assistant.

## Where things are

- `docs/ARCHITECTURE.md`: the pipeline, what each source file does, the
  invariants the compiler relies on, and the language server design.
- `CONTRIBUTING.md`: build and test targets, the test layout and markers, the
  checklist for adding a feature or an AST/type kind, code conventions, and
  the engineering principles behind design decisions.
- `spec/fc-spec.html`: the language specification. `spec/examples.fc` is a
  runnable tour of the language; read it first.
- `spec/TODO.md`: open work, including the language server's known limits.

## Rules for the assistant

- Never commit, and never invoke `/commit`; the user commits. The user may
  also commit or change branches outside the session, so don't assume the
  repository state stays put.
- Run `make check` (or at least `make test-all`) before presenting a final
  summary of compiler changes, and `make test-gcc-len16` /
  `make test-clang-len16` when touching slice code. Changes only to `demos/`
  or `spec/` (other than `spec/examples.fc`) don't need the suite.
- `spec/fc-spec.html` and `src/` are the only authorities on the language.
  There is no grammar file; don't create one, and don't treat `spec/hist/` as
  current. If the spec doesn't settle a question, ask the user and then write
  the answer into the spec.
- Before proposing a language design decision, lay out how C, Rust, Zig and
  similar languages handle it, and check the spec for an existing rule.
- A compiler bug found while doing something else: confirm it with a
  reproduction, report it prominently, and ask before fixing it.

## FC syntax that is easy to get wrong

- No type annotations on `let`; the type comes from the right-hand side.
  Function parameters always have types.
- Functions have no return type annotation. `->` introduces the body:
  `let f = (x: i32) -> x * 2`. `let f = (x: i32) -> i32 = ...` and
  `-> void` are both wrong.
- Match arms line up with `match`, not indented under it:
  ```fc
  match x with
  | some(v) -> use(v)
  | none -> fallback()
  ```
- Union variants are declared with a leading `|`, constructed qualified
  (`shape.circle(5)`), and matched bare (`| circle(r) -> ...`). Each variant
  has zero or one payload.
- `.` on a pointer dereferences one level (`p.field`). `->` is never field
  access.
- `for` needs `do`: `for i in 0..n do ...`, `for x in xs do ...`. There is
  no `while`; use `loop` with `break`.
- No compound assignment (`+=`), no `null` (use options), comments are `//`
  and `/* */` only, and indentation is spaces only.
- All names are lowercase `snake_case`, types and modules included.
