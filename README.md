# feishin-visualizer

Standalone MilkDrop visualizer for [Feishin](https://github.com/jeffvli/feishin), built on
[libprojectM](https://github.com/projectM-visualizer/projectm).

Status: prototype. Audio currently comes from WASAPI loopback (whatever the default Windows output
device plays, following device changes). Receiving audio directly from Feishin is planned.

## Download (Windows)

GitHub > Actions > latest `build` run > artifact `feishin-visualizer-windows`. Unzip and start
`feishin-visualizer.exe`. Presets (Cream of the Crop) and textures are included.

## Keys

| Key | Action |
| --- | --- |
| Right / N | next preset (soft transition) |
| Left / P | previous preset |
| H | next preset (hard cut) |
| L | lock / unlock current preset |
| F / F11 / double click | fullscreen |
| Esc | leave fullscreen, then quit |

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
