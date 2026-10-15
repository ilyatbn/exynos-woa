@echo off
rem Build btspp.exe for the PC (x64) and the phone (ARM64). Usage: build.cmd <outdir>  -> <outdir>\x64, <outdir>\arm64
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
set VC="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%OUT%\x64" mkdir "%OUT%\x64"
if not exist "%OUT%\arm64" mkdir "%OUT%\arm64"
setlocal
call %VC% x64 > nul
cl /nologo /W4 /WX /wd4201 /O2 /MT "%~dp0btspp.c" /Fo"%OUT%\x64\\" /Fe"%OUT%\x64\btspp.exe" || exit /b 1
endlocal
setlocal
call %VC% x64_arm64 > nul
cl /nologo /W4 /WX /wd4201 /O2 /MT "%~dp0btspp.c" /Fo"%OUT%\arm64\\" /Fe"%OUT%\arm64\btspp.exe" || exit /b 1
endlocal
echo built %OUT%
