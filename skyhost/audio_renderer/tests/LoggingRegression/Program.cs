using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Ryujinx.Common.Logging;

sealed class ForbiddenConsoleWriter : TextWriter
{
    public override Encoding Encoding => Encoding.UTF8;
    public override void Write(char value) => throw new PlatformNotSupportedException("Colored console target must not receive host audio logs");
    public override void WriteLine(string? value) => throw new PlatformNotSupportedException("Colored console target must not receive host audio logs");
}

sealed class CapturedErrorWriter : TextWriter
{
    private readonly object _lock = new();
    private readonly StringBuilder _text = new();
    public readonly ManualResetEventSlim Ready = new();
    public override Encoding Encoding => Encoding.UTF8;
    public override void WriteLine(string? value)
    {
        lock (_lock)
        {
            _text.AppendLine(value);
            if (_text.ToString().Contains("error-marker") && _text.ToString().Contains("notice-marker")) Ready.Set();
        }
    }
    public string Text { get { lock (_lock) return _text.ToString(); } }
}

static class Program
{
    [DllImport("libc")] private static extern nint write(int descriptor, byte[] bytes, nuint count);

    static int Main()
    {
        // Run the actual production initializer, including native stderr
        // preservation and retirement of Ryujinx's default console target.
        RuntimeHelpers.RunClassConstructor(typeof(SkyAudioBridge).TypeHandle);
        TextWriter stdout = Console.Out, stderr = Console.Error;
        CapturedErrorWriter captured = new();
        Console.SetOut(new ForbiddenConsoleWriter());
        Console.SetError(captured);
        try
        {
            Logger.Error!.Value.Print(LogClass.AudioRenderer, "error-marker", "data-marker");
            Logger.Notice.Print(LogClass.AudioRenderer, "notice-marker");
            Logger.Flush();
            if (!captured.Ready.Wait(5000)) throw new InvalidOperationException("Error/Notice did not reach color-free stderr");
            string text = captured.Text;
            if (!text.Contains("AUDIO_RENDERER_LOG level=Error") || !text.Contains("AUDIO_RENDERER_LOG level=Notice") ||
                !text.Contains("AudioRenderer") || !text.Contains("data-marker"))
                throw new InvalidOperationException("Host logging lost a level, message, or payload");
            Logger.RemoveTarget("sky-audio-stderr");
            byte[] native = Encoding.UTF8.GetBytes("NATIVE_STDERR_PRESERVED\n");
            if (write(2, native, (nuint)native.Length) != native.Length) throw new IOException("Native stderr no longer writes directly");
        }
        finally
        {
            Console.SetOut(stdout);
            Console.SetError(stderr);
            captured.Ready.Dispose();
        }
        Console.WriteLine("AUDIO_LOGGING_OK actual-bridge-init error-notice-data color-free-stderr no-console-target native-fd2-preserved");
        return 0;
    }
}
