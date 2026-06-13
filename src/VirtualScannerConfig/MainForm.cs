using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using MbfTwain.VirtualScannerConfig.Ipc;
using MbfTwain.VirtualScannerConfig.Updates;

namespace MbfTwain.VirtualScannerConfig;

public sealed partial class MainForm : Form
{
    private static readonly Color AppBackground = Color.FromArgb(244, 246, 239);
    private static readonly Color SurfaceBackground = Color.FromArgb(255, 255, 252);
    private static readonly Color AccentColor = Color.FromArgb(29, 111, 84);
    private static readonly Color AccentSoft = Color.FromArgb(228, 242, 235);
    private static readonly Color BorderColor = Color.FromArgb(210, 217, 206);
    private static readonly Color MutedTextColor = Color.FromArgb(94, 104, 97);
    private static readonly string[] PixelTypeOptions = ["BW", "GRAY", "RGB"];
    private static readonly string[] PaperSizeOptions = ["A4", "A3"];
    private static readonly int[] DpiOptions = [150, 200, 300, 600];
    private static readonly Size ThumbnailImageSize = new(172, 172);
    private const int MaxThumbnailSourcePixels = 1_048_576;
    private const int MaxPreviewImagePixels = 16_777_216;
    private const int ThumbnailCacheLimit = 128;

    private readonly object stateLock = new();
    private readonly List<ScannerImageSelection> selectedImages = [];
    private readonly ThumbnailCache thumbnailCache = new(ThumbnailCacheLimit);
    private readonly ScannerPipeServer pipeServer;
    private readonly GitHubUpdateService updateService = new();
    private readonly CancellationTokenSource updateCancellation = new();
    private CancellationTokenSource thumbnailLoadCancellation = new();
    private SynchronizationContext? uiContext;

    private uint revision;
    private bool pipeServerStarted;
    private bool scanRequested;
    private bool duplexEnabled;
    private string pixelType = "RGB";
    private string paperSize = "A4";
    private int scanDpi = 300;
    private int selectedImageIndex = -1;
    private ReleaseUpdateInfo? latestRelease;

    private readonly FlowLayoutPanel thumbnailStrip = new();
    private readonly LinkLabel clearImagesLink = new();
    private readonly Label settingsSummaryLabel = new();
    private readonly Button settingsButton = new();
    private readonly Label statusLabel = new();
    private readonly ToolTip toolTip = new();
    private Button? addTileButton;

    public MainForm()
    {
        Text = "mbfTwain 虚拟扫描仪";
        Size = new Size(900, 600);
        MinimumSize = new Size(820, 560);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = AppBackground;

        pipeServer = new ScannerPipeServer(
            GetSnapshot,
            sessionSettings => PostPipeCallback(() => BeginScanSession(sessionSettings)),
            acknowledgedRevision => PostPipeCallback(() => HideScanUi(acknowledgedRevision)),
            acknowledgedRevision => PostPipeCallback(() => AcknowledgeScan(acknowledgedRevision)));
        BuildLayout();
        UpdateSettingsSummary();
        RefreshQueueVisuals();
        Shown += (_, _) =>
        {
            StartPipeServer();
            StartSilentUpdateCheck();
        };
        DiagnosticsLog.Write("UI", "MainForm initialized");
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            updateCancellation.Cancel();
            thumbnailLoadCancellation.Cancel();
            foreach (Control control in thumbnailStrip.Controls)
            {
                DisposeControlImages(control);
            }

            thumbnailCache.Dispose();
            pipeServer.Dispose();
            thumbnailLoadCancellation.Dispose();
            updateCancellation.Dispose();
        }

