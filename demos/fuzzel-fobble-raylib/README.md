# fuzzel-fobble-raylib

`demos/fuzzel-fobble` ported from SDL2 to **raylib**, changing nothing about
the game. Same rules, same art, same music, same 1280x720 layout — the two
are meant to be run side by side and diffed.

The point is the diff. FC's demos have all been SDL2 so far, and the question
this answers is what the other obvious C game library costs and buys for the
kind of game these demos are: 2D, software-authored art, no assets, one
window, keyboard only, and a synthesised soundtrack.

For the game itself — rules, scoring, special bubbles, the sound engine — read
[`../fuzzel-fobble/README.md`](../fuzzel-fobble/README.md). Everything there
still applies. This file is only about the port.

## Usage

```
./demos/fuzzel-fobble-raylib/run.sh
```

**No package to install.** The script fetches raylib 5.5's source on first run,
builds the seven modules the demo uses into
`demos/shared/raylib/build/<os>/libraylib.a`, and links that in. The tree it
downloads to is gitignored, so nothing third-party is committed; delete
`demos/shared/raylib/` to force a clean re-fetch.

First run: ~42 MB downloaded, 20 MB on disk after extraction, and about 12
seconds to compile raylib (parallel, 14 cores). Every run after that reuses
the cached `.a`.

What you do still need is what raylib itself links against:

- Linux: OpenGL and X11 dev packages — on Debian/Ubuntu `libgl1-mesa-dev
  libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`
- MSYS2 UCRT64 / MINGW64: nothing extra; the toolchain has the OpenGL libs
- macOS: Xcode command line tools

Controls are unchanged. So is the best-score file
(`~/.fuzzel-fobble/highscore.txt`) — deliberately, since it is the same game
and a separate file would be a difference the comparison did not ask for.

## The port, by the numbers

