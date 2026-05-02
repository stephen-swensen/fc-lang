#!/bin/bash
# Build and run Face Invaders. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s

SRCS="demos/shared/sdl2.fc demos/face-invaders/main.fc \
      stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/math.fc stdlib/random.fc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        ./fcc $SRCS -o "$OUTDIR/face-invaders.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/face-invaders.exe" "$OUTDIR/face-invaders.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Face Invaders..."
        "$OUTDIR/face-invaders.exe"
        echo "[exit: $?]"
        ;;
    *)
        ./fcc $SRCS -o /tmp/face-invaders.c
        cc -std=c11 -Wall -Werror -o /tmp/face-invaders-bin /tmp/face-invaders.c -lSDL2 -lm
        echo "Running Face Invaders..."
        /tmp/face-invaders-bin
        echo "[exit: $?]"
        ;;
esac
