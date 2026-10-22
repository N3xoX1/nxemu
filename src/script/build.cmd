@ECHO OFF
SETLOCAL

set BuildMode=Release
if not "%1" == "" set BuildMode=%1
if "%1" == "debug" set BuildMode=Debug
if "%1" == "release" set BuildMode=Release

set MSVC-BUILDER=
set origdir=%cd%
cd /d %~dp0..\..\
set base_dir=%cd%
cd /d %origdir%

if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\devenv.com" ( set MSVC-BUILDER="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\devenv.com")
if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\devenv.com" ( set MSVC-BUILDER="C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\devenv.com")
if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\devenv.com" ( set MSVC-BUILDER="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\devenv.com")

if %MSVC-BUILDER% == "" ( 
	echo can not find visual studio 2022
	goto :end
)

:: Build x64 version of the software
%MSVC-BUILDER% "%base_dir%\nxemu.sln" /rebuild "%BuildMode%|x64"
set Result=%ERRORLEVEL%
echo Done - ERRORLEVEL: %Result%
IF %Result% NEQ 0 goto :EndErr

echo Build ok
goto :end

:EndErr
ENDLOCAL
echo Build failed
exit /B 1

:End
ENDLOCAL
exit /B 0
