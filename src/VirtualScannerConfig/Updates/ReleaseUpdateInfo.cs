namespace MbfTwain.VirtualScannerConfig.Updates;

internal sealed record ReleaseUpdateInfo(
    Version Version,
    string TagName,
    bool IsNewer,
    Uri HtmlUri,
    Uri? InstallerUri,
    string? InstallerName,
    long? InstallerSize,
    DateTimeOffset? PublishedAt);
