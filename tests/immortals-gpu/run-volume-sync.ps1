$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskOut=Join-Path $taskRepo 'build/immortals-menu-diagnostic-20261009/volume-sync-tests'
New-Item -ItemType Directory -Force -Path $taskOut | Out-Null
$taskAnalysis=Join-Path $taskRepo 'build/immortals-menu-diagnostic-20261009/capture-analysis'
$taskPrior=Join-Path $taskRepo 'build/pr396-corrections/tests'
$taskPython='C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $taskPython (Join-Path $PSScriptRoot 'extract-barriers.py')
if($LASTEXITCODE -ne 0){throw 'Production extraction failed'}
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& 'C:\VulkanSDK\1.4.357.0\Bin\glslangValidator.exe' -V --target-env vulkan1.3 (Join-Path $PSScriptRoot 'volume-sync.comp') -o (Join-Path $taskOut 'volume-sync.spv')
if($LASTEXITCODE -ne 0){throw 'Volume shader failed'}
& 'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe' --target-env vulkan1.3 (Join-Path $taskOut 'volume-sync.spv')
if($LASTEXITCODE -ne 0){throw 'Invalid test SPIR-V'}
$taskExe=Join-Path $taskOut 'volume-sync.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:\VulkanSDK\1.4.357.0\Include' "/I$taskAnalysis" "/I$taskPrior" (Join-Path $PSScriptRoot 'volume-sync.cpp') "/Fo$taskOut/volume-sync.obj" "/Fe$taskExe" /link 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib' *> (Join-Path $taskOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOut 'build.log');throw 'Volume fixture failed'}
$env:VK_LAYER_PATH='C:\VulkanSDK\1.4.357.0\Bin';$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation';$env:VK_LAYER_SETTINGS_PATH=$taskOut
[IO.File]::WriteAllText((Join-Path $taskOut 'vk_layer_settings.txt'),"khronos_validation.validate_sync = true`nkhronos_validation.syncval_shader_accesses_heuristic = true`nkhronos_validation.duplicate_message_limit = 2`n",[Text.UTF8Encoding]::new($false))
foreach($taskMode in @('baseline','synced')){
 $taskLog=Join-Path $taskOut "$taskMode.log"
 & $taskExe (Join-Path $taskPrior 'read.spv') (Join-Path $taskOut 'volume-sync.spv') $taskMode *> $taskLog
 if($LASTEXITCODE -ne 0){Get-Content $taskLog;throw 'GPU volume result failed'}
 $taskHazards=@(Select-String -LiteralPath $taskLog -Pattern 'SYNC-HAZARD').Count
 $taskErrors=@(Select-String -LiteralPath $taskLog -Pattern 'Validation Error|ERROR / SPEC').Count
 Write-Output "VOLUME $taskMode hazards=$taskHazards errors=$taskErrors"
 if($taskMode -eq 'baseline' -and !$taskHazards){throw 'Negative control failed to detect missing barrier'}
 if($taskMode -eq 'synced' -and $taskErrors){Get-Content $taskLog;throw 'Production barrier failed validation'}
 Get-Content $taskLog | Select-String 'VOLUME seed=' | Select-Object -Last 1
}
Remove-Item Env:VK_INSTANCE_LAYERS;Remove-Item Env:VK_LAYER_SETTINGS_PATH
& $taskExe (Join-Path $taskPrior 'read.spv') (Join-Path $taskOut 'volume-sync.spv') bench *> (Join-Path $taskOut 'benchmark.log')
if($LASTEXITCODE -ne 0){throw 'Benchmark failed'}
Get-Content (Join-Path $taskOut 'benchmark.log')
