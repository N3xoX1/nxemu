param(
    [string]$BuildDirectory = 'build/windows-x64',
    [string]$BaseRef = 'master',
    [string]$BundlePatch = '',
    [int]$Repetitions = 9
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
$snapshotRoot = Join-Path $buildRoot 'architecture-baselines'
$utf8 = [Text.UTF8Encoding]::new($false)
$variants = @('master')
if ($BundlePatch) { $variants += 'bundle' }
foreach ($variant in $variants) {
    $variantRoot = Join-Path $snapshotRoot $variant
    $sourceRoot = Join-Path $variantRoot 'src/nxemu-core/settings'
    New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
    foreach ($name in @('settings.cpp', 'settings.h', 'identifiers.h')) {
        $contents = & git -C $repoRoot show "${BaseRef}:src/nxemu-core/settings/$name"
        if ($LASTEXITCODE) { throw "git show failed: $name" }
        [IO.File]::WriteAllText((Join-Path $sourceRoot $name), ($contents -join "`n") + "`n", $utf8)
    }
    if ($variant -eq 'bundle') {
        & git -C $variantRoot init --quiet
        & git -C $variantRoot apply --ignore-space-change --include=src/nxemu-core/settings/settings.cpp --include=src/nxemu-core/settings/settings.h (Resolve-Path $BundlePatch).Path
        if ($LASTEXITCODE) { throw 'Cannot apply bundled SettingsStore changes to baseline' }
    }
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'settings.cpp') -Destination $variantRoot
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'settings.h') -Destination $variantRoot
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'identifiers.h') -Destination $variantRoot
    $includeRoot = Join-Path $variantRoot 'include/nxemu-core/settings'
    New-Item -ItemType Directory -Force -Path $includeRoot | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot 'settings.h') -Destination $includeRoot
}
& cmake -S $repoRoot -B $buildRoot -DNXEMU_BUILD_TESTS=ON "-DNXEMU_SETTINGS_BASELINES=$snapshotRoot"
if ($LASTEXITCODE) { throw 'Configure failed' }
$targets = @('benchmark_settings') + @($variants | ForEach-Object { "benchmark_settings_$_" })
& cmake --build $buildRoot --config Release --parallel 3 --target $targets
if ($LASTEXITCODE) { throw 'Benchmark build failed' }
$rows = @()
for ($repeat = 0; $repeat -lt $Repetitions; ++$repeat) {
    # Alternate execution order to reduce systematic thermal/order bias.
    $order = if ($repeat % 2) { @($targets | Sort-Object -Descending) } else { $targets }
    foreach ($target in $order) {
        $binary = Join-Path $buildRoot "tests/unit/Release/$target.exe"
        $measurements = & $binary 2000000 | ConvertFrom-Csv
        if ($LASTEXITCODE) { throw "Benchmark failed: $target" }
        foreach ($measurement in $measurements) {
            $rows += [pscustomobject]@{ variant=$target; repeat=$repeat; operation=$measurement.operation; ns_per_iteration=$measurement.ns_per_iteration; checksum=$measurement.checksum }
        }
    }
}
$resultPath = Join-Path $buildRoot 'architecture-settings-benchmark.csv'
$rows | Export-Csv -LiteralPath $resultPath -NoTypeInformation
Write-Output "Results: $resultPath"
