$ErrorActionPreference='Stop'
$downloadRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$downloadOut=Join-Path $downloadRepo 'build/performance-live-20261011/download-tests'
& 'C:/Users/fabri/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' "$PSScriptRoot/extract-downloads.py"
if($LASTEXITCODE -ne 0){throw 'Download function extraction failed'}
$downloadVcvars='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
$downloadEnvironment=& cmd.exe /d /c "call `"$downloadVcvars`" >nul && set"
foreach($downloadLine in $downloadEnvironment){if($downloadLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNDEBUG /DFIXED_MODE=1 /DPR_MODE=1 "/I$downloadOut" "/I$downloadRepo/build/pr396-corrections/tests" "/I$downloadRepo/src" "/I$downloadRepo/src/yuzu_video_core" "/I$downloadRepo/external/boost" "/I$downloadRepo/external/fmt/include" "$PSScriptRoot/downloads.cpp" "/Fo$downloadOut/downloads.obj" "/Fe$downloadOut/downloads.exe" *> "$downloadOut/build.log"
if($LASTEXITCODE -ne 0){Get-Content "$downloadOut/build.log";throw 'Download fixture build failed'}
& "$downloadOut/downloads.exe" > "$downloadOut/results.txt"
$downloadExit=$LASTEXITCODE
Get-Content "$downloadOut/results.txt"
if($downloadExit -ne 0){throw 'Download oracle failed'}
