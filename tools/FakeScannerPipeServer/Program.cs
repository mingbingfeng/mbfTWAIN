using System.Globalization;
using System.IO.Pipes;
using System.Text;

namespace MbfTwain.FakeScannerPipeServer;

internal sealed record Options(
    string PipeName,
    uint Revision,
    bool DuplexEnabled,
    string PixelType,
    int XResolution,
    int YResolution,
    bool ScanRequested,
    int MaxConnections,
    IReadOnlyList<string> Images);

internal static class Program
{
    private const string DefaultPipeName = "mbfTwain.VirtualScanner.v1";

    private static int Main(string[] args)
    {
        if (!TryParseOptions(args, out Options? options, out string? error))
        {
            Console.Error.WriteLine(error);
            PrintUsage();
            return 2;
        }

        RunServer(options);
        return 0;
    }

    private static void RunServer(Options initialOptions)
    {
        uint revision = initialOptions.Revision;
        bool scanRequested = initialOptions.ScanRequested;

        for (int connection = 0; connection < initialOptions.MaxConnections; connection++)
        {
            using var pipe = new NamedPipeServerStream(
                initialOptions.PipeName,
                PipeDirection.InOut,
                NamedPipeServerStream.MaxAllowedServerInstances,
                PipeTransmissionMode.Byte,
                PipeOptions.None);

            pipe.WaitForConnection();
            using var reader = new StreamReader(
                pipe,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                detectEncodingFromByteOrderMarks: false,
                leaveOpen: true);
            using var writer = new StreamWriter(
                pipe,
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                leaveOpen: true)
            {
                AutoFlush = true,
                NewLine = "\n",
            };

            string? command = reader.ReadLine();
            Console.Error.WriteLine(command ?? "<null>");

            if (string.Equals(command, "PING", StringComparison.Ordinal))
            {
                writer.Write("OK PONG\n");
                continue;
            }

            if (string.Equals(command, "GET_STATE", StringComparison.Ordinal))
            {
                WriteState(writer, initialOptions, revision, scanRequested);
                continue;
            }

            if (string.Equals(command, "BEGIN_SCAN", StringComparison.Ordinal))
            {
                writer.Write("OK BEGIN_SCAN\n");
                continue;
            }

            const string ackPrefix = "ACK_SCAN ";
            if (command is not null &&
                command.StartsWith(ackPrefix, StringComparison.Ordinal) &&
                uint.TryParse(command.AsSpan(ackPrefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out uint acknowledgedRevision))
            {
                if (acknowledgedRevision == revision)
                {
                    scanRequested = false;
                    revision++;
                }

                writer.Write("OK ACK\n");
                continue;
            }

            writer.Write("ERR unknown-command\n");
        }
    }

    private static void WriteState(StreamWriter writer, Options options, uint revision, bool scanRequested)
    {
        writer.WriteLine("OK STATE");
        writer.WriteLine(FormattableString.Invariant($"revision {revision}"));
        writer.WriteLine(FormattableString.Invariant($"duplex {(options.DuplexEnabled ? 1 : 0)}"));
        writer.WriteLine($"pixel {options.PixelType}");
        writer.WriteLine(FormattableString.Invariant($"xres {options.XResolution}"));
        writer.WriteLine(FormattableString.Invariant($"yres {options.YResolution}"));
        writer.WriteLine(FormattableString.Invariant($"scan {(scanRequested ? 1 : 0)}"));

        foreach (string image in options.Images)
        {
            writer.WriteLine($"image {image}");
        }

        writer.WriteLine("END");
    }

    private static bool TryParseOptions(
        string[] args,
        [System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out Options? options,
        out string? error)
    {
        string pipeName = DefaultPipeName;
        uint revision = 1;
        bool duplexEnabled = false;
        string pixelType = "RGB";
        int xResolution = 300;
        int yResolution = 300;
        bool scanRequested = true;
        int maxConnections = 3;
        List<string> images = [];

        for (int index = 0; index < args.Length; index++)
        {
            string arg = args[index];
            string? value = index + 1 < args.Length ? args[index + 1] : null;

            switch (arg)
            {
                case "--pipe":
                    if (string.IsNullOrWhiteSpace(value))
                    {
                        return Fail("Missing value for --pipe", out options, out error);
                    }

                    pipeName = value;
                    index++;
                    break;

                case "--revision":
                    if (!uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out revision))
                    {
                        return Fail("Invalid --revision value", out options, out error);
                    }

                    index++;
                    break;

                case "--duplex":
                    if (!TryParseBoolFlag(value, out duplexEnabled))
                    {
                        return Fail("Invalid --duplex value", out options, out error);
                    }

                    index++;
                    break;

                case "--pixel":
                    if (value is not ("BW" or "GRAY" or "RGB"))
                    {
                        return Fail("Invalid --pixel value", out options, out error);
                    }

                    pixelType = value;
                    index++;
                    break;

                case "--xres":
                    if (!int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out xResolution))
                    {
                        return Fail("Invalid --xres value", out options, out error);
                    }

                    index++;
                    break;

                case "--yres":
                    if (!int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out yResolution))
                    {
                        return Fail("Invalid --yres value", out options, out error);
                    }

                    index++;
                    break;

                case "--scan":
                    if (!TryParseBoolFlag(value, out scanRequested))
                    {
                        return Fail("Invalid --scan value", out options, out error);
                    }

                    index++;
                    break;

                case "--connections":
                    if (!int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out maxConnections) ||
                        maxConnections <= 0)
                    {
                        return Fail("Invalid --connections value", out options, out error);
                    }

                    index++;
                    break;

                case "--image":
                    if (string.IsNullOrWhiteSpace(value))
                    {
                        return Fail("Missing value for --image", out options, out error);
                    }

                    images.Add(Path.GetFullPath(value));
                    index++;
                    break;

                case "--help":
                case "-h":
                case "/?":
                    return Fail("Usage requested", out options, out error);

                default:
                    if (File.Exists(arg))
                    {
                        images.Add(Path.GetFullPath(arg));
                        break;
                    }

                    return Fail($"Unknown argument: {arg}", out options, out error);
            }
        }

        if (images.Count == 0)
        {
            return Fail("At least one --image path is required", out options, out error);
        }

        options = new Options(
            pipeName,
            revision,
            duplexEnabled,
            pixelType,
            xResolution,
            yResolution,
            scanRequested,
            maxConnections,
            images);
        error = null;
        return true;
    }

    private static bool TryParseBoolFlag(string? value, out bool result)
    {
        switch (value)
        {
            case "1":
            case "true":
            case "True":
                result = true;
                return true;
            case "0":
            case "false":
            case "False":
                result = false;
                return true;
            default:
                result = false;
                return false;
        }
    }

    private static bool Fail(string message, out Options? options, out string error)
    {
        options = null;
        error = message;
        return false;
    }

    private static void PrintUsage()
    {
        Console.Error.WriteLine(
            "Usage: FakeScannerPipeServer --image <path> [--connections 3] [--pixel RGB|GRAY|BW] [--revision n]");
    }
}
