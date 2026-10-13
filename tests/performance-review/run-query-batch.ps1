$ErrorActionPreference='Stop'
$batchRoot=(Resolve-Path "$PSScriptRoot/../..").Path
$batchOut="$batchRoot/build/performance-live-20261011/query-batch-tests"
$batchPrior="$batchRoot/build/pr396-corrections/tests"
$batchPython='C:/Users/fabri/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $batchPython "$PSScriptRoot/extract-query-batch.py"
if($LASTEXITCODE){throw 'Query batch extraction failed'}
$batchEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($batchLine in $batchEnvironment){if($batchLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:/VulkanSDK/1.4.357.0/Include' "/I$batchOut" "/I$batchPrior" "$PSScriptRoot/query-batch.cpp" "/Fo$batchOut/query-batch.obj" "/Fe$batchOut/query-batch.exe" /link 'C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib' *> "$batchOut/build.log"
if($LASTEXITCODE){Get-Content "$batchOut/build.log";throw 'Batch test compile failed'}
$env:VK_LAYER_PATH='C:/VulkanSDK/1.4.357.0/Bin'
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC='1'
$env:VK_LAYER_SETTINGS_PATH="$batchRoot/build/immortals-menu-diagnostic-20261009/capture-analysis"
try {
 & "$batchOut/query-batch.exe" "$batchPrior/read.spv" validate *> "$batchOut/validation.log"
 if($LASTEXITCODE){Get-Content "$batchOut/validation.log";throw 'Query batch validation failed'}
 if(Select-String -LiteralPath "$batchOut/validation.log" -Pattern 'SYNC-HAZARD|Validation Error|ERROR / SPEC'){Get-Content "$batchOut/validation.log";throw 'Vulkan validation error'}
 Get-Content "$batchOut/validation.log"
} finally {Remove-Item Env:VK_INSTANCE_LAYERS;Remove-Item Env:VK_LAYER_VALIDATE_SYNC;Remove-Item Env:VK_LAYER_SETTINGS_PATH}
& "$batchOut/query-batch.exe" "$batchPrior/read.spv" bench *> "$batchOut/benchmark.txt"
if($LASTEXITCODE){throw 'Batch benchmark failed'}
Get-Content "$batchOut/benchmark.txt"
