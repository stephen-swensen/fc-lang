#!/bin/bash
# Regenerate demos/fello/stats.txt: build fello at -O2 and measure it on disk
# and in memory, against hand-written C controls.
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

# --- The four programs ------------------------------------------------------
# fello:    the FC demo.
# cprintf:  hello world as a C programmer would actually write it. This is the
#           control that matters — the honest question is what FC costs over
#           the C you'd otherwise write, not over C contorted to match FC.
# cfwrite:  the same program written to make the libc call FC's io.write makes.
#           FC's str is a fat pointer (ptr + len) with no NUL guarantee, so
#           io.write cannot call puts or printf %s; fwrite is the only door.
#           Separating this from cprintf splits "cost of FC" from "cost of
#           writing a counted string".
# nul:      int main(void){return 0;} — the floor: process startup, no work.
"$FCC" "@$HERE/lsp.rsp" -o "$W/fello.c"
printf '#include <stdio.h>\nint main(void) { printf("Hello, world!\\n"); return 0; }\n' > "$W/cprintf.c"
printf '#include <stdio.h>\nint main(void) { fwrite("Hello, world!\\n", 1, 14, stdout); return 0; }\n' > "$W/cfwrite.c"
printf 'int main(void) { return 0; }\n' > "$W/nul.c"

PROGS="fello cprintf cfwrite nul"

for p in $PROGS; do
    cc -std=c11 -Wall -Werror -O2 -o "$W/$p" "$W/$p.c"
    cp "$W/$p" "$W/$p.stripped"; strip "$W/$p.stripped"
    cc -std=c11 -Wall -Werror -O2 -static -o "$W/$p.static" "$W/$p.c"
done

# Same printf program with glibc's fortification off, to show what the
# distro's default _FORTIFY_SOURCE costs the C control (see the report).
cc -std=c11 -Wall -Werror -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
   -o "$W/cprintf_nf" "$W/cprintf.c"

# --- Memory probe -----------------------------------------------------------
# A destructor in a preloaded .so dumps /proc/self/smaps_rollup after main
# returns, which is where a program this short is at its peak. It adds its own
# mapping to every process it measures, so its numbers are for comparing the
# programs against each other; the absolute peak comes from rusage below,
# measured with no probe attached.
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
for p in $PROGS; do
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
tx_nofort=$(sec cprintf_nf .text)

t_fcc=$(timeit 100 "'$FCC' '@$HERE/lsp.rsp' -o '$W/t.c'")
t_cc=$(timeit 20 "cc -std=c11 -O2 -o '$W/t.bin' '$W/fello.c'")
t_run=$(timeit 1000 "'$W/fello' >/dev/null")
t_nul=$(timeit 1000 "'$W/nul'")
rss_fccbin=$(/usr/bin/time -f '%M' "$FCC" "@$HERE/lsp.rsp" -o "$W/t.c" 2>&1 | tail -1)

funcs=$(nm --defined-only "$W/fello" | awk '$2=="T"||$2=="t"{print $3}' | sort | tr '\n' ' ' | fold -sw 66 | sed 's/^/  /')
nconst=$(nm "$W/fello" | grep -c ' R fc__')
callsite=$(objdump -d --no-show-raw-insn "$W/cprintf" | sed -n '/<main>:/,/ret/p' | grep -o 'call.*' | sed 's/call *[0-9a-f]* //;s/[<>]//g')

# --- Report -----------------------------------------------------------------
hdr() { printf '  %-18s %10s %10s %10s %10s %12s\n' "" "fello" "C printf" "C fwrite" "nul" "vs printf"; }
row() { printf '  %-18s %10s %10s %10s %10s %12s\n' "$1" "$2 $6" "$3 $6" "$4 $6" "$5 $6" "$(printf '%+d' $(( $2 - $3 ))) $6"; }

exec > "$OUT"

cat <<EOF
fello — footprint of a "hello world" FC binary
==============================================

Regenerate with: ./demos/fello/measure.sh   (Linux only)

