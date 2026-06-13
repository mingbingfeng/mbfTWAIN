param(
    [string]$Version = "1.0.0",
    [string]$Configuration = "Release",
    [string]$InnoSetupPath = "D:\Program Files (x86)\Inno Setup 6",
    [switch]$SkipSmoke
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$InstallerScript = Join-Path $Root "installer\mbfTwain.iss"
$ReleaseDir = Join-Path $Root "build\release"
$InnoCompiler = Join-Path $InnoSetupPath "ISCC.exe"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "== $Message =="
}

function Convert-ToFourPartVersion([string]$SemanticVersion) {
    if ($SemanticVersion -notmatch '^\d+\.\d+\.\d+([\-+].*)?$') {
        throw "Version must use semantic version format, for example 1.0.0. Actual: $SemanticVersion"
    }

    $baseVersion = ($SemanticVersion -split '[-+]')[0]
    return "$baseVersion.0"
}

function Assert-File([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Message`: $Path"
    }
}

Push-Location $Root
try {
    Assert-File $InstallerScript "Inno Setup script missing"
    Assert-File $InnoCompiler "Inno Setup compiler missing"

    New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null

    $buildArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $Root "Install-LocalTwain.ps1"),
        "-Configuration", $Configuration,
        "-Version", $Version,
        "-BuildOnly"
    )
    if ($SkipSmoke) {
        $buildArgs += "-SkipSmoke"
    }

    Write-Step "Building and staging release binaries"
    & powershell.exe @buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Release binary build failed with exit code $LASTEXITCODE"
    }

    Write-Step "Compiling Inno Setup installer"
    $fourPartVersion = Convert-ToFourPartVersion $Version
    & $InnoCompiler `
        "/DMyAppVersion=$Version" `
        "/DMyAppVersionFourPart=$fourPartVersion" `
        "/DRepoRoot=$Root" `
        $InstallerScript
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compiler failed with exit code $LASTEXITCODE"
    }

    $installer = Join-Path $ReleaseDir "mbfTwain-Setup-v$Version.exe"
    Assert-File $installer "Installer missing after Inno Setup compile"

    Write-Step "Writing SHA-256 checksum"
    $hash = Get-FileHash -LiteralPath $installer -Algorithm SHA256
    $checksumPath = "$installer.sha256"
    $checksumLine = "$($hash.Hash.ToLowerInvariant())  $(Split-Path -Leaf $installer)"
    Set-Content -LiteralPath $checksumPath -Value $checksumLine -Encoding UTF8

    [pscustomobject]@{
        Version = $Version
        Installer = $installer
        Checksum = $checksumPath
        Sha256 = $hash.Hash.ToLowerInvariant()
    } | Format-List
}
finally {
    Pop-Location
}
