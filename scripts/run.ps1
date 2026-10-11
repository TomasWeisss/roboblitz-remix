<#
.SYNOPSIS
    Starts RoboBlitz. Extra arguments are passed to RoboGame.exe.
.DESCRIPTION
    RoboGame.exe imports Steam.dll, which lives in the Steam install folder, so the exe
    cannot be started on its own. By default the game is launched through Steam, like the
    Library "Play" button (Steam must be able to start; it uses Binaries\ as working dir).
.PARAMETER Direct
    Start RoboGame.exe directly from Binaries\ with the Steam folder on PATH, so it finds
    Steam.dll. Useful for debugging; Steam still needs to be running.
#>
param(
    [switch]$Direct,
    [Parameter(ValueFromRemainingArguments)]
    [string[]]$GameArgs
)

. "$PSScriptRoot\Common.ps1"

$config   = Get-LocalConfig
$steamDir = Get-SteamDir

if ($Direct) {
    $binaries = Join-Path $config.GameDir 'Binaries'
    # Child processes inherit PATH; Windows searches it for Steam.dll
    $env:PATH = "$steamDir;$env:PATH"
    $startArgs = @{ FilePath = (Join-Path $binaries 'RoboGame.exe'); WorkingDirectory = $binaries }
    if ($GameArgs) { $startArgs.ArgumentList = $GameArgs }
}
else {
    $steamArgs = @('-applaunch', $SteamAppId)
    if ($GameArgs) { $steamArgs += $GameArgs }
    $startArgs = @{ FilePath = (Join-Path $steamDir 'steam.exe'); ArgumentList = $steamArgs }
}

Start-Process @startArgs
