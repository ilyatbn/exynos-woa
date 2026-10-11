@echo off
rem S10eAgent script (ESP: \S10EDIAG\collect.cmd). Runs as SYSTEM once explorer.exe has been up for 30 s.
rem ESP = the ESP drive letter (set by S10eAgent), OUT = %ESP%\S10EDIAG\out. Every run overwrites %OUT%\last.
rem notes/m11-s10ediag.md.
rem   %ESP%\S10EDIAG\full.flg  present -> also the slow inventory (systeminfo, driverquery, full pnp dump)
rem   %ESP%\S10EDIAG\extra.cmd present -> called after the standard collection
rem   %ESP%\S10EDIAG\stay.flg  present -> collect only, Windows keeps running (interactive use)
rem   otherwise: clean restart into TWRP (5 min hard-reset fallback). \WINRUN.FLG is LEFT on the ESP: if the
rem   restart crashes, the bugcheck reset goes through the firmware boot pair (PRAM dump, then TWRP).
rem   Delete it from TWRP before the next run.
setlocal
set L=%OUT%\last
if exist "%L%" rd /s /q "%L%"
mkdir "%L%"
echo === collect start %DATE% %TIME%
s10ediag info

echo --- quick
pnputil /enum-devices /problem > "%L%\pnp-problem.txt" 2>&1
powershell -NoProfile -Command "Get-PnpDevice | Sort-Object Status,Class | Format-Table -AutoSize Status,Problem,Class,FriendlyName,InstanceId | Out-String -Width 400" > "%L%\pnp.txt" 2>&1
sc query S10eDiag > "%L%\sc.txt" 2>&1
sc query S10eAgent >> "%L%\sc.txt" 2>&1
powercfg /a > "%L%\powercfg-a.txt" 2>&1
wevtutil epl System "%L%\System.evtx"
wevtutil epl Application "%L%\Application.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%L%\KernelPnP.evtx"

if exist %ESP%\S10EDIAG\full.flg (
  echo --- full
  systeminfo > "%L%\systeminfo.txt" 2>&1
  tasklist /v > "%L%\tasks.txt" 2>&1
  driverquery /v /fo csv > "%L%\drivers.csv" 2>&1
  pnputil /enum-devices /drivers > "%L%\pnp-all.txt" 2>&1
  wevtutil epl Setup "%L%\Setup.evtx"
  wevtutil epl Microsoft-Windows-Winlogon/Operational "%L%\Winlogon.evtx"
  wevtutil epl Microsoft-Windows-Kernel-Boot/Operational "%L%\KernelBoot.evtx"
  wevtutil epl Microsoft-Windows-Diagnostics-Performance/Operational "%L%\DiagPerf.evtx"
)

if exist %ESP%\S10EDIAG\extra.cmd (
  echo --- extra
  call %ESP%\S10EDIAG\extra.cmd
)

echo === collect done %DATE% %TIME%
if exist %ESP%\S10EDIAG\stay.flg (
  echo stay.flg present - leaving Windows running
  exit /b 0
)

echo --- back to TWRP
s10ediag recovery
s10ediag fallback 300
s10ediag info
shutdown /r /t 5 /f /d p:4:1
exit /b 0
