@echo off
rem Disable Windows Update (run as SYSTEM). Log -> C:\S10eTmp\wuoff.log
set P=HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate
reg add "%P%\AU" /v NoAutoUpdate /t REG_DWORD /d 1 /f
reg add "%P%\AU" /v AUOptions /t REG_DWORD /d 1 /f
reg add "%P%\AU" /v NoAutoRebootWithLoggedOnUsers /t REG_DWORD /d 1 /f
reg add "%P%" /v ExcludeWUDriversInQualityUpdate /t REG_DWORD /d 1 /f
reg add "%P%" /v DisableWindowsUpdateAccess /t REG_DWORD /d 1 /f
reg add "%P%" /v SetDisableUXWUAccess /t REG_DWORD /d 1 /f
reg add "%P%" /v DoNotConnectToWindowsUpdateInternetLocations /t REG_DWORD /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\DriverSearching" /v SearchOrderConfig /t REG_DWORD /d 0 /f
reg add "HKLM\SOFTWARE\Policies\Microsoft\WindowsStore" /v AutoDownload /t REG_DWORD /d 2 /f
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\DeliveryOptimization" /v DODownloadMode /t REG_DWORD /d 0 /f
rem Services: start type only (no stop - servicing may still be finishing). Effective next boot.
for %%S in (wuauserv UsoSvc WaaSMedicSvc DoSvc) do reg add "HKLM\SYSTEM\CurrentControlSet\Services\%%S" /v Start /t REG_DWORD /d 4 /f
for %%T in ("\Microsoft\Windows\WindowsUpdate\Scheduled Start" "\Microsoft\Windows\UpdateOrchestrator\Schedule Scan" "\Microsoft\Windows\UpdateOrchestrator\Schedule Scan Static Task" "\Microsoft\Windows\UpdateOrchestrator\Report policies" "\Microsoft\Windows\UpdateOrchestrator\Schedule Wake To Work" "\Microsoft\Windows\UpdateOrchestrator\Schedule Maintenance Work" "\Microsoft\Windows\UpdateOrchestrator\Reboot_AC" "\Microsoft\Windows\UpdateOrchestrator\Reboot_Battery" "\Microsoft\Windows\UpdateOrchestrator\USO_UxBroker" "\Microsoft\Windows\UpdateOrchestrator\UUS Failover Task" "\Microsoft\Windows\WaaSMedic\PerformRemediation") do schtasks /change /tn %%T /disable
echo WUOFF_DONE
