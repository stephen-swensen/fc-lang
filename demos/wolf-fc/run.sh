#!/bin/bash
# Build and run Wolfenstein FC. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s fc

SRCS="demos/shared/sdl2.fc demos/wolf-fc/main.fc \
      stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/math.fc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        ./fc $SRCS -o "$OUTDIR/wolf-fc.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/wolf-fc.exe" "$OUTDIR/wolf-fc.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Wolfenstein FC..."
        "$OUTDIR/wolf-fc.exe"
        echo "[exit: $?]"
        ;;
    *)
        ./fc $SRCS -o /tmp/wolf-fc.c
        cc -std=c11 -Wall -Werror -o /tmp/wolf-fc-bin /tmp/wolf-fc.c -lSDL2 -lm
        echo "Running Wolfenstein FC..."
        /tmp/wolf-fc-bin
        echo "[exit: $?]"
        ;;
esac
