@echo off
rem build-bcetc-test.cmd [mesa tree] [generated-headers dir] - build + run bcetc_test.exe (G26, notes/g26-bc-etc2.md)
rem on the PC (x64). The generated u_format_gen.h comes from a configured Mesa build (build-mesa-g76.ps1 host stage).
rem SPDX-License-Identifier: MIT
setlocal
set MESA=%~1
if "%MESA%"=="" set MESA=C:\Users\ilya\code\mali\mesa-26.2-g76
set GEN=%~2
if "%GEN%"=="" set GEN=C:\Users\ilya\code\mali\g12-work\build-host-x64\src
set OUT=%TEMP%\bcetc-test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cl.exe /nologo /W4 /O2 /std:c11 /D _CRT_SECURE_NO_WARNINGS /I "%MESA%\src\gallium\frontends\d3d10umd" /I "%MESA%\src\gallium\include" /I "%MESA%\src" /I "%MESA%\include" /I "%GEN%" /Fo"%OUT%\\" /Fe"%OUT%\bcetc_test.exe" "%~dp0bcetc_test.c"
if errorlevel 1 exit /b 2
"%OUT%\bcetc_test.exe"
