@echo off
rem Spike A3: builds a3fit.exe (desktop fit-accuracy sweep) from the core's own sources into %USERPROFILE%\build\sorus\a3.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "B=%USERPROFILE%\build\sorus\a3"
set "C=%~dp0..\..\core\src"
if not exist "%B%" mkdir "%B%"
cl /nologo /std:c++20 /O2 /W4 /EHsc /I"%C%" "%~dp0a3fit.cpp" "%C%\band.cpp" "%C%\params.cpp" "%C%\dp_fit.cpp" /Fo"%B%\\" /Fe"%B%\a3fit.exe" || exit /b 1
