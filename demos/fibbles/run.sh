#!/bin/bash
# Build and run Fibbles. Auto-detects host OS and picks the right SDL2 link line.
#
# Linux: requires SDL2 dev package (e.g. libsdl2-dev on Debian/Ubuntu).
# Windows (MSYS2 UCRT64 or MINGW64): requires SDL2 for your environment, e.g.:
#   UCRT64:   pacman -S mingw-w64-ucrt-x86_64-SDL2
#   MINGW64:  pacman -S mingw-w64-x86_64-SDL2
set -e
cd "$(dirname "$0")/../.."
make -s

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        ./fcc demos/shared/sdl2.fc demos/fibbles/main.fc stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/random.fc -o "$OUTDIR/fibbles.c"
        gcc -std=c11 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/fibbles.exe" "$OUTDIR/fibbles.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Fibbles..."
        "$OUTDIR/fibbles.exe"
        echo "[exit: $?]"
        ;;
    *)
        ./fcc demos/shared/sdl2.fc demos/fibbles/main.fc stdlib/io.fc stdlib/text.fc stdlib/sys.fc stdlib/random.fc -o /tmp/fibbles.c
        cc -std=c11 -Wall -Werror -o /tmp/fibbles-bin /tmp/fibbles.c -lSDL2 -lm
        echo "Running Fibbles..."
        /tmp/fibbles-bin
        echo "[exit: $?]"
        ;;
esac
