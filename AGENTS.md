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

# Codex 主控 + Claude Code 执行助手协作协议

> 本节用于约束主代理：由 Codex 负责理解需求、拆任务、调度 Claude Code、验收结果；由 Claude Code 负责机械重复、代码搜索、批量修改、初步测试等高 token 消耗工作。
>
> Codex 常见自动读取文件名是 `AGENTS.md`，Claude Code 常见自动读取文件名是 `CLAUDE.md`。本工作目录同时维护这两个文件，确保两类工具都能读取同一套协作规则。

## 总目标

- 用户只和 Codex 对话。
- Codex 必须在任务适合时自动调用 Claude Code，不要求用户在两者之间传话。
- 用户必须能看到 Codex 调用了 Claude Code，并能看到 Claude Code 的工作过程或工作日志位置。
- Claude Code 使用本机已配置的 baseurl 与模型路由，例如火山 Agentplan 自动匹配 `doubao-seed-2.0-code`、`glm-5.1`、`deepseek-v4-pro`、`kimi-k2.6` 等模型；Codex 不手动指定模型，除非用户明确要求。
- Codex 保留最终责任：需求澄清、任务边界、代码审查、测试结果解读、最终验收和对用户汇报。

## 角色分工

### Codex 主控

- 读懂用户目标，判断是否需要派发给 Claude Code。
- 拆分任务，给 Claude Code 一次性提供完整上下文、约束、允许修改范围、验收标准。
- 启动 Claude Code 后持续读取其日志或输出，必要时自动补充指令。
- 审查 Claude Code 的改动，不盲信其结论。
- 亲自运行或复核关键验证命令。
- 最终向用户汇报：是否调用了 Claude Code、Claude Code 做了什么、Codex 验收了什么、还有哪些风险。

### Claude Code 执行助手

- 只执行 Codex 派发的具体任务。
- 优先承担高 token 消耗、机械重复、局部批量修改、初步定位、初步测试、日志整理工作。
- 不直接向用户提问；遇到问题写入交接报告，由 Codex 决策。
- 不扩大修改范围，不做无关重构，不新增依赖，除非 Codex 明确授权。
- 完成后必须输出结构化交接报告。

## 什么时候必须优先考虑调用 Claude Code

满足任一条件时，Codex 应优先考虑派发给 Claude Code：

- 需要跨多个文件搜索、统计、整理引用关系。
- 需要批量机械修改、格式统一、重复样板替换。
- 需要读取长日志、长构建输出、长测试输出并提取关键信息。
- 需要先做一轮候选方案或影响范围摸底。
- 需要节省 Codex 上下文窗口，且子任务边界清晰。

以下情况 Codex 应直接处理，通常不派发：

- 用户只问一个概念性问题。
- 单文件、小范围、低风险修改。
- 涉及密钥、凭据、账号、私有配置泄露风险。
- 需要产品判断、需求取舍、用户偏好确认。
- 需要破坏性操作，例如删除大量文件、重置分支、强推、清库。

## 可见性要求

Codex 每次调用 Claude Code 前，必须先向用户发一条简短进度说明，格式建议：

```text
我会把「<子任务名称>」交给 Claude Code 处理，并打开可视化聊天窗口；任务包保存到 <prompt 路径>，截图保存到 <screenshot 路径>；完成后我会审查 diff 和验证结果。
```

Claude Code 的每次执行必须落盘到当前仓库内：

```text
.codex_delegate/
  prompts/       # Codex 发给 Claude Code 的任务包
  screenshots/   # 可视化聊天窗口截图
  reports/       # Claude Code 最终交接报告
  state/         # session id、任务状态、重试信息
  logs/          # 仅保存轻量启动记录或 stderr；禁止把 stream-json 当作主流程
```

Codex 最终报告必须包含：

- 是否调用 Claude Code。
- 调用次数、任务包路径、截图路径、报告路径。
- Claude Code 的核心结论。
- Codex 自己完成的验收。
- 未验证或残余风险。

## Claude Code 调用方式

默认且唯一默认模式：可视化聊天窗口模式。

默认启动命令：

```powershell
cd E:\Project\mbfTwain
claude --dangerously-skip-permissions
```

- 任务包仍写入 `.codex_delegate/prompts/<task-id>.md`，并在窗口可输入后粘贴提交。
- 默认不要使用 `claude -p`、`--output-format stream-json`、`--include-partial-messages`、`--include-hook-events`。
- 默认不要读取 `.stream.jsonl`、debug log 或其它长原始日志；主线程只读取短交接报告、截图、diff 和必要验证输出。
- 如果当前环境需要自定义命令，`CLAUDE_DELEGATE_EXE` / `CLAUDE_DELEGATE_ARGS` 只有在仍然打开可视化 TUI 时才允许生效；若它们会切回 print/json 模式，应忽略，除非用户明确撤销本仓库规则。
- 保留一条兼容性知识：如果未来用户显式要求恢复旧的 `-p --output-format stream-json` 路径，必须同时带上 `--verbose`，否则 Claude Code CLI 会直接报错 `When using --print, --output-format=stream-json requires --verbose`。

