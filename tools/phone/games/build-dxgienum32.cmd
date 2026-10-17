@echo off
rem build-dxgienum32.cmd OUTDIR - dxgienum32.exe (x86: what a 32-bit game sees through WOW64)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_x86 >nul
cl.exe /nologo /W4 /WX /O2 /MT /Fo"%~1\dxgienum32.obj" /Fe"%~1\dxgienum32.exe" "%~dp0dxgienum.c" /link /MACHINE:X86 /SUBSYSTEM:CONSOLE /Brepro dxgi.lib dxguid.lib
