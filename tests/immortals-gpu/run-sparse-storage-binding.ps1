$ErrorActionPreference='Stop'
$taskRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskOut=Join-Path $taskRepo 'build/immortals-menu-diagnostic-20261009/sparse-storage-tests'
$taskPython='C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $taskPython (Join-Path $PSScriptRoot 'extract-sparse-storage-binding.py')
if($LASTEXITCODE -ne 0){throw 'Storage resolver extraction failed'}
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnvironment=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnvironment){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
foreach($taskVariant in @('baseline','fixed')) {
 $taskMode=if($taskVariant -eq 'fixed'){1}else{0}
 & cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/DFIXED_MODE=$taskMode" "/I$taskOut" (Join-Path $PSScriptRoot 'sparse-storage-binding.cpp') "/Fo$taskOut/$taskVariant.obj" "/Fe$taskOut/$taskVariant.exe" *> (Join-Path $taskOut "$taskVariant-build.log")
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskOut "$taskVariant-build.log");throw 'Storage resolver test compile failed'}
 & (Join-Path $taskOut "$taskVariant.exe") > (Join-Path $taskOut "$taskVariant-results.txt")
 $taskExit=$LASTEXITCODE
 Get-Content (Join-Path $taskOut "$taskVariant-results.txt")
 if($taskVariant -eq 'fixed' -and $taskExit -ne 0){throw 'Storage resolver regression tests failed'}
 if($taskVariant -eq 'baseline' -and $taskExit -eq 0){throw 'Baseline did not reproduce leading-hole failure'}
}
