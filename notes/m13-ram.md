# M13 - more RAM for Windows (S10e) (2026-10-11)

User: "the hard job of adding at least 1 more core and as much ram as possible".

## Facts
- Windows today: bank0 0x80000000..0xBAB00000 + carved bank1 0xC0000000+0x20000000 + bank2 0xE1900000+0x1E700000
  (~1.5 GB; m10 W33).
- TWRP /proc/iomem (2026-10-11, root): System RAM 80000000-baafffff, c0000000-dfffffff, e1900000-ffffffff,
  880000000-97fffffff (4 GiB = Exynos9820.h BANK3 0x880000000+2 GiB and BANK4 0x900000000+2 GiB).
- UEFI boot #16 reset when DXE used the high banks (m8 B62/B63, RamManagerDxe) - cause never isolated.
- Vendor DT (AI-sourced, notes/vendor-dt-reserved.md): sec_rdx_bootdev 0x880000000+0xAD00000.

## Progress log
- Run 10 plan: drivers/S10eDiag/esp/extra-ramprobe.cmd - from Windows (EL1 under Samsung EL2), single-word
  MMIO-mapped reads of 0x8C0000000, 0x900000000, 0x940000000, 0x97FFFF000, 0x88AD00000, then write/readback/restore
  at 0x8C0000000 and 0x940000000, last 0x880000000; a mark before each step. The first abort names the address.
