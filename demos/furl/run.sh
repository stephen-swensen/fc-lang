#!/bin/bash
# Build and run furl. Auto-detects host OS; on Windows (MSYS2/MinGW) it links
# Winsock via -lws2_32.
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

# The source list lives in lsp.rsp, so this script and `fcc --lsp` compile
# exactly the same unit.
RSP="@demos/furl/lsp.rsp"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" "$RSP" -o "$OUTDIR/furl.c"
        gcc -std=c11 -O2 -Wall -Werror -o "$OUTDIR/furl.exe" "$OUTDIR/furl.c" -lws2_32
        "$OUTDIR/furl.exe" "$@"
        ;;
    *)
        "$FCC" "$RSP" -o /tmp/furl.c
        cc -std=c11 -O2 -Wall -Werror -o /tmp/furl-bin /tmp/furl.c
        /tmp/furl-bin "$@"
        ;;
esac
