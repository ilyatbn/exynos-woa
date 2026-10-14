@echo off
rem Build d2dtext.exe (ARM64, user mode, test-signed) -> %1. notes/gpu-hw-path.md. Same toolchain as d3dtri.
rem SPDX-License-Identifier: GPL-2.0
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\d2dtext
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /EHsc /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /D UNICODE /Fo"%OUT%\d2dtext.obj" /Fe"%OUT%\d2dtext.exe" "%SRC%d2dtext.cpp" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE dxgi.lib d3d11.lib d3dcompiler.lib d2d1.lib dwrite.lib /Brepro
if errorlevel 1 exit /b 2
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\d2dtext.exe" >nul
if errorlevel 1 exit /b 3
del "%OUT%\d2dtext.obj" >nul 2>&1
echo D2DTEXT_BUILD_OK %OUT%
