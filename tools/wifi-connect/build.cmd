@echo off
rem Builds wifi-connect.exe for Windows on ARM64 (MSVC x64->ARM64 cross tools, static CRT, UAC requireAdministrator).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsamd64_arm64.bat" > nul 2>nul
cl /nologo /O2 /W4 /WX /MT wifi-connect.c /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'" user32.lib advapi32.lib setupapi.lib gdi32.lib /OUT:wifi-connect.exe
