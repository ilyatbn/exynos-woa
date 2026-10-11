@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): install the touch stack inside Windows and record
rem what PnP did with it. notes/m12-touch.md. Called from collect.cmd with ESP / OUT / L set.
set D=%ESP%\S10EDIAG\drivers
set T=%L%\touch
mkdir "%T%"

echo --- registers before (sysreg USI slots, CMU_PERIC0 gates, PERIC0 gpp1 / ALIVE gpa1)
s10ediag peek 0x10421000 0x54 > "%T%\sysreg-usi-before.txt" 2>&1
s10ediag peek 0x10402000 0x100 > "%T%\cmu-peric0-gates-before.txt" 2>&1
s10ediag peek 0x10403000 0x40 > "%T%\cmu-peric0-qch-before.txt" 2>&1
s10ediag peek 0x10430020 0x10 > "%T%\gpp1-before.txt" 2>&1

echo --- trust the test certificate, install both packages
certutil -addstore -f Root "%D%\wdktestcert.cer" > "%T%\cert.txt" 2>&1
certutil -addstore -f TrustedPublisher "%D%\wdktestcert.cer" >> "%T%\cert.txt" 2>&1
pnputil /add-driver "%D%\Exynos9820Hsi2c\Exynos9820Hsi2c.inf" /install > "%T%\pnputil-hsi2c.txt" 2>&1
pnputil /add-driver "%D%\S6SY761TouchBeyond0\S6SY761TouchBeyond0.inf" /install > "%T%\pnputil-touch.txt" 2>&1
ping -n 60 127.0.0.1 > nul

echo --- what PnP made of it
pnputil /enum-devices /deviceid "ACPI\SAMS9820" /properties > "%T%\dev-hsi2c.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SCSY0761" /properties > "%T%\dev-touch.txt" 2>&1
pnputil /enum-devices /class HIDClass > "%T%\dev-hid.txt" 2>&1
sc query Exynos9820Hsi2c > "%T%\sc.txt" 2>&1
sc query S6SY761TouchBeyond0 >> "%T%\sc.txt" 2>&1
sc query mshidkmdf >> "%T%\sc.txt" 2>&1
sc query SpbCx >> "%T%\sc.txt" 2>&1

echo --- registers after
s10ediag peek 0x10421000 0x54 > "%T%\sysreg-usi-after.txt" 2>&1
s10ediag peek 0x10402000 0x100 > "%T%\cmu-peric0-gates-after.txt" 2>&1
s10ediag peek 0x10430020 0x10 > "%T%\gpp1-after.txt" 2>&1
wevtutil epl System "%T%\System.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%T%\KernelPnP.evtx"