        base.Dispose(disposing);
    }

    private void BuildLayout()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            Padding = new Padding(12),
            BackColor = AppBackground,
        };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        root.Controls.Add(BuildQueueSurface(), 0, 0);
        root.Controls.Add(BuildFooterSurface(), 0, 1);

        Controls.Add(root);
    }

    private Control BuildFooterSurface()
    {
        var footer = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            AutoSize = true,
            BackColor = AppBackground,
            Margin = new Padding(0, 8, 0, 0),
        };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        footer.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        statusLabel.AutoSize = true;
        statusLabel.Anchor = AnchorStyles.Left;
        statusLabel.ForeColor = MutedTextColor;
        statusLabel.TextAlign = ContentAlignment.MiddleLeft;
        statusLabel.Margin = new Padding(2, 0, 12, 0);
        footer.Controls.Add(statusLabel, 0, 0);

        footer.Controls.Add(BuildSettingsSurface(), 1, 0);
        return footer;
    }

    private Control BuildSettingsSurface()
    {
        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            Padding = new Padding(10, 8, 10, 8),
            Margin = new Padding(0),
            AutoSize = true,
            BackColor = Color.FromArgb(247, 249, 245),
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.Paint += (_, args) =>
        {
            using var pen = new Pen(BorderColor);
            var bounds = Rectangle.Inflate(layout.ClientRectangle, -1, -1);
            args.Graphics.DrawRectangle(pen, bounds);
        };

        settingsSummaryLabel.AutoSize = true;
        settingsSummaryLabel.Anchor = AnchorStyles.Left;
        settingsSummaryLabel.ForeColor = MutedTextColor;
        settingsSummaryLabel.Margin = new Padding(0, 0, 8, 0);
        layout.Controls.Add(settingsSummaryLabel, 0, 0);

        settingsButton.Width = 30;
        settingsButton.Height = 26;
        settingsButton.Text = "⚙";
        settingsButton.FlatStyle = FlatStyle.Flat;
        settingsButton.BackColor = layout.BackColor;
        settingsButton.ForeColor = Color.FromArgb(49, 58, 52);
        settingsButton.Anchor = AnchorStyles.None;
        settingsButton.Margin = new Padding(0);
        settingsButton.FlatAppearance.BorderColor = BorderColor;
        settingsButton.Click += (_, _) => ShowSettingsDialog();
        toolTip.SetToolTip(settingsButton, "扫描设置");
        layout.Controls.Add(settingsButton, 1, 0);

        UpdateSettingsSummary();
        return layout;
    }

    private static Panel BuildSurfacePanel()
    {
        var panel = new Panel
        {
            Dock = DockStyle.Fill,
            BackColor = SurfaceBackground,
        };
        panel.Paint += (_, args) =>
        {
            using var pen = new Pen(BorderColor);
            var bounds = Rectangle.Inflate(panel.ClientRectangle, -1, -1);
            args.Graphics.DrawRectangle(pen, bounds);
        };
        return panel;
    }

    private void UpdateSettingsSummary()
    {
        settingsSummaryLabel.Text = $"{pixelType} · {paperSize} · {scanDpi} DPI · {(duplexEnabled ? "双面" : "单面")}";
    }

    private void StartSilentUpdateCheck()
    {
        if (updateCancellation.IsCancellationRequested)
        {
            return;
        }

        _ = Task.Run(() => CheckForUpdatesSilentlyAsync(updateCancellation.Token), updateCancellation.Token);
    }

    private void StartPipeServer()
    {
        if (pipeServerStarted)
        {
            return;
        }

        uiContext ??= SynchronizationContext.Current ?? new WindowsFormsSynchronizationContext();
        pipeServerStarted = true;
        DiagnosticsLog.Write("UI", $"Starting pipe server after form shown handleCreated={IsHandleCreated}");
        pipeServer.Start();
    }

    private void PostPipeCallback(Action callback)
    {
        SynchronizationContext? context = uiContext;
        if (context is null)
        {
            DiagnosticsLog.Write("UI", "Ignoring pipe callback before UI context is ready");
            return;
        }

        context.Post(
            _ =>
            {
                if (IsDisposed || Disposing)
                {
                    DiagnosticsLog.Write("UI", "Ignoring pipe callback because form is disposing or disposed");
                    return;
                }

                callback();
            },
            null);
    }

    private async Task CheckForUpdatesSilentlyAsync(CancellationToken cancellationToken)
    {
        try
        {
            ReleaseUpdateInfo release = await updateService.GetLatestReleaseAsync(cancellationToken).ConfigureAwait(false);
            if (cancellationToken.IsCancellationRequested || IsDisposed)
            {
                return;
            }

            void Apply()
            {
                latestRelease = release;
                if (release.IsNewer)
                {
                    UpdateStatus();
                }
            }

            if (!IsHandleCreated)
            {
                return;
            }

            if (InvokeRequired)
            {
                BeginInvoke((MethodInvoker)Apply);
            }
            else
            {
                Apply();
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception exception)
        {
            DiagnosticsLog.WriteException("UI", exception, "Silent update check");
        }
    }

    private void ShowSettingsDialog()
    {
        bool currentDuplex;
        string currentPixelType;
        string currentPaperSize;
        int currentDpi;
        lock (stateLock)
        {
            currentDuplex = duplexEnabled;
            currentPixelType = pixelType;
            currentPaperSize = paperSize;
            currentDpi = scanDpi;
        }

        using var dialog = new Form
        {
            Text = "扫描设置",
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterParent,
            MinimizeBox = false,
            MaximizeBox = false,
            ShowInTaskbar = false,
            ClientSize = new Size(380, 320),
            BackColor = SurfaceBackground,
        };

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 6,
            Padding = new Padding(16),
            BackColor = SurfaceBackground,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 70));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int row = 0; row < 5; row++)
        {
            layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        }
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        var pixelInput = BuildSettingsCombo(PixelTypeOptions.Cast<object>().ToArray(), currentPixelType);
        var paperInput = BuildSettingsCombo(PaperSizeOptions.Cast<object>().ToArray(), currentPaperSize);
        var dpiInput = BuildSettingsCombo(DpiOptions.Cast<object>().ToArray(), currentDpi);
        var duplexInput = new CheckBox
        {
            Text = "双面",
            Checked = currentDuplex,
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            Margin = new Padding(0, 6, 0, 8),
        };

        AddSettingsDialogRow(layout, "像素", pixelInput, 0);
        AddSettingsDialogRow(layout, "纸张", paperInput, 1);
        AddSettingsDialogRow(layout, "DPI", dpiInput, 2);
        layout.Controls.Add(new Label { AutoSize = true, Text = string.Empty, Margin = new Padding(0) }, 0, 3);
        layout.Controls.Add(duplexInput, 1, 3);
        Control updatePanel = BuildUpdateSettingsPanel(dialog);
        layout.Controls.Add(updatePanel, 0, 4);
        layout.SetColumnSpan(updatePanel, 2);

        var buttons = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft,
            AutoSize = true,
            Margin = new Padding(0, 12, 0, 0),
        };
        var okButton = new Button { Text = "确定", DialogResult = DialogResult.OK, Width = 76 };
        var cancelButton = new Button { Text = "取消", DialogResult = DialogResult.Cancel, Width = 76 };
        buttons.Controls.Add(okButton);
        buttons.Controls.Add(cancelButton);
        layout.Controls.Add(buttons, 0, 5);
        layout.SetColumnSpan(buttons, 2);

        dialog.AcceptButton = okButton;
        dialog.CancelButton = cancelButton;
        dialog.Controls.Add(layout);

        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return;
        }

        ApplySettingsFromDialog(
            duplexInput.Checked,
            pixelInput.SelectedItem?.ToString() ?? "RGB",
            paperInput.SelectedItem?.ToString() ?? "A4",
            Convert.ToInt32(dpiInput.SelectedItem ?? 300));
    }

    private Control BuildUpdateSettingsPanel(IWin32Window owner)
    {
        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            ColumnCount = 2,
            RowCount = 2,
            AutoSize = true,
            Margin = new Padding(0, 12, 0, 0),
            BackColor = SurfaceBackground,
        };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        var versionLabel = new Label
        {
            AutoSize = true,
            Text = $"当前版本 {updateService.CurrentVersionText}",
            Anchor = AnchorStyles.Left,
            ForeColor = Color.FromArgb(49, 58, 52),
            Margin = new Padding(0, 2, 8, 4),
        };
        panel.Controls.Add(versionLabel, 0, 0);

        var updateButton = new Button
        {
            Text = "检查更新",
            Width = 92,
            Height = 28,
            Anchor = AnchorStyles.Right,
            Margin = new Padding(8, 0, 0, 4),
        };
        panel.Controls.Add(updateButton, 1, 0);

        var updateStatus = new Label
        {
            AutoSize = false,
            Dock = DockStyle.Top,
            Height = 36,
            Text = GetUpdateStatusText(),
            ForeColor = MutedTextColor,
            Margin = new Padding(0, 0, 0, 0),
        };
        panel.Controls.Add(updateStatus, 0, 1);
        panel.SetColumnSpan(updateStatus, 2);

        updateButton.Click += async (_, _) =>
        {
            await RunUpdateFromSettingsAsync(owner, updateStatus, updateButton);
        };

        return panel;
    }

    private string GetUpdateStatusText()
    {
        return latestRelease switch
        {
            { IsNewer: true } release => $"发现新版本 {release.TagName}",
            { } release => $"已检查，最新发布 {release.TagName}",
            _ => "从 GitHub Releases 检查安装包更新。",
        };
    }

    private async Task RunUpdateFromSettingsAsync(IWin32Window owner, Label updateStatus, Button updateButton)
    {
        updateButton.Enabled = false;
        try
        {
            updateStatus.Text = "正在检查 GitHub Releases...";
            CancellationToken cancellationToken = updateCancellation.Token;
            ReleaseUpdateInfo release = await Task
                .Run(() => updateService.GetLatestReleaseAsync(cancellationToken), cancellationToken)
                .ConfigureAwait(true);
            latestRelease = release;
            UpdateStatus();

            if (!release.IsNewer)
            {
                updateStatus.Text = $"已是最新版本 {updateService.CurrentVersionText}";
                MessageBox.Show(owner, "当前已经是最新版本。", "检查更新", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (release.InstallerUri is null)
            {
                updateStatus.Text = $"发现 {release.TagName}，但没有安装包。";
                MessageBox.Show(owner, "最新 GitHub Release 没有可下载的安装包。", "检查更新", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            updateStatus.Text = $"发现新版本 {release.TagName}";
            DialogResult answer = MessageBox.Show(
                owner,
                $"发现新版本 {release.TagName}。\r\n当前版本 {updateService.CurrentVersionText}。\r\n\r\n是否下载并启动安装程序？",
                "检查更新",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Information);
            if (answer != DialogResult.Yes)
            {
                updateStatus.Text = "已取消更新。";
                return;
            }

            var progress = new Progress<DownloadProgress>(progressValue =>
            {
                if (!updateStatus.IsDisposed)
                {
                    updateStatus.Text = $"正在下载 {progressValue.ToDisplayText()}";
                }
            });
            string installerPath = await Task
                .Run(() => updateService.DownloadInstallerAsync(release, progress, cancellationToken), cancellationToken)
                .ConfigureAwait(true);
            updateStatus.Text = "下载完成，正在启动安装程序...";
            LaunchInstallerElevated(installerPath);
            updateStatus.Text = "安装程序已启动，请按提示完成安装。";
        }
        catch (OperationCanceledException)
        {
            updateStatus.Text = "更新检查已取消。";
        }
        catch (Win32Exception exception) when (exception.NativeErrorCode == 1223)
        {
            updateStatus.Text = "已取消管理员授权。";
        }
        catch (Exception exception)
        {
            DiagnosticsLog.WriteException("UI", exception, "Update from settings");
            updateStatus.Text = "更新失败，请稍后重试。";
            MessageBox.Show(owner, exception.Message, "更新失败", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            if (!updateButton.IsDisposed)
            {
                updateButton.Enabled = true;
            }
        }
    }

    private static void LaunchInstallerElevated(string installerPath)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = installerPath,
            UseShellExecute = true,
            Verb = "runas",
        };

        Process? process = Process.Start(startInfo);
        if (process is null)
        {
            throw new InvalidOperationException("无法启动安装程序。");
        }
    }

    private static ComboBox BuildSettingsCombo(object[] items, object selected)
    {
        var input = new ComboBox
        {
            Dock = DockStyle.Top,
            DropDownStyle = ComboBoxStyle.DropDownList,
            Margin = new Padding(0, 0, 0, 8),
        };
        input.Items.AddRange(items);
        input.SelectedItem = selected;
        if (input.SelectedIndex < 0 && input.Items.Count > 0)
        {
            input.SelectedIndex = 0;
        }

        return input;
    }

    private static void AddSettingsDialogRow(TableLayoutPanel layout, string labelText, Control control, int row)
    {
        layout.Controls.Add(
            new Label
            {
                AutoSize = true,
                Text = labelText,
                Anchor = AnchorStyles.Left,
                ForeColor = Color.FromArgb(49, 58, 52),
                Margin = new Padding(0, 4, 8, 8),
            },
            0,
            row);
        layout.Controls.Add(control, 1, row);
    }

    private void ApplySettingsFromDialog(bool duplex, string selectedPixelType, string selectedPaperSize, int selectedDpi)
    {
        lock (stateLock)
        {
            duplexEnabled = duplex;
            pixelType = PixelTypeOptions.Contains(selectedPixelType) ? selectedPixelType : "RGB";
            paperSize = PaperSizeOptions.Contains(selectedPaperSize) ? selectedPaperSize : "A4";
            scanDpi = DpiOptions.Contains(selectedDpi) ? selectedDpi : 300;
            revision++;
        }

        UpdateSettingsSummary();
        UpdateStatus();
    }

    private bool AddImages()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "图片文件|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff|所有文件|*.*",
            Multiselect = true,
            Title = "选择待扫描图片",
        };

        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return false;
        }

        int firstAddedIndex = -1;
        int addedCount = 0;
        lock (stateLock)
        {
            foreach (string fileName in dialog.FileNames.Select(Path.GetFullPath).Where(File.Exists))
            {
                selectedImages.Add(new ScannerImageSelection(fileName, 0));
                addedCount++;
                if (firstAddedIndex < 0)
                {
                    firstAddedIndex = selectedImages.Count - 1;
                }
            }

            if (addedCount > 0)
            {
                if (selectedImageIndex < 0)
                {
                    selectedImageIndex = firstAddedIndex;
                }

                revision++;
            }
        }

        if (addedCount == 0)
        {
            return false;
        }

        RefreshQueueVisuals();
        return true;
    }

    private void RemoveImage(int index)
    {
        bool removed = false;
        lock (stateLock)
        {
            if (index < 0 || index >= selectedImages.Count)
            {
                return;
            }

            selectedImages.RemoveAt(index);
            if (selectedImages.Count == 0)
            {
                selectedImageIndex = -1;
                scanRequested = false;
            }
            else if (selectedImageIndex == index)
            {
                selectedImageIndex = Math.Min(index, selectedImages.Count - 1);
            }
            else if (selectedImageIndex > index)
            {
                selectedImageIndex--;
            }
            else if (selectedImageIndex >= selectedImages.Count)
            {
                selectedImageIndex = selectedImages.Count - 1;
            }

            revision++;
            removed = true;
        }

        if (removed)
        {
            RefreshQueueVisuals();
        }
    }

    private void ClearImages()
    {
        lock (stateLock)
        {
            selectedImages.Clear();
            selectedImageIndex = -1;
            scanRequested = false;
            revision++;
        }

        RefreshQueueVisuals();
    }

    private void MoveImage(int index, int direction)
    {
        bool moved = false;
        lock (stateLock)
        {
            int targetIndex = index + direction;
            if (index < 0 ||
                index >= selectedImages.Count ||
                targetIndex < 0 ||
                targetIndex >= selectedImages.Count)
            {
                return;
            }

            (selectedImages[index], selectedImages[targetIndex]) =
                (selectedImages[targetIndex], selectedImages[index]);
            selectedImageIndex = targetIndex;
            revision++;
            moved = true;
        }

        if (moved)
        {
            RefreshQueueVisuals();
        }
    }

    private void RotateImage(int index, int deltaDegrees)
    {
        bool rotated = false;
        lock (stateLock)
        {
            if (index < 0 || index >= selectedImages.Count)
            {
                return;
            }

            ScannerImageSelection current = selectedImages[index];
            int updatedRotation = NormalizeRotationDegrees(current.RotationDegrees + deltaDegrees);
            selectedImages[index] = current with { RotationDegrees = updatedRotation };
            selectedImageIndex = index;
            revision++;
            rotated = true;
        }

        if (rotated)
        {
            RefreshQueueVisuals();
        }
    }

    private void TriggerScan()
    {
        bool hasImages;
        lock (stateLock)
        {
            hasImages = selectedImages.Count > 0;
        }

        if (!hasImages && !AddImages())
        {
            return;
        }

        lock (stateLock)
        {
            if (selectedImages.Count > 0)
            {
                scanRequested = true;
                revision++;
            }
        }

        UpdateSettingsSummary();
        UpdateStatus();
    }

    private ScannerStateSnapshot GetSnapshot()
    {
        lock (stateLock)
        {
            return new ScannerStateSnapshot(
                revision,
                duplexEnabled,
                pixelType,
                paperSize,
                scanDpi,
                scanDpi,
                scanRequested,
                selectedImages.ToArray());
        }
    }

    private void BeginScanSession(ScannerSessionSettings? sessionSettings)
    {
        DiagnosticsLog.Write(
            "UI",
            $"BeginScanSession requested disposed={IsDisposed} handleCreated={IsHandleCreated} paper={sessionSettings?.PaperSize ?? "<none>"}");
        if (IsDisposed)
        {
            return;
        }

        lock (stateLock)
        {
            if (sessionSettings is not null)
            {
                ApplySessionSettings(sessionSettings);
            }

            selectedImages.Clear();
            selectedImageIndex = -1;
            scanRequested = false;
            revision++;
        }

        DiagnosticsLog.Write("UI", $"BeginScanSession.Apply revision={revision}");
        RefreshQueueVisuals();
        if (WindowState == FormWindowState.Minimized)
        {
            WindowState = FormWindowState.Normal;
        }

        Show();
        Activate();
        DiagnosticsLog.Write("UI", "BeginScanSession.Apply completed and window activated");
    }

    private void ApplySessionSettings(ScannerSessionSettings settings)
    {
        duplexEnabled = settings.DuplexEnabled;

        if (PixelTypeOptions.Contains(settings.PixelType))
        {
            pixelType = settings.PixelType;
        }

        if (PaperSizeOptions.Contains(settings.PaperSize))
        {
            paperSize = settings.PaperSize;
        }

        if (DpiOptions.Contains(settings.XResolution))
        {
            scanDpi = settings.XResolution;
        }

        UpdateSettingsSummary();
    }

    private void AcknowledgeScan(uint acknowledgedRevision)
    {
        DiagnosticsLog.Write("UI", $"AcknowledgeScan requested revision={acknowledgedRevision}");
        bool acknowledged = false;
        lock (stateLock)
        {
            if (scanRequested && acknowledgedRevision <= revision)
            {
                scanRequested = false;
                selectedImages.Clear();
                selectedImageIndex = -1;
                revision++;
                acknowledged = true;
            }
        }

        if (!acknowledged)
        {
            DiagnosticsLog.Write("UI", "AcknowledgeScan ignored because current state did not match");
            return;
        }

        RefreshQueueVisuals();
        Hide();
        DiagnosticsLog.Write("UI", "AcknowledgeScan applied and window hidden");
    }

    private void HideScanUi(uint acknowledgedRevision)
    {
        DiagnosticsLog.Write("UI", $"HideScanUi requested revision={acknowledgedRevision}");
        bool shouldHide;
        lock (stateLock)
        {
            shouldHide = acknowledgedRevision == 0 || (scanRequested && acknowledgedRevision <= revision);
        }

        if (!shouldHide)
        {
            DiagnosticsLog.Write("UI", "HideScanUi ignored because current state did not match");
            return;
        }

        Hide();
        DiagnosticsLog.Write("UI", "HideScanUi applied and window hidden");
    }

    private void NormalizeSelection()
    {
        lock (stateLock)
        {
            if (selectedImages.Count == 0)
            {
                selectedImageIndex = -1;
            }
            else if (selectedImageIndex < 0 || selectedImageIndex >= selectedImages.Count)
            {
                selectedImageIndex = 0;
            }
        }
    }

    private void RefreshThumbnailStrip()
    {
        List<ScannerImageSelection> snapshot;
        int activeIndex;
        lock (stateLock)
        {
            snapshot = [.. selectedImages];
            activeIndex = selectedImageIndex;
        }

        CancellationToken cancellationToken = RestartThumbnailLoads();
        Button addButton = GetAddTileButton();

        thumbnailStrip.SuspendLayout();
        try
        {
            for (int index = 0; index < snapshot.Count; index++)
            {
                Control? existing = index < thumbnailStrip.Controls.Count ? thumbnailStrip.Controls[index] : null;
                if (existing is Panel tile && tile.Tag is ThumbnailTileState state)
                {
                    UpdateThumbnailTile(tile, state, snapshot[index], index, index == activeIndex, cancellationToken);
                    continue;
                }

                Control newTile = BuildThumbnailTile(snapshot[index], index, index == activeIndex, cancellationToken);
                thumbnailStrip.Controls.Add(newTile);
                thumbnailStrip.Controls.SetChildIndex(newTile, index);
            }

            for (int index = thumbnailStrip.Controls.Count - 1; index >= snapshot.Count; index--)
            {
                Control control = thumbnailStrip.Controls[index];
                if (ReferenceEquals(control, addButton))
                {
                    continue;
                }

                thumbnailStrip.Controls.RemoveAt(index);
                DisposeControlImages(control);
                control.Dispose();
            }

            if (!thumbnailStrip.Controls.Contains(addButton))
            {
                thumbnailStrip.Controls.Add(addButton);
            }

            thumbnailStrip.Controls.SetChildIndex(addButton, snapshot.Count);
        }
        finally
        {
            thumbnailStrip.ResumeLayout();
        }
    }

    private CancellationToken RestartThumbnailLoads()
    {
        CancellationTokenSource previous = thumbnailLoadCancellation;
        thumbnailLoadCancellation = new CancellationTokenSource();
        previous.Cancel();
        previous.Dispose();
        return thumbnailLoadCancellation.Token;
    }

    private Button GetAddTileButton()
    {
        if (addTileButton is not null)
        {
            return addTileButton;
        }

        addTileButton = BuildAddTileButton();
        return addTileButton;
    }

    private Button BuildAddTileButton()
    {
        var button = new Button
        {
            Width = 176,
            Height = 170,
            Margin = new Padding(0, 0, 14, 16),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.FromArgb(251, 252, 248),
            ForeColor = AccentColor,
            Font = new Font(Font.FontFamily, 14F, FontStyle.Regular),
            Text = "+\r\n添加图片",
            TextAlign = ContentAlignment.MiddleCenter,
        };
        button.FlatAppearance.BorderColor = BorderColor;
        button.FlatAppearance.BorderSize = 1;
        button.Click += (_, _) => AddImages();
        return button;
    }

    private Control BuildThumbnailTile(
        ScannerImageSelection image,
        int index,
        bool isSelected,
        CancellationToken cancellationToken)
    {
        var tile = new Panel
        {
            Width = 190,
            Height = 190,
            Margin = new Padding(0, 0, 14, 16),
            Padding = new Padding(8),
            BackColor = isSelected ? AccentSoft : Color.White,
            Cursor = Cursors.Hand,
        };
        tile.DoubleClick += (sender, _) => ShowFullscreenPreview(GetTileIndex((Control)sender!));
        tile.Paint += (_, args) => DrawThumbnailBorder(
            args.Graphics,
            tile.ClientRectangle,
            tile.BackColor.ToArgb() == AccentSoft.ToArgb());

        var thumbnail = new PictureBox
        {
            Dock = DockStyle.Fill,
            Margin = new Padding(0),
            SizeMode = PictureBoxSizeMode.Zoom,
            BackColor = Color.FromArgb(248, 249, 244),
            Cursor = Cursors.Hand,
        };
        thumbnail.DoubleClick += (sender, _) => ShowFullscreenPreview(GetTileIndex((Control)sender!));
        tile.Controls.Add(thumbnail);

        var state = new ThumbnailTileState(thumbnail);
        tile.Tag = state;
        UpdateThumbnailTile(tile, state, image, index, isSelected, cancellationToken);

        tile.Click += (sender, _) => SelectImage(GetTileIndex((Control)sender!));
        thumbnail.Click += (sender, _) => SelectImage(GetTileIndex((Control)sender!));
        return tile;
    }

    private void UpdateThumbnailTile(
        Panel tile,
        ThumbnailTileState state,
        ScannerImageSelection image,
        int index,
        bool isSelected,
        CancellationToken cancellationToken)
    {
        state.Image = image;
        state.Index = index;
        state.IsSelected = isSelected;

        tile.BackColor = isSelected ? AccentSoft : Color.White;
        if (isSelected)
        {
            AddThumbnailActionControls(tile, index);
        }
        else
        {
            RemoveThumbnailActionControls(tile);
        }

        EnsureThumbnailImage(state, image, ThumbnailImageSize, cancellationToken);
        tile.Invalidate();
    }

    private static int GetTileIndex(Control control)
    {
        for (Control? current = control; current is not null; current = current.Parent)
        {
            if (current.Tag is ThumbnailTileState state)
            {
                return state.Index;
            }
        }

        return -1;
    }

    private void EnsureThumbnailImage(
        ThumbnailTileState state,
        ScannerImageSelection image,
        Size size,
        CancellationToken cancellationToken)
    {
        if (!TryCreateThumbnailCacheKey(image, size, out ThumbnailCacheKey cacheKey))
        {
            state.ThumbnailKey = null;
            state.HasLoadedThumbnail = true;
            state.ThumbnailRequestId++;
            ReplacePictureBoxImage(state.PictureBox, CreateFallbackThumbnail(size, "无法预览"));
            return;
        }

        if (state.ThumbnailKey is ThumbnailCacheKey currentKey &&
            currentKey.Equals(cacheKey) &&
            state.HasLoadedThumbnail)
        {
            return;
        }

        state.ThumbnailKey = cacheKey;
        state.HasLoadedThumbnail = false;
        int requestId = ++state.ThumbnailRequestId;

        Image? cached = thumbnailCache.TryClone(cacheKey);
        if (cached is not null)
        {
            state.HasLoadedThumbnail = true;
            ReplacePictureBoxImage(state.PictureBox, cached);
            return;
        }

        ReplacePictureBoxImage(state.PictureBox, CreateFallbackThumbnail(size, "加载中"));
        Task<Bitmap> loadTask = Task.Run(() => CreateThumbnailForCache(image, size, cancellationToken));
        _ = loadTask.ContinueWith(
            task => CompleteThumbnailLoad(task, state, cacheKey, requestId, size, cancellationToken),
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    private void CompleteThumbnailLoad(
        Task<Bitmap> task,
        ThumbnailTileState state,
        ThumbnailCacheKey cacheKey,
        int requestId,
        Size size,
        CancellationToken cancellationToken)
    {
        bool failed = false;
        if (task.Status == TaskStatus.RanToCompletion)
        {
            Bitmap thumbnail = task.Result;
            if (cancellationToken.IsCancellationRequested)
            {
                thumbnail.Dispose();
                return;
            }

            thumbnailCache.Store(cacheKey, thumbnail);
        }
        else
        {
            failed = !task.IsCanceled && !cancellationToken.IsCancellationRequested;
            if (task.IsFaulted)
            {
                _ = task.Exception;
            }
        }

        if (cancellationToken.IsCancellationRequested)
        {
            return;
        }

        try
        {
            BeginInvoke((MethodInvoker)(() =>
                ApplyThumbnailLoadResult(state, cacheKey, requestId, size, failed)));
        }
        catch (InvalidOperationException)
        {
        }
    }

    private void ApplyThumbnailLoadResult(
        ThumbnailTileState state,
        ThumbnailCacheKey cacheKey,
        int requestId,
        Size size,
        bool failed)
    {
        if (IsDisposed ||
            state.PictureBox.IsDisposed ||
            state.ThumbnailRequestId != requestId ||
            state.ThumbnailKey is not ThumbnailCacheKey currentKey ||
            !currentKey.Equals(cacheKey))
        {
            return;
        }

        state.HasLoadedThumbnail = true;
        Image thumbnail = failed
            ? CreateFallbackThumbnail(size, "无法预览")
            : thumbnailCache.TryClone(cacheKey) ?? CreateFallbackThumbnail(size, "无法预览");
        ReplacePictureBoxImage(state.PictureBox, thumbnail);
    }

    private void AddThumbnailActionControls(Panel tile, int index)
    {
        RemoveThumbnailActionControls(tile);
        int totalCount;
        lock (stateLock)
        {
            totalCount = selectedImages.Count;
        }

        const int edgeInset = 12;
        const int verticalInset = 12;

        var moveLeft = BuildMoveButton("‹", index, -1, enabled: index > 0);
        moveLeft.Location = new Point(edgeInset, 72);
        tile.Controls.Add(moveLeft);
        moveLeft.BringToFront();

        var moveRight = BuildMoveButton("›", index, 1, enabled: index < totalCount - 1);
        moveRight.Location = new Point(tile.Width - moveRight.Width - edgeInset, 72);
        tile.Controls.Add(moveRight);
        moveRight.BringToFront();

        var rotateLeft = BuildTileActionButton("↶", "左转 90°", index, () => RotateImage(index, -90));
        rotateLeft.Location = new Point(edgeInset, tile.Height - rotateLeft.Height - verticalInset);
        tile.Controls.Add(rotateLeft);
        rotateLeft.BringToFront();

        var rotateRight = BuildTileActionButton("↷", "右转 90°", index, () => RotateImage(index, 90));
        rotateRight.Location = new Point(tile.Width - rotateRight.Width - edgeInset, tile.Height - rotateRight.Height - verticalInset);
        tile.Controls.Add(rotateRight);
        rotateRight.BringToFront();

        var deleteButton = BuildTileActionButton("×", "删除", index, () => RemoveImage(index), isDanger: true);
        deleteButton.Location = new Point(tile.Width - deleteButton.Width - 10, 12);
        tile.Controls.Add(deleteButton);
        deleteButton.BringToFront();
    }

    private static void RemoveThumbnailActionControls(Panel tile)
    {
        foreach (Control control in tile.Controls.Cast<Control>().Where(control => control is not PictureBox).ToArray())
        {
            tile.Controls.Remove(control);
            control.Dispose();
        }
    }

    private Button BuildMoveButton(string text, int index, int direction, bool enabled)
    {
        var button = new Button
        {
            Width = 28,
            Height = 42,
            Text = text,
            Enabled = enabled,
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.FromArgb(238, 245, 240),
            ForeColor = Color.FromArgb(35, 57, 49),
            Font = new Font(Font.FontFamily, 18F, FontStyle.Bold),
            Padding = new Padding(0),
            Margin = new Padding(0),
            Cursor = Cursors.Hand,
            TabStop = false,
        };
        button.FlatAppearance.BorderColor = BorderColor;
        button.FlatAppearance.BorderSize = 1;
        button.Click += (_, _) => MoveImage(index, direction);
        return button;
    }

    private Button BuildTileActionButton(string text, string tooltip, int index, Action action, bool isDanger = false)
    {
        var button = new Button
        {
            Width = 34,
            Height = 30,
            Text = isDanger ? string.Empty : text,
            TextAlign = ContentAlignment.MiddleCenter,
            FlatStyle = FlatStyle.Flat,
            BackColor = isDanger ? Color.FromArgb(255, 246, 242) : Color.FromArgb(238, 245, 240),
            ForeColor = isDanger ? Color.FromArgb(151, 68, 49) : Color.FromArgb(35, 57, 49),
            Font = new Font(Font.FontFamily, 14F, FontStyle.Bold),
            Padding = new Padding(0),
            Margin = new Padding(0),
            Cursor = Cursors.Hand,
            TabStop = false,
            Tag = index,
        };
        button.FlatAppearance.BorderColor = isDanger ? Color.FromArgb(231, 199, 186) : BorderColor;
        button.FlatAppearance.BorderSize = 1;
        if (isDanger)
        {
            button.Paint += (_, args) => TextRenderer.DrawText(
                args.Graphics,
                text,
                button.Font,
                button.ClientRectangle,
                button.ForeColor,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine);
        }

        button.Click += (_, _) => action();
        toolTip.SetToolTip(button, tooltip);
        return button;
    }

    private static void DrawThumbnailBorder(Graphics graphics, Rectangle bounds, bool isSelected)
    {
        graphics.SmoothingMode = SmoothingMode.AntiAlias;
        using var pen = new Pen(isSelected ? AccentColor : BorderColor, isSelected ? 2F : 1F);
        var border = Rectangle.Inflate(bounds, -1, -1);
        graphics.DrawRectangle(pen, border);
    }

    private void SelectImage(int index)
    {
        int previousIndex;
        bool changed = false;
        lock (stateLock)
        {
            previousIndex = selectedImageIndex;
            if (index >= 0 && index < selectedImages.Count && selectedImageIndex != index)
            {
                selectedImageIndex = index;
                changed = true;
            }
        }

        if (changed)
        {
            UpdateThumbnailSelection(previousIndex, isSelected: false);
            UpdateThumbnailSelection(index, isSelected: true);
            UpdateStatus();
        }
    }

    private void UpdateThumbnailSelection(int index, bool isSelected)
    {
        if (index < 0 || index >= thumbnailStrip.Controls.Count)
        {
            return;
        }

        if (thumbnailStrip.Controls[index] is not Panel tile)
        {
            return;
        }

        tile.SuspendLayout();
        tile.BackColor = isSelected ? AccentSoft : Color.White;
        if (tile.Tag is ThumbnailTileState state)
        {
            state.Index = index;
            state.IsSelected = isSelected;
        }

        if (isSelected)
        {
            AddThumbnailActionControls(tile, index);
        }
        else
        {
            RemoveThumbnailActionControls(tile);
        }

        tile.ResumeLayout();
        tile.Invalidate();
    }

    private void ShowFullscreenPreview(int index)
    {
        ScannerImageSelection? image;
        int previousIndex;
        bool changed;
        lock (stateLock)
        {
            if (index < 0 || index >= selectedImages.Count)
            {
                return;
            }

            image = selectedImages[index];
            previousIndex = selectedImageIndex;
            selectedImageIndex = index;
            changed = previousIndex != index;
        }

        if (changed)
        {
            UpdateThumbnailSelection(previousIndex, isSelected: false);
            UpdateThumbnailSelection(index, isSelected: true);
            UpdateStatus();
        }

        using Bitmap preview = LoadPreparedImage(image.Path, image.RotationDegrees);
        using var dialog = new Form
        {
            Text = Path.GetFileName(image.Path),
            WindowState = FormWindowState.Maximized,
            StartPosition = FormStartPosition.CenterParent,
            BackColor = Color.Black,
            KeyPreview = true,
        };
        using var previewCopy = new Bitmap(preview);
        var viewer = new PictureBox
        {
            Dock = DockStyle.Fill,
            BackColor = Color.Black,
            SizeMode = PictureBoxSizeMode.Zoom,
            Image = previewCopy,
        };
        dialog.Controls.Add(viewer);
        dialog.KeyDown += (_, args) =>
        {
            if (args.KeyCode == Keys.Escape || args.KeyCode == Keys.Enter)
            {
                dialog.Close();
            }
        };
        viewer.DoubleClick += (_, _) => dialog.Close();
        dialog.ShowDialog(this);
        viewer.Image = null;
    }

    private void UpdateStatus()
    {
        ScannerStateSnapshot snapshot = GetSnapshot();
        int activePage;
        lock (stateLock)
        {
            activePage = selectedImageIndex + 1;
        }

        string scanState = snapshot.ScanRequested ? "等待 TWAIN 程序取图" : "等待选择图片";
        string activeText = snapshot.SelectedImages.Count == 0 || activePage <= 0
            ? "未选中页面"
            : $"当前第 {activePage}/{snapshot.SelectedImages.Count} 页";
        string updateText = latestRelease is { IsNewer: true } release
            ? $" | 新版本 {release.TagName}"
            : string.Empty;
        statusLabel.Text =
            $"{scanState} | 队列 {snapshot.SelectedImages.Count} | {activeText} | {snapshot.PixelType} | {snapshot.PaperSize} | {snapshot.XResolution}x{snapshot.YResolution} DPI | 管道 {ScannerPipeServer.PipeName}{updateText}";
    }

    private static int NormalizeRotationDegrees(int rotationDegrees)
    {
        int normalized = rotationDegrees % 360;
        return normalized < 0 ? normalized + 360 : normalized;
    }

    private static void DisposeControlImages(Control control)
    {
        foreach (Control child in control.Controls)
        {
            DisposeControlImages(child);
        }

        if (control is PictureBox pictureBox)
        {
            DisposePictureBoxImage(pictureBox);
        }
    }

    private static void DisposePictureBoxImage(PictureBox pictureBox)
    {
        Image? image = pictureBox.Image;
        pictureBox.Image = null;
        image?.Dispose();
    }

    private static void ReplacePictureBoxImage(PictureBox pictureBox, Image image)
    {
        Image? previous = pictureBox.Image;
        pictureBox.Image = image;
        previous?.Dispose();
    }

    private static bool TryCreateThumbnailCacheKey(
        ScannerImageSelection image,
        Size size,
        out ThumbnailCacheKey cacheKey)
    {
        try
        {
            var file = new FileInfo(image.Path);
            if (!file.Exists)
            {
                cacheKey = default;
                return false;
            }

            cacheKey = new ThumbnailCacheKey(
                Path.GetFullPath(file.FullName).ToUpperInvariant(),
                file.LastWriteTimeUtc.Ticks,
                file.Length,
                NormalizeRotationDegrees(image.RotationDegrees),
                size.Width,
                size.Height);
            return true;
        }
        catch
        {
            cacheKey = default;
            return false;
        }
    }

    private static Bitmap LoadPreparedImage(string path, int rotationDegrees)
        => LoadPreparedImage(path, rotationDegrees, MaxPreviewImagePixels);

    private static Bitmap LoadPreparedImage(string path, int rotationDegrees, int maxPixels)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var source = Image.FromStream(stream, useEmbeddedColorManagement: true, validateImageData: true);
        using Bitmap scaled = CopyImageWithinPixelLimit(source, maxPixels);
        var bitmap = new Bitmap(scaled);
        ApplyExifOrientation(bitmap);
        ApplyRotation(bitmap, rotationDegrees);
        return bitmap;
    }

    private static Bitmap CreateThumbnailForCache(
        ScannerImageSelection image,
        Size size,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        using Bitmap bitmap = LoadPreparedImage(image.Path, image.RotationDegrees, MaxThumbnailSourcePixels);
        cancellationToken.ThrowIfCancellationRequested();
        return RenderThumbnail(bitmap, size);
    }

    private static Bitmap CopyImageWithinPixelLimit(Image source, int maxPixels)
    {
        Size targetSize = FitWithinPixelLimit(source.Size, maxPixels);
        if (targetSize == source.Size)
        {
            return new Bitmap(source);
        }

        var bitmap = new Bitmap(targetSize.Width, targetSize.Height);
        using var graphics = Graphics.FromImage(bitmap);
        graphics.Clear(Color.White);
        graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
        graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
        graphics.SmoothingMode = SmoothingMode.HighQuality;
        graphics.DrawImage(source, new Rectangle(Point.Empty, targetSize));
        return bitmap;
    }

    private static Size FitWithinPixelLimit(Size sourceSize, int maxPixels)
    {
        if (sourceSize.Width <= 0 || sourceSize.Height <= 0)
        {
            return new Size(1, 1);
        }

        long pixelCount = (long)sourceSize.Width * sourceSize.Height;
        if (pixelCount <= maxPixels)
        {
            return sourceSize;
        }

        double scale = Math.Sqrt(maxPixels / (double)pixelCount);
        return new Size(
            Math.Max(1, (int)Math.Floor(sourceSize.Width * scale)),
            Math.Max(1, (int)Math.Floor(sourceSize.Height * scale)));
    }

    private static void ApplyExifOrientation(Image image)
    {
        const int ExifOrientationId = 0x0112;
        if (!image.PropertyIdList.Contains(ExifOrientationId))
        {
            return;
        }

        try
        {
            PropertyItem? property = image.GetPropertyItem(ExifOrientationId);
            if (property?.Value is null || property.Value.Length < 2)
            {
                return;
            }

            ushort orientation = BitConverter.ToUInt16(property.Value, 0);
            image.RotateFlip(orientation switch
            {
                2 => RotateFlipType.RotateNoneFlipX,
                3 => RotateFlipType.Rotate180FlipNone,
                4 => RotateFlipType.Rotate180FlipX,
                5 => RotateFlipType.Rotate90FlipX,
                6 => RotateFlipType.Rotate90FlipNone,
                7 => RotateFlipType.Rotate270FlipX,
                8 => RotateFlipType.Rotate270FlipNone,
                _ => RotateFlipType.RotateNoneFlipNone,
            });
        }
        catch
        {
        }
    }

    private static void ApplyRotation(Image image, int rotationDegrees)
    {
        image.RotateFlip(NormalizeRotationDegrees(rotationDegrees) switch
        {
            90 => RotateFlipType.Rotate90FlipNone,
            180 => RotateFlipType.Rotate180FlipNone,
            270 => RotateFlipType.Rotate270FlipNone,
            _ => RotateFlipType.RotateNoneFlipNone,
        });
    }

    private static Bitmap RenderThumbnail(Image source, Size targetSize)
    {
        var thumbnail = new Bitmap(targetSize.Width, targetSize.Height);
        using var graphics = Graphics.FromImage(thumbnail);
        graphics.Clear(Color.FromArgb(247, 248, 242));
        graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
        graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
        graphics.SmoothingMode = SmoothingMode.HighQuality;

        Rectangle destination = FitWithin(source.Size, targetSize);
        graphics.DrawImage(source, destination);
        return thumbnail;
    }

    private static Bitmap CreateFallbackThumbnail(Size size, string text)
    {
        var thumbnail = new Bitmap(size.Width, size.Height);
        using var graphics = Graphics.FromImage(thumbnail);
        graphics.Clear(Color.FromArgb(245, 246, 240));
        using var pen = new Pen(BorderColor);
        graphics.DrawRectangle(pen, 0, 0, size.Width - 1, size.Height - 1);
        TextRenderer.DrawText(
            graphics,
            text,
            SystemFonts.MessageBoxFont,
            new Rectangle(6, 6, size.Width - 12, size.Height - 12),
            MutedTextColor,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
        return thumbnail;
    }

    private static Rectangle FitWithin(Size source, Size target)
    {
        if (source.Width <= 0 || source.Height <= 0)
        {
            return new Rectangle(0, 0, target.Width, target.Height);
        }

        float ratio = Math.Min(target.Width / (float)source.Width, target.Height / (float)source.Height);
        int width = Math.Max(1, (int)Math.Round(source.Width * ratio));
        int height = Math.Max(1, (int)Math.Round(source.Height * ratio));
        int left = (target.Width - width) / 2;
        int top = (target.Height - height) / 2;
        return new Rectangle(left, top, width, height);
    }

    private sealed class ThumbnailTileState(PictureBox pictureBox)
    {
        public PictureBox PictureBox { get; } = pictureBox;

        public ScannerImageSelection? Image { get; set; }

        public int Index { get; set; }

        public bool IsSelected { get; set; }

        public ThumbnailCacheKey? ThumbnailKey { get; set; }

        public int ThumbnailRequestId { get; set; }

        public bool HasLoadedThumbnail { get; set; }
    }
}
