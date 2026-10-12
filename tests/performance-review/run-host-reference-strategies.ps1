$ErrorActionPreference='Stop'
$hostRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$hostOut=Join-Path $hostRepo 'build/performance-live-20261011/host-reference-tests'
New-Item -ItemType Directory -Force -Path $hostOut | Out-Null
$hostEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($hostLine in $hostEnvironment){if($hostLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 "/I$hostRepo/src" (Join-Path $PSScriptRoot 'host-reference-strategies.cpp') "/Fo$hostOut/strategies.obj" "/Fe$hostOut/strategies.exe" *> (Join-Path $hostOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $hostOut 'build.log') -Tail 15;throw 'Host strategies build failed'}
& (Join-Path $hostOut 'strategies.exe') *> (Join-Path $hostOut 'results.txt')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $hostOut 'results.txt') -Tail 20;throw 'Host strategies oracle failed'}
Get-Content (Join-Path $hostOut 'results.txt')
