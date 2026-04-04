#!/bin/bash
# Run Fibbles on Windows using MSYS2 (UCRT64 or MINGW64 shell).
# Requires SDL2 for your environment, e.g.:
#   UCRT64:   pacman -S mingw-w64-ucrt-x86_64-SDL2
#   MINGW64:  pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s fc
OUTDIR="${TEMP:-/tmp}"
./fc demos/fibbles/sdl.fc demos/fibbles/main.fc stdlib/io.fc stdlib/text.fc stdlib/sys.fc --flag windows -o "$OUTDIR/fibbles.c"
gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/fibbles.exe" "$OUTDIR/fibbles.c" -lmingw32 -lSDL2main -lSDL2 -lm
echo "Running Fibbles..."
"$OUTDIR/fibbles.exe"
echo "[exit: $?]"
