@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd, with the ARM64 kdnet.exe + VerifiedNICList.xml +
rem kdnet-dev-ext.dll in \S10EDIAG\kd\): turn on Windows kernel debugging over USB (KDNET over USB EEM,
rem inbox kd_8003_5143.dll on the DWC3 in device role - firmware boot #72+, ACPI DBG2 entry 0 -> \_SB.UFN0).
rem notes/m17-usb-kd.md T4. Needs no host to be attached; the debugger connects on the NEXT boot.
rem Host: USB 3 Type-A port + C-to-A cable, firewall off, windbg -k net:port=50005,key=1.2.3.4
rem Undo: bcdedit /debug off (or delete this extra.cmd and run extra-kd-off.cmd).
set T=%L%\kd
mkdir "%T%" > nul 2>&1
set K=C:\S10eTmp\kd
if not exist C:\S10eTmp mkdir C:\S10eTmp
xcopy /e /i /y "%ESP%\S10EDIAG\kd" "%K%" > "%T%\xcopy.txt" 2>&1

s10ediag mark k: kdnet list
rem What Windows itself enumerates as KDNET-capable (expects "Device-mode USB controller with Vendor ID: 5143").
"%K%\kdnet.exe" > "%T%\kdnet-list.txt" 2>&1

s10ediag mark k: dbgsettings
rem DBG2 has ONE entry, so its index is 0 (Microsoft: busparams = the DBG2 array index on ARM).
bcdedit /dbgsettings net hostip:169.254.255.255 port:50005 key:1.2.3.4 > "%T%\bcd.txt" 2>&1
bcdedit /set {dbgsettings} busparams 0 >> "%T%\bcd.txt" 2>&1
bcdedit /set {dbgsettings} dhcp No >> "%T%\bcd.txt" 2>&1
bcdedit /debug on >> "%T%\bcd.txt" 2>&1
bcdedit /dbgsettings >> "%T%\bcd.txt" 2>&1
bcdedit /enum {current} >> "%T%\bcd.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SAMS9821" /properties > "%T%\ufn0.txt" 2>&1
s10ediag mark k: done
