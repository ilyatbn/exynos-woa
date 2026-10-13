@echo off
rem S10eAgent extra step (M12, notes/m12-wifi-wificx.md "User report 2026-10-13"): read-only snapshot of the WiFi state
rem in the running Windows - device/driver status, services, NDIS adapters, WLAN AutoConfig's view, the driver's
rem own logs - written to the ESP while Windows runs (an offline read after a hard reset sees stale NTFS data).
rem Changes nothing. Output: out\last\wifistate\ and a numbered copy in \S10EDIAG\out\wifi-history\ (kept).
set Y=%L%\wifistate
mkdir "%Y%" > nul 2>&1
s10ediag mark w: wifi state
pnputil /enum-devices /deviceid "ACPI\SAMS9825" /drivers /properties > "%Y%\dev-wlan.txt" 2>&1
pnputil /enum-devices /class Net > "%Y%\dev-net.txt" 2>&1
rem DSDT _STA of WLAN/GPU0/XHC0/AUD0 reads the progress page words [2..5] at 0x8FE0F008..0x8FE0F017 ('USBH' 'WLAN'
rem 'GPU0' 'NOAU'); the runtime value tells "firmware never set it" from "Windows did not enumerate it anyway".
s10ediag peek 0x8FE0F000 0x20 > "%Y%\progress.txt" 2>&1
pnputil /enum-devices /connected > "%Y%\dev-connected.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SAMS9828" >> "%Y%\dev-wlan.txt" 2>&1
sc queryex Exynos9820WlanCx > "%Y%\sc.txt" 2>&1
sc queryex Exynos9820Wlan >> "%Y%\sc.txt" 2>&1
sc queryex WlanSvc >> "%Y%\sc.txt" 2>&1
sc queryex wcmsvc >> "%Y%\sc.txt" 2>&1
netsh wlan show drivers > "%Y%\netsh-drivers.txt" 2>&1
netsh wlan show interfaces > "%Y%\netsh-interfaces.txt" 2>&1
netsh wlan show networks mode=bssid > "%Y%\netsh-networks.txt" 2>&1
netsh wlan show settings > "%Y%\netsh-settings.txt" 2>&1
netsh wlan show profiles > "%Y%\netsh-profiles.txt" 2>&1
ipconfig /all > "%Y%\ipconfig.txt" 2>&1
powershell -NoProfile -Command "Get-NetAdapter -IncludeHidden | Format-List Name,InterfaceDescription,Status,MediaConnectionState,AdminStatus,NdisPhysicalMedium,MacAddress,DriverFileName,HardwareInterface; Get-PnpDevice -InstanceId 'ACPI\SAMS9825\0' | Format-List *" > "%Y%\netadapter.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Control\RadioManagement" /s > "%Y%\radio.txt" 2>&1
wevtutil qe Microsoft-Windows-WLAN-AutoConfig/Operational /c:60 /rd:true /f:text > "%Y%\wlan-autoconfig.txt" 2>&1
wevtutil qe System /c:400 /rd:true /f:text /q:"*[System[Provider[@Name='Microsoft-Windows-Kernel-PnP' or @Name='Service Control Manager' or @Name='NDIS' or @Name='Microsoft-Windows-WLAN-AutoConfig' or @Name='Exynos9820WlanCx']]]" > "%Y%\system-net.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820WlanCx.txt" "%Y%\Exynos9820WlanCx.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820WlanCx-run.txt" "%Y%\run.txt" > nul 2>&1
for %%N in (1 2) do copy /y "%SystemRoot%\System32\LogFiles\Exynos9820WlanCx-run.%%N.txt" "%Y%\run.%%N.txt" > nul 2>&1
for /f "usebackq tokens=1,*" %%A in (`findstr /i /c:"Status:" /c:"Problem" "%Y%\dev-wlan.txt"`) do s10ediag mark "w: %%A %%B"
for /f "usebackq tokens=1,*" %%A in (`findstr /i /c:"STATE" "%Y%\sc.txt"`) do s10ediag mark "w: sc %%B"
for /f "usebackq delims=" %%L in ("%Y%\progress.txt") do s10ediag mark "w: %%L"
for /f "usebackq delims=" %%L in (`findstr /i /c:"There is" /c:" State " /c:"SSID" "%Y%\netsh-interfaces.txt"`) do s10ediag mark "w: %%L"
rem numbered history copy (out\last is replaced every boot); first free number, as in extra-gfxperf.cmd
set H=%ESP%\S10EDIAG\out\wifi-history
mkdir "%H%" > nul 2>&1
set N=
for /l %%I in (1,1,999) do if not defined N if not exist "%H%\%%I\" set N=%%I
mkdir "%H%\%N%" > nul 2>&1
copy /y "%Y%\*.txt" "%H%\%N%\" > nul 2>&1
s10ediag mark "w: history %N%"
s10ediag flush > nul 2>&1
