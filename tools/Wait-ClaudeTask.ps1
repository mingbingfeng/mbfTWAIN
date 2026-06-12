param(
    [Parameter(Mandatory = $true)]
    [string]$TaskId,
    [string]$StatePath,
    [int]$TimeoutSeconds = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($StatePath)) {
    $StatePath = Join-Path (Get-Location) ".codex_delegate\state\$TaskId.json"
}

$StatePath = [System.IO.Path]::GetFullPath($StatePath)
$StateDirectory = Split-Path -Parent $StatePath
$StateFileName = Split-Path -Leaf $StatePath

if (-not (Test-Path -LiteralPath $StateDirectory -PathType Container)) {
    throw "State directory does not exist: $StateDirectory"
}

function Read-StateFile {
    if (-not (Test-Path -LiteralPath $StatePath -PathType Leaf)) {
        return $null
    }

    try {
        $raw = Get-Content -LiteralPath $StatePath -Raw -Encoding UTF8
        if ([string]::IsNullOrWhiteSpace($raw)) {
            return $null
        }

        return $raw | ConvertFrom-Json
    }
    catch {
        return $null
    }
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$watcher = [System.IO.FileSystemWatcher]::new($StateDirectory, $StateFileName)
$watcher.NotifyFilter = [System.IO.NotifyFilters]'FileName, LastWrite, CreationTime, Size'
$watcher.EnableRaisingEvents = $true

try {
    while ((Get-Date) -lt $deadline) {
        $state = Read-StateFile
        if ($null -ne $state -and $state.status -ne "running") {
            $state | ConvertTo-Json -Depth 5
            exit 0
        }

        $remainingMilliseconds = [Math]::Max(
            1,
            [int][Math]::Min(
                ($deadline - (Get-Date)).TotalMilliseconds,
                30000))

        $null = $watcher.WaitForChanged([System.IO.WatcherChangeTypes]::All, $remainingMilliseconds)

        Start-Sleep -Milliseconds 150
    }

    throw "Timed out waiting for Claude task state to leave 'running': $StatePath"
}
finally {
    $watcher.Dispose()
}
