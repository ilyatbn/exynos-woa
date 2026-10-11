@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): install the S10eBattery miniclass inside Windows and record
rem what PnP and the power stack made of it. notes/battery-driver-m1.md ("device test procedure").
rem Called from collect.cmd with ESP / OUT / L set. Needs on the ESP: \S10EDIAG\drivers\S10eBattery\{S10eBattery.sys,
rem S10eBattery.inf,s10ebattery.cat} and \S10EDIAG\drivers\wdktestcert.cer, and firmware with DSDT \_SB.BAT0 (SAMS9826).
rem Fresh temp dir + copy to NTFS first: same reasons as extra-touch.cmd (run 6: corrupt C:\Windows\TEMP index,
rem pnputil straight from the ESP failed).
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set D=%TMP%\s10e-drivers
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers" "%D%" > "%L%\battery-xcopy.txt" 2>&1
set B=%L%\battery
mkdir "%B%"

echo --- HSI2C45 bus state before (single words: USI_CMGP3 mode, gpm12/gpm13 CON, HSI2C45 CTL)
s10ediag mark b: peek before
s10ediag peek 0x15C22030 > "%B%\usi-cmgp3-mode-before.txt" 2>&1
s10ediag peek 0x15C30180 > "%B%\gpm12-con-before.txt" 2>&1
s10ediag peek 0x15C301A0 > "%B%\gpm13-con-before.txt" 2>&1
s10ediag peek 0x15D60000 > "%B%\hsi2c45-ctl-before.txt" 2>&1

echo --- safety: no critical/low battery action while the driver is unproven (a bogus 0 %% must not hibernate)
s10ediag mark b: powercfg actions
powercfg /q SCHEME_CURRENT SUB_BATTERY > "%B%\powercfg-battery-before.txt" 2>&1
powercfg /setdcvalueindex SCHEME_CURRENT SUB_BATTERY BATACTIONCRIT 0 >> "%B%\powercfg-set.txt" 2>&1
powercfg /setacvalueindex SCHEME_CURRENT SUB_BATTERY BATACTIONCRIT 0 >> "%B%\powercfg-set.txt" 2>&1
powercfg /setdcvalueindex SCHEME_CURRENT SUB_BATTERY BATACTIONLOW 0 >> "%B%\powercfg-set.txt" 2>&1
powercfg /setacvalueindex SCHEME_CURRENT SUB_BATTERY BATACTIONLOW 0 >> "%B%\powercfg-set.txt" 2>&1
powercfg /setactive SCHEME_CURRENT >> "%B%\powercfg-set.txt" 2>&1

echo --- trust the test certificate, install the package
for /r "%D%\S10eBattery" %%F in (*) do certutil -hashfile "%%F" SHA256 >> "%B%\hashes.txt" 2>&1
s10ediag mark b: certutil
certutil -addstore -f Root "%D%\wdktestcert.cer" > "%B%\cert.txt" 2>&1
certutil -addstore -f TrustedPublisher "%D%\wdktestcert.cer" >> "%B%\cert.txt" 2>&1
s10ediag mark b: pnputil add battery
pnputil /add-driver "%D%\S10eBattery\S10eBattery.inf" /install > "%B%\pnputil-battery.txt" 2>&1
rem three 20 s polls
s10ediag mark b: settle 70 s
ping -n 70 127.0.0.1 > nul

echo --- what PnP and the power stack made of it
s10ediag mark b: enum
pnputil /enum-devices /deviceid "ACPI\SAMS9826" /properties > "%B%\dev-battery.txt" 2>&1
pnputil /enum-devices /class Battery /drivers > "%B%\dev-class-battery.txt" 2>&1
sc query S10eBattery > "%B%\sc.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters" > "%B%\battery-params.txt" 2>&1
s10ediag mark b: wmi
powershell -NoProfile -Command "Get-CimInstance Win32_Battery | Format-List *" > "%B%\win32-battery.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus | Format-List *" > "%B%\wmi-batterystatus.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance -Namespace root\wmi -ClassName BatteryStaticData | Format-List *" > "%B%\wmi-batterystatic.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance -Namespace root\wmi -ClassName BatteryTemperature | Format-List *" > "%B%\wmi-batterytemp.txt" 2>&1
s10ediag mark b: batteryreport
powercfg /batteryreport /output "%B%\battery-report.html" > "%B%\batteryreport.txt" 2>&1
powercfg /batteryreport /xml /output "%B%\battery-report.xml" >> "%B%\batteryreport.txt" 2>&1
powercfg /q SCHEME_CURRENT SUB_BATTERY > "%B%\powercfg-battery-after.txt" 2>&1

echo --- second sample 60 s later (rate / percentage movement)
s10ediag mark b: second sample
ping -n 60 127.0.0.1 > nul
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters" > "%B%\battery-params-2.txt" 2>&1
powershell -NoProfile -Command "Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus | Format-List *" > "%B%\wmi-batterystatus-2.txt" 2>&1

echo --- HSI2C45 bus state after (driver in D0: mode 4, pins function 2 expected)
s10ediag mark b: peek after
s10ediag peek 0x15C22030 > "%B%\usi-cmgp3-mode-after.txt" 2>&1
s10ediag peek 0x15C30180 > "%B%\gpm12-con-after.txt" 2>&1
s10ediag peek 0x15C301A0 > "%B%\gpm13-con-after.txt" 2>&1
s10ediag peek 0x15D60000 > "%B%\hsi2c45-ctl-after.txt" 2>&1
s10ediag mark b: export logs
wevtutil epl System "%B%\System.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%B%\KernelPnP.evtx"
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%B%\setupapi.dev.log" > nul 2>&1
s10ediag mark b: done
