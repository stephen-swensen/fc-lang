# fasteroids

A modern Asteroids clone written in FC using SDL2 — vector-styled polygonal
asteroids with neon glow lines, particle bursts, a thrust-and-rotate ship
with screen wrap, procedural audio, and a scrolling starfield.

## Usage

```
./demos/fasteroids/run.sh
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
| LEFT / A | Rotate left |
| RIGHT / D | Rotate right |
| UP / W | Thrust |
| SPACE | Fire |
| DOWN / H | Hyperspace (random teleport) |
| P | Pause |
| F11 | Toggle fullscreen |
| ENTER | Start / restart |
| ESC | Quit |

## Rules

- Destroy all asteroids to advance to the next wave.
- Large asteroids split into two medium; medium into two small; small are destroyed.
- Scoring: large 20 pts, medium 50 pts, small 100 pts, UFO 200 pts (large) / 1000 pts (small).
- Extra life every 10,000 points.
- You start with three lives. Lose one when you collide with an asteroid or get hit by a UFO bullet.
- High score persists to `~/.fasteroids/highscore.txt`.
- A UFO appears periodically and fires aimed shots at your ship.

## Display

The game opens a resizable, high-DPI aware window that is maximized
immediately at startup, then uses `SDL_RenderSetLogicalSize(1280, 720)`
so all drawing happens in a fixed logical coordinate space. F11 toggles
borderless fullscreen.

## FC features demonstrated

- **Vector graphics**: all game objects rendered with SDL2 line primitives
  and glow effects via multi-pass alpha-blended overdraw.
- **Procedural asteroid generation**: jagged polygons from randomized
  vertex radii around a base radius.
- **Screen-wrap physics**: toroidal topology with distance calculations
  that account for wraparound.
- **Union types for game phases**: `splash`, `playing`, `dying`,
  `game_over`, dispatched with `match`.
- **Extern structs and functions** for SDL2 C interop via shared bindings.
- **Procedural audio**: square-wave sound effects with envelope shaping.
- **Particle system**: alpha-fading explosion bursts.
- **Bitmap font**: 3x5 pixel glyphs for HUD text.
- **Stdlib modules**: `std::io`, `std::text`, `std::sys`, `std::math`,
  `std::random` (PCG generator).
