@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): M12 WiFi milestone 1 (notes/m12-wifi-m1.md).
rem Did UEFI publish the WLAN device (status word 'WLAN' = 0x4E414C57 at 0x8FE0F00C), install the
rem Exynos9820Wlan probe package, and collect what it read from the BCM4375. Called from collect.cmd with
rem ESP / OUT / L set. Package expected at %ESP%\S10EDIAG\drivers\Exynos9820Wlan\ (+ wdktestcert.cer).
rem Same temp-dir workaround as extra-touch.cmd (C:\Windows\TEMP index corrupt on this unit, run 6).
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set W=%L%\wlan
mkdir "%W%"

s10ediag mark w: status word
s10ediag peek 0x8FE0F00C > "%W%\wlan-status-word.txt" 2>&1

set D=%TMP%\s10e-wlan
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers\Exynos9820Wlan" "%D%" > "%W%\xcopy.txt" 2>&1
for /r "%D%" %%F in (*) do certutil -hashfile "%%F" SHA256 >> "%W%\hashes.txt" 2>&1
if exist "%ESP%\S10EDIAG\drivers\wdktestcert.cer" (
  certutil -addstore -f Root "%ESP%\S10EDIAG\drivers\wdktestcert.cer" > "%W%\cert.txt" 2>&1
  certutil -addstore -f TrustedPublisher "%ESP%\S10EDIAG\drivers\wdktestcert.cer" >> "%W%\cert.txt" 2>&1
)

s10ediag mark w: pnputil add wlan
pnputil /add-driver "%D%\Exynos9820Wlan.inf" /install > "%W%\pnputil-add.txt" 2>&1
ping -n 15 127.0.0.1 > nul

s10ediag mark w: enum
pnputil /enum-devices /deviceid "ACPI\SAMS9825" /properties > "%W%\dev-wlan.txt" 2>&1
sc query Exynos9820Wlan > "%W%\sc.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Wlan\Parameters" > "%W%\params.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Wlan.txt" "%W%\Exynos9820Wlan.txt" > nul 2>&1
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%W%\setupapi.dev.log" > nul 2>&1
s10ediag mark w: done
