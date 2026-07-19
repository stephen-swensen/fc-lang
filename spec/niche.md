# FC's Niche: A Modern Language for 1992's Toolchains

*Strategy document — 2026-07-18. This is positioning analysis, not language spec. The
language rules live in `fc-spec.html`; nothing here constrains them.*

## The positioning question

Language adoption has almost nothing to do with design quality. Languages get adopted
through platform lock-in (Swift, JS), corporate backing (Go, Kotlin), or one killer
capability nobody else has (Rust's borrow checker, Zig's cross-compilation). The
"better C" space is the most crowded graveyard in language design — Zig, Odin, C3,
Hare, Nim all fight over it with years of head start and real communities. Choosing a
language means choosing its ecosystem, and a rational outsider comparing FC to Zig on
general-purpose merits picks Zig every time.

So FC does not compete on general-purpose merits. It needs a niche where its one
structurally unique property — **emitting warning-clean, UB-defined, int-width-agnostic
C11** — is load-bearing rather than decorative, and where the usual adoption blockers
(bus factor, ecosystem depth, hiring) don't apply.

## Candidate niches, ranked honestly

- **ML-academic crowd — skip.** Academics engage with languages that have something to
  *study*: novel type theory, effects, verification. FC's design ethos is
  "conservative, complete, no magic" — a virtue for users, a void for researchers. The
  adjacent teaching angle (readable C output shows students what their code becomes) is
  real but is a years-long institutional grind with no community payoff in between.
- **Professional embedded — not yet, maybe never directly.** Commercial shops cannot
  adopt a bus-factor-1 language regardless of merit (certification, MISRA, hiring). The
  embedded *discipline* — int-width-agnostic C, no runtime — is kept because it is free
  to maintain and compounds with the niche below.
- **General hobby — too diffuse.** Python owns casual hobby. The performance-aware
  recreational crowd (Handmade/low-level-jam orbit) is already claimed by Odin and Jai,
  which came from that community; FC would fight an incumbency battle with an offside
  rule that crowd will mostly bounce off.
- **Retro platforms — the niche.** See below.

## Why retro wins

Four structural reasons, each of which the other candidates fail:

1. **The technical wedge is load-bearing here.** Retro targets — DOS, Amiga, Genesis,
   GBA — are reached through C toolchains (djgpp, gcc-ia16, m68k-gcc, devkitARM, SDCC).
   Zig and Rust genuinely cannot follow: LLVM has no 65816 and no serious Z80 story and
   never will. "Emits clean C" stops being a mushy portability claim and becomes the
   entire reason FC can exist on the platform at all.
2. **This community demonstrably adopts bespoke languages.** NESFab and Millfork are
   single-author niche languages that attracted real users and shipped games. Retro
   homebrew people are exactly the enthusiasts who will try a weird new tool for fun.
   No other candidate niche has that precedent.
3. **Bus factor doesn't matter.** Nothing is commercial; nothing needs a five-year
   support promise. The objection that kills FC everywhere else is inert here.
4. **The proof artifact already exists.** wolf-fc proves FC is *usable*. The same
   project running on period hardware proves FC is *for this* — and "Wolfenstein
   reimplemented in a new ML-flavored language, running on a 486" is a story that
   carries itself to the retro forums and Hacker News without a marketing budget.

## Beyond restoration: retro as a living medium

The deeper thesis is that these platforms have **eternal life** — not merely as
subjects of emulation and restoration, but as targets for *new* game development,
indefinitely.

The generation that grew up on the NES, SNES, and early-90s PC loved those games,
then lived through a roughly twenty-year desert before Minecraft and Stardew Valley
revived the retro *craft* — but on modern hardware. That revival proved the appetite
is permanent. The next step of the argument: there is no reason the *actual platforms*
can't thrive as creative media indefinitely, the way people still compose for piano or
shoot on film. The hardware is fully documented, emulated everywhere, manufactured as
FPGA clones, and small enough for one person to hold entirely in their head — which is
precisely what modern platforms have lost.

New development for living retro platforms is a *larger* ambition than restoration,
and FC serves both with the same machinery: modern ergonomics — `match`, options,
result types, generics, string interpolation, bounds checks, defined overflow —
delivered through the C toolchain the platform already trusts. The pitch in one line:

> **FC — a modern language for 1992's toolchains.**

## Technical reach (honest scope)

FC emits C11: `<stdint.h>`, `_Static_assert`, anonymous unions. That draws a hard line
through the retro world:

**Reachable now (16/32-bit era):**
- DOS protected mode — djgpp (gcc)
- DOS real mode / 8086–286 — gcc-ia16
- Amiga, Atari ST, Genesis/Mega Drive — m68k-gcc
- GBA, DS — devkitARM (gcc)
- Z80/8080-family (Game Boy-adjacent, MSX, CP/M) — SDCC (mostly C11-capable; verify
  per-construct, especially anonymous unions)

