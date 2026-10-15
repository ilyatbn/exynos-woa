@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): install Exynos9820Bt (D4c Bluetooth transport) and record
rem what PnP and the Bluetooth stack made of it. notes/bt-driver.md (W4). Called from collect.cmd with ESP / OUT / L set.
rem Needs on the ESP: \S10EDIAG\drivers\Exynos9820Bt\{Exynos9820Bt.sys,Exynos9820Bt.inf,exynos9820bt.cat,
rem bcm4375B1_semco.hcd} + \S10EDIAG\drivers\wdktestcert.cer, and firmware with the BT role latch (DSDT BTH0 present).
rem Fresh temp dir + copy to NTFS first: same reasons as extra-battery.cmd / extra-touch.cmd.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set D=%TMP%\s10e-drivers
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers" "%D%" > "%L%\bt-xcopy.txt" 2>&1
set B=%L%\bt
mkdir "%B%"

echo --- BT UART state before (UCON, UFCON, UMSTAT of uart@10840000)
s10ediag mark t: peek before
s10ediag peek 0x10840004 > "%B%\ucon-before.txt" 2>&1
s10ediag peek 0x10840008 > "%B%\ufcon-before.txt" 2>&1
s10ediag peek 0x1084001C > "%B%\umstat-before.txt" 2>&1

echo --- trust the test certificate, install the package
for /r "%D%\Exynos9820Bt" %%F in (*) do certutil -hashfile "%%F" SHA256 >> "%B%\hashes.txt" 2>&1
certutil -addstore -f Root "%D%\wdktestcert.cer" > "%B%\cert.txt" 2>&1
certutil -addstore -f TrustedPublisher "%D%\wdktestcert.cer" >> "%B%\cert.txt" 2>&1
s10ediag mark t: pnputil add bt
pnputil /add-driver "%D%\Exynos9820Bt\Exynos9820Bt.inf" /install > "%B%\pnputil-bt.txt" 2>&1
rem patchram takes ~20 s at 115200 inside PrepareHardware, then the BT stack enumerates the radio
s10ediag mark t: settle 75 s
ping -n 75 127.0.0.1 > nul

echo --- what PnP and the Bluetooth stack made of it
s10ediag mark t: enum
pnputil /enum-devices /deviceid "ACPI\SAMS9829" /properties > "%B%\dev-bth0.txt" 2>&1
pnputil /enum-devices /deviceid "SerialBusWdk\UART_H4" /properties > "%B%\dev-uart-h4.txt" 2>&1
pnputil /enum-devices /class Bluetooth /drivers > "%B%\dev-class-bluetooth.txt" 2>&1
sc query Exynos9820Bt > "%B%\sc.txt" 2>&1
sc query BTHPORT >> "%B%\sc.txt" 2>&1
sc query bthserv >> "%B%\sc.txt" 2>&1
powershell -NoProfile -Command "Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | Format-List *" > "%B%\pnp-bluetooth.txt" 2>&1
powershell -NoProfile -Command "[Windows.Devices.Radios.Radio,Windows.System.Devices,ContentType=WindowsRuntime] | Out-Null; $r=[Windows.Devices.Radios.Radio]::GetRadiosAsync(); while($r.Status -eq 0){Start-Sleep -m 100}; $r.GetResults() | Format-List Name,Kind,State" > "%B%\radios.txt" 2>&1

echo --- BT UART state after (driver in interrupt mode: UCON 0x3C5, UFCON 0x111 expected)
s10ediag mark t: peek after
s10ediag peek 0x10840004 > "%B%\ucon-after.txt" 2>&1
s10ediag peek 0x10840008 > "%B%\ufcon-after.txt" 2>&1
s10ediag peek 0x1084001C > "%B%\umstat-after.txt" 2>&1
s10ediag mark t: export logs
wevtutil epl System "%B%\System.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%B%\KernelPnP.evtx"
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%B%\setupapi.dev.log" > nul 2>&1
s10ediag mark t: done
