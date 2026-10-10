$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $repo 'build\pr396-corrections\tests'
$python = 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $python (Join-Path $PSScriptRoot 'generate-tests.py')
if ($LASTEXITCODE -ne 0) { throw 'Source extraction failed' }
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$environment = & cmd.exe /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
foreach ($variant in @('master','pr','fixed')) {
    $define = if ($variant -eq 'master') { '/DPR_MODE=0' } else { '/DPR_MODE=1' }
    $fixedDefine = if ($variant -eq 'fixed') { '/DFIXED_MODE=1' } else { '/DFIXED_MODE=0' }
    $src = Join-Path $repo 'src'
    $exe = Join-Path $out "$variant-tests.exe"
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG $define $fixedDefine "/I$src" "/I$repo\external\boost" "/I$out" (Join-Path $PSScriptRoot 'synthetic.cpp') "/Fo$out\$variant-tests.obj" "/Fe$exe" *> (Join-Path $out "$variant-test-build.log")
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath (Join-Path $out "$variant-test-build.log") -Tail 50; throw "Compile failed: $variant" }
    & $exe (Join-Path $out 'integration-plan.inc') > (Join-Path $out "$variant-results.txt")
    $code = $LASTEXITCODE
    Get-Content -LiteralPath (Join-Path $out "$variant-results.txt")
    Write-Output "Synthetic invariant test exit code: $code (1 means reproduced failures)"
    if ($variant -eq 'fixed' -and $code -ne 0) { throw 'Corrected code failed invariant tests' }
}
