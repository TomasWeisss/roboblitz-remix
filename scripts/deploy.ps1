<#
.SYNOPSIS
    Installs the RTX Remix runtime and this repo's game files into the RoboBlitz install.
.PARAMETER SkipRuntime
    Only copy the repo's game\ files (e.g. after editing rtx.conf); leave the Remix runtime as is.
#>
param(
    [switch]$SkipRuntime
)

. "$PSScriptRoot\Common.ps1"

$config   = Get-LocalConfig
$binaries = Join-Path $config.GameDir 'Binaries'

# 1. Remix runtime: bridge client d3d9.dll + .trex\ (renderer, DLSS, USD libraries)
if (-not $SkipRuntime) {
    $bridge = Join-Path $config.RemixDir 'd3d9.dll'
    $trex   = Join-Path $config.RemixDir '.trex'
    if (-not (Test-Path $bridge) -or -not (Test-Path $trex)) {
        throw "RemixDir does not look like a Remix runtime release (needs d3d9.dll and .trex\): $($config.RemixDir)"
    }

    Write-Host "Installing Remix runtime from $($config.RemixDir)"
    Copy-Item $bridge (Join-Path $binaries 'd3d9.dll') -Force
    Sync-Folder $trex (Join-Path $binaries '.trex')
}

# 2. Repo game\ folder mirrors the install layout; copy it over the game
$gameFiles = Join-Path $RepoRoot 'game'
Get-ChildItem $gameFiles -Recurse -File | Where-Object { $_.Name -ne '.gitkeep' } | ForEach-Object {
    $relative = $_.FullName.Substring($gameFiles.Length + 1)
    $target   = Join-Path $config.GameDir $relative
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item $_.FullName $target -Force
    Write-Host "  $relative"
}

Write-Host "Deployed to $($config.GameDir)"
