# FC Language — VSCode extension

Editor support for the [FC](../../README.md) language, backed by the compiler's
built-in language server (`fcc --lsp`).

## Features

- **Syntax highlighting** (TextMate grammar — works even without the server;
  block comments nest, matching the compiler).
- **Live diagnostics** — error squiggles as you type, from the real compiler.
- **Hover** — inferred type of the identifier under the cursor, plus its doc
  comment: the run of `//` lines directly above a definition, and the `//`
  trailing a struct/union field.
- **Type CodeLens** — the inferred type shown on a line *above* each `let`.
- **Go to definition** — module-level functions, structs, unions, modules;
  block-locals (parameters, `let`s, `for` variables, match bindings); and
  struct fields (jumps to the field's declaration).
- **Completion** — keywords, in-scope/top-level names, and `.`/`::` members.

By default the server merges the installed standard library and the open file's
sibling `.fc` files into every analysis, so `import ... from std::...` resolves
and diagnostics for `std::` symbols are suppressed (only the open file's
diagnostics are shown).

### Project files (`lsp.rsp`)

For projects spanning multiple directories, drop an `lsp.rsp` file at the project
root. The server discovers it by walking up from the file you're editing and
treats it as authoritative — the analysis then uses exactly the inputs it lists
(instead of guessing via the sibling/stdlib heuristic), so cross-directory
imports resolve correctly. It is an ordinary compiler response file: each line
holds command-line arguments (input paths, globs, `--flag`s), paths are relative
to the `lsp.rsp` itself, `#`/`//` begin comments, and `@other.rsp` pulls in
another file. The very same file works on the CLI as `fcc @lsp.rsp`. Example:

```
# lsp.rsp — the project's compilation unit
src/*.fc
lib/*.fc
stdlib/io.fc        # opt into just the stdlib modules you use
--flag debug
```

## Install

From the repository root:

```sh
sudo make install    # put `fcc` on PATH and the stdlib in the data dir (needs root)
make install-vscode  # package + install the extension via the editor CLI (NO sudo)
```

Then reload the VSCode window (Command Palette → "Developer: Reload Window").

The extension is **dependency-free** — it drives `fcc --lsp` with VSCode's
built-in Node runtime, so there is no `npm install` step. `install-vscode`
packages a `.vsix` by hand (no `vsce`) and installs it with
`code --install-extension`, because modern VSCode ignores extensions that are
merely copied into `~/.vscode/extensions`. Run it as your normal user (not
root). Override the editor CLI with `FC_CODE_CLI=codium` (or `cursor`, …); if no
CLI is found it falls back to unpacking into `~/.vscode/extensions` (then fully
restart the editor). Linux is the supported target.

## Settings

- `fc.serverPath` (default `"fcc"`) — path to the `fcc` binary. Set this to an
  absolute build path (e.g. `.../build/linux/fcc`) if `fcc` is not on `PATH`.
- `fc.stdlibPath` (default empty) — directory holding the stdlib `.fc` files;
  sets `FCC_STDLIB_DIR` for the server. Leave empty to use the installed
  location (or a repo-relative `./stdlib`).

The type CodeLens uses VSCode's standard CodeLens rendering; toggle it with the
built-in `"editor.codeLens"` setting (scoped to `[fc]` by default).
