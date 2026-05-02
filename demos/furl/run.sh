#!/bin/bash
# Build and run furl. Auto-detects host OS; on Windows (MSYS2/MinGW) it links
# Winsock via -lws2_32.
set -e
cd "$(dirname "$0")/../.."
make -s

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        ./fcc demos/furl/main.fc stdlib/net.fc stdlib/io.fc stdlib/sys.fc stdlib/text.fc -o "$OUTDIR/furl.c"
        gcc -std=c11 -Wall -Werror -o "$OUTDIR/furl.exe" "$OUTDIR/furl.c" -lws2_32
        "$OUTDIR/furl.exe" "$@"
        ;;
    *)
        ./fcc demos/furl/main.fc stdlib/net.fc stdlib/io.fc stdlib/sys.fc stdlib/text.fc -o /tmp/furl.c
        cc -std=c11 -Wall -Werror -o /tmp/furl-bin /tmp/furl.c
        /tmp/furl-bin "$@"
        ;;
esac
