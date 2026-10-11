$ErrorActionPreference = 'Stop'
$reviewRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$reviewOut = Join-Path $reviewRoot 'build/performance-review-20261011'
$reviewExtracted = Join-Path $reviewRoot 'build/pr396-corrections/tests'
$reviewArrayShader = Join-Path $reviewRoot 'build/immortals-menu-diagnostic-20261009/capture-analysis/emit-fixed/4.spv'
& 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' (Join-Path $PSScriptRoot 'generate-descriptors.py')
if ($LASTEXITCODE -ne 0) { throw 'Descriptor fixture generation failed' }
$reviewVcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$reviewEnvironment = & cmd.exe /d /c "call `"$reviewVcvars`" >nul && set"
foreach ($reviewLine in $reviewEnvironment) {
    if ($reviewLine -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:\VulkanSDK\1.4.357.0\Include' "/I$reviewExtracted" (Join-Path $reviewOut 'descriptors.cpp') "/Fo$reviewOut/descriptors.obj" "/Fe$reviewOut/descriptors.exe" /link 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib' *> (Join-Path $reviewOut 'descriptors-build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $reviewOut 'descriptors-build.log'); throw 'Descriptor benchmark build failed' }
$reviewOldLayers = $env:VK_INSTANCE_LAYERS
$reviewOldPath = $env:VK_LAYER_PATH
$reviewOldSettings = $env:VK_LAYER_SETTINGS_PATH
try {
    $env:VK_LAYER_PATH = 'C:\VulkanSDK\1.4.357.0\Bin'
    $env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
    $env:VK_LAYER_SETTINGS_PATH = $reviewOut
    & (Join-Path $reviewOut 'descriptors.exe') (Join-Path $reviewExtracted 'read.spv') $reviewArrayShader 8193 *> (Join-Path $reviewOut 'descriptors-validation.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $reviewOut 'descriptors-validation.log'); throw 'Large descriptor layout failed' }
    if (Select-String -LiteralPath (Join-Path $reviewOut 'descriptors-validation.log') -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC') { Get-Content (Join-Path $reviewOut 'descriptors-validation.log'); throw 'Large descriptor layout invalid' }
    Remove-Item Env:VK_INSTANCE_LAYERS
    Remove-Item Env:VK_LAYER_SETTINGS_PATH
    foreach ($reviewCount in @(5, 1025, 8193)) {
        & (Join-Path $reviewOut 'descriptors.exe') (Join-Path $reviewExtracted 'read.spv') $reviewArrayShader $reviewCount *> (Join-Path $reviewOut "descriptors-$reviewCount.log")
        if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $reviewOut "descriptors-$reviewCount.log"); throw 'Descriptor update benchmark failed' }
        Get-Content (Join-Path $reviewOut "descriptors-$reviewCount.log") | Select-String 'BENCH|PASS|GPU '
    }
} finally {
    $env:VK_INSTANCE_LAYERS = $reviewOldLayers
    $env:VK_LAYER_PATH = $reviewOldPath
    $env:VK_LAYER_SETTINGS_PATH = $reviewOldSettings
}
