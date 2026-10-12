$ErrorActionPreference='Stop'
$shutdownRepo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$shutdownOut=Join-Path $shutdownRepo 'build/performance-live-20261011/parallel-shutdown-tests'
New-Item -ItemType Directory -Force -Path $shutdownOut | Out-Null
$shutdownEnvironment=& cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && set'
foreach($shutdownLine in $shutdownEnvironment){if($shutdownLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
foreach($shutdownLegacy in @($false,$true)) {
    $shutdownName=if($shutdownLegacy){'legacy-tls'}else{'scoped'}
    $shutdownArguments=@('/nologo','/std:c++20','/EHsc','/O2','/MT','/LD',"/I$shutdownRepo/src",(Join-Path $PSScriptRoot 'parallel-readback-dll.cpp'),"/Fo$shutdownOut/$shutdownName.obj","/Fe$shutdownOut/$shutdownName.dll")
    if($shutdownLegacy){$shutdownArguments+='/DNXEMU_LEGACY_TLS'}
    & cl.exe @shutdownArguments *> (Join-Path $shutdownOut "$shutdownName-build.log")
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $shutdownOut "$shutdownName-build.log") -Tail 15;throw 'Shutdown fixture DLL build failed'}
}
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT (Join-Path $PSScriptRoot 'parallel-readback-dll-host.cpp') "/Fo$shutdownOut/host.obj" "/Fe$shutdownOut/host.exe" *> (Join-Path $shutdownOut 'host-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $shutdownOut 'host-build.log') -Tail 15;throw 'Shutdown host build failed'}
$shutdownPrevious=$env:NXEMU_PARALLEL_READBACK_THREADS
try {
    foreach($shutdownMode in @('0','2','4')) {
        $env:NXEMU_PARALLEL_READBACK_THREADS=$shutdownMode
        $shutdownProcess=Start-Process -FilePath (Join-Path $shutdownOut 'host.exe') -ArgumentList ('"'+(Join-Path $shutdownOut 'scoped.dll')+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $shutdownOut "mode-$shutdownMode.txt") -RedirectStandardError (Join-Path $shutdownOut "mode-$shutdownMode-errors.txt")
        try {
            # Retain the process handle before a fast fixture exits; otherwise
            # Windows PowerShell can return a null ExitCode after WaitForExit.
            $null=$shutdownProcess.Handle
            if(!$shutdownProcess.WaitForExit(15000)){$shutdownProcess.Kill();$shutdownProcess.WaitForExit();throw 'Scoped shutdown fixture timed out'}
            if($shutdownProcess.ExitCode -ne 0){throw "Scoped shutdown fixture failed: $($shutdownProcess.ExitCode)"}
            Get-Content (Join-Path $shutdownOut "mode-$shutdownMode.txt")
        } finally {$shutdownProcess.Dispose()}
    }
    $shutdownProcess=Start-Process -FilePath (Join-Path $shutdownOut 'host.exe') -ArgumentList ('"'+(Join-Path $shutdownOut 'legacy-tls.dll')+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $shutdownOut 'legacy.txt') -RedirectStandardError (Join-Path $shutdownOut 'legacy-errors.txt')
    try {
        if($shutdownProcess.WaitForExit(5000)){throw 'Legacy control did not reproduce the expected DLL TLS shutdown deadlock'}
        $shutdownProcess.Kill();$shutdownProcess.WaitForExit()
        'PASS legacy TLS owner reproduces shutdown deadlock; disposable test child terminated' | Tee-Object -FilePath (Join-Path $shutdownOut 'legacy-result.txt')
    } finally {$shutdownProcess.Dispose()}
} finally {$env:NXEMU_PARALLEL_READBACK_THREADS=$shutdownPrevious}
