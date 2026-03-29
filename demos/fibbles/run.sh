#!/bin/bash
set -e
cd "$(dirname "$0")/../.."
make -s fc
./fc demos/fibbles/sdl.fc demos/fibbles/main.fc stdlib/io.fc -o /tmp/fibbles.c
cc -std=c11 -Wall -Werror -o /tmp/fibbles /tmp/fibbles.c -lSDL2 -lm
echo "Running Fibbles..."
/tmp/fibbles
echo "[exit: $?]"
