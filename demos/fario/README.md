# Fario

A side-scrolling platformer written in FC, in the spirit of the NES
classics: eight worlds of running, jumping, stomping, block-bumping,
swimming and castle-storming, with a flagpole at the end of most of them and
an axe at the end of the last one. raylib for the window, an emulated OPL2
(YM3812) for every sound, and four original tunes shipped as Standard MIDI
Files.

**On the "in the spirit of" part, plainly:** everything in this demo is
original work. The level layouts, the character and creature designs, the
pixel art, the names and all four pieces of music were made for this demo.
What it borrows from the genre's most famous ancestor is the *design
vocabulary* — question blocks, pipes, powerup progression, a countdown
timer, flagpole finishes, underground/water/castle world types — which is
the platform genre's shared grammar, not anyone's protected expression. No
asset, level, sprite, melody or name is copied from anything.

## Usage

```
./demos/fario/run.sh                # fetches and builds raylib on first run
./demos/fario/run.sh --level 4      # open the title with the picker on world 4
```

raylib is fetched and built into `demos/shared/raylib/` on first run
(gitignored, shared with the other raylib demos — if you have run
fuzzel-fobble's raylib build, this step is already cached). What you need
installed is what raylib links against: OpenGL and X11 dev packages on
Linux, nothing extra on MSYS2, the Xcode command line tools on macOS.

## Controls

The NES pad, on a keyboard:

| Key | Action |
|-----|--------|
| Left / Right | Move |
| CTRL (either) or X | **A** — jump; swim stroke underwater |
| ALT (either) or Z | **B** — run while held; throw fire when armed |
| SPACE | **Select** — on the title, pick a starting world (of those reached) |
| ENTER | **Start** — begin; pause and resume in play |
| M | Mute / unmute the music |
| S then 1-9 | Save the game to that slot |
| L then 1-9 | Load the game in that slot |
| F11 | Fullscreen |
| ESC | Quit |

## The game

- **Eight worlds**, each one screen tall and a few hundred tiles long:
  meadows, an underground, athletic sky-hops, a castle, a pipe gauntlet, an
  underwater world, a night climb, and Gnarl's keep. World types change the
  music, the backdrop, the palette — and underwater, the physics.
- **Blocks**: `?` blocks pay a coin or hold a powerup; bricks break once
  you're grown (bump them from below — anything standing on top is flipped
  off). A hundred coins is a life.
- **Powerups**: a mushroom grows you (a hit shrinks you back instead of
  killing you), a fire flower arms you (B throws bouncing fireballs, two in
  the air at once), a star makes you briefly invincible, a green mushroom
  is a life.
- **Creatures**: grumbles walk and stomp flat; snappers tuck into shells
  you can kick — a sliding shell mows down everything it meets, at rising
  prices; prickles refuse both boots and fire; chompers ambush from pipes
  (they won't come out while you stand at the mouth); firebars spin over
  castle floors; finns and jellies patrol the water, where nothing can be
  stomped.
- **The clock** counts down; at zero it costs a life, and at the flag what
  is left pays 50 a unit. The flagpole pays by height — the top is 5000.
- Castle worlds end at an **axe** instead, and taking it drops the bridge —
  in world 8, with **Gnarl the usurper** still pacing on it. Six fireballs
  also work, if you brought them.
- **Checkpoints**: past each level's midpoint, a lost life resumes there
  rather than at the start.
- **Lives, score, best score.** The best score survives in
  `~/.fario/highscore.txt`, and the furthest world you have ever reached in
  `~/.fario/progress.txt` — that is what the title screen's picker offers.

## Saved games

Nine slots in `~/.fario/save1.txt` through `save9.txt`, on fuzzel-fobble's
two-keystroke scheme: `S` arms a prompt showing which slots hold a game,
the digit writes one; `L` and a digit reads one back. Saving is offered in
play and on pause; loading everywhere, the title included.

The file is text, one fact per line: score, coins, lives, the clock, the
hero mid-stride — and the *whole level as it stands*: fifteen `row` lines of
map characters (bricks broken, blocks spent, coins taken) and an `ent` line
per creature. It is a genuine mid-level save state, and it is diffable,
mailable and hand-editable. Loading is all-or-nothing: a file from another
format version, or one a full disk cut short, is refused whole and says so,
leaving the game you were looking at untouched.

## The levels are text

`levels.fc` holds all eight maps as fifteen strings each, one character per
48x48 tile — `#` ground, `B` brick, `?` a coin block, `(`/`)` a pipe cap,
`g` a grumble, `F` the flagpole, and so on (the legend is at the top of the
file). The maps are the level editor: change a character, rerun, play it.

## Sound

Everything you hear is FM synthesis on emulated OPL2 chips through the
shared engine (`demos/shared/opl_audio.fc`) — two chips, music and effects
apart, mixed on the host's audio thread so a slow frame can never bend the
tune. See fuzzel-fobble's README for the architecture; Fario adds nothing
to it, which is the point of a shared engine.

The music is four original compositions — written for this game, note by
note, in `tools/mkmusic.fc`, which emits the `.mid` files in `music/`:

| File | Piece | Where |
|---|---|---|
| `overworld.mid` | *Sunny Scamper* — C major, 4/4 at 168 | meadows, hills, night |
| `underground.mid` | *Root Cellar* — C minor, 4/4 at 110 | under the roots |
| `water.mid` | *Tidepool Waltz* — F major, 3/4 at 88 | the tidepool |
| `castle.mid` | *Gnarl's Keep* — C minor, 4/4 at 138 | both castles |

The note tables in the tool are where the tunes stay editable as music
rather than as bytes; run it and the files regenerate. The twenty effects —
the jump chirp, the coin ring, the flagpole's long slide, the bridge's
grinding collapse — are instruments and scripts in `sound.fc`.

## Architecture

Fuzzel-fobble's layering, minus the second backend (this demo is raylib
only — no backend flag, the response file's list *is* the selection):

| File | What it is |
|---|---|
| `plat.fc` | The contract: `struct input` as a NES pad, the `gfx` interface as a comment |
| `gfx.fc` | `module gfx` on raylib: window, letterboxed 1280x720 logical frame, keys, audio device |
| `levels.fc` | The eight worlds, as text |
| `game.fc` | The rules: tiles, physics, creatures, clocks, scoring, saves. No pixels, no keys, no library |
| `art.fc` | The look: char-grid pixel sprites, themed tiles, parallax backdrops, HUD, overlays |
| `sound.fc` | The effect instruments and scripts |
| `main.fc` | Loads the music, hands the pieces to each other, and loops |

The sprites are pixel art kept as arrays of strings — one character per
pixel through one shared palette, with character remapping for the suit
variants (fire Fario, the star shimmer, the 1-up's green) — the same
text-as-data move the level maps make.

## FC features demonstrated

- **Enums** (`theme`, `ent_kind`) driving exhaustive `match` dispatch for
  level families and creature behaviour.
- **A world struct threaded explicitly** — every scrap of mutable state
  born in `game.create`, no globals in the game.
- **Slices of string literals as data** — the level maps and the sprite
  sheets, both `const (const str)[]` frozen into `.rodata`.
- **`std::io` + `std::text` on real file formats** — the save files, the
  high score, the progress file, all strict-parsed with `T!` results.
- **The shared OPL2 engine** — lock-free command ring, music player and
  effect chip, exactly as fuzzel-fobble runs it.
- **Extern C interop with raylib** — by-value structs, render textures, an
  audio callback.
