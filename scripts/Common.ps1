# Shared helpers for the scripts in this folder. Dot-source it: . "$PSScriptRoot\Common.ps1"

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Get-LocalConfig {
    $path = Join-Path $RepoRoot 'local.config.psd1'
    if (-not (Test-Path $path)) {
        throw "Missing $path. Copy local.config.example.psd1 to local.config.psd1 and set your paths."
    }
    $config = Import-PowerShellDataFile $path

    $exe = Join-Path $config.GameDir 'Binaries\RoboGame.exe'
    if (-not (Test-Path $exe)) {
        throw "GameDir does not look like a RoboBlitz install (no $exe)."
    }
    return $config
}

# Mirror a folder with robocopy. /MIR also deletes files in $Destination that are not in $Source.
function Sync-Folder([string]$Source, [string]$Destination) {
    robocopy $Source $Destination /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
    # robocopy: exit codes below 8 mean success
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE): $Source -> $Destination" }
    $global:LASTEXITCODE = 0
}

$SteamAppId = 4300  # RoboBlitz

# Steam install folder, from the registry (contains steam.exe and Steam.dll)
function Get-SteamDir {
    $path = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if (-not $path -or -not (Test-Path (Join-Path $path 'steam.exe'))) {
        throw 'Steam install not found (HKCU\Software\Valve\Steam\SteamPath).'
    }
    return [System.IO.Path]::GetFullPath($path)
}
