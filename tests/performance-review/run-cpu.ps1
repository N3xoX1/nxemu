$ErrorActionPreference = 'Stop'
$reviewRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$reviewOut = Join-Path $reviewRoot 'build/performance-review-20261011'
$reviewExtracted = Join-Path $reviewRoot 'build/pr396-corrections/tests'
New-Item -ItemType Directory -Force -Path $reviewOut | Out-Null
$reviewPython = 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $reviewPython (Join-Path $reviewRoot 'tests/pr396/generate-tests.py')
if ($LASTEXITCODE -ne 0) { throw 'Production extraction failed' }
$reviewVcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$reviewEnvironment = & cmd.exe /d /c "call `"$reviewVcvars`" >nul && set"
foreach ($reviewLine in $reviewEnvironment) {
    if ($reviewLine -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /DPR_MODE=1 /DFIXED_MODE=1 "/I$reviewRoot/src" "/I$reviewRoot/external/boost" "/I$reviewExtracted" (Join-Path $PSScriptRoot 'cpu.cpp') "/Fo$reviewOut/cpu.obj" "/Fe$reviewOut/cpu.exe" *> (Join-Path $reviewOut 'cpu-build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $reviewOut 'cpu-build.log'); throw 'CPU benchmark build failed' }
& (Join-Path $reviewOut 'cpu.exe') > (Join-Path $reviewOut 'cpu-scaling.log')
$reviewExit = $LASTEXITCODE
Get-Content (Join-Path $reviewOut 'cpu-scaling.log')
if ($reviewExit -ne 0) { throw 'CPU performance fixture failed correctness checks' }
