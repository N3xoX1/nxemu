# Usage: ./src/script/build_windows.ps1 [Release|Debug] [build-directory] [CMake options...]
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [Parameter(Position = 1)]
    [string]$BuildDirectory = 'build/windows-x64-ninja',
    [Parameter(Position = 2, ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArguments = @()
)
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') {
    throw 'Run this script on Windows with Visual Studio 2022 C++ tools installed.'
}
$jobs = if ($env:NXEMU_BUILD_JOBS) { $env:NXEMU_BUILD_JOBS } else { '2' }
if ($jobs -notmatch '^[1-9][0-9]*$') {
    throw 'NXEMU_BUILD_JOBS must be a positive integer.'
}

# A regular PowerShell prompt works as well as an x64 developer prompt.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or
    ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH -ne 'x64')) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Visual Studio Installer was not found. Install Visual Studio 2022 with Desktop development with C++.'
    }
    $installation = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) {
        throw 'Visual Studio 2022 C++ tools were not found.'
    }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Host
}
if ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    throw 'The active Visual Studio developer environment must target x64.'
}
foreach ($tool in 'cmake', 'ninja', 'cl.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required build tool not found on PATH: $tool"
    }
}

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Push-Location $repo
try {
    & cmake --preset windows-x64-ninja -B $BuildDirectory @CMakeArguments
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
    & cmake --build $BuildDirectory --config $Configuration --target nxemu --parallel $jobs
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
