@echo off
rem Configure, build and test the core engine. Extra args go to ctest, e.g.: scripts\core.cmd -R clamp
rem Build output lives outside OneDrive so sync never locks build files.
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
set "B=%USERPROFILE%\build\sorus\core"
cmake -S "%~dp0..\core" -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release >nul || exit /b 1
cmake --build "%B%" || exit /b 1
ctest --test-dir "%B%" --output-on-failure %*
