@echo off
rem Build gltest.exe (user mode, test-signed) -> %1. notes/gl-icd.md. Same toolchain as drivers\S10eDiag\build.bat.
rem %2 = x64 builds gltest64.exe for x64 (emulated) processes - the ARM64EC ICD path (GL4); default ARM64.
rem SPDX-License-Identifier: GPL-2.0
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\gltest
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
set VCARCH=x64_arm64
set MACH=ARM64
set EXE=gltest
if /i "%~2"=="x64" (
  set VCARCH=x64
  set MACH=X64
  set EXE=gltest64
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /wd4201 /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /I "%SRC%..\..\include" /Fo"%OUT%\%EXE%.obj" /Fe"%OUT%\%EXE%.exe" "%SRC%gltest.cpp" /link /MACHINE:%MACH% /SUBSYSTEM:CONSOLE opengl32.lib gdi32.lib user32.lib /Brepro
if errorlevel 1 exit /b 2
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\%EXE%.exe" >nul
if errorlevel 1 exit /b 3
del "%OUT%\%EXE%.obj" >nul 2>&1
echo GLTEST_BUILD_OK %OUT%\%EXE%.exe
