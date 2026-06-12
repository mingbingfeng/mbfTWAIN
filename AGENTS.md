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

> Codex 是主控和最终验收者；Claude Code 是可见的重活执行助手。
> 用户只和 Codex 对话。Codex 负责拆任务、决策、验收和汇报；Claude Code 负责大范围搜索、批量机械修改、长日志归纳、初步测试等高 token 消耗工作。
>
> 本目录同时维护 `AGENTS.md` 和 `CLAUDE.md`，两者应保持同一套规则。

## 协作原则

- **窄任务优先**：Claude Code 每次只接一个边界清晰的子任务，默认不做“全面调查 + 实现 + 验收”的大包。
- **Codex 不重复调查**：一旦派给 Claude Code，Codex 不并行搜索同一批文件；除非 Claude 超时、跑偏、缺报告或结果明显不可信。
- **报忧不报喜**：正常完成时 Claude 只写小状态 JSON；只有失败、风险、需要决策或验证异常时才写详细报告。
- **状态文件驱动等待**：Claude 工作中，Codex 低频检查小状态 JSON；不要为了“看看进展”读取长原始输出。
- **不截图作证据**：可视化 Claude Code 窗口由用户直接观察，不保存截图作为常规证据。
- **最终责任不外包**：Codex 必须复核授权范围、关键 diff、验证结果和安装结果，不能只转述 Claude 的结论。
- **Claude 默认可视**：凡在本仓库启用 Claude Code 协作，默认必须启动可见的交互式 Claude Code 会话；只有用户明确要求后台、非交互或机器可消费输出时，才允许例外。不得擅自退化为 `claude -p`、后台 print 模式或仅保留日志文件。
- **协议偏差当场纠正**：如果当前协作方式偏离用户要求或本协议，Codex 必须在当前任务立即修正，不要用“下次改进”替代本次纠偏。
- **正常路径节流**：在 `status=ok` 且状态 JSON 已足够说明结论时，Codex 默认只读取状态 JSON，不读取 Claude 全量对话、stream-json 或冗长报告。

## 何时委托

优先委托给 Claude Code：

- 跨多个文件搜索、引用统计、证据定位。
- 批量机械修改、格式统一、重复样板替换。
- 长日志、长构建输出、长测试输出的摘要和错误提取。
- 初步影响范围摸底、候选路径比较、补盲审查。
- 用户明确要求 Claude Code 介入。

Codex 直接处理：

- 单文件或两三个文件的小改动。
- 需要产品判断、需求取舍、协议重构、最终方案裁决。
- 涉及密钥、凭据、账号、私有配置泄露风险。
- 破坏性操作或不可逆操作的判断与授权。
- Claude Code 协议本身的修改，避免递归委托。

## 推荐分工

常用三段式：

1. Codex 写窄任务包并启动 Claude Code。
2. Claude Code 执行重活，写 `.codex_delegate/state/<task-id>.json`；异常时再写详细报告。
3. Codex 等待时只低频检查状态 JSON；发现异常才读详细报告、回到 Claude 会话追加指令，或向用户发起询问。

实现任务的推荐拆法：

- **调查 lane**：Claude 找证据和行号，Codex 不重复搜索。
- **实现 lane**：Claude 可做边界明确的批量改动；Codex 复核 diff。
- **补盲 lane**：Codex 写完最小修复后，可让 Claude 只查遗漏路径、异常路径或测试缺口。
- **验收 lane**：Codex 负责最终 `git diff --check`、构建、smoke、安装和汇报。
- **Claude lane 默认可视**：只要启用 Claude Code，Codex 就负责准备任务包并启动可见窗口；之后以状态 JSON 协调，不要再用后台模式偷偷替代。

## 通用分阶段委托协议

适用场景：

- 需求开放、质量敏感、主观性强。
- 需要先收敛方案再动手的大改动。
- 跨多文件、跨层级、容易“做出来但不对味”的任务。
- 上一版尝试已经证明“直接让 Claude 一步做到位”效果不稳的任务。

