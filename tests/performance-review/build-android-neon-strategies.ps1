param(
    [Parameter(Mandatory=$true)][string]$Ndk,
    [string]$Output
)
$ErrorActionPreference='Stop'
$neonRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if(!$Output){$Output=Join-Path $neonRepo 'build/performance-live-20261011/neon-android-tests'}
$neonTools=Join-Path (Resolve-Path -LiteralPath $Ndk).Path 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$neonCompiler=Join-Path $neonTools 'aarch64-linux-android28-clang++.cmd'
if(!(Test-Path -LiteralPath $neonCompiler)){throw 'Expected Windows Android NDK with the arm64 API 28 Clang wrapper'}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$neonBinary=Join-Path $Output 'neon-strategies'
& $neonCompiler -O2 -std=c++20 -Wall -Wextra -Werror -static-libstdc++ "-I$neonRepo/src" (Join-Path $PSScriptRoot 'neon-reference-strategies.cpp') -o $neonBinary *> (Join-Path $Output 'build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $Output 'build.log') -Tail 20;throw 'Android NEON fixture compilation failed'}
& (Join-Path $neonTools 'llvm-objdump.exe') -d --demangle $neonBinary > (Join-Path $Output 'disassembly.txt')
if($LASTEXITCODE -ne 0){throw 'Android disassembly failed'}
Write-Output "Built Android arm64-v8a API 28 fixture: $neonBinary"
Write-Output 'Run correctness on the device before --benchmark; QEMU timing is not a performance measurement.'
