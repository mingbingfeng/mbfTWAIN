namespace MbfTwain.VirtualScannerConfig.Updates;

internal readonly record struct DownloadProgress(long BytesRead, long? TotalBytes)
{
    public string ToDisplayText()
    {
        if (TotalBytes is { } totalBytes && totalBytes > 0)
        {
            double percent = BytesRead * 100D / totalBytes;
            return $"{FormatBytes(BytesRead)} / {FormatBytes(totalBytes)} ({percent:0}%)";
        }

        return FormatBytes(BytesRead);
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        double value = bytes;
        int unitIndex = 0;
        while (value >= 1024D && unitIndex < units.Length - 1)
        {
            value /= 1024D;
            unitIndex++;
        }

        return $"{value:0.#} {units[unitIndex]}";
    }
}