默认不要把下面几件事打包成一个 Claude 子任务：

- 调查证据
- 做方案裁决
- 大范围实现
- 自我审美判断
- 最终验收与完成宣告

对这类任务，默认走分阶段而不是一步到位：

1. `调查/收敛阶段`
   - Claude 只做证据收集、候选方案、风险点、原型草图或最小试探。
   - 默认不做大改动；最多允许只读调查或极小原型。
   - 产物应让 Codex 能做明确裁决。
2. `实现阶段`
   - 只有在方案已足够明确后，才让 Claude 改代码。
   - 任务包必须写清允许范围、禁止事项、验收标准。
   - Claude 只实现，不替 Codex 做产品/审美最终裁决。
3. `验收/补盲阶段`
   - Claude 可做定向补盲、补测试、补检查。
   - Codex 负责最终 diff、构建、测试、安装和对用户汇报。

阶段切换规则：

- 如果 Claude 在当前阶段发现“还需要先定方案”或“需要 Codex 做裁决”，应停在当前阶段，写状态 JSON 或报告，不要越级直接做下一阶段。
- 如果任务有明显主观质量目标（例如 UI、文案、交互、命名、架构边界、重构形态），至少拆成“方案”+“实现”两段，除非用户明确要求直接试做。
- 如果上一版结果“不丑但不对”，不要继续直接补丁式重试；先补充验收标准，再发下一阶段任务包。

任务编号建议：

- `<task-id>-discover`
- `<task-id>-propose`
- `<task-id>-implement`
- `<task-id>-verify`

这样状态文件、报告、等待和重试都能按阶段独立追踪。

## 落盘结构

每次委托使用当前仓库内的固定目录：

```text
.codex_delegate/
  prompts/       # Codex 发给 Claude Code 的任务包
  reports/       # 仅异常或需要关注时写详细报告
  state/         # 小状态 JSON，主线程默认只读这里
  logs/          # 轻量启动记录或失败日志尾部；禁止把 stream-json 当作主流程
```

## Claude Code 调用方式

默认使用可视化聊天窗口：

```powershell
cd <repo-root>
claude --dangerously-skip-permissions
```

- 只要使用 Claude Code 协作，上面的可视化启动方式就是**强制要求**，不是建议项。
- 默认不要使用 `claude -p`、`--output-format stream-json`、`--include-partial-messages`、`--include-hook-events`。
- 只有当用户明确接受后台/非交互执行，或任务本身明确要求机器可消费输出时，才允许使用 `claude -p` 或其它非交互模式。
- 如果未来用户明确要求恢复 `-p --output-format stream-json`，必须同时带 `--verbose`，否则 Claude Code CLI 会报错：`When using --print, --output-format=stream-json requires --verbose`。
- 任务包写入 `.codex_delegate/prompts/<task-id>.md`，启动窗口后提交给 Claude。
- **中文/非 ASCII 任务包禁止直接作为终端启动参数或整段粘贴到 Claude 终端**。先把任务包落盘为 UTF-8 文件，再用 **ASCII-only** 引导词让 Claude 自己去读文件。
- 推荐启动方式：`claude --dangerously-skip-permissions "Read E:\\Project\\mbfTwain\\.codex_delegate\\prompts\\<task-id>.md from disk and follow it exactly."`
- 如果任务包包含中文、Emoji、特殊符号或长 Markdown，优先采用“文件落盘 + ASCII 引导词”而不是命令行内联 prompt；不要把整段中文 prompt 直接塞进 `powershell.exe -Command`、`claude "<prompt>"` 或 Windows Terminal 粘贴缓冲。
- Windows 上若从 PowerShell 再拉起一个可见 Claude，会额外遇到一层引号截断风险：长启动词可能只剩第一个 token（例如只传进 `Read`）。这种情况下不要继续试引号拼接，改用临时 `.ps1` / `.cmd` wrapper 脚本，从文件读取启动词后再调用 `claude`。
- 如果终端已经出现乱码，不要继续让 Claude 基于乱码内容执行；立即停止该会话，保留 UTF-8 任务包文件，改用 ASCII 引导词重启。
- 不保存截图作为流程证据；如果窗口状态、粘贴提交或 Claude 是否卡住无法判断，Codex 应优先依赖状态 JSON，仍不确定时通过 Codex App 询问窗口或短消息问用户。
- 禁止把 `.stream.jsonl`、debug log、完整对话转储作为主流程输入。
- 不得为了省 token、图方便或减少手动操作，而把默认可视的 Claude 协作偷偷改成后台模式。

