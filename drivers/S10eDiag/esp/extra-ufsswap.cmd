@echo off
rem S10eAgent extra step: swap Exynos9820Ufs.sys inside Windows, restart Windows CLEANLY, measure on the second pass.
rem TWRP cannot write the NTFS volume (the agent's flushreset leaves it unclean, and ntfs-3g refuses rw without
rem the journal reset that corrupted it before), so the swap stays in Windows; the clean restart makes it one cycle.
rem Pass 1 (no C:\S10eTmp\ufsswap.flg): rename the live driver to .prev, copy %ESP%\S10EDIAG\Exynos9820Ufs-new.sys,
rem   write the flag, shutdown /r (the firmware continues into Windows after an OS-requested restart).
rem Pass 2 (flag present): delete the flag, run the read measurement (w34+ UfsDiag counters around 64 KiB loops).
set P=%L%\ufsswap
mkdir "%P%"
set F=C:\Windows\System32\DriverStore\FileRepository\exynos9820ufs.inf_arm64_4efdc4525d11d997\Exynos9820Ufs.sys
if exist C:\S10eTmp\ufsswap.flg goto measure
s10ediag mark s: swap pass
if not exist "%ESP%\S10EDIAG\Exynos9820Ufs-new.sys" (s10ediag mark s: FAIL no new driver on ESP & goto end)
takeown /f "%F%" > "%P%\takeown.txt" 2>&1
icacls "%F%" /grant *S-1-5-18:F > "%P%\icacls.txt" 2>&1
if exist "%F%.prev" del /f /q "%F%.prev"
move /y "%F%" "%F%.prev" > "%P%\move.txt" 2>&1
copy /y "%ESP%\S10EDIAG\Exynos9820Ufs-new.sys" "%F%" > "%P%\copy.txt" 2>&1
certutil -hashfile "%F%" SHA256 > "%P%\hash-after.txt" 2>&1
for /f "usebackq skip=1 delims=" %%L in ("%P%\hash-after.txt") do (s10ediag mark "s: now %%L" & goto marked)
:marked
echo swapped > C:\S10eTmp\ufsswap.flg
s10ediag mark s: clean restart
shutdown /r /t 0 /f
rem Never return to collect.cmd while Windows shuts down: a killed ping let collect.cmd write torecovery.flg
rem before the restart, so the firmware went to TWRP instead of pass 2 (run g15full).
:holdrestart
ping -n 30 127.0.0.1 > nul
goto holdrestart
goto end
:measure
del /f /q C:\S10eTmp\ufsswap.flg
s10ediag mark s: measure pass
copy /y "%ESP%\S10EDIAG\UfsDiag.exe" C:\S10eTmp\UfsDiag.exe > nul
copy /y "%ESP%\S10EDIAG\s10eperf-m19.exe" C:\S10eTmp\s10eperf.exe > nul
certutil -hashfile "%F%" SHA256 > "%P%\hash-live.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c0.txt" 2>&1
C:\S10eTmp\s10eperf.exe loop \\.\PhysicalDrive0 10 > "%P%\loop-disk.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c1.txt" 2>&1
C:\S10eTmp\s10eperf.exe loop \\.\C: 10 > "%P%\loop-vol.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c2.txt" 2>&1
C:\S10eTmp\s10eperf.exe raw \\.\C: > "%P%\raw.txt" 2>&1
for %%F in (c0 c1 c2) do for /f "usebackq delims=" %%L in (`findstr /b "PERF_COUNTER" "%P%\%%F.txt"`) do s10ediag mark "%%F: %%L"
for /f "usebackq delims=" %%L in ("%P%\loop-disk.txt") do s10ediag mark "ld: %%L"
for /f "usebackq delims=" %%L in ("%P%\loop-vol.txt") do s10ediag mark "lv: %%L"
for /f "usebackq delims=" %%L in ("%P%\raw.txt") do s10ediag mark "r: %%L"
:end
s10ediag mark s: done