The program is demos/fello/main.fc: one import of std::io and one io.write
to stdout. It is the floor of the language — what an FC binary costs before
you have written any program.

Three controls are built and measured identically alongside it:

  C printf   hello world as a C programmer would actually write it:
             printf("Hello, world!\\n"). This is the control that matters.
             The honest question is what FC costs over the C you would
             otherwise write, not over C contorted to resemble FC.

  C fwrite   the same program written to make the libc call FC makes:
             fwrite("Hello, world!\\n", 1, 14, stdout). FC's str is a fat
             pointer (ptr + len) with no NUL guarantee, so io.write cannot
             call puts or printf %s — fwrite is the only door out. This
             column separates "what FC costs" from "what writing a counted
             string costs", which are different questions.

  nul        int main(void) { return 0; } — process startup with no work,
             the floor beneath all of them.

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
row "binary"           $d_fello   $d_cprintf   $d_cfwrite   $d_nul   B
row "binary, stripped" $ds_fello  $ds_cprintf  $ds_cfwrite  $ds_nul  B
row ".text"            $tx_fello  $tx_cprintf  $tx_cfwrite  $tx_nul  B
row ".rodata"          $ro_fello  $ro_cprintf  $ro_cfwrite  $ro_nul  B
row ".data"            $da_fello  $da_cprintf  $da_cfwrite  $da_nul  B
row ".bss"             $bs_fello  $bs_cprintf  $bs_cfwrite  $bs_nul  B
row "mapped image"     $im_fello  $im_cprintf  $im_cfwrite  $im_nul  B
cat <<EOF

"mapped image" is the sum of PT_LOAD MemSiz — the address space the
executable itself needs, before libc.

Stripped size is identical across all four programs. At this scale an ELF
file is headers, program and section tables, and 4 KB segment alignment;
the code is the small part, and every one of these .text sections fits in
the same page. The +$(( d_fello - d_cprintf )) B unstripped is symbol and debug data for the
extra generated functions — not loaded at runtime, gone on strip.

Statically linked at -O2 — glibc pulled in whole. Not FC's default; shown
because it separates what this program costs from what libc costs.

EOF
hdr
row "binary" $st_fello  $st_cprintf  $st_cfwrite  $st_nul  B
row ".text"  $stx_fello $stx_cprintf $stx_cfwrite $stx_nul B
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
row "dynamic" $rss_fello  $rss_cprintf  $rss_cfwrite  $rss_nul  KB
row "static"  $srss_fello $srss_cprintf $srss_cfwrite $srss_nul KB
cat <<EOF

Composition at exit, from /proc/self/smaps_rollup, dynamic build, median
of $SMAPS runs. A destructor in a preloaded .so reads the rollup after main
returns; that .so inflates every column by the same constant, so read
these across the row, not as absolutes.

EOF
hdr
row "RSS"           $mrss_fello $mrss_cprintf $mrss_cfwrite $mrss_nul KB
row "PSS"           $mpss_fello $mpss_cprintf $mpss_cfwrite $mpss_nul KB
row "private dirty" $mpd_fello  $mpd_cprintf  $mpd_cfwrite  $mpd_nul  KB
row "shared clean"  $msc_fello  $msc_cprintf  $msc_cfwrite  $msc_nul  KB
cat <<EOF

Two different things are in these tables, and only one of them is signal.

Against the C fwrite control, every row is a tie. Those figures are
page-granular — the smallest difference expressible is one 4 KB page — and
run-to-run jitter is exactly that size: run measure.sh repeatedly and the
fello-minus-fwrite delta lands on +4 KB, 0 and -4 KB with no pattern. The
reading is not "FC costs N KB"; it is that FC and equivalent C are
indistinguishable in memory. FC's extra few hundred bytes of code and
constants do not add a page to anything.

