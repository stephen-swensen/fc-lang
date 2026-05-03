#!/bin/bash
# Build and run fing. Linux only — fing uses ICMP raw sockets, gettimeofday,
# and timeval-based SO_RCVTIMEO that don't directly map to Windows. On Windows
# the FC build still produces a binary, but it just prints a friendly message
# and exits 1.
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" demos/fing/main.fc stdlib/net.fc stdlib/io.fc stdlib/sys.fc -o "$OUTDIR/fing.c"
        gcc -std=c11 -Wall -Werror -o "$OUTDIR/fing.exe" "$OUTDIR/fing.c" -lws2_32
        "$OUTDIR/fing.exe" "$@"
        ;;
    *)
        "$FCC" demos/fing/main.fc stdlib/net.fc stdlib/io.fc stdlib/sys.fc -o /tmp/fing.c
        cc -std=c11 -Wall -Werror -o /tmp/fing-bin /tmp/fing.c

        # ICMP sockets need cap_net_raw (same as /usr/bin/ping)
        if ! getcap /tmp/fing-bin 2>/dev/null | grep -q cap_net_raw; then
            sudo setcap cap_net_raw+ep /tmp/fing-bin
        fi

        /tmp/fing-bin "$@"
        ;;
esac
