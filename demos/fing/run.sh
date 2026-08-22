#!/bin/bash
# Build and run fing. Linux only — fing uses ICMP raw sockets, gettimeofday,
# and timeval-based SO_RCVTIMEO that don't directly map to Windows. On Windows
# the FC build still produces a binary, but it just prints a friendly message
# and exits 1.
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

# The source list lives in lsp.rsp, so this script and `fcc --lsp` compile
# exactly the same unit.
RSP="@demos/fing/lsp.rsp"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" "$RSP" -o "$OUTDIR/fing.c"
        gcc -std=c11 -O2 -Wall -Werror -o "$OUTDIR/fing.exe" "$OUTDIR/fing.c" -lws2_32
        "$OUTDIR/fing.exe" "$@"
        ;;
    *)
        "$FCC" "$RSP" -o /tmp/fing.c
        cc -std=c11 -O2 -Wall -Werror -o /tmp/fing-bin /tmp/fing.c

        # ICMP sockets need cap_net_raw (same as /usr/bin/ping)
        if ! getcap /tmp/fing-bin 2>/dev/null | grep -q cap_net_raw; then
            sudo setcap cap_net_raw+ep /tmp/fing-bin
        fi

        /tmp/fing-bin "$@"
        ;;
esac
