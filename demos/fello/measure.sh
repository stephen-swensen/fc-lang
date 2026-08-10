#!/bin/bash
# Regenerate demos/fello/stats.txt: build fello at -O2 and measure it on disk
# and in memory, against a byte-for-byte equivalent C program as the control.
#
# Linux only — the in-memory numbers come from wait4() rusage (via
# /usr/bin/time) and /proc/self/smaps_rollup, neither of which exists on
# Windows. Everything is built in a scratch directory; nothing but stats.txt
# lands in the repo.
set -e
cd "$(dirname "$0")/../.."
HERE="demos/fello"
OUT="$HERE/stats.txt"

make -s
FCC="$(make -s print-bin)"

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT

RUNS=25       # samples per max-RSS figure
SMAPS=9       # samples per smaps figure (median taken; page-granular and noisy)

# --- The three programs -----------------------------------------------------
# fello:  the FC demo.
# hello:  the same program hand-written in C — the control for "what does FC
#         add over the C you would have written".
# nul:    int main(void){return 0;} — the floor: process startup, no work.
"$FCC" "@$HERE/lsp.rsp" -o "$W/fello.c"
printf '#include <stdio.h>\nint main(void) { fwrite("Hello, world!\\n", 1, 14, stdout); return 0; }\n' > "$W/hello.c"
printf 'int main(void) { return 0; }\n' > "$W/nul.c"

for p in fello hello nul; do
    cc -std=c11 -Wall -Werror -O2 -o "$W/$p" "$W/$p.c"
    cp "$W/$p" "$W/$p.stripped"; strip "$W/$p.stripped"
    cc -std=c11 -Wall -Werror -O2 -static -o "$W/$p.static" "$W/$p.c"
done

# --- Memory probe -----------------------------------------------------------
# A destructor in a preloaded .so dumps /proc/self/smaps_rollup after main
# returns, which is where a program this short is at its peak. It adds its own
# mapping to every process it measures, so its numbers are for comparing the
# three programs against each other; the absolute peak comes from rusage
# below, measured with no probe attached.
cat > "$W/probe.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
static void dump(void) __attribute__((destructor));
static void dump(void) {
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "Rss:", 4) || !strncmp(line, "Pss:", 4) ||
            !strncmp(line, "Private_Dirty:", 14) || !strncmp(line, "Shared_Clean:", 13))
            fputs(line, stderr);
    fclose(f);
}
EOF
cc -std=c11 -O2 -shared -fPIC -o "$W/probe.so" "$W/probe.c"

# --- Measurement primitives -------------------------------------------------
# (awk here is mawk on Ubuntu — no gawk extensions.)
bytes() { stat -c %s "$W/$1"; }
sec()   { size -A "$W/$1" | awk -v s="$2" '$1==s{print $2; f=1} END{if(!f) print 0}'; }
# Sum of PT_LOAD MemSiz: the address space the image itself needs mapped.
image() {
    local total=0 m
    for m in $(readelf -lW "$W/$1" | awk '$1=="LOAD"{print $6}'); do
        total=$(( total + m ))   # MemSiz prints as 0x...; bash reads that directly
    done
    echo "$total"
}
# Worst peak RSS over RUNS runs, straight from the kernel's rusage.
maxrss() { local m=0 r; for _ in $(seq 1 $RUNS); do r=$(/usr/bin/time -f '%M' "$W/$1" 2>&1 >/dev/null); [ "$r" -gt "$m" ] && m=$r; done; echo "$m"; }
# Median of SMAPS samples of one smaps_rollup field.
smaps() {
    local i
    if [ ! -f "$W/sm.$1" ]; then
        for ((i = 0; i < SMAPS; i++)); do
            LD_PRELOAD="$W/probe.so" "$W/$1" >/dev/null 2>>"$W/sm.$1"
        done
    fi
    awk -v k="$2:" '$1==k{print $2}' "$W/sm.$1" | sort -n |
        awk '{v[n++]=$1} END{ print v[int(n/2)] }'
}
# Mean wall-clock ms of N repetitions of a command.
timeit() { local n=$1; shift; local t; t=$(/usr/bin/time -f '%e' bash -c "for i in \$(seq 1 $n); do $* ; done" 2>&1 | tail -1); awk -v t="$t" -v n="$n" 'BEGIN{printf "%.2f", t*1000/n}'; }

rm -f "$W"/sm.*

# --- Collect ----------------------------------------------------------------
for p in fello hello nul; do
    eval "d_$p=$(bytes $p)"
    eval "ds_$p=$(bytes $p.stripped)"
    eval "tx_$p=$(sec $p .text)"
    eval "ro_$p=$(sec $p .rodata)"
    eval "da_$p=$(sec $p .data)"
    eval "bs_$p=$(sec $p .bss)"
    eval "im_$p=$(image $p)"
    eval "st_$p=$(bytes $p.static)"
    eval "stx_$p=$(sec $p.static .text)"
    eval "rss_$p=$(maxrss $p)"
    eval "srss_$p=$(maxrss $p.static)"
    eval "mrss_$p=$(smaps $p Rss)"
    eval "mpss_$p=$(smaps $p Pss)"
    eval "mpd_$p=$(smaps $p Private_Dirty)"
    eval "msc_$p=$(smaps $p Shared_Clean)"
