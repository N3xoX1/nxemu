$ErrorActionPreference='Stop'
$deltaRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$deltaOut=Join-Path $deltaRepo 'build/performance-live-20261011/gpu-delta-tests'
New-Item -ItemType Directory -Force -Path $deltaOut | Out-Null
$deltaPython='C:/Users/fabri/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
@'
import pathlib,hashlib,json
r=pathlib.Path.cwd();p=r/'src/yuzu_video_core/renderer_vulkan/vk_compute_pass.cpp'
s=p.read_text();a=s.index('void ReadbackDeltaPass::Copy(');b=s.index('{',a)+1;depth=1
while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
body=s[a:b];out=r/'build/performance-live-20261011/gpu-delta-tests'
(out/'delta-pass.inc').write_text(body.replace('ReadbackDeltaPass::Copy','NativeDelta::Copy'))
(out/'extraction.json').write_text(json.dumps({'source':str(p),'sha256':hashlib.sha256(body.encode()).hexdigest()},indent=2))
'@ | & $deltaPython -
& 'C:/VulkanSDK/1.4.357.0/Bin/glslangValidator.exe' -V --target-env spirv1.3 (Join-Path $deltaRepo 'src/yuzu_video_core/host_shaders/vulkan_readback_delta.comp') -o (Join-Path $deltaOut 'delta.spv')
if($LASTEXITCODE -ne 0){throw 'Production delta shader compile failed'}
& 'C:/VulkanSDK/1.4.357.0/Bin/spirv-val.exe' --target-env spv1.3 (Join-Path $deltaOut 'delta.spv')
if($LASTEXITCODE -ne 0){throw 'Production delta SPIR-V invalid'}
$deltaEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($line in $deltaEnvironment){if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 /I'C:/VulkanSDK/1.4.357.0/Include' "/I$deltaRepo/src" "/I$deltaRepo/external/boost" "/I$deltaRepo/build/pr396-corrections/tests" "/I$deltaOut" (Join-Path $PSScriptRoot 'vulkan-delta-readback.cpp') "/Fo$deltaOut/delta.obj" "/Fe$deltaOut/delta.exe" /link 'C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib' *> (Join-Path $deltaOut 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $deltaOut 'build.log') -Tail 12;throw 'Production delta fixture compile failed'}
$deltaOldLayers=$env:VK_INSTANCE_LAYERS;$deltaOldPath=$env:VK_LAYER_PATH;$deltaOldSettings=$env:VK_LAYER_SETTINGS_PATH
try{
 $env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation';$env:VK_LAYER_PATH='C:/VulkanSDK/1.4.357.0/Bin';$env:VK_LAYER_SETTINGS_PATH=$deltaOut
 [IO.File]::WriteAllText((Join-Path $deltaOut 'vk_layer_settings.txt'),"khronos_validation.validate_sync = true`nkhronos_validation.syncval_shader_accesses_heuristic = true`n")
 & (Join-Path $deltaOut 'delta.exe') (Join-Path $deltaRepo 'build/pr396-corrections/tests/read.spv') (Join-Path $deltaOut 'delta.spv') *> (Join-Path $deltaOut 'validation.txt')
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $deltaOut 'validation.txt') -Tail 14;throw 'Production delta GPU oracle failed'}
 if(Select-String -LiteralPath (Join-Path $deltaOut 'validation.txt') -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC'){Get-Content (Join-Path $deltaOut 'validation.txt') -Tail 14;throw 'Production delta Vulkan sync invalid'}
 Get-Content (Join-Path $deltaOut 'validation.txt')
}finally{$env:VK_INSTANCE_LAYERS=$deltaOldLayers;$env:VK_LAYER_PATH=$deltaOldPath;$env:VK_LAYER_SETTINGS_PATH=$deltaOldSettings}
try {
 $env:VK_INSTANCE_LAYERS=$null;$env:VK_LAYER_SETTINGS_PATH=$null
 & (Join-Path $deltaOut 'delta.exe') (Join-Path $deltaRepo 'build/pr396-corrections/tests/read.spv') (Join-Path $deltaOut 'delta.spv') --benchmark *> (Join-Path $deltaOut 'benchmark.txt')
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $deltaOut 'benchmark.txt') -Tail 14;throw 'Production delta benchmark oracle failed'}
 Get-Content (Join-Path $deltaOut 'benchmark.txt')
}finally{$env:VK_INSTANCE_LAYERS=$deltaOldLayers;$env:VK_LAYER_SETTINGS_PATH=$deltaOldSettings}
