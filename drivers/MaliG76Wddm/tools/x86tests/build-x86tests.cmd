@echo off
rem Build x86 (32-bit) copies of the D3D11 test tools -> this directory: d3dtri32.exe, d3dvbring32.exe. They run in
rem x86 emulation on the 32-bit UMD (SysWOW64\maliumd32.dll), like the 32-bit games - threaded-context tests,
rem notes/perf-threaded-context.md T5. Same flags as the ARM64 builds (build-d3dtri.cmd / build-d3dvbring.cmd).
rem SPDX-License-Identifier: GPL-2.0
setlocal
set T=%~dp0..
set OUT=%~dp0
set OUT=%OUT:~0,-1%
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_x86 >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /EHsc /I "%T%\..\include" /Fo"%OUT%\\" /Fe"%OUT%\d3dtri32.exe" "%T%\d3dtri\d3dtri.c" "%T%\d3dtri\overlay.cpp" /link /MACHINE:X86 /SUBSYSTEM:CONSOLE dxgi.lib d3d11.lib d3dcompiler.lib user32.lib gdi32.lib d2d1.lib dwrite.lib /Brepro
if errorlevel 1 exit /b 2
cl.exe /nologo /W4 /WX /wd4201 /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /I "%T%\..\include" /Fo"%OUT%\\" /Fe"%OUT%\d3dvbring32.exe" "%T%\d3dvbring\d3dvbring.cpp" /link /MACHINE:X86 /SUBSYSTEM:CONSOLE d3d11.lib d3dcompiler.lib /Brepro
if errorlevel 1 exit /b 3
cl.exe /nologo /W4 /WX /wd4201 /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /I "%T%\..\include" /Fo"%OUT%\\" /Fe"%OUT%\d3dcbdyn32.exe" "%T%\d3dcbdyn\d3dcbdyn.c" /link /MACHINE:X86 /SUBSYSTEM:CONSOLE d3d11.lib d3dcompiler.lib /Brepro
if errorlevel 1 exit /b 5
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\d3dtri32.exe" "%OUT%\d3dvbring32.exe" "%OUT%\d3dcbdyn32.exe" >nul
if errorlevel 1 exit /b 4
del "%OUT%\*.obj" >nul 2>&1
echo X86TESTS_BUILD_OK %OUT%
