@echo off
rem S10eAgent extra step (m15 UFS throughput, notes/m15-ufs-throughput.md read A/B): is the UFS link silently
rem retrying? UECPA 0x38 / UECDL 0x3C (UFSHCI error-code registers, clear-on-read, the Windows driver never
rem reads them): read once to clear, run 64 KiB raw reads, read again. Plus the B reads (single words that Linux
rem itself reads/writes). All addresses from notes/m15-ufs-throughput.md (Linux ufs-exynos 9820 citations).
set P=%L%\ufsprobe
mkdir "%P%"
s10ediag mark u: clear UECPA/UECDL/UECN/UECT/UECDME
s10ediag peek 0x13D60038 > "%P%\uec-before.txt" 2>&1
s10ediag peek 0x13D6003C >> "%P%\uec-before.txt" 2>&1
s10ediag peek 0x13D60040 >> "%P%\uec-before.txt" 2>&1
s10ediag peek 0x13D60044 >> "%P%\uec-before.txt" 2>&1
s10ediag peek 0x13D60048 >> "%P%\uec-before.txt" 2>&1
s10ediag peek 0x13D60020 >> "%P%\uec-before.txt" 2>&1
s10ediag mark u: raw 64K reads
s10eperf raw \\.\C: > "%P%\raw.txt" 2>&1
s10ediag mark u: UEC after
s10ediag peek 0x13D60038 > "%P%\uec-after.txt" 2>&1
s10ediag peek 0x13D6003C >> "%P%\uec-after.txt" 2>&1
s10ediag peek 0x13D60040 >> "%P%\uec-after.txt" 2>&1
s10ediag peek 0x13D60044 >> "%P%\uec-after.txt" 2>&1
s10ediag peek 0x13D60048 >> "%P%\uec-after.txt" 2>&1
s10ediag peek 0x13D60020 >> "%P%\uec-after.txt" 2>&1
s10ediag mark u: unipro status
s10ediag peek 0x13D57B00 > "%P%\unipro.txt" 2>&1
s10ediag peek 0x13D57B04 >> "%P%\unipro.txt" 2>&1
s10ediag peek 0x13D57B20 >> "%P%\unipro.txt" 2>&1
s10ediag peek 0x13D57C00 >> "%P%\unipro.txt" 2>&1
s10ediag peek 0x13D50044 >> "%P%\unipro.txt" 2>&1
s10ediag peek 0x13D6110C >> "%P%\unipro.txt" 2>&1
s10ediag peek 0x13C10700 >> "%P%\unipro.txt" 2>&1
s10ediag mark u: done
