using System.Collections.Generic;

namespace MbfTwain.VirtualScannerConfig.Ipc;

public sealed record ScannerStateSnapshot(
    uint Revision,
    bool DuplexEnabled,
    string PixelType,
    int XResolution,
    int YResolution,
    bool ScanRequested,
    IReadOnlyList<string> SelectedImages);
