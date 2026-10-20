@echo off
rem Build usbcaltmode.exe -> %1 for %2 = arm64 (default, the phone) or x64 (a PC "-dry" run of the plan).
rem tools/phone/usbc/usbcaltmode.c, notes/g18-hdmi-prep.md. SPDX-License-Identifier: GPL-2.0
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
set ARCH=%~2
if "%ARCH%"=="" set ARCH=arm64
if /i "%ARCH%"=="x64" (set VC=x64& set MACH=X64) else (set VC=x64_arm64& set MACH=ARM64)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" %VC% >nul
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\usbcaltmode.obj" /Fe"%OUT%\usbcaltmode.exe" "%~dp0usbcaltmode.c" /link /MACHINE:%MACH% /Brepro setupapi.lib
if errorlevel 1 exit /b 2
del "%OUT%\usbcaltmode.obj" >nul 2>&1
echo USBCALTMODE_BUILD_OK %OUT%\usbcaltmode.exe
