#!/bin/bash
# Build and run Folitaire. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

SRCS="demos/shared/sdl2.fc demos/folitaire/main.fc \
      stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/random.fc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" $SRCS -o "$OUTDIR/folitaire.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/folitaire.exe" "$OUTDIR/folitaire.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Folitaire..."
        "$OUTDIR/folitaire.exe"
        echo "[exit: $?]"
        ;;
    *)
        "$FCC" $SRCS -o /tmp/folitaire.c
        cc -std=c11 -Wall -Werror -o /tmp/folitaire-bin /tmp/folitaire.c -lSDL2 -lm
        echo "Running Folitaire..."
        /tmp/folitaire-bin
        echo "[exit: $?]"
        ;;
esac
