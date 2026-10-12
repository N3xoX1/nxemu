$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOutput=Join-Path $taskRoot 'build\bindless-input-tests'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskIncludes=@("/I$taskRoot\src","/I$taskRoot\src\nxemu-video","/I$taskRoot\external\boost","/I$taskRoot\external\fmt\include","/I$taskRoot\external\sirit\include","/I$taskRoot\external\sirit\externals\SPIRV-Headers\include")
$taskLibs=@('yuzu_shader_recompiler.lib','yuzu_common.lib','common.lib','fmt.lib','sirit.lib') | ForEach-Object {Join-Path $taskRoot "bin\x64\Release\lib\$_"}
$taskExe=Join-Path $taskOutput 'emit.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 @taskIncludes (Join-Path $PSScriptRoot 'emit-test.cpp') "/Fo$taskOutput\emit.obj" "/Fe$taskExe" /link @taskLibs user32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib iphlpapi.lib winmm.lib bcrypt.lib *> (Join-Path $taskOutput 'emit-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOutput 'emit-build.log');throw 'Shader test compile failed'}
& $taskExe (Join-Path $taskOutput 'emit')
if($LASTEXITCODE -ne 0){throw 'Shader emission failed'}
foreach($taskShader in (Get-ChildItem (Join-Path $taskOutput 'emit') -Filter '*.spv')){
 & 'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe' --target-env vulkan1.3 $taskShader.FullName
 if($LASTEXITCODE -ne 0){throw 'SPIR-V validation failed'}
 Write-Output "PASS SPIR-V $($taskShader.Name)"
}
