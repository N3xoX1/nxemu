$ErrorActionPreference='Stop'
$taskVcvars='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$taskEnv=& cmd.exe /d /c "call `"$taskVcvars`" >nul && set"
foreach($taskLine in $taskEnv){if($taskLine -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$taskDir=Join-Path $PSScriptRoot '..\..\build\pr396-corrections\immortal-freeze'
& cl.exe /nologo /std:c++20 /EHsc /O2 /MT /utf-8 (Join-Path $taskDir 'ips-production-test.cpp') "/Fo$taskDir\ips-production-test.obj" "/Fe$taskDir\ips-production-test.exe" *> (Join-Path $taskDir 'ips-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $taskDir 'ips-build.log') -Tail 20;throw 'IPS test build failed'}
& (Join-Path $taskDir 'ips-production-test.exe') (Join-Path $taskDir 'vp9-mod\exefs\70F3F6751D73C644BCE904CCB414E35B00000000000000000000000000000000.ips')
if($LASTEXITCODE -ne 0){throw 'IPS production parser verification failed'}
