using System;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace MbfTwain.VirtualScannerConfig.Ipc;

public sealed class ScannerPipeServer : IDisposable
{
    public const string PipeName = "mbfTwain.VirtualScanner.v1";

    private readonly Func<ScannerStateSnapshot> getSnapshot;
    private readonly Action<ScannerSessionSettings?> beginScan;
    private readonly Action<uint> acknowledgeScan;
    private readonly CancellationTokenSource cancellation = new();
    private Task? serverTask;

    public ScannerPipeServer(
        Func<ScannerStateSnapshot> getSnapshot,
        Action<ScannerSessionSettings?> beginScan,
        Action<uint> acknowledgeScan)
    {
        this.getSnapshot = getSnapshot;
        this.beginScan = beginScan;
        this.acknowledgeScan = acknowledgeScan;
    }

    public void Start()
    {
        DiagnosticsLog.Write("UI-IPC", $"Starting pipe server {PipeName}");
        serverTask ??= Task.Run(() => RunAsync(cancellation.Token));
    }

    public void Dispose()
    {
        cancellation.Cancel();
        try
        {
            serverTask?.Wait(TimeSpan.FromSeconds(2));
        }
        catch (AggregateException)
        {
        }

        cancellation.Dispose();
    }

    private async Task RunAsync(CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(
                PipeName,
                PipeDirection.InOut,
                NamedPipeServerStream.MaxAllowedServerInstances,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            try
            {
                DiagnosticsLog.Write("UI-IPC", $"Waiting for connection on {PipeName}");
                await pipe.WaitForConnectionAsync(token).ConfigureAwait(false);
                DiagnosticsLog.Write("UI-IPC", "Client connected");
                await HandleClientAsync(pipe, token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested)
            {
                DiagnosticsLog.Write("UI-IPC", "Pipe server cancellation requested");
                break;
            }
            catch (IOException exception)
            {
                DiagnosticsLog.WriteException("UI-IPC", exception, "RunAsync");
            }
        }
    }

    private async Task HandleClientAsync(Stream stream, CancellationToken token)
    {
        using var reader = new StreamReader(stream, new UTF8Encoding(false), detectEncodingFromByteOrderMarks: false, leaveOpen: true);
        await using var writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true)
        {
            AutoFlush = true,
            NewLine = "\n",
        };

        string? command = await reader.ReadLineAsync(token).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(command))
        {
            DiagnosticsLog.Write("UI-IPC", "Received empty command");
            await writer.WriteLineAsync("ERR empty-command").ConfigureAwait(false);
            return;
        }

        DiagnosticsLog.Write("UI-IPC", $"Received command: {command}");

        if (string.Equals(command, "PING", StringComparison.Ordinal))
        {
            await writer.WriteAsync("OK PONG\n").ConfigureAwait(false);
            DiagnosticsLog.Write("UI-IPC", "Responded with OK PONG");
            return;
        }

        if (string.Equals(command, "GET_STATE", StringComparison.Ordinal))
        {
            ScannerStateSnapshot snapshot = getSnapshot();
            DiagnosticsLog.Write(
                "UI-IPC",
                $"GET_STATE revision={snapshot.Revision} scan={snapshot.ScanRequested} images={snapshot.SelectedImages.Count} pixel={snapshot.PixelType} paper={snapshot.PaperSize} xres={snapshot.XResolution} yres={snapshot.YResolution}");
            await WriteStateAsync(writer, snapshot).ConfigureAwait(false);
            return;
        }

        if (string.Equals(command, "BEGIN_SCAN", StringComparison.Ordinal) ||
            command.StartsWith("BEGIN_SCAN ", StringComparison.Ordinal))
        {
            DiagnosticsLog.Write("UI-IPC", "BEGIN_SCAN accepted");
            beginScan(ParseBeginScanSettings(command));
            await writer.WriteAsync("OK BEGIN_SCAN\n").ConfigureAwait(false);
            return;
        }

        const string ackPrefix = "ACK_SCAN ";
        if (command.StartsWith(ackPrefix, StringComparison.Ordinal) &&
            uint.TryParse(command.AsSpan(ackPrefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out uint revision))
        {
            DiagnosticsLog.Write("UI-IPC", $"ACK_SCAN accepted revision={revision}");
            acknowledgeScan(revision);
            await writer.WriteAsync("OK ACK\n").ConfigureAwait(false);
            return;
        }

        DiagnosticsLog.Write("UI-IPC", $"Unknown command: {command}");
        await writer.WriteLineAsync("ERR unknown-command").ConfigureAwait(false);
    }

    private static async Task WriteStateAsync(StreamWriter writer, ScannerStateSnapshot snapshot)
    {
        await writer.WriteLineAsync("OK STATE").ConfigureAwait(false);
        await writer.WriteLineAsync(FormattableString.Invariant($"revision {snapshot.Revision}")).ConfigureAwait(false);
        await writer.WriteLineAsync(FormattableString.Invariant($"duplex {(snapshot.DuplexEnabled ? 1 : 0)}")).ConfigureAwait(false);
        await writer.WriteLineAsync($"pixel {snapshot.PixelType}").ConfigureAwait(false);
        await writer.WriteLineAsync($"paper {snapshot.PaperSize}").ConfigureAwait(false);
        await writer.WriteLineAsync(FormattableString.Invariant($"xres {snapshot.XResolution}")).ConfigureAwait(false);
        await writer.WriteLineAsync(FormattableString.Invariant($"yres {snapshot.YResolution}")).ConfigureAwait(false);
        await writer.WriteLineAsync(FormattableString.Invariant($"scan {(snapshot.ScanRequested ? 1 : 0)}")).ConfigureAwait(false);
        foreach (string image in snapshot.SelectedImages)
        {
            await writer.WriteLineAsync($"image {image}").ConfigureAwait(false);
        }

        await writer.WriteLineAsync("END").ConfigureAwait(false);
    }

    private static ScannerSessionSettings? ParseBeginScanSettings(string command)
    {
        string[] tokens = command.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (tokens.Length <= 1)
        {
            return null;
        }

        bool duplexEnabled = false;
        string pixelType = "RGB";
        string paperSize = "A4";
        int xResolution = 300;
        int yResolution = 300;
        bool sawSetting = false;

        foreach (string token in tokens.Skip(1))
        {
            string[] parts = token.Split('=', 2);
            if (parts.Length != 2)
            {
                continue;
            }

            switch (parts[0])
            {
                case "duplex":
                    if (parts[1] is "0" or "1")
                    {
                        duplexEnabled = parts[1] == "1";
                        sawSetting = true;
                    }
                    break;
                case "pixel":
                    if (parts[1] is "BW" or "GRAY" or "RGB")
                    {
                        pixelType = parts[1];
                        sawSetting = true;
                    }
                    break;
                case "paper":
                    if (parts[1] is "A4" or "A3")
                    {
                        paperSize = parts[1];
                        sawSetting = true;
                    }
                    break;
                case "xres":
                    if (int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out int parsedXResolution))
                    {
                        xResolution = parsedXResolution;
                        sawSetting = true;
                    }
                    break;
                case "yres":
                    if (int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out int parsedYResolution))
                    {
                        yResolution = parsedYResolution;
                        sawSetting = true;
                    }
                    break;
            }
        }

        return sawSetting
            ? new ScannerSessionSettings(duplexEnabled, pixelType, paperSize, xResolution, yResolution)
            : null;
    }
}
