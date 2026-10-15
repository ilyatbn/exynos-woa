@echo off
rem Build btpair.exe (ARM64, C++/WinRT console) for the phone's Windows. Usage: build.cmd <outdir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 > nul || exit /b 1
cl /nologo /std:c++20 /EHsc /O1 /MT /permissive- /DWIN32_LEAN_AND_MEAN "%~dp0btpair.cpp" /Fo"%OUT%\\" /Fe"%OUT%\btpair.exe" /link windowsapp.lib || exit /b 1
echo built %OUT%\btpair.exe
