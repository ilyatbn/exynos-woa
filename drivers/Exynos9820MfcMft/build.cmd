@echo off
rem Build Exynos9820MfcMft.dll (ARM64 Media Foundation H.264 decoder MFT on the Exynos9820Mfc IOCTLs, D10 V4).
rem Usage: build.cmd <outdir>
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 > nul || exit /b 1
cl /nologo /W4 /WX /O2 /MT /EHsc /std:c++17 /LD "%~dp0Mft.cpp" /Fo"%OUT%\\" /Fe"%OUT%\Exynos9820MfcMft.dll" /link /DEF:"%~dp0Mft.def" ole32.lib advapi32.lib user32.lib || exit /b 1
copy /y "%~dp0install.cmd" "%OUT%\install.cmd" > nul
echo built %OUT%\Exynos9820MfcMft.dll
