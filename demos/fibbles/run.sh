#!/bin/bash
set -e
cd "$(dirname "$0")/../.."
make -s fc
./fc demos/fibbles/sdl.fc demos/fibbles/main.fc stdlib/io.fc stdlib/text.fc stdlib/sys.fc -o /tmp/fibbles.c
cc -std=c11 -Wall -Werror -o /tmp/fibbles-bin /tmp/fibbles.c -lSDL2 -lm
echo "Running Fibbles..."
/tmp/fibbles-bin
echo "[exit: $?]"
