# fibbles

A Snake/Nibbles clone written in FC using SDL2 for graphics and audio.

## Usage

```
./demos/fibbles/run.sh
```

Requires `libsdl2-dev` (or equivalent) installed on the system.

Controls:

| Key | Action |
|-----|--------|
| Arrow keys | Steer the snake |
| SPACE | Start / pause / restart |
| ESC | Quit |

Rules:

- Eat red food: +10 pts, +1 length
- Eat yellow bonus (25% spawn chance): +20 pts, +4 length
- Level up every 100 points
- Speed increases by 3ms per food (max speed 50ms)
- Game over if you hit a wall or yourself
- High score persisted to `~/.fibbles/highscore.txt`

## FC features demonstrated

- **SDL2 C interop**: Binds to SDL2 via `extern` functions, structs, and constants in a separate `sdl.fc` module — window creation, rendering, event polling, and audio queuing.
- **Extern structs**: `rect`, `sdl_event`, and `audio_spec` are declared as extern structs and used directly for SDL API calls.
- **Union types for game state**: `game_state` union with `splash`, `playing`, `paused`, and `over` variants, matched for rendering and update logic.
- **Procedural audio**: Square wave sound effects generated at startup using raw `int16*` buffers and queued via SDL audio.
- **Bitmap font rendering**: 3x5 pixel glyphs packed into `int32` bitmasks, unpacked with bitwise operations for text rendering.
- **File I/O with `std::io`**: High score persistence via `io.open`/`io.read`/`io.write`/`io.close`.
- **String interpolation**: Window title updates and terminal output use `%d{expr}` and `%s{expr}` format specifiers.
- **Stdlib modules**: Uses `std::io` (file I/O), `std::text` (score parsing), and `std::sys` (home directory lookup).
