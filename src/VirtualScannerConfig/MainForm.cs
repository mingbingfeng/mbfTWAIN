using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using MbfTwain.VirtualScannerConfig.Ipc;

namespace MbfTwain.VirtualScannerConfig;

public sealed class MainForm : Form
{
    private readonly object stateLock = new();
    private readonly List<string> selectedImages = new();
    private readonly ScannerPipeServer pipeServer;
    private bool suppressControlStateUpdate;

    private uint revision;
    private bool scanRequested;
    private bool duplexEnabled;
    private string pixelType = "RGB";
    private string paperSize = "A4";
    private int scanDpi = 300;

    private readonly ListBox imageList = new();
    private readonly CheckBox duplexCheckBox = new();
    private readonly ComboBox pixelTypeComboBox = new();
    private readonly ComboBox paperSizeComboBox = new();
    private readonly ComboBox dpiComboBox = new();
    private readonly Label statusLabel = new();

    public MainForm()
    {
        Text = "mbfTwain 虚拟扫描仪";
        MinimumSize = new Size(760, 520);
        StartPosition = FormStartPosition.CenterScreen;

        pipeServer = new ScannerPipeServer(GetSnapshot, BeginScanSession, AcknowledgeScan);
        BuildLayout();
        UpdateStateFromControls(incrementRevision: false);
        pipeServer.Start();
        UpdateStatus();
        DiagnosticsLog.Write("UI", "MainForm initialized");
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            pipeServer.Dispose();
        }

