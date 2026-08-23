# The freestanding profile — Lane 1 gates

*Audited and written 2026-08-22, immediately after `--len-repr` shipped. This is the
planning document for the profile-level work that carries FC to freestanding and
partially-hosted targets (Genesis, GBA, bare-metal MCUs, and the thinner DOS libcs).
It details the gates sketched in `niche.md` → "Defining the non-compromising retro
subset". The governing rule is inherited from there: **every construct in a profile
keeps its full FC semantics, and every construct outside it is a compile error on that
target** — no silent degradation. The one sanctioned exception is diagnostic-only
degradation (the Windows backtraces precedent).*

Scope note: this document is **Lane 1 only** (GCC-toolchain targets). Lane 2 — the
de-GNU + C89 emission mode for cc65/SDCC/period compilers — is a separate, larger
project (dominated by retiring ~49 statement-expression emission forms) and is out of
scope here. Nothing below depends on it or forecloses it.

Relationship to `--len-repr` (shipped 2026-08-22, spec §Length representation): that
flag solved the *cost* half of small-target support — representation-parameterized
lengths with identical semantics. This document is the *dependency* half: what the
emitted C assumes about its runtime, and how each assumption becomes a hook or a gate.
The precedent it set carries over: each knob must keep the whole test suite runnable
on the development host (`make test-gcc-len16` style), because semantics never vary
per profile.

---

## The dependency inventory (audited against codegen.c, 2026-08-22)

**Headers the emitted C includes unconditionally:** `stdint.h`, `stddef.h`,
`stdbool.h`, `limits.h` — all *freestanding* headers per the C standard, fine
everywhere — plus three *hosted* ones: `stdlib.h`, `string.h`, `assert.h`.
Conditionally: `stdio.h` (any runtime guard, string interpolation, integer div/mod,
option unwrap, `--backtraces`, and any narrow `--len-repr`), `math.h` / `float.h`
(float type properties: `f64.nan`, `epsilon`, …), `errno.h` (errno-protocol externs).

**Hosted symbols actually called, by feature:**

| Feature | Hosted symbols |
|---|---|
| Every program (argv → `str[]` setup; `main` must take `args: str[]`) | `strlen`, `__builtin_alloca` |
| Narrowing helpers `fc_to_size` / `fc_to_int` / `fc_alloc_n` | C `assert` — the only real `assert.h` use |
| All runtime guards (bounds, unwrap, div-zero, len caps, null-some, zero-err, trunc) **and FC `assert`** | `fprintf(stderr)` + `abort()` via `FC_ABORT` |
| `alloc` / `free` / `alloc(s)!` | `calloc` / `malloc` / `free` |
| Slice copies, aggregate equality, cstr casts | `memcpy` / `memset` / `memcmp` / `strlen` |
| String interpolation | `snprintf` — the one hard stdio dependency |
| Atomics | `__atomic_*` builtins + lock-free `_Static_assert` (no libc) |
| `checked` arithmetic | `__builtin_*_overflow` (no libc) |
| `--backtraces` | `execinfo.h` (glibc/macOS; already a no-op stub on Windows) |

Two findings that shrink the problem:

- **The `string.h` tier is nearly free.** GCC's freestanding contract already requires
  the platform to provide `memcpy`/`memset`/`memcmp`/`memmove` (the compiler
  synthesizes calls to them regardless of what the source writes), so this dependency
  needs no FC-side work beyond, at most, emitting prototypes instead of the include.
- **FC `assert` is not C `assert`.** It emits the same `fprintf` + `FC_ABORT` shape as
  the guards, so it rides the trap hook (item 1) for free. The only C-`assert` uses
  are the three narrowing helpers, which fold into the same hook.

**Stdlib tiering as it stands:** `data.fc`, `random.fc`, `wideint.fc` are already pure
FC — zero externs. `math.fc` is a `math.h` wrapper (float-gated by nature). `text.fc`
is libc-free *except* `strtoll`/`strtod` + errno in its parse functions. `io.fc`,
`sys.fc`, `net.fc` are fully hosted by design. The "core layer" mostly already exists;
nobody has drawn the line yet (item 7).

---

## Work items

Numbered for reference; dependency order is **1 → 2 → (4, 5, 6 in any order) → 3 →
7 → 8**.

### 1. `fc_trap` — the guard-failure hook (keystone)

All abort helpers (`fc_oob`, `fc_oob_u`, `fc_oob_sub`, `fc_neg_len`, `fc_len_cap`,
`fc_null_some`, `fc_zero_err`, `fc_overflow`, `fc_trunc`), the inline div-zero and
unwrap paths, and FC `assert` share one shape: print `file:line: kind`, then
`FC_ABORT()`. Under the freestanding profile this becomes a call to an `fc_trap` the
platform supplies (an extern, with a `__builtin_trap()` loop as the emitted default),
and the `fc_to_*` C-asserts fold into the same channel — retiring `assert.h`, and,
absent interpolation, `stdio.h` entirely.

