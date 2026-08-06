#!/bin/bash
# Build and run Face Invaders. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

# The source list lives in lsp.rsp, so this script and `fcc --lsp` compile
# exactly the same unit.
RSP="@demos/face-invaders/lsp.rsp"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" "$RSP" -o "$OUTDIR/face-invaders.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/face-invaders.exe" "$OUTDIR/face-invaders.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Face Invaders..."
        "$OUTDIR/face-invaders.exe"
        echo "[exit: $?]"
        ;;
    *)
        "$FCC" "$RSP" -o /tmp/face-invaders.c
        cc -std=c11 -Wall -Werror -o /tmp/face-invaders-bin /tmp/face-invaders.c -lSDL2 -lm
        echo "Running Face Invaders..."
        /tmp/face-invaders-bin
        echo "[exit: $?]"
        ;;
esac
