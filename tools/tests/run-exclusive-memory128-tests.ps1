param([string] $OutputDirectory, [string] $MemorySourcePath)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot 'build\validation\exclusive-memory128-tests'
}
if (-not $MemorySourcePath) {
    $MemorySourcePath = Join-Path $repoRoot 'src\nxemu-os\core\memory.cpp'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

# Compile the actual module adapter without constructing the full OS and GPU.
# The optional source path also allows testing the parent revision as a negative control.
$source = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $MemorySourcePath).Path)
$adapter = [regex]::Match($source, '(?ms)^bool Memory::WriteExclusive128\([^\r\n]*\)\r?\n\{.*?^\}')
if (-not $adapter.Success) { throw 'Memory::WriteExclusive128 adapter not found.' }
Set-Content -LiteralPath (Join-Path $OutputDirectory 'memory_exclusive128_adapter.inc') -Value $adapter.Value

$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
$setup = ''
if (-not $compiler) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Run this script from a Visual Studio x64 developer shell.'
    }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
    $vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    $setup = '@call "' + $vcvars + '" >nul' + [Environment]::NewLine + '@if errorlevel 1 exit /b %errorlevel%' + [Environment]::NewLine
}
$arguments = @('/nologo', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4',
    ('/I"' + $repoRoot + '\src"'), ('/I"' + $OutputDirectory + '"'),
    ('"' + $PSScriptRoot + '\exclusive_memory128.cpp"'), '/Fe:exclusive-memory128-tests.exe')
Set-Content -LiteralPath (Join-Path $OutputDirectory 'tests.rsp') -Value $arguments
$command = $setup + @'
@cl @tests.rsp
@if errorlevel 1 exit /b %errorlevel%
@exclusive-memory128-tests.exe
@exit /b %errorlevel%
'@
Set-Content -LiteralPath (Join-Path $OutputDirectory 'run.cmd') -Value $command
Push-Location -LiteralPath $OutputDirectory
try {
    & cmd.exe /d /c run.cmd
    if ($LASTEXITCODE -ne 0) { throw "Exclusive memory128 tests failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
