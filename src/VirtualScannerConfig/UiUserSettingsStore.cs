using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using MbfTwain.VirtualScannerConfig.Ipc;

namespace MbfTwain.VirtualScannerConfig;

internal sealed class UiUserSettingsStore
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        WriteIndented = true,
    };

    private readonly string settingsPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "mbfTwain",
        "VirtualScannerConfig",
        "user-settings.json");

    internal string PathOnDisk => settingsPath;

    internal UiUserSettings Load()
    {
        if (!File.Exists(settingsPath))
        {
            return new UiUserSettings();
        }

        using var stream = new FileStream(settingsPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        UiUserSettings? settings = JsonSerializer.Deserialize<UiUserSettings>(stream, SerializerOptions);
        return settings ?? new UiUserSettings();
    }

    internal void Save(UiUserSettings settings)
    {
        string? directory = Path.GetDirectoryName(settingsPath);
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
        }

        using var stream = new FileStream(settingsPath, FileMode.Create, FileAccess.Write, FileShare.Read);
        JsonSerializer.Serialize(stream, settings, SerializerOptions);
    }
}

internal sealed class UiUserSettings
{
    public bool RememberSelectedImages { get; init; }

    public List<ScannerImageSelection> RememberedImages { get; init; } = [];
}
