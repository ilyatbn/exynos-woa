@echo off
rem Build keyrec.exe + keyplay.exe (ARM64) -> %1. NFSU2 race automation (notes/games-nfsu2.md). SPDX-License-Identifier: GPL-2.0
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
for %%P in (keyrec keyplay) do (
  cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\%%P.obj" /Fe"%OUT%\%%P.exe" "%~dp0%%P.c" /link /MACHINE:ARM64 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /Brepro user32.lib
  if errorlevel 1 exit /b 2
  del "%OUT%\%%P.obj" >nul 2>&1
)
echo KEYREC_BUILD_OK
