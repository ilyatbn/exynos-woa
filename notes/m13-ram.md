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
- Run 10 (12:00): Windows restarted itself at 192 s uptime, before the agent (S10eDiag loaded 60 s) - the second
  chkdsk /spotfix (run 9's extra-touch.cmd scheduled it again) repairs and restarts; restart -> 0xA0 PSCI ->
  S10eDiag reset -> TWRP. RAM probe never ran. Mark ring decoded after a firmware bounce (0x81 hardening worked:
  the bounce dumped instead of continuing). Re-run = run 11 (12:16).
- Run 11 (12:07, back 421 s): ALL high-bank accesses from Windows (EL1, MMIO-mapped) SURVIVED:
  0x8C0000000=0, 0x900000000=4b45908a, 0x940000000=58444e49, 0x97FFFF000=abafaf8f, 0x88AD00000=a8004980;
  write 0x5A5AA5A5 + readback OK at 0x8C0000000 and 0x940000000 (restored to 0); 0x880000000=0c1d0201.
  => the 64-bit bank is EL1-accessible under Samsung's EL2. Boot #16's reset (m8 B62) was not the bank itself.
- Live reserved-memory regs (TWRP root, 2026-10-11): sec_rdx_bootdev 0x880000000+0xAD00000, el2_earlymem
  0x97E000000+0x2000000 (Exynos9820.h said 0x200000 - 16x short; fixed), secure_camera 0x96000000+0x2200000,
  camera 0x99000000+0xE400000, camera_contig 0x98200000+0xE00000, el2_code 0x87000000+0x200000, kaslr, sec_debug_magic,
  seclog 0xC0000000+0x80000, modem_spare 0xD0000000+0x1600000, modem_if 0xF0000000+0xB900000, cp_ram_logging
  0xFB900000+0x2000000, mm_debug 0x91200000+0x1000.
- Boot #48 (built, not flashed yet): BEYOND0LTE_PUBLISH_HIGH_BANKS 1, carve-outs + sec_rdx_bootdev + secure_camera
  (likely TZ-protected, was inside Windows' bank0 RAM), el2_earlymem size fixed, high-bank resource HOBs. Table
  capacity MMIO+12 -> +16 (10 DRAM pieces + FB + terminator filled +12 exactly). Expect ~5.3 GB for Windows.
  camera/camera_contig/mm_debug stay RAM (non-secure Android carve-outs) - revisit if Windows faults there.
