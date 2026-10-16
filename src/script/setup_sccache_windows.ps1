$ErrorActionPreference = 'Stop'

# Install a pinned prebuilt binary for CI; CMake uses it as a launcher with
# Ninja. No cl.exe shim or changes to the Visual Studio property sheets.
$version = '0.18.0'
$archiveName = "sccache-v$version-x86_64-pc-windows-msvc.zip"
$archiveHash = '8965c74d5e8a225244f741e18ad2f3f504f48228dc1bac948fc22761a348363d'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$toolDir = Join-Path $repo 'build/sccache-tools'
New-Item -ItemType Directory -Force -Path $toolDir | Out-Null
$archive = Join-Path $toolDir $archiveName
Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/mozilla/sccache/releases/download/v$version/$archiveName" -OutFile $archive
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $archiveHash) {
    throw 'The downloaded sccache archive failed its SHA256 check.'
}
Expand-Archive -Path $archive -DestinationPath $toolDir -Force
$sccache = Join-Path $toolDir "sccache-v$version-x86_64-pc-windows-msvc/sccache.exe"
Copy-Item $sccache (Join-Path $toolDir 'sccache.exe') -Force
$env:PATH = "$toolDir;$env:PATH"
