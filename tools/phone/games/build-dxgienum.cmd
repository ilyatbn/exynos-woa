@echo off
rem build-dxgienum.cmd OUTDIR - dxgienum.exe (ARM64); the x86 dxgienum32.exe is built by build-dxgienum32.cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
cl.exe /nologo /W4 /WX /O2 /MT /Fo"%~1\dxgienum.obj" /Fe"%~1\dxgienum.exe" "%~dp0dxgienum.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro dxgi.lib dxguid.lib
