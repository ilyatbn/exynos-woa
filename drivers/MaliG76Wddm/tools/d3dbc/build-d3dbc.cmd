@echo off
rem Build d3dbc.exe -> %1 for %2 = x86 (default: the 32-bit UMD games use), arm64, or x64 (emulated on the phone - G27). notes/g23-bc-emulation.md.
rem SPDX-License-Identifier: GPL-2.0
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\d3dbc
set ARCH=%~2
if "%ARCH%"=="" set ARCH=x86
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
if /i "%ARCH%"=="arm64" (set VC=x64_arm64& set MACH=ARM64& set EXE=d3dbc.exe) else if /i "%ARCH%"=="x64" (set VC=x64& set MACH=X64& set EXE=d3dbc64x.exe) else (set VC=x64_x86& set MACH=X86& set EXE=d3dbc32.exe)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" %VC% >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /GS /D _CRT_SECURE_NO_WARNINGS /D NOMINMAX /Fo"%OUT%\d3dbc.obj" /Fe"%OUT%\%EXE%" "%SRC%d3dbc.cpp" /link /MACHINE:%MACH% /SUBSYSTEM:CONSOLE dxgi.lib d3d11.lib d3dcompiler.lib dxguid.lib /Brepro
if errorlevel 1 exit /b 2
del "%OUT%\d3dbc.obj" >nul 2>&1
echo D3DBC_BUILD_OK %OUT%\%EXE%
