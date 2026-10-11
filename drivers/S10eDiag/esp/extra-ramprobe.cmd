@echo off
rem S10eAgent extra step: may EL1 (Windows, under Samsung's EL2) touch the 64-bit DRAM bank?
rem TWRP /proc/iomem: System RAM 0x880000000-0x97FFFFFFF (Exynos9820.h BANK3 0x880000000 + BANK4). UEFI boot #16
rem reset when DXE used the high banks (notes/m8-uefi-image.md B62/B63) - this decides whether that was the banks
rem themselves. One word per step, a mark before each: the first abort names the address. Reads first (safest,
rem away from sec_rdx_bootdev 0x880000000+0xAD00000), then write+readback+restore of scratch words.
rem notes/m13-ram.md.
s10ediag mark r: read 0x8C0000000
s10ediag peek 0x8C0000000
s10ediag mark r: read 0x900000000
s10ediag peek 0x900000000
s10ediag mark r: read 0x940000000
s10ediag peek 0x940000000
s10ediag mark r: read 0x97FFFF000
s10ediag peek 0x97FFFF000
s10ediag mark r: read 0x88AD00000 (first page after sec_rdx_bootdev)
s10ediag peek 0x88AD00000
s10ediag mark r: write 0x8C0000000
s10ediag poke 0x8C0000000 0x5A5AA5A5
s10ediag peek 0x8C0000000
s10ediag poke 0x8C0000000 0
s10ediag mark r: write 0x940000000
s10ediag poke 0x940000000 0x5A5AA5A5
s10ediag peek 0x940000000
s10ediag poke 0x940000000 0
s10ediag mark r: read 0x880000000 (sec_rdx_bootdev start - last, riskiest)
s10ediag peek 0x880000000
s10ediag mark r: all high-bank accesses survived
