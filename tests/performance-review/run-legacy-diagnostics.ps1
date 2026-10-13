$ErrorActionPreference='Stop'
$legacyRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$legacyOut=Join-Path $legacyRepo 'build/performance-review-20261013/legacy-diagnostics'
New-Item -ItemType Directory -Force -Path "$legacyOut/yuzu_common/logging" | Out-Null
# Exercise the real diagnostic header; replace only formatting/log emission.
# FPS and elapsed-time measurements are not made by this correctness fixture.
@'
#pragma once
#include <string>
namespace fmt {template<class... Args> std::string format(const char*,Args&&...){return {};}}
#define LOG_INFO(...) (void)0
'@ | Set-Content -Encoding UTF8 "$legacyOut/yuzu_common/logging/log.h"
$legacyEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($legacyLine in $legacyEnvironment){if($legacyLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
foreach($legacyMode in @('default','enabled')) {
    $legacyFlag=if($legacyMode -eq 'enabled'){'/DNXEMU_ENABLE_LEGACY_PERFORMANCE_DIAGNOSTICS=1'}else{'/DNDEBUG'}
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 $legacyFlag "/I$legacyOut" "/I$legacyRepo/src" "$PSScriptRoot/legacy-diagnostics.cpp" "/Fo$legacyOut/$legacyMode.obj" "/Fe$legacyOut/$legacyMode.exe" *> "$legacyOut/$legacyMode-build.log"
    if($LASTEXITCODE -ne 0){Get-Content "$legacyOut/$legacyMode-build.log";throw 'Diagnostic fixture compilation failed'}
    & "$legacyOut/$legacyMode.exe"
    if($LASTEXITCODE -ne 0){throw "Diagnostic behavior failed: $legacyMode"}
}
