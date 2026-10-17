@echo off
rem S10eDiag build (ARM64): S10ePower.sys + s10ediag.exe + S10eAgent.exe -> %1 (default %TEMP%\s10ediag)
rem Same toolchain/flags as the Exynos9820Ufs exact build; test-signed with the WDK test certificate.
setlocal
set SRC=%~dp0
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\s10ediag
if not exist "%OUT%" mkdir "%OUT%"
set KIT=C:\Program Files (x86)\Windows Kits\10
set KV=10.0.26100.0
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64_arm64 >nul
if errorlevel 1 exit /b 1
set SAVED_INCLUDE=%INCLUDE%

set INCLUDE=%KIT%\Include\%KV%\km;%KIT%\Include\%KV%\shared;%SAVED_INCLUDE%
cl.exe /c /nologo /W4 /WX /Ox /Os /D _ARM64_ /D ARM64 /D _USE_DECLSPECS_FOR_SAL=1 /D STD_CALL /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000010 /GF /Gm- /Zp8 /GS /guard:cf /Gy /Zc:wchar_t- /GR- /kernel /FI"%KIT%\Include\%KV%\shared\warning.h" /Fo"%OUT%\S10ePower.obj" "%SRC%S10ePower.c"
if errorlevel 1 exit /b 2
link.exe /NOLOGO /WX /NODEFAULTLIB /MANIFEST:NO /DEBUG:NONE /SUBSYSTEM:NATIVE,"10.00" /Driver /OPT:REF /OPT:ICF /ENTRY:"GsDriverEntry" /RELEASE /Brepro /MACHINE:ARM64 /guard:cf /kernel /osversion:10.0 /OUT:"%OUT%\S10ePower.sys" "%OUT%\S10ePower.obj" "%KIT%\Lib\%KV%\km\arm64\BufferOverflowFastFailK.lib" "%KIT%\Lib\%KV%\km\arm64\ntoskrnl.lib" "%KIT%\Lib\%KV%\km\arm64\hal.lib" "%KIT%\Lib\%KV%\km\arm64\wdmsec.lib" "%KIT%\Lib\%KV%\km\arm64\aux_klib.lib" "%KIT%\Lib\%KV%\um\arm64\arm64rt.lib"
if errorlevel 1 exit /b 3
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\S10ePower.sys" >nul
if errorlevel 1 exit /b 4
del "%OUT%\S10ePower.obj" >nul 2>&1
echo S10EPOWER_BUILD_OK %OUT%
set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%SAVED_INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\s10epowerctl.obj" /Fe"%OUT%\s10epowerctl.exe" "%SRC%s10epowerctl.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 5
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\s10epowerctl.exe" >nul
del "%OUT%\s10epowerctl.obj" >nul 2>&1
echo S10EPOWERCTL_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\s10egovernor.obj" /Fe"%OUT%\s10egovernor.exe" "%SRC%s10egovernor.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro advapi32.lib user32.lib pdh.lib
if errorlevel 1 exit /b 6
"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\s10egovernor.exe" >nul
del "%OUT%\s10egovernor.obj" >nul 2>&1
echo S10EGOVERNOR_BUILD_OK
cl.exe /nologo /W4 /WX /O2 /MT /Fo"%OUT%\cpusets.obj" /Fe"%OUT%\cpusets.exe" "%SRC%cpusets.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 7
del "%OUT%\cpusets.obj" >nul 2>&1
echo CPUSETS_BUILD_OK
