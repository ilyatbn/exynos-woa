@echo off
rem build-stats.cmd [mesa tree] [generated dir] [x64|x86] - EAC table statistics on the PC (G26).
setlocal
set MESA=%~1
if "%MESA%"=="" set MESA=C:\Users\ilya\code\mali\mesa-26.2-g76
set GEN=%~2
if "%GEN%"=="" set GEN=C:\Users\ilya\code\mali\g12-work\build-host-x64\src
set ARCH=%~3
if "%ARCH%"=="" set ARCH=x64
set OUT=%TEMP%\bcetc-test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul 2>&1
cl.exe /nologo /W4 /O2 /std:c11 /I "%MESA%\src\gallium\frontends\d3d10umd" /I "%MESA%\src" /I "%MESA%\include" /I "%GEN%" /Fo"%OUT%\\" /Fe"%OUT%\stats-%ARCH%.exe" "%~dp0stats.c"
if errorlevel 1 exit /b 2
"%OUT%\stats-%ARCH%.exe"
