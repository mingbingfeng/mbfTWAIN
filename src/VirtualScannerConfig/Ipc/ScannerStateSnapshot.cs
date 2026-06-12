using System.Collections.Generic;

namespace MbfTwain.VirtualScannerConfig.Ipc;

public sealed record ScannerImageSelection(
    string Path,
    int RotationDegrees);

public sealed record ScannerStateSnapshot(
    uint Revision,
    bool DuplexEnabled,
    string PixelType,
    string PaperSize,
    int XResolution,
    int YResolution,
    bool ScanRequested,
    IReadOnlyList<ScannerImageSelection> SelectedImages);

public sealed record ScannerSessionSettings(
    bool DuplexEnabled,
    string PixelType,
    string PaperSize,
    int XResolution,
    int YResolution);
