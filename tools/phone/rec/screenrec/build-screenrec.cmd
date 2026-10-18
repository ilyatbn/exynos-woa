@echo off
rem Build screenrec.exe -> %1 for %2 = arm64 (default). tools/phone/rec/screenrec/screenrec.cpp, notes/g27-x64-apps.md.
rem SPDX-License-Identifier: GPL-2.0
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\screenrec
set ARCH=%~2
if "%ARCH%"=="" set ARCH=arm64
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
if /i "%ARCH%"=="arm64" (set VC=x64_arm64& set MACH=ARM64& set EXE=screenrec.exe) else if /i "%ARCH%"=="x64" (set VC=x64& set MACH=X64& set EXE=screenrec64x.exe) else (set VC=x64_x86& set MACH=X86& set EXE=screenrec32.exe)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" %VC% >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /GS /D _CRT_SECURE_NO_WARNINGS /D NOMINMAX /Fo"%OUT%\screenrec.obj" /Fe"%OUT%\%EXE%" "%SRC%screenrec.cpp" /link /MACHINE:%MACH% /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib windowscodecs.lib /Brepro
if errorlevel 1 exit /b 2
del "%OUT%\screenrec.obj" >nul 2>&1
echo SCREENREC_BUILD_OK %OUT%\%EXE%
