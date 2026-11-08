@echo off
rem Spike A1: builds the debug APK with the cached Gradle 9.1.0 and Android Studio's JDK.
rem Output: %USERPROFILE%\build\sorus\a1\app\outputs\apk\debug\app-debug.apk
set "JAVA_HOME=%ProgramFiles%\Android\Android Studio\jbr"
set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
for /d %%d in ("%USERPROFILE%\.gradle\wrapper\dists\gradle-9.1.0-all\*") do set "G=%%d\gradle-9.1.0\bin\gradle.bat"
call "%G%" -p "%~dp0." --project-cache-dir "%USERPROFILE%\build\sorus\a1\.gradle" --console=plain assembleDebug %*