**Out of reach without a C89/C90 emission mode:**
- NES, C64 (6502 — cc65 is C99-ish at best)
- SNES (65816 — no C11 compiler exists)

The NES and SNES are the emotional heart of the era, and they are exactly the targets
FC cannot reach today. Per the completeness-over-partiality principle: do **not** build
a speculative C89 mode. Prove the niche on the reachable platforms first; a C89
emission mode is the *earned* second act, undertaken only once real users on reachable
platforms exist to justify it. (It is also a well-bounded project: the C11 dependencies
are few and enumerable — typedefs for `stdint`, struct-wrapped unions or mangled
members for anonymous unions, runtime or build-time asserts for `_Static_assert`.)

**Expected first-port fractures:** anything POSIX-flavored in `std::io`, `alloca`,
the `execinfo` backtrace machinery (already stubbed on Windows — the same gating
generalizes), heap assumptions in `alloc`, and `abort()`-based checks on targets
without a meaningful `abort`. These are stdlib-layering problems, not language
problems, and surfacing them is a goal of the experiment, not a blocker to it.

## The falsifiable experiment

Sequenced so each step is cheap and informative:

1. **Build wolf-fc's emitted C with djgpp (and/or gcc-ia16).** One afternoon; measures
   the true distance between "emits C11" and "runs on a 1992 toolchain."
2. **Fix what breaks until wolf-fc runs in DOSBox**, then on real hardware. The fixes
   (stdlib layering, target profiles) are the actual product work.
3. **Write the positioning piece** — "a modern language for 1992's toolchains" — with
   the running demo as the headline, aimed where this crowd lives: r/retrogamedev, the
   gbadev Discord, nesdev-adjacent forums.
4. **Signal: ten interested strangers.** If the post can't attract ten people who want
   to try it, the niche hypothesis is falsified cheaply and FC continues as a personal
   instrument with nothing lost.

## Compounding

Everything the DOS port forces — int-width agnosticism, runtime-free codegen, stdlib
layering, target profiles — is exactly the discipline the (deferred) embedded wedge
needs later. Investing in retro doesn't foreclose embedded; it quietly builds it.

---

## Addendum: what the emitted C actually requires (audit, 2026-07-18)

An audit of `src/codegen.c` against the framing above. It refines the "Technical
reach" section in two ways: the emitted dialect is stricter than "C11", and several
blockers are structural (independent of dialect). The audit is what makes a
*non-compromising retro subset* definable at all: you can only draw the line once you
know exactly what sits on each side of it.

### Correction: the emitted dialect is "GCC's C11", not ISO C11

The generated C compiles with `gcc`/`clang -std=c11 -Wall -Werror`, but it leans on
GNU extensions that ISO C11 does not have. Enumerated by tier:

**C99 baseline (relative to C89):**
- `<stdint.h>` / `<stdbool.h>` / `<stddef.h>` fixed-width types and `bool`
- `long long` — required for `int64_t`/`uint64_t` (see the structural section: this
  is not an edge case; it is load-bearing in every slice)
- `static inline` helper functions (`fc_to_size`, `fc_to_int`, `fc_alloc_n`, …)
- Compound literals and designated initializers (`(fc_str){ .ptr = …, .len = … }`)
- Declarations interleaved with statements

**C11 proper:**
- Anonymous union member in the tagged-union representation:
  `struct shape { shape_tag tag; union { … }; }`
- The defined type-pun for bitcasts: a union *compound literal* read through the
  other member (`(((union { T from; U to; }){ .from = x }).to)`)
- `_Static_assert` — emitted only inside the atomics lock-free guard (below), so it
  falls away with atomics

**GNU extensions (the real dialect ceiling):**
- **Statement expressions `({ … })` — ~49 emission sites**, and they are not
  peripheral: option unwrap, bounds-checked indexing, result propagation, string
  interpolation, and cstr conversion all lower through them. This is the single
  hardest dialect dependency to remove.
- `__builtin_alloca` (the `alloca` operator, interpolation buffers, cstr casts)
- `__builtin_expect` on every guard branch (bounds, null-some, zero-error)
- `__builtin_add/sub/mul_overflow` — the entire `checked` arithmetic feature
- `__atomic_load_n` / `__atomic_store_n` / `__atomic_always_lock_free` — the
  `atomic_*` intrinsics (GNU builtins, not even C11 `<stdatomic.h>`)
- `__attribute__((unused))` on every emitted helper and function (`g_fn_attr`);
  `__attribute__((constructor))` on the Windows-only abort-dialog suppressor

