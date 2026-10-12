<#
.SYNOPSIS
    Bakes PBR materials from all Remix captures and material dumps and applies them to the Toolkit
    project (tools\material_bake).
.DESCRIPTION
    Sets up the Python environment in .venv on first use. bake.py writes build\material_bake with a
    report.html; apply.py ingests the textures through the running Toolkit (project open) and writes
    remix-project\generated_materials.usda, a sublayer of mod.usda.
.PARAMETER BakeOnly
    Only bake; do not ingest or touch the project (the Toolkit need not be running).
.PARAMETER NormalStrength
    Scale of normal map slopes: 0 flat, 1 as in the game. Default: bake.py's (0.5).
#>
param(
    [switch]$BakeOnly,
    [double]$NormalStrength = -1
)

. "$PSScriptRoot\Common.ps1"

$tools        = Join-Path $RepoRoot 'tools\material_bake'
$venv         = Join-Path $RepoRoot '.venv'
$python       = Join-Path $venv 'Scripts\python.exe'
$requirements = Join-Path $tools 'requirements.txt'
$stamp        = Join-Path $venv 'requirements.txt'

function Invoke-Python([string[]]$Arguments) {
    & $python @Arguments
    if ($LASTEXITCODE -ne 0) { throw "python $($Arguments -join ' ') failed ($LASTEXITCODE)" }
}

# Python environment: created once, updated when the requirements change
if (-not (Test-Path $python)) {
    $launcher = Get-Command py -ErrorAction SilentlyContinue
    Write-Host 'Creating .venv'
    if ($launcher) { & py -3 -m venv $venv } else { & python -m venv $venv }
    if ($LASTEXITCODE -ne 0) { throw 'Creating .venv failed: install Python 3 (python.org)' }
}
if (-not (Test-Path $stamp) -or (Get-FileHash $stamp).Hash -ne (Get-FileHash $requirements).Hash) {
    Write-Host 'Installing Python requirements'
    Invoke-Python @('-m', 'pip', 'install', '--quiet', '--disable-pip-version-check', '-r', $requirements)
    Copy-Item $requirements $stamp
}

$bakeArguments = @(Join-Path $tools 'bake.py')
if ($NormalStrength -ge 0) { $bakeArguments += @('--normal-strength', "$NormalStrength") }
Invoke-Python $bakeArguments

if (-not $BakeOnly) {
    Invoke-Python @(Join-Path $tools 'apply.py')
}
