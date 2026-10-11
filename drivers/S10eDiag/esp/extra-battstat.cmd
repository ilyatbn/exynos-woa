@echo off
rem S10eAgent extra step: battery driver state over ~80 s (charger watchdog kick test, notes/battery-driver-m1.md).
set T=%L%\battstat
mkdir "%T%" > nul 2>&1
for /l %%i in (1,1,5) do (
  echo --- sample %%i >> "%T%\params.txt"
  reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters" >> "%T%\params.txt" 2>&1
  ping -n 21 127.0.0.1 > nul
)
powercfg /batteryreport /xml /output "%T%\battery-report.xml" > nul 2>&1
sc query S10eBattery > "%T%\sc.txt" 2>&1
