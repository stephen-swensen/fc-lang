# TODO

Open items for the FC compiler and specification. Resolved items archived in `spec/hist/archived-todos.md`.

## Compiler — diagnostics and language ergonomics

Surfaced during the wolf-fc subsystem-extraction refactor (2026-04). Each one cost real time to work around; roughly ordered by frequency-of-bite.

- **Unused-binding warning (opt-in).** A file-scope / module-scope `let` that is declared but never referenced is detectable statically with zero extra analysis. In wolf-fc this round, hand-rolled shell scripts turned up `tile_area`, `tex_size`, `alt_elevator_tile`, and `hud.draw_vsep` — all dead code that survived the original file split. A quiet `-Wunused` (off by default) would catch these in one pass. Same check naturally extends to same-scope shadowing: a `let` shadowed by another `let` in the same scope is almost always a mistake.

- **Const-expr propagation through same-scope name references in module-level initializers.** The 2026-04 loosening (commit `a8c1221`) accepts array literals, `some(...)`, and variant constructors as module-level initializers, but element expressions still must be literals. This block fails even though every RHS name is a module-level `let X = <int-literal>` in the same scope:
  ```fc
  module music =
      let getthem = 3
      let searchn = 11
      ...
      let songs = int32[60] { getthem, searchn, getthem, ..., pacman }
      //                       ^ error: must be constant expression
  ```
  The workaround is writing `3, 11, 3, ..., 26` with a comment apologizing for the raw ints, which loses both readability and grep-ability (renaming `getthem` wouldn't touch the table). The table would round-trip cleanly through the typechecker: each reference is to a constant already known at that scope. Extending const-expr to include references to prior `let X = <const-expr>` bindings in the same module (and probably top-level at minimum) would unlock this.

- **Cycle diagnostics should report the shortest call-graph path, not just the file/module pair.** When wolf-fc's refactor accidentally introduced the transitive cycle `overlay → save.wolf_data_dir` + `save.from_slot → intermission.enter → overlay.draw_centered_text`, the error identified the two modules but not the specific edges. Tracing by hand through a 2300-line graph took a few minutes. Printing the shortest symbol-to-symbol path that forms the cycle (e.g., `overlay.take_screenshot → save.wolf_data_dir, save.from_slot → intermission.enter → overlay.draw_centered_text`) would turn that into an instant fix.

## Standard library — fleshing out

Part 9 of the spec is intentionally a stub; the stdlib itself still has gaps identified in the 2026-04-20 audit. Candidate work, roughly ordered by impact:

- **Test coverage for `std::sys`** — currently zero tests. `env`, `time`, `sleep`, `get_pid`, `temp_dir`, `home_dir`, `exit` all need coverage, especially across POSIX/MinGW.
- **Fill in `std::io` test gaps** — `read_all`, `read_char`, `exists`, `can_read`, `can_write` are currently untested.
- **Formatted output beyond interpolation** — `sprintf`-style number→string into a caller-supplied buffer; formatted writers on a `FILE*` handle. Interpolation is `alloca`-backed and doesn't cover every case.
- **Error type for I/O / syscalls** — today `io`/`sys`/`net` surface failure as `bool` or `option`, losing `errno`. A small `io_error` union (or a thread-local `last_errno`) would make diagnostics actionable.
- **Bit utilities** — `popcount`, `leading_zeros`, `trailing_zeros`, `rotl`/`rotr`, `byteswap`. Cheap to add, commonly needed.
- **Encoding helpers** — `base64`, `hex`, URL encoding.
