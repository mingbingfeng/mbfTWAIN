param(
    [string]$Configuration = "Release",
    [string]$Version = "",
    [switch]$BuildOnly,
    [switch]$SkipSmoke,
    [switch]$NoForceUi
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$DsProject = Join-Path $Root "src\VirtualTwainDS\VirtualTwainDS.vcxproj"
$UiProject = Join-Path $Root "src\VirtualScannerConfig\VirtualScannerConfig.csproj"
$FakeServerProject = Join-Path $Root "tools\FakeScannerPipeServer\FakeScannerPipeServer.csproj"
$TwainHeaderDir = Join-Path $Root "external\twain\2.4"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "== $Message =="
}

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-File([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Message`: $Path"
    }
}

function Assert-Directory([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Message`: $Path"
    }
}

function Invoke-Checked([scriptblock]$Command, [string]$Label) {
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Invoke-WithTemporaryEnvironment([hashtable]$Variables, [scriptblock]$Action) {
    $originalValues = @{}
    foreach ($entry in $Variables.GetEnumerator()) {
        $originalValues[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, "Process")
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }

    try {
        & $Action
    }
    finally {
        foreach ($entry in $originalValues.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
        }
    }
}

function Get-DotNetVersionArguments {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        return @()
    }

    if ($Version -notmatch '^\d+\.\d+\.\d+([\-+].*)?$') {
        throw "Version must use semantic version format, for example 1.0.0. Actual: $Version"
    }

    $baseVersion = ($Version -split '[-+]')[0]
    $assemblyVersion = "$baseVersion.0"
    return @(
        "/p:Version=$Version",
        "/p:AssemblyVersion=$assemblyVersion",
        "/p:FileVersion=$assemblyVersion",
        "/p:InformationalVersion=$Version"
    )
}

function Get-VsWherePath {
    $path = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        return $path
    }

    return $null
}

function Get-VisualStudioInstallations {
    $vswhere = Get-VsWherePath
    if ($null -eq $vswhere) {
        return @()
    }

    $json = & $vswhere -all -products * -format json
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($json)) {
        return @()
    }

    return @($json | ConvertFrom-Json)
}

function Get-ScopeCppSdk {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($vs in Get-VisualStudioInstallations) {
        if ($vs.resolvedInstallationPath) {
            $candidates.Add((Join-Path $vs.resolvedInstallationPath "SDK\ScopeCppSDK\vc15"))
        }
        if ($vs.installationPath) {
            $candidates.Add((Join-Path $vs.installationPath "SDK\ScopeCppSDK\vc15"))
        }
    }

    $candidates.Add("D:\Program Files\Microsoft Visual Studio\18\Enterprise\SDK\ScopeCppSDK\vc15")

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        $cl = Join-Path $candidate "VC\bin\cl.exe"
        $windows = Join-Path $candidate "SDK\include\um\Windows.h"
        if ((Test-Path -LiteralPath $cl -PathType Leaf) -and
            (Test-Path -LiteralPath $windows -PathType Leaf)) {
            return $candidate
        }
    }

    throw "Could not find Visual Studio ScopeCppSDK vc15. Install Visual Studio C++ tools or update this script's tool discovery."
}

function Get-WindowsKit10Root {
    $candidates = @(
        "D:\Windows Kits\10",
        "${env:ProgramFiles(x86)}\Windows Kits\10",
        "${env:ProgramFiles}\Windows Kits\10"
    )

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Path -LiteralPath (Join-Path $candidate "Include") -PathType Container) {
            return $candidate
        }
    }

    throw "Could not find Windows Kits 10."
}

function Get-WindowsKitVersion([string]$KitRoot) {
    $versions = @(Get-ChildItem -LiteralPath (Join-Path $KitRoot "Include") -Directory |
        Where-Object {
            (Test-Path -LiteralPath (Join-Path $_.FullName "um\Windows.h") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $KitRoot "Lib\$($_.Name)\um\x86\kernel32.lib") -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $KitRoot "Lib\$($_.Name)\ucrt\x86\ucrt.lib") -PathType Leaf)
        } |
        Sort-Object Name -Descending)

    if ($versions.Count -eq 0) {
        throw "Could not find a Windows Kit version with headers and x86 libraries under $KitRoot."
    }

    return $versions[0].Name
}

