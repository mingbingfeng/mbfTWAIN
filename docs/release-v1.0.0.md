# mbfTwain v1.0.0

首个自动发布版本，包含本地 Inno Setup 安装包、GitHub Release 发布资产，以及 UI 内置更新检查。

## 包含内容

- `mbfVirtualTwainDS.ds` 32 位版本安装到 `C:\Windows\twain_32`
- `mbfVirtualTwainDS.ds` 64 位版本安装到 `C:\Windows\twain_64`
- `mbfTwain.VirtualScannerConfig.*` UI 运行文件随 DS 一起安装
- 安装器写入机器环境变量 `MBF_TWAIN_FORCE_UI=1`
- 配置 UI 可从 GitHub Releases 检查新版本、下载安装包并提权启动安装器

## 安装包

下载安装资产：

- `mbfTwain-Setup-v1.0.0.exe`
- `mbfTwain-Setup-v1.0.0.exe.sha256`

安装器需要管理员权限，因为 TWAIN source 需要写入 Windows 的 TWAIN 目录。
