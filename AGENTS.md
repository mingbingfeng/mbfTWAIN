# Agent Rules

- After any user-requested code change in this repository, rebuild the latest Win32 and x64 virtual TWAIN Data Source plus the UI, run the relevant smoke tests, and install the latest build locally with `.\Install-LocalTwain.ps1`.
- Local installation targets are `C:\Windows\twain_32` and `C:\Windows\twain_64`. The install must place both `mbfVirtualTwainDS.ds` and the `mbfTwain.VirtualScannerConfig.*` runtime files so the DS can launch the UI.
- If the current agent process cannot write to those directories or cannot set machine-level environment variables because it lacks Administrator/UAC permission, do not stop at a permissions failure. After build and smoke verification pass, try to launch the installer through UAC with `Start-Process -Verb RunAs -Wait`, redirecting output to `build\install-elevated.log`, for example:

```powershell
$log = Join-Path (Get-Location) 'build\install-elevated.log'
$command = "Set-Location -LiteralPath '$PWD'; .\Install-LocalTwain.ps1 *> '$log'"
Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-Command',$command)
```

- Only if UAC elevation is unavailable, canceled, or fails after the verified build/smoke phase, report the exact elevated PowerShell command the user must run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-LocalTwain.ps1
```

- After permission is available, verify the installed files and confirm `MBF_TWAIN_FORCE_UI=1` is set at machine scope unless the user explicitly asks not to force the UI.

---

# AI Aide 协作协议

- 通用协作协议已提炼为用户级 `$ai-aide` skill。
- 需要调用 Claude Code、Codex CLI 或其他 AI 执行助手时，先使用 `$ai-aide`；Codex 仍负责拆任务、决策、验收和最终汇报。
- 本仓库保留 `.codex_delegate/` 作为协作状态目录；详细任务包、状态 JSON、等待、重试和验收流程以 `$ai-aide` 为准。
- 本目录同时维护 `AGENTS.md` 和 `CLAUDE.md`，两者应保持同一套规则。

## TWAIN 参考实现索引

本次已下载/保留的参考项目路径：

- `E:\Project\mbfTwain\.codex_delegate\external\twain-samples`
- `E:\Project\mbfTwain\.codex_delegate\external\twain-dsm`
- `E:\Project\mbfTwain\.codex_delegate\external\virtual-scanner`

下次排查 TWAIN DS/DSM 问题时优先看这些位置，不要重新下载：

- TWAIN sample DS 启用流程：`.codex_delegate/external/twain-samples/TWAIN-Samples/Twain_DS_sample01/src/CTWAINDS_FreeImage.cpp`，重点 `enableDS` 附近，NoUI 模式会准备图像并调用 `DoXferReadyEvent()`。
- TWAIN sample DS 发送 `XFERREADY`：`.codex_delegate/external/twain-samples/TWAIN-Samples/Twain_DS_sample01/src/CTWAINDS_Base.cpp`，重点 `DoXferReadyEvent()`，Windows 路径调用 `_DSM_Entry(getIdentity(), getApp(), DG_CONTROL, DAT_NULL, MSG_XFERREADY, 0)`。
- TWAIN DSM `DAT_NULL` 分发：`.codex_delegate/external/twain-dsm/TWAIN_DSM/src/dsm.cpp`，重点 `DSM_Null` 附近，说明 `DAT_NULL` 用于 driver 向 application 发送 `MSG_XFERREADY` 等消息。
- TWAIN DSM callback 注册：`.codex_delegate/external/twain-dsm/TWAIN_DSM/src/dsm.cpp`，重点 `DAT_CALLBACK` / `DAT_CALLBACK2` / `MSG_REGISTER_CALLBACK` 分支；应用注册 callback 由 DSM 管理，DS 通常通过 `DAT_NULL` 触发。
- TWAIN DSM 关闭清理：`.codex_delegate/external/twain-dsm/TWAIN_DSM/src/apps.cpp`，重点关闭 DS 时 DSM 会尝试 `ENDXFER`、`RESET`、`DISABLEDS`、`CLOSEDS` 的清理序列。
- yushulx virtual-scanner 的 Windows 参考实际包含 TWAIN samples 镜像：`.codex_delegate/external/virtual-scanner/windows/TWAIN-Samples/...`；优先用上面的 `twain-samples` 路径。
