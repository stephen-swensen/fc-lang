# opl_midi — a General MIDI player for the OPL2 demos

A plan for `demos/shared/opl_midi.fc`: parse a Standard MIDI File, allocate its
notes across the nine voices of the emulated YM3812, and play it. It replaces
the bespoke three-part sequencer inside `opl_audio.fc`, not `opl_audio.fc`
itself — the ring, the mixer, the effect voices and the device split all stay.

Status: **implemented.** All five open questions from the first draft are
settled and folded into the text below; §12 records them. §13 records what
shipped and how it was verified. Listening is the user's, done separately.

Archive this to `spec/hist/` once it has served its purpose, per the
convention `MANGLING-PLAN.md` and `BACKTRACES-O2-PLAN.md` set.

---

## 1. What the shape of the thing is

Four concerns, deliberately separable, so the pieces are testable on their own
and a program can take only what it needs:

| Concern | Lives in | Depends on |
| --- | --- | --- |
| Chip | `opl2.fc` (exists) | `std::math` |
| SMF parsing → a time-ordered event list | `opl_midi.fc` | nothing |
| Patch bank (GM program → OPL registers) | `opl_bank_gm.fc` (data) | `opl_midi.fc` for the type |
| Playback: voices, channels, tempo | `opl_midi.fc` | `opl2.fc` |
| Game audio: ring, SFX, mixer, device | `opl_audio.fc` (exists, narrowed) | `opl_midi.fc` |

The parser takes a `const u8[]` and knows nothing about files, so a caller can
`io.read_all` a path, embed the bytes as static data, or receive them over a
socket. `opl_midi.fc` imports no `std::io`.

The bank is a plain value, not a compile-time file swap. `opl_bank_gm.fc`
supplies one as source data; `opl_midi.load_bank` parses a standard bank file
into the same type at runtime. A caller passes whichever it has.

### Two chips, and why that stays

`opl_audio` runs a separate `opl2.chip` for music and for effects. That is 18
voices rather than 9, and — more to the point — music and effects can never
steal voices from each other, so a burst of six overlapping pops cannot punch
holes in the tune. With voice stealing about to become a constant background
activity on the music side, that isolation gets *more* valuable, not less.
Two chips, two independent allocators. Unifying them is off the table.

---

## 2. Phase 0 — three prerequisites in `opl2.fc`

Small, independently reviewable, and each one improves the existing demos on
its own. Do these first and separately, so a change in how fuzzel-fobble sounds
can be attributed.

**Note:** `wolf-fc/src/opl2.fc` is a byte-identical copy of this file apart
from its missing `namespace shared::` line. All three changes are mirrored
there, so the two copies stay code-identical.

### 0a. Kill the per-sample `math.pow`

`sample()` calls `math.pow` once per active channel for the block scale
(`opl2.fc:670`) and again for any channel with feedback (`opl2.fc:699`). At the
current three-voice tune that is invisible. A MIDI player keeps all nine voices
busy, which at 44.1 kHz is 400k–800k `pow` calls per second inside the audio
callback.

Both exponents are small integers over fixed ranges. Precompute in `init`:

- `block_scale: f64[8]`, `block_scale[b] = 2^(b-1)`
- `fb_div: f64[8]`, `fb_div[f] = 2^(9-f)`

Two 8-entry tables, two lookups replacing two `pow` calls. No behaviour change.

### 0b. `note_off` must preserve block and F-number

Today (`opl2.fc:777`):

```fc
let note_off = (c: chip*, channel: i32) ->
    write(c, 0xB0 + channel, 0)
```

**Is that a spec violation?** No. Register `0xB0+n` packs F-num high bits
(0–1), block (2–4) and key-on (5) into one byte, and writing `0x00` to it is a
perfectly legal write. Nothing in the YM3812 spec forbids it.

**But it is not what a driver does, and it makes the release stage wrong in two
ways.** The AdLib-era drivers all kept a shadow copy of the `0xB0` byte and
wrote it back with bit 5 cleared. Zeroing the whole byte instead means:

1. **The release tail loses its pitch.** Key-off drops both operators into
   release; the envelope decays while the phase generator keeps running. With
   `fnum = 0` and `block = 0` the frequency is zero, so the phase stops
   advancing entirely and the operator output freezes at whatever value it held
   — the note decays as DC instead of ringing out at pitch. The emulator
   reproduces this faithfully, because `sample()` recomputes `base_freq` from
   `ch_fnum`/`ch_block` every sample (`opl2.fc:667`).
2. **The release runs at the wrong rate.** Envelope rates are key-scaled by
   (block, fnum) — `key_scale` at `opl2.fc:295`, applied through
   `refresh_channel_ops` on every `0xB0` write (`opl2.fc:551`). Zeroing them
   recomputes the release at the lowest key scale, so notes release *slower*
   than the hardware would at their actual pitch.

Why nobody has noticed: `opl_audio`'s `key_on` does note-off then note-on back
to back (`opl_audio.fc:256`), so for a retrigger the zeroed byte is overwritten
in the same instant. Only a genuine note-off — a rest in the melody, the end of
an effect script — leaves it zeroed, and those instruments have fast release
rates, so the DC freeze passes as an abrupt cutoff.

So this is a **bug fix, not a MIDI extension**, and it should not be optional.
The chip struct already holds the state:

```fc
let note_off = (c: chip*, channel: i32) ->
    write(c, 0xB0 + channel,
        ((c.ch_block[channel] & 7) << 2) | ((c.ch_fnum[channel] >> 8) & 3))
```

Two spellings of key-off, one of them subtly wrong, is exactly the trap this
codebase avoids elsewhere. One correct spelling.

**It will change how the existing demos sound** — release tails ring at pitch
and decay at the correct (faster) rate. Better, but different. Its own commit,
with a listen test, before any MIDI work stacks on top.

### 0c. A `channel_idle` query

The voice allocator wants to prefer a genuinely silent voice over one still
ringing out. The chip already knows:

