# fello

The smallest complete FC program — one import, one write, exit 0:

```fc
import io from std::

let main = (args: str[]) ->
    io.write("Hello, world!\n", stdout)
    0
```

Unlike the other demos, fello isn't here to show off a feature. It's a
**measuring stick**: what does an FC binary cost before you have written any
program? [`stats.txt`](stats.txt) is the answer, and
[`measure.sh`](measure.sh) is how it was produced.

## Running

```sh
./demos/fello/run.sh
```

## Measuring

```sh
./demos/fello/measure.sh    # Linux only; rewrites stats.txt
```

It builds fello at `-O2` alongside three controls built and measured
identically, then reports on-disk size (binary, sections, static and dynamic),
in-memory footprint (peak RSS from the kernel's `rusage`, plus an
RSS/PSS/private/shared breakdown from `/proc/self/smaps_rollup`), build and
run time, and where FC's difference from C actually lives.

The controls are deliberately two-sided:

- **`printf("Hello, world!\n")`** — hello world as a C programmer would
  actually write it. This is the one that matters: the honest question is what
  FC costs over the C you'd otherwise write, not over C contorted to resemble
  FC.
- **`fwrite("Hello, world!\n", 1, 14, stdout)`** — the same libc call FC makes.
  FC's `str` is a fat pointer (ptr + len) with no NUL guarantee, so `io.write`
  can't reach `puts` or `printf %s`. This column separates *what FC costs* from
  *what writing a counted string costs*.
- **`int main(void) { return 0; }`** — the floor beneath all of them.

Everything is built in a scratch directory; the only file it touches in the
repo is `stats.txt`.

## The short version

At the time of writing, on x86-64 Linux with glibc 2.39 and GCC 13.3:

| | fello | C `printf` | C `fwrite` |
|---|---|---|---|
| binary, `-O2` dynamic | 16,440 B | 15,968 B | 16,008 B |
| binary, stripped | 14,472 B | 14,472 B | 14,472 B |
| `.text` | 457 B | 281 B | 281 B |
| peak RSS | 1,304 KB | 1,368 KB | 1,304 KB |
| private dirty at exit | 144 KB | 144 KB | 144 KB |

All the stripped binaries are the same size — at this scale an ELF file is
headers, tables and 4 KB alignment, and every one of these `.text` sections
fits in a page.

FC's whole measurable cost is **176 bytes of `.text`** — the entry wrapper that
turns `argv` into the `str[]` that `main` takes — plus 28 bytes of `.rodata`
for `std::io`'s module constants, which have external linkage and so survive
dead code elimination. Everything that actually costs the machine memory
(PSS, private dirty) is identical across all three.

Two findings that only showed up once the idiomatic control was measured:

- Peak RSS runs 64 KB *lower* than the `printf` version, reproducibly.
  `printf` pulls glibc's format interpreter into residency and `fwrite`
  doesn't, and FC can't reach `printf` even in principle. The pages are
  `shared_clean` libc, so it costs the machine nothing either way — but the
  measurement points that direction and the report says so.
- This distro defaults to `_FORTIFY_SOURCE=3`, which rewrites `printf` to
  `__printf_chk` and blocks GCC's usual `printf` → `puts` fold. Build the
  control with `-D_FORTIFY_SOURCE=0` and C's floor drops 16 bytes, moving FC's
  delta from 176 to 192 bytes. Both are honest; which is the baseline depends
  on whether you count your distro's hardening defaults as "the C you'd write".

There is no FC runtime to link against: `ldd` shows libc and the loader, the
same as the C controls. The program never touches the heap.
