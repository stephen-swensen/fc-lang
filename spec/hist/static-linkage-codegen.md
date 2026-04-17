# Static Linkage for Emitted Functions

A codegen proposal: mark every FC-emitted function as `static` in the generated C, with `main` as the sole exception. Motivated by the same wolf-fc audit as `bounds-check-codegen.md`, this one targeting inlining quality across the single translation unit FC produces.

This is a one-line codegen change with a small but real win in generated-binary quality. No language-surface change, no semantic change, no test impact.

---

## Current state

Every function definition FC emits uses external linkage. From a typical wolf-fc build:

```c
double std__math__pow(double x, double y, void* _ctx) { ... }
int16_t opl2__sample(opl2__chip* c, void* _ctx) { ... }
void fc__update_enemy(level* lv, game* g, audio_ctx* ac, ...) { ... }
```

Not `static`. Forward declarations earlier in the file match — also external linkage.

Since FC always emits a **single translation unit per program** (the compiler takes all input `.fc` files, including imported stdlib, and produces one `.c` file), external linkage is doing no work here. Nothing outside this TU can see or call these symbols — the final binary is linked against libc, libm, and SDL2, and none of those call back into FC code.

The only function that genuinely needs external linkage is `main`, because libc's startup code calls it.

---

## Why this matters

Three effects, in decreasing order of impact:

### 1. Inlining threshold

Within a single TU, GCC at `-O2` will inline a `static` function more aggressively than an external-linkage one. The reasoning in the optimizer:

- **Static**: the compiler can prove no other caller exists. If it inlines everywhere, the standalone definition can be dropped entirely — inlining is a pure size-and-speed win.
- **External linkage**: the standalone definition must be emitted regardless (something outside the TU might call it). Inlining is extra *on top of* the standalone body. The heuristic threshold for "is this worth it" is correspondingly higher.

In practice at `-O2` GCC still inlines most small external-linkage functions, but the tail that *doesn't* get inlined is real. In wolf-fc's audio inner loop:

```
opl2__sample
  → std__math__pow(2.0, block - 1)      // per channel, per sample, 44100 Hz
  → std__math__sin(phase * 2π)          // twice per channel
  → std__math__abs(...)                 // called through wrapper
```

These are trivial forwarders (`return pow(x, y);` with a `void* _ctx` parameter to discard). If they don't inline, each call is an actual `call`/`ret` with the full ABI dance — 2-argument setup, stack alignment, possible spills. For a helper whose body is `return fabs(x);`, the call overhead dominates the work by 5–10×.

With `static`, GCC almost certainly inlines all of these. The trig/pow calls to libm survive (the libm functions themselves are external), but the FC-emitted wrappers around them disappear entirely.

### 2. Dead code elimination

If the FC compiler emits a helper that ends up having no callers in the final program — say, an unused function from an imported stdlib module — GCC must still emit the body, because the function's external linkage means some hypothetical future TU could call it. Mark it `static`, and the standalone-unused check becomes trivial: no callers, drop it.

For a typical wolf-fc build, the compiler imports `std::io`, `std::text`, `std::sys`, `std::math`, `std::random` — not every function in each is actually referenced from the game. The leaf dead functions are small but there are many of them. Expect a measurable but modest binary-size drop.

### 3. Symbol table hygiene

External-linkage function names go into the final binary's dynamic/static symbol table and show up in `nm` output, `objdump -t`, stack traces, etc. Static functions still appear with debug info (which FC doesn't emit today, but someday), but are absent from the plain symbol table.

Minor quality-of-life win for anyone reading a crash dump or running `nm`.

---

## Proposal

In codegen, when emitting a function definition or forward declaration, prefix `static ` unless the function is the program entry point (`main`).

Before:

```c
double std__math__pow(double x, double y, void* _ctx);
...
double std__math__pow(double x, double y, void* _ctx) {
    (void)_ctx;
    return pow(x, y);
}
```

After:

```c
static double std__math__pow(double x, double y, void* _ctx);
...
static double std__math__pow(double x, double y, void* _ctx) {
    (void)_ctx;
    return pow(x, y);
}
```

That's the entire change.

---

## Why this is safe

### Function pointers / closures still work

FC's closure ABI uses `{ fn_ptr, ctx }` pairs (the `fc_fn_*` struct types at the top of generated C). Taking the address of a static function is perfectly valid C — `static` controls linkage, not addressability. Function pointers into static functions, called through the closure wrapper, work identically to today.

If a function has its address taken (via closure construction, callback to an SDL API, etc.), the compiler retains its definition even if all direct calls are inlined. Standard behavior. No code change needed; this "just works."

### C calling into FC

The only C-to-FC call in the current model is libc calling `main` at startup. That's why `main` is the exception.

Are there other cases? Reviewing the patterns:

- **SDL callbacks**: SDL takes function pointers (e.g., audio device callback in `SDL_OpenAudioDevice`). These are taken via `&some_fn` in FC code, so the address is taken inside FC — `static` is fine.
- **pthread / signal handlers**: same — address is taken in FC, linkage-class irrelevant.
- **qsort-style comparators**: same.

There is no path today by which a hand-written C file outside of FC-emitted output could call into FC code, because FC controls the entire compilation and emits everything into one `.c`. If that model ever changes (e.g., FC starts supporting a library output mode that produces a `.a` or `.so` for consumption by non-FC C code), the linkage rule would need to revisit — but that's a separate feature that doesn't exist today.

### Interaction with the cold helper from `bounds-check-codegen.md`

Proposal 1 in `bounds-check-codegen.md` already proposes a `static void fc_oob(...)` helper. This doc subsumes that as a special case: it's not a new rule, it's the same rule applied uniformly.

### Debug builds

`static` has no effect on debuggability when debug info is emitted. `gdb` can set breakpoints on static functions by name, step into them, and resolve addresses to function names in backtraces. Inlined functions (static or not) fold into their caller in backtraces under `-O2`, but that's an optimization-level concern, not a linkage concern, and it's already the case today for any function that gets inlined.

---

## Implementation

In `codegen.c`, find the two emission sites for function signatures:

- Forward declaration emission (the block that runs before any function body, emitting `<return_type> <name>(<params>);`).
- Definition emission (the block that emits `<return_type> <name>(<params>) { <body> }`).

At each site, prepend `static ` to the output unless the function being emitted is `main`. The `main`-check is a single string compare against the function's name (or a flag on the function AST node set during parsing).

No changes to the rest of codegen. No changes to any other pass. No changes to the header.

The diff should be under 20 lines.

---

## Measurement

On wolf-fc and the FC test suite:

- **Binary size**: expect a small drop (a few KB — unused stdlib helpers get DCE'd). Measure with `size /tmp/wolf-fc-bin`.
- **Hot-path perf**: run a profile-ish benchmark of the game under `perf stat` — expect a modest improvement in cycles/instruction-count driven by inlining of `std__math__*` wrappers in the audio path. `opl2__sample` is the most exposed function.
- **Compile time**: negligible change. GCC's work goes up slightly (more inlining considered) and down slightly (less code to emit). Likely a wash.
- **Test suite**: no behavioral change expected. Every test should pass unchanged.

---

## Rollout

Single-commit change. No flag, no phased rollout. Either the build works and tests pass (in which case ship it), or something about the FC model actually does need external linkage and the test suite catches it.

The only plausible failure mode is a hand-written C file somewhere in the FC test suite that references an FC-emitted symbol by name (e.g., testing the C output mechanically). If any such test exists, it would need either adjustment or a narrow `extern` exception — but I don't think any such test exists today; the standard pattern is to run the generated binary and assert on its behavior, not its symbol table.
