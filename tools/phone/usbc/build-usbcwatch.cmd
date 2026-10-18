@echo off
rem Build usbcwatch.exe (ARM64) -> %1. tools/phone/usbc/usbcwatch.c, notes/g18-hdmi-dp-alt.md. SPDX-License-Identifier: GPL-2.0
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\usbcwatch.obj" /Fe"%OUT%\usbcwatch.exe" "%~dp0usbcwatch.c" /link /MACHINE:ARM64 /Brepro setupapi.lib
if errorlevel 1 exit /b 2
del "%OUT%\usbcwatch.obj" >nul 2>&1
echo USBCWATCH_BUILD_OK