```fc
// True when neither operator is producing output, so the channel can be
// reprogrammed without cutting a sounding note.
let channel_idle = (c: chip*, channel: i32) ->
    c.op_stage[channel * 2] == envelope_stage.off &&
    c.op_stage[channel * 2 + 1] == envelope_stage.off
```

Read-only, no state change. It is the difference between stealing that is
inaudible and stealing that clicks.

---

## 3. Phase 1 — the SMF parser

Standard MIDI File is one of the friendlier binary formats, and FC's result
types make the error paths read well.

```fc
error midi =
    | bad_header
    | bad_track
    | unsupported_format
    | truncated
```

### Events

A union with named payloads rather than an `i32` script — the whole point of
this exercise is to stop hand-decoding integer tuples.

```fc
struct note_msg = chan: i32, note: i32, vel: i32
struct ctrl_msg = chan: i32, cc: i32, val: i32
struct chan_msg = chan: i32, val: i32

union msg =
    | note_on(note_msg)
    | note_off(note_msg)
    | program(chan_msg)
    | control(ctrl_msg)
    | bend(chan_msg)        // val: -8192..8191
    | tempo(i32)            // microseconds per quarter note
    | track_end

struct event =
    tick: i32               // absolute, from the start of the song
    m: msg

struct song =
    events: event[]         // heap, time-ordered, merged across tracks
    division: i32           // ticks per quarter (>0) or SMPTE (<0)
    length: i32             // total ticks, for looping
```

Roughly 20 bytes an event; a dense four-minute file is a few hundred KB.

### Parsing steps

1. **`MThd`** — 6-byte body: format, track count, division. Formats 0 and 1
   are the ~99% case and both fall out of the same code. Format 2 is
   independent sequences rather than one piece of music; reject it with
   `unsupported_format` rather than silently playing a fragment.
2. **Division** — positive is ticks per quarter note. Negative is SMPTE
   (`-frames_per_sec`, `ticks_per_frame`), where tick duration is absolute and
   tempo events do not apply. Ten extra lines, and it is part of what makes
   "most MIDI files" true rather than "most of the ones I tried".
3. **`MTrk` × n** — delta time (variable-length quantity) then an event.
   Running status (a data byte where a status byte was expected reuses the
   previous status) is not optional; plenty of real files lean on it heavily.
   Meta events are `FF type len data` — keep `2F` (end of track) and `51`
   (tempo), skip the rest. SysEx is `F0`/`F7` with a VLQ length; skip.
   Note-on with velocity 0 is a note-off, per spec — normalise it at parse
   time so the player never has to know.
4. **Merge** — each track is already sorted by tick, so a k-way merge over
   `ntrks` cursors produces the flat list in one pass. Stable in track order,
   which matters: a program change and a note-on at the same tick must stay in
   file order. About forty lines, and it leaves playback as a single index into
   a flat array — much easier to loop and rewind than per-track cursors.

Unknown/malformed status bytes abort the track with `bad_track` rather than
resyncing. A MIDI file that fails to parse is a broken file; guessing at it
produces noise, and the caller can print the error.

---

## 4. Phase 2 — the patch bank

### The type

A GM program number is a *name*, not a sound. The bank supplies the eleven
AdLib register bytes behind each name, plus two fields the register set has no
room for:

```fc
struct patch =
    m_char: i32     // 0x20  AM | VIB | EGT | KSR | MULT
    m_scale: i32    // 0x40  KSL<<6 | TL
    m_atk: i32      // 0x60  AR<<4 | DR
    m_sus: i32      // 0x80  SL<<4 | RR
    m_wave: i32     // 0xE0
    c_char: i32
    c_scale: i32
    c_atk: i32
    c_sus: i32
    c_wave: i32
    n_conn: i32     // 0xC0  FB<<1 | connection
    transpose: i32  // semitones, for patches voiced an octave off
    fixed_note: i32 // -1 = pitched; >= 0 = always sound this note (percussion)
```

The bank itself is **distinct patches plus two index tables**, which is what
keeps 128 programs from becoming 128 hand-authored entries:

```fc
struct bank =
    name: str
    patches: const patch[]  // however many distinct voices this bank has
    melodic: const i32[]    // 128 entries: GM program -> index into patches
    drums: const i32[]      // 128 entries: note number -> index, -1 = silent
```

A runtime-loaded bank fills `melodic` with an identity mapping. A hand-authored
one collapses each GM family onto a shared voice, so the source is a readable
table of forty-odd patches and two index arrays.

### The hand-authored bank — `opl_bank_gm.fc`

Checked into the repo, ours to redistribute, no licensing question. Target
roughly:

- **Melodic, ~44 distinct patches** spread over the sixteen GM families —
  three or four per family (pianos, chromatic percussion, organ, guitar, bass,
  strings, ensemble, brass, reed, pipe, synth lead, synth pad, synth effects,
  ethnic, percussive, sound effects), with every one of the 128 programs
  indexed onto the nearest.
- **Percussion, ~15 distinct patches** across notes 35–81: kick, snare,
  rimshot, low/mid/high tom, closed and open hi-hat, crash, ride, clap,
  cowbell, woodblock, tambourine, and a catch-all tick. Each with
  `fixed_note` set, since a drum's pitch belongs to the patch and not the note.

**This is the long pole of the whole project.** Not in lines — in ear time.
Every patch is four envelope rates, two levels, a multiplier ratio, a feedback
setting and a waveform, tuned by listening. The headless renderer in Phase 5
exists specifically so this can be done without launching a game.

Seeding it: fuzzel-fobble's existing three music instruments are hand-tuned,
proven through this exact emulator, and worth keeping on their own merits even
though the tune they were written for is being replaced. `ins_music_box`
becomes GM 11 (Music Box), `ins_chime` GM 9 (Celesta), and `ins_bass` seeds the
plucked-bass slots — three of the forty-four finished before we start, and the
demo keeps a thread of its old character.

### Swapping in a user bank

