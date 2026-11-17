@echo off
rem GpuVM viewer build: sets up MSVC (Build Tools included) and Qt, then runs upstream's build script.
setlocal
set "QT_BIN=C:\Qt\6.12.0\msvc2022_64\bin"
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%i"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
set "PATH=%QT_BIN%;%PATH%"
cd /d "%~dp0.."
call scripts\build-arch.bat %1
