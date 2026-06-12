# Phase 4a Native Transfer

当前实现已经把 UI/IPC 扫描请求接入 TWAIN 事件循环、native DIB 传输和 buffered memory transfer。

## 已实现链路

```text
DAT_USERINTERFACE / MSG_ENABLEDS
  -> State 4 转 State 5
  -> 若宿主允许显示 Source UI，则启动/唤醒配置 UI
  -> 发送 BEGIN_SCAN，UI 清空旧图片并等待用户选择图片后点击“开始扫描”

DAT_EVENT / MSG_PROCESSEVENT
  -> 读取 Named Pipe GET_STATE
  -> 当 scan=1 且 image 列表非空时返回 TWRC_DSEVENT + MSG_XFERREADY
  -> State 5 转 State 6

DG_IMAGE / DAT_IMAGEINFO / MSG_GET
  -> 使用 WIC 读取当前图片尺寸
  -> 按当前 pixel type / DPI 填充 TW_IMAGEINFO

DG_IMAGE / DAT_IMAGENATIVEXFER / MSG_GET
  -> WIC 解码 PNG/JPG/BMP/TIFF 等 Windows 支持的图片
  -> 生成 packed DIB HGLOBAL
  -> 返回 TWRC_XFERDONE
  -> 通知 UI 执行 HIDE_SCAN_UI，只隐藏窗口，不清空扫描状态
  -> State 6 转 State 7

DG_CONTROL / DAT_SETUPMEMXFER / MSG_GET
  -> 返回 MinBufSize / Preferred / MaxBufSize

DG_IMAGE / DAT_IMAGEMEMXFER / MSG_GET
  -> WIC 解码为 top-down raster
  -> 按应用提供的 TW_MEMORY buffer 分块写入完整行
  -> 非最后一块返回 TWRC_SUCCESS
  -> 最后一块通知 UI 执行 HIDE_SCAN_UI 并返回 TWRC_XFERDONE

DAT_PENDINGXFERS / MSG_ENDXFER
  -> 返回剩余页数
  -> 最后一页完成后 ACK_SCAN，UI 清除 scan 标记、清空图片并隐藏窗口
```

## 像素格式

Native DIB 和 memory transfer 均支持：

```text
TWPT_RGB  -> native: 24bpp BI_RGB DIB；memory: R/G/B interleaved
TWPT_GRAY -> native: 8bpp BI_RGB DIB + 256 色灰度调色板；memory: 8bpp 灰度
TWPT_BW   -> native: 1bpp BI_RGB DIB + 黑白调色板；memory: 1bpp packed bits
```

`ICAP_PIXELTYPE` 的当前值来自 TWAIN capability 设置，也会在 `MSG_XFERREADY` 前同步 UI pipe 的 `pixel` 字段。

## 内存所有权

`DAT_IMAGENATIVEXFER` 返回的 DIB 是 `GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT)` 分配的 `HGLOBAL`。TWAIN 应用消费后负责释放该句柄。

DS 内部不缓存返回给宿主的 DIB 句柄，避免跨宿主生命周期误释放。

## 仍未完成

当前验证使用直接加载 `.ds` 的 smoke harness 和 `tools/FakeScannerPipeServer` fake pipe server，覆盖：

```text
DAT_EVENT / MSG_PROCESSEVENT -> MSG_XFERREADY
DAT_IMAGEINFO / MSG_GET
DAT_IMAGENATIVEXFER / MSG_GET
DAT_SETUPMEMXFER / MSG_GET
DAT_IMAGEMEMXFER / MSG_GET
DAT_PENDINGXFERS / MSG_ENDXFER -> ACK_SCAN
```

还没有用真实 TWAIN DSM/Adobe Acrobat/第三方扫描宿主做安装后枚举与扫描验证。