```fc
let load_bank    = (bytes: const u8[]) -> bank!   // sniffs the format
let load_genmidi = (bytes: const u8[]) -> bank!
let load_ibk     = (bytes: const u8[]) -> bank!
let load_sbi     = (bytes: const u8[]) -> patch!
```

All three legacy formats reduce to the same eleven bytes, so the second and
third are cheap once the first exists:

- **`.op2` / GENMIDI** — `#OPL_II#` magic, 175 entries (128 melodic + 47
  percussion) of 36 bytes each, then 175 32-byte names. Carries both the
  transpose and fixed-note fields this `patch` type already has, so it is the
  closest match and the most widely available.
- **`.ibk`** — `IBK\x1A` magic, 128 × 16-byte instruments plus 128 × 9-byte
  names. Melodic only; the drum table is left at the built-in bank's.
- **`.sbi`** — `SBI\x1A` magic, one instrument, 32-byte name + 16 bytes of
  register data. A single patch rather than a bank; useful for auditioning one
  voice, so it returns a `patch` and the caller decides where to put it.

`load_bank` sniffs the magic and dispatches, so a caller passes bytes and gets
a bank without caring which of the three it was.

One caution to document rather than solve: most circulating bank files are
extracted game data with unclear redistribution terms. Loading one a user
supplies is fine; shipping one is not, which is why the authored bank exists.

---

## 5. Phase 3 — channels, voices, and the mapping

### Channel state — 16 of them

```fc
struct channel =
    program: i32        // 0..127
    volume: i32         // CC7,  default 100
    expression: i32     // CC11, default 127
    bend: i32           // -8192..8191
    bend_range: i32     // semitones, default 2, set via RPN 0
    sustain: bool       // CC64
    rpn: i32            // currently selected RPN, for the bend-range dance
```

Channel index 9 is percussion: the note number picks the patch out of
`bank.drums` and the patch's `fixed_note` picks the pitch.

Controllers worth implementing, and nothing else: 1 (modulation — only as the
per-operator VIB bit, coarse), 7 (volume), 11 (expression), 64 (sustain),
100/101/6/38 (RPN select and data entry, for bend range), 120/123 (all sound
off / all notes off), 121 (reset controllers). Pan (CC10) is **not
implementable**: OPL2 is mono, and `opl2.sample` returns one summed value for
the whole chip, so there is no per-voice signal to pan even in our own mixer.
Document it as a chip limit, not a to-do.

### Voice allocation

```fc
struct voice =
    midi_ch: i32        // -1 = free
    note: i32
    patch: i32
    keyed: bool         // key still down (vs. released and ringing out)
    serial: u32         // allocation order, for oldest-first stealing
```

Nine of them, one per OPL channel. Allocation preference, in order:

1. A voice whose OPL channel reports `channel_idle` — genuinely silent, free.
2. A released-but-ringing voice, oldest first.
3. A sounding voice, oldest first.

Note-off is a linear scan of nine entries for a matching `(midi_ch, note)`;
with sustain down it marks the voice released-pending instead of keying off,
and the CC64 release keys off everything pending on that channel.

This is where the nine-voice ceiling actually bites. A typical GM file wants
8–14 simultaneous notes plus drums, so stealing runs more or less continuously.
That is authentic — it is why AdLib playback sounded thinner than the same file
on a Roland — but the stealing *policy* is the single biggest lever on how good
this sounds, and it is worth revisiting once there is something to listen to.

### Velocity, volume and expression → TL

Only the carrier's TL is loudness in FM connection. In additive connection
(`n_conn` bit 0) both operators reach the output, so **both** TLs must scale —
a detail naive implementations miss, and the reason additive patches come out
too loud elsewhere.

Combine multiplicatively and convert once:

```
level   = vel/127 * volume/127 * expression/127
tl_add  = round(-20 * log10(level) / 0.75)      // TL is 0.75 dB per step
tl      = clamp(patch_tl + tl_add, 0, 63)
```

Precompute a 128-entry velocity→attenuation table at init rather than calling
`log10` per note-on.

### Note → block and F-number

```
freq = 440 * 2^((note - 69 + bend_semitones + patch.transpose) / 12)
fnum = freq * 2^19 / (49716 * 2^(block-1))
```

Pick the block that lands `fnum` in 512..1023 for the best resolution, then
clamp block to 0..7 and fnum to 0..1023 — notes above roughly MIDI 107 run out
of chip, and clamping is the sane failure. The existing 12-entry `fnum_of`
table in `opl_audio.fc:249` cannot express bends and is retired.

Pitch bend needs no chip change: `note_on` on an already-keyed channel updates
fnum and block without retriggering, because the key-on path only fires on a
rising edge (`opl2.fc:552`). A named `opl2.set_pitch` wrapper is worth adding
for readability, but the mechanism is already correct.

`math.pow` per note-on and per bend message is fine — MIDI bandwidth caps that
in the low thousands per second. If it ever shows up in a profile, a
twelve-entry table with fractional interpolation replaces it.

---

## 6. Phase 4 — the player

```fc
struct player =
    chip: opl2.chip*
    song: song
    bank: bank
    rate: i32
    pos: i32            // index of the next event
    tick: i32           // current tick
    to_next: f64        // samples remaining until events[pos]
    per_tick: f64       // samples per tick at the current tempo
    playing: bool
    looping: bool
    channels: channel[16]
    voices: voice[9]
    serial: u32
```

Between two consecutive events the tempo is constant by construction — a tempo
change *is* an event — so the clock counts down samples to the next event
rather than ticking one tick at a time. That is O(events), not O(ticks).

```
per_tick = (usec_per_quarter / 1e6) * rate / division     // or, for SMPTE:
per_tick = rate / (frames_per_sec * ticks_per_frame)      // tempo-independent
```

Public API, small on purpose:

