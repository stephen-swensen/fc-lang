#!/bin/bash
# Run Fibbles on Windows using MSYS2 (MINGW64 shell recommended).
# Requires: pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-gcc
set -e
cd "$(dirname "$0")/../.."
make -s fc
OUTDIR="${TEMP:-/tmp}"
./fc demos/fibbles/sdl.fc demos/fibbles/main.fc stdlib/io.fc stdlib/text.fc stdlib/sys.fc -o "$OUTDIR/fibbles.c"
gcc -std=c11 -Wall -Werror -o "$OUTDIR/fibbles.exe" "$OUTDIR/fibbles.c" $(pkg-config --cflags --libs sdl2) -lm
echo "Running Fibbles..."
"$OUTDIR/fibbles.exe"
echo "[exit: $?]"
