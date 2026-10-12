$ErrorActionPreference='Stop'
$rangeRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$rangeOut=Join-Path $rangeRepo 'build/performance-live-20261011/range-tests'
New-Item -ItemType Directory -Force -Path $rangeOut | Out-Null
foreach ($rangeItem in @(@('usage_tracker.h','BaselineUsage','baseline-usage.h'),@('word_manager.h','BaselineWord','baseline-word.h'))) {
    $rangeSource=& git -C $rangeRepo show "61f2d4b6:src/yuzu_video_core/buffer_cache/$($rangeItem[0])"
    if ($LASTEXITCODE -ne 0) {throw 'Baseline extraction failed'}
    $rangeText=($rangeSource -join "`n").Replace('namespace VideoCommon {',"namespace $($rangeItem[1]) {")
    [IO.File]::WriteAllText((Join-Path $rangeOut $rangeItem[2]),$rangeText,[Text.UTF8Encoding]::new($false))
}
$rangeV9Source=& git -C $rangeRepo show '308818ba:src/yuzu_video_core/buffer_cache/word_manager.h'
if($LASTEXITCODE -ne 0){throw 'Validated v9 word tracker extraction failed'}
$rangeV9Text=($rangeV9Source -join "`n").Replace('namespace VideoCommon {','namespace PreviousWord {')
[IO.File]::WriteAllText((Join-Path $rangeOut 'v9-word.h'),$rangeV9Text,[Text.UTF8Encoding]::new($false))
& 'C:\Users\fabri\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe' "$PSScriptRoot/extract-ranges.py"
if($LASTEXITCODE -ne 0){throw 'Continuity function extraction failed'}
$rangeVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$rangeEnvironment=& cmd.exe /d /c "call `"$rangeVcvars`" >nul && set"
foreach ($rangeLine in $rangeEnvironment) {if($rangeLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /DNDEBUG "/I$rangeOut" "/I$rangeRepo/src" "/I$rangeRepo/src/yuzu_video_core" "/I$rangeRepo/external/boost" "/I$rangeRepo/external/fmt/include" "$PSScriptRoot/ranges.cpp" "/Fo$rangeOut/ranges.obj" "/Fe$rangeOut/ranges.exe" *> "$rangeOut/build.log"
if ($LASTEXITCODE -ne 0) {Get-Content "$rangeOut/build.log";throw 'Range fixture build failed'}
& "$rangeOut/ranges.exe" > "$rangeOut/results.txt"
$rangeExit=$LASTEXITCODE
Get-Content "$rangeOut/results.txt"
if ($rangeExit -ne 0) {throw 'Range oracle failed'}
