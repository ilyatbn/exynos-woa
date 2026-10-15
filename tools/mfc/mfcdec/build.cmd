@echo off
rem Build mfcdec.exe (ARM64 console) for the phone's Windows. Usage: build.cmd <outdir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 > nul || exit /b 1
cl /nologo /W4 /WX /O2 /MT /D_CRT_SECURE_NO_WARNINGS "%~dp0mfcdec.c" /Fo"%OUT%\\" /Fe"%OUT%\mfcdec.exe" || exit /b 1
echo built %OUT%\mfcdec.exe
