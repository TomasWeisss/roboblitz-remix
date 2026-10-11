<#
.SYNOPSIS
    Builds the d3d9 proxy and installs it, the RTX Remix runtime and this repo's game files
    into the RoboBlitz install.
.DESCRIPTION
    The game loads Binaries\d3d9.dll. With the proxy, that is the proxy and the Remix bridge
    is installed next to it as d3d9_remix.dll.
.PARAMETER SkipRuntime
    Do not sync the Remix renderer (.trex\). Faster when only repo files changed.
.PARAMETER NoProxy
    Install the Remix bridge as d3d9.dll without the proxy (plain Remix, for comparison).
.PARAMETER Configuration
    Proxy build configuration: Release (default) or Debug.
#>
param(
    [switch]$SkipRuntime,
    [switch]$NoProxy,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

. "$PSScriptRoot\Common.ps1"

$config   = Get-LocalConfig
$binaries = Join-Path $config.GameDir 'Binaries'

$bridge = Join-Path $config.RemixDir 'd3d9.dll'
$trex   = Join-Path $config.RemixDir '.trex'
if (-not (Test-Path $bridge) -or -not (Test-Path $trex)) {
    throw "RemixDir does not look like a Remix runtime release (needs d3d9.dll and .trex\): $($config.RemixDir)"
}

# 1. Remix renderer, DLSS, USD libraries
if (-not $SkipRuntime) {
    Write-Host "Syncing Remix runtime from $($config.RemixDir)"
    Sync-Folder $trex (Join-Path $binaries '.trex')
}

# 2. d3d9.dll: proxy + renamed bridge, or the bridge alone
$remixDll = Join-Path $binaries 'd3d9_remix.dll'
if ($NoProxy) {
    Write-Host 'Installing Remix bridge as d3d9.dll (no proxy)'
    Copy-Item $bridge (Join-Path $binaries 'd3d9.dll') -Force
    if (Test-Path $remixDll) { Remove-Item $remixDll }
}
else {
    $proxy = & "$PSScriptRoot\build.ps1" -Configuration $Configuration
    Write-Host 'Installing proxy as d3d9.dll, Remix bridge as d3d9_remix.dll'
    Copy-Item $proxy (Join-Path $binaries 'd3d9.dll') -Force
    Copy-Item $bridge $remixDll -Force
}

# 3. Repo game\ folder mirrors the install layout; copy it over the game
$gameFiles = Join-Path $RepoRoot 'game'
Get-ChildItem $gameFiles -Recurse -File | Where-Object { $_.Name -ne '.gitkeep' } | ForEach-Object {
    $relative = $_.FullName.Substring($gameFiles.Length + 1)
    $target   = Join-Path $config.GameDir $relative
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    Copy-Item $_.FullName $target -Force
    Write-Host "  $relative"
}

Write-Host "Deployed to $($config.GameDir)"
