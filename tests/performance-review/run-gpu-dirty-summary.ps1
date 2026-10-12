$ErrorActionPreference='Stop'
$dirtyRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dirtyOut=Join-Path $dirtyRepo 'build/performance-live-20261011/current-dirty-tests'
New-Item -ItemType Directory -Force -Path $dirtyOut | Out-Null
$dirtyEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($dirtyLine in $dirtyEnvironment){if($dirtyLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 "/I$dirtyRepo/src" "/I$dirtyRepo/external/boost" (Join-Path $PSScriptRoot 'gpu-dirty-summary.cpp') "/Fo$dirtyOut/summary.obj" "/Fe$dirtyOut/summary.exe" *> (Join-Path $dirtyOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $dirtyOut 'build.log') -Tail 20;throw 'Dirty summary fixture build failed'}
$dirtyPrevious=$env:NXEMU_CURRENT_GPU_DIRTY_FILTER
try {
    foreach($dirtyMode in @($null,'0','1','true','10')) {
        $env:NXEMU_CURRENT_GPU_DIRTY_FILTER=$dirtyMode
        $dirtyLabel=if($null -eq $dirtyMode){'unset'}else{$dirtyMode}
        $dirtyResult=Join-Path $dirtyOut "mode-$dirtyLabel-results.txt"
        & (Join-Path $dirtyOut 'summary.exe') > $dirtyResult
        if($LASTEXITCODE -ne 0){Get-Content $dirtyResult;throw 'Dirty summary oracle failed'}
        Get-Content $dirtyResult -Tail 1
    }
} finally {$env:NXEMU_CURRENT_GPU_DIRTY_FILTER=$dirtyPrevious}
