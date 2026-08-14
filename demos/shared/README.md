# demos/shared

Modules the demos have in common. Nothing here is a library in the stdlib
sense — these are demo-support files, and a demo pulls in only the ones it
needs by listing them in its `lsp.rsp` (the response file its `run.sh` builds
from, and the one `fcc --lsp` reads to scope the editor's analysis).

Everything here lives in **`namespace shared::`**, so a demo names what it uses
(`import sdl2 from shared::`) the same way it does for `std::`. Within the
namespace the modules see each other without imports, which is why `opl_audio`
can drive `opl2` and the platform layer with no import lines of its own.

| File | What it is | Depends on |
|---|---|---|
| `sdl2.fc` | SDL2 bindings (`module sdl2`) | SDL2 headers + `-lSDL2` |
| `raylib.fc` | raylib bindings (`module raylib`) | raylib source, fetched and built by `demos/fuzzel-fobble-raylib/run.sh` into the gitignored `raylib/` |
| `opl2.fc` | OPL2 / YM3812 FM chip emulator (`module opl2`) | `stdlib/math.fc` |
| `opl_audio.fc` | Game audio engine built on the chip (`module opl_audio`, `module spsc`) | `opl2.fc`, one of `sdl2.fc` / `raylib.fc`, `stdlib/math.fc`, `stdlib/io.fc` |

`fuzzel-fobble` is the worked example of the SDL2 set. Its `sound.fc` is
nothing but data — instruments, effect scripts, a tune — which is the shape a
game's audio file is meant to have.

`sdl2.fc` and `raylib.fc` cover deliberately the same ground, because
`fuzzel-fobble` and `fuzzel-fobble-raylib` are the same game over each of
them — see [that demo's README](../fuzzel-fobble-raylib/README.md) for what
the swap costs and buys.

---

# The audio system

Everything is FM synthesis on an emulated **OPL2 (YM3812)**, the chip an AdLib
or Sound Blaster card put in a 1990 PC. No samples, no assets, no files: a
game supplies register values and note numbers, and the engine turns them into
audio inside the host's audio callback.

Two chips run per engine instance — one for music, one for effects — so a
burst of effects can never disturb the music's registers.

The engine is backend-agnostic: only the ~30-line Device section at the foot
of `opl_audio.fc` names SDL or raylib, and it is `#if`-gated. Everything
else — the chips, the ring, the sequencers, the mixdown — is the same code
either way. See [Picking a backend](#picking-a-backend) below.

## Wiring it into a game

Add three files to your demo's `lsp.rsp` (order is irrelevant; FC compiles
whole-program). Paths there are relative to the response file itself:

```
# demos/yourgame/lsp.rsp
../shared/sdl2.fc
../shared/opl2.fc
../shared/opl_audio.fc
sound.fc
main.fc

../../stdlib/io.fc
../../stdlib/math.fc
```

Then import what you name directly — `opl2` stays an implementation detail of
the engine, so a game never mentions it:

```fc
import sdl2 from shared::
import opl_audio from shared::
```

Then, in `main`:

```fc
sdl2.init(sdl2.init_video | sdl2.init_audio)   // init_audio is required

let a = snd.init(44100)                        // your sound.fc builds the bank + song
if !opl_audio.start(a) then                    // opens the device, starts the callback
    io.write("Audio: device open failed - running silent\n", stdout)

opl_audio.music_begin(a)                       // start the tune
...
opl_audio.play(a, snd.sfx_pop)                 // fire an effect, from anywhere
```

and at the end of `main`, **nothing**. See [Shutdown](#shutdown) — that is not
an omission.

If the device fails to open, every call above stays safe: commands queue into a
ring nobody drains and are dropped once it fills. There is no silent-mode
branch to write.

**Put the data inside a `module`.** A game's sound file should be one
`module snd = …`, not top-level `let`s, because a *file-level* initializer may
not reference other bindings — so a `sounds` table naming its instrument
constants (`instr = i_shoot`) compiles at module level and is rejected at file
level. Module scope also freezes the tables, which is what gives them the
`const i32[]` / `const instrument[]` types the `bank` and `song` fields expect.

### Picking a backend

The SDL2 backend is the default and needs nothing said. To run the engine on
raylib instead, list `../shared/raylib.fc` in place of `../shared/sdl2.fc` and
add the flag to your `lsp.rsp`:

```
--flag raylib_audio
```

The only visible difference is `start`. raylib's audio callback is
`void (*)(void *buffer, unsigned int frames)` with **no userdata pointer**, so
the engine instance it drives cannot be passed in and has to be reachable from
file scope — which FC allows only in the entry-point file. So the raylib
`start` takes the callback rather than owning it, and your `main.fc` supplies
a two-line trampoline over the public `opl_audio.render`:

```fc
let mut audio_engine = default(any*)            // file scope, entry-point file

let audio_callback = (buffer: any*, frames: u32) ->
    let a = (opl_audio.audio*) audio_engine
    opl_audio.render(a, i16[] { ptr = (i16*) buffer, len = (i64) frames },
                     (i32) frames)

// in main, before the device opens — the callback reads it immediately after
audio_engine = (any*) a
if !opl_audio.start(a, &audio_callback) then
    io.write("Audio: device open failed - running silent\n", stdout)
```

`demos/fuzzel-fobble-raylib` is the worked example. Everything else in this
document — the threading rules, the API surface, the shutdown note — applies
unchanged to both.

### API surface

The game thread calls only these. Everything else in `opl_audio` belongs to
the audio thread.

| Call | Effect |
|---|---|
| `init(rate, bank, song, music_gain, sfx_gain) -> audio*` | Build the engine. No device yet. |
| `start(a) -> bool` | Open the device and start the callback. `false` = no audio device. Takes `(a, cb)` under `raylib_audio` — see [Picking a backend](#picking-a-backend). |
| `play(a, id)` | Fire effect `id` on the next voice by rotation. |
| `music_begin(a)` | Start the tune from the top. |
| `music_end(a)` | Stop it and rewind, so the next `music_begin` opens on bar 1. |
| `music_hold(a)` / `music_unhold(a)` | Pause mid-phrase and resume there. |
| `music_toggle_mute(a)` | The player's own switch, independent of the above. |
| `music_muted(a) -> bool` | For a HUD indicator. |
| `render(a, out, frames)` | The consumer side of the ring. Called *by* the callback under `raylib_audio`; otherwise offline only — see [Tuning without launching](#tuning-without-launching-the-game). Exactly one caller, ever. |

`music_end` and `music_hold` are different on purpose: a run ending should
rewind, a pause menu should not. The mute switch is tracked separately again,
so the game's stop/start and the player's mute can never undo each other.

## Threading contract

Samples are generated **in the host's audio callback, on the host's audio
thread**, at the sound card's rate. They are *not* generated by the game loop
and pushed (`SDL_QueueAudio`, raylib's `UpdateAudioStream`).

That is the whole reason this engine exists. Under the push model the game
loop is the sample clock: every frame that runs long or short bends the
audio's timebase, so music wobbles in pitch and tempo, and a frame slow enough
to empty the queue stops the tune mid-note. A callback asks for exactly the
samples the device needs at exactly the moment it needs them, so frame pacing
cannot reach the sound at all.

The cost is a second thread on the chips, and it is paid once, here:

- The game thread **only ever posts commands** onto an `spsc.ring<cmd, 64>` —
  a wait-free single-producer queue using `atomic_load_acquire` /
  `atomic_store_release`, with a power-of-two capacity enforced by
  `static_assert`.
- The callback drains that ring and is the **sole mutator** of every chip,
  voice and sequencer field.
- Nothing else is shared. No lock is taken anywhere. The game thread never
  blocks on audio, and audio never waits on a frame.
- A full ring **drops** the request. A sound nobody hears costs less than a
  frame nobody sees.

So: never read or write an `audio` field from the game thread. The two
exceptions are `dev` and `muted`, which the game thread owns.

## Shutdown

**Do not call `SDL_CloseAudioDevice`. Do not call `SDL_Quit`. Do not call
raylib's `CloseAudioDevice`. Do not free anything reachable from `audio*`.**

Every one of those calls *joins* the audio callback thread. When a host audio
server wedges that thread — a sink disappearing mid-session is the usual way —
the join never returns and the exiting game hangs forever. Neither library
offers a close-with-timeout, so the robust shutdown is not to join at all: let
the process exit and the kernel reap the thread, the device, and the memory.
Closing the audio connection by process death also releases the device cleanly
for the next launch. Freeing engine state while the device is open is unsafe
anyway — a live callback could read a chip mid-free.

Tearing down the window and renderer *is* correct and worth doing: that is
video, which an audio wedge cannot block. raylib's `CloseWindow` does not
touch the audio device, so it is safe on that side too.

Verified in fuzzel-fobble: exit code 0 in ~1 s with the device live and the
callback running.

---

## Programming instruments

An `instrument` is eleven bytes: five registers for the **modulator**, five
for the **carrier**, and one they share. The field names say which register
each lands in.

```fc
opl_audio.instrument {
    m_char = 0x03, m_scale = 0x53, m_atk = 0xF4, m_sus = 0x36, m_wave = 0x00,
    c_char = 0x01, c_scale = 0x02, c_atk = 0xF4, c_sus = 0x36, c_wave = 0x00,
    n_conn = 0x00 }
```

| Field | Reg | Bit layout |
|---|---|---|
| `m_char` / `c_char` | `0x20` | `AM 0x80` \| `VIB 0x40` \| `EGT 0x20` \| `KSR 0x10` \| `MULT 0x0F` |
| `m_scale` / `c_scale` | `0x40` | `KSL << 6` \| `TL` (0..63) |
| `m_atk` / `c_atk` | `0x60` | `AR << 4` \| `DR` |
| `m_sus` / `c_sus` | `0x80` | `SL << 4` \| `RR` |
| `m_wave` / `c_wave` | `0xE0` | waveform 0..3 |
| `n_conn` | `0xC0` | `FB << 1` \| connection (bit 0: 0 = FM, 1 = additive) |

### The mental model

In FM mode the modulator's output bends the carrier's phase. So:

- **`TL` on the carrier is volume. `TL` on the modulator is brightness.** They
  are the same register field doing two completely different jobs, and it is
  the single most common thing to get backwards. `TL` counts *down*: 0 is
  loudest, 63 silent, 0.75 dB per step.
- **`MULT` picks the timbre** via the frequency ratio between the pair. `1:1`
  with feedback is a reedy sawtooth, `2:1` hollow, `3:1` and up metallic and
  bell-like, `7:1` pure glitter. `MULT = 0` means ×0.5 — an octave *below* the
  note, which is how you get weight out of a chip with no noise generator.
- **`EGT`** decides the note's shape: set = hold at the sustain level while the
  key is down (organ, brass, reed); clear = keep decaying to nothing on its own
  (bell, pluck, percussion).
- **`FB`** (0..7) applies the modulator to itself. On a 1:1 pair it is what
  turns a sine into a saw; it is the usual source of "edge".
- **Waveform** 0 = sine, 1 = half-sine, 2 = abs-sine, 3 = quarter-pulse. Half-
  sine on a modulator adds odd harmonics, which is what makes a short blip read
  as *wet* rather than as a beep.

### Envelopes

Four stages per operator, rates 0..15, **higher is faster**. Rough time for a
full envelope traversal (KSR off, mid register — see the gotchas, it moves
with pitch):

| Rate | ~time | Rate | ~time |
|---|---|---|---|
| 1 | 32 s | 7 | 0.5 s |
| 2 | 16 s | 8 | 0.25 s |
| 3 | 8 s | 9 | 0.12 s |
| 4 | 4 s | 10 | 60 ms |
| 5 | 2 s | 11 | 30 ms |
| 6 | 1 s | 12–15 | near-instant |

**Rate 0 is frozen, not slow** — the envelope never advances at all. An `AR` of
0 is a note that never sounds; an `RR` of 0 is one that never releases.

A percussive voice's total length is `DR` down to the sustain level, then `RR`
the rest of the way. Setting `RR ≈ DR` makes the key-off at the end of a script
seamless.

`SL` is where decay ends: 0..14 is 0 to −42 dB in 3 dB steps.

`KSL` is the other pitch-dependent field: 0 = off, then 1, 2, 3 = 1.5, 3, 6 dB
of attenuation per octave as the note rises. It is there to stop high notes
shouting over low ones — useful on a melody voice, actively harmful on one
whose job is to ring at the top of the register.

### Four gotchas that will cost you an afternoon

1. **`SL = 15` is silence, not "hold".** The YM3812 special-cases it to full
   attenuation, so `c_sus = 0xF5` — which looks like "sustain, release 5" —
   decays your sustaining instrument out to nothing while the key is still
   down. For a voice meant to hold, use `SL = 0` (`c_sus = 0x06`).
2. **`KSR` and `KSL` are pitch-dependent, by design.** `KSR` shortens the
   envelope as pitch rises; `KSL` attenuates it. On a voice that lives at the
   top of the register they will quietly make it short and quiet at exactly the
   moment it is meant to ring out. Turn both off for high bells and sparkles.
3. **A key-on only retriggers on a *rising* key bit.** Keying on a channel that
   is already on slides the pitch of the note already sounding instead of
   articulating a new one. The engine's `key_on` issues a key-off first, so you
   never see this — but you will if you drive `opl2` directly.
4. **Reprogram a voice *before* keying it on, not while it sounds.** Changing an
   operator's level or waveform under a ringing note is an audible click. The
   engine keys off first for the same reason.

---

## Effects: the bank

A game's whole effect set is three arrays.

```fc
struct bank =
    instruments: const instrument[]   // the voices
    sounds: const sound[]             // one entry per effect id
    script: const i32[]               // every effect's steps, end to end
    tick_rate: i32                    // steps per second; 140 is the AdLib rate
```

`script` is flat `(note, ticks)` pairs — *all* effects' steps concatenated —
and each `sound` says where its own steps begin:

```fc
struct sound =
    instr: i32      // index into bank.instruments
    first: i32      // index of this sound's first step
    steps: i32      // how many steps it has
```

Splitting the steps out of the struct is what lets every effect in a game live
in one readable table:

```fc
private let script = i32[66] {
    72,1, 79,1, 84,3,                            // shoot     3 steps
    64,3,                                        // bounce    1
    50,2, 45,3,                                  // stick     2
    84,1, 79,1, 74,1, 69,1, 64,1, 59,4,          // drop      6
    ...
}

private let sounds = opl_audio.sound[9] {
    opl_audio.sound { instr = i_shoot,  first = 0, steps = 3 },
    opl_audio.sound { instr = i_bounce, first = 3, steps = 1 },
    opl_audio.sound { instr = i_stick,  first = 4, steps = 2 },
    ...
}
```

A note of `-1` is a rest. At the end of the last step the voice **keys off**,
so the instrument's release rate — not the script — decides how long the tail
rings. A pitch sweep is just several short steps in a row.

`voices = 6`, taken strictly round-robin, so effects overlap: a pop landing on
top of a still-ringing pop is what a chain reaction is supposed to sound like.
Play a seventh effect before the first is done and the first is cut.

The effect clock is independent of the music's: at `tick_rate = 140` a step is
about 7 ms, which is about as fine as a sweep needs before the steps stop being
audible.

## Music: the song

```fc
struct song =
    melody: const i32[]     // (note, length in grid steps) pairs; -1 = rest
    chords: const i32[]     // one chord index per half-bar
    roots: const i32[]      // bass note per chord index
    arps: const i32[]       // arp_len arpeggio notes per chord index
    grid: i32               // sequencer steps per beat
    arp_len: i32            // arpeggio notes per half-bar; 0 = no arpeggio
    bpm: i32
    lead: instrument
    bass: instrument
    arp: instrument
```

Three voices, but **only the melody carries note data**. The bass and the
arpeggio are read off `chords`, one index per half-bar, which is why a whole
arrangement costs barely more than the tune:

```fc
// 0 = C, 1 = F, 2 = G
private let chords = i32[25] { 0,0,  1,0,  1,0,  2,0,  0,1,  0,2, ... }
private let roots  = i32[3]  { 36, 41, 43 }                 // C3, F3, G3
private let arps   = i32[12] { 48,52,55,52,  48,53,57,53,  47,50,55,50 }
```

**`grid` is resolution, not range.** It is the number of sequencer steps per
beat: 2 puts the grid on eighths, 4 on sixteenths, 3 on beat-triplets. Every
duration in the song is counted in those steps, and a melody note may be any
whole number of them — at `grid = 2` a quarter note is `2`, a dotted half is
`6`. Raising the grid does not change the music: the same tune written at
`grid = 4` with every duration doubled renders byte-for-byte identically.

**The loop length is not stated.** It is `chords.len` half-bars, because the
chord table *is* the form — so the two cannot disagree.

The arpeggio spreads `arp_len` notes evenly across each half-bar, so a figure
keeps its rhythm at any grid. Set `arp_len = 0` to drop the voice for a
two-part tune.

### The one guard

A melody **longer** than the chord progression aborts at `init` with a message
naming both lengths. That case is silently truncated at the loop wrap — the
overhanging notes are simply never reached — so it has no audible signature to
debug from.

A melody **shorter** than the loop is deliberately fine: it wraps and repeats,
which is how you write a two-bar figure under an eight-bar progression without
spelling it out four times.

This is a runtime check, not a `static_assert`, and it cannot be one: the data
is compile-time constant, but summing it needs iteration, and FC's
constant-expression grammar admits no calls. Checked once at `init`, never per
tick.

## Pitch

A note is `block * 12 + semitone`, so each block is an octave and **48 is
middle C**. Range 0..95, clamped. Rests are `-1`.

```
        C   C#  D   D#  E   F   F#  G   G#  A   A#  B
oct 3   36  37  38  39  40  41  42  43  44  45  46  47
oct 4   48  49  50  51  52  53  54  55  56  57  58  59     A4 = 57 = 440 Hz
oct 5   60  61  62  63  64  65  66  67  68  69  70  71
```

The engine's F-number table is one equal-tempered octave at A = 440, rounded
for the YM3812's `f = fnum * 2^(block-1) * 49716 / 2^19`.

## Levels and mixing

A single OPL2 voice at `TL = 0` peaks around **±8192** (`opl2.output_scale` is
2.0). The two chips are summed with the per-source gains you passed to `init`,
then pass through one soft knee: below 24000 untouched, above it bending toward
full scale without reaching it. Gentle saturation is what an analog mixer did
with FM on a sound card, and it beats the flat-topping a hard clamp gives when
a fanfare and a fistful of effects land together.

Working numbers from fuzzel-fobble: `music_gain = 0.85`, `sfx_gain = 2.0`,
which puts single effects at 32–45 % of full scale, music at 30 %, and a busy
mix at ~70 % with only ~0.4 % of samples entering the knee.

## Tuning without launching the game

`render(a, out, frames)` runs the ring, both sequencers and the mixer exactly
as the callback does, but with no device attached. Point it at a `i16[]` and
write a WAV, and you can iterate on instruments without opening a window — and
listen to a single effect in isolation, which you cannot do in play.

```fc
let a = snd.init(44100)
opl_audio.play(a, snd.sfx_pop)
let buf = alloc(i16[44100 * 2] { })!
opl_audio.render(a, buf, 44100 * 2)      // two seconds, tail included
```

Do not call it from the game thread while a device is open. There must be
exactly one consumer of the ring, and with a device open the callback is
already it — under the SDL backend by doing this work inline, under the raylib
backend by calling this very function.

A useful trick for checking a tune's parts against each other — silence two of
the three music channels by writing their carrier `TL` directly, which leaves
the sequencing untouched:

```fc
opl2.write(a.music_chip, 0x40 + opl2.op_mod_offset(ch) + 3, 63)
```

## Cookbook

| You want | Change |
|---|---|
| Brighter / buzzier | Lower the **modulator** `TL` (`m_scale`), or raise `FB` |
| Louder | Lower the **carrier** `TL` (`c_scale`) |
| Longer tail | Lower `RR` (and `DR`) — each step down roughly doubles the time |
| Shorter, tighter | Raise `RR` and `DR` together |
| Bell / music box | `MULT` 3:1, `EGT` clear on both, fast `AR`, medium `DR`/`RR` |
| Plucked bass | `MULT` 1:1, `FB` 5–6, `EGT` clear, fast `AR`, medium decay |
| Brass / organ | `EGT` **set** on both, `SL = 0`, `FB` 6, moderate `AR` |
| Wooden knock | `MULT` 1:1, `FB` 4, very fast `AR`, `DR`/`RR` 7–8 |
| Sparkle | `MULT` 7:1, `KSR` and `KSL` **off**, slow `RR` |
| Weight without noise | `MULT = 0` (×0.5) on the modulator |

---

# opl2.fc — the chip

A from-scratch YM3812 emulator: 9 two-operator channels, four waveforms, the
real 9-bit envelope generator with its sub-rate fire patterns, KSR/KSL, and
both LFOs. Rhythm mode is not implemented. It is shared with the `wolf-fc`
project, which uses it to play Wolfenstein 3D's IMF music.

You should not need it directly — `opl_audio` covers the game-facing use. Reach
past it when you want to drive registers yourself:

| Call | Use |
|---|---|
| `init(rate) -> chip*` | Allocate a chip at a sample rate |
| `write(c, reg, val)` | Raw register write |
| `load_instrument(c, ch, ...)` | Program the 11 bytes onto a channel |
| `note_on(c, ch, block, fnum)` / `note_off(c, ch)` | Key a channel |
| `sample(c) -> i32` | Produce one mono sample |
| `silence_voices(c)` | Hush every voice, keeping the programming |
| `fill_ticked_vol(...)` | Stereo fill with a tick callback (used by wolf-fc's drivers) |

Sampling runs at the host rate rather than the chip's native 49716 Hz; the rate
constants are calibrated against the output rate.

---

# sdl2.fc — the bindings

Hand-written externs against `SDL2/SDL.h`, in `namespace shared::` as
`module sdl2`. It covers the union of what the demos need, not all of SDL:
accelerated rendering, high-DPI, alpha blending, textures, events, timing, and
audio. Add what you need — that is the file's job.

```fc
extern SDL_RenderDrawLine as draw_line: (any*, i32, i32, i32, i32) -> void
extern SDL_WINDOW_RESIZABLE as window_resizable: u32
extern struct SDL_Rect as rect =
    x: i32
    y: i32
    w: i32
    h: i32
```

Conventions and things worth knowing:

- **Opaque handles are `any*`.** `SDL_Window*`, `SDL_Renderer*` and
  `SDL_Texture*` are never dereferenced from FC, so they cross as `any*`.
- **`SDL_AudioSpec.callback` is `any*`, and you assign a function reference to
  it.** `callback = &opl_audio.callback` works because FC emits a C trampoline
  with SDL's exact `void (*)(void*, Uint8*, int)` signature. The lambda must be
  **non-capturing** — referencing only module-level bindings — since the
  trampoline passes a null context. Type `stream` as `u8*`, not `any*`, or the
  emitted C trips `-Werror=incompatible-pointer-types` against the real field.
- **A C `char*` field in an extern struct must be `any*`**, with an explicit
  `(const cstr)` cast at the use site. Declaring it `cstr` trips
  `-Werror=pointer-sign`.
- **Fake fullscreen beats real fullscreen.** `face-invaders` and
  `fuzzel-fobble` both toggle a borderless window sized to
  `SDL_GetDisplayBounds` rather than calling `SDL_SetWindowFullscreen`, which
  sidesteps a class of DPI/drawable-size quirks on some window managers. Re-set
  the logical size afterward.
- **`SDL_RenderSetLogicalSize`** lets a demo draw in one fixed coordinate space
  and leaves the scaling to SDL. Pair it with the
  `SDL_RENDER_SCALE_QUALITY=linear` hint before creating the renderer.
- **`SDL_INIT_AUDIO`** must be in the `sdl2.init` flags, or the device will not
  open.

---

# raylib.fc — the bindings

Hand-written externs against `raylib.h`, in `namespace shared::` as
`module raylib`. Deliberately scoped to the same ground `sdl2.fc` covers, so
that `fuzzel-fobble` and `fuzzel-fobble-raylib` are a fair comparison: init and
lifecycle, window management, 2D shapes, render textures, keyboard input,
timing, and a raw audio stream. Nothing 3D.

The library is **not in this repository**. `demos/fuzzel-fobble-raylib/run.sh`
fetches raylib's source into `demos/shared/raylib/` on first run and builds it
there; that directory is gitignored. Delete it to force a clean re-fetch.

```fc
extern struct Color as color =
    r: u8
    g: u8
    b: u8
    a: u8
extern DrawCircle as fill_circle: (i32, i32, f32, color) -> void
extern LoadRenderTexture as load_render_texture: (i32, i32) -> render_texture
extern KEY_SPACE as key_space: i32
```

What the port taught us about binding raylib specifically:

- **Structs cross by value, in both directions.** This is the defining shape
  of raylib's API — `Color` on every draw call, `Vector2` and `Rectangle` as
  arguments, `AudioStream` and `RenderTexture2D` *returned* by value, with
  `RenderTexture` nesting two `Texture`s inside it. All of it works through
  `extern struct` with no special handling. FC emits `struct <tag>`, and every
  raylib type is a tagged `typedef struct Name { … } Name;`, so the tag and
  the typedef name coincide.
- **There is no context handle to bind.** raylib keeps the window, the GL
  context and the current draw target in its own globals, so no drawing
  function takes one. That is why this file's drawing externs have one fewer
  parameter than their SDL2 counterparts, and why the demo's own `draw_*`
  helpers do too.
- **`SetAudioStreamCallback` takes a bare function pointer with no userdata**,
  unlike `SDL_AudioSpec.callback`. Bind it as `any*` and pass `&fn` the same
  way — but see [Picking a backend](#picking-a-backend) for the global it
  forces on the caller.
- **Enum constants bind as `extern NAME as alias: i32`.** raylib's key codes,
  config flags, log levels and texture filters are all plain C enumerators, so
  they need no special treatment.
- **`FLAG_WINDOW_HIGHDPI` is not usable as of raylib 5.5** — at least on X11
  with a 2x desktop. It makes `GetScreenWidth()` report physical pixels while
  raylib's projection stays in logical ones, so anything you size from those
  queries comes out scaled by the DPI factor. This reproduces in plain C; it is
  not a binding artifact. Leave the flag off.
- **There is no `SDL_RenderSetLogicalSize`.** Draw into a `RenderTexture2D` at
  the fixed size and blit it yourself with `DrawTexturePro`, with a **negative
  source height** — GL framebuffers are bottom-up, and flipping the source
  rectangle is how raylib says so. `demos/fuzzel-fobble-raylib/main.fc`'s
  `present` is the worked example.
- **Blit a render texture with `BLEND_ALPHA_PREMULTIPLY`, not the default.**
  raylib's `BLEND_ALPHA` is `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`
  — not the `…Separate` variant (`rlgl.h`) — so the source alpha scales the
  target's *alpha* channel as well as its colour. Draw a 62%-opaque white into
  a render texture and that texel keeps alpha 0.62² + 0.38 = 0.764 instead of
  the 1.0 an opaque target should have; blitting it back with alpha blending
  then multiplies its colour by 0.764, and everything translucent in the frame
  comes out about 24% too dark. Premultiplied blending is
  `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`, which drops the alpha factor
  from the colour term — correct whenever the target was cleared opaque. This
  bit the demo for real: its bubble highlights rendered at `(189,141,146)`
  instead of the SDL2 build's `(247,185,191)`, uniformly across all three
  channels, until the blend mode was set.
- **`SetTraceLogLevel(LOG_WARNING)`** early, or raylib narrates every texture
  and shader it loads over the game's own stdout.
