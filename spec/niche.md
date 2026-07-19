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
