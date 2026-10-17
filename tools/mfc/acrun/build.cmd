@echo off
rem Build acrun.exe (ARM64 console, AppContainer runner, G17 V6d). Usage: build.cmd <outdir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 > nul || exit /b 1
cl /nologo /W4 /WX /O2 /MT "%~dp0acrun.c" /Fo"%OUT%\\" /Fe"%OUT%\acrun.exe" || exit /b 1
echo built %OUT%\acrun.exe
