# feishin-visualizer

Standalone MilkDrop visualizer for [Feishin](https://github.com/jeffvli/feishin), built on
[libprojectM](https://github.com/projectM-visualizer/projectm).

Status: prototype. Audio currently comes from WASAPI loopback (whatever the default Windows output
device plays, following device changes). Receiving audio directly from Feishin is planned.

## Download (Windows)

GitHub > Actions > latest `build` run > artifact `feishin-visualizer-windows`. Unzip and start
`feishin-visualizer.exe`. Presets (Cream of the Crop) and textures are included.

## Keys

Modelled on MilkDrop 2 (see the official MilkDrop 2 documentation by Ryan Geiss).

| Key | Action |
| --- | --- |
| F1 | help |
| Space | next preset (blend) |
| H | next preset (hard cut) |
| Backspace | previous preset (history of 64) |
| R | random / sequential order |
| L | load menu: arrows, PgUp/PgDn, Home/End, type to jump, Enter load, Shift+Enter hard cut, Left/Backspace = parent folder. Loading from a folder makes it the pool for switching. |
| Scroll Lock | lock current preset (keyboard LED on = locked) |
| + / - | rate current preset 0-5 (rating = weight for random picks, 0 = never) |
| A | mash-up: random preset + warp shader of a second + composite shader of a third |
| D | cycle shader lock: warp, composite, both, none (kept when presets change) |
| F4 / F5 / F6 | preset name / fps / rating |
| F8 | type or paste (Ctrl+V) a preset directory |
| Alt+Enter / F11 / double click | fullscreen |
| Esc | close menu, leave fullscreen, quit |

Planned with the Feishin link: F2/F3/T (song title/length), Z X C V B (playback), arrows (volume/seek).

Files next to the `.exe`: `config.ini` (settings), `ratings.ini` (preset ratings), `state.ini` (random mode, current folder).

## Settings

`config.ini` is created next to the `.exe` on first start (preset duration, blend time, hard cuts,
beat sensitivity, mesh size, FPS, shuffle, window). Restart after changes.

## Build

Requires CMake 3.21+, a C++17 compiler and [vcpkg](https://vcpkg.io):

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Known limitations

- WASAPI loopback cannot see audio played in exclusive mode (e.g. Feishin MPV with exclusive
  output enabled) or through ASIO.
- Some presets use shaders projectM cannot translate; they are skipped automatically.
