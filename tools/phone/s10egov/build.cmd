@echo off
rem Build s10egov.exe (ARM64): governor + the ACPM client (s10eacpm.c without its main). Usage: build.cmd <outdir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 > nul || exit /b 1
cl /nologo /W4 /WX /O2 /MT /DS10EACPM_NO_MAIN "%~dp0s10egov.c" "%~dp0..\s10eacpm\s10eacpm.c" /Fo"%OUT%\\" /Fe"%OUT%\s10egov.exe" || exit /b 1
echo built %OUT%\s10egov.exe
