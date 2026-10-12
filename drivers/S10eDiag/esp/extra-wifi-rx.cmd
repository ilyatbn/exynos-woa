@echo off
rem S10eAgent extra step (M12 M7b rx test, notes/m12-wifi-m7-rx-audit.md): does the BCM4375 hand received frames to
rem the host? The AP's EAPOL msg 1/4 arrives BEFORE the passphrase matters, so the test runs against the home SSID
rem with a deliberately WRONG placeholder passphrase (never the real one; the user's credentials are not stored
rem anywhere in this repo). Expected with a working rx path: "eapol rx" > 0 and a MIC failure at msg 3 (or the AP
rem retrying msg 1), instead of "rx cpl w 0". Package: %ESP%\S10EDIAG\drivers\Exynos9820Wlan\ (1.0.7.3+).
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
set W=%L%\wifi
mkdir "%W%"
set D=%TMP%\s10e-wlan
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers\Exynos9820Wlan" "%D%" > "%W%\xcopy.txt" 2>&1
reg add HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters /v Ssid /t REG_SZ /d HomeX-5 /f > "%W%\reg.txt" 2>&1
reg add HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters /v Passphrase /t REG_SZ /d rx-test-wrong-passphrase /f >> "%W%\reg.txt" 2>&1
s10ediag mark f: pnputil add wlan
pnputil /add-driver "%D%\Exynos9820Wlan.inf" /install > "%W%\pnputil-add.txt" 2>&1
pnputil /restart-device "ACPI\SAMS9825\0" > "%W%\restart.txt" 2>&1
s10ediag mark f: wait 60 s
ping -n 60 127.0.0.1 > nul
pnputil /enum-devices /deviceid "ACPI\SAMS9825" /properties > "%W%\dev-wlan.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Wlan.txt" "%W%\Exynos9820Wlan.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Wlan-run.txt" "%W%\run.txt" > nul 2>&1
findstr /i /c:"eapol" /c:"rx cpl" /c:"JOIN" /c:"DEAUTH" /c:"rx frames" /c:"SET_SSID" /c:"rxbufpost" "%W%\run.txt" > "%W%\key.txt" 2>nul
for /f "usebackq delims=" %%L in ("%W%\key.txt") do s10ediag mark "f: %%L"
s10ediag mark f: done
