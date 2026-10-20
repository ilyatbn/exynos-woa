@echo off
rem build-wascap.cmd [OUTDIR] - wascap.exe (ARM64): WASAPI shared-mode capture test for the phone mic (notes/d8-mic.md)
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
cl.exe /nologo /W4 /WX /O2 /MT /D UNICODE /D _UNICODE /Fo"%OUT%\wascap.obj" /Fe"%OUT%\wascap.exe" "%~dp0wascap.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE ole32.lib
if errorlevel 1 exit /b 1
del "%OUT%\wascap.obj" >nul 2>&1
echo WASCAP_BUILD_OK %OUT%\wascap.exe