```fc
let init      = (rate: i32, song: song, b: bank) -> player*
let advance   = (p: player*)              // one sample of clock; dispatches due events
let start     = (p: player*)
let stop      = (p: player*)              // and rewind
let pause     = (p: player*)
let resume    = (p: player*)
let set_loop  = (p: player*, on: bool)
let finished  = (p: player*) -> bool
```

`advance` does not produce a sample — the caller pulls that from the chip with
`opl2.sample`, exactly as `opl_audio.fill` does today. Keeping the clock and
the sampling separate is what lets `opl_audio` interleave two chips in one
mixing loop, and lets a headless renderer drive the same code with no device.

At a loop wrap: all notes off, `opl2.silence_voices` (`opl2.fc:787`), reset
every channel's controllers, rewind to event 0 and re-establish the initial
tempo. A wrap that leaves stale controller state is the classic way a looped
MIDI gets quieter each time round.

---

## 7. Phase 5 — a headless renderer (recommended)

`demos/shared/tools/midi_render.fc` — reads a `.mid` and a bank, renders to a
WAV file, exits. No device, no window, no game.

Perhaps 120 lines, and it earns them immediately: tuning forty-four patches by
ear through a game launch is miserable, and through a five-second command it is
pleasant. It also exercises parse → sequence → chip end to end without a
device, which is the only "test" this project realistically gets, since the
compiler suite deliberately does not cover `demos/`.

---

## 8. Phase 6 — narrowing `opl_audio.fc`

Removed: `struct song` (`opl_audio.fc:196`), `music_tick` (`:418`), the
`fnum_of` table (`:249`), the melody-length check in `init` (`:379`) and the
melody/chord/arp fields of `struct audio`. About 130 lines.

Changed: `audio.tune: song` becomes `audio.music: opl_midi.player*`; `fill`
(`:533`) calls `opl_midi.advance` where it called `music_tick`; `sync_music`
(`:408`) stops the player rather than only silencing the chip.

Unchanged, and this is most of the file: the SPSC ring, the command enum and
`apply`/`drain`, the six-voice effect engine, `soft_clip`, `render`, the
threading contract and `shutdown_note`. The game-thread API — `play`,
`music_begin`, `music_end`, `music_hold`, `music_unhold`,
`music_toggle_mute`, `music_muted` — keeps its exact signatures, so `game.fc`
and `main.fc` need no changes on that axis.

`init` grows a `music: opl_midi.player*` parameter and loses `tune: song`.

---

## 9. Phase 7 — fuzzel-fobble ships an external `.mid`

The demo loads a real file from disk, so the capability is visible and users
can drop in their own.

**The file.** `demos/fuzzel-fobble/music/minuet.mid`, ours, checked in.

The existing nursery tune is *not* carried over. A demo whose whole point is a
General MIDI player should play something multi-timbral, so the new music is an
arrangement of **Bach's Minuet in G major, BWV Anh. 114** across five GM
channels:

| Ch | Program | Part |
| --- | --- | --- |
| 0 | 11 Music Box | Melody |
| 1 | 9 Celesta | Inner counter-line |
| 2 | 46 Pizzicato Strings | Bass |
| 3 | 7 Harpsichord | Chordal fill on the repeats |
| 9 | — | Light percussion: closed hat and a soft kick |

Four to six sounding voices, which fits the nine-voice budget with headroom and
still exercises program changes, per-channel volume, velocity, and the
percussion path — the whole feature set, without stealing dominating the sound.

Why this piece: it is short (32 bars, two repeated sections, ~1:10 at 120 bpm),
loops cleanly at the double bar, is instantly recognisable, and its light
two-and-three-voice texture is what OPL2 does best. Its cheerful, toy-like
character also carries over the nursery feel the demo already had, which is the
one thing worth keeping from the old tune.

An attribution note belongs in the README: the Minuet in G is from the
*Notebook for Anna Magdalena Bach* and is now generally attributed to Christian
Petzold rather than Bach himself. Public domain either way — this is a
footnote about credit, not about rights.

**Authoring it.** `demos/fuzzel-fobble/tools/mkmid.fc` holds the arrangement as
readable note tables and emits the SMF; both the tool and the `.mid` are
checked in. Keeping the source of the music in the repo means the tune stays
editable, the provenance is unambiguous, and the SMF writer exercises the same
byte layout the parser reads — a round-trip check that costs nothing extra.

**Finding it.** The run scripts `cd` to the repo root before building
(`run-sdl2.sh:11`), so the default path is
`demos/fuzzel-fobble/music/fuzzel-fobble.mid` relative to cwd. `main.fc`
already takes `args: str[]`, so add `--music <path>` and have the run scripts
pass the path explicitly — then it works from any cwd, and swapping in another
file is a command-line argument rather than a rebuild.

**Failing well.** A missing or unparseable file prints the error and starts the
game with music silent; effects still work, since they are on the other chip
and need no MIDI at all. This matters precisely because the point is for people
to swap files in — the first one somebody tries will be a format-2 file or a
truncated download, and the game must survive it with a message that says which.

**`sound.fc`.** Loses the `song` literal (~60 lines of melody/chords/roots/arps),
keeps all nine effect instruments and nine effect scripts untouched, and its
three music instruments migrate into `opl_bank_gm.fc` as GM slots (see §4).

**READMEs.** `demos/shared/README.md` gains an `opl_midi` section;
`demos/fuzzel-fobble/README.md` documents `--music` and the bank; both `.rsp`
files gain `../shared/opl_midi.fc` and `../shared/opl_bank_gm.fc`.

---

## 10. Sizing

| Piece | FC lines | Notes |
| --- | --- | --- |
| Phase 0 — `opl2.fc` fixes | ~30 | Three small, independent changes |
| SMF parser + merge | ~320 | VLQ, running status, meta, k-way merge |
| Bank type + loaders | ~150 | GENMIDI first, `.ibk`/`.sbi` after |
| Hand-authored GM bank | ~250 data | **The long pole — ear time, not lines** |
| Voices, channels, mapping | ~260 | Allocator, controllers, TL, block/fnum |
| Player + API | ~180 | Clock, dispatch, loop wrap |
| Headless renderer | ~120 | Optional, pays for itself in Phase 2 |
| `opl_audio` narrowing | −130 | Net removal |
| fuzzel-fobble integration | ~80 | `--music`, error handling, `mkmid` tool |

