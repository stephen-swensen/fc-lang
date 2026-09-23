# Foregon Frail

The journey to Oregon, 1847, played the way it was first played: at a
teletype. You outfit a wagon with $700, then cross 2040 miles one fortnight
at a time, answering the printer's questions — hunt or press on, how well to
eat, what to do about the riders on the ridge — and when it comes to
shooting, the printer types a word and the rifle is only as quick as you are
at typing it back.

Written in FC, on raylib, with an emulated OPL2 (YM3812) for the sound: the
print head's clatter, the bell, the rifle, a dirge and a fanfare.

```
./demos/foregon-frail/run.sh        # fetches and builds raylib on first run
```

raylib is fetched and built into `demos/shared/raylib/` on first run
(gitignored, shared with the other raylib demos). You need what raylib links
against: OpenGL and X11 dev packages on Linux, nothing extra on MSYS2, the
Xcode command line tools on macOS.

## Licence, and what this is after

**This directory is public domain** under the Unlicense (`UNLICENSE`, next to
this file). That's a deliberate exception to the repository's BSD 2-Clause
licence, and it covers everything in `demos/foregon-frail/`. The shared
engine code it builds with (`demos/shared/`, the stdlib) keeps the
repository's licence.

**It is a new program, not a port.** The game it follows was written in 1971
by Bill Heinemann and Paul Dillenberger with Don Rawitsch, rewritten by
Rawitsch at MECC, and its BASIC listing was published in *Creative
Computing* in 1978. The listing's text and code belong to their authors. So
this game takes from it only what can be taken: the **rules and numbers**. The
$700 outfit and the $200–$300 team, forts every other turn at a one-third
markup, the three rations, the riders and their four tactics, the table of
fifteen misfortunes and their odds, South Pass and the Blue Mountains, the
illness roll, and the typed gunshot scored against how good a shot you
claimed to be all follow the listing number for number, down to its quirks
(a wagon knocked back below South Pass reports exactly 950 miles for a
turn).

**Every sentence the printer types is this game's own**, and so are the
pictures, the sounds and the code. What overlaps with the listing is the
vocabulary a trip like this can't avoid: the names of the supplies, the
landmarks, the dates, and `TYPE BANG` (the rifle's other three words are
new). The name is the demo series' usual first-letter swap, and it keeps
clear of anyone's trademark.

Two changes from the listing, on purpose:

- **The calendar is computed.** The listing's turn dates are a table with one
  slip in it (an August date a day late); here every turn is exactly two
  weeks after the last, starting Monday, March 29, 1847.
- **A negative purchase at a fort buys nothing.** In the listing it silently
  shrank the item without refunding a cent.

The music in the effects is public domain too: the opening of Chopin's
funeral march (1839) and the first line of Stephen Foster's "Oh! Susanna"
(1848, strictly a year too late for this trip).

## Playing

Type your answer and press RETURN. The printer runs at four and a half
times a real Model 33's ten characters a second; **any key** while it's
printing hurries it along, and a key pressed while it's printing is spent
on that rather than typed. **F11** toggles full screen; closing
the window quits. ESC does nothing, on purpose, because it sits too close to
the keys you type with.

**The rifle.** When the printer types `TYPE BANG` (or one of the other
words), type the word and press RETURN. The clock starts when the word is
fully printed. Your score is the seconds you took, less a head start for
admitting you're not a deadeye: claim to be a **deadeye** (1) and you get no
head start, so you need to be done in about a second; **all thumbs** (5)
gets four seconds' grace. A wrong word always counts as the worst possible
shot. An answer above 5 counts as 0, which is worse than claiming to be a
deadeye, just as it was in 1978. The time is printed after every shot.

**The outfit.** Oxen $200–$300 (a better team covers more ground each
fortnight), then any amounts of food, ammunition (50 rounds a dollar),
clothing and supplies (medicine and spare parts), up to $700 in all. Cash
you keep can be spent at forts, which come every other turn, at two dollars'
worth for every three.

**The screen.** The top is the whole trail as a map, with **west on the
left**. The wagon rolls right to left from Independence to Oregon City, the
sky and grass follow the season, and whoever has come to meet the wagon
stands beside it. The ruler underneath names the landmarks (their mileages
are the historical trail's, approximately; South Pass and the Blue Mountains
sit where the rules put them, at 950 and 1700). Bottom left is the paper
roll; bottom right is the ledger, which tracks the wagon live, so its
numbers can run ahead of the last stock-taking on the paper.

## How it's built

The same layering as fario and fuzzel-fobble, with a teletype where their
game pad was:

| File | What it is |
|------|------------|
| `plat.fc` | the contract: one `input` per frame (typed characters, RETURN, rubout), and what `module gfx` must provide |
| `gfx.fc` | `module gfx` on raylib: window, letterbox, audio device, and text input from raylib's character queue |
| `tty.fc` | the paper roll: word-wrapped lines in one fixed buffer, a print head that runs behind the text, questions answered on their own line, and **cues** (sound effects that fire as the head reaches their line) |
| `game.fc` | the rules, as a state machine: each function prints what happens and either continues or asks a question and returns. `game.answer` takes the reply. No pixels, no keys, no library. |
| `art.fc` | the panorama, the landmark ruler, the paper and the ledger, plus the 5x7 dot font, kept as text (one string of `#`s per character) |
| `sound.fc` | the OPL2 instruments and effect scripts |
| `main.fc` | the loop: keys to the paper, answers to the game, cues to the sound engine |

The 1978 program is one long GOTO listing that stops at each `INPUT`. The
state machine in `game.fc` is that listing turned inside out: every place
the BASIC waits for input is a `phase`, and every `GOTO` after it is a
function call from `answer`. The comments point out where a branch
reproduces something non-obvious in the listing.
