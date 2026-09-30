param([string] $OutputDirectory, [string] $SourceRoot,
      [ValidateSet('All', 'Timing', 'Memory')] [string] $TestSet = 'All')
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $SourceRoot) { $SourceRoot = $repoRoot }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'build/validation/cpu-timing-tests' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

# Extract production code. The harness replaces the two clock inputs, never the
# method logic, so wall corrections can be simulated without changing host time.
function Extract-Function([string] $Source, [string] $Signature) {
    $start = $Source.IndexOf($Signature)
    if ($start -lt 0) { throw "Missing signature: $Signature" }
    $end = $Source.IndexOf("`n}", $start)
    if ($end -lt 0) { throw "Missing closing brace: $Signature" }
    return $Source.Substring($start, $end - $start).TrimEnd() + "`n}"
}
$timing = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/nxemu-os/core/core_timing.cpp'))
$methods = (Extract-Function $timing 'void CoreTiming::AddTicks(u64 ticks_to_add)') + "`n" + (Extract-Function $timing 'void CoreTiming::ResetTicks()')
Set-Content -LiteralPath (Join-Path $OutputDirectory 'core_timing_ticks.inc') -Value $methods
$clockSource = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/yuzu_common/wall_clock.cpp'))
$start = $clockSource.IndexOf('class StandardWallClock final')
$end = $clockSource.IndexOf("`n};", $start)
if ($start -lt 0 -or $end -lt 0) { throw 'Missing StandardWallClock class.' }
$clock = $clockSource.Substring($start, $end - $start).TrimEnd() + "`n};"
$clock = $clock.Replace('std::chrono::system_clock::now()', 'FakeSystemClock::now()').Replace('std::chrono::steady_clock::now()', 'FakeSteadyClock::now()')
Set-Content -LiteralPath (Join-Path $OutputDirectory 'standard_wall_clock.inc') -Value $clock
$cpu = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/nxemu-cpu/arm_dynarmic_64.cpp'))
$check = [regex]::Match($cpu, '(?s)    bool CheckMemoryAccess\(uint64_t [^\r\n]+\)\s*\{.*?\n    \}')
if (-not $check.Success) { throw 'Missing A64 memory check.' }
Set-Content -LiteralPath (Join-Path $OutputDirectory 'memory_abort64.inc') -Value $check.Value

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
$setup = ''
if (-not $compiler) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Run from a Visual Studio x64 developer shell.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
    $setup = '@call "' + (Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat') + '" >nul' + [Environment]::NewLine + '@if errorlevel 1 exit /b %errorlevel%' + [Environment]::NewLine
}
$tests = switch ($TestSet) {
    'Timing' { @('timing_contracts') }
    'Memory' { @('memory_abort64') }
    default { @('timing_contracts', 'memory_abort64') }
}
foreach ($name in $tests) {
    $arguments = @('/nologo', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4',
        ('/I"' + $SourceRoot + '\src"'), ('/I"' + $OutputDirectory + '"'),
        ('"' + $PSScriptRoot + '\' + $name + '.cpp"'), ('/Fe:' + $name + '.exe'))
    Set-Content -LiteralPath (Join-Path $OutputDirectory "$name.rsp") -Value $arguments
    $command = $setup + '@cl @' + $name + '.rsp' + [Environment]::NewLine + '@if errorlevel 1 exit /b %errorlevel%' + [Environment]::NewLine + '@' + $name + '.exe' + [Environment]::NewLine + '@exit /b %errorlevel%'
    Set-Content -LiteralPath (Join-Path $OutputDirectory "$name.cmd") -Value $command
    Push-Location -LiteralPath $OutputDirectory
    try {
        & cmd.exe /d /c "$name.cmd"
        if ($LASTEXITCODE -ne 0) { throw "$name tests failed ($LASTEXITCODE)." }
    } finally { Pop-Location }
}
