# mbfTwain v1.0.1

这是 `v1.0.0` 的更新流程修复版本。

## 修复内容

- 修复 UI 首次打开时自动检查 GitHub Release 可能卡住首帧渲染的问题。
- 自动更新检查延迟到窗口显示后启动，并完整运行在后台任务中。
- 手动“检查更新”和安装包下载也改为后台执行，只把状态文字更新切回 UI 线程。
- GitHub API 和下载路径不再捕获 WinForms UI 同步上下文。

## 安装包

下载安装资产：

- `mbfTwain-Setup-v1.0.1.exe`
- `mbfTwain-Setup-v1.0.1.exe.sha256`

安装器需要管理员权限，因为 TWAIN source 需要写入 Windows 的 TWAIN 目录。
如果仓库保持私有，UI 更新检查需要在启动前设置可读取 release 的
`MBF_TWAIN_GITHUB_TOKEN`；公开仓库不需要令牌。