Around 1100 lines net of new FC, comparable to `opl2.fc` itself (843).

## 11. What this will and will not sound like

**Will:** any format 0 or 1 SMF loads and plays, with tempo changes, running
status, bends, sustain, and per-channel volume tracked correctly. Anything
written in a six-to-nine-note texture with GM-ish instrumentation comes out
recognisable and often genuinely good — this is the sound the AdLib card
actually made.

**Will not:** stereo (chip is mono), more than nine simultaneous notes,
four-operator timbres, or faithful drums. Dense orchestral files will audibly
thin out under constant voice stealing.

Both of the first two limits are **OPL3** limits, and OPL3 is a strict register
superset of OPL2: 18 two-op voices, stereo L/R bits per channel, four-op modes.
Extending `opl2.fc` later is a well-scoped job, and nothing designed here
against nine mono voices would have to be thrown away — the allocator grows a
larger pool and the mixer grows a second sum.

## 12. Decisions

Settled before implementation started; the sections above are written to match.

1. **Bank breadth** — ~44 melodic patches covering all 128 programs by family,
   plus ~15 percussion. §4.
2. **Bank file formats** — GENMIDI/`.op2`, `.ibk` and `.sbi`, behind a
   sniffing `load_bank`. §4.
3. **fuzzel-fobble's tune** — a new multi-timbral arrangement of Bach's Minuet
   in G, not a conversion of the old one. The three existing music instruments
   are kept anyway, as seeds for the GM bank. §9.
4. **The headless renderer** — in. §7.
5. **wolf-fc** — the three Phase 0 fixes are mirrored into its copy of
   `opl2.fc`. §2.

Verification of how any of it *sounds* is the user's, done separately.

---

## 13. What shipped

| File | Lines | State |
| --- | ---: | --- |
| `demos/shared/opl2.fc` | +55 | Phase 0: block/feedback tables, `note_off` fix, `set_pitch`, `channel_idle` |
| `wolf-fc/src/opl2.fc` | +55 | The same three fixes, mirrored; the copies stay code-identical |
| `demos/shared/opl_midi.fc` | 1004 | Parser, bank type, three bank loaders, allocator, player |
| `demos/shared/opl_bank_gm.fc` | 526 | 60 hand-authored voices, two index tables |
| `demos/shared/tools/midi_render.fc` | 194 | Headless `.mid` → WAV renderer |
| `demos/fuzzel-fobble/tools/mkmid.fc` | 421 | The Minuet arrangement, and the SMF writer for it |
| `demos/fuzzel-fobble/music/minuet.mid` | 2829 B | Generated by the above, checked in |
| `demos/shared/opl_audio.fc` | −106 | Narrowed: the bespoke sequencer out, the player in |
| `demos/fuzzel-fobble/sound.fc` | −81 | Effects only; the tune and its three instruments left |
| `demos/fuzzel-fobble/main.fc` | +55 | `--music`, load, parse, and the failure paths |

Two departures from the plan as drafted, both found during implementation:

- **`patch.fixed_note` uses 0 as its "pitched" sentinel, not −1.** That makes a
  zeroed `patch` a valid melodic one, which is what lets `opl_bank_gm` omit
  `transpose` and `fixed_note` from all 44 melodic entries. MIDI note 0 is a C
  five octaves below the piano and no percussion map names it.
- **`smf.truncated` was added to the error group.** A file whose chunk header
  declares more bytes than the file holds was originally parsed as far as it
  went, which meant a truncated download played two bars and stopped with no
  explanation. It is now a named error, which matters precisely because people
  will be dropping their own files in.

### Verification

Listening is the user's. What was checked mechanically:

- **Every demo and both fuzzel-fobble backends compile**, and the SDL2 build
  links. `make check` is green at 2362 passed / 0 failed on gcc and clang — no
  compiler source was touched, so this is a regression guard rather than a
  result.
- **`wolf-fc` builds** with the mirrored `opl2.fc`.
- **The generated `.mid` validates against an independent parser** (a Python
  SMF reader written for the purpose): format 1, 6 tracks, 677 events, every
  chunk consumed exactly, byte count matching the file.
- **Pitch is right to within 8 cents.** Autocorrelation on the render puts
  bar 1 beat 1 at 588.0 Hz against D5's 587.33, and bar 1 beat 2 at 393.8
  against G4's 392.00 — inside the F-number's own quantisation.
- **The arrangement's two passes read as scored**: RMS 1800 → 2300 and peak
  8800 → 22900 between the thin first half and the full second.
- **A 17-track torture file** — 16 channels, running status, pitch bends over
  the full range, RPN bend-range changes, sustain pedal, three tempo changes,
  sysex in both forms, poly and channel aftertouch, out-of-map percussion
  notes — parses to 2668 merged events and renders with 711 voice steals and
  no clipping.
- **SMPTE timing is exact**: a 25 fps × 40 ticks-per-frame file with notes
  every 250 ticks renders onsets 0.250 s apart, ending at 10.00 s.
- **Looping is clean**: 120 s of a 48 s song gives identical RMS per pass
  (2082.8 both) and no discontinuity at the wrap. The passes are not
  bit-identical, and should not be — the chip's tremolo and vibrato LFOs are
  global and free-running, as the hardware's were.
- **Every failure path reports which failure it was**: format 2, truncated,
  not-a-MIDI-file, and unreadable path, both through the renderer and through
  the game.
- **The retrigger path holds**: 20 note-ons of one note with no note-offs
  between them use one voice and steal nothing.

---

## 14. After the first listen

The user's listening pass raised two things, and both turned out to be real.
Neither was a defect in `opl_midi` or `opl_audio`; both were consequences of
the work landing, and both were fixed where they belonged.

