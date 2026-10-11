@echo off
rem S10eAgent: one-time desktop helpers for interactive use (user request, notes/m11-s10ediag.md). Idempotent.
rem Called from collect.cmd as SYSTEM. Everything goes on the Public desktop (visible to the S10e user).
set PD=C:\Users\Public\Desktop
if not exist C:\S10eTmp\req mkdir C:\S10eTmp\req
icacls C:\S10eTmp\req /grant *S-1-5-32-545:(OI)(CI)M > nul 2>&1

rem Request files are picked up by the S10eAgent service (LocalSystem) within 2 s - no UAC prompt.
> "%PD%\Reboot to TWRP.cmd" echo @echo x^> C:\S10eTmp\req\recovery.req
>> "%PD%\Reboot to TWRP.cmd" echo @echo Rebooting to TWRP...
> "%PD%\Collect logs.cmd" echo @echo x^> C:\S10eTmp\req\collect.req
>> "%PD%\Collect logs.cmd" echo @echo Collecting logs to the ESP (S10EDIAG\out\last), Windows keeps running.
>> "%PD%\Collect logs.cmd" echo @timeout /t 5

powershell -NoProfile -Command ^
  "$s = New-Object -ComObject WScript.Shell;" ^
  "foreach ($l in @(@('Task Manager','C:\Windows\System32\Taskmgr.exe'),@('On-Screen Keyboard','C:\Windows\System32\osk.exe'),@('Command Prompt','C:\Windows\System32\cmd.exe'),@('Device Manager','C:\Windows\System32\devmgmt.msc'))) {" ^
  "  $k = $s.CreateShortcut('%PD%\' + $l[0] + '.lnk'); $k.TargetPath = $l[1]; $k.Save() }" > "%L%\desktop.txt" 2>&1
dir "%PD%" >> "%L%\desktop.txt" 2>&1
