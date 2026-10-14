param([string]$Python='python',[string]$VulkanSdk=$env:VULKAN_SDK)
$ErrorActionPreference='Stop'
if(!$VulkanSdk){throw 'Set VULKAN_SDK to a Vulkan SDK installation'}
$repoRoot=(Resolve-Path "$PSScriptRoot/../../..").Path
$out=Join-Path $repoRoot 'build/performance-review-v11'
New-Item -ItemType Directory -Path $out -Force|Out-Null
Copy-Item "$PSScriptRoot/*" -Destination $out
& $Python "$PSScriptRoot/extract.py"
if($LASTEXITCODE){throw 'Extraction failed'}
Copy-Item "$PSScriptRoot/*-selected.inc","$PSScriptRoot/candidate-draw.inc" -Destination $out
# Run in a Visual Studio developer PowerShell; cl.exe must be available.
foreach($case in @('draw','dirty','pool')){
 $flags=@('/nologo','/std:c++20','/EHsc','/O2','/MT','/DNDEBUG','/DMATRIX=9','/DDIRTY_INC=dirty-selected.inc','/DPOOL_INC=pool-selected.inc',"/I$VulkanSdk/Include","/I$repoRoot/external/boost")
 & cl.exe @flags "$out/$case.cpp" "/Fo$out/$case.obj" "/Fe$out/$case.exe" /link "$VulkanSdk/Lib/vulkan-1.lib"
 if($LASTEXITCODE){throw "$case build failed"}
 & "$out/$case.exe"
 if($LASTEXITCODE){throw "$case oracle failed"}
}
