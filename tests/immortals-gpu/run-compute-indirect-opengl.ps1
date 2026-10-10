$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskOut=Join-Path $taskRepo 'build/immortals-menu-diagnostic-20261009/compute-indirect-tests'
New-Item -ItemType Directory -Force -Path $taskOut | Out-Null
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/I$taskRepo/src/3rd_party/glad/include" (Join-Path $PSScriptRoot 'compute-indirect-opengl.cpp') "/Fo$taskOut/opengl.obj" "/Fe$taskOut/opengl.exe" /link "$taskRepo/bin/x64/Release/lib/glad.lib" opengl32.lib user32.lib gdi32.lib *> "$taskOut/opengl-build.log"
if($LASTEXITCODE -ne 0){Get-Content "$taskOut/opengl-build.log";throw 'OpenGL indirect test compile failed'}
& "$taskOut/opengl.exe" (Join-Path $taskRepo 'src/yuzu_video_core/host_shaders/opengl_compute_indirect.comp') *> "$taskOut/opengl-results.log"
$taskCode=$LASTEXITCODE
Get-Content "$taskOut/opengl-results.log"
if($taskCode -ne 0){throw 'OpenGL indirect GPU test failed'}
