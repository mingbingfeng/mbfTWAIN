# Phase 1 架构说明

## 目标

Phase 1 建立 TWAIN Data Source `.ds` 模块的可加载骨架：

- 导出标准 `DS_Entry`。
- 返回稳定的 `TW_IDENTITY`。
- 模拟最小状态：State 3 `SourceLoaded` 和 State 4 `SourceOpened`。
- 通过 `DG_CONTROL / DAT_STATUS / MSG_GET` 返回上一条失败原因。
- 对尚未实现的能力、事件、图像传输 triplet 返回明确失败码，避免宿主程序崩溃或误判。

## 组件布局

```text
external/twain/2.4/twain.h
    TWAIN Working Group 发布的 TWAIN 2.4 公共头文件。

src/VirtualTwainDS/
    VirtualTwainDS.vcxproj
        Native C++ 动态库工程。

    VirtualTwainDS.def
        强制导出未修饰名称 DS_Entry。32 位 stdcall 下尤其重要。

    dllmain.cpp
        模块生命周期入口。只关闭线程通知，不启动线程、不做 IPC、不加载 UI。

    TwainDataSource.h/.cpp
        单例 Data Source 上下文，管理 TWAIN 状态、身份信息和 triplet 分发。
```

## 宿主如何发现虚拟扫描仪

标准 TWAIN 应用通常加载 DSM（Data Source Manager），由 DSM 负责枚举、打开和关闭 Data Source。应用不会直接调用我们的业务类，而是通过 DSM 间接让 `.ds` 模块被加载；DSM 随后调用模块导出的 `DS_Entry`。

发现链路如下：

```text
TWAIN 应用
  -> 打开 DSM
  -> DSM 枚举 Data Source
  -> DSM 加载本项目 .ds 模块
  -> DSM 调用 DS_Entry(DG_CONTROL, DAT_IDENTITY, MSG_GET/MSG_OPENDS)
  -> 本项目返回 TW_IDENTITY
```

因为 TWAIN Data Source 是被宿主进程加载的模块，所以位数必须匹配：

```text
32 位扫描宿主 -> 32 位 .ds 模块
64 位扫描宿主 -> 64 位 .ds 模块
```

后续做安装器时需要同时发布 Win32 和 x64 构建，并安装到目标 DSM 能发现的 Data Source 目录。TWAIN DSM 2.x 在 Windows 上通常扫描 `C:\Windows\twain_32` 或 `C:\Windows\twain_64` 下的 `.ds` 模块。Phase 1 不写安装器，先保证模块入口和身份响应正确。

## 当前 DS_Entry 行为

已实现：

```text
DG_CONTROL / DAT_IDENTITY / MSG_GET
    返回本虚拟扫描仪身份。

DG_CONTROL / DAT_IDENTITY / MSG_OPENDS
    State 3 -> State 4，记录打开它的宿主 TW_IDENTITY。

DG_CONTROL / DAT_IDENTITY / MSG_CLOSEDS
    State 4+ -> State 3，清理宿主身份。

DG_CONTROL / DAT_STATUS / MSG_GET
    返回上一条 TWAIN 条件码。
```

Phase 1 尚未实现：

```text
DG_CONTROL / DAT_CAPABILITY
DG_CONTROL / DAT_USERINTERFACE
DG_CONTROL / DAT_EVENT
DG_IMAGE / DAT_IMAGEINFO
DG_IMAGE / DAT_IMAGENATIVEXFER
DG_IMAGE / DAT_IMAGEMEMXFER
DG_AUDIO
```

这些将在 Phase 2 到 Phase 4 实现。Phase 1 对它们返回 `TWRC_FAILURE`，并将 `ConditionCode` 设为 `TWCC_BADPROTOCOL`。

## 设计边界

`DllMain` 不能做重活。以后 IPC、UI、图片解码都不能放进 `DllMain`，否则容易造成 Loader Lock、宿主卡死或死锁。Phase 1 的 `DllMain` 只调用 `DisableThreadLibraryCalls`，状态初始化由首次 `DS_Entry` 调用触发。

`DS_Entry` 必须对空指针、防状态错序、未知 triplet 做保护。TWAIN 宿主通常会在失败后调用 `DAT_STATUS/MSG_GET`，所以所有失败路径都必须设置 `lastStatus_`。

本阶段不分配 `HGLOBAL`，也不返回图像数据。内存句柄管理从 Phase 4 开始实现，届时 `GlobalAlloc/GlobalLock/GlobalUnlock/GlobalFree` 的所有权规则会单独封装。
