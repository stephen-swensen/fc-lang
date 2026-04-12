# Wolfenstein FC

A Wolfenstein 3D engine written in FC using SDL2. Software-rendered raycaster
with textured walls, procedural audio, and a minimap.

## Running

```bash
demos/wolf-fc/run.sh
```

Requires SDL2 development libraries installed on your system.

## Controls

| Key | Action |
|---|---|
| Up / W | Move forward |
| Down / S | Move backward |
| Left / A | Turn left |
| Right / D | Turn right |
| Alt + Left/Right | Strafe |
| Space | Run (hold) / Use / Open door |
| Left Shift | Shoot |
| Tab | Toggle minimap |
| F11 | Toggle fullscreen |
| Escape | Quit |

## Asset Loading (future)

To use original Wolfenstein 3D textures, sprites, and sounds, place the
following files from your legitimate copy of the game into `data/`:

- `VSWAP.WL6` — wall textures, sprites, digitized sounds
- `MAPHEAD.WL6` — level offset table
- `GAMEMAPS.WL6` — level data
- `VGAHEAD.WL6`, `VGADICT.WL6`, `VGAGRAPH.WL6` — HUD graphics, fonts
- `AUDIOHED.WL6`, `AUDIOT.WL6` — audio data

These files are copyrighted by id Software and are not included in this
repository. The shareware data files (`.WL1`) also work for the first episode.

## Credits

- Engine written from scratch in FC — no code from id Software's GPL release
- Original game by id Software (1992). Data file formats documented by the
  Wolfenstein 3D modding community.