### 14a. The effects had got harsh

The effect instruments were carried over from the bespoke engine byte for
byte, so the change had to be the chip — and it was. `note_off` used to write
a bare 0 to 0xB0, which zeroes the F-number as well as the key bit, and a
zero F-number freezes the phase generator: the release decayed as DC instead
of ringing at pitch. Nine effects had therefore been authored against a chip
that silenced their tails, and four of them were carrying release rates
nobody had ever heard. Measured, one effect at a time, as time-to-0.1%-of-
full-scale:

| Effect | Script | Old (frozen) | After the fix | Retuned | Comment claims |
| --- | ---: | ---: | ---: | ---: | --- |
| `shoot` | 35 ms | 173 | 135 | 135 | "gone in 150 ms" |
| `stick` | 35 ms | 382 | 319 | 176 | "a soft rubbery thock" |
| `pop` | 35 ms | 385 | 253 | 144 | "a short blip" |
| `drop` | 64 ms | 788 | 559 | 311 | "so a glissando doesn't machine-gun" |
| `descend` | 100 ms | 1645 | 1357 | 414 | "grinds in rather than hits" |

The other four — `bounce`, `star`, `clear`, `over` — already matched their
comments and are untouched; `star` and `over` ring for half a second and a
second and a third *on purpose*.

Length was not the only symptom. On the loudest 93 ms window, the share of
energy above 2 kHz went 63% → 77% for `shoot` and 63% → 84% for `pop`: the
frozen tail had been sitting under the attack as a low thump, and removing it
let the bright pitched ring dominate. That is the "harsh". The fix is four
instruments, one nibble each, in `demos/fuzzel-fobble/sound.fc`.

`wolf-fc` reached the opposite conclusion for the opposite reason and is also
right: its 87 effects are Wolfenstein's own data, authored against AdLib's
bare-0 driver and not retunable, so it keeps a local key-off that writes the
zero. Sound of one's own gets retuned; sound that arrives as a fixed format
gets the old stream back. The comment on `opl2.note_off` said "every AdLib-era
driver kept a shadow of this register" — that was wrong, AdLib's driver did
write a bare 0, and the comment now says so in both copies.

### 14b. The music was melody and nothing else

Also real, also measurable, and not a mixing bug — the arrangement was thin.
Rendering each channel alone and measuring RMS and duty cycle (the share of
10 ms blocks above 1% of full scale) over the 48 s song:

| Part | Duty | RMS | |
| --- | ---: | ---: | --- |
| Melody | 89% | −24.2 dBFS | |
| Bass (pizzicato) | 7% | −39.6 | two blips a bar, ~100 ms each |
| Chords (harpsichord) | 2% | −45.3 | one stab a bar, second half only |
| Inner line (celesta) | 1% | −52.0 | one note a bar, decays in 0.5 s |
| Drums | 2% | −43.2 | |

The accompaniment sounded for a fiftieth of the time at a twentieth of the
level. There was nothing to hear but the tune, exactly as reported.

The rewrite, in `tools/mkmid.fc`:

- **The whole minuet, not half of it.** Part II (bars 17–32) is now here,
  melody and bass, from the score. With both halves' repeats taken that is
  four sections, sixty-four bars, ninety-six seconds.
- **A sustained harmony part** on GM 48, whose EGT bit is set, holding a
  two-note chord for each whole bar. This is the change that matters: it fills
  time instead of decaying out of the way.
- **The score's real left hand** instead of a root-and-fifth reduction — it
  walks, so it sounds three-quarters of the time rather than a fifteenth.
- **Harpsichord figuration on the repeats**, four eighths across beats two and
  three, one voice at a time.
- **CC7 per channel**, so the balance is a property of the file.

| Part | Duty | RMS | |
| --- | ---: | ---: | --- |
| Melody | 90% | −23.2 dBFS | |
| Harmony (strings) | 78% | −26.1 | |
| Bass | 77% | −28.9 | |
| Harpsichord | 6% | −44.6 | repeats only, by design |
| Drums | 3% | −42.2 | |
| **Sum** | **99%** | **−20.4** | peak 15224, 0 clipped, **0 steals** |

`opl_bank_gm`'s acoustic bass gave up its carrier TL of 11 for 7. Eleven was
right for a voice that played alone in the old engine; against a melody at TL
2 it read as missing rather than as quiet.

### Verification of this round

- **1594 events, 6442 bytes, format 1, 6 tracks** — validated against the
  independent Python SMF reader: every note closed, every EOT exactly at its
  chunk end, all bytes consumed.
- **Peak six simultaneous notes** of nine, and the renderer reports **0 voice
  steals** over the full 96 s.
- **The loop seam is inaudible**: over 190 s the largest sample-to-sample step
  at the 96 s wrap is 3370, against 3408 for a typical busy bar elsewhere.
  Both passes measure the same RMS (3112.5 / 3118.6).
- **The sum has more room than before, not less**: RMS −23.9 → −20.4 dBFS
  while peak fell 22968 → 15224, so the effects land on top of a fuller mix
  with a better crest factor.
- Every demo and both fuzzel-fobble backends compile, the SDL2 build links,
  and `wolf-fc` builds with the mirrored comment.

### 14c. The harpsichord was inaudible

Reported after the second listen, and the most interesting of the three
because the obvious diagnosis was wrong twice.

Removing the harpsichord track from the mix changed it by **0.07 dB**. The
first guess was masking by register — the figuration was derived from
`pad_note()`, so it played the exact pitches the sustained strings were
already holding. That guess was testable and false: moving it an octave up
made it *worse*, because sources add in quadrature and a broadband energy
metric cannot see a critical band. The second guess was level, and its own
arithmetic refuted it — the patch's carrier already sat at TL 0, so velocity
and CC7 together had barely six decibels left to give.

