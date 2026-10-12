@echo off
rem S10eAgent extra step (M12): collect-only for an already installed Exynos9820Wlan - wait for the boot-time load's
rem join (t = 30 s rescan + join), then copy both run logs. No reinstall/restart: a device restart while the
rem firmware runs makes the next download fail its TCM read-back (run wifi-m7c, DL 6).
set W=%L%\wifi
mkdir "%W%"
s10ediag mark f: wait 45 s for the join
ping -n 45 127.0.0.1 > nul
pnputil /enum-devices /deviceid "ACPI\SAMS9825" /drivers > "%W%\dev-wlan.txt" 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Wlan.txt" "%W%\Exynos9820Wlan.txt" > nul 2>&1
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Wlan-run.txt" "%W%\run.txt" > nul 2>&1
findstr /i /c:"eapol" /c:"rx cpl" /c:"JOIN" /c:"DATA 0" /c:"[cnt]" /c:"[diag]" /c:"rxbufpost" "%W%\run.txt" "%W%\Exynos9820Wlan.txt" > "%W%\key.txt" 2>nul
for /f "usebackq delims=" %%L in ("%W%\key.txt") do s10ediag mark "f: %%L"
s10ediag mark f: done
