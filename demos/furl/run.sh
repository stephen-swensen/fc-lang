#!/bin/bash
set -e
cd "$(dirname "$0")/../.."
make -s fc
./fc demos/furl/main.fc stdlib/net.fc stdlib/io.fc stdlib/sys.fc stdlib/text.fc -o /tmp/furl.c
cc -std=c11 -Wall -Werror -o /tmp/furl-bin /tmp/furl.c
/tmp/furl-bin "$@"