A per-band comparison against everything else playing found the real answer:
the part was 13 to 27 dB under its masker **in every band**, not just in the
one it shared. Almost all of that was the envelope. The bank's harpsichord
had SL 7 — a 21 dB drop inside the first tenth of a second — so a 0.22 s
eighth note was nearly all attack and no note, and its average energy was
tiny however loud the attack was.

Three changes, none of them large:

| | |
| --- | --- |
| `opl_bank_gm` harpsichord | SL 7 → 2 and a slower decay, on both operators |
| `mkmid` figuration | octave-doubled — one more voice out of nine, +3 dB that velocity could not buy |
| `mkmid` harmony | CC7 70 → 56 for the length of each repeat, so the masker steps back rather than the figuration shouting |

The last one is why the mix did not get peaky doing this. Raising the
harpsichord alone reached the same audibility at a peak of 24846 against
15224 before; making room for it instead lands at 20890, of which only five
samples in ninety-six seconds exceed 20000.

Calibrating each part against the rest of the mix in its own strongest band —
the same measurement for all five, so the parts are comparable:

| Part | Band | Before | After |
| --- | --- | ---: | ---: |
| Melody | 2–4 kHz | +26.0 dB | +13.4 dB |
| Bass | 125–250 Hz | +3.0 | +3.8 |
| Harmony | 250–500 Hz | +1.8 | −1.3 |
| **Harpsichord** | **500 Hz–1 kHz** | **−14.9** | **−6.1** |
| Drums | 125–250 Hz | −9.5 | −9.1 |

The melody's drop from +26 to +13 is relative, not absolute: its own level is
unchanged to a tenth of a decibel, and what fell is its share of 2–4 kHz now
that something else lives there. Absolute levels moved only where intended —
melody, bass and drums identical, harmony −1.1 dB from the duck, harpsichord
+12.5 dB.

Verified as before: 1813 events and 7105 bytes validate against the
independent SMF reader with every note closed, peak seven simultaneous notes
of nine, **0 voice steals**, and the loop seam still joins on a step of 22
against 3408 for a typical busy bar. (An earlier ±50 ms window reported 5296
and looked like a regression; the step was an ordinary note attack 39 ms
before the wrap, not the seam.)

---

## 15. Calibrating the bank

Prompted by "I can't hear the drums" and a request to generalise whatever the
harpsichord taught. Both turned out to be the same defect at bank scale, so the
work was to measure all sixty patches rather than to guess at a few.

`demos/shared/tools/bank_probe.fc` (new, checked in) plays every patch under
identical conditions and reports **loudness over the patch's own sounding
span** — not a fixed window, which would rate a 60 ms hi-hat far below a held
string purely for being short — and **length**, separately, because that is a
different property. It takes an optional bank file, so a downloaded `.op2` can
be measured against the built-in one.

### Two rules, and one thing that is nobody's fault

**SL is a level, not a rate.** On a voice with EGT clear the envelope runs
attack → decay-to-SL → release, so SL is how far the note falls *before* the
release rate gets a say, and it falls that far as fast as DR allows. SL 7
throws away 21 dB in the first tenth of a second; SL 11 throws away 33 dB in
ten. What is left is a high peak with nothing behind it — a patch that reads
loud on a peak meter and vanishes in a mix. Twenty-five patches had it. The
fix is the same every time: SL 0, and put the length in RR, which leaves a
plain exponential decay. A high SL is not wrong by itself — the piano is SL 5
and sounds for nearly three seconds, because its DR is slow enough that the
two-stage fall reads as hammer decay. It hurts only when the drop is both far
and fast, which is why the probe leads with the measurement and mentions SL
second.

**Patches must be level-matched, because velocity cannot do it for them.** The
melodic voices spanned **23 dB** at the same velocity, so a program change was
also a volume change and no arbitrary file could sound balanced through one.
Calibrated to a common target (carrier TL for FM, both operators for additive,
matching what `apply_level` does so timbre survives):

| | before | after |
| --- | ---: | ---: |
| Melodic, middle 80% | 10.6 dB | **4.3 dB** |
| Melodic, full range | 23.2 dB | 12.9 dB |
| Drums, median | −27.9 dBFS | −26.9 dBFS |
| Drums, full range | 13.1 dB | 9.3 dB |

**And the part that is inherent.** The noise voices — snare, hats, cymbals,
claps — stay 8–12 dB under everything else and cannot be brought up. They make
their noise with a large inharmonic MULT on a pulse-sine with feedback, which
spreads their energy thinly across the whole spectrum, and their carriers are
already at TL 0. Three ways out were tried and measured before concluding
that: raising modulation depth buys 2 dB across its entire range, moving them
down four octaves buys 4 dB, and neither is worth the character it spends. The
OPL2 has no noise generator; this is the bill. The probe reports these as
residual outliers rather than pretending otherwise.

Two hypotheses were tested and killed on the way, both worth recording because
both were plausible: that the drums were *out of band* (their centroid is
10–11 kHz, but dropping them four octaves barely moved it — pulse-sine plus
feedback spreads harmonics to Nyquist regardless of fundamental), and that they
were *undersampled* (a MULT of 15 on `fixed_note` 84 runs at 15.7 kHz, 2.8
samples per cycle — but four octaves down bought only 4 dB).

### The drums in this file

Separately from the bank, `mkmid.fc` was writing the hi-hat at **velocity 44**.
On `opl_midi`'s velocity curve that is 12.75 dB of attenuation, applied to the
quietest voice in the bank. A part that should sit low in the mix is written
low in the *mix*, which is what CC7 is for. Velocities are now 80–96 and the
channel volumes were re-derived, since recalibrating the bank moved every
patch under them.

Balanced back to the mix already signed off, with the drums where they should
have been all along:

| Part | signed-off | now |
| --- | ---: | ---: |
| Melody | −23.2 dBFS | −23.2 |
| Harmony | −27.2 | −27.2 |
| Bass | −28.9 | −28.9 |
| Harpsichord | −32.1 | −33.5 |
| **Drums** | **−42.2** | **−34.9** |
| Sum | −20.4 | −20.3 |

