$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOut=Join-Path $taskRoot 'build\immortals-menu-diagnostic-20261009\capture-analysis'
$taskPrior=Join-Path $taskRoot 'build\pr396-corrections\tests'
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:\VulkanSDK\1.4.357.0\Include' "/I$taskPrior" (Join-Path $PSScriptRoot 'storage-array.cpp') "/Fo$taskOut\storage-array.obj" "/Fe$taskOut\storage-array.exe" /link 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib' *> (Join-Path $taskOut 'storage-array-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOut 'storage-array-build.log');throw 'Storage array test compile failed'}
$env:VK_LAYER_PATH='C:\VulkanSDK\1.4.357.0\Bin'
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_SETTINGS_PATH=$taskOut
& (Join-Path $taskOut 'storage-array.exe') (Join-Path $taskPrior 'read.spv') (Join-Path $taskOut 'emit-fixed\4.spv') *> (Join-Path $taskOut 'storage-array-results.log')
Get-Content (Join-Path $taskOut 'storage-array-results.log')
if($LASTEXITCODE -ne 0){throw 'Storage array test results failed'}
if(Select-String -Path (Join-Path $taskOut 'storage-array-results.log') -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC'){throw 'Storage array Vulkan validation failed'}
