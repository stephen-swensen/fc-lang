#!/usr/bin/env bash
# Run the interactive session's tests. Invoked by `make test-repl`.
set -euo pipefail

cd "$(dirname "$0")/../.."

if ! command -v python3 >/dev/null 2>&1; then
    echo "test-repl: python3 is required" >&2
    exit 1
fi

# FCC overrides the binary (make test-asan passes the sanitizer build).
BIN="${FCC:-$(make -s print-bin)}"
if [ ! -x "$BIN" ]; then
    echo "test-repl: $BIN not built (run make first)" >&2
    exit 1
fi

# The repo's stdlib, whatever is installed.
export FCC_STDLIB_DIR="$(pwd)/stdlib"

python3 tests/repl/repl_test.py "$BIN"