## Claude Code 可视化聊天模式

本仓库中，Claude Code 委托默认就使用可视化聊天模式，不需要用户额外提醒。

可视化模式要求：

- 使用正常 Claude Code 聊天界面，不使用 `--output-format stream-json`。
- 任务包仍写入 `.codex_delegate/prompts/<task-id>.md`，并把该任务包完整复制到剪贴板。
- 启动 Claude Code 后必须等到输入框可用，再执行粘贴和回车；不能只因为剪贴板里有提示词就认为已经提交。
- 粘贴后要验证成功：要么截图能看到提示词已进入输入框或已提交，要么终端已有 Claude 的响应。若自动粘贴失败，重新聚焦窗口并再次发送 `Ctrl+V`、`Enter`，仍失败则改用粘贴菜单或手动可见输入，不要静默继续。
- 可见窗口截图保存到 `.codex_delegate/screenshots/`，最终汇报给出任务包路径、截图路径和报告路径。
- 禁止把读取 `.stream.jsonl` / debug log 当成主流程兜底；需要继续追问或修复时，优先在同一个可视化窗口里继续对话。

## Codex 派工流程

1. 建立目录：

```powershell
New-Item -ItemType Directory -Force .codex_delegate/prompts,.codex_delegate/screenshots,.codex_delegate/reports,.codex_delegate/state,.codex_delegate/logs | Out-Null
```

2. 生成任务编号：

```text
yyyyMMdd-HHmmss-<short-task-name>
```

3. 写入任务包：

```text
.codex_delegate/prompts/<task-id>.md
```

4. 启动可见 Claude Code 窗口，等待输入框可用，粘贴任务包并提交；至少保存一张截图到：

```text
.codex_delegate/screenshots/<task-id>-*.png
```

5. 要求 Claude Code 把最终交接报告写入：

```text
.codex_delegate/reports/<task-id>.md
```

6. Codex 读取报告、检查 diff、运行验证命令。

   省 token 规则：Codex 默认只读取交接报告、截图、`git diff --stat`、必要的精确 diff 和验证输出。禁止读取 `.stream.jsonl` 或长 debug log。

7. 如报告缺失、输出不完整、验证失败，Codex 应优先回到同一个可视化 Claude Code 会话继续补充指令；必要时自行接手，不让用户来回传话。

## 推荐 PowerShell 启动模板

Codex 可以使用以下模板，按任务替换 `<task-id>`：

```powershell
$taskId = "<task-id>"
New-Item -ItemType Directory -Force .codex_delegate/prompts,.codex_delegate/screenshots,.codex_delegate/reports,.codex_delegate/state,.codex_delegate/logs | Out-Null

$promptPath = ".codex_delegate/prompts/$taskId.md"
$screenshotDir = ".codex_delegate/screenshots"
$reportPath = ".codex_delegate/reports/$taskId.md"

Set-Clipboard -Value (Get-Content -Raw -LiteralPath $promptPath)

Start-Process powershell.exe -ArgumentList @(
  "-NoExit",
  "-Command",
  "Set-Location 'E:\Project\mbfTwain'; claude --dangerously-skip-permissions"
)

# 然后等待输入框可用，执行粘贴、回车，并把窗口截图保存到 $screenshotDir
```

如果自定义参数包含带空格路径或复杂引号，Codex 应直接构造参数数组；但前提仍然是打开可视化 TUI，而不是切回 print/json 模式。

## Claude Code 省 token / 省时间规则

- 能让 Claude Code 写短交接报告时，Codex 只读报告，不读长原始日志。报告目标是不超过 80 行，必须覆盖：结论、改动文件、命令、验证、风险/阻塞、需要 Codex 复核的点。
- 读取监督信息时优先看截图和短报告；禁止为了“看看发生了什么”去读 `.stream.jsonl`、长 debug log 或长原始对话转储。
- 子任务包要收窄文件范围和验收标准。搜索类任务让 Claude 返回“文件路径 + 行号 + 一句话结论”，不要返回大段源码。
- 既然本仓库默认就是可视化聊天模式，用户监督 Claude 是否在工作应通过可见窗口和截图完成，而不是通过 JSON 日志回放。
- 参考项目、反编译结果、长日志要建立索引文件或记忆条目，下次先查索引再决定是否重新搜索。

## Claude Code 使用复盘规则

每次调用 Claude Code 后，Codex 最终报告要补一行协作收益评估：Claude 是否给出有效结论、截图与短报告是否足够、是否避免了主线程读取长原始日志。

