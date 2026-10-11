@echo off
rem S10eAgent extra step: battery driver state vs what Windows reports, ~6 min (notes/battery-driver-m1.md).
rem 6 samples 60 s apart: the driver saves its diagnostics every 15 polls (5 min), so QueryStatusCount shows
rem whether the battery class re-reads status; the WMI queries are Windows' own view at the same moment.
set T=%L%\battstat
mkdir "%T%" > nul 2>&1
for /l %%i in (1,1,6) do (
  echo --- sample %%i >> "%T%\params.txt"
  reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters" >> "%T%\params.txt" 2>&1
  echo --- sample %%i >> "%T%\windows-view.txt"
  powershell -NoProfile -Command "Get-CimInstance Win32_Battery | Format-List BatteryStatus,EstimatedChargeRemaining,EstimatedRunTime; Get-CimInstance -Namespace root/wmi BatteryStatus | Format-List PowerOnline,Charging,Discharging,RemainingCapacity,ChargeRate,DischargeRate,Voltage" >> "%T%\windows-view.txt" 2>&1
  ping -n 61 127.0.0.1 > nul
)
powercfg /batteryreport /xml /output "%T%\battery-report.xml" > nul 2>&1
sc query S10eBattery > "%T%\sc.txt" 2>&1
