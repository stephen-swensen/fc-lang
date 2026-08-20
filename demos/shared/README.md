# demos/shared

Modules the demos have in common. Nothing here is a library in the stdlib
sense — these are demo-support files, and a demo pulls in only the ones it
needs by listing them in its response file (the one its `run.sh` builds from,
and the one `fcc --lsp` reads — `lsp.rsp` by convention — to scope the editor's
analysis).

Everything here lives in **`namespace shared::`**, so a demo names what it uses
(`import sdl2 from shared::`) the same way it does for `std::`. Within the
namespace the modules see each other without imports, which is why `opl_audio`
can drive `opl2` and the platform layer with no import lines of its own.

| File | What it is | Depends on |
|---|---|---|
| `sdl2.fc` | SDL2 bindings (`module sdl2`) | SDL2 headers + `-lSDL2` |
| `raylib.fc` | raylib bindings (`module raylib`) | raylib source, fetched and built by `demos/fuzzel-fobble/run-raylib.sh` into the gitignored `raylib/` |
| `opl2.fc` | OPL2 / YM3812 FM chip emulator, incl. rhythm mode (`module opl2`) | `stdlib/math.fc` |
| `opl_midi.fc` | Standard MIDI File parser and General MIDI player (`module opl_midi`) | `opl2.fc`, `stdlib/math.fc` — **no I/O** |
| `opl_bank_gm.fc` | A hand-authored General MIDI instrument bank (`module opl_bank_gm`) | `opl_midi.fc` |
| `opl_audio.fc` | Game audio engine built on the chip (`module opl_audio`, `module spsc`) | `opl2.fc`, `opl_midi.fc`, `stdlib/math.fc` — **no platform library** |
| `opl_audio_sdl.fc` | SDL2 audio device for the engine (`module opl_dev`) | `opl_audio.fc`, `sdl2.fc` |
| `opl_audio_raylib.fc` | raylib audio device for the engine (`module opl_dev`) | `opl_audio.fc`, `raylib.fc` |

