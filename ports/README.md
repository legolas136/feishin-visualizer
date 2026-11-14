# vcpkg overlay ports

`projectm/` is the official vcpkg port of libprojectM 4.1.8 plus
`audio-levels-override.patch`, which adds `projectm_set_audio_levels_override()` so the
host can supply the bass/mid/treb (+ `_att`) values presets react to, and
`shader-binary-cache.patch`, a process-wide cache of linked shader program binaries so presets
warmed up by a background instance switch without compiling. The patched sources
are libprojectM (LGPL-2.1) with this patch applied.
