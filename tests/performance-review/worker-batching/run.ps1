param([string]$Python='python')
$ErrorActionPreference='Stop'
$repoRoot=(Resolve-Path "$PSScriptRoot/../../..").Path
$out=Join-Path $repoRoot 'build/performance-review-worker-batching'
& $Python "$PSScriptRoot/extract.py"
if($LASTEXITCODE){throw 'Worker cadence extraction failed'}
# Run in a Visual Studio developer PowerShell with cl.exe on PATH.
foreach($platform in @('Windows','Android')){
    $flags=@('/nologo','/std:c++20','/EHsc','/O2','/MT',"/I$out")
    if($platform -eq 'Android'){$flags+='/DANDROID'}
    & cl.exe @flags "$PSScriptRoot/cadence.cpp" "/Fo$out/$platform.obj" "/Fe$out/$platform.exe"
    if($LASTEXITCODE){throw "$platform fixture compilation failed"}
    & "$out/$platform.exe"
    if($LASTEXITCODE){throw "$platform submission cadence changed"}
}
