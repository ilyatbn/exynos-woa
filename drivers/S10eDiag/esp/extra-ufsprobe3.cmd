@echo off
rem S10eAgent extra step (m15 u3, read-only): where do Windows' ~4 ms per 64 KiB read go? UEFI reads 64 KiB in 0.73 ms
rem on the same controller and memory (boot #110 UfsSpeed lines, any buffer region). Exynos9820Ufs w34 perf counters
rem (UfsDiag.exe from the same w34 source build) before and after a 15 s s10eperf loop of 64 KiB reads: the deltas give
rem doorbell-to-completion DEVICE time and copy-back per 64 KiB command vs the wall time per request.
set P=%L%\ufsprobe3
mkdir "%P%"
copy /y "%ESP%\S10EDIAG\UfsDiag.exe" C:\S10eTmp\UfsDiag.exe > nul
s10ediag mark u3: ufsdiag before
C:\S10eTmp\UfsDiag.exe > "%P%\before.txt" 2>&1
s10eperf loop \\.\C: 15 > "%P%\loop.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\after.txt" 2>&1
s10ediag mark u3: ufsdiag after
for /f "usebackq delims=" %%L in (`findstr /b "PERF" "%P%\before.txt"`) do s10ediag mark "b: %%L"
for /f "usebackq delims=" %%L in (`findstr /b "PERF" "%P%\after.txt"`) do s10ediag mark "a: %%L"
for /f "usebackq delims=" %%L in ("%P%\loop.txt") do s10ediag mark "l: %%L"
s10ediag mark u3: done
