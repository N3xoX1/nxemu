$ErrorActionPreference = 'Stop'
$reviewRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$reviewOut = Join-Path $reviewRoot 'build/performance-review-20261011'
$reviewExtracted = Join-Path $reviewRoot 'build/pr396-corrections/tests'
New-Item -ItemType Directory -Force -Path $reviewOut | Out-Null
$reviewVcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$reviewEnvironment = & cmd.exe /d /c "call `"$reviewVcvars`" >nul && set"
foreach ($reviewLine in $reviewEnvironment) {
    if ($reviewLine -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
& 'C:\VulkanSDK\1.4.357.0\Bin\glslangValidator.exe' -V --target-env vulkan1.3 (Join-Path $PSScriptRoot 'write-mask.comp') -o (Join-Path $reviewOut 'write-mask.spv')
if ($LASTEXITCODE -ne 0) { throw 'Mask shader failed to compile' }
& 'C:\VulkanSDK\1.4.357.0\Bin\spirv-val.exe' --target-env vulkan1.3 (Join-Path $reviewOut 'write-mask.spv')
if ($LASTEXITCODE -ne 0) { throw 'Invalid mask shader' }
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:\VulkanSDK\1.4.357.0\Include' "/I$reviewExtracted" (Join-Path $PSScriptRoot 'readback.cpp') "/Fo$reviewOut/readback.obj" "/Fe$reviewOut/readback.exe" /link 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib' *> (Join-Path $reviewOut 'readback-build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $reviewOut 'readback-build.log'); throw 'Readback benchmark build failed' }
$reviewOldLayers = $env:VK_INSTANCE_LAYERS
$reviewOldLayerPath = $env:VK_LAYER_PATH
$reviewOldSettings = $env:VK_LAYER_SETTINGS_PATH
try {
    $env:VK_LAYER_PATH = 'C:\VulkanSDK\1.4.357.0\Bin'
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
    $env:VK_LAYER_SETTINGS_PATH = $reviewOut
    [IO.File]::WriteAllText((Join-Path $reviewOut 'vk_layer_settings.txt'), "khronos_validation.validate_sync = true`nkhronos_validation.syncval_shader_accesses_heuristic = true`n", [Text.UTF8Encoding]::new($false))
    & (Join-Path $reviewOut 'readback.exe') (Join-Path $reviewOut 'write-mask.spv') check *> (Join-Path $reviewOut 'readback-validation.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $reviewOut 'readback-validation.log'); throw 'Mask verification failed' }
    if (Select-String -LiteralPath (Join-Path $reviewOut 'readback-validation.log') -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC') { Get-Content (Join-Path $reviewOut 'readback-validation.log'); throw 'Mask synchronization invalid' }
    Remove-Item Env:VK_INSTANCE_LAYERS
    Remove-Item Env:VK_LAYER_SETTINGS_PATH
    & (Join-Path $reviewOut 'readback.exe') (Join-Path $reviewOut 'write-mask.spv') bench *> (Join-Path $reviewOut 'readback-benchmark.log')
    if ($LASTEXITCODE -ne 0) { throw 'Readback benchmark failed' }
    Get-Content (Join-Path $reviewOut 'readback-benchmark.log')
} finally {
    $env:VK_INSTANCE_LAYERS = $reviewOldLayers
    $env:VK_LAYER_PATH = $reviewOldLayerPath
    $env:VK_LAYER_SETTINGS_PATH = $reviewOldSettings
}
