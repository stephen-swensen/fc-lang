#pragma once

/* Run the FC language server: speak LSP (JSON-RPC 2.0 over stdio, Content-Length
 * framed) until the client sends `exit`. Invoked from main() for `fcc --lsp`.
 * Returns the process exit code. */
int lsp_main(void);
