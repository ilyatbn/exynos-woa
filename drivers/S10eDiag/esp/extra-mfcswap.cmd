@echo off
rem S10eAgent extra step: swap Exynos9820Mfc.sys inside Windows WITHOUT a PnP remove, restart cleanly, record pass 2.
rem Why not pnputil: a driver update removes the device, and the V-W1 driver's ReleaseHardware reset the running MFC
rem -> SError, bugcheck 0x122 (vidc2, notes/d10-mfc-decode-flow.md). A loaded .sys can be renamed, not overwritten;
rem the next boot loads the new file. Same pattern as extra-ufsswap.cmd.
rem Needs on the ESP: \S10EDIAG\drivers\Exynos9820Mfc\{Exynos9820Mfc.sys,mfc_test.h264}.
rem Pass 1 (no C:\S10eTmp\mfcswap.flg): rename the live driver to .prev, copy the new one + mfc_test.h264, flag,
rem   shutdown /r (the firmware continues into Windows after an OS-requested restart).
rem Pass 2 (flag present): delete the flag, record the device state; the driver's self-test log goes to KD.
set P=%L%\mfcswap
mkdir "%P%"
set N=%ESP%\S10EDIAG\drivers\Exynos9820Mfc
set F=%SystemRoot%\System32\drivers\Exynos9820Mfc.sys
if exist C:\S10eTmp\mfcswap.flg goto pass2
s10ediag mark m: swap pass
if not exist "%N%\Exynos9820Mfc.sys" (s10ediag mark m: FAIL no new driver on ESP & goto end)
takeown /f "%F%" > "%P%\takeown.txt" 2>&1
icacls "%F%" /grant *S-1-5-18:F > "%P%\icacls.txt" 2>&1
if exist "%F%.prev" del /f /q "%F%.prev"
move /y "%F%" "%F%.prev" > "%P%\move.txt" 2>&1
copy /y "%N%\Exynos9820Mfc.sys" "%F%" > "%P%\copy.txt" 2>&1
copy /y "%N%\mfc_test.h264" "%SystemRoot%\System32\drivers\mfc_test.h264" >> "%P%\copy.txt" 2>&1
certutil -hashfile "%F%" SHA256 > "%P%\hash-after.txt" 2>&1
for /f "usebackq skip=1 delims=" %%L in ("%P%\hash-after.txt") do (s10ediag mark "m: now %%L" & goto marked)
:marked
if not exist C:\S10eTmp mkdir C:\S10eTmp
echo swapped > C:\S10eTmp\mfcswap.flg
s10ediag mark m: clean restart
shutdown /r /t 0 /f
rem never fall back into collect.cmd while Windows shuts down (it would write torecovery.flg - run g15full)
:holdrestart
ping -n 30 127.0.0.1 > nul
goto holdrestart
:pass2
del /f /q C:\S10eTmp\mfcswap.flg
s10ediag mark m: pass 2
certutil -hashfile "%F%" SHA256 > "%P%\hash-live.txt" 2>&1
pnputil /enum-devices /deviceid "ACPI\SAMS982A" /properties > "%P%\dev-vidc.txt" 2>&1
sc query Exynos9820Mfc > "%P%\sc.txt" 2>&1
:end
s10ediag mark m: done
