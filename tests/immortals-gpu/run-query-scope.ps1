$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOut=Join-Path $taskRoot 'build\immortals-menu-diagnostic-20261009\capture-analysis'
$taskPrior=Join-Path $taskRoot 'build\pr396-corrections\tests'
$taskPython='C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $taskPython (Join-Path $PSScriptRoot 'extract-query-scope.py')
if($LASTEXITCODE -ne 0){throw 'Query scope integration failed'}
@'
#version 450
void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexIndex],0,1);}
'@ | Set-Content (Join-Path $taskOut 'query-scope.vert')
@'
#version 450
layout(location=0) out vec4 color;
void main(){color=vec4(1,0,0,1);}
'@ | Set-Content (Join-Path $taskOut 'query-scope.frag')
foreach($taskStage in @('vert','frag')){
 & 'C:\VulkanSDK\1.4.357.0\Bin\glslangValidator.exe' -V (Join-Path $taskOut "query-scope.$taskStage") -o (Join-Path $taskOut "query-scope.$taskStage.spv") | Out-Null
 if($LASTEXITCODE -ne 0){throw 'Graphics shader fixture failed'}
}
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /I'C:\VulkanSDK\1.4.357.0\Include' "/I$taskPrior" "/I$taskOut" (Join-Path $PSScriptRoot 'query-scope.cpp') "/Fo$taskOut\query-scope.obj" "/Fe$taskOut\query-scope.exe" /link 'C:\VulkanSDK\1.4.357.0\Lib\vulkan-1.lib' *> (Join-Path $taskOut 'query-scope-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOut 'query-scope-build.log');throw 'Query fixture failed to compile'}
$env:VK_LAYER_PATH='C:\VulkanSDK\1.4.357.0\Bin'
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_SETTINGS_PATH=$taskOut
foreach($taskVariant in @('baseline','fixed')){
 $taskLog=Join-Path $taskOut "query-scope-$taskVariant.log"
 & (Join-Path $taskOut 'query-scope.exe') (Join-Path $taskPrior 'read.spv') (Join-Path $taskOut 'query-scope.vert.spv') (Join-Path $taskOut 'query-scope.frag.spv') $taskVariant *> $taskLog
 if($LASTEXITCODE -ne 0){Get-Content $taskLog;throw 'Query values failed'}
 $taskErrors=@(Select-String -Path $taskLog -Pattern 'Validation Error|SYNC-HAZARD|ERROR / SPEC').Count
 Get-Content $taskLog | Select-Object -Last 2
 Write-Output "Query scope $taskVariant errors=$taskErrors"
 if($taskVariant -eq 'baseline' -and $taskErrors -eq 0){throw 'Negative control missed invalid query'}
 if($taskVariant -eq 'fixed' -and $taskErrors -ne 0){Get-Content $taskLog;throw 'Query scope Vulkan validation failed'}
}
