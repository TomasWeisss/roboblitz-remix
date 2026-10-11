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
.PARAMETER Clean
    Run the unmodified game: removes the proxy and Remix bridge DLLs from Binaries\ first,
    so the game uses the system d3d9. Run scripts\deploy.ps1 to install them again.
#>
param(
    [switch]$Direct,
    [switch]$Clean,
    [Parameter(ValueFromRemainingArguments)]
    [string[]]$GameArgs
)

. "$PSScriptRoot\Common.ps1"

$config   = Get-LocalConfig
$steamDir = Get-SteamDir
$binaries = Join-Path $config.GameDir 'Binaries'

if ($Clean) {
    foreach ($name in 'd3d9.dll', 'd3d9_remix.dll') {
        $path = Join-Path $binaries $name
        if (Test-Path $path) {
            Remove-Item $path
            Write-Host "Removed $name"
        }
    }
    Write-Host 'Running without proxy and Remix. Run scripts\deploy.ps1 to reinstall them.'
}
elseif (-not (Test-Path (Join-Path $binaries 'd3d9.dll'))) {
    Write-Warning 'No d3d9.dll in Binaries: the game will run without Remix. Run scripts\deploy.ps1 first.'
}

if ($Direct) {
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
