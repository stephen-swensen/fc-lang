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

# Build compiler if needed. The binary path is per-OS (build/linux/, build/
# windows/, ...), so we ask Make for it instead of hard-coding it — that
# keeps a shared source tree across e.g. WSL + MSYS2 from cross-execing
# each other's binaries.
make -s
FCC="$(make -s print-bin)"

tmpdir=$(mktemp -d /tmp/fc-run.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

"$FCC" "${fc_files[@]}" stdlib/* "${fc_flags[@]}" -o "$tmpdir/out.c"
src_dir=$(dirname "${fc_files[0]}")
cc -std=c11 -Wall -Werror -I "$src_dir" -o "$tmpdir/out" "$tmpdir/out.c" -lm
set +e
"$tmpdir/out"
echo "[exit: $?]"
