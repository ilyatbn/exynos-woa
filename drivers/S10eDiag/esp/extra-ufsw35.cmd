@echo off
rem S10eAgent extra step (m15 w35): swap Exynos9820Ufs.sys for w35 INSIDE Windows (no NTFS writes from TWRP).
rem The loaded image can be renamed but not overwritten: the w34 file is kept next to it as Exynos9820Ufs.sys.w34
rem (rollback = rename back). w35 = w34 + UFS_DMA64_WITH_RESERVED_WORKSPACE (notes/m15-ufs-throughput.md u3).
rem Package expected at %ESP%\S10EDIAG\Exynos9820Ufs-w35.sys. Takes effect on the next boot.
set P=%L%\ufsw35
mkdir "%P%"
set F=C:\Windows\System32\DriverStore\FileRepository\exynos9820ufs.inf_arm64_4efdc4525d11d997\Exynos9820Ufs.sys
reg query HKLM\SYSTEM\CurrentControlSet\Services\Exynos9820Ufs /v ImagePath > "%P%\imagepath.txt" 2>&1
s10ediag mark w: swap ufs w35
if not exist "%F%" (s10ediag mark w: FAIL driver file missing & goto end)
if not exist "%ESP%\S10EDIAG\Exynos9820Ufs-w35.sys" (s10ediag mark w: FAIL no w35 on ESP & goto end)
certutil -hashfile "%F%" SHA256 > "%P%\hash-before.txt" 2>&1
certutil -hashfile "%ESP%\S10EDIAG\Exynos9820Ufs-w35.sys" SHA256 > "%P%\hash-w35-esp.txt" 2>&1
takeown /f "%F%" > "%P%\takeown.txt" 2>&1
icacls "%F%" /grant *S-1-5-18:F > "%P%\icacls.txt" 2>&1
if exist "%F%.w34" del /f /q "%F%.w34"
move /y "%F%" "%F%.w34" > "%P%\move.txt" 2>&1
copy /y "%ESP%\S10EDIAG\Exynos9820Ufs-w35.sys" "%F%" > "%P%\copy.txt" 2>&1
certutil -hashfile "%F%" SHA256 > "%P%\hash-after.txt" 2>&1
for /f "usebackq skip=1 delims=" %%L in ("%P%\hash-after.txt") do (s10ediag mark "w: now %%L" & goto marked)
:marked
dir "%F%*" > "%P%\dir.txt" 2>&1
:end
s10ediag mark w: done
