$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$out = Join-Path $repo 'build\pr396-corrections\tests'
$sdk = 'C:\VulkanSDK\1.4.357.0'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$environment = & cmd.exe /d /c "call `"$vcvars`" >nul && set"
foreach ($line in $environment) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') } }
& "$sdk\Bin\glslc.exe" (Join-Path $PSScriptRoot 'read.comp') -o (Join-Path $out 'read.spv')
if ($LASTEXITCODE -ne 0) { throw 'Shader compile failed' }
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/I$sdk\Include" "/I$out" (Join-Path $PSScriptRoot 'vulkan-synthetic.cpp') "/Fo$out\vulkan-synthetic.obj" "/Fe$out\vulkan-synthetic.exe" /link "$sdk\Lib\vulkan-1.lib" *> (Join-Path $out 'vulkan-test-build.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $out 'vulkan-test-build.log') -Tail 25; throw 'Vulkan fixture compile failed' }
& (Join-Path $out 'vulkan-synthetic.exe') (Join-Path $out 'read.spv') > (Join-Path $out 'vulkan-results.txt')
$code = $LASTEXITCODE
Get-Content (Join-Path $out 'vulkan-results.txt')
Write-Output "Vulkan test exit code: $code"
if ($code -ne 0) { throw 'Vulkan regression tests failed' }
