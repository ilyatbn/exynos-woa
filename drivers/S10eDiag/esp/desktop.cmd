@echo off
rem S10eAgent: one-time desktop helpers for interactive use (user request, notes/m11-s10ediag.md). Idempotent.
rem Called from collect.cmd as SYSTEM. Everything goes on the Public desktop (visible to the S10e user).
rem Plain .cmd launchers only: the first version made .lnk files through powershell WScript.Shell and that step
rem never finished on one core (interactive session after run 13).
set PD=C:\Users\Public\Desktop
if exist "%PD%\Touch log.cmd" if exist "%PD%\Reboot to TWRP.exe" exit /b 0
if not exist C:\S10eTmp\req mkdir C:\S10eTmp\req
icacls C:\S10eTmp\req /grant *S-1-5-32-545:(OI)(CI)M > nul 2>&1

rem Request files are picked up by the S10eAgent service (LocalSystem) within 2 s - no UAC prompt.
> "%PD%\Collect logs.cmd" echo @echo x^> C:\S10eTmp\req\collect.req
>> "%PD%\Collect logs.cmd" echo @echo Collecting logs to the ESP (S10EDIAG\out\last), Windows keeps running.
>> "%PD%\Collect logs.cmd" echo @timeout /t 5
> "%PD%\Task Manager.cmd" echo @start "" C:\Windows\System32\Taskmgr.exe
> "%PD%\On-Screen Keyboard.cmd" echo @start "" C:\Windows\System32\osk.exe
> "%PD%\Command Prompt.cmd" echo @start "" C:\Windows\System32\cmd.exe
> "%PD%\Device Manager.cmd" echo @start "" C:\Windows\System32\devmgmt.msc
> "%PD%\Rotate landscape.cmd" echo @C:\Windows\System32\s10erotate.exe 90
> "%PD%\Rotate portrait.cmd" echo @C:\Windows\System32\s10erotate.exe 0
> "%PD%\Rotate next.cmd" echo @C:\Windows\System32\s10erotate.exe cycle
> "%PD%\Touch log.cmd" echo @start "" C:\Windows\System32\s10etouchlog.exe 40
rem Written last: its presence means the set is complete (the idempotency check above).
rem m16: the .cmd version failed to launch ("Windows cannot access the specified device, path or file")
rem while every other .cmd worked - replaced by s10ereboot.exe copied onto the desktop itself.
del /f /q "%PD%\Reboot to TWRP.cmd" > nul 2>&1
copy /y C:\Windows\System32\s10ereboot.exe "%PD%\Reboot to TWRP.exe" > nul
dir "%PD%" > "%L%\desktop.txt" 2>&1
