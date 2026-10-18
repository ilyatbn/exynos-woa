@echo off
rem Build S10eIddView.exe (ARM64) -> %1. tools/phone/rec/iddview/S10eIddView.cpp (G18 remote-monitor PoC viewer).
rem SPDX-License-Identifier: GPL-2.0
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
cl.exe /nologo /W3 /O2 /MT /EHsc /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10eIddView.obj" /Fe"%OUT%\S10eIddView.exe" "%~dp0S10eIddView.cpp" /link /MACHINE:ARM64 /Brepro ws2_32.lib ole32.lib oleaut32.lib windowscodecs.lib
if errorlevel 1 exit /b 2
del "%OUT%\S10eIddView.obj" >nul 2>&1
echo IDDVIEW_BUILD_OK
