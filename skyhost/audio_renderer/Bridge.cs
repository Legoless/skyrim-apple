using System;
using System.Buffers;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using Ryujinx.Common.Logging;
using Ryujinx.Common.Logging.Targets;
using Ryujinx.Audio.Integration;
using Ryujinx.Audio.Renderer.Parameter;
using Ryujinx.Audio.Renderer.Server;
using Ryujinx.Memory;

public static unsafe class SkyAudioBridge
{
    [DllImport("libc")] private static extern int dup(int descriptor);
    [DllImport("libc")] private static extern int dup2(int source, int destination);
    [DllImport("libc")] private static extern int close(int descriptor);

    private sealed class HostLogTarget : ILogTarget
    {
        public string Name => "sky-audio-stderr";
        public void Log(object sender, LogEventArgs args)
        {
            // iOS supports Console.Error (NSLog), but every console color API
            // throws PlatformNotSupportedException. Preserve errors and notices
            // without touching the process-wide console color or stdout.
            string data = args.Data is null ? string.Empty : $"\n{args.Data}";
            Console.Error.WriteLine($"[skyhost] AUDIO_RENDERER_LOG level={args.Level} thread={args.ThreadName} {args.Message}{data}");
        }
        public void Dispose() { }
    }

    static SkyAudioBridge()
    {
        // Ryujinx.Common's logger redirects process stderr on initialization.
        // Retain the host log descriptor; renderer logging must not capture or
        // asynchronously discard native runtime/crash diagnostics.
        int stderr = dup(2);
        try { Ryujinx.Common.Logging.Logger.SetEnable(Ryujinx.Common.Logging.LogLevel.Info, false); }
        finally { if (stderr >= 0) { dup2(stderr, 2); close(stderr); } }
        // The default asynchronous target uses Console.ForegroundColor before
        // printing. Retire it before any DSP thread starts, and keep the same
        // bounded asynchronous delivery policy with a color-free host target.
        Logger.SetConsoleTargetEnabled(false);
        Logger.AddTarget(new AsyncLogTargetWrapper(new HostLogTarget(), 1000,
            AsyncLogTargetOverflowAction.Discard));
        Ryujinx.Common.Logging.Logger.SetEnable(Ryujinx.Common.Logging.LogLevel.Warning, false);
        Ryujinx.Common.Logging.Logger.SetEnable(Ryujinx.Common.Logging.LogLevel.Trace, false);
    }
    private sealed class Device : IHardwareDevice
    {
        private readonly delegate* unmanaged<nint, short*, uint, uint, void> _output;
        private readonly nint _context;
        private float _volume = 1;
        public Device(delegate* unmanaged<nint, short*, uint, uint, void> output, nint context)
        { _output = output; _context = context; }
        public uint GetChannelCount() => 2;
        public uint GetSampleRate() => 48000;
        public float GetVolume() => _volume;
        public void SetVolume(float volume) => _volume = volume;
        public void AppendBuffer(ReadOnlySpan<short> data, uint channels)
        { fixed (short* samples = data) _output(_context, samples, (uint)data.Length, channels); }
        public void Dispose() { }
    }

    private sealed class FrameEvent : IWritableEvent
    {
        private readonly delegate* unmanaged<nint, int, void> _event;
        private readonly nint _context;
        public FrameEvent(delegate* unmanaged<nint, int, void> callback, nint context)
        { _event = callback; _context = context; }
        public void Signal() => _event(_context, 1);
        public void Clear() => _event(_context, 0);
    }

    private sealed class Session : IDisposable
    {
        public AudioRenderSystem System { get; }
        private readonly void* _work;
        private readonly Thread _thread;
        private readonly FrameEvent _frameEvent;
        private readonly long _periodTicks;
        private volatile bool _exit;
        private volatile bool _running;
        public volatile int Error;

        public Session(byte* guestParameter, delegate* unmanaged<nint, short*, uint, uint, void> output,
                       delegate* unmanaged<nint, int, void> frameEvent, nint context)
        {
            AudioRendererConfiguration parameter = Unsafe.ReadUnaligned<AudioRendererConfiguration>(guestParameter);
            // SDK public parameters have separate 32-bit rendering device and
            // execution mode at 0x2c/0x30; their revision is at 0x34. The native
            // host always runs the renderer automatically on its own CPU thread.
            parameter.ExternalContextSize = 0;
            parameter.Revision = *(int*)(guestParameter + 0x34);
            parameter.RenderingDevice = 0;
            parameter.ExecutionMode = 0;
            if (parameter.SampleRate == 0 || parameter.SampleCount == 0)
                throw new ArgumentException("invalid audio frame rate");
            ulong size = AudioRenderSystem.GetWorkBufferSize(ref parameter);
            if (size == 0 || size > int.MaxValue) throw new ArgumentException("invalid audio work buffer size");
            _work = NativeMemory.AlignedAlloc((nuint)size, 0x1000);
            if (_work == null) throw new OutOfMemoryException();
            try
            {
                _frameEvent = new FrameEvent(frameEvent, context);
                System = new AudioRenderSystem(new AudioRendererManager(new Device(output, context)), _frameEvent);
                var memory = new NativeMemoryManager<byte>((byte*)_work, (int)size).Memory;
                var result = System.Initialize(ref parameter, 1, memory, (ulong)_work, size, 0, 1, new HostMemory());
                if ((uint)result != 0) throw new InvalidOperationException($"audio renderer initialization failed: {result}");
                _periodTicks = checked((long)((double)Stopwatch.Frequency * parameter.SampleCount / parameter.SampleRate));
                _thread = new Thread(Render) { IsBackground = true, Name = "Skyrim Audio DSP" };
                _thread.Start();
            }
            catch { NativeMemory.AlignedFree(_work); throw; }
        }

