$ErrorActionPreference='Stop'
$filterRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$filterOut=Join-Path $filterRepo 'build/performance-live-20261011/cache-filter-tests'
New-Item -ItemType Directory -Force -Path $filterOut | Out-Null
& 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' (Join-Path $PSScriptRoot 'extract-cache-filter.py')
if($LASTEXITCODE -ne 0){throw 'Cache predicate extraction failed'}
$filterEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($filterLine in $filterEnvironment){if($filterLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
foreach($filterSource in @('gpu-write-history','cache-filter')) {
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 "/I$filterRepo/src" "/I$filterRepo/external/boost" "/I$filterOut" (Join-Path $PSScriptRoot "$filterSource.cpp") "/Fo$filterOut/$filterSource.obj" "/Fe$filterOut/$filterSource.exe" *> (Join-Path $filterOut "$filterSource-build.log")
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $filterOut "$filterSource-build.log") -Tail 20;throw 'Cache fixture build failed'}
}
& (Join-Path $filterOut 'gpu-write-history.exe') --benchmark > (Join-Path $filterOut 'history-results.txt')
if($LASTEXITCODE -ne 0){throw 'History oracle failed'}
Get-Content (Join-Path $filterOut 'history-results.txt')
$filterPrevious=$env:NXEMU_GPU_WRITE_FILTER
try {
    foreach($filterMode in @('default','0','1','invalid')) {
        $env:NXEMU_GPU_WRITE_FILTER=if($filterMode -eq 'default'){$null}else{$filterMode}
        & (Join-Path $filterOut 'cache-filter.exe') > (Join-Path $filterOut "mode-$filterMode-results.txt")
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $filterOut "mode-$filterMode-results.txt");throw 'Cache predicate oracle failed'}
        $filterExpected=if($filterMode -in @('default','1')){1}else{0}
        if(!(Select-String -LiteralPath (Join-Path $filterOut "mode-$filterMode-results.txt") -Pattern "filter_enabled=$filterExpected$")){throw 'Unexpected filter configuration'}
        Get-Content (Join-Path $filterOut "mode-$filterMode-results.txt")
    }
} finally {$env:NXEMU_GPU_WRITE_FILTER=$filterPrevious}
