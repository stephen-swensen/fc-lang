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

It builds fello at `-O2` alongside two controls built and measured
identically — the same program hand-written in C, and `int main(void) { return
0; }` — then reports on-disk size (binary, sections, static and dynamic),
in-memory footprint (peak RSS from the kernel's `rusage`, plus an
RSS/PSS/private/shared breakdown from `/proc/self/smaps_rollup`), build and
run time, and where FC's difference from the C control actually lives.

Everything is built in a scratch directory; the only file it touches in the
repo is `stats.txt`.

## The short version

At the time of writing, on x86-64 Linux with glibc 2.39 and GCC 13.3:

| | fello | hello (C) |
|---|---|---|
| binary, `-O2` dynamic | 16,440 B | 16,008 B |
| binary, stripped | 14,472 B | 14,472 B |
| `.text` | 457 B | 281 B |
| peak RSS | 1,304 KB | 1,304 KB |
| private dirty at exit | 144 KB | 144 KB |

The stripped binaries are byte-for-byte the same size, and the memory figures
are identical at page granularity. FC's entire measurable difference from
hand-written C is 176 bytes of `.text` — the entry wrapper that turns `argv`
into the `str[]` that FC's `main` takes — and 28 bytes of `.rodata` for
`std::io`'s module constants, which have external linkage and so survive dead
code elimination.

There is no FC runtime to link against: `ldd` shows libc and the loader, the
same as the C control. The program never touches the heap.
