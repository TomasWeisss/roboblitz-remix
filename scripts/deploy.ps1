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
.PARAMETER NoShaderPatches
    Put the game's original shaders back instead of applying shaders\patches\*.patch.
#>
param(
    [switch]$SkipRuntime,
    [switch]$NoProxy,
    [switch]$NoShaderPatches,
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

# 4. Shader patches. The game compiles Engine\Shaders\*.usf itself, so patched sources take
#    effect once its shader cache is rebuilt. Pristine copies of the patched files are kept in
#    shaders\original\ (saved from the game on first use); every deploy patches those afresh.
$patchMarker = 'RTX Remix'
$patches   = @(Get-ChildItem (Join-Path $RepoRoot 'shaders\patches') -Filter '*.patch')
$originals = Join-Path $RepoRoot 'shaders\original'
$staging   = Join-Path $RepoRoot 'build\shaders'

$shaderFiles = $patches | Select-String '^diff --git a/(\S+) ' | ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Sort-Object -Unique
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
foreach ($relative in $shaderFiles) {
    $original = Join-Path $originals $relative
    $inGame   = Join-Path $config.GameDir $relative
    if (-not (Test-Path $original)) {
        if (Select-String -Path $inGame -SimpleMatch $patchMarker -Quiet) {
            throw "$inGame is already patched and no original is saved. Restore it (Steam: Verify integrity of game files), then deploy again."
        }
        New-Item -ItemType Directory -Force (Split-Path $original) | Out-Null
        Copy-Item $inGame $original
        Write-Host "Saved original $relative"
    }
    $staged = Join-Path $staging $relative
    New-Item -ItemType Directory -Force (Split-Path $staged) | Out-Null
    Copy-Item $original $staged
}
if (-not $NoShaderPatches) {
    foreach ($patch in $patches) {
        git -C $RepoRoot apply --directory=build/shaders $patch.FullName
        if ($LASTEXITCODE -ne 0) { throw "Applying $($patch.Name) failed" }
    }
}

$shadersChanged = $false
foreach ($relative in $shaderFiles) {
    $staged = Join-Path $staging $relative
    $inGame = Join-Path $config.GameDir $relative
    if ((Get-FileHash $staged).Hash -ne (Get-FileHash $inGame).Hash) {
        Copy-Item $staged $inGame -Force
        Write-Host "  $relative ($(if ($NoShaderPatches) { 'original' } else { 'patched' }))"
        $shadersChanged = $true
    }
}
$shaderCache = Join-Path $config.GameDir 'RoboGame\Content\LocalShaderCache.upk'
if ($shadersChanged -and (Test-Path $shaderCache)) {
    Remove-Item $shaderCache
    Write-Host 'Shaders changed: deleted LocalShaderCache.upk, the game recompiles shaders on its next start (slow once)'
}

Write-Host "Deployed to $($config.GameDir)"
