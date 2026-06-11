using System.Collections.Generic;

namespace MbfTwain.VirtualScannerConfig.Ipc;

public sealed record ScannerStateSnapshot(
    uint Revision,
    bool DuplexEnabled,
    string PixelType,
    string PaperSize,
    int XResolution,
    int YResolution,
    bool ScanRequested,
    IReadOnlyList<string> SelectedImages);

public sealed record ScannerSessionSettings(
    bool DuplexEnabled,
    string PixelType,
    string PaperSize,
    int XResolution,
    int YResolution);
