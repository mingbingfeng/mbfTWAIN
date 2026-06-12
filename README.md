# mbfTwain

`mbfTwain` is a phased virtual TWAIN 2.x scanner implementation.

Current work includes Phase 1 through Phase 4a scaffolding: the native C++
TWAIN Data Source module exports `DS_Entry`, tracks source-loaded/opened/enabled
and transfer-ready states, negotiates core scanner capabilities, connects to a
.NET image-selection UI over Named Pipes, starts a new image-selection session
when a TWAIN host enables the source UI, and supports native DIB image transfer
plus buffered memory transfer. Real DSM/application installation validation is
the next major hardening phase.

## Project Layout

```text
external/twain/2.4/twain.h        Official TWAIN 2.4 public header
src/VirtualTwainDS/               Native TWAIN Data Source module
docs/twain-discovery.md           How TWAIN applications discover the DS
docs/phase-1-architecture.zh-CN.md Phase 1 design notes in Chinese
docs/phase-2-capabilities.zh-CN.md Phase 2 capability behavior
docs/ipc-protocol.zh-CN.md        Phase 3 Named Pipe protocol
docs/phase-4a-native-transfer.zh-CN.md Native DIB transfer behavior
tools/SmokeDsEntry/               Minimal loader smoke test for DS_Entry
tools/SmokeIpcClient/             C++ IPC client smoke test
tools/FakeScannerPipeServer/      Deterministic pipe server for transfer smoke tests
```

## Build

Build from a Visual Studio Developer PowerShell:

```powershell
msbuild .\src\VirtualTwainDS\VirtualTwainDS.vcxproj /p:Configuration=Release /p:Platform=Win32
msbuild .\src\VirtualTwainDS\VirtualTwainDS.vcxproj /p:Configuration=Release /p:Platform=x64
```

Use the Win32 build for 32-bit TWAIN applications and the x64 build for 64-bit
TWAIN applications. TWAIN sources are loaded in-process by the Data Source
Manager, so bitness must match the host process.
The build output uses a `.ds` extension because DSM discovery expects TWAIN
Data Source modules to use that extension.

The project defaults to the MSVC `v143` toolset. If your Visual Studio
installation uses another toolset, change the `PlatformToolset` value in
`src/VirtualTwainDS/VirtualTwainDS.vcxproj` or override it with
`/p:PlatformToolset=<installed-toolset>`.

## Smoke Test

`tools/SmokeDsEntry/SmokeDsEntry.cpp` dynamically loads a built DS module and calls
the lifecycle and capability triplets:

```text
DAT_IDENTITY / MSG_GET
DAT_IDENTITY / MSG_OPENDS
DAT_IDENTITY / MSG_CLOSEDS
DAT_STATUS   / MSG_GET
```

`tools/SmokeIpcClient/SmokeIpcClient.cpp` connects to the configuration UI's
Named Pipe server and verifies the C++ IPC client can read the UI state.

`tools/FakeScannerPipeServer` can stand in for the UI when testing transfer
paths:

```powershell
dotnet build .\tools\FakeScannerPipeServer\FakeScannerPipeServer.csproj -c Release
dotnet .\tools\FakeScannerPipeServer\bin\Release\net10.0\mbfTwain.FakeScannerPipeServer.dll --image .\build\test-assets\page1.bmp --connections 3 --revision 42
```

With the fake server already listening, set `MBF_SMOKE_EXPECT_XFERREADY=1` and
run `SmokeDsEntry.exe` against the built `.ds`. Set `MBF_SMOKE_USE_MEMORY=1` as
well to exercise `DAT_IMAGEMEMXFER` instead of `DAT_IMAGENATIVEXFER`.

To exercise the UI-style delayed-ready path, start the fake server with
`--scan 0 --scan-after-begin-delay-ms 200 --connections 40`, then also set
`MBF_SMOKE_EXPECT_ENABLE_CALLBACK=1`. That asserts the DS raises
`DAT_NULL/MSG_XFERREADY` before the first explicit `DAT_EVENT` poll.

## Runtime UI

When a TWAIN host calls `DAT_USERINTERFACE / MSG_ENABLEDS` with `ShowUI=TRUE`,
the DS asks the UI process to begin a fresh scan session. If the UI is not
already running, the DS tries to start `mbfTwain.VirtualScannerConfig.exe` from:

```text
MBF_TWAIN_UI_EXE
same directory as mbfVirtualTwainDS.ds
src\VirtualScannerConfig\bin\Release\net10.0-windows
```

The UI clears the previous image list, shows itself, waits for image selection,
then sends those images after the user clicks Start Scan. Once image transfer
to the TWAIN host starts, the DS asks the UI to hide without clearing its
session state. After the final transfer is acknowledged, the UI clears the list
and remains hidden until the next scan. If the host sets `CAP_XFERCOUNT` to a
positive value, the DS only transfers that many images in the current session
and discards any additional selected images.

For an installed TWAIN source, copy the `.ds` file and the
`mbfTwain.VirtualScannerConfig.*` runtime files into the same TWAIN source
directory, or set `MBF_TWAIN_UI_EXE` to the full path of
`mbfTwain.VirtualScannerConfig.exe`.
