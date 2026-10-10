$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOutput=Join-Path $taskRoot 'build\immortals-menu-diagnostic-20261009\capture-analysis'
$taskPrior=Join-Path $taskRoot 'build\pr396-corrections\tests'
$taskPython='C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $taskPython (Join-Path $PSScriptRoot 'extract-barriers.py')
if($LASTEXITCODE -ne 0){throw 'Production barrier extraction failed'}
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskExe=Join-Path $taskOutput 'vulkan-barriers.exe'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:\VulkanSDK\1.4.357.0\Include' "/I$taskOutput" "/I$taskPrior" (Join-Path $PSScriptRoot 'vulkan-barriers.cpp') "/Fo$taskOutput\vulkan-barriers.obj" "/Fe$taskExe" /link 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib' *> (Join-Path $taskOutput 'vulkan-barriers-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOutput 'vulkan-barriers-build.log');throw 'GPU fixture compile failed'}
$env:VK_LAYER_PATH='C:\VulkanSDK\1.4.357.0\Bin'
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC='1'
$env:VK_LAYER_SETTINGS_PATH=$taskOutput
[IO.File]::WriteAllText((Join-Path $taskOutput 'vk_layer_settings.txt'), "khronos_validation.validate_sync = true`nkhronos_validation.syncval_shader_accesses_heuristic = true`n", [Text.UTF8Encoding]::new($false))
foreach($taskKind in @('wfi','query','image','compute')){
 foreach($taskVariant in @('baseline','fixed')){
  $taskLog=Join-Path $taskOutput "vulkan-$taskKind-$taskVariant.log"
  & $taskExe (Join-Path $taskPrior 'read.spv') $taskKind $taskVariant *> $taskLog
  if($LASTEXITCODE -ne 0){Get-Content $taskLog;throw 'GPU result failed'}
  $taskHazards=@(Select-String -Path $taskLog -Pattern 'SYNC-HAZARD').Count
  $taskErrors=@(Select-String -Path $taskLog -Pattern 'Validation Error|ERROR / SPEC').Count
  Write-Output "GPU $taskKind $taskVariant hazards=$taskHazards errors=$taskErrors"
  if($taskVariant -eq 'baseline' -and $taskHazards -eq 0){throw 'Negative control failed to expose hazard'}
  if($taskVariant -eq 'fixed' -and ($taskHazards -ne 0 -or $taskErrors -ne 0)){Get-Content $taskLog;throw 'Fixed GPU chain failed Vulkan validation'}
 }
}
Remove-Item Env:VK_INSTANCE_LAYERS
Remove-Item Env:VK_LAYER_VALIDATE_SYNC
Remove-Item Env:VK_LAYER_SETTINGS_PATH
foreach($taskKind in @('bench','compute-bench')){foreach($taskVariant in @('baseline','fixed')){
 & $taskExe (Join-Path $taskPrior 'read.spv') $taskKind $taskVariant *> (Join-Path $taskOutput "vulkan-$taskKind-$taskVariant.log")
 if($LASTEXITCODE -ne 0){throw 'GPU benchmark failed'}
 Get-Content (Join-Path $taskOutput "vulkan-$taskKind-$taskVariant.log")
}}
