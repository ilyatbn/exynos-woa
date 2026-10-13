@echo off
rem Build d3dtri.exe (ARM64, user mode, test-signed) -> %1. notes/gpu-hw-path.md. Same toolchain as d3dfill.
rem SPDX-License-Identifier: GPL-2.0
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\d3dtri
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\d3dtri.obj" /Fe"%OUT%\d3dtri.exe" "%SRC%d3dtri.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE dxgi.lib d3d11.lib d3dcompiler.lib user32.lib gdi32.lib /Brepro
if errorlevel 1 exit /b 2
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\d3dtri.exe" >nul
if errorlevel 1 exit /b 3
del "%OUT%\d3dtri.obj" >nul 2>&1
echo D3DTRI_BUILD_OK %OUT%
