@echo off
rem S10eAgent extra step (m15 UFS throughput, notes/m15-ufs-research-clocks.md + notes/m15-ufs-research-dma.md):
rem read-only. 1) static reads: IO coherency, CMU_FSYS1 / CMU_TOP clock muxes+dividers for the UFS bus and UniPro
rem clocks, UFSP/FMP config, HCI vendor INVALID_*/VS_IS; 2) size sweep 4..64 KiB (s10eperf sweep); 3) HCI DMA
rem monitors (FSM, PRDT hit ratio, DMA0/1) sampled while s10eperf loop runs 64 KiB reads in the background.
rem Every address is one the research notes cite from Linux (cmucal-sfr.c / ufs-exynos-dbg.c). No PMA reads (gated APB).
set P=%L%\ufsprobe2
mkdir "%P%"
s10ediag mark u2: static reads
rem IO coherency (UFS bits 22|23 on 9820, 0x300 = our 9810 leftover)
for %%A in (0x13C10700) do s10ediag peek %%A >> "%P%\iocoh.txt" 2>&1
rem CMU_FSYS1: MUX_CLKCMU_FSYS1_BUS_USER b4, MUX_CLKCMU_FSYS1_UFS_EMBD_USER b4, PLL_CON0_PLL_MMC, MUX_CLK_FSYS1_BUS b0, DIV_CLK_FSYS1_BUS [3:0]
for %%A in (0x13C00100 0x13C00180 0x13C001A0 0x13C01000 0x13C01800) do s10ediag peek %%A >> "%P%\cmu-fsys1.txt" 2>&1
rem CMU_TOP: MUX_CLKCMU_FSYS1_BUS [1:0], MUX_CLKCMU_FSYS1_UFS_EMBD [1:0], DIV_CLKCMU_FSYS1_UFS_EMBD [2:0], GAT_CLKCMU_FSYS1_BUS
for %%A in (0x1A241068 0x1A241078 0x1A241864 0x1A242000) do s10ediag peek %%A >> "%P%\cmu-top.txt" 2>&1
rem UFSP/FMP (0x10 bits [20:19] = DESCTYPE)
for %%A in (0x13D70000 0x13D70008 0x13D70010) do s10ediag peek %%A >> "%P%\ufsp.txt" 2>&1
rem HCI vendor: INVALID_* 0x110-0x124, INVALID_PRDT_CTRL 0x130, VS_IS 0x138, 0x15C
for %%A in (0x13D61110 0x13D61114 0x13D61118 0x13D6111C 0x13D61120 0x13D61124 0x13D61130 0x13D61138 0x13D6115C) do s10ediag peek %%A >> "%P%\hci-vs.txt" 2>&1
s10ediag mark u2: sweep
s10eperf sweep \\.\C: > "%P%\sweep.txt" 2>&1
for /f "usebackq delims=" %%L in ("%P%\sweep.txt") do s10ediag mark "s: %%L"
s10ediag mark u2: monitors under load
start "" /b cmd /c "s10eperf loop \\.\C: 15 > "%P%\loop.txt" 2>&1"
ping -n 3 127.0.0.1 > nul
for %%N in (1 2 3 4 5 6) do (
  echo sample %%N >> "%P%\dma-mon.txt"
  for %%A in (0x13D611C0 0x13D611C4 0x13D611C8 0x13D611CC 0x13D611D0 0x13D611D4 0x13D611D8 0x13D611DC 0x13D611F8) do s10ediag peek %%A >> "%P%\dma-mon.txt" 2>&1
  ping -n 2 127.0.0.1 > nul
)
ping -n 12 127.0.0.1 > nul
s10ediag mark u2: after load
for %%A in (0x13D60038 0x13D6003C 0x13D60040 0x13D60044 0x13D60048 0x13D61130 0x13D61138) do s10ediag peek %%A >> "%P%\after.txt" 2>&1
for /f "usebackq delims=" %%L in ("%P%\iocoh.txt") do s10ediag mark "c: %%L"
for /f "usebackq delims=" %%L in ("%P%\cmu-fsys1.txt") do s10ediag mark "f: %%L"
for /f "usebackq delims=" %%L in ("%P%\cmu-top.txt") do s10ediag mark "t: %%L"
s10ediag mark u2: done