        private void Render()
        {
            long next = Stopwatch.GetTimestamp();
            while (!_exit)
            {
                next += _periodTicks;
                try { if (_running) System.ProcessHostFrame(); }
                catch (Exception error)
                {
                    Error = 0x2a2;
                    _running = false;
                    // A guest may already be asleep waiting for this frame.
                    // Publish the failure before waking it so its next update
                    // returns the error instead of waiting forever.
                    _frameEvent.Signal();
                    Console.Error.WriteLine($"[skyhost] AUDIO_DSP_ERROR {error}");
                }
                long remaining = next - Stopwatch.GetTimestamp();
                if (remaining > 0) Thread.Sleep(TimeSpan.FromSeconds((double)remaining / Stopwatch.Frequency));
                else next = Stopwatch.GetTimestamp();
            }
        }

        public void Start() { System.Start(); _running = true; }
        public void Stop() { _running = false; System.Stop(); }
        public void Dispose()
        {
            _exit = true;
            _thread.Join();
            System.Dispose();
            NativeMemory.AlignedFree(_work);
        }
    }

    private static Session Get(nint handle) => (Session)GCHandle.FromIntPtr(handle).Target;

    [UnmanagedCallersOnly(EntryPoint = "sky_audren_create")]
    public static nint Create(byte* parameter, delegate* unmanaged<nint, short*, uint, uint, void> output,
                              delegate* unmanaged<nint, int, void> frameEvent, nint context)
    {
        try { return GCHandle.ToIntPtr(GCHandle.Alloc(new Session(parameter, output, frameEvent, context))); }
        catch (Exception error) { Console.Error.WriteLine($"[skyhost] AUDIO_CREATE_ERROR {error}"); return 0; }
    }

    [UnmanagedCallersOnly(EntryPoint = "sky_audren_update")]
    public static uint Update(nint handle, byte* output, nuint outputSize, byte* performance, nuint performanceSize,
                              byte* input, nuint inputSize)
    {
        try
        {
            var session = Get(handle);
            if (session.Error != 0) return (uint)session.Error;
            Memory<byte> outMemory = new NativeMemoryManager<byte>(output, checked((int)outputSize)).Memory;
            Memory<byte> perfMemory = new NativeMemoryManager<byte>(performance, checked((int)performanceSize)).Memory;
            var inputMemory = new NativeMemoryManager<byte>(input, checked((int)inputSize)).Memory;
            uint result = (uint)session.System.Update(outMemory, perfMemory, new ReadOnlySequence<byte>(inputMemory));
            // An update already in flight can clear the final failure event.
            // Return a concurrently published DSP error directly in that case.
            return session.Error != 0 ? (uint)session.Error : result;
        }
        catch (Exception error) { Console.Error.WriteLine($"[skyhost] AUDIO_UPDATE_ERROR {error}"); return 0x2a2; }
    }

    [UnmanagedCallersOnly(EntryPoint = "sky_audren_start")]
    public static uint Start(nint handle)
    {
        try { var session = Get(handle); if (session.Error != 0) return (uint)session.Error; session.Start(); return 0; }
        catch (Exception error) { Console.Error.WriteLine($"[skyhost] AUDIO_START_ERROR {error}"); return 0x2a2; }
    }

    [UnmanagedCallersOnly(EntryPoint = "sky_audren_stop")]
    public static uint Stop(nint handle)
    { try { Get(handle).Stop(); return 0; } catch (Exception error) { Console.Error.WriteLine($"[skyhost] AUDIO_STOP_ERROR {error}"); return 0x2a2; } }

    [UnmanagedCallersOnly(EntryPoint = "sky_audren_destroy")]
    public static void Destroy(nint handle)
    {
        var owner = GCHandle.FromIntPtr(handle);
        try { ((Session)owner.Target).Dispose(); }
        catch (Exception error) { Console.Error.WriteLine($"[skyhost] AUDIO_DESTROY_ERROR {error}"); }
        finally { owner.Free(); }
    }
}
