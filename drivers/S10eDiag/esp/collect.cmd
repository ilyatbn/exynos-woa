@echo off
rem S10eAgent script (ESP: \S10EDIAG\collect.cmd). Runs as SYSTEM once explorer.exe is up.
rem OUT = S:\S10EDIAG\out. Every run overwrites %OUT%\last. notes/m11-s10ediag.md.
rem   S:\S10EDIAG\stay.flg present -> collect only, Windows keeps running (interactive use)
rem   absent                       -> collect, then clean restart into TWRP (5 min hard-reset fallback)
setlocal
set L=%OUT%\last
if exist "%L%" rd /s /q "%L%"
mkdir "%L%"
echo === collect start %DATE% %TIME%
s10ediag info

echo --- system
systeminfo > "%L%\systeminfo.txt" 2>&1
tasklist /v > "%L%\tasks.txt" 2>&1
driverquery /v /fo csv > "%L%\drivers.csv" 2>&1
powercfg /a > "%L%\powercfg-a.txt" 2>&1
sc query S10eDiag > "%L%\sc.txt" 2>&1
sc query S10eAgent >> "%L%\sc.txt" 2>&1

echo --- pnp
pnputil /enum-devices /problem > "%L%\pnp-problem.txt" 2>&1
pnputil /enum-devices /drivers > "%L%\pnp-all.txt" 2>&1
powershell -NoProfile -Command "Get-PnpDevice | Sort-Object Status,Class | Format-Table -AutoSize Status,Problem,Class,FriendlyName,InstanceId | Out-String -Width 400" > "%L%\pnp.txt" 2>&1

echo --- event logs
wevtutil epl System "%L%\System.evtx"
wevtutil epl Application "%L%\Application.evtx"
wevtutil epl Setup "%L%\Setup.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%L%\KernelPnP.evtx"
wevtutil epl Microsoft-Windows-Winlogon/Operational "%L%\Winlogon.evtx"
wevtutil epl Microsoft-Windows-Kernel-Boot/Operational "%L%\KernelBoot.evtx"
wevtutil epl Microsoft-Windows-Diagnostics-Performance/Operational "%L%\DiagPerf.evtx"
wevtutil qe System /c:300 /rd:true /f:text > "%L%\System-last300.txt" 2>&1

echo --- extra (S:\S10EDIAG\extra.cmd, optional)
if exist S:\S10EDIAG\extra.cmd call S:\S10EDIAG\extra.cmd

echo === collect done %DATE% %TIME%
if exist S:\S10EDIAG\stay.flg (
  echo stay.flg present - leaving Windows running
  exit /b 0
)

echo --- back to TWRP
del /f /q S:\WINRUN.FLG 2>nul
s10ediag recovery
s10ediag fallback 300
s10ediag info
shutdown /r /t 5 /f /d p:4:1
exit /b 0