function Get-Vs18MsvcToolRoot {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($vs in Get-VisualStudioInstallations) {
        foreach ($basePath in @($vs.resolvedInstallationPath, $vs.installationPath)) {
            if ($basePath) {
                $toolsRoot = Join-Path $basePath "VC\Tools\MSVC"
                if (Test-Path -LiteralPath $toolsRoot -PathType Container) {
                    $candidates.Add($toolsRoot)
                }
            }
        }
    }

    $candidates.Add("D:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC")

    foreach ($rootPath in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $rootPath -PathType Container)) {
            continue
        }

        $versions = @(Get-ChildItem -LiteralPath $rootPath -Directory |
            Where-Object {
                (Test-Path -LiteralPath (Join-Path $_.FullName "bin\Hostx64\x86\cl.exe") -PathType Leaf) -and
                (Test-Path -LiteralPath (Join-Path $_.FullName "lib\onecore\x86\msvcprt.lib") -PathType Leaf)
            } |
            Sort-Object Name -Descending)

        if ($versions.Count -gt 0) {
            return $versions[0].FullName
        }
    }

    throw "Could not find an MSVC x86 compiler and onecore libraries."
}

function Build-Managed {
    Write-Step "Building managed tools"
    [string[]]$versionArgs = @(Get-DotNetVersionArguments)
    dotnet build $UiProject -c $Configuration @versionArgs
    if ($LASTEXITCODE -ne 0) {
        throw "VirtualScannerConfig build failed with exit code $LASTEXITCODE"
    }

    dotnet build $FakeServerProject -c $Configuration
    if ($LASTEXITCODE -ne 0) {
        throw "FakeScannerPipeServer build failed with exit code $LASTEXITCODE"
    }
}

function Build-NativeX64([string]$ScopeSdk) {
    Write-Step "Building native x64"
    $cl = Join-Path $ScopeSdk "VC\bin\cl.exe"
    $vc = Join-Path $ScopeSdk "VC"
    $sdk = Join-Path $ScopeSdk "SDK"
    $outDir = Join-Path $Root "build\manual\x64\$Configuration"
    $dsObjDir = Join-Path $Root "build\obj\manual\x64\$Configuration\VirtualTwainDS"
    $toolObjDir = Join-Path $Root "build\obj\manual\x64\$Configuration\SmokeTools"
    New-Item -ItemType Directory -Force -Path $outDir, $dsObjDir, $toolObjDir | Out-Null

    Invoke-Checked {
        & $cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MD /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WINDOWS /D_USRDLL /D_WINDLL `
            /I"$Root\src\VirtualTwainDS" /I"$TwainHeaderDir" /I"$vc\include" /I"$sdk\include\um" /I"$sdk\include\shared" /I"$sdk\include\ucrt" `
            /LD "$Root\src\VirtualTwainDS\dllmain.cpp" "$Root\src\VirtualTwainDS\ImageDib.cpp" "$Root\src\VirtualTwainDS\ScannerIpcClient.cpp" "$Root\src\VirtualTwainDS\TwainDataSource.cpp" `
            /Fo"$dsObjDir\\" /Fe"$outDir\mbfVirtualTwainDS.ds" /Fd"$dsObjDir\mbfVirtualTwainDS.pdb" `
            /link /NOLOGO /DEF:"$Root\src\VirtualTwainDS\VirtualTwainDS.def" /SUBSYSTEM:WINDOWS /LIBPATH:"$vc\lib" /LIBPATH:"$sdk\lib" windowscodecs.lib ole32.lib uuid.lib advapi32.lib
    } "x64 DS build"

    Invoke-Checked {
        & $cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MD /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
            /I"$TwainHeaderDir" /I"$vc\include" /I"$sdk\include\um" /I"$sdk\include\shared" /I"$sdk\include\ucrt" `
            "$Root\tools\SmokeDsEntry\SmokeDsEntry.cpp" /Fo"$toolObjDir\SmokeDsEntry.obj" /Fe"$outDir\SmokeDsEntry.exe" `
            /link /NOLOGO /SUBSYSTEM:CONSOLE /LIBPATH:"$vc\lib" /LIBPATH:"$sdk\lib"
    } "x64 SmokeDsEntry build"

    Invoke-Checked {
        & $cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MD /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
            /I"$Root\src\VirtualTwainDS" /I"$TwainHeaderDir" /I"$vc\include" /I"$sdk\include\um" /I"$sdk\include\shared" /I"$sdk\include\ucrt" `
            "$Root\tools\SmokeIpcClient\SmokeIpcClient.cpp" "$Root\src\VirtualTwainDS\ScannerIpcClient.cpp" /Fo"$toolObjDir\\" /Fe"$outDir\SmokeIpcClient.exe" `
            /link /NOLOGO /SUBSYSTEM:CONSOLE /LIBPATH:"$vc\lib" /LIBPATH:"$sdk\lib"
    } "x64 SmokeIpcClient build"
}

