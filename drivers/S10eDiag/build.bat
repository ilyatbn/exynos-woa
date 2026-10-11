@echo off
rem S10eDiag build (ARM64): S10eDiag.sys + s10ediag.exe + S10eAgent.exe -> %1 (default %TEMP%\s10ediag)
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
cl.exe /c /nologo /W4 /WX /Ox /Os /D _ARM64_ /D ARM64 /D _USE_DECLSPECS_FOR_SAL=1 /D STD_CALL /D _WIN32_WINNT=0x0A00 /D WINVER=0x0A00 /D WINNT=1 /D NTDDI_VERSION=0xA000010 /GF /Gm- /Zp8 /GS /guard:cf /Gy /Zc:wchar_t- /GR- /kernel /FI"%KIT%\Include\%KV%\shared\warning.h" /Fo"%OUT%\S10eDiag.obj" "%SRC%S10eDiag.c"
if errorlevel 1 exit /b 2
link.exe /NOLOGO /WX /NODEFAULTLIB /MANIFEST:NO /DEBUG:NONE /SUBSYSTEM:NATIVE,"10.00" /Driver /OPT:REF /OPT:ICF /ENTRY:"GsDriverEntry" /RELEASE /Brepro /MACHINE:ARM64 /guard:cf /kernel /osversion:10.0 /OUT:"%OUT%\S10eDiag.sys" "%OUT%\S10eDiag.obj" "%KIT%\Lib\%KV%\km\arm64\BufferOverflowFastFailK.lib" "%KIT%\Lib\%KV%\km\arm64\ntoskrnl.lib" "%KIT%\Lib\%KV%\km\arm64\hal.lib" "%KIT%\Lib\%KV%\km\arm64\wdmsec.lib" "%KIT%\Lib\%KV%\km\arm64\aux_klib.lib" "%KIT%\Lib\%KV%\um\arm64\arm64rt.lib"
if errorlevel 1 exit /b 3

set INCLUDE=%KIT%\Include\%KV%\um;%KIT%\Include\%KV%\shared;%KIT%\Include\%KV%\ucrt;%SAVED_INCLUDE%
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D UNICODE /D _UNICODE /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10eDiagCtl.obj" /Fe"%OUT%\s10ediag.exe" "%SRC%S10eDiagCtl.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 4
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D UNICODE /D _UNICODE /Fo"%OUT%\S10eAgent.obj" /Fe"%OUT%\S10eAgent.exe" "%SRC%S10eAgent.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro advapi32.lib
if errorlevel 1 exit /b 5
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10eRotate.obj" /Fe"%OUT%\s10erotate.exe" "%SRC%S10eRotate.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro user32.lib
if errorlevel 1 exit /b 7
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10ePerf.obj" /Fe"%OUT%\s10eperf.exe" "%SRC%S10ePerf.c" /link /MACHINE:ARM64 /SUBSYSTEM:CONSOLE /Brepro
if errorlevel 1 exit /b 8
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D UNICODE /D _UNICODE /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10eTouchLog.obj" /Fe"%OUT%\s10etouchlog.exe" "%SRC%S10eTouchLog.c" /link /MACHINE:ARM64 /SUBSYSTEM:WINDOWS /Brepro user32.lib gdi32.lib advapi32.lib
if errorlevel 1 exit /b 9
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D UNICODE /D _UNICODE /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10eReboot.obj" /Fe"%OUT%\s10ereboot.exe" "%SRC%S10eReboot.c" /link /MACHINE:ARM64 /SUBSYSTEM:WINDOWS /Brepro user32.lib
if errorlevel 1 exit /b 10
cl.exe /nologo /W4 /WX /O2 /MT /GS /guard:cf /D UNICODE /D _UNICODE /D _CRT_SECURE_NO_WARNINGS /Fo"%OUT%\S10eScale.obj" /Fe"%OUT%\s10escale.exe" "%SRC%S10eScale.c" /link /MACHINE:ARM64 /SUBSYSTEM:WINDOWS /Brepro user32.lib gdi32.lib advapi32.lib comctl32.lib
if errorlevel 1 exit /b 11

"%KIT%\bin\%KV%\x64\signtool.exe" sign /sha1 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 /fd SHA256 "%OUT%\S10eDiag.sys" "%OUT%\s10ediag.exe" "%OUT%\S10eAgent.exe" "%OUT%\s10erotate.exe" "%OUT%\s10eperf.exe" "%OUT%\s10etouchlog.exe" "%OUT%\s10ereboot.exe" "%OUT%\s10escale.exe" >nul
if errorlevel 1 exit /b 6
echo S10EDIAG_BUILD_OK %OUT%
