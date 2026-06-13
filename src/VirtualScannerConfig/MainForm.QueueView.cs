using System.Drawing;
using System.Windows.Forms;

namespace MbfTwain.VirtualScannerConfig;

public sealed partial class MainForm
{
    private Control BuildQueueSurface()
    {
        var queueCard = BuildSurfacePanel();

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            Padding = new Padding(18, 16, 18, 16),
            BackColor = SurfaceBackground,
        };
        layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.Controls.Add(BuildQueueHeader(), 0, 0);

        thumbnailStrip.Dock = DockStyle.Fill;
        thumbnailStrip.AutoScroll = true;
        thumbnailStrip.WrapContents = true;
        thumbnailStrip.FlowDirection = FlowDirection.LeftToRight;
        thumbnailStrip.Margin = new Padding(0, 18, 0, 0);
        thumbnailStrip.Padding = new Padding(0, 0, 4, 4);
        thumbnailStrip.BackColor = SurfaceBackground;
        layout.Controls.Add(thumbnailStrip, 0, 1);

        queueCard.Controls.Add(layout);
        return queueCard;
    }

    private Control BuildQueueHeader()
    {
        var header = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            ColumnCount = 3,
            RowCount = 2,
            AutoSize = true,
            BackColor = SurfaceBackground,
            Margin = new Padding(0),
        };
        header.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        header.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        header.RowStyles.Add(new RowStyle(SizeType.AutoSize));

        header.Controls.Add(
            new Label
            {
                AutoSize = true,
                Text = "待扫描图片",
                Font = new Font(Control.DefaultFont, FontStyle.Bold),
                ForeColor = Color.FromArgb(33, 43, 37),
                Margin = new Padding(0, 0, 12, 0),
            },
            0,
            0);

        ConfigureClearImagesLink();
        header.Controls.Add(clearImagesLink, 1, 0);

        var triggerButton = new Button
        {
            Text = "开始扫描",
            Width = 112,
            Height = 46,
            FlatStyle = FlatStyle.Flat,
            BackColor = AccentColor,
            ForeColor = Color.White,
            Font = new Font(Font, FontStyle.Bold),
            Margin = new Padding(12, 0, 0, 0),
            Anchor = AnchorStyles.Top | AnchorStyles.Right,
        };
        triggerButton.FlatAppearance.BorderSize = 0;
        triggerButton.Click += (_, _) => TriggerScan();
        header.Controls.Add(triggerButton, 2, 0);
        header.SetRowSpan(triggerButton, 2);

        var subtitle = new Label
        {
            AutoSize = true,
            Text = "用缩略图两侧箭头调整顺序，列表顺序就是扫描传输顺序。",
            ForeColor = MutedTextColor,
            Margin = new Padding(0, 6, 0, 0),
        };
        header.Controls.Add(subtitle, 0, 1);
        header.SetColumnSpan(subtitle, 2);

        return header;
    }

    private void ConfigureClearImagesLink()
    {
        clearImagesLink.AutoSize = true;
        clearImagesLink.Text = "清空队列";
        clearImagesLink.LinkColor = Color.FromArgb(38, 100, 171);
        clearImagesLink.ActiveLinkColor = Color.FromArgb(24, 75, 132);
        clearImagesLink.VisitedLinkColor = clearImagesLink.LinkColor;
        clearImagesLink.Margin = new Padding(0, 2, 0, 0);
        clearImagesLink.LinkBehavior = LinkBehavior.AlwaysUnderline;
        clearImagesLink.LinkClicked += (_, _) => ClearImages();
    }

    private void RefreshQueueVisuals()
    {
        NormalizeSelection();
        RefreshThumbnailStrip();
        UpdateStatus();
    }
}
