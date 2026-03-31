#!/bin/bash
set -e
cd "$(dirname "$0")/../.."
make -s fc
./fc demos/fing/main.fc stdlib/net.fc stdlib/io.fc stdlib/sys.fc -o /tmp/fing.c
cc -std=c11 -Wall -Werror -o /tmp/fing-bin /tmp/fing.c

# ICMP sockets need cap_net_raw (same as /usr/bin/ping)
if ! getcap /tmp/fing-bin 2>/dev/null | grep -q cap_net_raw; then
    sudo setcap cap_net_raw+ep /tmp/fing-bin
fi

/tmp/fing-bin "$@"
