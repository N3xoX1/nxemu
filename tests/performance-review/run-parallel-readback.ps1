$ErrorActionPreference='Stop'
$parallelRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$parallelOut=Join-Path $parallelRepo 'build/performance-live-20261011/parallel-tests'
New-Item -ItemType Directory -Force -Path $parallelOut | Out-Null
$parallelEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($parallelLine in $parallelEnvironment){if($parallelLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 "/I$parallelRepo/src" (Join-Path $PSScriptRoot 'parallel-readback.cpp') "/Fo$parallelOut/parallel.obj" "/Fe$parallelOut/parallel.exe" *> (Join-Path $parallelOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $parallelOut 'build.log') -Tail 15;throw 'Parallel readback build failed'}
$parallelPrevious=$env:NXEMU_PARALLEL_READBACK_THREADS
try {
 foreach($parallelMode in @('default','0','2','4','invalid')) {
  $env:NXEMU_PARALLEL_READBACK_THREADS=if($parallelMode -eq 'default'){$null}else{$parallelMode}
  & (Join-Path $parallelOut 'parallel.exe') *> (Join-Path $parallelOut "mode-$parallelMode-results.txt")
  if($LASTEXITCODE -ne 0){Get-Content (Join-Path $parallelOut "mode-$parallelMode-results.txt") -Tail 10;throw 'Parallel readback oracle failed'}
  Get-Content (Join-Path $parallelOut "mode-$parallelMode-results.txt") -Tail 1
 }
}finally{$env:NXEMU_PARALLEL_READBACK_THREADS=$parallelPrevious}
