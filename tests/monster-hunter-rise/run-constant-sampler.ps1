param([string]$LibraryRoot='')
$ErrorActionPreference='Stop'
$mhrRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$mhrOut=Join-Path $mhrRoot 'build\monster-hunter-rise-20261013\constant-sampler-tests'
New-Item -ItemType Directory -Force -Path $mhrOut | Out-Null
if(-not $LibraryRoot){$LibraryRoot=Join-Path $mhrRoot 'build\monster-hunter-rise-20261013\sampler-lib'}
$mhrVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$mhrEnv=& cmd.exe /d /c "call `"$mhrVcvars`" >nul && set"
foreach($mhrLine in $mhrEnv){if($mhrLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$mhrIncludes=@("/I$mhrRoot\src","/I$mhrRoot\src\nxemu-video","/I$mhrRoot\external\boost","/I$mhrRoot\external\fmt\include","/I$mhrRoot\external\sirit\include","/I$mhrRoot\external\sirit\externals\SPIRV-Headers\include")
$mhrLibs=@('yuzu_common.lib','common.lib','fmt.lib','sirit.lib') | ForEach-Object {Join-Path $mhrRoot "bin\x64\Release\lib\$_"}
$mhrLibs+=Join-Path $LibraryRoot 'yuzu_shader_recompiler.lib'
$mhrBaseline=git show '2c68b91222e0a01e43c143e2917c40920b510868:src/yuzu_shader_recompiler/ir_opt/texture_pass.cpp'
if($LASTEXITCODE -ne 0){throw 'Baseline extraction failed'}
[IO.File]::WriteAllLines((Join-Path $mhrOut 'baseline-texture.cpp'),$mhrBaseline,[Text.UTF8Encoding]::new($false))
foreach($mhrVariant in @('baseline','fixed')){
    $mhrSource=if($mhrVariant -eq 'baseline'){Join-Path $mhrOut 'baseline-texture.cpp'}else{Join-Path $mhrRoot 'src\yuzu_shader_recompiler\ir_opt\texture_pass.cpp'}
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 @mhrIncludes /c $mhrSource "/Fo$mhrOut\$mhrVariant-texture.obj" *> (Join-Path $mhrOut "$mhrVariant-build.log")
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $mhrOut "$mhrVariant-build.log");throw 'Texture pass compilation failed'}
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /utf-8 @mhrIncludes (Join-Path $PSScriptRoot 'constant-sampler.cpp') "/Fo$mhrOut\$mhrVariant-test.obj" "/Fe$mhrOut\$mhrVariant.exe" /link (Join-Path $mhrOut "$mhrVariant-texture.obj") @mhrLibs user32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib iphlpapi.lib winmm.lib bcrypt.lib >> (Join-Path $mhrOut "$mhrVariant-build.log")
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $mhrOut "$mhrVariant-build.log");throw 'Test compilation failed'}
    & (Join-Path $mhrOut "$mhrVariant.exe") > (Join-Path $mhrOut "$mhrVariant-results.txt")
    $mhrExit=$LASTEXITCODE
    Get-Content (Join-Path $mhrOut "$mhrVariant-results.txt")
    if($mhrVariant -eq 'fixed' -and $mhrExit -ne 0){throw 'Constant sampler tests failed'}
    if($mhrVariant -eq 'baseline' -and $mhrExit -eq 0){throw 'Baseline did not reproduce lost samplers'}
}