There are also two standalone tools in `tools/`: `midi_render.fc` renders a
`.mid` to a WAV file with no window and no audio device, and `bank_probe.fc`
plays every patch in a bank and reports what it measures — see
[Tuning without launching the game](#tuning-without-launching-the-game).

`fuzzel-fobble` is the worked example of the SDL2 set. Its `sound.fc` is
nothing but data — instruments and effect scripts — while its music is a real
Standard MIDI File in `music/`, loaded at startup and swappable with
`--music`.

`sdl2.fc` and `raylib.fc` cover deliberately the same ground, because
`fuzzel-fobble` builds on either of them from one set of sources — see
[that demo's README](../fuzzel-fobble/README.md) for what
the swap costs and buys.

---

# The audio system

Everything is FM synthesis on an emulated **OPL2 (YM3812)**, the chip an AdLib
or Sound Blaster card put in a 1990 PC. No samples, no assets, no files: a
game supplies register values and note numbers, and the engine turns them into
audio inside the host's audio callback.

Two chips run per engine instance — one for music, one for effects. That is
eighteen voices rather than nine and, more to the point, a burst of six
overlapping effects can never take a voice away from the tune, nor a dense bar
of music from a sound the player is waiting to hear. The two mix at the very
end and never contend.

Music is a **Standard MIDI File**, played by `opl_midi.fc` through the
instrument bank in `opl_bank_gm.fc`. Effects are a small bespoke format — a
bank of instruments and scripts of `(note, ticks)` pairs — because an effect is
a one-shot with tight timing rather than a piece of music.

### Drums: rhythm mode

The OPL2 has exactly one noise source, and it is not a register you can point
a melodic voice at — it is **rhythm mode**, which turns channels 6, 7 and 8
into five percussion voices (bass drum, snare, tom, cymbal, hi-hat) and drives
three of them from a 23-bit noise LFSR. Faking a drum kit on melodic voices
instead, with big inharmonic frequency ratios, gets you a dense *harmonic*
hash rather than noise: measured spectral flatness 0.49 against rhythm mode's
0.81 for a snare, where 1.0 is white noise.

`opl_midi` turns it on by itself, deciding once at `init` from the song and
the bank together — on if the bank has a kit and the song plays a note it
covers. There is no knob because there is no interesting way to answer it
wrong. It costs three melodic channels, and pays for them immediately: the
drums stop competing for voices at all, so a beat of kick and hi-hat that used
to occupy two of nine voices now occupies none of six.

The mapping is a **hybrid, not a switch**. Rhythm mode does the core kit very
well and everything else not at all — one hi-hat, no cowbell, no conga, no
tambourine — so the bank's `rhythm_map` names only the notes it does well
(kick, snare, six toms, both hats, two crashes) and every other percussion
note keeps its melodic voice. Two map entries may share one operator with
different envelopes, which is how open and closed hi-hats differ on a chip
with a single hi-hat.

The engine is backend-agnostic, and literally so: `opl_audio.fc` names no
platform library, contains no `#if`, and reads no flag. Everything about
*where the samples go* is one of the two thirty-line device files beside it,
each defining `module opl_dev`, of which a build lists one — the same
either/or, and the same mechanism, that `demos/fuzzel-fobble` uses to pick a
`gfx` backend. A program that wants the engine with no device at all (a tool
rendering to a file, a headless test) lists neither. See
[Supplying a device](#supplying-a-device) below.

## Wiring it into a game

Add six files to your demo's `lsp.rsp` (order is irrelevant; FC compiles
whole-program). Paths there are relative to the response file itself:

```
# demos/yourgame/lsp.rsp
../shared/sdl2.fc
../shared/opl2.fc
../shared/opl_midi.fc
../shared/opl_bank_gm.fc        # or your own bank, or none if you load one
../shared/opl_audio.fc
../shared/opl_audio_sdl.fc      # the device — swap for _raylib.fc on raylib
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
import opl_midi from shared::
import opl_bank_gm from shared::
import opl_dev from shared::      // whichever device file you listed
```

Then, in `main`:

```fc
sdl2.init(sdl2.init_video | sdl2.init_audio)   // init_audio is required

let tune = opl_midi.parse(io.read_all("music/song.mid")?)?
let p = opl_midi.init(44100, tune, opl_bank_gm.make())

let a = snd.init(44100, p)                     // your sound.fc builds the effect bank
if !opl_dev.start(a) then                      // opens the device, starts the callback
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

### Supplying a device

`opl_audio.fc` produces samples; it does not open anything. A device file
does that, and there are two, each defining `module opl_dev`:

| List this | With | For |
|---|---|---|
| `../shared/opl_audio_sdl.fc` | `sdl2.fc` | SDL2 |
| `../shared/opl_audio_raylib.fc` | `raylib.fc` | raylib |
| *neither* | — | rendering to a file, headless tests — call `render` yourself |

There is **no flag**: the file list is the selection, the same way
`demos/fuzzel-fobble` picks one of two files defining `module gfx`. A build
that lists the wrong one gets an undefined name from the bindings it isn't
linking, at compile time. This replaces an earlier `#if` inside the engine —
which meant a shared, library-agnostic module had to know the set of backends
that existed, and had to guess what an unrecognised one meant.

Both device files are thirty lines and both do the same thing: open the host's
device and call `opl_audio.render` from its callback.

The only visible difference between them is `start`. raylib's audio callback is
`void (*)(void *buffer, unsigned int frames)` with **no userdata pointer**, so
the engine instance it drives cannot be passed in and has to be reachable from
file scope. The raylib `start` therefore takes the callback rather than owning
it — a program may hold more than one engine, and only the caller knows which
one a given stream is for — so you supply a two-line trampoline over the
public `opl_audio.render` alongside a writable global to hold the engine:

```fc
module gfx =
    private let mut engine = default(any*)      // module-level `let mut`:
                                                // static storage, writable
    private let audio_callback = (buffer: any*, frames: u32) ->
        let a = (opl_audio.audio*) engine
        opl_audio.render(a, i16[] { ptr = (i16*) buffer, len = (i64) frames },
                         (i32) frames)

    let audio_start = (a: opl_audio.audio*) ->
        engine = (any*) a                       // before the device opens —
                                                // the callback reads it
                                                // immediately after
        opl_dev.start(a, &audio_callback)
```

`demos/fuzzel-fobble/gfx_raylib.fc` is the worked example: it keeps the engine
pointer beside the trampoline that reads it, and its `audio_start` is the two
lines above. Everything else in this document — the threading rules, the API
surface, the shutdown note — applies unchanged to both devices.

### API surface

The game thread calls only these. Everything else in `opl_audio` belongs to
the audio thread.

| Call | Effect |
|---|---|
| `init(rate, bank, player, music_gain, sfx_gain) -> audio*` | Build the engine over an effect bank and an `opl_midi.player*`. No device yet; the engine takes the player over from here. |
| *(the device's `opl_dev.start`)* | Open the device and start the callback. `false` = no audio device. Lives in the device file, not here — see [Supplying a device](#supplying-a-device). |
| `play(a, id)` | Fire effect `id` on the next voice by rotation. |
| `set_playlist(a, songs)` | Hand the engine the songs `music_select` may choose between. **Before the device opens** — after that the slice is the audio thread's. |
| `music_select(a, i)` | Put playlist entry `i` under the player, from the top. Out of range does nothing, so a game whose levels outrun its playlist keeps the tune it has. |
| `music_begin(a)` | Start the tune from the top. |
| `music_end(a)` | Stop it and rewind, so the next `music_begin` opens on the first event. |
| `music_hold(a)` / `music_unhold(a)` | Pause mid-phrase and resume there. |
| `music_toggle_mute(a)` | The player's own switch, independent of the above. |
| `music_muted(a) -> bool` | For a HUD indicator. |
| `render(a, out, frames)` | The consumer side of the ring, and the only way samples leave the engine. Called *by* the device's callback — or directly, with no device open, see [Tuning without launching](#tuning-without-launching-the-game). Exactly one caller, ever. |

`music_end` and `music_hold` are different on purpose: a run ending should
rewind, a pause menu should not. The mute switch is tracked separately again,
so the game's stop/start and the player's mute can never undo each other.

`music_select` is a playlist, not a second player. One `opl_midi.player`
carries every song a game has, because the expensive half of a player is the
emulated chip it owns — 68 KB of state against a song's few thousand events —
and a game with a piece per level wants a playlist, not a rack of
synthesisers. Underneath it is `opl_midi.set_song`, which re-decides
everything a song decides, rhythm mode included, and is safe *only* at a song
boundary: the chip is silenced on the way in, so the melodic voice count can
move from nine to six without a sounding note losing its voice underneath it.
A selected song obeys the same rule `music_begin` does — it plays if the music
is live, and waits at its first event if it is held or muted, so unmuting
opens the piece rather than dropping into the middle of it.

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

## Music: a MIDI file

Music is a Standard MIDI File on disk. Parse it, build a player over an
instrument bank, and hand the player to `opl_audio.init`:

```fc
let bytes = io.read_all("music/minuet.mid")?
let tune  = opl_midi.parse(bytes)?          // bytes, not a path — see below
let p     = opl_midi.init(44100, tune, opl_bank_gm.make())
let a     = opl_audio.init(44100, fx, p, music_gain, sfx_gain)
```

`parse` takes a `const u8[]` and `opl_midi.fc` imports no I/O at all, so a song
can come off disk, out of a static array compiled into the program, or over a
socket. Whoever has the bytes decides where they came from.

### What plays

Formats 0 and 1 both work and both come out as a single merged event list —
which is the whole of the difference between them once the tracks are merged.
Format 2 is a file of *independent* pieces, so merging it would be wrong and
playing only the first would be a guess; it is refused with a named error.

Handled: note on/off (including the velocity-zero note-off convention),
running status, program change, pitch bend, channel volume (CC7), expression
(CC11), sustain pedal (CC64), bend range via RPN 0, all-notes-off and reset
(CC120/121/123), tempo changes, and both time bases — ticks-per-quarter and
SMPTE. Dropped at parse time rather than carried and ignored: aftertouch,
sysex, and every meta event but tempo and end-of-track.

**Pan (CC10) is not implementable.** The OPL2 sums its nine voices to one mono
signal, so there is no per-voice output to place. This is a chip limit, not an
omission — an OPL3 has stereo bits per channel and is a strict register
superset, so the day this file grows an OPL3 mode is the day pan arrives.

### Nine voices, sixteen channels

A MIDI channel is **not** an OPL channel. MIDI has sixteen independent
polyphonic channels; the chip has nine two-operator voices. Between them sits a
voice allocator: a note is handed a physical voice at key-on and gives it back
at key-off, and when all nine are busy the next note steals one. Every real OPL
driver worked this way.

Stealing prefers, in order: a voice the chip reports has finished sounding,
then the longest-released one still ringing out, then the note that has been
held longest. The first tier is why `opl2.channel_idle` exists — reusing a
silent voice is inaudible and reusing a ringing one clicks.

A typical General MIDI file wants more simultaneous notes than nine, so
stealing runs more or less continuously. That thinning is what an AdLib card
actually sounded like. `player.steals` counts it, and `midi_render` reports the
total, which is the one number that says whether a piece fits the chip:
fuzzel-fobble's minuet steals **0** times, a sixteen-channel torture file
steals 711 in twenty seconds.

### Instruments: the bank

A MIDI file names its instruments and does not describe them — program 40 is
"Violin", and what a violin *is* has to come from somewhere else. That
somewhere is a `bank`:

```fc
struct bank =
    name: const str
    patches: patch[]        // the distinct voices, however many
    melodic: i32[]          // 128 entries: GM program -> index into patches
    drums: i32[]            // 128 entries: note number -> index, -1 = silent
```

The indirection is what keeps 128 programs from meaning 128 hand-authored
voices. `opl_bank_gm.make()` returns sixty voices — three or four per GM family
plus sixteen percussion — with every program pointed at the nearest, so nothing
is silent and nothing is wildly wrong, though a bassoon and an oboe share a
patch and a splash cymbal is a crash.

A `patch` is the same eleven register bytes as an effect `instrument`, plus two
fields the register set has no room for: `transpose`, and `fixed_note` for
percussion, where a drum's pitch belongs to the drum rather than to the note
number that selected it. `fixed_note = 0` means pitched, so a zeroed `patch` is
a valid melodic one and a bank written as source can omit both fields.

`opl_midi.load_bank` reads the three legacy formats — `.op2`/GENMIDI, `.ibk`
and `.sbi` — sniffing the magic to tell which. Worth knowing: nearly every bank
file in circulation was extracted from a commercial game and its
redistribution terms are unclear. Loading one a user supplies is a different
thing from shipping one, which is why `opl_bank_gm.fc` is written out by hand
and checked in.

`banks/` is where a borrowed one goes while you listen to it. The directory is
gitignored for the reason above, so it is empty in a fresh clone and every tool
that reads a bank — both demos' `--bank`, `midi_render`, `bank_probe` — takes a
path, so nothing depends on it existing. Put a `README` beside whatever you drop
in naming the licence you got it under.

### Temperament: what the keyboard is tuned to

A player tunes itself equal-tempered at A = 440 and will go on doing that
untouched. `set_temperament` changes both halves of that:

```fc
opl_midi.set_temperament(p, opl_midi.werckmeister3)
opl_midi.set_temperament(p, opl_midi.at_pitch(opl_midi.vallotti, opl_midi.baroque_a))
```

Twelve pure fifths overshoot seven octaves by the Pythagorean comma — about a
quarter of a semitone — and every tuning system is a decision about where to
put that error. Equal temperament spreads it evenly, so nothing is pure and
nothing is bad and all twelve keys sound alike; it is a nineteenth-century
default rather than a timeless one. A **well temperament** spreads it
unevenly: common keys come out near-pure, remote ones tense, all twelve stay
playable, and the difference between them is a colour composers before about
1800 wrote for. **Meantone**, older still, buys eight pure major thirds by
making the other four keys unusable.

```fc
struct temperament =
    name: const str
    cents: const f64[]      // 12 deviations from equal, C first
    a4: f64                 // pitch standard: the A above middle C, in Hz
```

Six are provided, and `temperaments` is the slice of all of them in menu
order — the one place the set is written down, and what `temperament_named`
reads for a command-line flag:

| Name | What it is |
| --- | --- |
| `equal` | The default, and the thing to A/B against |
| `werckmeister3` | Werckmeister III (1691), what a recording usually means by "Bach's tuning" |
| `kirnberger3` | Kirnberger III (1779), published by a pupil of Bach as what he was taught |
| `vallotti` | Vallotti (1754), the gentlest — modern period ensembles' default |
| `young2` | Young II (1800), Vallotti rotated one fifth sharper |
| `meantone` | Quarter-comma meantone — eight pure thirds and a howling wolf at G♯–E♭ |

The tables are published-form, reading zero at C; the player re-anchors them on
A as it reads, so `a4` always means what it says and switching temperaments
moves the colours without moving the pitch. `at_pitch` is the other axis:
`baroque_a` (415.3 Hz, a semitone below modern, roughly Bach's Leipzig) applies
to any of the six.

**Percussion is exempt.** A drum takes the pitch standard — tuning the ensemble
down should take the whole ensemble — but not the temperament, which is a
statement about intervals a drum has none of.

**What the chip does to it.** The OPL2 tunes by a 10-bit F-number, and the
player keeps that number between 512 and 1023 for every note from G1 up, so one
step is 1.7–3.4 cents and rounding costs at most half of that. The tables reach
±27 cents and the interval differences they exist to create are larger still —
Werckmeister III's major thirds span 18 cents end to end — so the character
comes through on a grid roughly a cent wide.

### Driving the player

```fc
let start    = (p: player*)             // from the top
let stop     = (p: player*)             // and rewind
let pause    = (p: player*)             // hold position, key everything off
let resume   = (p: player*)
let set_loop = (p: player*, on: bool)   // on by default
let finished = (p: player*) -> bool
let advance  = (p: player*)             // one output sample of clock
let set_temperament = (p: player*, t: temperament)
```

`set_temperament` is safe at any time, mid-phrase included: every sounding note
is retuned in place, so nothing is re-articulated and no voice is stolen. Like
everything else here it is the chip owner's to call — in a game that means
before `audio_start`, or through the engine's command ring.

`advance` moves the clock and dispatches whatever that reveals; it does **not**
produce a sample. The caller pulls audio from `p.chip` with `opl2.sample`.
Keeping the two apart is what lets `opl_audio` interleave two chips in one
mixing loop and lets `midi_render` drive the same code with no device at all.

Between two consecutive events the tempo is constant by construction — a tempo
change *is* an event — so the clock counts samples down to the next event
rather than ticking tick by tick. Cost is O(events), not O(ticks).

A loop wrap silences the chip and resets all sixteen channels to General MIDI's
defaults. Skipping that is the classic reason a looped MIDI gets quieter, or
stranger, on each repeat: it would inherit whatever volume, bend and pedal
state the last time round happened to end on.

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

There are two ways in, one per half of the engine.

**For music and instrument banks**, `tools/midi_render.fc` is a standalone
program: it reads a `.mid`, renders it through `opl_midi` to a WAV file, and
reports what happened. No window, no device, no game. The whole of
`opl_bank_gm.fc` was written against it.

```
fcc demos/shared/tools/midi_render.fc demos/shared/opl_midi.fc \
    demos/shared/opl_bank_gm.fc demos/shared/opl2.fc \
    stdlib/io.fc stdlib/math.fc stdlib/text.fc -o /tmp/mr.c
cc -std=c11 -o /tmp/mr /tmp/mr.c -lm

/tmp/mr demos/fuzzel-fobble/music/minuet.mid /tmp/out.wav
  demos/fuzzel-fobble/music/minuet.mid: division 480, 1813 events, 92160 ticks
  bank fc-gm: 60 voices
  rendered 4233600 frames (96.00 s) at 44100 Hz to /tmp/out.wav
  peak 20890 of 32767, 0 clipped samples, 0 voice steals
```

A third argument sets the length in seconds — ask for more than the song has
and looping turns on, which is how you audition the wrap. A fourth loads a
`.op2`/`.ibk`/`.sbi` bank in place of the built-in one, which is how two banks
get compared on the same piece; `-` there means the built-in bank.

A fifth names a temperament, optionally with a pitch standard after an `@`.
That is the pair of commands a tuning has to be judged by, since the whole
difference is a few cents per note:

```
/tmp/mr music/minuet.mid /tmp/eq.wav 20 - equal
/tmp/mr music/minuet.mid /tmp/kb.wav 20 - kirnberger3@415
```

**For the bank itself**, `tools/bank_probe.fc` plays every patch under
identical conditions and reports two numbers per voice: its loudness, measured
over its own sounding span rather than a fixed window, and how long it lasts.
Then it names anything more than 6 dB under the bank's median, because that is
what makes an arbitrary MIDI file sound badly mixed — a program change that is
also a volume change. It takes an optional bank file, so a downloaded `.op2`
can be compared against the built-in one before you trust it with your music.

```
/tmp/bp
  bank fc-gm: 60 voices
  ...
  melodic: 44 patches, median -21.7 dBFS, middle 80% spans 4.6 dB
  drums  : 16 patches, median -26.9 dBFS, middle 80% spans 8.8 dB
```

Writing it found two things worth knowing about any OPL bank. **SL is a level,
not a rate**: on a voice that decays on its own, the note falls that far before
the release rate is consulted at all, and as fast as DR allows — SL 11 threw
away 33 dB in ten milliseconds and left a hi-hat that measured 24 dB under the
rest of its kit. Length belongs in RR. And **patches have to be level-matched,
because velocity cannot do it for them**: this bank's melodic voices once
spanned 23 dB at the same velocity, and now sit inside about 4 dB.

A third came from the acoustic piano — program 0, and so the voice every file
that names no instrument arrives on. It was reasoned out from first principles
twice and wrong both times; what fixed it was measuring a real one. The
University of Iowa publishes single grand-piano notes struck ff and left to
decay, and analysing C2 through C6 of them gives a target no amount of
reasoning about FM had produced:

**A piano's spectral centre of gravity is roughly constant in hertz across the
keyboard** — around 500 Hz from C2 to C5 — rather than constant in harmonics.
A bass note is therefore its *eighth* partial, with the fundamental 21 dB down,
and a treble note is very nearly a sine. That is the soundboard's radiation
curve, and it is what separates a piano from a synthesiser playing the same
notes. Miss it and the bottom octave comes out as a plucked bass guitar.

On a two-operator voice that means the modulation index must fall steeply with
pitch: KSL at its maximum 6 dB/octave, plus a modulator at MULT 3, because a
multiplier scales the reach of whatever level KSL has left — it lifts the bass
hard and the treble not at all, where simply turning the modulator up lifts
everything equally.

**But a ratio buys reach with gaps.** Sidebands land at the carrier plus and
minus multiples of the modulator, so 3:1 fills every harmonic except multiples
of three. Take the same reach from a richer modulator *waveform* instead and
the gaps get much worse: abs-sine doubles the modulator's own frequency, so the
effective ratio is 6:1 and a bass note arrives as partials 1, 5, 7, 11, 13, 17
and nothing else. Six widely spaced partials is not a piano, it is the standard
recipe for a bell — and it sounded like one. Sparse-and-high and dense-and-low
are both wrong; what a piano wants is dense *and* high, and on two operators
the multiplier is the only one of the two knobs that does not cost density.

**And a piano darkens much less than it seems to** — a factor of about 1.5 in
the first tenth of a second, then it holds. What the ear reads as the note
closing is mostly the amplitude decay. An earlier attempt collapsed the
brightness 18 dB, which leaves a sine: a flute, not a piano.

Two things the chip cannot do, both measured before being given up: the real
double decay (fast to −20 dB, then very slow) costs 2 to 9 dB of level that a
carrier already at TL 0 has no way to earn back; and a real decay spans a
factor of 6.5 from C2 to C6 where one KSR bit buys 3.9, so the rate is chosen
for the register the music is actually in.

All of this shows up in a mix rather than on a meter: twenty-one Bach
engravings came out 2 to 6 dB quieter afterwards, because a note that never
decays is a note still at full level when the next one lands.

It reports levels rather than managing them. A game mixes this against effects
and puts a soft knee after the sum; a tool that quietly did the same would hide
exactly the thing you are trying to hear, so this one hard-clamps and tells you
how often it had to.

**For effects**, `render(a, out, frames)` runs the ring, both sequencers and
the mixer exactly as the callback does, but with no device attached. Point it
at an `i16[]`, write a WAV, and you can listen to a single effect in isolation
— which you cannot do in play.

```fc
let a = snd.init(44100, music)
opl_audio.play(a, snd.sfx_pop)
let buf = alloc(i16[44100 * 2] { })!
opl_audio.render(a, buf, 44100 * 2)      // two seconds, tail included
```

Do not call it from the game thread while a device is open. There must be
exactly one consumer of the ring, and with a device open the callback is
already it — under the SDL backend by doing this work inline, under the raylib
backend by calling this very function.

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
that `fuzzel-fobble`'s two backends are a fair comparison: init and
lifecycle, window management, 2D shapes, render textures, keyboard input,
timing, and a raw audio stream. Nothing 3D.

The library is **not in this repository**. `demos/fuzzel-fobble/run-raylib.sh`
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
  way — but see [Supplying a device](#supplying-a-device) for the global it
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
  rectangle is how raylib says so. `demos/fuzzel-fobble/gfx_raylib.fc`'s
  `frame_end` is the worked example.
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
