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

# The music. fetch-music.sh downloads the Notebook for Anna Magdalena Bach
# into music/notebook/ (gitignored, ~270 KB) so that each level plays the next
# piece of it. Entirely optional, and its failure is not this script's: with
# no network the game plays its own minuet on every level, which is what it
# did before the notebook existed. Set FF_NO_MUSIC_FETCH=1 to skip it.
if [ -z "${FF_NO_MUSIC_FETCH:-}" ]; then
    demos/fuzzel-fobble/fetch-music.sh || true
fi

make -s
FCC="$(make -s print-bin)"

# Arguments are forwarded to the game, so `./run-sdl2.sh --music other.mid`
# plays a different Standard MIDI File. Paths are relative to the repository
# root, which this script has already changed to.
#
# The source list and the backend flag live in sdl2.rsp, so this script and
# `fcc --lsp` compile exactly the same unit.
RSP="@demos/fuzzel-fobble/sdl2.rsp"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OUTDIR="${TEMP:-/tmp}"
        "$FCC" "$RSP" -o "$OUTDIR/fuzzel-fobble-sdl2.c"
        gcc -std=c11 -O2 -Wall -Werror -Dmain=SDL_main -o "$OUTDIR/fuzzel-fobble-sdl2.exe" "$OUTDIR/fuzzel-fobble-sdl2.c" -lmingw32 -lSDL2main -lSDL2 -lm
        echo "Running Fuzzel Fobble (SDL2)..."
        "$OUTDIR/fuzzel-fobble-sdl2.exe" "$@"
        echo "[exit: $?]"
        ;;
    *)
        "$FCC" "$RSP" -o /tmp/fuzzel-fobble-sdl2.c
        cc -std=c11 -O2 -Wall -Werror -o /tmp/fuzzel-fobble-sdl2-bin /tmp/fuzzel-fobble-sdl2.c -lSDL2 -lm
        echo "Running Fuzzel Fobble (SDL2)..."
        /tmp/fuzzel-fobble-sdl2-bin "$@"
        echo "[exit: $?]"
        ;;
esac
