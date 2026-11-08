@echo off
rem Spike W1: builds sorus_w1_apo.dll and meter.exe into %USERPROFILE%\build\sorus\w1.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "B=%USERPROFILE%\build\sorus\w1"
if not exist "%B%" mkdir "%B%"
cl /nologo /std:c++20 /O2 /W4 /WX /EHsc /LD "%~dp0apo.cpp" /Fo"%B%\\" /Fe"%B%\sorus_w1_apo.dll" /link /DEF:"%~dp0apo.def" ole32.lib || exit /b 1
cl /nologo /std:c++20 /O2 /W4 /WX /EHsc "%~dp0meter.cpp" /Fo"%B%\\" /Fe"%B%\meter.exe" /link ole32.lib || exit /b 1
cl /nologo /std:c++20 /O2 /W4 /WX /EHsc "%~dp0selftest.cpp" /Fo"%B%\\" /Fe"%B%\selftest.exe" || exit /b 1
"%B%\selftest.exe" "%B%\sorus_w1_apo.dll"
