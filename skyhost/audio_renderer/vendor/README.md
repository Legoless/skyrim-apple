Ryujinx Audio Renderer source subset

Source: https://github.com/jamesoram/Ryujinx
Commit: 7d158acc3b5826a08941d6e8d50d3a3897021bcd
License: MIT, see LICENSE.txt.

The Renderer sources have one mechanical change: AudioRenderSystem is declared
partial so HostAdapters.cs can release memory-pool usage after synchronous host
DSP completion under the original renderer lock. The host project excludes the original
AudioRendererManager, AudioProcessor, and virtual-device management and supplies synchronous host adapters.
NativeMemoryManager and IRefCounted are from the same revision.
