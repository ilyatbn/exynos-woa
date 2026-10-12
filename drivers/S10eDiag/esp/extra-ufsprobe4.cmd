@echo off
rem S10eAgent extra step (m15 u4, read-only): w35 (Dma64 advertised) still turned 4151 x 64 KiB volume reads into
rem ~34000 ~8 KiB commands. Which layer splits? 1) the adapter limits classpnp sees (StorageAdapterProperty) on the
rem disk and the volume; 2) w34/w35 counters around a 10 s 64 KiB loop on \\.\PhysicalDrive0 (below volume/partition
rem filters) and then on \\.\C:. s10eperf (m19 build) and UfsDiag.exe run from the ESP copies.
set P=%L%\ufsprobe4
mkdir "%P%"
copy /y "%ESP%\S10EDIAG\UfsDiag.exe" C:\S10eTmp\UfsDiag.exe > nul
copy /y "%ESP%\S10EDIAG\s10eperf-m19.exe" C:\S10eTmp\s10eperf.exe > nul
s10ediag mark u4: adapter
C:\S10eTmp\s10eperf.exe adapter \\.\PhysicalDrive0 adapter \\.\C: > "%P%\adapter.txt" 2>&1
for /f "usebackq delims=" %%L in ("%P%\adapter.txt") do s10ediag mark "d: %%L"
C:\S10eTmp\UfsDiag.exe > "%P%\c0.txt" 2>&1
C:\S10eTmp\s10eperf.exe loop \\.\PhysicalDrive0 10 > "%P%\loop-disk.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c1.txt" 2>&1
C:\S10eTmp\s10eperf.exe loop \\.\C: 10 > "%P%\loop-vol.txt" 2>&1
C:\S10eTmp\UfsDiag.exe > "%P%\c2.txt" 2>&1
for %%F in (c0 c1 c2) do for /f "usebackq delims=" %%L in (`findstr /b "PERF_COUNTER" "%P%\%%F.txt"`) do s10ediag mark "%%F: %%L"
for /f "usebackq delims=" %%L in ("%P%\loop-disk.txt") do s10ediag mark "ld: %%L"
for /f "usebackq delims=" %%L in ("%P%\loop-vol.txt") do s10ediag mark "lv: %%L"
s10ediag mark u4: done
