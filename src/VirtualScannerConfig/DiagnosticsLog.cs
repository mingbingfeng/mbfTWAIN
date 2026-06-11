using System;
using System.IO;
using System.Text;

namespace MbfTwain.VirtualScannerConfig;

internal static class DiagnosticsLog
{
    private static readonly object Sync = new();
    private static readonly string LogPath = Path.Combine(Path.GetTempPath(), "mbfTwain-diagnostics.log");

    internal static string PathOnDisk => LogPath;

    internal static void Write(string component, string message)
    {
        string line =
            $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} pid={Environment.ProcessId} tid={Environment.CurrentManagedThreadId} [{component}] {message}{Environment.NewLine}";

        try
        {
            lock (Sync)
            {
                using var stream = new FileStream(
                    LogPath,
                    FileMode.Append,
                    FileAccess.Write,
                    FileShare.ReadWrite | FileShare.Delete);
                using var writer = new StreamWriter(stream, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
                writer.Write(line);
            }
        }
        catch
        {
            // Diagnostics must never affect the TWAIN UI runtime.
        }
    }

    internal static void WriteException(string component, Exception exception, string context)
    {
        Write(component, $"{context}: {exception}");
    }
}
