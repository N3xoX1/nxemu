@echo off
setlocal
set "ZipFileName=nxemu.zip"
set "VSPlatform=x64"
if not "%~1"=="" set "ZipFileName=%~1"
if not "%~2"=="" set "VSPlatform=%~2"
set "base_dir=%~dp0..\.."
set "output=%base_dir%\bin\%VSPlatform%\Release"
if not exist "%base_dir%\package" mkdir "%base_dir%\package"
if not exist "%output%\nxemu.exe" goto EndErr
if not exist "%output%\SDL2.dll" goto EndErr
for %%m in (cpu loader video operating_system) do if not exist "%output%\modules\%%m" goto EndErr
pushd "%output%"
7z a -tzip "%base_dir%\package\%ZipFileName%" nxemu.exe SDL2.dll modules ui -xr!*.pdb -xr!*.lib -xr!*.exp
if errorlevel 1 (
    popd
    goto EndErr
)
popd
endlocal
exit /b 0
:EndErr
echo Packaging failed
endlocal
exit /b 1
