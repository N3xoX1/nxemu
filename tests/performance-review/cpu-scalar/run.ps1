param(
    [Parameter(Mandatory)][string]$ReferenceLibrary,
    [Parameter(Mandatory)][string]$CandidateLibrary,
    [Parameter(Mandatory)][string]$DependencyDirectory
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path "$PSScriptRoot/../../..").Path
$out = Join-Path $repoRoot 'build/performance-review-cpu-scalar'
New-Item -ItemType Directory -Path $out -Force | Out-Null
# Run in a Visual Studio developer PowerShell. Libraries must be from two
# otherwise identical builds, using the same runtime/compiler/dependencies.
foreach ($variant in @('reference', 'candidate')) {
    $library = if ($variant -eq 'reference') { $ReferenceLibrary } else { $CandidateLibrary }
    & cl.exe /nologo /std:c++20 /EHsc /O2 /MT "/I$repoRoot/src/nxemu-cpu" "/I$repoRoot/external/mcl/include" `
        "$PSScriptRoot/scalar-roundtrip.cpp" "/Fo$out/$variant.obj" "/Fe$out/$variant.exe" `
        /link $library "$DependencyDirectory/fmt.lib" "$DependencyDirectory/mcl.lib" `
        "$DependencyDirectory/zydis.lib" "$DependencyDirectory/Zycore.lib"
    if ($LASTEXITCODE) { throw "$variant fixture build failed" }
    foreach ($accuracy in @('safe', 'auto')) {
        & "$out/$variant.exe" "$out/$variant-$accuracy.bin" $accuracy
        if ($LASTEXITCODE) { throw "$variant/$accuracy did not execute correctly" }
    }
}
foreach ($accuracy in @('safe', 'auto')) {
    $reference = Get-FileHash -LiteralPath "$out/reference-$accuracy.bin" -Algorithm SHA256
    $candidate = Get-FileHash -LiteralPath "$out/candidate-$accuracy.bin" -Algorithm SHA256
    if ($reference.Hash -ne $candidate.Hash) { throw "$accuracy complete state mismatch" }
    "PASS $accuracy reference/candidate state SHA256 $($candidate.Hash)"
}
