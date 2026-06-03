# folitaire

A Klondike Solitaire clone written in FC using SDL2 — styled after the
1990s Windows version: green felt table, 71x96-ish white cards with
suit pips arranged in the traditional layout, a blue cross-hatched
card back, and the classic stock / waste / 4 foundation / 7 tableau
column layout. Mouse driven: click to draw, drag to move.

## Usage

```
./demos/folitaire/run.sh
```

The script auto-detects the host OS (Linux or MSYS2/MinGW Windows) and
picks the right SDL2 link line.

Requires SDL2 installed for your environment:

- Linux: `libsdl2-dev` (or your distro's equivalent)
- MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-SDL2`
- MSYS2 MINGW64: `pacman -S mingw-w64-x86_64-SDL2`

## Controls

| Input | Action |
|-------|--------|
| Left click stock | Turn a card to the waste |
| Left click empty stock | Recycle waste back to stock |
| Left drag | Move a card (or stack) between piles |
| Double-click a card | Auto-send to its foundation if possible |
| SPACE | Deal a new game |
| A | Auto-finish (only once all face-down cards are exposed) |
| ESC | Quit |

## Rules

- Build the four **foundations** Ace → King by suit (upper-right slots).
- Build the seven **tableau** columns down by alternating colors
  (red on black, black on red).
- Only Kings can fill an empty tableau column.
- A face-down tableau card flips over when it becomes the column's
  bottom-most card.
- You may move a partial run of face-up tableau cards onto another
  pile, as long as the bottom card of the run is a valid landing on
  the target.
- You win when every foundation holds King-down-to-Ace (52 cards
  total).
- Scoring is simple: +10 for each card sent to a foundation, +5 for
  each face-down tableau card revealed.

## Display

The game uses a fixed-size 1024x720 window. Card art is rendered
entirely from FC code — pixel-art suit pips at multiple scales and
a 3x5 bitmap font scaled for rank corners, face-card letters, and HUD
text. The card back is a blue inset panel with white diagonal
cross-hatch lines. No image assets.

## FC features demonstrated

- **SDL2 mouse handling**: motion, button down/up, double-click
  detection via `SDL_MouseButtonEvent.clicks`.
- **Heap-allocated game state**: a single `alloc(game)!` holding
  multiple `pile` sub-objects, each with its own `alloc(card[24] { })!`
  storage — the same "world struct" pattern as `face-invaders`.
- **Union types for game phase**: `splash`, `playing`, `won`,
  dispatched with `match`.
- **Pattern matching for pip art**: per-rank pip layouts and per-suit
  bitmap rows encoded with binary literals (`0b0011100`).
- **Pixel-art rendering**: suit pips and bitmap font scaled up by
  emitting filled rects per "logical pixel".
- **Procedural audio**: square-wave card-flip / placement / win-fanfare
  buffers queued through SDL audio.
- **String interpolation** for the HUD: `"SCORE: %d{g->score}"`.
- **Stdlib modules**: `std::io`, `std::text`, `std::sys`,
  `std::random` (PCG generator) for the Fisher-Yates shuffle.
