@echo off
rem Build gpuclock.exe (ARM64, test-signed) -> %1.
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\gpuclock
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
if errorlevel 1 exit /b 1
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /I "%SRC%..\..\include" /Fo"%OUT%\gpuclock.obj" /Fe"%OUT%\gpuclock.exe" "%SRC%gpuclock.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE gdi32.lib /Brepro
if errorlevel 1 exit /b 2
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\gpuclock.exe" >nul
del "%OUT%\gpuclock.obj" >nul 2>&1
echo GPUCLOCK_BUILD_OK %OUT%
