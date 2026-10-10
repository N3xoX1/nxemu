param([ValidateSet('original','fixed')][string]$Variant='fixed',[string]$LibraryRoot='')
$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if(-not $LibraryRoot){$LibraryRoot=$taskRoot}
$taskOutput=Join-Path $taskRoot 'build\immortals-menu-diagnostic-20261009\capture-analysis'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskHeaderRoot=if($Variant -eq 'original'){$LibraryRoot}else{$taskRoot}
$taskIncludes=@("/I$taskHeaderRoot\src","/I$taskHeaderRoot\src\nxemu-video","/I$taskHeaderRoot\external\boost","/I$taskHeaderRoot\external\fmt\include","/I$taskHeaderRoot\external\sirit\include","/I$taskHeaderRoot\external\sirit\externals\SPIRV-Headers\include")
[string[]]$taskDefines=if($Variant -eq 'original'){@('/DORIGINAL_SHADER_BASELINE=1')}else{@()}
$taskLibs=@('yuzu_shader_recompiler.lib','yuzu_common.lib','common.lib','fmt.lib','sirit.lib') | ForEach-Object {Join-Path $LibraryRoot "bin\x64\Release\lib\$_"}
$taskExe=Join-Path $taskOutput "emit-$Variant.exe"
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 @taskDefines @taskIncludes (Join-Path $PSScriptRoot 'emit-test.cpp') "/Fo$taskOutput\emit-$Variant.obj" "/Fe$taskExe" /link @taskLibs user32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib iphlpapi.lib winmm.lib bcrypt.lib *> (Join-Path $taskOutput "emit-$Variant-build.log")
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOutput "emit-$Variant-build.log");throw 'Production shader test compile failed'}
& $taskExe (Join-Path $taskOutput "emit-$Variant") *> (Join-Path $taskOutput "emit-$Variant-results.txt")
if($LASTEXITCODE -ne 0){throw 'Production shader emission failed'}
foreach($taskShader in (Get-ChildItem (Join-Path $taskOutput "emit-$Variant") -Filter '*.spv')){
 if($Variant -eq 'original' -and [int]$taskShader.BaseName -ge 4){continue}
 & 'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe' --relax-block-layout --uniform-buffer-standard-layout --workgroup-scalar-block-layout --target-env vulkan1.3 $taskShader.FullName 2>&1 | Set-Content (Join-Path $taskOutput ("emit-$Variant-"+$taskShader.BaseName+'.txt'))
 $taskActual=$LASTEXITCODE
 $taskExpected=if($Variant -eq 'original' -and $taskShader.BaseName -in @('0','2')){1}else{0}
 Write-Output "Shader $Variant case $($taskShader.BaseName): validation=$taskActual expected=$taskExpected"
 if($taskActual -ne $taskExpected){throw 'Unexpected production shader validation result'}
}
