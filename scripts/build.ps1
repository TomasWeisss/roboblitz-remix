<#
.SYNOPSIS
    Builds the d3d9 proxy (32-bit) and runs its tests.
.PARAMETER Configuration
    Release (default) or Debug.
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

. "$PSScriptRoot\Common.ps1"

$source = Join-Path $RepoRoot 'proxy'
$build  = Join-Path $RepoRoot 'build\proxy'

# Tool output goes to the console; the script's only pipeline output is the DLL path
function Invoke-Native([string]$Description, [scriptblock]$Command) {
    & $Command | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Description failed ($LASTEXITCODE)" }
}

if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    Invoke-Native 'CMake configure' { cmake -S $source -B $build -G 'Visual Studio 17 2022' -A Win32 }
}
Invoke-Native 'Build' { cmake --build $build --config $Configuration }
Invoke-Native 'Tests' { ctest --test-dir $build -C $Configuration --output-on-failure }

Join-Path $build "$Configuration\d3d9.dll"
