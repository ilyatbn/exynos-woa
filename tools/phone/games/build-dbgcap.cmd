@echo off
rem build-dbgcap.cmd OUTDIR - dbgcap.exe for ARM64 (same toolchain as drivers/S10eDiag/build.bat)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%~1\dbgcap.obj" /Fe"%~1\dbgcap.exe" "%~dp0dbgcap.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
