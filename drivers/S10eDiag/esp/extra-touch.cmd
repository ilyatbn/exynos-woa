@echo off
rem S10eAgent extra step (copy to ESP \S10EDIAG\extra.cmd): install the touch stack inside Windows and record
rem what PnP did with it. notes/m12-touch.md. Called from collect.cmd with ESP / OUT / L set.
rem Register reads are SINGLE WORDS at cal-if / DT addresses only: a page read of 0x10421000 aborted (run 4),
rem the same words read one by one did not (run 5).
rem Run 6: pnputil straight from the ESP failed with "The file or directory is corrupted and unreadable" for both
rem packages. Copy them to NTFS first and record hashes of what Windows actually read.
rem Run 6 setupapi.dev.log: the real error is 0x570 creating C:\Windows\TEMP\{guid} - that directory index
rem is corrupt (Ntfs 55 events after the forced resets). Use a fresh temp dir for everything here.
set TMP=C:\S10eTmp
set TEMP=C:\S10eTmp
if not exist "%TMP%" mkdir "%TMP%"
set D=%TMP%\s10e-drivers
if exist "%D%" rd /s /q "%D%"
xcopy /e /i /y "%ESP%\S10EDIAG\drivers" "%D%" > "%L%\touch-xcopy.txt" 2>&1
set T=%L%\touch
mkdir "%T%"

echo --- registers before
s10ediag mark t: peek before
s10ediag peek 0x1042101C > "%T%\usi03-mode-before.txt" 2>&1
s10ediag peek 0x10430020 > "%T%\gpp1-con-before.txt" 2>&1

echo --- trust the test certificate, install both packages
for /r "%D%" %%F in (*) do certutil -hashfile "%%F" SHA256 >> "%T%\hashes.txt" 2>&1
s10ediag mark t: certutil
certutil -addstore -f Root "%D%\wdktestcert.cer" > "%T%\cert.txt" 2>&1
certutil -addstore -f TrustedPublisher "%D%\wdktestcert.cer" >> "%T%\cert.txt" 2>&1
s10ediag mark t: pnputil add hsi2c
pnputil /add-driver "%D%\Exynos9820Hsi2c\Exynos9820Hsi2c.inf" /install > "%T%\pnputil-hsi2c.txt" 2>&1
s10ediag mark t: pnputil add touch
pnputil /add-driver "%D%\S6SY761TouchBeyond0\S6SY761TouchBeyond0.inf" /install > "%T%\pnputil-touch.txt" 2>&1
s10ediag mark t: settle 60 s
ping -n 60 127.0.0.1 > nul

echo --- what PnP made of it
s10ediag mark t: enum
pnputil /enum-devices /deviceid "ACPI\SAMS9820" /properties > "%T%\dev-hsi2c.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SCSY0761" /properties > "%T%\dev-touch.txt" 2>&1
pnputil /enum-devices /class HIDClass > "%T%\dev-hid.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SCSY0761\0\Device Parameters" > "%T%\touch-params.txt" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9820\a\Device Parameters" > "%T%\hsi2c-params.txt" 2>&1
sc query Exynos9820Hsi2c > "%T%\sc.txt" 2>&1
sc query S6SY761TouchBeyond0 >> "%T%\sc.txt" 2>&1
sc query mshidkmdf >> "%T%\sc.txt" 2>&1
sc query SpbCx >> "%T%\sc.txt" 2>&1

echo --- registers after
s10ediag mark t: peek after
s10ediag peek 0x1042101C > "%T%\usi03-mode-after.txt" 2>&1
s10ediag peek 0x10430020 > "%T%\gpp1-con-after.txt" 2>&1
s10ediag peek 0x1040209c 8 > "%T%\usi03-gates-after.txt" 2>&1
s10ediag peek 0x1040303c > "%T%\usi03-qch-after.txt" 2>&1
s10ediag peek 0x104B0000 > "%T%\hsi2c-ctl-after.txt" 2>&1
s10ediag mark t: export logs
wevtutil epl System "%T%\System.evtx"
wevtutil epl Microsoft-Windows-Kernel-PnP/Configuration "%T%\KernelPnP.evtx"
copy /y "%SystemRoot%\INF\setupapi.dev.log" "%T%\setupapi.dev.log" > nul 2>&1
s10ediag mark t: chkdsk spotfix scheduled
echo y| chkdsk C: /spotfix > "%T%\chkdsk.txt" 2>&1
s10ediag mark t: done
