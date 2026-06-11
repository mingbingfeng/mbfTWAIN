# Phase 3 IPC 协议

UI 进程作为 Named Pipe server，TWAIN Data Source 作为 client。

```text
Pipe name: \\.\pipe\mbfTwain.VirtualScanner.v1
Encoding: UTF-8
Transport: one command line per connection
```

## 命令

```text
PING
```

返回：

```text
OK PONG
```

```text
GET_STATE
```

返回：

```text
OK STATE
revision 3
duplex 1
pixel RGB
paper A3
xres 300
yres 300
scan 1
image C:\scan\front.png
image C:\scan\back.png
END
```

```text
BEGIN_SCAN
```

也可以携带 DS 当前能力设置：

```text
BEGIN_SCAN duplex=0 pixel=RGB paper=A3 xres=300 yres=300
```

DS 在 TWAIN 宿主调用 `DAT_USERINTERFACE / MSG_ENABLEDS` 且允许显示 Source UI 时发送。UI 收到后开始一次新的扫描会话：先应用随命令传入的当前设置，再清空上一次图片选择、清除 `scan` 标记、显示并置前窗口，等待用户添加图片并点击“开始扫描”。

返回：

```text
OK BEGIN_SCAN
```

```text
ACK_SCAN 3
```

当 DS 已消费某次扫描请求后发送。若 revision 匹配，UI 清除 `scan` 标记并递增 revision。

## 字段

- `revision`：UI 状态版本，任意设置变化都会递增。
- `duplex`：`0` 单面，`1` 双面。
- `pixel`：`BW`、`GRAY`、`RGB`。
- `paper`：纸张类型/尺寸，当前支持 `A4`、`A3`；DS 会映射到 TWAIN `ICAP_SUPPORTEDSIZES`。
- `xres` / `yres`：当前 DPI。
- `scan`：`1` 表示用户已点击“开始扫描”，DS 下一阶段会把它转为 `MSG_XFERREADY`。
- `image`：一行一个待扫描图片路径，保持用户选择顺序。

这个协议刻意不用 JSON，避免 C++ DS 引入额外解析依赖。图片路径保留为 UTF-8 文本，C++ 侧转换为 UTF-16 后再交给 Windows 图像解码或后续处理层。
