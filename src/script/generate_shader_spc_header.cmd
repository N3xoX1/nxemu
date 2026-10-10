@echo off
setlocal EnableDelayedExpansion

set origdir=%cd%
cd /d %~dp0..\..
set base_dir=%cd%
cd /d %origdir%

set "OutDir=%~1"
set "FullPath=%~2"

for %%F in ("%FullPath%") do (
    set "Filename=%%~nF"
    set "Extension=%%~xF"
)

set "Extension=!Extension:~1!"
set FilenameUpper=%Filename%
set ExtensionUpper=%Extension%
call :ToUpper FilenameUpper
call :ToUpper ExtensionUpper

"%OutDir%string_shader_header.exe" "%FullPath%" "%base_dir%\src\yuzu_video_core\host_shaders\source_shader.h.in" "%base_dir%\src\yuzu_video_core\host_shaders\%Filename%_%Extension%.h"
if %errorlevel% neq 0 goto :cmEnd

glslangValidator.exe -V --quiet -I"%base_dir%\src\3rd_party\fidelityfx_fsr\ffx-fsr" --variable-name !FilenameUpper!_!ExtensionUpper!_SPV -o "%base_dir%\src\yuzu_video_core\host_shaders\%Filename%_%Extension%_spv.h" "%FullPath%" --target-env spirv1.3
if %errorlevel% neq 0 goto :cmEnd

:cmEnd
endlocal & call :cmErrorLevel %errorlevel% & goto :cmDone
:cmErrorLevel
exit /b %1
:cmDone
if %errorlevel% neq 0 goto :VCEnd

:ToUpper
if not defined %~1 EXIT /b
for %%a in ("a=A" "b=B" "c=C" "d=D" "e=E" "f=F" "g=G" "h=H" "i=I"
            "j=J" "k=K" "l=L" "m=M" "n=N" "o=O" "p=P" "q=Q" "r=R"
            "s=S" "t=T" "u=U" "v=V" "w=W" "x=X" "y=Y" "z=Z") do (
    call set %~1=%%%~1:%%~a%%
)
EXIT /b