done

t_fcc=$(timeit 100 "'$FCC' '@$HERE/lsp.rsp' -o '$W/t.c'")
t_cc=$(timeit 20 "cc -std=c11 -O2 -o '$W/t.bin' '$W/fello.c'")
t_run=$(timeit 1000 "'$W/fello' >/dev/null")
t_nul=$(timeit 1000 "'$W/nul'")
rss_fccbin=$(/usr/bin/time -f '%M' "$FCC" "@$HERE/lsp.rsp" -o "$W/t.c" 2>&1 | tail -1)

funcs=$(nm --defined-only "$W/fello" | awk '$2=="T"||$2=="t"{print $3}' | sort | tr '\n' ' ' | fold -sw 66 | sed 's/^/  /')
nconst=$(nm "$W/fello" | grep -c ' R fc__')

# --- Report -----------------------------------------------------------------
hdr() { printf '  %-20s %11s %11s %11s %13s\n' "" "fello" "hello (C)" "nul" "fello - hello"; }
row() { printf '  %-20s %11s %11s %11s %13s\n' "$1" "$2 $5" "$3 $5" "$4 $5" "$(printf '%+d' $(( $2 - $3 ))) $5"; }

exec > "$OUT"

cat <<EOF
fello — footprint of a "hello world" FC binary
==============================================

Regenerate with: ./demos/fello/measure.sh   (Linux only)

The program is demos/fello/main.fc: one import of std::io and one io.write
to stdout. It is the floor of the language — what an FC binary costs before
you have written any program.

Two controls are built and measured identically alongside it:

  hello   the same program hand-written in C (fwrite to stdout) — the
          control for what FC costs over the C you'd otherwise write
  nul     int main(void) { return 0; } — process startup with no work,
          the floor beneath both

Measured on
-----------
  host       $(uname -srm)
  libc       $(ldd --version | head -1)
  cc         $(cc --version | head -1)
  ld         $(ld --version | head -1)
  fcc        $(git describe --always --dirty --tags 2>/dev/null || echo unknown)  ($FCC)
  C flags    -std=c11 -Wall -Werror -O2
  date       $(date -u '+%Y-%m-%d %H:%M UTC')


ON DISK
=======

Dynamically linked at -O2 — the default build, what run.sh produces.

EOF
hdr
row "binary"           $d_fello   $d_hello   $d_nul   B
row "binary, stripped" $ds_fello  $ds_hello  $ds_nul  B
row ".text"            $tx_fello  $tx_hello  $tx_nul  B
row ".rodata"          $ro_fello  $ro_hello  $ro_nul  B
row ".data"            $da_fello  $da_hello  $da_nul  B
row ".bss"             $bs_fello  $bs_hello  $bs_nul  B
row "mapped image"     $im_fello  $im_hello  $im_nul  B
cat <<EOF

"mapped image" is the sum of PT_LOAD MemSiz — the address space the
executable itself needs, before libc.

Stripped size is identical for fello and hello, and within a rounding step
of nul's. At this scale an ELF file is headers, program/section tables,
symbol and relocation data and 4 KB segment alignment; the code is the
small part. All three programs' .text fits in the same page, so all three
files come out the same size once the symbol table is gone. The +$(( d_fello - d_hello )) B
unstripped is debug and symbol data for the extra generated functions,
which is not loaded at runtime and vanishes on strip.

Statically linked at -O2 — glibc pulled in whole. Not FC's default; shown
because it separates what this program costs from what libc costs.

EOF
hdr
row "binary"           $st_fello   $st_hello   $st_nul   B
row ".text"            $stx_fello  $stx_hello  $stx_nul  B
cat <<EOF

Shared objects loaded: $(ldd "$W/fello" | wc -l) — $(ldd "$W/fello" | sed 's/^[[:space:]]*//;s/ (0x.*//;s/ =>.*//' | tr '\n' ' ')

FC links nothing of its own. There is no FC runtime library to link
against: the compiler emits C, and the C it emits calls libc.


IN MEMORY
=========

Peak RSS, reported by the kernel from the process's own rusage after it
exits (/usr/bin/time -f %M, worst of $RUNS runs, no probe attached).

EOF
hdr
row "dynamic" $rss_fello  $rss_hello  $rss_nul  KB
row "static"  $srss_fello $srss_hello $srss_nul KB
cat <<EOF

