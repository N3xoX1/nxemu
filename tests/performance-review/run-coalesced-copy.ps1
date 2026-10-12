$ErrorActionPreference='Stop'
$copyRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$copyOut=Join-Path $copyRepo 'build/performance-live-20261011/coalesced-tests'
& 'C:/Users/fabri/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' "$PSScriptRoot/extract-device-copy.py"
if($LASTEXITCODE -ne 0){throw 'Device copy extraction failed'}
$copyVcvars='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
$copyEnvironment=& cmd.exe /d /c "call `"$copyVcvars`" >nul && set"
foreach($copyLine in $copyEnvironment){if($copyLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNDEBUG "/I$copyOut" "/I$copyRepo/src" "$PSScriptRoot/coalesced-copy.cpp" "/Fo$copyOut/coalesced-copy.obj" "/Fe$copyOut/coalesced-copy.exe" *> "$copyOut/build.log"
if($LASTEXITCODE -ne 0){Get-Content "$copyOut/build.log";throw 'Readback copy fixture build failed'}
& "$copyOut/coalesced-copy.exe" > "$copyOut/results.txt"
$copyExit=$LASTEXITCODE
Get-Content "$copyOut/results.txt"
if($copyExit -ne 0){throw 'Readback copy oracle failed'}
