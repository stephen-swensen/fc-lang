# FC Language — VSCode extension

Editor support for the [FC](../../README.md) language, backed by the compiler's
built-in language server (`fcc --lsp`).

## Features

- **Syntax highlighting** (TextMate grammar — works even without the server).
- **Live diagnostics** — error squiggles as you type, from the real compiler.
- **Hover** — inferred type of the identifier under the cursor.
- **Type CodeLens** — the inferred type shown on a line *above* each `let`.
- **Go to definition** — for module-level functions, structs, unions, modules.
- **Completion** — keywords, in-scope/top-level names, and `.`/`::` members.

The server merges the installed standard library into every analysis, so
`import ... from std::...` resolves and diagnostics for `std::` symbols are
suppressed (only the open file's diagnostics are shown).

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

## Known limitations (v1)

- Go-to-definition resolves module-level symbols only (not block-local `let`s).
- Field-name hover targeting is approximate.
- Completion over-offers (no flow-sensitive local scoping yet).
- Nested block comments are not fully nested by the TextMate grammar (cosmetic).