Composition at exit, from /proc/self/smaps_rollup, dynamic build, median
of $SMAPS runs. A destructor in a preloaded .so reads the rollup after main
returns; that .so inflates all three columns by the same constant, so read
these across the row, not as absolutes.

EOF
hdr
row "RSS"           $mrss_fello $mrss_hello $mrss_nul KB
row "PSS"           $mpss_fello $mpss_hello $mpss_nul KB
row "private dirty" $mpd_fello  $mpd_hello  $mpd_nul  KB
row "shared clean"  $msc_fello  $msc_hello  $msc_nul  KB
cat <<EOF

Do not read the delta column here as signal. These figures are
page-granular — the smallest difference they can express is one 4 KB page —
and run-to-run jitter is exactly that size: run measure.sh twice and the
fello-minus-hello delta lands on +4 KB, 0 and -4 KB with no pattern, in
both the rusage and smaps tables. The reading is not "FC costs N KB more
than C"; it is that FC and C are indistinguishable in memory here. The FC
program's extra $(( tx_fello - tx_hello )) bytes of code and $(( ro_fello - ro_hello )) bytes of constants do not add a
page to anything.

Nearly all of the RSS is shared_clean: glibc and the dynamic loader,
mapped from page cache, shared with every other process on the machine,
paid for once system-wide. PSS (RSS with each shared page divided among
its sharers) and private dirty are what this process actually costs, and
they are within a page of an empty C program's. The static rows show the
same thing from the other side — no loader, no libc mapping, less than
half the RSS, in exchange for a $(( st_fello / d_fello ))x larger file.

The program touches the heap zero times: no allocator call, no arena, no
GC. Its one runtime allocation is the argv -> str[] slice FC's entry
wrapper builds with alloca — on the stack, sized to argc, gone on return.


BUILD AND RUN TIME
==================

  fcc   main.fc + stdlib/io.fc -> C         $(printf '%7s' $t_fcc) ms   (fcc peak RSS $rss_fccbin KB)
  cc    -O2, generated C -> binary          $(printf '%7s' $t_cc) ms
  run   fork + exec + write + exit          $(printf '%7s' $t_run) ms   (mean of 1000)
  run   the nul control, same loop          $(printf '%7s' $t_nul) ms   (fork/exec floor)

The run figures include the shell's fork and exec, which is most of what
they measure — the nul row is that floor. fcc is ~$(awk -v a="$t_cc" -v b="$t_fcc" 'BEGIN{printf "%d", a/b}')x faster than the C
compiler it feeds, on the same input.


CODE SIZE
=========

  main.fc                    $(printf '%5s' "$(wc -l < $HERE/main.fc)") lines  $(printf '%7s' "$(stat -c %s $HERE/main.fc)") B
  stdlib/io.fc, merged in    $(printf '%5s' "$(wc -l < stdlib/io.fc)") lines  $(printf '%7s' "$(stat -c %s stdlib/io.fc)") B
  generated C                $(printf '%5s' "$(wc -l < $W/fello.c)") lines  $(printf '%7s' "$(stat -c %s $W/fello.c)") B

fcc emits an imported module whole rather than tree-shaking it: every
function of stdlib/io.fc — open, read_all, list_dir, all of it — is in the
generated C. It emits them as 'static __attribute__((unused))', so the C
compiler discards the unreferenced ones and the linker never sees them.
$(wc -l < $W/fello.c) lines of C become $tx_fello bytes of .text, and the only functions
left in the binary are crt's plus one:

$funcs

io.write and fc_main were both inlined into that main.

One thing survives DCE that a reader might not predict: io.fc's
module-level constants — io.seek_set/cur/end and the four io.file error
codes — are emitted as file-scope C globals with external linkage, so the
linker must keep them even though nothing reads them. $(( nconst * 4 )) bytes of
.rodata across $nconst constants. That is the entire residue of importing
std::io and calling one function from it.


WHERE THE DELTA OVER C GOES
===========================

fello's .text is $(( tx_fello - tx_hello )) bytes larger than the hand-written C control's, and
that is the whole difference between them. It isn't overhead in the sense
of a runtime — FC has none — it is one thing fello does that hello does
not. FC's main takes str[], so the entry wrapper walks argv, calls strlen
on each element, and builds a slice before calling into FC code:

  int main(int argc, char **argv) {
      fc_str *_args = alloca((size_t)argc * sizeof(fc_str));
      for (int _i = 0; _i < argc; _i++) {
          _args[_i].ptr = (uint8_t*)argv[_i];
          _args[_i].len = (int64_t)strlen(argv[_i]);
      }
      return fc_main((fc_slice_fc_str){ .ptr = _args, .len = argc });
  }

The io.write call itself compiles to the same fwrite the C control calls
directly. There is no allocator, no runtime init, no GC, no exception
machinery, no type metadata, no startup hook: the binary's constructor and
destructor lists are the ones crt puts in any C program.
EOF

exec >&2
echo "wrote $OUT"
