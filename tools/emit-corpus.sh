#!/bin/bash
# Compile every test and demo to C and save the C and fcc's stderr, so a
# refactor can be checked for byte-identical output:
#
#   tools/emit-corpus.sh /tmp/before              # on the old compiler
#   ... change the compiler, make ...
#   tools/emit-corpus.sh /tmp/after
#   diff -r /tmp/before /tmp/after
#
# Extra @response files (another project's lsp.rsp, say) are emitted too:
#   tools/emit-corpus.sh /tmp/before @../wolf-fc/lsp.rsp
# Paths and line numbers embedded in the output change when sources move, so
# compare runs of the same checkout. FCC overrides the compiler.
set -e
cd "$(dirname "$0")/.."
OUT="${1:?usage: tools/emit-corpus.sh OUTDIR [@extra.rsp ...]}"
shift
FCC="${FCC:-$(make -s print-bin)}"
mkdir -p "$OUT"

emit() {  # name, fcc args...
    local slug="${1//\//.}"
    shift
    "$FCC" "$@" -o "$OUT/$slug.c" 2> "$OUT/$slug.err" && rc=0 || rc=$?
    echo "exit=$rc" >> "$OUT/$slug.err"
}
export -f emit
export FCC OUT

{
    bash tests/run_tests.sh --list | cut -d'|' -f1,2
    for rsp in demos/*/lsp.rsp; do
        echo "demo/$(basename "$(dirname "$rsp")")|@$rsp"
    done
    for rsp in "$@"; do
        echo "extra/$(basename "$(dirname "${rsp#@}")")|$rsp"
    done
} | xargs -P "$(nproc)" -I {} bash -c 'IFS="|" read -r name args <<< "{}"; emit "$name" $args'
echo "$(ls "$OUT" | grep -c '\.c$') C files in $OUT"
