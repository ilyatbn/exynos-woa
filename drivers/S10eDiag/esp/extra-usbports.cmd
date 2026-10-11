@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): M16 dock test - read the xHCI port registers while Windows
rem runs, to split "port never sees the device" (CCS 0) from "Windows never hears about it" (CCS/CSC set, no
rem interrupt). xHCI at 0x10C00000: CAPLENGTH 0x20 -> operational base 0x10C00020: USBCMD +0, USBSTS +4
rem (bit3 EINT, bit4 PCD). PORTSC(n) = 0x10C00420 + 0x10*n: bit0 CCS connected, bit1 PED, bit9 PP power,
rem bits 8:5 PLS, 13:10 speed, bit17 CSC change. Runtime IMAN(0) = 0x10C00440+0x20 = 0x10C00460 (bit0 IP pending, bit1 IE).
rem Sampled 6 times 10 s apart so the user can plug/unplug the dock in between. notes/m16-usb.md.
set T=%L%\usbports
mkdir "%T%" > nul 2>&1
for /l %%i in (1,1,6) do (
  echo --- sample %%i %time% >> "%T%\ports.txt"
  s10ediag peek 0x10C00020 >> "%T%\ports.txt" 2>&1
  s10ediag peek 0x10C00024 >> "%T%\ports.txt" 2>&1
  s10ediag peek 0x10C00420 >> "%T%\ports.txt" 2>&1
  s10ediag peek 0x10C00430 >> "%T%\ports.txt" 2>&1
  s10ediag peek 0x10C00460 >> "%T%\ports.txt" 2>&1
  ping -n 11 127.0.0.1 > nul
)
pnputil /enum-devices /connected /class USB > "%T%\usb-devices.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\PNP0D10" /properties > "%T%\xhc0.txt" 2>&1
wevtutil qe System /q:"*[System[Provider[@Name='USBXHCI' or @Name='USBHUB3' or @Name='Microsoft-Windows-Kernel-PnP']]]" /c:40 /rd:true /f:text > "%T%\usb-events.txt" 2>&1