## 状态 JSON 与等待机制

Claude 启动或进入长步骤时先写 `running` 状态：

```json
{
  "task_id": "<task-id>",
  "status": "running",
  "updated_at": "2026-06-12T12:00:00+08:00",
  "changed_files": [],
  "commands_failed": [],
  "needs_codex_attention": false,
  "summary": "正在执行：一句话当前阶段"
}
```

Claude 正常完成时只需要更新为：

```json
{
  "task_id": "<task-id>",
  "status": "ok",
  "updated_at": "2026-06-12T12:10:00+08:00",
  "changed_files": [],
  "commands_failed": [],
  "needs_codex_attention": false,
  "summary": "一句话结论"
}
```

需要 Codex 关注时才写详细报告，并在状态 JSON 指向它：

```json
{
  "task_id": "<task-id>",
  "status": "needs_attention",
  "updated_at": "2026-06-12T12:10:00+08:00",
  "needs_codex_attention": true,
  "summary": "一句话异常或决策点",
  "report_path": ".codex_delegate/reports/<task-id>.md"
}
```

Codex 等待规则：

- 已知 `task-id` 时，优先使用事件驱动等待：运行 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\Wait-ClaudeTask.ps1 -TaskId <task-id>`，在状态文件离开 `running` 时立即返回。
- 固定 20-30 秒轮询只作为后备路径；不要在已知状态文件路径时优先用长时间 `Start-Sleep` 轮询。
- `status=running` 且 `updated_at` 持续更新：继续等待，不读取 Claude 长对话或日志。
- `status=ok`：读取小状态 JSON 后进入 Codex 验收，不读取详细报告。
- `updated_at` 5-10 分钟无变化：视为可能卡住，优先回到 Claude 会话追加一句状态请求；仍无法判断时通过 Codex App 询问窗口或短消息问用户。
- 任务超过预期预算或 Claude 明显等待人工输入：Codex 自行接手或询问用户，不静默等待。
- Claude 已接手调查 lane 后，Codex 不要在等待期间并行做同一批搜索；只有在状态停滞、授权越界或结论明显不可信时才接管重查。

只有这些情况才读详细报告或长日志尾部：

- `status` 不是 `ok`。
- `needs_codex_attention` 为 `true`。
- `commands_failed` 非空。
- `git diff --check`、构建、测试或 smoke 失败。
- Claude 改动超出授权范围或结论和 diff 不一致。

## 任务包模板

```markdown
# Claude Code 子任务

## 任务编号
<task-id>

## 任务类型
<调查 | 方案 | 实现 | 验收>

## 当前阶段
<只做这一阶段；如果需要下一阶段，停止并写状态/报告，不要自行越级>

## 角色
你是 Claude Code 执行助手。Codex 是主控和最终验收者。你只执行本任务，不直接向用户提问。

## 用户目标
<一句话转述>

## 本次子任务
<窄任务；只做这一件事>

## 允许范围
- <允许读取/修改的文件或目录>

## 禁止事项
- 不要修改未授权文件。
- 不要新增依赖，除非任务明确要求。
- 不要执行破坏性 git 操作，例如 reset --hard、checkout 覆盖、clean -fd、强推。
- 不要读取或输出密钥、token、证书私钥、生产凭据。
- 不要向用户提问；不确定性写入状态或报告。

