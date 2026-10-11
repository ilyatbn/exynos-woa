@echo off
rem S10eAgent extra step: find which PERIC0 read aborts (run 4: the first peek of the USI sysreg page
rem 0x10421000 -> bugcheck 0x124 SEA). One word per peek, a mark before each, safest first; every value also
rem lands in the PRAM mark ring. Offsets: cal-if exynos9820 cmucal-sfr.c (CMU_PERIC0 0x10400000). notes/m12-touch.md.
s10ediag mark p: CMU PERIC0 CMU_PCLK gate 0x2004
s10ediag peek 0x10402004
s10ediag mark p: D_TZPC_PERIC0 gate 0x2014
s10ediag peek 0x10402014
s10ediag mark p: GPIO_PERIC0 gate 0x2018
s10ediag peek 0x10402018
s10ediag mark p: LHM_AXI_P_PERIC0 gate 0x201c
s10ediag peek 0x1040201c
s10ediag mark p: RSTNSYNC USI03_USI 0x2038
s10ediag peek 0x10402038
s10ediag mark p: SYSREG_PERIC0 gate 0x2058
s10ediag peek 0x10402058
s10ediag mark p: USI03_USI IPCLK/PCLK 0x209c 0x20a0
s10ediag peek 0x1040209c 8
s10ediag mark p: XIU_P_PERIC0 gate 0x2104
s10ediag peek 0x10402104
s10ediag mark p: QCH TZPC GPIO LHM CMU 0x3004..0x3010
s10ediag peek 0x10403004 16
s10ediag mark p: QCH SYSREG 0x3018
s10ediag peek 0x10403018
s10ediag mark p: QCH USI03_USI 0x303c
s10ediag peek 0x1040303c
s10ediag mark p: GPIO PERIC0 gpp1 con 0x10430020
s10ediag peek 0x10430020
s10ediag mark p: USI03 mode 0x1042101C (aborted in run 4)
s10ediag peek 0x1042101C
s10ediag mark p: HSI2C 0x104B0000
s10ediag peek 0x104B0000
s10ediag mark p: all reads survived
