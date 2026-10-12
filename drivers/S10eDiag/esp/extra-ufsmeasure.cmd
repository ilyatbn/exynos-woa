@echo off
rem S10eAgent extra step (m15, read-only): the measure pass of extra-ufsswap.cmd on its own - w34+ UfsDiag counters
rem around 10 s 64 KiB loops on \\.\PhysicalDrive0 and \\.\C:, then s10eperf raw. For A/B of firmware-side changes.
set P=%L%\ufsmeasure
mkdir "%P%"
copy /y "%ESP%\S10EDIAG\UfsDiag.exe" C:\S10eTmp\UfsDiag.exe > nul
copy /y "%ESP%\S10EDIAG\s10eperf-m19.exe" C:\S10eTmp\s10eperf.exe > nul
s10ediag mark m: measure
C:\S10eTmp\UfsDiag.exe > "%P%\c0.txt" 2>&1
C:\S10eTmp\s10eperf.exe loop \\.\PhysicalDrive0 10 > "%P%\loop-disk.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c1.txt" 2>&1
C:\S10eTmp\s10eperf.exe loop \\.\C: 10 > "%P%\loop-vol.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c2.txt" 2>&1
C:\S10eTmp\s10eperf.exe raw \\.\C: > "%P%\raw.txt" 2>&1
for /f "usebackq delims=" %%L in ("%P%\loop-disk.txt") do s10ediag mark "ld: %%L"
for /f "usebackq delims=" %%L in ("%P%\loop-vol.txt") do s10ediag mark "lv: %%L"
for /f "usebackq delims=" %%L in ("%P%\raw.txt") do s10ediag mark "r: %%L"
s10ediag mark m: done
