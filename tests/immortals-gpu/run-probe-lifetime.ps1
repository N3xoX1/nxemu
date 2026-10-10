$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOut=Join-Path $taskRoot 'build\immortals-menu-diagnostic-20261009\capture-analysis'
& 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' (Join-Path $PSScriptRoot 'extract-probe-lifetime.py')
if($LASTEXITCODE -ne 0){throw 'Probe extraction failed'}
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
foreach($taskVariant in @('baseline','fixed')){
 $taskBody=Join-Path $taskOut "probe-lifetime-$taskVariant.inc"
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG "/I$taskRoot\src" /I'C:\VulkanSDK\1.4.357.0\Include' "/DPROBE_BODY=`"$taskBody`"" (Join-Path $PSScriptRoot 'probe-lifetime.cpp') "/Fo$taskOut\probe-lifetime-$taskVariant.obj" "/Fe$taskOut\probe-lifetime-$taskVariant.exe" /link user32.lib *> (Join-Path $taskOut "probe-lifetime-$taskVariant-build.log")
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOut "probe-lifetime-$taskVariant-build.log");throw 'Probe test compile failed'}
 & (Join-Path $taskOut "probe-lifetime-$taskVariant.exe") *> (Join-Path $taskOut "probe-lifetime-$taskVariant-results.log")
 $taskExit=$LASTEXITCODE
 Get-Content (Join-Path $taskOut "probe-lifetime-$taskVariant-results.log")
 $taskExpected=if($taskVariant -eq 'baseline'){1}else{0}
 if($taskExit -ne $taskExpected){throw 'Unexpected Vulkan probe lifetime test result'}
}
