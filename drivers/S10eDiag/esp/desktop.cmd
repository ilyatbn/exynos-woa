@echo off
rem S10eAgent: desktop helpers for interactive use (user request, notes/m11-s10ediag.md). Idempotent.
rem Called from collect.cmd as SYSTEM. Everything goes on the Public desktop (visible to the S10e user).
rem 2026-10-16 user: "get rid of all the cmd files on the desktop we dont need them anymore" - the .cmd launchers
rem (Collect logs, Task Manager, On-Screen Keyboard, Command Prompt, Device Manager, Rotate x3, Touch log) are no
rem longer created and old copies are removed every boot. The .exe helpers below stay.
set PD=C:\Users\Public\Desktop
del /f /q "%PD%\*.cmd" > nul 2>&1
if not exist C:\S10eTmp\req mkdir C:\S10eTmp\req
icacls C:\S10eTmp\req /grant *S-1-5-32-545:(OI)(CI)M > nul 2>&1

rem m16: a .cmd reboot launcher failed to start ("Windows cannot access the specified device, path or file") -
rem s10ereboot.exe is copied onto the desktop itself.
if not exist "%PD%\Reboot to TWRP.exe" copy /y C:\Windows\System32\s10ereboot.exe "%PD%\Reboot to TWRP.exe" > nul
rem Display scale slider 100-300 %% (s10escale.exe, custom scaling via LogPixels; applies after sign-out).
if exist C:\Windows\System32\s10escale.exe if not exist "%PD%\Display scale.exe" copy /y C:\Windows\System32\s10escale.exe "%PD%\Display scale.exe" > nul
rem G33: per-game shader cache list + clear (s10eshadercache.exe, the Mali UMD's %LOCALAPPDATA%\MaliUMD\ShaderCache).
if exist C:\Windows\System32\s10eshadercache.exe if not exist "%PD%\Shader cache.exe" copy /y C:\Windows\System32\s10eshadercache.exe "%PD%\Shader cache.exe" > nul
dir "%PD%" > "%L%\desktop.txt" 2>&1