function Build-NativeWin32([string]$ScopeSdk, [string]$MsvcToolRoot, [string]$KitRoot, [string]$KitVersion) {
    Write-Step "Building native Win32"
    $cl = Join-Path $MsvcToolRoot "bin\Hostx64\x86\cl.exe"
    $vcInc = Join-Path $ScopeSdk "VC\include"
    $vcLib = Join-Path $MsvcToolRoot "lib\onecore\x86"
    $sdkInc = Join-Path $KitRoot "Include\$KitVersion"
    $sdkLib = Join-Path $KitRoot "Lib\$KitVersion"
    $outDir = Join-Path $Root "build\manual\Win32\$Configuration"
    $dsObjDir = Join-Path $Root "build\obj\manual\Win32\$Configuration\VirtualTwainDS"
    $toolObjDir = Join-Path $Root "build\obj\manual\Win32\$Configuration\SmokeTools"
    New-Item -ItemType Directory -Force -Path $outDir, $dsObjDir, $toolObjDir | Out-Null

    Invoke-Checked {
        & $cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MD /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WINDOWS /D_USRDLL /D_WINDLL `
            /I"$Root\src\VirtualTwainDS" /I"$TwainHeaderDir" /I"$vcInc" /I"$sdkInc\um" /I"$sdkInc\shared" /I"$sdkInc\ucrt" `
            /LD "$Root\src\VirtualTwainDS\dllmain.cpp" "$Root\src\VirtualTwainDS\ImageDib.cpp" "$Root\src\VirtualTwainDS\ScannerIpcClient.cpp" "$Root\src\VirtualTwainDS\TwainDataSource.cpp" `
            /Fo"$dsObjDir\\" /Fe"$outDir\mbfVirtualTwainDS.ds" /Fd"$dsObjDir\mbfVirtualTwainDS.pdb" `
            /link /NOLOGO /DEF:"$Root\src\VirtualTwainDS\VirtualTwainDS.def" /SUBSYSTEM:WINDOWS /LIBPATH:"$vcLib" /LIBPATH:"$sdkLib\um\x86" /LIBPATH:"$sdkLib\ucrt\x86" windowscodecs.lib ole32.lib uuid.lib advapi32.lib
    } "Win32 DS build"

    Invoke-Checked {
        & $cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MD /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
            /I"$TwainHeaderDir" /I"$vcInc" /I"$sdkInc\um" /I"$sdkInc\shared" /I"$sdkInc\ucrt" `
            "$Root\tools\SmokeDsEntry\SmokeDsEntry.cpp" /Fo"$toolObjDir\SmokeDsEntry.obj" /Fe"$outDir\SmokeDsEntry.exe" `
            /link /NOLOGO /SUBSYSTEM:CONSOLE /LIBPATH:"$vcLib" /LIBPATH:"$sdkLib\um\x86" /LIBPATH:"$sdkLib\ucrt\x86"
    } "Win32 SmokeDsEntry build"

    Invoke-Checked {
        & $cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MD /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX `
            /I"$Root\src\VirtualTwainDS" /I"$TwainHeaderDir" /I"$vcInc" /I"$sdkInc\um" /I"$sdkInc\shared" /I"$sdkInc\ucrt" `
            "$Root\tools\SmokeIpcClient\SmokeIpcClient.cpp" "$Root\src\VirtualTwainDS\ScannerIpcClient.cpp" /Fo"$toolObjDir\\" /Fe"$outDir\SmokeIpcClient.exe" `
            /link /NOLOGO /SUBSYSTEM:CONSOLE /LIBPATH:"$vcLib" /LIBPATH:"$sdkLib\um\x86" /LIBPATH:"$sdkLib\ucrt\x86"
    } "Win32 SmokeIpcClient build"
}

function Copy-UiRuntime([string]$TargetDir) {
    $uiOutDir = Join-Path $Root "src\VirtualScannerConfig\bin\$Configuration\net10.0-windows"
    Assert-Directory $uiOutDir "UI output directory missing"

    Get-ChildItem -LiteralPath $uiOutDir -File -Filter "mbfTwain.VirtualScannerConfig.*" |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $TargetDir $_.Name) -Force
        }
}

function Stage-UiRuntime {
    Write-Step "Staging UI runtime next to local DS builds"
    foreach ($platform in @("Win32", "x64")) {
        $target = Join-Path $Root "build\manual\$platform\$Configuration"
        Assert-Directory $target "Native output directory missing"
        Copy-UiRuntime $target
    }
}

function Run-Smoke([string]$Platform) {
    $outDir = Join-Path $Root "build\manual\$Platform\$Configuration"
    $dsPath = Join-Path $outDir "mbfVirtualTwainDS.ds"
    $smokePath = Join-Path $outDir "SmokeDsEntry.exe"
    Assert-File $dsPath "DS missing"
    Assert-File $smokePath "SmokeDsEntry missing"

    Get-Process -Name "mbfTwain.VirtualScannerConfig" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Write-Step "Smoke test $Platform"
    Invoke-WithTemporaryEnvironment @{
        MBF_TWAIN_FORCE_UI = "0"
        MBF_SMOKE_EXPECT_XFERREADY = $null
        MBF_SMOKE_EXPECT_ENABLE_CALLBACK = $null
        MBF_SMOKE_EXPECT_PAPER_A3 = $null
        MBF_SMOKE_USE_MEMORY = $null
    } {
        Invoke-Checked {
            & $smokePath $dsPath
        } "$Platform SmokeDsEntry"
    }
}

function Run-UiDelayedReadySmoke([string]$Platform) {
    $outDir = Join-Path $Root "build\manual\$Platform\$Configuration"
    $dsPath = Join-Path $outDir "mbfVirtualTwainDS.ds"
    $smokePath = Join-Path $outDir "SmokeDsEntry.exe"
    $fakeServerDll = Join-Path $Root "tools\FakeScannerPipeServer\bin\$Configuration\net10.0\mbfTwain.FakeScannerPipeServer.dll"
    $imagePath = Join-Path $Root "build\test-assets\page1.bmp"
    $stdoutLog = Join-Path $Root "build\verify-$Platform-ui-delayed.out.log"
    $stderrLog = Join-Path $Root "build\verify-$Platform-ui-delayed.err.log"
    Assert-File $dsPath "DS missing"
    Assert-File $smokePath "SmokeDsEntry missing"
    Assert-File $fakeServerDll "FakeScannerPipeServer missing"
    Assert-File $imagePath "Delayed-ready smoke image missing"

    Get-Process -Name "mbfTwain.VirtualScannerConfig" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue

    $server = Start-Process `
        -FilePath "dotnet" `
        -ArgumentList @(
            $fakeServerDll,
            "--image", $imagePath,
            "--scan", "0",
            "--scan-after-begin-delay-ms", "200",
            "--connections", "80",
            "--revision", "42"
        ) `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru `
        -WindowStyle Hidden

    try {
        Start-Sleep -Milliseconds 150
        Write-Step "UI delayed-ready smoke $Platform"
        Invoke-WithTemporaryEnvironment @{
            MBF_TWAIN_FORCE_UI = "1"
            MBF_SMOKE_EXPECT_XFERREADY = "1"
            MBF_SMOKE_EXPECT_ENABLE_CALLBACK = "1"
            MBF_SMOKE_EXPECT_PAPER_A3 = "1"
            MBF_SMOKE_USE_MEMORY = $null
        } {
            Invoke-Checked {
                & $smokePath $dsPath
            } "$Platform delayed-ready SmokeDsEntry"
        }
    }
    finally {
        if ($null -ne $server -and -not $server.HasExited) {
            Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
            $server.WaitForExit()
        }
    }

    if (-not (Select-String -LiteralPath $stderrLog -Pattern "HIDE_SCAN_UI" -Quiet)) {
        throw "UI delayed-ready smoke $Platform did not observe HIDE_SCAN_UI before scan acknowledgement. See $stderrLog"
    }
}

function Run-UiCloseWithoutSelectionSmoke([string]$Platform) {
    $outDir = Join-Path $Root "build\manual\$Platform\$Configuration"
    $dsPath = Join-Path $outDir "mbfVirtualTwainDS.ds"
    $smokePath = Join-Path $outDir "SmokeDsEntry.exe"
    $fakeServerDll = Join-Path $Root "tools\FakeScannerPipeServer\bin\$Configuration\net10.0\mbfTwain.FakeScannerPipeServer.dll"
    $stdoutLog = Join-Path $Root "build\verify-$Platform-ui-close-without-selection.out.log"
    $stderrLog = Join-Path $Root "build\verify-$Platform-ui-close-without-selection.err.log"
    Assert-File $dsPath "DS missing"
    Assert-File $smokePath "SmokeDsEntry missing"
    Assert-File $fakeServerDll "FakeScannerPipeServer missing"

    Get-Process -Name "mbfTwain.VirtualScannerConfig" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue

    $server = Start-Process `
        -FilePath "dotnet" `
        -ArgumentList @(
            $fakeServerDll,
            "--scan", "0",
            "--connections", "4",
            "--revision", "126"
        ) `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru `
        -WindowStyle Hidden

    try {
        Start-Sleep -Milliseconds 150
        Write-Step "UI close-without-selection smoke $Platform"
        Invoke-WithTemporaryEnvironment @{
            MBF_TWAIN_FORCE_UI = "1"
            MBF_SMOKE_EXPECT_XFERREADY = $null
            MBF_SMOKE_EXPECT_CLOSEDSREQ = "1"
            MBF_SMOKE_EXPECT_ENABLE_CALLBACK = "1"
            MBF_SMOKE_EXPECT_PAPER_A3 = $null
            MBF_SMOKE_SET_XFERCOUNT = $null
            MBF_SMOKE_EXPECT_TRANSFER_TOTAL = $null
            MBF_SMOKE_USE_MEMORY = $null
        } {
            Invoke-Checked {
                & $smokePath $dsPath
            } "$Platform close-without-selection SmokeDsEntry"
        }
    }
    finally {
        if ($null -ne $server -and -not $server.HasExited) {
            Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
            $server.WaitForExit()
        }
    }
}

function Run-UiXferCountSmoke([string]$Platform) {
    $outDir = Join-Path $Root "build\manual\$Platform\$Configuration"
    $dsPath = Join-Path $outDir "mbfVirtualTwainDS.ds"
    $smokePath = Join-Path $outDir "SmokeDsEntry.exe"
    $fakeServerDll = Join-Path $Root "tools\FakeScannerPipeServer\bin\$Configuration\net10.0\mbfTwain.FakeScannerPipeServer.dll"
    $imagePath = Join-Path $Root "build\test-assets\page1.bmp"
    $stdoutLog = Join-Path $Root "build\verify-$Platform-ui-xfercount.out.log"
    $stderrLog = Join-Path $Root "build\verify-$Platform-ui-xfercount.err.log"
    Assert-File $dsPath "DS missing"
    Assert-File $smokePath "SmokeDsEntry missing"
    Assert-File $fakeServerDll "FakeScannerPipeServer missing"
    Assert-File $imagePath "XferCount smoke image missing"

    Get-Process -Name "mbfTwain.VirtualScannerConfig" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue

    $server = Start-Process `
        -FilePath "dotnet" `
        -ArgumentList @(
            $fakeServerDll,
            "--image", $imagePath,
            "--image", $imagePath,
            "--image", $imagePath,
            "--scan", "0",
            "--scan-after-begin-delay-ms", "200",
            "--connections", "120",
            "--revision", "84"
        ) `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru `
        -WindowStyle Hidden

    try {
        Start-Sleep -Milliseconds 150
        Write-Step "UI xfercount smoke $Platform"
        Invoke-WithTemporaryEnvironment @{
            MBF_TWAIN_FORCE_UI = "1"
            MBF_SMOKE_EXPECT_XFERREADY = "1"
            MBF_SMOKE_EXPECT_ENABLE_CALLBACK = "1"
            MBF_SMOKE_EXPECT_PAPER_A3 = $null
            MBF_SMOKE_SET_XFERCOUNT = "2"
            MBF_SMOKE_EXPECT_TRANSFER_TOTAL = "2"
            MBF_SMOKE_USE_MEMORY = $null
        } {
            Invoke-Checked {
                & $smokePath $dsPath
            } "$Platform xfercount SmokeDsEntry"
        }
    }
    finally {
        if ($null -ne $server -and -not $server.HasExited) {
            Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
            $server.WaitForExit()
        }
    }

    if (-not (Select-String -LiteralPath $stderrLog -Pattern "HIDE_SCAN_UI" -Quiet)) {
        throw "UI xfercount smoke $Platform did not observe HIDE_SCAN_UI before scan acknowledgement. See $stderrLog"
    }
}

function Install-Platform([string]$Platform, [string]$Destination) {
    $source = Join-Path $Root "build\manual\$Platform\$Configuration"
    Assert-Directory $source "Source build output missing"
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null

    $files = @(
        "mbfVirtualTwainDS.ds",
        "mbfTwain.VirtualScannerConfig.exe",
        "mbfTwain.VirtualScannerConfig.dll",
        "mbfTwain.VirtualScannerConfig.deps.json",
        "mbfTwain.VirtualScannerConfig.runtimeconfig.json"
    )

    foreach ($file in $files) {
        $sourceFile = Join-Path $source $file
        Assert-File $sourceFile "Install source file missing"
        Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $Destination $file) -Force
    }

    Write-Host "Installed $Platform TWAIN source to $Destination"
}

function Install-LocalTwain {
    Write-Step "Installing to local TWAIN directories"

    if (-not (Test-IsAdministrator)) {
        throw "Installing to C:\Windows\twain_32 and C:\Windows\twain_64 requires Administrator. Re-run this script from an elevated PowerShell."
    }

    Get-Process -Name "mbfTwain.VirtualScannerConfig" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    Install-Platform "Win32" "C:\Windows\twain_32"
    Install-Platform "x64" "C:\Windows\twain_64"

    if (-not $NoForceUi) {
        [Environment]::SetEnvironmentVariable("MBF_TWAIN_FORCE_UI", "1", "Machine")
        Write-Host "Set machine environment variable MBF_TWAIN_FORCE_UI=1"
    }
}

Push-Location $Root
try {
    Assert-File $DsProject "Native project missing"
    Assert-File $UiProject "UI project missing"
    Assert-Directory $TwainHeaderDir "TWAIN header directory missing"

    $scopeSdk = Get-ScopeCppSdk
    $kitRoot = Get-WindowsKit10Root
    $kitVersion = Get-WindowsKitVersion $kitRoot
    $msvcToolRoot = Get-Vs18MsvcToolRoot

    Write-Host "ScopeCppSDK: $scopeSdk"
    Write-Host "Windows Kit: $kitRoot $kitVersion"
    Write-Host "MSVC Tool Root: $msvcToolRoot"

    Build-Managed
    Build-NativeX64 $scopeSdk
    Build-NativeWin32 $scopeSdk $msvcToolRoot $kitRoot $kitVersion
    Stage-UiRuntime

    if (-not $SkipSmoke) {
        Run-Smoke "x64"
        Run-Smoke "Win32"
        Run-UiDelayedReadySmoke "x64"
        Run-UiDelayedReadySmoke "Win32"
        Run-UiCloseWithoutSelectionSmoke "x64"
        Run-UiCloseWithoutSelectionSmoke "Win32"
        Run-UiXferCountSmoke "x64"
        Run-UiXferCountSmoke "Win32"
    }

    if ($BuildOnly) {
        Write-Step "Build output ready"
        Get-ChildItem (Join-Path $Root "build\manual\Win32\$Configuration"), (Join-Path $Root "build\manual\x64\$Configuration") -File |
            Where-Object { $_.Name -like "mbfVirtualTwainDS.*" -or $_.Name -like "mbfTwain.VirtualScannerConfig.*" } |
            Select-Object FullName, Length, LastWriteTime |
            Format-Table -AutoSize
        return
    }

    Install-LocalTwain

    Write-Step "Installed files"
    Get-ChildItem C:\Windows\twain_32, C:\Windows\twain_64 -File |
        Where-Object { $_.Name -like "mbfVirtualTwainDS.*" -or $_.Name -like "mbfTwain.VirtualScannerConfig.*" } |
        Select-Object FullName, Length, LastWriteTime |
        Format-Table -AutoSize
}
finally {
    Pop-Location
}
