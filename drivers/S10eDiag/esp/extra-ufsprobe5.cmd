@echo off
rem S10eAgent extra step (m15 u5, read-only): INQUIRY VPD 0x00 and 0xB0 (Block Limits) from the UFS device through
rem IOCTL_SCSI_PASS_THROUGH_DIRECT - does the device report a small MAXIMUM/OPTIMAL TRANSFER LENGTH that classpnp
rem splits 64 KiB reads by (u3/u4: ~8 KiB commands on both \.\C: and \.\PhysicalDrive0)?
set P=%L%\ufsprobe5
mkdir "%P%"
copy /y "%ESP%\S10EDIAG\s10eperf-m19.exe" C:\S10eTmp\s10eperf.exe > nul
s10ediag mark u5: vpd
C:\S10eTmp\s10eperf.exe vpd \.\PhysicalDrive0 0 vpd \.\PhysicalDrive0 B0 vpd \.\PhysicalDrive0 B1 > "%P%\vpd.txt" 2>&1
for /f "usebackq delims=" %%L in ("%P%\vpd.txt") do s10ediag mark "v: %%L"
s10ediag mark u5: done
