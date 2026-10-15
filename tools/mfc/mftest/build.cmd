@echo off
rem Build mftest.exe (ARM64 console, Media Foundation Source Reader decode test). Usage: build.cmd <outdir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 > nul || exit /b 1
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 "%~dp0mftest.cpp" /Fo"%OUT%\\" /Fe"%OUT%\mftest.exe" || exit /b 1
echo built %OUT%\mftest.exe
