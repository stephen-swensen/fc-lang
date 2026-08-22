#!/bin/bash
# Build and run Fuzzel Fobble on raylib. For the SDL2 build, run run-sdl2.sh
# instead — same game, same sources, one different backend file.
#
# Unlike the SDL2 demos, this one has no system package to install: raylib is
# fetched from source on first run, built into a static library under
# demos/shared/raylib/, and linked in. That directory is gitignored — nothing
# vendored is committed. Delete it to force a clean re-fetch.
#
# First run downloads ~42 MB and takes a minute or so to compile raylib; every
# run after that reuses the cached libraylib.a and is as fast as any other demo.
#
# What you still need from the system is what raylib itself links against:
#   Linux:   OpenGL + X11 dev packages
#            Debian/Ubuntu: libgl1-mesa-dev libx11-dev libxrandr-dev \
#                           libxinerama-dev libxcursor-dev libxi-dev
#   Windows: MSYS2 UCRT64 / MINGW64 — the toolchain's own OpenGL libs, no extra
#            package needed.
#   macOS:   Xcode command line tools (the frameworks ship with the OS).
set -e
cd "$(dirname "$0")/../.."

RAYLIB_VERSION=5.5
RAYLIB_URL="https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz"
RAYLIB_DIR="demos/shared/raylib"
RAYLIB_SRC="${RAYLIB_DIR}/raylib-${RAYLIB_VERSION}/src"

# The modules the demo actually uses. rmodels.c is deliberately absent — it is
# raylib's 3D half, and skipping it also skips every model-format parser it
# drags in. Nothing else in raylib references it.
MODULES=(rcore rshapes rtextures rtext utils rglfw raudio)

# --- Platform ---------------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OS=windows
        # rglfw.c picks _GLFW_WIN32 off _WIN32 by itself.
        RL_DEFS=()
        RL_LIBS=(-lopengl32 -lgdi32 -lwinmm)
        OUTDIR="${TEMP:-/tmp}"
        BIN="$OUTDIR/fuzzel-fobble-raylib.exe"
        ;;
    Darwin)
        OS=macos
        # rglfw.c picks _GLFW_COCOA off __APPLE__, but its GLFW sources are
        # Objective-C, so that one module needs the ObjC front end.
        RL_DEFS=()
        RL_LIBS=(-framework OpenGL -framework Cocoa -framework IOKit
                 -framework CoreVideo -framework CoreAudio -framework AudioToolbox)
        OUTDIR=/tmp
        BIN="$OUTDIR/fuzzel-fobble-raylib-bin"
        ;;
    *)
        OS=linux
        # On Linux, GLFW's backend is not auto-detected: rglfw.c errors out
        # unless one of _GLFW_X11 / _GLFW_WAYLAND is defined. X11 is the
        # portable pick — a Wayland session runs it through XWayland.
        RL_DEFS=(-D_GLFW_X11)
        RL_LIBS=(-lGL -lm -lpthread -ldl -lrt -lX11)
        OUTDIR=/tmp
        BIN="$OUTDIR/fuzzel-fobble-raylib-bin"
        ;;
esac

OBJDIR="${RAYLIB_DIR}/build/${OS}"
LIB="${OBJDIR}/libraylib.a"

# --- Fetch ------------------------------------------------------------------
if [ ! -f "${RAYLIB_SRC}/raylib.h" ]; then
    echo "Fetching raylib ${RAYLIB_VERSION} into ${RAYLIB_DIR}/ ..."
    mkdir -p "$RAYLIB_DIR"
    TARBALL="${RAYLIB_DIR}/raylib-${RAYLIB_VERSION}.tar.gz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$TARBALL" "$RAYLIB_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$TARBALL" "$RAYLIB_URL"
    else
        echo "error: need curl or wget to fetch raylib" >&2
        exit 1
    fi
    # Only src/ and the licence — the rest of the repo is examples, projects
    # and packaging we never build.
    tar xzf "$TARBALL" -C "$RAYLIB_DIR" \
        "raylib-${RAYLIB_VERSION}/src" "raylib-${RAYLIB_VERSION}/LICENSE"
    rm -f "$TARBALL"
    echo "Fetched raylib ${RAYLIB_VERSION} (zlib/libpng licence, see ${RAYLIB_DIR}/raylib-${RAYLIB_VERSION}/LICENSE)"
fi

# --- Build raylib -----------------------------------------------------------
if [ ! -f "$LIB" ]; then
    echo "Building raylib (${OS}, once — cached in ${OBJDIR}/) ..."
    mkdir -p "$OBJDIR"
    RL_CFLAGS=(-std=gnu99 -O2 -w
               -DPLATFORM_DESKTOP_GLFW -DGRAPHICS_API_OPENGL_33
               "${RL_DEFS[@]}"
               -I"$RAYLIB_SRC" -I"$RAYLIB_SRC/external/glfw/include")
    pids=()
    for m in "${MODULES[@]}"; do
        objc=()
        if [ "$OS" = macos ] && [ "$m" = rglfw ]; then
            objc=(-x objective-c)
        fi
        cc "${RL_CFLAGS[@]}" "${objc[@]}" \
           -c -o "$OBJDIR/$m.o" "$RAYLIB_SRC/$m.c" &
        pids+=($!)
    done
    fail=0
    for p in "${pids[@]}"; do wait "$p" || fail=1; done
    if [ "$fail" -ne 0 ]; then
        echo "error: raylib failed to build — check the dev packages listed at the top of this script" >&2
        rm -rf "$OBJDIR"
        exit 1
    fi
    ar rcs "$LIB" "$OBJDIR"/*.o
fi

# --- Music ------------------------------------------------------------------
# The music. fetch-music.sh downloads the Notebook for Anna Magdalena Bach
# into music/notebook/ (gitignored, ~270 KB) so that each level plays the next
# piece of it. Entirely optional, and its failure is not this script's: with
# no network the game plays its own minuet on every level, which is what it
# did before the notebook existed. Set FF_NO_MUSIC_FETCH=1 to skip it.
if [ -z "${FF_NO_MUSIC_FETCH:-}" ]; then
    demos/fuzzel-fobble/fetch-music.sh || true
fi

# --- Build and run the demo -------------------------------------------------
make -s
FCC="$(make -s print-bin)"

# The source list and the backend flag live in raylib.rsp, so this script
# compiles exactly the unit the editor analyses when lsp.rsp points there. The
# flag also picks opl_audio's raylib device backend.
RSP="@demos/fuzzel-fobble/raylib.rsp"

"$FCC" "$RSP" -o "$OUTDIR/fuzzel-fobble-raylib.c"
cc -std=c11 -O2 -Wall -Werror -I"$RAYLIB_SRC" \
   -o "$BIN" "$OUTDIR/fuzzel-fobble-raylib.c" "$LIB" "${RL_LIBS[@]}"

# Arguments are forwarded to the game, so `./run-raylib.sh --music other.mid`
# plays a different Standard MIDI File. Paths are relative to the repository
# root, which this script has already changed to.
echo "Running Fuzzel Fobble (raylib)..."
"$BIN" "$@"
echo "[exit: $?]"
