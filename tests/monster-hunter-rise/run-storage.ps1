$ErrorActionPreference='Stop'
$mhrRoot=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$mhrOut=Join-Path $mhrRoot 'build\monster-hunter-rise-20261013\storage-tests'
& 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' (Join-Path $PSScriptRoot 'extract-storage.py')
if($LASTEXITCODE -ne 0){throw 'Extraction failed'}
$mhrVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$mhrEnv=& cmd.exe /d /c "call `"$mhrVcvars`" >nul && set"
foreach($mhrLine in $mhrEnv){if($mhrLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
foreach($mhrVariant in @('baseline','fixed')){
    $mhrMode=if($mhrVariant -eq 'fixed'){1}else{0}
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/DFIXED_MODE=$mhrMode" "/I$mhrOut" (Join-Path $mhrOut 'storage-binding.cpp') "/Fo$mhrOut/$mhrVariant.obj" "/Fe$mhrOut/$mhrVariant.exe" *> (Join-Path $mhrOut "$mhrVariant-build.log")
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $mhrOut "$mhrVariant-build.log");throw 'Compilation failed'}
    & (Join-Path $mhrOut "$mhrVariant.exe") > (Join-Path $mhrOut "$mhrVariant-results.txt")
    $mhrExit=$LASTEXITCODE
    Get-Content (Join-Path $mhrOut "$mhrVariant-results.txt")
    if($mhrVariant -eq 'fixed' -and $mhrExit -ne 0){throw 'Regression failed'}
    if($mhrVariant -eq 'baseline' -and $mhrExit -eq 0){throw 'Baseline did not reproduce defect'}
}
