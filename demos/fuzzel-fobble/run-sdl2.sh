#!/bin/bash
# Build and run Fuzzel Fobble on SDL2. Auto-detects host OS and picks the right
# SDL2 link line. For the raylib build, run run-raylib.sh instead — same game,
# same sources, one different backend file.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

# The source list and the backend flag live in sdl2.rsp, so this script and
# `fcc --lsp` compile exactly the same unit.
RSP="@demos/fuzzel-fobble/sdl2.rsp"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" "$RSP" -o "$OUTDIR/fuzzel-fobble-sdl2.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/fuzzel-fobble-sdl2.exe" "$OUTDIR/fuzzel-fobble-sdl2.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Fuzzel Fobble (SDL2)..."
        "$OUTDIR/fuzzel-fobble-sdl2.exe"
        echo "[exit: $?]"
        ;;
    *)
        "$FCC" "$RSP" -o /tmp/fuzzel-fobble-sdl2.c
        cc -std=c11 -Wall -Werror -o /tmp/fuzzel-fobble-sdl2-bin /tmp/fuzzel-fobble-sdl2.c -lSDL2 -lm
        echo "Running Fuzzel Fobble (SDL2)..."
        /tmp/fuzzel-fobble-sdl2-bin
        echo "[exit: $?]"
        ;;
esac
