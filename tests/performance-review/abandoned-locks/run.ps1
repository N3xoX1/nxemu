$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path "$PSScriptRoot/../../..").Path
$taskOut=Join-Path $taskRoot 'build/performance-review/abandoned-locks'
& 'C:/Users/fabri/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' "$PSScriptRoot/extract.py"
if($LASTEXITCODE){throw 'Production extraction failed'}
$taskVcvars='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
$taskEnv=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnv){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/I$taskOut" "$PSScriptRoot/handoff.cpp" "/Fo$taskOut/handoff.obj" "/Fe$taskOut/handoff.exe" *> "$taskOut/build.log"
if($LASTEXITCODE){Get-Content "$taskOut/build.log" -Tail 20;throw 'Handoff fixture compilation failed'}
& "$taskOut/handoff.exe" | Tee-Object -FilePath "$taskOut/results.txt"
if($LASTEXITCODE){throw 'Handoff regression failed'}
