#!/bin/bash
set -e

fc_flags=()
fc_files=()

while [ $# -gt 0 ]; do
    case "$1" in
        --flag)
            fc_flags+=("--flag" "$2")
            shift 2
            ;;
        *)
            fc_files+=("$1")
            shift
            ;;
    esac
done

if [ ${#fc_files[@]} -eq 0 ]; then
    echo "usage: ./run.sh [--flag name ...] file.fc [file2.fc ...]"
    exit 1
fi

# Build compiler if needed
make -s fc

tmpdir=$(mktemp -d /tmp/fc-run.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

./fc "${fc_files[@]}" stdlib/* "${fc_flags[@]}" -o "$tmpdir/out.c"
cc -std=c11 -Wall -Werror -o "$tmpdir/out" "$tmpdir/out.c"
set +e
"$tmpdir/out"
echo "[exit: $?]"