        base.Dispose(disposing);
    }

    private void BuildLayout()
    {
        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 3,
            Padding = new Padding(12),
        };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 62));
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 38));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        imageList.Dock = DockStyle.Fill;
        imageList.HorizontalScrollbar = true;
        root.Controls.Add(imageList, 0, 0);

        var settings = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 5,
            Padding = new Padding(12, 0, 0, 0),
        };
        settings.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        settings.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

        duplexCheckBox.Text = "双面扫描";
        duplexCheckBox.AutoSize = true;
        duplexCheckBox.CheckedChanged += (_, _) => UpdateStateFromControls(incrementRevision: true);
        settings.Controls.Add(duplexCheckBox, 0, 0);
        settings.SetColumnSpan(duplexCheckBox, 2);

        pixelTypeComboBox.DropDownStyle = ComboBoxStyle.DropDownList;
        pixelTypeComboBox.Items.AddRange(new object[] { "BW", "GRAY", "RGB" });
        pixelTypeComboBox.SelectedItem = "RGB";
        pixelTypeComboBox.SelectedIndexChanged += (_, _) => UpdateStateFromControls(incrementRevision: true);
        AddLabeledControl(settings, "像素类型", pixelTypeComboBox, row: 1);

        paperSizeComboBox.DropDownStyle = ComboBoxStyle.DropDownList;
        paperSizeComboBox.Items.AddRange(new object[] { "A4", "A3" });
        paperSizeComboBox.SelectedItem = "A4";
        paperSizeComboBox.SelectedIndexChanged += (_, _) => UpdateStateFromControls(incrementRevision: true);
        AddLabeledControl(settings, "纸张类型", paperSizeComboBox, row: 2);

        ConfigureDpiInput(dpiComboBox);
        dpiComboBox.SelectedItem = 300;
        dpiComboBox.SelectedIndexChanged += (_, _) => UpdateStateFromControls(incrementRevision: true);
        AddLabeledControl(settings, "DPI", dpiComboBox, row: 3);

        var triggerButton = new Button
        {
            Text = "开始扫描",
            Dock = DockStyle.Top,
            Height = 36,
        };
        triggerButton.Click += (_, _) => TriggerScan();
        settings.Controls.Add(triggerButton, 0, 4);
        settings.SetColumnSpan(triggerButton, 2);

        root.Controls.Add(settings, 1, 0);

        var imageButtons = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            AutoSize = true,
        };
        imageButtons.Controls.Add(BuildButton("添加图片", AddImages));
        imageButtons.Controls.Add(BuildButton("移除选中", RemoveSelectedImages));
        imageButtons.Controls.Add(BuildButton("清空", ClearImages));
        root.Controls.Add(imageButtons, 0, 1);

        statusLabel.AutoSize = true;
        statusLabel.Dock = DockStyle.Fill;
        root.Controls.Add(statusLabel, 0, 2);
        root.SetColumnSpan(statusLabel, 2);

        Controls.Add(root);
    }

    private static void AddLabeledControl(TableLayoutPanel panel, string labelText, Control control, int row)
    {
        var label = new Label
        {
            Text = labelText,
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            Padding = new Padding(0, 6, 8, 0),
        };
        control.Dock = DockStyle.Top;
        panel.Controls.Add(label, 0, row);
        panel.Controls.Add(control, 1, row);
    }

    private static void ConfigureDpiInput(ComboBox input)
    {
        input.DropDownStyle = ComboBoxStyle.DropDownList;
        input.Items.AddRange(new object[] { 150, 200, 300, 600 });
    }

    private static Button BuildButton(string text, Action action)
    {
        var button = new Button
        {
            Text = text,
            AutoSize = true,
            Height = 32,
            Margin = new Padding(0, 0, 8, 0),
        };
        button.Click += (_, _) => action();
        return button;
    }

    private void AddImages()
    {
        using var dialog = new OpenFileDialog
        {
            Filter = "图片文件|*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff|所有文件|*.*",
            Multiselect = true,
            Title = "选择待扫描图片",
        };

        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return;
        }

        lock (stateLock)
        {
            foreach (string fileName in dialog.FileNames.Where(File.Exists))
            {
                if (!selectedImages.Contains(fileName, StringComparer.OrdinalIgnoreCase))
                {
                    selectedImages.Add(fileName);
                }
            }

            revision++;
        }

        RefreshImageList();
        UpdateStatus();
    }

    private void RemoveSelectedImages()
    {
        var selected = imageList.SelectedItems.Cast<string>().ToList();
        if (selected.Count == 0)
        {
            return;
        }

        lock (stateLock)
        {
            foreach (string item in selected)
            {
                selectedImages.Remove(item);
            }

            revision++;
        }

        RefreshImageList();
        UpdateStatus();
    }

    private void ClearImages()
    {
        lock (stateLock)
        {
            selectedImages.Clear();
            scanRequested = false;
            revision++;
        }

        RefreshImageList();
        UpdateStatus();
    }

    private void TriggerScan()
    {
        lock (stateLock)
        {
            if (selectedImages.Count > 0)
            {
                scanRequested = true;
                revision++;
                UpdateStatus();
                return;
            }
        }

        AddImages();

        lock (stateLock)
        {
            scanRequested = selectedImages.Count > 0;
            if (scanRequested)
            {
                revision++;
            }
        }

        UpdateStatus();
    }

    private void UpdateStateFromControls(bool incrementRevision)
    {
        if (suppressControlStateUpdate)
        {
            return;
        }

        lock (stateLock)
        {
            duplexEnabled = duplexCheckBox.Checked;
            pixelType = pixelTypeComboBox.SelectedItem?.ToString() ?? "RGB";
            paperSize = paperSizeComboBox.SelectedItem?.ToString() ?? "A4";
            scanDpi = Convert.ToInt32(dpiComboBox.SelectedItem ?? 300);
            if (incrementRevision)
            {
                revision++;
            }
        }

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
            $"BeginScanSession requested disposed={IsDisposed} invokeRequired={InvokeRequired} handleCreated={IsHandleCreated} paper={sessionSettings?.PaperSize ?? "<none>"}");
        if (IsDisposed)
        {
            return;
        }

        void Apply()
        {
            lock (stateLock)
            {
                if (sessionSettings is not null)
                {
                    ApplySessionSettings(sessionSettings);
                }

                selectedImages.Clear();
                scanRequested = false;
                revision++;
            }

            DiagnosticsLog.Write("UI", $"BeginScanSession.Apply revision={revision}");
            RefreshImageList();
            if (WindowState == FormWindowState.Minimized)
            {
                WindowState = FormWindowState.Normal;
            }

            Show();
            Activate();
            UpdateStatus();
            DiagnosticsLog.Write("UI", "BeginScanSession.Apply completed and window activated");
        }

        if (InvokeRequired && IsHandleCreated)
        {
            BeginInvoke((MethodInvoker)Apply);
        }
        else
        {
            Apply();
        }
    }

    private void ApplySessionSettings(ScannerSessionSettings settings)
    {
        duplexEnabled = settings.DuplexEnabled;

        if (pixelTypeComboBox.Items.Contains(settings.PixelType))
        {
            pixelType = settings.PixelType;
        }

        if (paperSizeComboBox.Items.Contains(settings.PaperSize))
        {
            paperSize = settings.PaperSize;
        }

        if (dpiComboBox.Items.Contains(settings.XResolution))
        {
            scanDpi = settings.XResolution;
        }

        suppressControlStateUpdate = true;
        try
        {
            duplexCheckBox.Checked = duplexEnabled;
            pixelTypeComboBox.SelectedItem = pixelType;
            paperSizeComboBox.SelectedItem = paperSize;
            dpiComboBox.SelectedItem = scanDpi;
        }
        finally
        {
            suppressControlStateUpdate = false;
        }
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
                revision++;
                acknowledged = true;
            }
        }

        if (!acknowledged)
        {
            DiagnosticsLog.Write("UI", "AcknowledgeScan ignored because current state did not match");
            return;
        }

        BeginInvoke((MethodInvoker)(() =>
        {
            RefreshImageList();
            UpdateStatus();
            Hide();
            DiagnosticsLog.Write("UI", "AcknowledgeScan applied and window hidden");
        }));
    }

    private void RefreshImageList()
    {
        imageList.BeginUpdate();
        imageList.Items.Clear();
        lock (stateLock)
        {
            foreach (string image in selectedImages)
            {
                imageList.Items.Add(image);
            }
        }

        imageList.EndUpdate();
    }

    private void UpdateStatus()
    {
        ScannerStateSnapshot snapshot = GetSnapshot();
        string scanState = snapshot.ScanRequested ? "等待 TWAIN 程序取图" : "等待选择图片";
        statusLabel.Text =
            $"{scanState} | 图片 {snapshot.SelectedImages.Count} | {snapshot.PixelType} | {snapshot.PaperSize} | {snapshot.XResolution}x{snapshot.YResolution} DPI | 管道 {ScannerPipeServer.PipeName}";
    }
}
