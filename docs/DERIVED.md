# Game-related material in this repository

中文: [DERIVED.zh-CN.md](DERIVED.zh-CN.md)

The current source tree does not contain the Skyrim program, RomFS, keys, saves, captured game shaders, or translated game shaders. Those stay on the machine that owns the cartridge dump.

## License scope

All project-authored code and documentation are covered by the repository's [MIT License](../LICENSE).
Third-party code retains the licenses and notices listed below.
Game-derived material, including private translated shaders and gameplay screenshots, is not covered by the project's MIT license.

## Shared shaders

The renderer's clear/presentation helpers and the synthetic fixtures in `skyhost/tests/fixtures/shaders/` are project-authored. They are covered by the MIT license. The fixtures exercise rendering and resource binding independently of Skyrim shader captures.

The Ryujinx translator also includes upstream GLSL helpers under its existing MIT license.

## Private game shader cache

`skyrim-work/build/shaders/` is ignored by Git. Captured Skyrim programs and their translated `.metal`/`.json` pairs are game-derived; translating them does not make them project-authored shaders. Keep them local. See [the capture workflow](../skyhost/README.md#local-game-shaders) for its current limitations.

Game-derived shader caches, captured test fixtures, and gameplay screenshots must not be committed or included in any published history. This distribution excludes them.

## Vendored code

| Where | What | License |
|---|---|---|
| `skyhost/third_party/ryujinx` | Ryujinx 1.3.2 shader translator (`Ryujinx.Graphics.Shader`, `Spv.Generator`) and `Ryujinx.Common`, used by `skyhost/tools/dmsh_xlate` | MIT, see `LICENSE.txt` |
| `skyhost/audio_renderer/vendor` | Ryujinx audio-renderer DSP subset | MIT, see that directory's license |
| `skyhost/third_party/bcdec` | bcdec 0.985 BC1–BC7 texture decoder; pinned source and small C compatibility fixes are recorded in `UPSTREAM.json` | MIT option, see that directory's `LICENSE` |
| `skyhost/nvn/nvn_tex.c` | Tegra X1 block-linear texture layout, ported from deko3d `dk_image.cpp` | [zlib](../skyhost/nvn/LICENSE.deko3d), Copyright (C) 2018-2020 fincs |

The NVN format table and block-linear deswizzle live next to that texture layout in `skyhost/nvn/`.
