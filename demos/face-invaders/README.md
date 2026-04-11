# face-invaders

A Space Invaders clone written in FC using SDL2 — fullscreen-desktop with
high-DPI awareness, accelerated vsync rendering, a parallax starfield,
particle explosions, and pixel-art face-themed enemies.

## Usage

```
./demos/face-invaders/run.sh
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
| Arrow keys / A,D | Move left/right |
| SPACE | Start / fire / restart |
| P | Pause |
| ESC | Quit |
| C (splash only) | Clear high score |

## Rules

- Shoot the descending alien formation before it reaches your line.
- Enemies accelerate as more of them die, and each wave gets faster.
- Top row (boss): 30 pts. Middle rows (soldiers): 20 pts. Bottom rows (grunts): 10 pts.
- You start with three lives. Lose one when an enemy bullet hits you.
- High score persists to `~/.face-invaders/highscore.txt`.

## Display

The game opens a **fullscreen-desktop** window with
`SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI`, then uses
`SDL_RenderSetLogicalSize(1280, 720)` so all drawing happens in a fixed
logical coordinate space. SDL scales that logical resolution to the
physical backing pixels at blit time, which is how the game stays crisp
on high-DPI displays without any per-platform scaling math in the game
code itself. The `SDL_RENDER_SCALE_QUALITY=linear` hint gives smooth
upscaling.

## FC features demonstrated

- **Fullscreen + high-DPI SDL2 setup**: `SDL_WINDOW_FULLSCREEN_DESKTOP`,
  `SDL_WINDOW_ALLOW_HIGHDPI`, `SDL_RenderSetLogicalSize`, accelerated
  renderer with vsync, blend-mode alpha.
- **Rich file-level initializers**: heap buffers (`alloc(enemy[55] {})!`,
  `alloc(bullet[64] {})!`, `alloc(game_state)!`, etc.) are allocated
  *at file scope* — a feature that lets the whole game share mutable
  state without threading pointers through every function.
- **Union types for game phases**: `splash`, `playing`, `paused`,
  `wave_clear`, `dying`, `game_over`, dispatched with `match`.
- **Extern structs and functions** for SDL2 C interop in `sdl.fc`.
- **Procedural audio**: square-wave sound effects generated at startup
  with envelopes, queued via SDL audio.
- **Pixel-art sprites**: three enemy types encoded as row bitmaps with
  two animation frames each, rendered into scaled rectangles.
- **Particle system**: gravity-affected explosion bursts with alpha
  fade-out.
- **Parallax starfield**: three speed tiers of twinkling stars.
- **Bitmap font**: 3x5 glyphs scaled up for HUD and overlay text.
- **String interpolation**: HUD text via `"%d{game->score}"` etc.
- **Stdlib modules**: `std::io`, `std::text`, `std::sys`, `std::math`,
  `std::random` (PCG generator).
