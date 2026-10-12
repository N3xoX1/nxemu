$ErrorActionPreference='Stop'
$auditRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$auditOut=Join-Path $auditRepo 'build/performance-live-20261011/retirement-audit-tests'
New-Item -ItemType Directory -Force -Path $auditOut | Out-Null
& 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' (Join-Path $PSScriptRoot 'extract-retirement-audit.py')
if($LASTEXITCODE -ne 0){throw 'Retirement/query extraction failed'}
$auditEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($auditLine in $auditEnvironment){if($auditLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 "/I$auditRepo/src" "/I$auditOut" (Join-Path $PSScriptRoot 'retirement-audit.cpp') "/Fo$auditOut/audit.obj" "/Fe$auditOut/audit.exe" *> (Join-Path $auditOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $auditOut 'build.log') -Tail 20;throw 'Retirement audit build failed'}
$auditPrevious=$env:NXEMU_PARALLEL_READBACK_THREADS
try{
    foreach($auditMode in @('0','4')){
        $env:NXEMU_PARALLEL_READBACK_THREADS=$auditMode
        & (Join-Path $auditOut 'audit.exe') > (Join-Path $auditOut "mode-$auditMode-results.txt")
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $auditOut "mode-$auditMode-results.txt");throw 'Retirement audit oracle failed'}
        Get-Content (Join-Path $auditOut "mode-$auditMode-results.txt")
    }
}finally{$env:NXEMU_PARALLEL_READBACK_THREADS=$auditPrevious}
