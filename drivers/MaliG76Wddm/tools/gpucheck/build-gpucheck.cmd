@echo off
rem Build GpuCheck.exe (ARM64, GUI, test-signed) -> %1. notes/gpu-hw-path.md. Same toolchain as d3dfill.
rem SPDX-License-Identifier: GPL-2.0
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\gpucheck
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /D UNICODE /I "%SRC%..\..\include" /Fo"%OUT%\gpucheck.obj" /Fe"%OUT%\gpucheck.exe" "%SRC%gpucheck.c" /link /MACHINE:ARM64 /SUBSYSTEM:WINDOWS dxgi.lib d3d11.lib d3dcompiler.lib user32.lib gdi32.lib pdh.lib /Brepro
if errorlevel 1 exit /b 2
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\gpucheck.exe" >nul
if errorlevel 1 exit /b 3
del "%OUT%\gpucheck.obj" >nul 2>&1
echo GPUCHECK_BUILD_OK %OUT%
