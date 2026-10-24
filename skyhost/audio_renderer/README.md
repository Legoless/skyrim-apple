# Native audio renderer

This module runs the vendored Ryujinx Audio Renderer DSP in a macOS arm64
NativeAOT library. It decodes voice buffers, applies mixes/effects, downmixes
surround to stereo, and returns real pool/voice statuses. Its host thread signals
the guest frame event after processing the DSP command list.

`audio.c` retains the original SDK config serialization and response parsing.
The inspected SDK service call at vtable offset `0x40` supplies output,
performance, and input spans to the bridge. The public renderer handle is a
16-byte value, so the config argument follows it in AArch64 register `x2`.

`audio_backend.c` sends the resulting PCM16 stereo samples through a bounded
SPSC ring to the default CoreAudio output device. Opening the game renderer
fails explicitly if either the native renderer or CoreAudio cannot initialize.
There is no silent or clock-only production fallback.

Build with `make -C skyhost` using .NET 10 with NativeAOT support. Generated
`bin`, `obj`, and `build/audio-renderer` files are ignored. Source provenance and
the upstream MIT license are recorded in `vendor/`.

`python3 skyhost/tests/test_audio_renderer.py -v` checks PCM16 and GC-ADPCM,
surround center dialogue downmixing, attached-pool and consumed-wave feedback,
and rejection of malformed renderer sections. `t_audio` checks the native
device lifecycle, guest handle ABI, and host events.

`AUDIO_PCM_NONZERO` proves DSP generation. `AUDIO_COREAUDIO_NONZERO` separately
proves a CoreAudio callback consumed nonzero samples. Neither marker by itself
proves that the game reached its menu or intro.