| | SDL2 | raylib |
|---|---|---|
| Bindings file | `shared/sdl2.fc`, 171 lines, 98 externs | `shared/raylib.fc`, 215 lines, 107 externs |
| `main.fc` code lines (comments stripped) | 1352 | 1286 |
| `sound.fc` | — | byte-identical |
| Game logic changed | — | none |
| Generated C | 5430 lines | 5297 lines |
| Linked binary | 188 KB (against a 1.9 MB shared libSDL2) | 1.8 MB (static, no runtime dependency but the system's GL/X11) |
| Library on the machine | distro package | fetched + built by `run.sh` |

Normalising away the purely mechanical loss of the renderer handle (below),
the substantive difference between the two `main.fc` files is **−198 / +132
lines out of 1352** — and every one of those lines is in the platform layer.
Not one line of grid, flood fill, collision, scoring or level pacing moved.

## What changed, and what it cost

### 1. No renderer handle — the biggest mechanical diff

SDL2 threads an `SDL_Renderer *` through everything that draws. raylib keeps
the window, the GL context and the draw target in globals of its own, so
there is nothing to pass. Every drawing helper and all ~35 `draw_*` functions
lost their first parameter, and ~130 call sites lost their first argument.

```fc
// SDL2
let fill = (r: any*, x: i32, y: i32, w: i32, h: i32, cr: u8, ...) ->
    let rc = rect { x = x, y = y, w = w, h = h }
    sdl2.set_color(r, cr, cg, cb, ca)
    sdl2.fill_rect(r, &rc)

// raylib
let fill = (x: i32, y: i32, w: i32, h: i32, cr: u8, ...) ->
    raylib.fill_rect(x, y, w, h, rgba(cr, cg, cb, ca))
```

Verdict: **pleasant, with a caveat.** It is less noise, and for a
single-window game there is nothing to lose. It also means a second window,
or drawing from anywhere but the main thread, is not a thing you can express
— SDL2's explicit handle is the more general design and this game is simply
not the case that needs it.

Note the colour, too: SDL2 sets a colour on the renderer and then draws;
raylib takes a `Color` **by value** on every call. FC handles both — an
`extern struct` is a C struct — but by-value structs are the shape of raylib's
whole API (`Vector2`, `Rectangle`, `AudioStream`, `RenderTexture2D` are all
passed and returned by value), and it was worth confirming FC crosses that
boundary cleanly. It does, including nested structs returned by value.

### 2. Real 2D primitives — the biggest win

SDL2's renderer draws points, lines and axis-aligned rectangles. That is all.
Everything round in fuzzel-fobble is built out of rectangles:

```fc
// SDL2: a filled circle as horizontal scanline bands
let disc = (r: any*, cx: i32, cy: i32, rad: i32, step: i32, ...) ->
    let fr = (f64) rad
    let mut y = -rad
    loop
        if y >= rad then break
        let mid = (f64) y + (f64) step * 0.5
        let inside = fr * fr - mid * mid
        if inside > 0.0 then
            let hw = (i32) math.sqrt(inside)
            if hw > 0 then
                fill(r, cx - hw, cy + y, hw * 2, step, cr, cg, cb, ca)
        y = y + step

// raylib
let disc = (cx: i32, cy: i32, rad: i32, ...) ->
    raylib.fill_circle(cx, cy, (f32) rad, rgba(cr, cg, cb, ca))
```

Three helpers collapsed this way:

- `disc` — 12 lines to 1. The `step` band-height parameter disappeared from
  the signature and from 19 call sites.
- `thick_line` — 11 lines to 3. SDL2 draws `th` parallel lines along the
  segment normal, computing the normal itself; `DrawLineEx` takes a
  thickness.
- `hex_fill` + `hex_half_width` — 18 lines to 4. A hexagon of circumradius
  `rad` is exactly `DrawPoly(centre, 6, rad, 0)`.

The quality gap is widest on the *small* discs, which is not where you would
expect to look for it. A bubble's specular highlight is `rad = 5` at
`step = 2` — five bands, of widths 6, 8, 10, 8, 6, i.e. a lumpy octagon. And
`(i32) math.sqrt(inside)` floors every half-width, biting up to a pixel off
each side, so the shading — which is entirely the crescent between the rim
disc (`rad` 23, `step` 4) and the body disc (`rad` 19, `step` 3) — comes out
ragged where two staircases of different riser heights fail to line up. In
raylib both are true circles and the crescent is a clean lune, which is most
of why the raylib bubbles read as glossier rather than merely smoother.

Verdict: **the reason to pick raylib for this kind of game.** It is not only
shorter, it is *better*: the circles are round instead of stepped, and they
cost one draw call instead of a dozen. The SDL2 build's banded edges were a
deliberate pixel-art choice made partly out of necessity; raylib does not
force that choice either way.

### 3. Polled input instead of an event queue

SDL2 delivers keydown/keyup events, so a held key is a state the game keeps
for itself. raylib latches the keyboard once per frame inside `EndDrawing`
and answers `IsKeyDown` from the latch.

That deleted, in order: the `SDL_Event` value, the bounded 256-event drain
loop and its comment about why it has to be bounded, the keydown/keyup split,
and the `aim_left` / `aim_right` fields in `game_state` that existed only to
turn edge events back into held state. What is left is a flat list of
questions about the current frame:

```fc
if raylib.is_key_down(raylib.key_left) || raylib.is_key_down(raylib.key_a) then
    w.game.angle = w.game.angle - aim_speed
```

Verdict: **a clear win here, and a trap elsewhere.** Two presses of the same
key inside one frame read as one, where SDL2 delivers both. At 60 Hz that is
not a distinction a human can make and this game has no rhythm input — but it
would be the wrong trade in a fighting game, and it is not a thing you can
opt out of.

Related freebies: `WindowShouldClose()` covers both the window's close button
and the ESC key, and `ToggleBorderlessWindowed()` is the whole of what the
SDL2 build hand-rolls as get-display-bounds / unborder / move / resize — and
it restores the previous geometry on the way back, which the SDL2 version has
to remember to do itself.

### 4. No logical size — the biggest loss

`SDL_RenderSetLogicalSize(ren, 1280, 720)` is one call, at setup, and from
then on SDL letterboxes and scales every frame for you. raylib has no
equivalent. The demo draws into a `RenderTexture2D` and blits it itself:

```fc
let present = (target: raylib.render_texture) ->
    let sw = raylib.screen_width()
    let sh = raylib.screen_height()
    let scale = math.min((f64) sw / (f64) logical_w, (f64) sh / (f64) logical_h)
    ...
    raylib.draw_texture_pro(target.texture,
        raylib.rect { x = 0.0f32, y = 0.0f32,
                      width = (f32) logical_w, height = -(f32) logical_h },
        ...)
```

About 20 lines, plus a `LoadRenderTexture` / `SetTextureFilter` at startup and
a `BeginTextureMode` / `EndTextureMode` pair around the frame. Three traps in
it, all of which cost real debugging time:

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
  the same 0.764 factor on all three channels, while the opaque body colour
  matched exactly. This one is nastier than the upside-down bug precisely
  because it *looks* fine — the softer highlights read as tasteful until you
  put a colour picker on them. The SDL2 build blends straight to the
  backbuffer and never meets it; it is a cost of the render texture, not of
  raylib's blending as such.
- **`FLAG_WINDOW_HIGHDPI` breaks the arithmetic.** The SDL2 build passes
  `SDL_WINDOW_ALLOW_HIGHDPI` and logical size absorbs the difference. Under
  raylib's flag on a 2x X11 desktop, `GetScreenWidth()` starts reporting
  physical pixels while raylib's own projection stays in logical ones — so
  every measurement you take from raylib disagrees with the space raylib
  draws into, by exactly the DPI factor, and the letterbox lands at twice its
  size and runs off the window. The demo does not pass the flag. This is not
  FC's problem or the binding's; a C raylib program has the same behaviour,
  which I confirmed with a straight C reproduction before concluding it.

Verdict: **the one place SDL2 is plainly better for this game.** A fixed
logical resolution with letterboxing is what nearly every 2D game wants, and
in raylib you write it, own it, and own its bugs.

One accidental consolation, worth naming because it cuts against the verdict:
the two approaches scale *different things*, and raylib's happens to look
better. `SDL_RenderSetLogicalSize` scales **geometry** — every rectangle is
rasterized crisply at the window's real resolution, so a 4px logical band
becomes a hard-edged ~7px staircase on a 2x display. raylib rasterizes into
the 1280x720 texture and then bilinearly upscales that **raster**, which
antialiases the polygon edges into gradients. So the raylib build renders at
lower effective resolution than the SDL2 one and still comes out smoother.
The indirection you are forced to write pays for a little of itself.

### 5. Audio: the same design, minus a pointer

Both builds run the OPL2 engine from the host's audio callback, on the host's
audio thread, and both post commands to it over the same lock-free ring.
`demos/shared/opl_audio.fc` is shared between them: only its ~30-line Device
section differs, behind `#if !raylib_audio`, and this demo's `lsp.rsp` sets
that flag. Nothing about the synthesis, sequencing or threading changed.

The one real difference is that raylib's callback signature is

```c
void (*AudioCallback)(void *bufferData, unsigned int frames)
```

with **no userdata pointer**, where `SDL_AudioSpec` carries one. So the engine
instance the callback drives cannot be passed in and has to be reachable from
file scope. FC allows a mutable binding at file scope only in the entry-point
file, which is why this demo's `main.fc` carries a global the SDL2 build does
not need:

```fc
let mut audio_engine = default(any*)

let audio_callback = (buffer: any*, frames: u32) ->
    let a = (opl_audio.audio*) audio_engine
    let out = i16[] { ptr = (i16*) buffer, len = (i64) frames }
    opl_audio.render(a, out, (i32) frames)
```

Verdict: **a wash, tilting to SDL2.** raylib's audio is easier to *start* —
`InitAudioDevice()` takes no arguments and picks a device — and the buffer it
hands the callback is in the stream's own format (mono i16 here), resampled
to the device downstream, which is one less thing to negotiate than
SDL2's requested-vs-obtained `SDL_AudioSpec`. But a callback with no user
pointer is a design mistake that costs every non-trivial user a global. The
push alternative (`UpdateAudioStream` when `IsAudioStreamProcessed`) avoids
the global and was rejected: its sub-buffer size is silently rounded up to
the device period, a short write is zero-filled rather than queued, and there
is no supported way to ask what the size actually is.

### 6. Dependency shape

SDL2 is a distro package on every platform the demos target, so `run.sh` is
five lines and a link flag. raylib is packaged much more thinly, which is why
this demo fetches and builds it — 42 MB down, 12 seconds of compile, once.

That is not free, but it does buy something: the resulting binary is
self-contained apart from GL and X11, where the SDL2 build needs a matching
libSDL2 on the machine that runs it. For a demo you hand somebody, static
raylib is arguably the easier delivery. For a demo somebody builds from a
clean checkout, the distro package is.

## Verdict for FC's demos

For this class of game — 2D, procedural art, one window, keyboard, synth
audio — **raylib wins on the drawing and loses on the framing.** The 2D
primitives are the real difference and they are worth a lot: a third of the
platform code in the SDL2 build exists only to draw circles and thick lines
out of rectangles. Against that, hand-rolling a letterbox is a fixed
one-time cost, and the HighDPI flag being unusable is a genuine wart.

Neither is a clear default. What the exercise did establish is that the FC
side is a non-issue: raylib's by-value structs, its struct returns, its
nested structs and its bare function pointers all cross `extern` cleanly,
the port needed no compiler change, and swapping the platform layer under a
1900-line game is a mechanical afternoon.

## Files

| File | What it is |
|---|---|
| `main.fc` | The game. Differences from the SDL2 twin are marked `// raylib:`. |
| `sound.fc` | Byte-identical copy of the SDL2 build's — instruments, effects, the tune. |
| `lsp.rsp` | The compilation unit, including `--flag raylib_audio`. |
| `run.sh` | Fetches raylib, builds it, builds and runs the demo. |
| `../shared/raylib.fc` | The raylib bindings. |
| `../shared/opl_audio.fc` | Shared with the SDL2 build; its Device section is `#if`-gated. |
