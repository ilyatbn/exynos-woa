@echo off
rem S10eAgent extra step (M12 M9, notes/m12-wifi-wificx.md "ESP test"): install the WiFiCx client package
rem Exynos9820WlanCx (Class=Net, same ACPI\SAMS9825 as the KMDF Exynos9820Wlan) and collect its logs.
rem Package: %ESP%\S10EDIAG\drivers\Exynos9820WlanCx\ (s10e-artifacts\wifi-cx<n>). Firmware blobs as for
rem extra-wifi-rx.cmd (%ESP%\S10EDIAG\bcm4375\ -> \SystemRoot\System32\drivers\bcm4375\).
rem The install replaces the running KMDF driver: that is a device restart while the firmware runs (the "DL 6"
rem case, notes/m12-wifi-m7.md); since T1 the download first holds the D11 cores in reset. If the run log shows
rem DL 6 anyway, reboot once and run this script again: the second boot loads the WiFiCx driver from cold.
rem No credentials are stored or typed here: connecting is done by the user in the Windows network flyout.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
set W=%L%\wificx
mkdir "%W%"
set D=%TMP%\s10e-wlancx
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers\Exynos9820WlanCx" "%D%" > "%W%\xcopy.txt" 2>&1
if not exist "%SystemRoot%\System32\drivers\bcm4375" mkdir "%SystemRoot%\System32\drivers\bcm4375"
xcopy /y "%ESP%\S10EDIAG\bcm4375\*" "%SystemRoot%\System32\drivers\bcm4375\" > "%W%\xcopy-fw.txt" 2>&1
s10ediag mark f: pnputil add wlancx
pnputil /add-driver "%D%\Exynos9820WlanCx.inf" /install > "%W%\pnputil-add.txt" 2>&1
s10ediag mark f: wait 90 s (bring-up + first scans)
ping -n 90 127.0.0.1 > nul
pnputil /enum-devices /deviceid "ACPI\SAMS9825" /drivers /properties > "%W%\dev-wlan.txt" 2>&1
netsh wlan show drivers > "%W%\netsh-drivers.txt" 2>&1
netsh wlan show interfaces > "%W%\netsh-interfaces.txt" 2>&1
netsh wlan show networks mode=bssid > "%W%\netsh-networks.txt" 2>&1
ipconfig /all > "%W%\ipconfig.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820WlanCx.txt" "%W%\Exynos9820WlanCx.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820WlanCx-run.txt" "%W%\run.txt" > nul 2>&1
for %%N in (1 2 3 4) do copy /y "%SystemRoot%\System32\LogFiles\Exynos9820WlanCx-run.%%N.txt" "%W%\run.%%N.txt" > nul 2>&1
findstr /l /i /c:"RESULT" /c:"DL " /c:"[dl] D11" /c:"mismatch" /c:"CX " /c:"[cx]" /c:"[scan] WDI" /c:"CONNECT" /c:"[join] WDI" /c:"[key]" /c:"[link]" /c:"[tx] packet layout" "%W%\Exynos9820WlanCx.txt" "%W%\run.txt" > "%W%\key.txt" 2>nul
findstr /l /c:"[wdi]" "%W%\run.txt" > "%W%\wdi.txt" 2>nul
findstr /i /c:"State" /c:"SSID" /c:"Radio type" /c:"Signal" "%W%\netsh-interfaces.txt" >> "%W%\key.txt" 2>nul
for /f "usebackq delims=" %%L in ("%W%\key.txt") do s10ediag mark "f: %%L"
s10ediag mark f: done
