#!/bin/bash
# Build and run Fario (raylib).
#
# raylib is fetched from source on first run, built into a static library
# under demos/shared/raylib/, and linked in — that directory is gitignored,
# and the cache is shared with every other raylib demo, so if you have run
# one of those this step is already done. Delete the directory to force a
# clean re-fetch.
#
# What you need from the system is what raylib itself links against:
#   Linux:   OpenGL + X11 dev packages
#            Debian/Ubuntu: libgl1-mesa-dev libx11-dev libxrandr-dev \
#                           libxinerama-dev libxcursor-dev libxi-dev
#   Windows: MSYS2 UCRT64 / MINGW64 — nothing extra
#   macOS:   Xcode command line tools
set -e
cd "$(dirname "$0")/../.."

RAYLIB_VERSION=5.5
RAYLIB_URL="https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz"
RAYLIB_DIR="demos/shared/raylib"
RAYLIB_SRC="${RAYLIB_DIR}/raylib-${RAYLIB_VERSION}/src"

# The modules the demo actually uses; rmodels (the 3D half) stays out.
MODULES=(rcore rshapes rtextures rtext utils rglfw raudio)

# --- Platform ---------------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        OS=windows
        RL_DEFS=()
        RL_LIBS=(-lopengl32 -lgdi32 -lwinmm)
        OUTDIR="${TEMP:-/tmp}"
        BIN="$OUTDIR/fario.exe"
        ;;
    Darwin)
        OS=macos
        RL_DEFS=()
        RL_LIBS=(-framework OpenGL -framework Cocoa -framework IOKit
                 -framework CoreVideo -framework CoreAudio -framework AudioToolbox)
        OUTDIR=/tmp
        BIN="$OUTDIR/fario-bin"
        ;;
    *)
        OS=linux
        RL_DEFS=(-D_GLFW_X11)
        RL_LIBS=(-lGL -lm -lpthread -ldl -lrt -lX11)
        OUTDIR=/tmp
        BIN="$OUTDIR/fario-bin"
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

# --- Build and run the demo -------------------------------------------------
make -s
FCC="$(make -s print-bin)"

"$FCC" "@demos/fario/fario.rsp" -o "$OUTDIR/fario.c"
cc -std=c11 -Wall -Werror -I"$RAYLIB_SRC" \
   -o "$BIN" "$OUTDIR/fario.c" "$LIB" "${RL_LIBS[@]}"

# Arguments are forwarded to the game — e.g. `./run.sh --level 4` opens the
# title with the level picker parked on world 4.
echo "Running Fario..."
"$BIN" "$@"
echo "[exit: $?]"
