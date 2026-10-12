$ErrorActionPreference='Stop'
$diffRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$diffOut=Join-Path $diffRepo 'build/performance-live-20261011/differential-tests'
New-Item -ItemType Directory -Force -Path $diffOut | Out-Null
$diffVcvars='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
$diffEnvironment=& cmd.exe /d /c "call `"$diffVcvars`" >nul && set"
foreach($diffLine in $diffEnvironment){if($diffLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNDEBUG "/I$diffRepo/src" "$PSScriptRoot/differential-copy.cpp" "/Fo$diffOut/differential-copy.obj" "/Fe$diffOut/differential-copy.exe" *> "$diffOut/build.log"
if($LASTEXITCODE -ne 0){Get-Content "$diffOut/build.log";throw 'Differential copy fixture build failed'}
& "$diffOut/differential-copy.exe" > "$diffOut/results.txt"
$diffExit=$LASTEXITCODE
Get-Content "$diffOut/results.txt"
if($diffExit -ne 0){throw 'Differential copy oracle failed'}