Against the rest of the mix in their own strongest band, the drums go from
**−9.5 dB to −0.8 dB**.

### Verification

- 1812 events, 7105 bytes, validated against the independent SMF reader: every
  note closed, every EOT at its chunk end, all bytes consumed.
- Peak seven simultaneous notes of nine, **0 voice steals**, peak sample 21028
  with 0 clipped.
- Loop seam joins on a step of 22 against 7240 for a typical busy bar.
- Every demo and both fuzzel-fobble backends compile, SDL2 links, `make check`
  green at 2362 passed / 0 failed / 4 skipped on gcc and clang.

---

## 16. Rhythm mode

The drums were still hard to hear at low volume, which turned out to be the
useful detail. Two findings, one of which corrected §15.

**Why "at low volume" was the clue.** Each part's energy, flat against
A-weighted (roughly the ear near 40 phon):

| Part | flat | A-weighted | loss |
| --- | ---: | ---: | ---: |
| Melody | 128.8 | 129.2 | **+0.4 dB** |
| Harpsichord | 118.3 | 118.2 | −0.1 |
| Bass | 122.9 | 118.4 | −4.4 |
| Harmony | 120.9 | 114.6 | −6.3 |
| **Drums** | 119.8 | 111.5 | **−8.3 dB** |

A kit's energy sits at both extremes — kick at 60–100 Hz, noise voices at
10–11 kHz — which is exactly where the ear's sensitivity collapses first as
level drops. The music box lives at 2–4 kHz and loses nothing. So the drums
were ~9 dB further under the melody than the flat numbers said, and no amount
of TL was going to fix a *spectrum* problem.

**§15 was wrong about the noise voices.** It said the OPL2 has no noise
generator and the 8–12 dB deficit was inherent. It has one: **rhythm mode**,
register 0xBD bit 5, which `opl2.fc` had never implemented — its own header
said so ("not implemented — unused by Wolf3D"). It was inherent to the subset
of the chip we emulated, not to the chip.

### What was built

`opl2.fc` gains rhythm mode: a 23-bit noise LFSR, the five percussion voices
on channels 6–8, and the phase generation for the three noise-driven ones —
the snare, cymbal and hi-hat do not read their own phase accumulator to make
sound at all, but fold a handful of bits from two others together with the
noise bit. The hi-hat reads the cymbal's bits from the *previous* sample,
because the hardware generates it first, and that ordering is audible enough
to keep. Public API is `rhythm_mode`, `rhythm_strike`, `rhythm_release`,
`load_rhythm_op`.

`opl_midi.fc` routes percussion to it. `bank` gains `rhythm` (the voices) and
`rhythm_map` (note → voice, −1 to fall through), and the player decides once at
`init` from the song and the bank together — no knob, because there is no
interesting way to answer it wrong.

**The mapping is a hybrid, and that is the design.** Rhythm mode does the core
kit very well and everything else not at all: one hi-hat, no cowbell, conga,
tambourine or woodblock. So `rhythm_map` names only what it does well and
every other percussion note keeps its melodic voice. Two entries may share one
operator with different envelopes, which is how the open and closed hi-hats
differ on a chip with one hi-hat — the alternative, leaving the open hat
melodic, left it 9 dB under the rest of the kit with nowhere to go.

Measured, struck through the real player path:

| | flatness (1.0 = white) | length | level |
| --- | ---: | ---: | ---: |
| Snare, faked on a melodic voice | 0.49 | 111 ms | — |
| **Snare, rhythm mode** | **0.81** | 111 ms | −29.5 dBFS |
| Hi-hat, faked | 0.49 | — | — |
| **Hi-hat, rhythm mode** | **0.74** | 55 ms | −29.5 |
| Open hi-hat (same operator) | 0.75 | 443 ms | −29.5 |
| Toms, three pitches | 0.02 | 330 ms | −29.5 |
| Crash | 0.37 | 996 ms | −29.2 |

The whole kit spans **0.4 dB** across its six voices — `bank_probe` now probes
the rhythm section too, under the same conditions as a melodic patch, so the
two are directly comparable.

**It costs three melodic channels and immediately pays for them.** A beat of
kick and hi-hat used to occupy two of nine voices and now occupies none of
six. The minuet renders with **0 voice steals** on six voices, as it did on
nine.

### The result

Drums against melody at a quiet listening level:

| | |
| --- | ---: |
| Faked kit, hi-hat at velocity 44 | ≈ −30 dB |
| Faked kit, relevelled (§15) | −22.5 |
| **Rhythm mode + velocities 104–120** | **−15.0** |

The harmony sits at −14.6 dB by the same measure, so the drums are now level
with a part that reads clearly.

### Verification

- Rhythm mode engages only when it should: **on** (6 melodic) for the minuet,
  **off** (9 melodic) with the drum track removed, **off** for a bank loaded
  from a file, which has no rhythm section. `midi_render` reports which.
- 1812 events / 7105 bytes validate against the independent SMF reader, peak
  seven simultaneous notes, **0 steals**, loop seam joins on a step of 22.
- Every demo compiles, SDL2 links, `make check` green at 2362 / 0 / 4 skipped.
- `wolf-fc` is deliberately **untouched** pending confirmation.

### Found on the way, not fixed

**The bank file loaders decode the wrong field order.** `patch_at` reads five
consecutive bytes per operator, but neither format is laid out that way: IBK
and SBI interleave the two operators (mod, car, mod, car, …, stride 2) while
GENMIDI groups six bytes per operator in the order char/attack/sustain/
waveform/KSL/level. The per-format *offsets* passed to `patch_at` are right;
the field order inside it matches neither. A synthetic `.ibk` renders silent
because the misalignment lands 0x00 in the carrier's attack-rate nibble.
Confirmed pre-existing — the same file is silent at `HEAD` — and untouched
here. Fixing it wants a real `.op2` and `.ibk` to validate against, since
guessing at layouts is what produced the bug.