## 输出规则
- 开始执行：先写 `.codex_delegate/state/<task-id>.json`，`status` 为 `running`。
- 长步骤：阶段变化或等待较久时更新 `updated_at` 和一句话 `summary`。
- 正常完成：把状态更新为 `ok`，不要写长报告；这一步应作为完成前最后一个显式状态写入，因为 Codex 可能正在监听该文件事件。
- 需要 Codex 关注：写状态 JSON，并把详细报告写到 `.codex_delegate/reports/<task-id>.md`；终态状态写入应放在报告落盘之后。
- 证据类任务最多返回 8 条 `文件路径:行号:一句话结论`。
- 长日志只摘要失败点和最后相关片段，不粘贴全文。
- 编码要求：任务包文件本身保存为 UTF-8；如果通过启动词把任务交给 Claude，启动词应尽量保持 ASCII-only，并明确要求 Claude 从磁盘读取任务包。
- 回复语言：除非任务包明确要求其它语言，Claude 给 Codex 的状态 `summary`、详细报告和最终交接说明默认使用中文；启动词本身可保持 ASCII/English。

## 验收标准
- <可验证条件>
- <建议运行的命令；没有则写“无需运行”>
```

## Codex 验收规则

Claude Code 完成后，Codex 至少执行：

```powershell
git diff --stat
git diff --check
```

如仓库有明确构建、测试、lint 或 typecheck 命令，Codex 按风险选择运行。本仓库凡涉及 DS/UI 代码改动，必须按顶部规则构建 Win32/x64、跑 smoke，并安装本机 TWAIN。

Codex 验收重点：

- diff 是否只包含授权范围内的改动。
- 是否引入新依赖或无关重构。
- 是否违反 AGENTS/CLAUDE 指令。
- 测试或构建输出是否真实通过。
- 状态 JSON、报告和实际 diff 是否一致。

## 自动重试

Codex 可以自动重试同一 Claude 子任务，最多 2 次：

- 状态 JSON 缺失或格式错误。
- `status=running` 但 `updated_at` 长时间无更新。
- 报告缺失但状态声明需要报告。
- 构建或测试失败且错误局部、可修复。
- Claude 改动超出范围但可通过补充指令收敛。
- 启动方式违反本协议，例如本应使用默认可见聊天窗口却误用了后台/非交互模式。

超过 2 次后，Codex 自行接手或向用户报告明确阻塞。

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

## 最终汇报要求

Codex 最终只汇报高信号内容：

- 是否调用 Claude Code；如调用，列任务包、状态 JSON、异常报告路径。
- Claude 的核心结论；正常路径不复述长过程。
- Codex 自己跑过的验证。
- 改动文件和剩余风险。

协作收益评估只需一句话：说明 Claude 是否有效、是否避免了主线程读取长日志或重复搜索。

## 已知经验

- Claude Code 对边界清晰的只读对比、批量搜索、长日志归纳和补盲审查有帮助。
- Codex 不应在 Claude 工作时并行做同一批搜索；这会浪费上下文和时间。
- 正常结果走状态 JSON；详细报告只在异常时进入 Codex 上下文。
- 等待 Claude 时以 `status/updated_at` 为准；无法判断时用询问窗口或短消息问用户，不用截图补证。
- 已知 `task-id` 时，`tools/Wait-ClaudeTask.ps1` 比固定 `Start-Sleep 60` 更合适；Claude 一写终态，Codex 就应立即结束等待。
- stream-json 路径会显著放大日志和上下文成本，也不利于用户监督；本仓库默认禁用。
- 中文任务包的稳定路径是“UTF-8 文件落盘 + ASCII 启动词引用文件”；不要把中文正文直接当作 Claude 终端参数传入，否则 Windows 终端链路可能出现乱码。

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
