#!/bin/bash
# Build and run fello. Auto-detects host OS.
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

# The source list lives in lsp.rsp, so this script and `fcc --lsp` compile
# exactly the same unit.
RSP="@demos/fello/lsp.rsp"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" "$RSP" -o "$OUTDIR/fello.c"
        gcc -std=c11 -Wall -Werror -O2 -o "$OUTDIR/fello.exe" "$OUTDIR/fello.c"
        "$OUTDIR/fello.exe" "$@"
        ;;
    *)
        "$FCC" "$RSP" -o /tmp/fello.c
        cc -std=c11 -Wall -Werror -O2 -o /tmp/fello-bin /tmp/fello.c
        /tmp/fello-bin "$@"
        ;;
esac