The one real design decision inside this item: **diagnostic strings cost ROM.** Every
guard site embeds its filename and expression text; a 32 KB cart cares. So the hook
wants two fidelity levels — `fc_trap(file, line, kind, detail)` (current fidelity)
and a quiet variant where sites pass only a kind enum and the string tables are not
emitted. Same axis as `--backtraces`' stated "pay static data for readable failures"
trade; one knob, per profile.

Scope: one emitter sweep over a closed helper list plus a detection change; a day or
two with tests. Everything after this is downhill.

### 2. Allocator hook

`alloc`/`free` lower to `fc_alloc`/`fc_free` externs under the profile (platform
supplies them; a bump arena is a fine first implementation on most retro targets),
preserving the zero-size-request semantics `fc_alloc_n` defines today. If the profile
declares no allocator, `alloc` is a compile error — completeness over partiality, not
silent degradation. `alloca` is a builtin and unaffected. Small: the emission sites
are exactly the `calloc`/`malloc`/`free` calls in the inventory. Half a day.

### 3. Freestanding interpolation formatter

`snprintf` is the deepest hosted dependency because `EXPR_INTERP_STRING` is common in
real code. The work splits cleanly:

- Integer / hex / char / str / `%T` segments need only an emitted `fc_fmt_*` family —
  bounded, no libc, a few hundred lines of emitted helpers. The buffer-sizing logic
  already exists and is snprintf-independent.
- **Float formatting is the hard 20%** (`%f`/`%g` done honestly is Ryū/Grisu
  territory) and is simply excluded where item 4 excludes floats — which is every
  target this profile serves anyway.

Medium: the largest single item, 2–3 days.

### 4. Float gate

A profile under which any `f32`/`f64` use is a pass2 compile error ("this profile has
no floating point"), excluding `std::math` with it. Floats are already pay-per-use in
codegen, so this is purely a diagnostic gate plus tests. Half a day.

### 5. Atomics gate

Same shape: `atomic_*` under a single-core profile is a compile error. The lock-free
`_Static_assert` disappears with it — the last C11-proper dependency in the emission
besides anonymous unions. Half a day.

### 6. Backtraces gate

Precedent already exists (Windows no-op stub, under the diagnostic-only exception).
Freestanding profile: compile error, or the same stub. Hours.

### 7. Stdlib layering

Mostly *declaring* the line that already exists: **core** = `data`, `random`,
`wideint`, `text` minus its parse tail; **hosted** = `io`, `sys`, `net`, `math`. Two
real tasks: reimplement `text`'s integer parse in pure FC (`strtoll` is used mainly
for its ERANGE reporting, which the whole-string-strict wrapper largely re-derives),
and defer float parse alongside item 4. The *mechanism* needs no work: response files
already pin a project's stdlib subset, so a freestanding project simply lists core
files. A day, mostly `text.fc` surgery and docs.

### 8. `--profile` bundles

Once items 1–6 exist as individual knobs, named profiles bundle them the way `gcc -O2`
bundles flags — never a parallel mechanism, and `--len-repr` stays the primitive:

- `--profile dos16` → `--len-repr 32` (32, not 16: a signed-16 len caps slices at
  32,767 elements, below a full 64 KB real-mode segment of bytes) + trap/allocator
  defaults appropriate to djgpp/gcc-ia16.
- `--profile gba` (illustrative) → `--len-repr 32` + no-float + no-atomics +
  `fc_trap` + allocator hook.

Plus a DOSBox smoke script once wolf-fc compiles under djgpp. Deliberately last.

---

## What each target actually needs

- **djgpp DOS (protected mode):** none of this — hosted libc, GCC, 32-bit. The
  falsifiable experiment (`niche.md`: wolf-fc in DOSBox) is runnable **today** and
  should stay first; its observed pain should re-rank this list.
- **gcc-ia16 real mode:** at most item 1 (its libc is partial) plus the shipped
  `--len-repr`.
- **Genesis / GBA / bare-metal MCUs:** items 1+2 to link at all; want 4/5; feel 3 the
  moment they print anything; 7 to have a stdlib story.
- **Lane 2 (cc65, SDCC, period compilers):** everything above *plus* the de-GNU + C89
  emission project — out of scope here.

## The headline

The surface is smaller than it looks. The guard family is one closed list behind one
macro; the allocator is two call-shapes; the gates are pass2 one-liners; half the
stdlib is already freestanding. The only genuinely meaty item is the interpolation
formatter, and its hard half is excludable. Total Lane 1 estimate: on the order of a
week of focused work, most of it testable on the host because — as with `--len-repr` —
none of it is allowed to change what a program means.
