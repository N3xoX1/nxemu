$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskOut=Join-Path $taskRepo 'build/immortals-menu-diagnostic-20261009/compute-indirect-tests'
$taskPython='C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $taskPython (Join-Path $PSScriptRoot 'extract-compute-indirect.py')
if($LASTEXITCODE -ne 0){throw 'Compute indirect extraction failed'}
$taskSdk='C:\VulkanSDK\1.4.357.0'
& "$taskSdk/Bin/glslangValidator.exe" -V --target-env spirv1.3 (Join-Path $taskRepo 'src/yuzu_video_core/host_shaders/vulkan_compute_indirect.comp') -o "$taskOut/convert.spv"
if($LASTEXITCODE -ne 0){throw 'Converter shader compilation failed'}
[IO.File]::WriteAllText("$taskOut/count.comp", @'
#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Count { uint invocations; };
void main(){atomicAdd(invocations,1u);}
'@)
& "$taskSdk/Bin/glslangValidator.exe" -V --target-env spirv1.3 "$taskOut/count.comp" -o "$taskOut/count.spv"
if($LASTEXITCODE -ne 0){throw 'Counter shader compilation failed'}
foreach($taskShader in @('convert','count')){
 & "$taskSdk/Bin/spirv-val.exe" --target-env vulkan1.1 "$taskOut/$taskShader.spv"
 if($LASTEXITCODE -ne 0){throw 'SPIR-V validation failed'}
}
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/I$taskSdk/Include" "/I$taskOut" (Join-Path $PSScriptRoot 'compute-indirect-vulkan.cpp') "/Fo$taskOut/test.obj" "/Fe$taskOut/test.exe" /link "$taskSdk/Lib/vulkan-1.lib" *> "$taskOut/build.log"
if($LASTEXITCODE -ne 0){Get-Content "$taskOut/build.log";throw 'Compute indirect GPU test compile failed'}
$env:VK_LAYER_PATH="$taskSdk/Bin"
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC='1'
$env:VK_LAYER_SETTINGS_PATH=$taskOut
[IO.File]::WriteAllText("$taskOut/vk_layer_settings.txt", "khronos_validation.validate_sync = true`nkhronos_validation.syncval_shader_accesses_heuristic = true`n")
& "$taskOut/test.exe" "$taskOut/convert.spv" "$taskOut/count.spv" *> "$taskOut/results.log"
$taskCode=$LASTEXITCODE
Get-Content "$taskOut/results.log"
if($taskCode -ne 0 -or (Select-String -Path "$taskOut/results.log" -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC')){throw 'Compute indirect GPU validation failed'}
Remove-Item Env:VK_INSTANCE_LAYERS
Remove-Item Env:VK_LAYER_VALIDATE_SYNC
Remove-Item Env:VK_LAYER_SETTINGS_PATH
& "$taskOut/test.exe" "$taskOut/convert.spv" "$taskOut/count.spv" bench *> "$taskOut/benchmark.log"
if($LASTEXITCODE -ne 0){throw 'Compute indirect GPU benchmark failed'}
Get-Content "$taskOut/benchmark.log"
