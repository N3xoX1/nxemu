$ErrorActionPreference='Stop'
$strategyRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$strategyOut=Join-Path $strategyRepo 'build/performance-live-20261011/strategy-tests'
New-Item -ItemType Directory -Force -Path $strategyOut | Out-Null
$strategyVcvars='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
$strategyEnvironment=& cmd.exe /d /c "call `"$strategyVcvars`" >nul && set"
foreach($strategyLine in $strategyEnvironment){if($strategyLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'reference-sse2-copy.h') -Destination (Join-Path $strategyOut 'v7-reference.h')
& 'C:/VulkanSDK/1.4.357.0/Bin/glslangValidator.exe' -V --target-env vulkan1.3 (Join-Path $PSScriptRoot 'readback-delta.comp') -o (Join-Path $strategyOut 'delta.spv')
if($LASTEXITCODE -ne 0){throw 'Delta shader compile failed'}
& 'C:/VulkanSDK/1.4.357.0/Bin/spirv-val.exe' --target-env vulkan1.3 (Join-Path $strategyOut 'delta.spv')
if($LASTEXITCODE -ne 0){throw 'Delta SPIR-V invalid'}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 /I'C:/VulkanSDK/1.4.357.0/Include' "/I$strategyRepo/src" "/I$strategyRepo/build/pr396-corrections/tests" "/I$strategyOut" (Join-Path $PSScriptRoot 'readback-strategies.cpp') "/Fo$strategyOut/strategies.obj" "/Fe$strategyOut/strategies.exe" /link 'C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib' *> (Join-Path $strategyOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $strategyOut 'build.log');throw 'Strategies compile failed'}
$strategyLayers=$env:VK_INSTANCE_LAYERS;$strategySettings=$env:VK_LAYER_SETTINGS_PATH;$strategyLayerPath=$env:VK_LAYER_PATH
try{
 $env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation';$env:VK_LAYER_PATH='C:/VulkanSDK/1.4.357.0/Bin';$env:VK_LAYER_SETTINGS_PATH=$strategyOut
 [IO.File]::WriteAllText((Join-Path $strategyOut 'vk_layer_settings.txt'),"khronos_validation.validate_sync = true`nkhronos_validation.syncval_shader_accesses_heuristic = true`n")
 & (Join-Path $strategyOut 'strategies.exe') (Join-Path $strategyRepo 'build/pr396-corrections/tests/read.spv') (Join-Path $strategyOut 'delta.spv') check *> (Join-Path $strategyOut 'validation.txt')
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $strategyOut 'validation.txt') -Tail 10;throw 'Strategy oracle failed'}
 if(Select-String -LiteralPath (Join-Path $strategyOut 'validation.txt') -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC'){Get-Content (Join-Path $strategyOut 'validation.txt') -Tail 14;throw 'Strategy Vulkan synchronization invalid'}
 Remove-Item Env:VK_INSTANCE_LAYERS;Remove-Item Env:VK_LAYER_SETTINGS_PATH
 & (Join-Path $strategyOut 'strategies.exe') (Join-Path $strategyRepo 'build/pr396-corrections/tests/read.spv') (Join-Path $strategyOut 'delta.spv') bench > (Join-Path $strategyOut 'results.txt')
 if($LASTEXITCODE -ne 0){throw 'Strategy benchmark failed'}
 Get-Content (Join-Path $strategyOut 'results.txt')
}finally{$env:VK_INSTANCE_LAYERS=$strategyLayers;$env:VK_LAYER_SETTINGS_PATH=$strategySettings;$env:VK_LAYER_PATH=$strategyLayerPath}