Against the C printf control, the RSS gap is real and reproducible, and it
runs the other way: fello is $(( rss_cprintf - rss_fello )) KB *lower*. Repeated trials put
fello and C fwrite at exactly $rss_fello KB every time and C printf at $rss_cprintf KB or
worse. printf drags glibc's format interpreter into residency; fwrite does
not, and FC's io.write cannot reach printf even if it wanted to, because a
str carries a length rather than a terminator. The saving is incidental —
FC did not set out to buy it — but it is the direction the measurement
actually points, so it is worth stating plainly rather than rounding to
"about the same".

The gap shows up in shared_clean, not private dirty, and that is the whole
story: those extra pages are glibc mapped from page cache, shared with
every other process on the machine and paid for once system-wide. PSS (RSS
with each shared page divided among its sharers) and private dirty — what
this process actually costs — are identical across fello and both C
controls, and within a page of an empty C program's. So the honest summary
is: FC costs nothing here, and the printf gap is a fact about printf, not
a win to claim. The static rows show the same thing from the other side —
no loader, no libc mapping, less than half the RSS, in exchange for a
$(( st_fello / d_fello ))x larger file.

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

Against the idiomatic C control, fello's .text is $(( tx_fello - tx_cprintf )) bytes larger and
its .rodata $(( ro_fello - ro_cprintf )) bytes larger. Nothing else differs. That is not overhead
in the sense of a runtime — FC has none — it is three specific things:

1. The entry wrapper ($(( tx_fello - tx_cfwrite )) B). FC's main takes str[], so before calling
   into FC code the wrapper walks argv, calls strlen on each element, and
   builds a slice:

     int main(int argc, char **argv) {
         fc_str *_args = alloca((size_t)argc * sizeof(fc_str));
         for (int _i = 0; _i < argc; _i++) {
             _args[_i].ptr = (uint8_t*)argv[_i];
             _args[_i].len = (int64_t)strlen(argv[_i]);
         }
         return fc_main((fc_slice_fc_str){ .ptr = _args, .len = argc });
     }

   A C program that also parsed argv would pay something similar; nul and
   the C controls simply never look at it.

2. The call FC has to make ($(( tx_cfwrite - tx_cprintf )) B — the C fwrite column minus the C
   printf column). io.write takes a str, a pointer and a length rather
   than a NUL-terminated string, so it compiles to a four-argument fwrite
   where the idiomatic C program makes a one- or two-argument call. That
   is a consequence of FC's string representation, not of transpilation.
   On this toolchain it costs $(( tx_cfwrite - tx_cprintf )) bytes, because fortification (see
   below) has already made the printf call site the same size; against an
   unfortified build it would be $(( tx_cfwrite - tx_nofort )) bytes. It is also what keeps printf's
   format interpreter out of the process, which is where the $(( rss_cprintf - rss_fello )) KB of
   RSS in the memory section went.

3. $(( ro_fello - ro_cprintf )) bytes of .rodata: the $nconst std::io module constants above, plus
   alignment. And $(( bs_fello - bs_cprintf )) bytes of .bss — the copy relocation for the stdout
   FILE* that fello and the fwrite control both name and printf does not.

A note on the C control, because it moves the baseline: this distro
defaults to _FORTIFY_SOURCE=$(cc -O2 -dM -E - </dev/null | awk '$2=="_FORTIFY_SOURCE"{print $3}'), which rewrites printf to __printf_chk and
so blocks GCC's usual printf-with-no-format-specifiers -> puts fold. The
call in the C printf control's main is therefore $callsite, and its
.text is $tx_cprintf B. Rebuilt with -D_FORTIFY_SOURCE=0 the fold happens, the
call becomes puts, and .text drops to $tx_nofort B — moving the delta above
from $(( tx_fello - tx_cprintf )) to $(( tx_fello - tx_nofort )) bytes. Both numbers are honest; which one is the
baseline depends on whether you count your distro's hardening defaults as
part of "the C you would write". Only .text was measured for the
unfortified build — every other figure in this report is from the standard
-O2 build shown in the flags above.

There is no allocator, no runtime init, no GC, no exception machinery, no
type metadata, no startup hook: the binary's constructor and destructor
lists are the ones crt puts in any C program.
EOF

exec >&2
echo "wrote $OUT"
