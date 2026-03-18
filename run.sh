#!/bin/bash
set -e

if [ $# -eq 0 ]; then
    echo "usage: ./run.sh file.fc [file2.fc ...]"
    exit 1
fi

# Build compiler if needed
make -s fc

tmpdir=$(mktemp -d /tmp/fc-run.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

./fc "$@" -o "$tmpdir/out.c"
cc -std=c11 -Wall -Werror -o "$tmpdir/out" "$tmpdir/out.c"
set +e
"$tmpdir/out"
echo "[exit: $?]"
