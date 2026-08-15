# fuzzel-fobble

A Puzzle Bobble / Bust-a-Move clone written in FC — a hex-packed bubble raft, a
rotating launcher with a bounce-accurate aim guide, flood-fill match detection,
gravity for everything you cut loose, and a ceiling that grinds down on you.

It builds on **SDL2** or on **raylib**, from one set of sources. Which one is a
compile-time flag, and the only file that differs between the two builds is the
backend — 100-odd lines out of 1400. See [Architecture](#architecture).

## Usage

```
./demos/fuzzel-fobble/run-sdl2.sh        # needs an SDL2 dev package
./demos/fuzzel-fobble/run-raylib.sh      # fetches and builds raylib on first run
```

Each script builds with the matching response file, which carries the required
`--flag backend=…`. Building by hand without it is a compile error rather than
a half-wired program:

```
$ fcc @demos/fuzzel-fobble/sdl2.rsp -o ff.c        # fine
$ fcc demos/fuzzel-fobble/*.fc … -o ff.c           # no flag
demos/fuzzel-fobble/main.fc:47:5: error: static assertion failed: fuzzel-fobble
needs a backend: --flag backend=sdl2 or --flag backend=raylib
```

**SDL2** requires the library installed for your environment:

- Linux: `libsdl2-dev` (or your distro's equivalent)
- MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-SDL2`
- MSYS2 MINGW64: `pacman -S mingw-w64-x86_64-SDL2`

**raylib** has no package to install. `run-raylib.sh` fetches raylib 5.5's
source on first run (~42 MB), builds the seven modules the demo uses into
`demos/shared/raylib/build/<os>/libraylib.a`, and links that in. That tree is
gitignored — nothing third-party is committed; delete it to force a re-fetch.
What you do still need is what raylib itself links against: OpenGL and X11 dev
packages on Linux (`libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libxi-dev` on Debian/Ubuntu), nothing extra on MSYS2, and the
Xcode command line tools on macOS.

Both builds share the best-score file (`~/.fuzzel-fobble/highscore.txt`).

## Controls

| Key | Action |
|-----|--------|
| Left / Right (or A, D) | Aim |
| SPACE | Fire (also starts / restarts) |
| ENTER | Start / restart |
| TAB | Swap the loaded bubble with the next one |
| P | Pause |
| S | Toggle the colour marks (off by default) |
| M | Mute / unmute the music |
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

## Architecture

Six files, in four layers:

| File | Lines of code | What it is |
|---|---:|---|
| `plat.fc` | 16 | The contract: `struct input`, the nine actions, and the `gfx` interface written out as a comment. |
| `gfx_sdl2.fc` | 154 | `module gfx` on SDL2. |
| `gfx_raylib.fc` | 101 | `module gfx` on raylib. |
| `game.fc` | 792 | The rules. No pixels, no keys, no library. |
| `art.fc` | 410 | The look, drawn on `gfx`'s four primitives. |
| `sound.fc` | 143 | Eleven instruments, nine effects, one tune. |
| `main.fc` | 46 | Hands them to each other and loops. |

Everything but the backend is shared, byte for byte: **1407 lines of game
against 154 (SDL2) or 101 (raylib) lines of platform.** Adding a rule or
retouching a bubble is one edit, not two edits kept in step.

### How the swap works

`gfx_sdl2.fc` and `gfx_raylib.fc` each declare `module gfx`, and a build takes
exactly one of them. The response files decide which:

```
# sdl2.rsp                      # raylib.rsp
--flag backend=sdl2             --flag backend=raylib
../shared/sdl2.fc               ../shared/raylib.fc
gfx_sdl2.fc                     gfx_raylib.fc
../shared/opl_audio_sdl.fc      ../shared/opl_audio_raylib.fc
plat.fc game.fc art.fc …        plat.fc game.fc art.fc …          <- identical
```

The same trick appears twice: `module gfx` is the renderer, and `module
opl_dev` — the audio device for the shared OPL2 engine — is the same either/or
one directory over (§5 below).

This is deliberately **not** a struct of function pointers. `gfx.disc(...)` is
a direct call resolved at compile time; the layering costs nothing at run time,
which is the only kind of abstraction a language with no GC to foot the bill
should be offering. FC has no interfaces and no traits, and does not need them
here: the interface is a *module name*, and the build says which file defines
it. The compilation unit is the unit of substitution — the same technique a C
project uses when it compiles one of `platform_win32.c` / `platform_posix.c`,
with the difference that FC resolves and type-checks the call at the boundary.

The `backend` flag is required, and it is checked in two places, each catching
a different mistake:

- `main.fc` static_asserts that it is one of the two values at all — a build
  with no flag fails there;
- each backend static_asserts that it names *that* backend — so a unit can't
  end up with one backend's sources and the other's flag.

Nothing in `demos/shared/` reads the flag. The audio engine used to, to pick
its device; now the device is a file of its own that the response file lists
(§5 below), so the flag stays a fact about *this game*, which genuinely has
two answers, rather than something a shared module has to know the answer set
for.

### Where the line falls

The rule the split follows is: **a backend owns everything it is the only one
able to answer, and nothing else.**

- `gfx` gets the window, the frame, the four primitives, and the *translation*
  of key codes into actions. It does not get any decision the game would make
  identically on both — the clear colour is passed in, the logical size is
  passed in, and what SPACE means is `game.fc`'s business.
- `game.fc` gets everything that would be the same on a third backend, which
  is all the rules and all the state. It is handed one `input` per frame and
  never learns what pressed it.
- `art.fc` is the interesting case: it is *not* platform code, because it is
  the same on both, but it is not gameplay either. It draws — on four
  primitives, in logical coordinates, naming no library.

Two edges are worth naming because they are the ones a purist would argue
with. The palette (`cell_rgb`) lives in `game.fc`, not `art.fc`, because the
rules spawn particles tinted by the bubble that popped — the colour is game
data and the shading on top of it is art. And the window title carries the
level, so `main.fc` watches `w.game.level` for a change rather than letting
`game.step` reach out to the window.

### What it cost

Two things, both in the SDL2 backend, and both worth knowing about before you
copy the pattern.

**A global.** SDL2 threads an `SDL_Renderer *` through everything that draws.
For `art.fc` to be shared, that handle cannot be a parameter — so it is a
module-level `let mut` in `gfx_sdl2.fc`. FC allows that (a module-level `let`
is a frozen constant; a `let mut` is a writable global in static storage), but
it means this build, like the raylib one, can have exactly one window. That is
raylib's constraint, adopted as the contract. A program that needs two windows
should thread a handle and not use this design.

**Two tuning knobs, absorbed.** The SDL2 build's `disc` took a band height and
its `hex_fill` took a step, chosen per call site. A parameter that only one
backend can use has no place in a shared interface, so the band is now derived
from the radius inside `gfx_sdl2.fc` — 4px on a bubble, 2px on a spark. Two
call sites out of nineteen render a band coarser or finer than they used to.
This is the general shape of the tax: an interface that both libraries can
implement is slightly less expressive than either one alone.

## SDL2 vs raylib

The demo started as two copies of the same game, one on each library, kept for
the comparison. Collapsing them into one program did not change the answer —
it sharpened it, because what used to be a 1300-line diff is now two files you
can put side by side.

| | SDL2 | raylib |
|---|---|---|
| Bindings | `shared/sdl2.fc`, 176 lines | `shared/raylib.fc`, 228 lines |
| Backend | `gfx_sdl2.fc`, 154 code lines | `gfx_raylib.fc`, 101 code lines |
| Game code | — | identical |
| Generated C | 5473 lines | 5362 lines |
| Linked binary | 190 KB (against a 1.9 MB shared libSDL2) | 1.8 MB (static, no runtime dependency but the system's GL/X11) |
| Library on the machine | distro package | fetched + built by `run-raylib.sh` |

### 1. Real 2D primitives — the biggest win

SDL2's renderer draws points, lines and axis-aligned rectangles. That is all.
Everything round in fuzzel-fobble is built out of rectangles:

```fc
// gfx_sdl2.fc — a filled circle as horizontal scanline bands
let disc = (cx: i32, cy: i32, rad: i32, cr: u8, cg: u8, cb: u8, ca: u8) ->
    let step = band(rad)
    let fr = (f64) rad
    let mut y = -rad
    loop
        if y >= rad then break
        let mid = (f64) y + (f64) step * 0.5
        let inside = fr * fr - mid * mid
        if inside > 0.0 then
            let hw = (i32) math.sqrt(inside)
            if hw > 0 then
                fill(cx - hw, cy + y, hw * 2, step, cr, cg, cb, ca)
        y = y + step

// gfx_raylib.fc
let disc = (cx: i32, cy: i32, rad: i32, cr: u8, cg: u8, cb: u8, ca: u8) ->
    raylib.fill_circle(cx, cy, (f32) rad, rgba(cr, cg, cb, ca))
```

Three of the four primitives collapse this way — `disc` 12 lines to 1,
`thick_line` 11 to 3, `hex` (plus its half-width helper) 18 to 4. That is
most of the 53-line difference between the two backends.

The quality gap is widest on the *small* discs, which is not where you would
expect to look for it. A bubble's specular highlight is `rad = 5` at
`step = 2` — five bands of widths 6, 8, 10, 8, 6, i.e. a lumpy octagon. And
`(i32) math.sqrt(inside)` floors every half-width, biting up to a pixel off
each side, so the shading — which is entirely the crescent between the rim
disc and the body disc — comes out ragged where two staircases of different
riser heights fail to line up. In raylib both are true circles and the
crescent is a clean lune, which is most of why the raylib bubbles read as
glossier rather than merely smoother.

Verdict: **the reason to pick raylib for this kind of game.** It is not only
shorter, it is *better*: the circles are round instead of stepped, and they
cost one draw call instead of a dozen.

### 2. No logical size — the biggest loss

`SDL_RenderSetLogicalSize(ren, 1280, 720)` is one call, at setup, and from then
on SDL letterboxes and scales every frame for you. raylib has no equivalent, so
`gfx_raylib.fc` draws into a `RenderTexture2D` and blits it itself: about 20
lines in `frame_end`, plus a `LoadRenderTexture` at startup and a
`BeginTextureMode` in `frame_begin`. Three traps in it, all of which cost real
debugging time:

- **The source height must be negative.** An OpenGL framebuffer's rows run
  bottom-up; flipping the source rectangle is how raylib says "read it the
  other way". Get it wrong and the game renders upside down.
- **The blit needs `BLEND_ALPHA_PREMULTIPLY`.** raylib's default `BLEND_ALPHA`
  is `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` — not the
  `…Separate` variant — so the source alpha scales the target's *alpha*
  channel too. A 62%-opaque white (the bubble specular) leaves its texel at
  alpha 0.62² + 0.38 = 0.764 rather than 1.0, and blitting that back with
  alpha blending multiplies the colour by 0.764. Every translucent thing in
  the frame — highlights, glints, the danger line, particle fades, the
  splash/pause/game-over dimming — renders about 24% too dark. Measured: the
  cherry specular came out `(189,141,146)` against SDL2's `(247,185,191)`,
  the same factor on all three channels, while the opaque body colour matched
  exactly. This one is nastier than the upside-down bug precisely because it
  *looks* fine — the softer highlights read as tasteful until you put a colour
  picker on them. The SDL2 backend blends straight to the backbuffer and never
  meets it; it is a cost of the render texture, not of raylib's blending.
- **`FLAG_WINDOW_HIGHDPI` breaks the arithmetic.** The SDL2 backend passes
  `SDL_WINDOW_ALLOW_HIGHDPI` and logical size absorbs the difference. Under
  raylib's flag on a 2x X11 desktop, `GetScreenWidth()` starts reporting
  physical pixels while raylib's own projection stays in logical ones — so
  every measurement you take from raylib disagrees with the space raylib draws
  into, by exactly the DPI factor, and the letterbox lands at twice its size.
  The demo does not pass the flag. This is not FC's problem or the binding's;
  a C raylib program has the same behaviour, confirmed against a straight C
  reproduction.

Verdict: **the one place SDL2 is plainly better for this game.** A fixed
logical resolution with letterboxing is what nearly every 2D game wants, and in
raylib you write it, own it, and own its bugs.

One accidental consolation, worth naming because it cuts against the verdict:
the two approaches scale *different things*, and raylib's happens to look
better. `SDL_RenderSetLogicalSize` scales **geometry** — every rectangle is
rasterized crisply at the window's real resolution, so a 4px logical band
becomes a hard-edged ~7px staircase on a 2x display. raylib rasterizes into the
1280x720 texture and then bilinearly upscales that **raster**, which
antialiases the polygon edges into gradients. So the raylib build renders at
lower effective resolution than the SDL2 one and still comes out smoother.

### 3. Polled input instead of an event queue

SDL2 delivers keydown/keyup events, so a held key is a state the backend has to
keep for itself. raylib latches the keyboard once per frame inside `EndDrawing`
and answers both `IsKeyDown` and `IsKeyPressed` from the latch. `plat.input`
asks for both — held *and* went-down-this-frame — so each backend gives one for
free and computes the other:

```fc
// gfx_raylib.fc — both come off the latch
private let latch = (inp: input*, key: i32, action: i32) ->
    if raylib.is_key_down(key) then inp.held = inp.held | action
    if raylib.is_key_pressed(key) then inp.tapped = inp.tapped | action

// gfx_sdl2.fc — rebuilt from the queue's edges
inp.held = inp.held | a          // on keydown (repeats excluded)
inp.tapped = inp.tapped | a
inp.held = inp.held & ~action_for(k)     // on keyup
```

Under the old two-copy demo this difference reached into the game: the SDL2
build carried `aim_left` / `aim_right` fields in `game_state` that existed only
to turn edge events back into held state, and the raylib build didn't. Those
fields are gone — turning edges into state is the queue-shaped backend's job,
and now it is done in the backend.

Verdict: **a clear win here, and a trap elsewhere.** Two presses of the same
key inside one frame read as one under raylib, where SDL2 delivers both. At 60
Hz that is not a distinction a human can make and this game has no rhythm
input — but it would be the wrong trade in a fighting game, and it is not a
thing you can opt out of.

Related freebies: `WindowShouldClose()` covers both the window's close button
and the ESC key, and `ToggleBorderlessWindowed()` is the whole of what the SDL2
backend hand-rolls as get-display-bounds / unborder / move / resize — and it
restores the previous geometry on the way back, which the SDL2 version has to
remember to do itself.

### 4. Handles, and by-value structs

SDL2 threads an `SDL_Renderer *`; raylib keeps the window, the GL context and
the draw target in globals of its own. In the two-copy demo this was the single
biggest mechanical diff — ~35 `draw_*` functions and ~130 call sites carrying a
parameter on one side and not the other. Under the layered design it vanishes
into `gfx_sdl2.fc`'s one module-level `let mut`, at the price named in
[What it cost](#what-it-cost).

Note the colour, too: SDL2 sets a colour on the renderer and then draws; raylib
takes a `Color` **by value** on every call. FC handles both — an `extern struct`
is a C struct — but by-value structs are the shape of raylib's whole API
(`Vector2`, `Rectangle`, `AudioStream`, `RenderTexture2D` are all passed and
returned by value), and it was worth confirming FC crosses that boundary
cleanly. It does, including nested structs returned by value.

### 5. Audio: the same design, minus a pointer

Both builds run the OPL2 engine from the host's audio callback, on the host's
audio thread, and both post commands to it over the same lock-free ring.
`demos/shared/opl_audio.fc` is shared *and library-free*: the device is a
separate thirty-line file — `opl_audio_sdl.fc` or `opl_audio_raylib.fc`, each
defining `module opl_dev` — and the response file lists one, exactly as it
lists one `gfx` backend. The engine itself names no library and reads no
flag.

The one real difference is that raylib's callback signature is

```c
void (*AudioCallback)(void *bufferData, unsigned int frames)
```

with **no userdata pointer**, where `SDL_AudioSpec` carries one. So the engine
instance the callback drives has to be reachable from file scope —
`gfx_raylib.fc` keeps it in a module-level `let mut` next to the callback that
reads it, which is the same escape hatch the renderer handle uses on the other
side.

Verdict: **a wash, tilting to SDL2.** raylib's audio is easier to *start* —
`InitAudioDevice()` takes no arguments and picks a device — and the buffer it
hands the callback is in the stream's own format (mono i16 here), resampled to
the device downstream, which is one less thing to negotiate than SDL2's
requested-vs-obtained `SDL_AudioSpec`. But a callback with no user pointer is a
design mistake that costs every non-trivial user a global. The push alternative
(`UpdateAudioStream` when `IsAudioStreamProcessed`) avoids the global and was
rejected: its sub-buffer size is silently rounded up to the device period, a
short write is zero-filled rather than queued, and there is no supported way to
ask what the size actually is.

### 6. Dependency shape

SDL2 is a distro package on every platform the demos target, so `run-sdl2.sh`
is five lines and a link flag. raylib is packaged much more thinly, which is
why `run-raylib.sh` fetches and builds it — 42 MB down, ~12 seconds of compile,
once.

That is not free, but it buys something: the resulting binary is self-contained
apart from GL and X11, where the SDL2 build needs a matching libSDL2 on the
machine that runs it. For a demo you hand somebody, static raylib is arguably
the easier delivery. For a demo somebody builds from a clean checkout, the
distro package is.

### Verdict

For this class of game — 2D, procedural art, one window, keyboard, synth audio
— **raylib wins on the drawing and loses on the framing.** The 2D primitives
are the real difference and they are worth a lot: a third of the SDL2 backend
exists only to draw circles and thick lines out of rectangles. Against that,
hand-rolling a letterbox is a fixed one-time cost, and the HighDPI flag being
unusable is a genuine wart.

Neither is a clear default, which is why the demo keeps both. What the exercise
established is that the FC side is a non-issue: raylib's by-value structs, its
struct returns, its nested structs and its bare function pointers all cross
`extern` cleanly, and swapping the platform under a 1400-line game is a
recompile.

## Display

The game draws in a fixed **1280x720 logical space** and the backend puts that
on the window: `SDL_RenderSetLogicalSize` for SDL2, a render texture and a
hand-rolled letterbox for raylib (§2 above). The window is resizable and is
maximized at startup. **F11** toggles a borderless fullscreen — hand-rolled
from the display bounds on SDL2, `ToggleBorderlessWindowed()` on raylib.

## Sound

Everything you hear is FM synthesis on an emulated **OPL2 (YM3812)** — the
chip an AdLib or Sound Blaster card put in a 1990 PC. Three files:
`demos/shared/opl2.fc` is the chip (shared with the `wolf-fc` project, which
uses it to play Wolfenstein 3D's own IMF music), `demos/shared/opl_audio.fc`
is the reusable engine built on it, and `sound.fc` is this game's content —
eleven instruments, nine effect scripts, one tune.

Two chips run side by side. One plays a three-voice arrangement of *Twinkle,
Twinkle, Little Star* — music box on top, plucked bass underneath, and a
chiming inner arpeggio filling the eighth notes between melody notes — on a
thirty-second loop. The other is a six-voice effects chip: a launch chirp, a
wall tick, a landing thock, a pop, a falling glissando, a star sparkle, a
ceiling grind, a brass fanfare for a cleared board, and a sagging reed for the
end of a run. Splitting them means a burst of pops can never steal a register
out from under the tune.

Nothing is pre-rendered and nothing is on disk. Samples are generated inside
**the host's audio callback, on the host's own audio thread**, at the sound
card's rate — never by the game loop. That is not an implementation detail: a
game loop that produces the samples is also their clock, so every frame that
runs long or short bends the music's pitch and tempo, and a frame slow enough
to empty the buffer stops the tune mid-note. A callback asks for exactly the
samples the device needs at exactly the moment it needs them, so the frame rate
cannot reach the sound at all.

The game thread never touches a chip. It posts commands ("play sound 3", "stop
the music") onto a lock-free single-producer ring that the callback drains, so
the chips have exactly one mutator, nothing takes a lock, and no frame ever
waits on audio. A full ring drops the request — a sound nobody hears costs
less than a frame nobody sees.

**M** mutes the music without touching the effects; **P** holds it mid-phrase
and picks it up where it left off.

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

**The shot is smaller than it looks.** What stops a shot is a circle three
quarters of the drawn bubble's width, which is the tolerance every arcade
bubble shooter is built on: the sprite says what the board looks like, a
smaller circle says what you can get through. It is the difference between a
threadable gap and a decorative one. A one-bubble hole in a row leaves its
flanking centres 96 apart, so the shot has a 24-wide lane down the middle of
a 48-wide hole — where a full-width test would leave a lane of nothing, and
even a near-full one leaves a lane narrower than a tap of the aim key moves
the shot at that range. The floor is 27.9, the interstice between three
touching bubbles; go under it and shots tunnel through solid raft. Everything
between those two numbers is a judgement call about how generous the game is,
and the genre has always answered generously.

**The aim guide is the physics.** The dotted trajectory walks the shot's own
path — wall bounces and all — through the same `hits_bubble` test the live
shot uses, so the preview can never disagree with what happens when you fire.
That means the same *step*, too, not just the same test: a guide that samples
the path more coarsely than the shot does can stride over a graze the shot
will catch, and then it has drawn you through a gap you stick in. It walks
the 3 px substep and thins the dots on the way out. It also lives in `art.fc`
and calls straight into `game.hits_bubble` — the view is allowed to read the
rules, which is exactly why it cannot drift from them.

**One cell value, several kinds of thing.** The grid is still a flat `i32`
per cell — `-1` empty, `0..5` a colour, then a star or a stone. That makes
`>= 0` ("is something here?") and "is this a colour I can index a table
with?" two different questions, which is what `is_color` is for: every site
that treats a cell as a colour — the palette, the cluster fill, the launcher's
colour pool, the interval's colour count — asks it first. The metal shot
shares the same numbering but is a launcher payload only and never reaches
the grid. It also keeps its own collision radius: what the metal ploughs
through is a question about overlap, not about sticking, so loosening the
grab doesn't thin out the plough.

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

- **Compile-time backend selection** — two files declaring one `module gfx`,
  chosen by which one the response file lists, with `--flag backend=…`
  required and checked by `static_assert`. Conditional compilation appears in
  exactly three places in the whole program, and never in the game.
- **Module-level `let mut` as a writable global** — the SDL2 renderer handle
  and raylib's audio-engine pointer, each in the module that owns it. (A
  module-level plain `let` is the opposite: a frozen, ROM-able constant.)
- **Cross-file name resolution without imports** — `struct world` and `module
  gfx` are top-level declarations, so every file in `global::` sees them;
  `art.fc` pulls the game's constants in with `import * from game`.
- **Tuples** — `bubble_rgb` returns `{u8, u8, u8}`, `neighbor` returns
  `{i32, i32}`, `cell_center` returns `{f64, f64}`; every caller destructures
  with `let { a, b } = f(…)`. Three-channel colours and grid coordinates
  travel together without a struct declaration apiece.
- **A world struct threaded explicitly** — all mutable gameplay state is born
  in `game.create` and passed as `world*`. The renderer, audio device, and rng
  stay outside it: they are channels (output, output, entropy), not state the
  game owns.
- **Heap slices with `alloc`** — `alloc(world)!`, `alloc(i32[grid_cells] { })!`,
  `alloc(faller[max_fallers] { })!`; `alloc` zero-fills, so the inline
  `game_state` and `shot` fields start clean.
- **Union types for game phases** — `splash`, `playing`, `paused`,
  `level_clear`, `game_over`, dispatched with `match`.
- **Extern structs and functions** for C interop via `demos/shared/sdl2.fc` and
  `demos/shared/raylib.fc` — including raylib's by-value struct arguments and
  returns.
- **Procedural audio** — two emulated OPL2 chips driven by note-list
  sequencers on the host's audio thread. See **Sound** above.
- **Lock-free threading** — `demos/shared/opl_audio.fc` carries game-thread
  requests to the audio thread over an `spsc.ring<cmd, 64>`, a
  const-generic single-producer queue using `atomic_load_acquire` /
  `atomic_store_release` and a power-of-two capacity enforced by
  `static_assert`. It is the only shared state between the two threads.
- **Drawing primitives built from rectangles** — SDL2 has no circle, so
  `gfx_sdl2.disc` scanlines one out of horizontal bands and `thick_line` walks
  a segment normal. Every bubble, pip, and gloss highlight in that build is
  those two calls; in the raylib build they are one call each.
- **Bitmap font** — 3x5 glyphs packed into an `i32`, scaled up for the HUD
  and overlays.
- **String interpolation** — HUD and popup text via `"%d{w.game.score}"`.
- **Result types** — `io.open` returns `T!`, matched with `ok`/`err`; the
  best-effort `io.close` is dropped with `ignore`.
- **Stdlib modules** — `std::io`, `std::text`, `std::sys`, `std::math`,
  `std::random` (PCG generator).
