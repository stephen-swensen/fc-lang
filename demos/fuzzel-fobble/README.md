# fuzzel-fobble

A Puzzle Bobble / Bust-a-Move clone written in FC using SDL2 — a hex-packed
bubble raft, a rotating launcher with a bounce-accurate aim guide, flood-fill
match detection, gravity for everything you cut loose, and a ceiling that
grinds down on you.

## Usage

```
./demos/fuzzel-fobble/run.sh
```

The script auto-detects the host OS (Linux or MSYS2/MinGW Windows) and
picks the right SDL2 link line.

Requires SDL2 installed for your environment:

- Linux: `libsdl2-dev` (or your distro's equivalent)
- MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-SDL2`
- MSYS2 MINGW64: `pacman -S mingw-w64-x86_64-SDL2`

## Controls

| Key | Action |
|-----|--------|
| Left / Right (or A, D) | Aim |
| SPACE | Fire (also starts / restarts) |
| ENTER | Start / restart |
| TAB | Swap the loaded bubble with the next one |
| P | Pause |
| S | Toggle the colour marks (off by default) |
| F11 | Toggle fullscreen |
| ESC | Quit |
| C (splash only) | Clear best score |

## Rules

- Fire a bubble into the raft. Land it against **three or more** of its own
  colour and the whole group pops.
- Anything left hanging with no path back to the ceiling **falls** — that is
  where the points are. Each bubble that falls **doubles** the payout: one is
  20, two is 40, three is 80, and a dozen is over forty thousand. Popped
  bubbles themselves are only 10 each. Cutting the raft off at the neck is
  not a tidier way to play, it is the whole game.
- The ceiling grinds down one row on a fixed count of **shots landed**.
  Popping pays in points, not in time — the ceiling is a metronome, not a
  penalty for missing. The `DROP IN` pips on the right show what is left.
- That count comes from **how many colours are still on the board**, not from
  the level number: a board down to two colours is an easy board, so it earns
  a faster ceiling. Clearing a colour off tightens the screw immediately.
- **You are on a turn clock.** Deliberate too long and the launcher starts
  flashing `HURRY!`; keep deliberating and it fires wherever you left it
  aimed. Roughly five seconds to the warning, ten to the shot.
- Push the raft past the red line and the run is over.
- Clear the board and the next level starts: more rows and more colours.
  Clearing pays `500 * level` plus a **speed bonus** that starts at 50,000,
  decays every few seconds, and pays nothing at all past 65. The `BONUS`
  readout in the left panel is that number, falling while you watch.
- The launcher is only ever loaded with a colour still on the board, so you
  can never be handed a dead shot.
- **The aim guide only runs on level 1.** It draws the bounce for you while
  the geometry is new; after that, reading the angle is the skill. The
  `CLEAR.` screen tells you it is going.
- Best score persists to `~/.fuzzel-fobble/highscore.txt`.

## Special bubbles

Three of them, arriving as you go:

| | What it is | What it does |
|---|---|---|
| **Star** (gold star on white) | on the board, from level 2 | Land any colour against it and **every bubble of that colour** goes, wherever it is on the board — adjacency doesn't matter. The star goes too. |
| **Stone** (grey faceted hexagon) | on the board, from level 3 | Indestructible. It never pops, not to a match and not to a metal shot. The only way to be rid of it is to **cut it loose** and let it fall. |
| **Metal** (chrome) | loaded into the launcher, from level 2 | Doesn't stick to anything. It **ploughs straight up through the raft**, popping everything it touches, and leaves at the ceiling. Stones shrug it off. |

A star is worth aiming *at* rather than around — it is the one shot that
ignores the three-of-a-colour rule entirely. A stone is the opposite: it
can't be removed directly, so it turns into an anchor you have to plan
around, and cutting a stone loose is the only way it ever leaves.

Stones never appear in the ceiling row. One there could never be cut loose,
and a board you can't finish isn't a hard board, it's a broken one — so the
generator won't place one, and since the raft only ever moves *down*, none
can arrive there later.

The bubbles can also carry a small mark (diamond, ring, cross, star, square,
tree) so the six colours stay distinguishable without relying on hue. That is
**off** by default; **S** turns it on. The specials are not part of that
setting: a star is a gold star inscribed across its whole face and a stone is
a faceted hexagon rather than a sphere, so both are told apart by shape at
any distance, and neither is the player's to switch off.

## Display

The game opens a resizable, high-DPI aware window that is maximized
immediately at startup (`SDL_MaximizeWindow`), then uses
`SDL_RenderSetLogicalSize(1280, 720)` so all drawing happens in a fixed
logical coordinate space. SDL scales that logical resolution to the physical
backing pixels at blit time. **F11** toggles a borderless window sized to the
display bounds — the same "fake fullscreen" approach `face-invaders` uses,
which sidesteps a class of DPI/drawable-size quirks on some window managers.

## Notes on the implementation

**Hex packing without a hex library.** Rows alternate wide (12 bubbles, flush
left) and narrow (11, shifted right by a radius), stored in one flat
`i32[168]` where `-1` means empty. A single `row_parity` field says which kind
row 0 currently is, and `row_is_narrow` derives the rest. The six neighbours
of a cell fall out of that one bit: a narrow row sits half a bubble right of
the wide row above it, so its upward neighbours are `(col, col + 1)` where a
wide row's are `(col - 1, col)`.

**The ceiling descent is a parity flip.** Sliding the raft down a row and
flipping `row_parity` is exactly what preserves every existing row's stagger
— a row moving from index *r* to *r + 1* keeps its own width and offset for
free, so no bubble has to be re-fitted to a new lattice.

**Two flood fills over one stack.** `flood_same_color` gathers the cluster
around the bubble that just landed; `mark_anchored` marks everything still
reachable from row 0. What the second one *doesn't* reach is what falls. Both
run over the same explicit `i32[]` work list — no recursion, no allocation
per shot.

**Nearest-attachable settling.** A landing bubble doesn't snap to the cell it
happens to be standing in; it takes the nearest empty cell that something
could actually hold onto (row 0, or a cell with an occupied neighbour). That
is more forgiving on tight shots and it structurally cannot place a bubble
that is already floating.

**The aim guide is the physics.** The dotted trajectory walks the shot's own
path — wall bounces and all — through the same `hits_bubble` test the live
shot uses, so the preview can never disagree with what happens when you fire.

**One cell value, several kinds of thing.** The grid is still a flat `i32`
per cell — `-1` empty, `0..5` a colour, then a star or a stone. That makes
`>= 0` ("is something here?") and "is this a colour I can index a table
with?" two different questions, which is what `is_color` is for: every site
that treats a cell as a colour — the palette, the cluster fill, the launcher's
colour pool, the interval's colour count — asks it first. The metal shot
shares the same numbering but is a launcher payload only and never reaches
the grid, which the autoplay harness asserts on every shot.

**Pacing is four rules, not a difficulty curve.** There is no level-indexed
table of numbers anywhere. The ceiling counts landed shots; the interval is
read off the colours on the board; the turn clock runs while it is your shot
to take; and the payouts (doubling for drops, decaying for speed) put the
pressure in the scoring rather than in an escalating handicap. Between them
they keep a competent player on a knife edge without the game ever deciding
in advance how hard it should be. All four are longstanding arcade
bubble-shooter conventions rather than anything invented here; the specific
numbers are this demo's own.

## FC features demonstrated

- **Tuples** — `bubble_rgb` returns `{u8, u8, u8}`, `neighbor` returns
  `{i32, i32}`, `cell_center` returns `{f64, f64}`; every caller destructures
  with `let { a, b } = f(…)`. Three-channel colours and grid coordinates
  travel together without a struct declaration apiece.
- **A world struct threaded explicitly** — all mutable gameplay state is born
  in `main` and passed as `world*`. The renderer, audio device, and rng stay
  outside it: they are channels (output, output, entropy), not state the game
  owns.
- **Heap slices with `alloc`** — `alloc(world)!`, `alloc(i32[168] { })!`,
  `alloc(faller[96] { })!`; `alloc` zero-fills, so the inline `game_state`
  and `shot` fields start clean.
- **Union types for game phases** — `splash`, `playing`, `paused`,
  `level_clear`, `game_over`, dispatched with `match`.
- **Extern structs and functions** for SDL2 C interop via
  `demos/shared/sdl2.fc`.
- **Procedural audio** — square waves with a linear attack/decay envelope,
  generated at startup and queued through SDL audio.
- **Drawing primitives built from rectangles** — SDL2 has no circle, so
  `disc` scanlines one out of horizontal bands and `thick_line` walks a
  segment normal. Every bubble, pip, and gloss highlight is those two calls.
- **Bitmap font** — 3x5 glyphs packed into an `i32`, scaled up for the HUD
  and overlays.
- **String interpolation** — HUD and popup text via `"%d{w.game.score}"`.
- **Result types** — `io.open` returns `T!`, matched with `ok`/`err`; the
  best-effort `io.close` is dropped with `ignore`.
- **Stdlib modules** — `std::io`, `std::text`, `std::sys`, `std::math`,
  `std::random` (PCG generator).
