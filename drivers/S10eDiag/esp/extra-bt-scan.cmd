@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): D4c rung R5 - Bluetooth radio state and a scan for nearby
rem devices through the already-installed Exynos9820Bt transport (notes/bt-driver.md). Needs \S10EDIAG\bt-scan.ps1.
set B=%L%\btscan
mkdir "%B%"
s10ediag mark t: bt scan start
pnputil /enum-devices /class Bluetooth > "%B%\dev-class-bluetooth.txt" 2>&1
sc query bthserv > "%B%\sc.txt" 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%ESP%\S10EDIAG\bt-scan.ps1" -Seconds 30 > "%B%\scan.txt" 2>&1
s10ediag mark t: bt scan done
s10ediag flush > nul 2>&1
