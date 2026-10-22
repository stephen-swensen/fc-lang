#!/bin/bash
# The native backend's linker (src/elflink.c) on what the suite's programs
# never give it: C objects, archives, constructors, copy relocations, and the
# inputs it refuses. Each program in tests/link/<name>/ is linked by fcc and,
# from `fcc --native -c`, by cc; both executables must print the case's
# `expected` file. The C files are compiled with $CC -fPIE, as a C compiler
# compiles them by default for a position-independent executable.
#
# Environment: CC (default cc), FCC (default the one `make` builds). Run it
# from the repository root; `make test-native` runs it after the suite.
set -u

FCC="${FCC:-$(make -s print-bin)}"
CC="${CC:-cc}"
LINK=tests/link
IO=stdlib/io.fc
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
passed=0
failed=0

pass() { echo "  PASS  $1"; passed=$((passed + 1)); }
fail() {
    echo "  FAIL  $1 ($2)"
    [ -n "${3:-}" ] && [ -s "$3" ] && sed 's/^/    /' "$3"
    failed=$((failed + 1))
}

cobj() { "$CC" -fPIE -O1 -c "$@"; }

# same NAME FC_SOURCES LINK_INPUTS: link both ways, run both, compare each
# with the expected output.
same() {
    local name=$1 fc=$2 link=$3
    local log="$W/$name.log"
    if ! $FCC --native $fc $link -o "$W/$name.fcc" 2>"$log"; then
        fail "$name" "fcc did not link it" "$log"; return
    fi
    if ! $FCC --native -c $fc -o "$W/$name.o" 2>"$log" ||
       ! "$CC" -o "$W/$name.cc" "$W/$name.o" $link -lm 2>"$log"; then
        fail "$name" "the cc link failed" "$log"; return
    fi
    for how in fcc cc; do
        "$W/$name.$how" >"$W/$name.$how.out" 2>&1
        if ! diff "$LINK/$name/expected" "$W/$name.$how.out" >"$log"; then
            fail "$name" "linked by $how, it printed something else" "$log"; return
        fi
    done
    pass "$name"
}

# refuses NAME MESSAGE ARGS...: fcc must refuse the link, saying MESSAGE.
refuses() {
    local name=$1 msg=$2
    local log="$W/$name.log"
    shift 2
    if $FCC --native "$@" -o "$W/$name" 2>"$log"; then
        fail "$name" "fcc linked it"; return
    fi
    if ! grep -qF -- "$msg" "$log"; then
        echo "expected: $msg" >>"$log"
        fail "$name" "wrong error" "$log"; return
    fi
    [ -e "$W/$name" ] && { fail "$name" "it left an output file"; return; }
    pass "$name"
}

echo "=== Testing the native backend's linker ==="

cobj -fcommon $LINK/objects/clib.c -o "$W/clib.o" &&
cp "$W/clib.o" "$W/clib2.o" &&
cobj -fcommon $LINK/objects/strong.c -o "$W/strong.o" &&
cobj $LINK/environ/envc.c -o "$W/envc.o" &&
cobj $LINK/archive/chain_first.c -o "$W/chain_first.o" &&
cobj $LINK/archive/chain_second.c -o "$W/chain_second.o" &&
cobj $LINK/archive/unused.c -o "$W/unused.o" &&
cobj $LINK/bad/tls.c -o "$W/tls.o" &&
"$CC" -fno-pic -O1 -fcommon -c $LINK/objects/clib.c -o "$W/nopic.o" &&
ar rcs "$W/libchain.a" "$W/unused.o" "$W/chain_second.o" "$W/chain_first.o" ||
    { echo "cannot compile the C parts of the tests"; exit 1; }

same objects "$LINK/objects/main.fc $IO" "$W/clib.o $W/strong.o"
same environ "$LINK/environ/main.fc $IO" "$W/envc.o"
same archive "$LINK/archive/main.fc $IO" "-L$W -lchain"

refuses undefined "undefined symbol 'c_add'" $LINK/objects/main.fc $IO
refuses no_library "cannot find -lfcc_no_such_library" \
    $LINK/archive/main.fc $IO -lfcc_no_such_library
refuses tls "tls.o: thread-local storage is not supported" \
    $LINK/objects/main.fc $IO "$W/tls.o" "$W/clib.o"
refuses not_pic "nopic.o: absolute 32-bit addresses" $LINK/objects/main.fc $IO "$W/nopic.o"
refuses defined_twice "clib2.o: symbol 'c_add' is defined twice" \
    $LINK/objects/main.fc $IO "$W/clib.o" "$W/clib2.o"
# An LTO object holds the compiler's IR, not code (gcc's form only).
if "$CC" -flto -O1 -c $LINK/archive/chain_second.c -o "$W/lto.o" 2>/dev/null &&
   [ "$(head -c 4 "$W/lto.o" | od -An -c | tr -d ' ')" = '177ELF' ] &&
   grep -q gnu.lto_ "$W/lto.o"; then
    refuses lto "lto.o: an LTO object" $LINK/archive/main.fc $IO "$W/chain_first.o" "$W/lto.o"
fi

echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
