@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): install Exynos9820Mfc (D10 video codec, V-W1) and record
rem what PnP made of it (notes/d10-mfc-main.md). Called from collect.cmd with ESP / OUT / L set. Needs on the ESP:
rem \S10EDIAG\drivers\Exynos9820Mfc\{Exynos9820Mfc.sys,Exynos9820Mfc.inf,exynos9820mfc.cat,mfc_fw.bin} +
rem \S10EDIAG\drivers\wdktestcert.cer, and firmware with DSDT VIDC (SAMS982A). The driver's own log goes to KD.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set D=%TMP%\s10e-drivers
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers" "%D%" > "%L%\mfc-xcopy.txt" 2>&1
set B=%L%\mfc
mkdir "%B%"
for /r "%D%\Exynos9820Mfc" %%F in (*) do certutil -hashfile "%%F" SHA256 >> "%B%\hashes.txt" 2>&1
certutil -addstore -f Root "%D%\wdktestcert.cer" > "%B%\cert.txt" 2>&1
certutil -addstore -f TrustedPublisher "%D%\wdktestcert.cer" >> "%B%\cert.txt" 2>&1
s10ediag mark v: pnputil add mfc
pnputil /add-driver "%D%\Exynos9820Mfc\Exynos9820Mfc.inf" /install > "%B%\pnputil-mfc.txt" 2>&1
s10ediag mark v: settle 30 s
ping -n 30 127.0.0.1 > nul
s10ediag mark v: enum
pnputil /enum-devices /deviceid "ACPI\SAMS982A" /properties > "%B%\dev-vidc.txt" 2>&1
sc query Exynos9820Mfc > "%B%\sc.txt" 2>&1
wevtutil epl System "%B%\System.evtx"
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%B%\setupapi.dev.log" > nul 2>&1
s10ediag mark v: done