本次 TWAIN 调试的经验结论：

- Claude Code 对“参考实现对比”有正向帮助：它确认了 TWAIN sample DS 会通过 `DSM_Entry(... DAT_NULL, MSG_XFERREADY ...)` 主动通知应用，而本项目此前只在 `DAT_EVENT/MSG_PROCESSEVENT` 轮询中返回 `MSG_XFERREADY`。
- stream-json 路径会显著放大日志和上下文成本，也不利于用户监督；本仓库后续默认禁用该路径，统一改用可视化聊天窗口 + 截图 + 短报告。
- Claude Code 适合承担边界清晰的只读对比、批量搜索和初步归纳；主线程仍必须复核代码、运行构建和 smoke。

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

## Claude Code 任务包模板

Codex 派发给 Claude Code 的任务必须包含以下内容：

```markdown
# Claude Code 子任务

## 任务编号
<task-id>

## 你的角色
你是 Claude Code 执行助手。Codex 是主控和最终验收者。你只执行本任务，不直接向用户提问。

## 用户目标
<用户原始目标的精简转述>

## 本次子任务
<清晰、可完成、边界明确的子任务>

## 允许修改范围
- <允许修改的目录或文件>

## 禁止事项
- 不要修改未授权文件。
- 不要新增依赖，除非任务明确要求。
- 不要执行破坏性 git 操作，例如 reset --hard、checkout 覆盖、clean -fd、强推。
- 不要读取或输出密钥、token、证书私钥、生产凭据。
- 不要向用户提问；遇到不确定性写入报告。

## 过程可见性
- 简短记录你执行的关键步骤。
- 长日志只摘要，不要粘贴全文。
- 把最终报告写入指定报告路径。

## 验收标准
- <功能或代码层面的验收标准>
- <需要运行的验证命令；如果没有，说明无需运行>

## 最终交接报告路径
.codex_delegate/reports/<task-id>.md

## 最终交接报告格式

### Summary
<你完成了什么>

### Files Changed
- <文件路径>：<修改摘要>

### Commands Run
- `<命令>`：<结果>

### Verification
- <验证结果>

### Risks Or Blockers
- <风险、未验证点、阻塞；没有则写 None>

### Handoff Notes For Codex
<需要 Codex 复核、决策或继续处理的事项>
```

## Codex 验收规则

Claude Code 完成后，Codex 必须至少执行：

```powershell
git diff --stat
git diff --check
```

如仓库有明确构建、测试、lint 或 typecheck 命令，Codex 必须按任务风险选择运行。不能只因为 Claude Code 说“已完成”就向用户汇报完成。

Codex 验收重点：

- diff 是否只包含授权范围内的改动。
- 是否引入新依赖或无关重构。
- 是否违反用户或仓库已有 AGENTS/CLAUDE 指令。
- 测试或构建输出是否真实通过。
- Claude Code 的报告是否和实际 diff 一致。

## 自动重试规则

Codex 可以在不询问用户的情况下自动重试 Claude Code，条件是：

- 第一次输出格式错误。
- 报告文件缺失。
- 构建或测试失败且错误局部、可修复。
- Claude Code 改动超出范围但可通过补充指令收敛。

自动重试上限：同一子任务最多 2 次。超过后 Codex 应自行接手或向用户报告明确阻塞。

重试任务包必须包含：

- 上一次报告路径。
- 上一次截图路径或窗口状态记录。
- Codex 发现的问题。
- 明确修复要求。

## 安全边界

Claude Code 默认不得执行：

```text
git reset --hard
git checkout -- .
git clean -fd
Remove-Item -Recurse -Force <不明确路径>
rm -rf
format
del /s
强推远端分支
上传密钥或源码到外部服务
```

确实需要破坏性操作时，Codex 必须停下来向用户说明具体命令、目标路径和风险，由用户明确授权。

## 最终汇报模板

Codex 给用户的最终汇报建议格式：

```markdown
已完成。

我调用了 Claude Code 处理：<子任务>。
任务包：<.codex_delegate/prompts/...>
截图：<.codex_delegate/screenshots/...>
报告：<.codex_delegate/reports/...>

Codex 验收：
- `git diff --check`：通过/失败
- `<构建或测试命令>`：通过/失败/未运行原因

改动文件：
- `<path>`：<摘要>

剩余风险：
- <没有则写“无已知风险”>
```

## 本文件生效后的工作方式

从现在开始，Codex 不需要用户提醒即可使用 Claude Code。只要任务适合外包，Codex 应自动：

1. 写任务包。
2. 调用 Claude Code。
3. 展示任务包路径、截图路径或关键过程。
4. 读取 Claude Code 交接报告。
5. 亲自验收。
6. 把最终结果汇报给用户。
