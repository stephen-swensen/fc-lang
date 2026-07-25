#!/bin/bash
# Build and run Fuzzel Fobble. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux:   requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows: MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
#          MSYS2 MINGW64: pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s
FCC="$(make -s print-bin)"

SRCS="demos/shared/sdl2.fc demos/fuzzel-fobble/main.fc \
      stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/math.fc stdlib/random.fc"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" $SRCS -o "$OUTDIR/fuzzel-fobble.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/fuzzel-fobble.exe" "$OUTDIR/fuzzel-fobble.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Fuzzel Fobble..."
        "$OUTDIR/fuzzel-fobble.exe"
        echo "[exit: $?]"
        ;;
    *)
        "$FCC" $SRCS -o /tmp/fuzzel-fobble.c
        cc -std=c11 -Wall -Werror -o /tmp/fuzzel-fobble-bin /tmp/fuzzel-fobble.c -lSDL2 -lm
        echo "Running Fuzzel Fobble..."
        /tmp/fuzzel-fobble-bin
        echo "[exit: $?]"
        ;;
esac
