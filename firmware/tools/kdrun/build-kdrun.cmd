@echo off
rem kdrun.exe (host x64) - notes/m14-smp.md. Output %~dp0kdrun.exe. No dbgeng.lib import: kdrun.c loads the Kits
rem dbgeng.dll by full path at run time (an import resolves to the old inbox System32 copy).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
set D=C:\Program Files (x86)\Windows Kits\10\Debuggers
cl.exe /nologo /W4 /O2 /D _CRT_SECURE_NO_WARNINGS /I "%D%\inc" /Fo"%TEMP%\kdrun.obj" /Fe"%~dp0kdrun.exe" "%~dp0kdrun.c"
