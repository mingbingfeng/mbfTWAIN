# mbfTwain v1.0.2

这是 `v1.0.1` 的扫描传输体验与 TWAIN 兼容性增强版本。

## 新增内容

- UI 新增送图进度窗口，可在 TWAIN 宿主取图期间显示当前页数并允许停止扫描。
- 设置中新增送图缓冲间隔，DS 会在 Native 和 Memory 传输过程中按配置短暂停顿，默认 100ms，最大 5000ms。
- DS 新增 `DAT_EXTIMAGEINFO` 支持，提供文档号、页号、页侧、帧号和纸张总数等扩展图像信息。
- IPC 协议新增 `TRANSFER_PROGRESS` 命令和 `transferDelayMs` 状态字段。

## 修复内容

- 当 UI 端停止扫描或清空队列时，DS 会及时收敛剩余传输，避免继续向宿主送出已取消的页面。
- smoke 工具覆盖扩展图像信息、送图进度命令和缓冲间隔状态，降低回归风险。

## 安装包

下载安装资产：

- `mbfTwain-Setup-v1.0.2.exe`
- `mbfTwain-Setup-v1.0.2.exe.sha256`

安装器需要管理员权限，因为 TWAIN source 需要写入 Windows 的 TWAIN 目录。
如果仓库保持私有，UI 更新检查需要在启动前设置可读取 release 的
`MBF_TWAIN_GITHUB_TOKEN`；公开仓库不需要令牌。
