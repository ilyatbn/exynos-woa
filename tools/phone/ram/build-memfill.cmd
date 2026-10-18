@echo off
rem Build memfill.exe (ARM64) -> %1. tools/phone/ram/memfill.c, notes/ram-6gb.md. SPDX-License-Identifier: GPL-2.0
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\memfill.obj" /Fe"%OUT%\memfill.exe" "%~dp0memfill.c" /link /MACHINE:ARM64 /Brepro
if errorlevel 1 exit /b 2
del "%OUT%\memfill.obj" >nul 2>&1
echo MEMFILL_BUILD_OK
