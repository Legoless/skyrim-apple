using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Ryujinx.Audio.Integration;
using Ryujinx.Audio.Renderer.Dsp.Command;

// Audio executes in the same address space as the guest. These are the memory
// operations used by the upstream renderer; no emulated CPU/page table is needed.
namespace Ryujinx.Memory
{
    public interface IVirtualMemoryManager
    {
        T Read<T>(ulong address) where T : unmanaged;
        void Read(ulong address, Span<byte> data);
        void Write<T>(ulong address, T value) where T : unmanaged;
        void Write(ulong address, ReadOnlySpan<byte> data);
        ReadOnlySpan<byte> GetSpan(ulong address, int size, bool tracked = false);
    }

    public sealed unsafe class HostMemory : IVirtualMemoryManager
    {
        public T Read<T>(ulong address) where T : unmanaged => *(T*)address;
        public void Read(ulong address, Span<byte> data) => new ReadOnlySpan<byte>((void*)address, data.Length).CopyTo(data);
        public void Write<T>(ulong address, T value) where T : unmanaged => *(T*)address = value;
        public void Write(ulong address, ReadOnlySpan<byte> data) => data.CopyTo(new Span<byte>((void*)address, data.Length));
        public ReadOnlySpan<byte> GetSpan(ulong address, int size, bool tracked = false) => new((void*)address, size);
    }
}

namespace Ryujinx.Audio.Renderer.Dsp
{
    // Upstream generates its usual DSP command list. Execute it synchronously
    // on the host audio thread before reporting frame completion to the guest.
    public sealed class AudioProcessor
    {
        public IHardwareDevice Device { get; }
        public IHardwareDevice[] OutputDevices { get; }
        public AudioProcessor(IHardwareDevice device) { Device = device; OutputDevices = new[] { device }; }
        public bool HasRemainingCommands(int sessionId) => false;
        public void Send(int sessionId, CommandList commands, int limit, ulong appletId)
        {
            using (commands) commands.Process(Device);
        }
    }
}

namespace Ryujinx.Audio.Renderer.Server
{
    public partial class AudioRenderSystem
    {
        public void ProcessHostFrame()
        {
            lock (_lock)
            {
                SendCommands();
                // The host processor completes the entire DSP command list
                // synchronously. Release its in-flight pool usage before a
                // woken client can request detach (including after Stop).
                MemoryPool.PoolMapper.ClearUsageState(_memoryPools);
            }
        }
    }

    public sealed class HostTickSource
    {
        private readonly Stopwatch _clock = Stopwatch.StartNew();
        public double ElapsedSeconds => _clock.Elapsed.TotalSeconds;
    }

    public sealed class AudioRendererManager
    {
        public HostTickSource TickSource { get; } = new();
        public Dsp.AudioProcessor Processor { get; }
        public AudioRendererManager(IHardwareDevice device) => Processor = new(device);
        // Host sessions are individually owned by the bridge rather than a
        // shared emulator manager, so disposal has no registration to remove.
        internal void Unregister(AudioRenderSystem renderer) { }
    }
}
