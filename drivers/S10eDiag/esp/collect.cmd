@echo off
rem S10eAgent script (ESP: \S10EDIAG\collect.cmd). Runs as SYSTEM once explorer.exe has been up for 30 s.
rem ESP = the ESP drive letter (set by S10eAgent), OUT = %ESP%\S10EDIAG\out. Every run overwrites %OUT%\last.
rem notes/m11-s10ediag.md.
rem   %ESP%\S10EDIAG\full.flg  present -> also the slow inventory (systeminfo, driverquery, full pnp dump)
rem   %ESP%\S10EDIAG\extra.cmd present -> called after the standard collection
rem   %ESP%\S10EDIAG\stay.flg  present -> collect only, Windows keeps running (interactive use)
rem   STAY=1 (env, set by the agent for a "Collect logs" desktop request) -> same
rem   %ESP%\S10EDIAG\desktop.cmd present -> desktop helper shortcuts (idempotent)
rem   otherwise: clean restart into TWRP (5 min hard-reset fallback). \WINRUN.FLG is LEFT on the ESP: if the
rem   restart crashes, the bugcheck reset goes through the firmware boot pair (PRAM dump, then TWRP).
rem   Delete it from TWRP before the next run.
rem Every step is announced with `s10ediag mark` first: the PRAM mark ring survives a crash (WINPRAM.BIN +0xA000).
setlocal
set L=%OUT%\last
if exist "%L%" rd /s /q "%L%"
mkdir "%L%"
echo === collect start %DATE% %TIME%
s10ediag info

if exist %ESP%\S10EDIAG\desktop.cmd (
  s10ediag mark c: desktop helpers
  call %ESP%\S10EDIAG\desktop.cmd
)

echo --- quick
s10ediag mark c: pnputil problem
pnputil /enum-devices /problem > "%L%\pnp-problem.txt" 2>&1
rem Get-PnpDevice dropped: 105 s on one core (run 4); pnputil /problem covers it
s10ediag mark c: sc query
sc query S10eDiag > "%L%\sc.txt" 2>&1
sc query S10eAgent >> "%L%\sc.txt" 2>&1
s10ediag mark c: powercfg
powercfg /a > "%L%\powercfg-a.txt" 2>&1
s10ediag mark c: wevtutil
wevtutil epl System "%L%\System.evtx"
wevtutil epl Application "%L%\Application.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%L%\KernelPnP.evtx"

if exist %ESP%\S10EDIAG\full.flg (
  echo --- full
  s10ediag mark c: full inventory
  systeminfo > "%L%\systeminfo.txt" 2>&1
  tasklist /v > "%L%\tasks.txt" 2>&1
  driverquery /v /fo csv > "%L%\drivers.csv" 2>&1
  pnputil /enum-devices /drivers > "%L%\pnp-all.txt" 2>&1
  wevtutil epl Setup "%L%\Setup.evtx"
  wevtutil epl Microsoft-Windows-Winlogon/Operational "%L%\Winlogon.evtx"
  wevtutil epl Microsoft-Windows-Kernel-Boot/Operational "%L%\KernelBoot.evtx"
  wevtutil epl Microsoft-Windows-Diagnostics-Performance/Operational "%L%\DiagPerf.evtx"
)

if exist C:\S10eTmp\req\touchlog.txt copy /y C:\S10eTmp\req\touchlog.txt "%L%\touchlog.txt" > nul
if exist %ESP%\S10EDIAG\extra.cmd (
  echo --- extra
  s10ediag mark c: extra.cmd
  call %ESP%\S10EDIAG\extra.cmd
)

echo === collect done %DATE% %TIME%
s10ediag mark c: done
if "%STAY%"=="1" (
  echo STAY=1 ^(agent collect request^) - leaving Windows running
  exit /b 0
)
if exist %ESP%\S10EDIAG\stay.flg (
  echo stay.flg present - leaving Windows running
  exit /b 0
)

echo --- back to TWRP
s10ediag mark c: restart to TWRP
rem 2026-10-12: CLEAN restart - %ESP%\S10EDIAG\torecovery.flg + shutdown /r; BootLogDxe (boot #113+) sees the OS
rem restart marker + the flag and goes to TWRP. The hard reset lost unflushed registry changes (driver installs,
rem MaliG76Wddm Parameters). Loop guard: a flag still present here means the firmware did not consume it (an older
rem image continues into Windows after an OS restart) -> delete it and use the old flush + hard reset.
s10ediag info
if exist %ESP%\S10EDIAG\torecovery.flg (
  del /f /q %ESP%\S10EDIAG\torecovery.flg
  s10ediag mark c: torecovery.flg not consumed - hard reset
  s10ediag flushreset
  exit /b 0
)
echo recovery > %ESP%\S10EDIAG\torecovery.flg
s10ediag mark c: clean restart (torecovery.flg)
shutdown /r /t 0 /f
ping -n 180 127.0.0.1 > nul
s10ediag mark c: clean restart did not happen - hard reset
s10ediag flushreset
exit /b 0
