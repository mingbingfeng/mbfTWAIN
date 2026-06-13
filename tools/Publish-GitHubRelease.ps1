param(
    [string]$Version = "1.0.0",
    [string]$Repo = "mingbingfeng/mbfTWAIN",
    [string]$Branch = "",
    [string]$InstallerPath = "",
    [string]$ChecksumPath = "",
    [string]$NotesPath = "",
    [switch]$SkipPush
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Tag = "v$Version"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "== $Message =="
}

function Invoke-Checked([scriptblock]$Command, [string]$Label) {
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Assert-File([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Message`: $Path"
    }
}

Push-Location $Root
try {
    if ([string]::IsNullOrWhiteSpace($Branch)) {
        $Branch = (& git branch --show-current).Trim()
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Branch)) {
            throw "Could not determine current git branch."
        }
    }

    if ([string]::IsNullOrWhiteSpace($InstallerPath)) {
        $InstallerPath = Join-Path $Root "build\release\mbfTwain-Setup-v$Version.exe"
    }

    if ([string]::IsNullOrWhiteSpace($ChecksumPath)) {
        $ChecksumPath = "$InstallerPath.sha256"
    }

    if ([string]::IsNullOrWhiteSpace($NotesPath)) {
        $NotesPath = Join-Path $Root "docs\release-v$Version.md"
    }

    Assert-File $InstallerPath "Installer asset missing"
    Assert-File $ChecksumPath "Checksum asset missing"
    Assert-File $NotesPath "Release notes file missing"

    Invoke-Checked { gh auth status } "GitHub CLI auth check"
    Invoke-Checked { git diff --quiet } "Uncommitted worktree check"
    Invoke-Checked { git diff --cached --quiet } "Staged changes check"

    if (-not $SkipPush) {
        Write-Step "Pushing branch $Branch"
        Invoke-Checked { git push origin $Branch } "git push origin $Branch"

        $existingTag = (@(& git tag --list $Tag) -join "`n").Trim()
        if ([string]::IsNullOrWhiteSpace($existingTag)) {
            Write-Step "Creating tag $Tag"
            Invoke-Checked { git tag -a $Tag -m "Release $Tag" } "git tag $Tag"
        }

        Write-Step "Pushing tag $Tag"
        Invoke-Checked { git push origin $Tag } "git push origin $Tag"
    }

    Write-Step "Publishing GitHub release $Tag"
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & gh release view $Tag --repo $Repo *> $null
        $releaseExists = $LASTEXITCODE -eq 0
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($releaseExists) {
        Invoke-Checked {
            gh release upload $Tag $InstallerPath $ChecksumPath --repo $Repo --clobber
        } "gh release upload $Tag"
    }
    else {
        Invoke-Checked {
            gh release create $Tag $InstallerPath $ChecksumPath --repo $Repo --title "mbfTwain $Tag" --notes-file $NotesPath
        } "gh release create $Tag"
    }

    $releaseUrl = (& gh release view $Tag --repo $Repo --json url --jq ".url").Trim()
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($releaseUrl)) {
        Write-Host $releaseUrl
    }
}
finally {
    Pop-Location
}
