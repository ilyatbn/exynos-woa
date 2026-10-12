@echo off
rem S10eAgent extra step (M12 M8): put tools/wifi-connect (SSID + passphrase -> Exynos9820Wlan Parameters) on the
rem public desktop as "WiFi connect.exe" for the user. Credentials are typed on the phone only; nothing is stored in
rem the repo or on the ESP. Use with \S10EDIAG\stay.flg (interactive session). Also collects the WLAN run logs.
set W=%L%\wifi
mkdir "%W%"
copy /y "%ESP%\S10EDIAG\wifi-connect.exe" "C:\Users\Public\Desktop\WiFi connect.exe" > "%W%\app-copy.txt" 2>&1
s10ediag mark f: wifi-connect on the desktop
ping -n 45 127.0.0.1 > nul
copy /y "%SystemRoot%\System32\LogFiles\Exynos9820Wlan-run.txt" "%W%\run.txt" > nul 2>&1
s10ediag mark f: done
