#!/bin/bash
# Build and run Fasteroids. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s

SRCS="demos/shared/sdl2.fc demos/fasteroids/main.fc \
      stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/math.fc stdlib/random.fc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        ./fcc $SRCS -o "$OUTDIR/fasteroids.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/fasteroids.exe" "$OUTDIR/fasteroids.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Fasteroids..."
        "$OUTDIR/fasteroids.exe"
        echo "[exit: $?]"
        ;;
    *)
        ./fcc $SRCS -o /tmp/fasteroids.c
        cc -std=c11 -Wall -Werror -o /tmp/fasteroids-bin /tmp/fasteroids.c -lSDL2 -lm
        echo "Running Fasteroids..."
        /tmp/fasteroids-bin
        echo "[exit: $?]"
        ;;
esac
