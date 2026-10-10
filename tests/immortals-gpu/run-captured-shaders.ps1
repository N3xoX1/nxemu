$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOut=Join-Path $taskRoot 'build\immortals-menu-diagnostic-20261009\capture-analysis'
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskIncludes=@("/I$taskRoot\src","/I$taskRoot\src\nxemu-video","/I$taskRoot\external\boost","/I$taskRoot\external\fmt\include","/I$taskRoot\external\sirit\include","/I$taskRoot\external\sirit\externals\SPIRV-Headers\include")
$taskLibs=@('yuzu_shader_recompiler.lib','yuzu_common.lib','common.lib','fmt.lib','sirit.lib') | ForEach-Object {Join-Path $taskRoot "bin\x64\Release\lib\$_"}
$taskExe=Join-Path $taskOut 'shader-replay-fixed.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 @taskIncludes (Join-Path $taskRoot 'tests\pr396\shader-replay.cpp') "/Fo$taskOut\shader-replay-fixed.obj" "/Fe$taskExe" /link @taskLibs user32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib iphlpapi.lib winmm.lib bcrypt.lib *> (Join-Path $taskOut 'shader-replay-fixed-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOut 'shader-replay-fixed-build.log');throw 'Captured shader test build failed'}
$taskShaders=@(Get-ChildItem -LiteralPath (Join-Path $taskOut 'live-gpu-fix-20261010-logs\shaders') -Filter '*.ash')
& $taskExe @($taskShaders.FullName) *> (Join-Path $taskOut 'shader-replay-fixed-results.log')
Get-Content (Join-Path $taskOut 'shader-replay-fixed-results.log')
if($LASTEXITCODE -ne 0){throw 'Captured shader translation failed'}
foreach($taskShader in $taskShaders){
 & 'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe' --relax-block-layout --uniform-buffer-standard-layout --workgroup-scalar-block-layout --target-env vulkan1.3 ($taskShader.FullName+'.spv')
 if($LASTEXITCODE -ne 0){throw 'Captured shader SPIR-V validation failed'}
 Write-Output "PASS SPIR-V $($taskShader.Name)"
}
