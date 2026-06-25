#!/usr/bin/env bash
# Run the LSP server wire tests. Invoked by `make test-lsp`.
set -euo pipefail

cd "$(dirname "$0")/../.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "test-lsp: python3 is required" >&2
    exit 1
fi

BIN="$(make -s print-bin)"
if [ ! -x "$BIN" ]; then
    echo "test-lsp: $BIN not built (run make first)" >&2
    exit 1
fi

# Use the repo-relative stdlib so std:: imports resolve regardless of install.
export FCC_STDLIB_DIR="$(pwd)/stdlib"

python3 tests/lsp/lsp_test.py "$BIN"