**Consequence.** Any GCC-based cross toolchain — djgpp, gcc-ia16, m68k-gcc,
devkitARM — accepts all of this (these extensions are ancient; most predate C99).
Non-GNU compilers do not: cc65 rejects nearly every line, Watcom/Borland-era
compilers reject the C99 tier, and **SDCC's GNU-extension coverage is partial —
statement-expression support in particular must be verified before SDCC is claimed
as reachable** (this downgrades the Z80 line in "Technical reach" from *reachable*
to *verify first*). A "true retro" emission mode is therefore not just "C89" — it is
**de-GNU + C89**: eliminate statement expressions (temporaries + real statements),
replace builtins (plain branches for `expect`, manual overflow checks, a
platform `alloca` or banning it), drop attributes, and shim `<stdint.h>`.

### Structural blockers (dialect-independent)

These bite even on a GCC toolchain, because they are about the target's runtime and
word size, not the compiler's parser:

1. **64-bit integers are load-bearing everywhere.** `fc_str` is
   `{ uint8_t *ptr; int64_t len; }`; every slice length, bounds comparison
   (`(uint64_t)i >= (uint64_t)s.len`), string-interpolation length computation, and
   `INT64_C` literal rides 64-bit math. On a 16-bit CPU every bounds check becomes a
   multi-word software comparison and every slice fattens by 6 bytes. GCC targets
   *compile* it (soft 64-bit via libgcc), but it taxes exactly the machines the
   niche is about. This is the deepest single item: a retro profile wants the
   slice-length type **parameterized** (e.g. `isize`-based lengths — `int32_t` or
   even `int16_t` per target) with identical checked semantics.
2. **Hosted-libc assumptions.** Always: `malloc`/`free` (alloc), `memcpy`/`strlen`,
   `abort` + `assert` (every safety guard). Feature-gated but common: `snprintf`
   (string interpolation is a hard stdio dependency — `EXPR_INTERP_STRING` sets
   `g_needs_stdio` unconditionally), `<math.h>`/`<float.h>` (float properties),
   `<errno.h>`. A freestanding retro target has none of these by default; each needs
   a platform mapping (guards → a `fc_trap` hook, alloc → a platform allocator) or a
   profile-level compile error.
3. **Floating point.** `f32`/`f64` soft-float works on GCC targets but is enormous
   on 8/16-bit machines; period platforms mostly had none. Per-target opt-out.
4. **Atomics.** GNU `__atomic` builtins plus a lock-free static assert — meaningless
   on single-core retro hardware and unavailable in period toolchains.
5. **Backtraces.** `<execinfo.h>`, glibc/macOS-only — already stubbed on Windows,
   so the gating precedent exists.

### Defining the non-compromising retro subset ("FC/retro profile")

The governing rule is the project's completeness-over-partiality principle applied to
targets: **every construct in the profile keeps its full FC semantics, and every
construct outside it is a compile error on that target.** No silent degradation, no
"works but means less here." (The one sanctioned exception is the existing Windows
backtraces precedent: a *diagnostic-only* feature may degrade to a no-op stub,
because it never changes program meaning. Semantics-bearing features never get that
option.)

**Untouchable — the identity travels intact.** Bounds checks, option-unwrap checks,
defined signed overflow, masked shifts, exhaustive match, defer, escape analysis.
These are what make FC worth carrying to a 386; they cost a compare-and-branch, which
even 6502-class machines afford. A retro profile that relaxed them would be a
different language wearing FC's syntax.

**In the subset unconditionally** (already codegen-clean given a GCC toolchain):
all control flow, structs/unions/enums/options/results, pattern matching, generics
and const generics, closures, slices, pointers, `checked`/`unchecked`, modules.

**Profile-gated — compile error where the target can't honor full semantics:**
- `atomic_*` (single-core targets)
- `f32`/`f64` (float-less targets)
- `i64`/`u64` as user types (targets where the profile deems soft-64 unacceptable)
- String interpolation, until a freestanding formatter replaces `snprintf`
- `alloc`/`free`, unless the target profile supplies an allocator
- `--backtraces` (no-op stub per the diagnostic-only exception, or error)

**Profile-parameterized — same semantics, target-sized machinery:**
- Slice/string length type (`int64_t` today → `int32_t`/`int16_t` per profile), with
  every guard emitted against the profile's width
- Guard failure: `abort()` → a per-target `fc_trap` (freestanding targets)
- Stdlib layering: a core layer with zero libc dependence, `std::io` et al. becoming
  per-platform modules

**Two lanes, restated precisely after the audit:**
- **Lane 1 — GCC retro (DOS, Amiga, Genesis, GBA):** no dialect work at all; the
  cost is the structural list — stdlib layering, `fc_trap`, the length-type
  parameter, float/atomic gates. This is tractable incrementally and is where the
  falsifiable experiment already points.
- **Lane 2 — true 8/16-bit (NES, C64, SNES, period PC compilers):** everything in
  Lane 1 *plus* the de-GNU + C89 emission mode. The addendum's enumeration is the
  scope of that project — finite, but dominated by one item: retiring ~49 statement-
  expression forms into statement-level lowering. Still the earned second act.
