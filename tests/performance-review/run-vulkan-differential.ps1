$ErrorActionPreference='Stop'
$diffRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$diffOut=Join-Path $diffRepo 'build/performance-live-20261011/differential-tests'
$diffSdk='C:/VulkanSDK/1.4.357.0'
$diffVcvars='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
$diffEnvironment=& cmd.exe /d /c "call `"$diffVcvars`" >nul && set"
foreach($diffLine in $diffEnvironment){if($diffLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNDEBUG "/I$diffRepo/src" "/I$diffRepo/build/pr396-corrections/tests" "/I$diffSdk/Include" "$PSScriptRoot/vulkan-differential-copy.cpp" "/Fo$diffOut/vulkan-differential-copy.obj" "/Fe$diffOut/vulkan-differential-copy.exe" /link "$diffSdk/Lib/vulkan-1.lib" *> "$diffOut/vulkan-build.log"
if($LASTEXITCODE -ne 0){Get-Content "$diffOut/vulkan-build.log";throw 'GPU differential fixture build failed'}
& "$diffOut/vulkan-differential-copy.exe" "$diffRepo/build/pr396-corrections/tests/read.spv" > "$diffOut/vulkan-results.txt"
$diffExit=$LASTEXITCODE
Get-Content "$diffOut/vulkan-results.txt"
if($diffExit -ne 0){throw 'GPU differential payload failed'}
