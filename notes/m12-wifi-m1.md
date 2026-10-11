# M12 WiFi milestone 1 — BCM4375 PCIe link-up in UEFI, ACPI WLAN device, Windows driver reads the chip ID

Goal: code-complete and BUILT, ready for a device test (no flashing in this session — another
session owns the phone). Ported from `origin/drivers` (base 419fc03) onto main by copying the
files (`git checkout origin/drivers -- <paths>`), never `git merge`. Plan:
`notes/drivers-branch-port-plan.md`. Chip research: `notes/m12-wifi-chip.md`. Design:
`docs/exynos9820-port/M12-WIFI-DESIGN.md`.

## Tasks (one commit each)

1. PcieDxe port + review fixes (Stall, FILE_GUID, BEYOND0LTE_PCIE_WLAN flag, 0x68 progress guard,
   'WLAN' status word, capped retries) + Exynos9820.h M12 constants + DSC/FDF.
2. PlatformMemoryMapLib: PCIe0 windows.
3. DSDT `Device (WLAN)` `SAMS9825`, `_STA` gated on the status word.
4. Windows driver `drivers/Exynos9820Wlan` building + signed package.
5. Notes / handoff / milestones.

## Progress log

- 2026-10-12: worktree branch `worktree-agent-afe491e33df42f9e6` was at a8d8a62 (264 behind main);
  fast-forwarded to main 302c285 (`git merge --ff-only main`, no merge commit).
- Exynos9820.h: main has none of the M12 PCIe/CMU_FSYS1/WLAN names (grep `PCIE|FSYS1|BCM|WLAN`
  hits only `EXYNOS_UFS_IOCOH_BASE 0x13C10700`). `EXYNOS_CMU_TOP_BASE`, `EXYNOS_PMU_BASE`,
  `EXYNOS_PERIC0_GPIO_BASE`, `EXYNOS_ALIVE_GPIO_BASE` already exist on main with the same values the
  drivers branch uses -> no duplicate or conflicting defines. Only the M12 block is needed (the D8
  ALIVE-EINT/ABOX block on the same drivers commit range is out of scope here).
- Progress page 0x8FE0F000: [0] prev boot last code, [1] this boot code, [2] USB status (cleared by
  SEC, `ArmPlatformLib.c:100`). Word [3] (0x8FE0F00C) is used by nothing (repo-wide grep for
  `8FE0F00C` / `[3]` on that page: no hits; bootshim only writes [0]/[1]) -> free for 'WLAN'.
- Progress codes in use: 0x51..0x56 shim, 0x60..0x63 SEC, 0x65 parking, 0x66 DVFS/touch, 0x67 USB,
  0x70..0x72 BootLogDxe, 0x80 (BootLogDxe check). 0x68 is free.
- PcieDxe as on drivers: FILE_GUID ...5549 (collides with Beyond0LteSecArmLib.inf), Stall(100*10)
  = 1 ms for a 100 ms WLAN_EN settle, no flag, no guard, worst case 10 tries x (200 ms PERST +
  200 ms poll + PHY seq ~2 ms) ~ 4 s. EP BAR0 written without sizing, BAR2 never assigned.
- Kernel re-read (raw pcie-exynos-host-v0.c lineage-23.2, 2026-10-12): establish_link waits
  usleep_range(18000,20000) after PERST# high (:1240), polls LTSSM 2000 x udelay(10) = 20 ms (:1292),
  retries up to 10 (:1300) and gates the PHY clock between tries. The drivers-branch port waited
  200 ms + 200 ms per try with a comment claiming "18-20ms" -> comment was wrong; kept the longer
  waits (tolerant first bring-up) but cut tries to 3, worst case ~1.2 s, every try logged.
- Order change vs the plan: the memory-map commit goes FIRST (with the constants), because the DXE
  touches those windows; a DXE-before-map commit would sync-abort if built alone.
- EXYNOS_FSYS1_GPIO_SIZE renamed EXYNOS_FSYS1_GPIO_SIZE_UNVERIFIED (AGENTS §6 naming); one page,
  only gpf2 (+0x20) accessed. BCM4375_EXPECTED_PCI_ID renamed ..._UNVERIFIED (not used by firmware;
  the device print closes it).
- Overlap check of the 7 new map windows against every existing 0x13xxxxxx/0x14xxxxxx window
  (UFS 0x13D5..0x13D7, UFS_IOCOH page 0x13C10000): none.
- PcieDxe fixes applied: Stall(100*1000) (100 ms); FILE_GUID ...5553 (repo grep: unused before;
  Guid.xref of the build lists `...5553 Beyond0LtePcieDxe`); `#define BEYOND0LTE_PCIE_WLAN 1`;
  0x68 guard in the UsbHostDxe shape ([0]==0x68 -> skip; [1]=0x68 while running, restored after);
  word [3] zeroed at DXE entry AND in SEC (ArmPlatformLib.c next to the [2] clear) - SEC clear is
  needed because WINPRAM survives a warm reset: a build without the DXE (or a skipped boot) would
  otherwise expose a stale 'WLAN'. 'WLAN' = 0x4E414C57 written only after link-up + VID 0x14E4.
- Behaviour additions (each logged): one canary read of CMU_FSYS1+0x2000 before any write, gate/QCH
  readback after ClocksOn, first ELBI read logged after WLAN_EN settle, per-try "[PCIe] link try n/3",
  failure line with RDLH + PM_DSTATE, link-up line with try/poll count. EP: all BARs sized with the
  standard all-ones probe (decode off), placed naturally aligned in the MEM window (0x14000000+0xFF0000),
  BAR that does not fit -> logged, left 0. COMMAND = MEM only (drivers branch also set BME; no DMA is
  needed before firmware download, so BME is left to the Windows side in milestone 2). Chipid read at
  BAR0+0x3000 (bcmdhd pcie_core.h:55-60, 16 KB BAR0 layout) when BAR0 >= 16 KB.
- Build blocker found + worked around (no repo change): firmware/edk2 is gitignored and absent in a
  worktree. A junction worktree/firmware/edk2 -> main's edk2 makes GNU make (GnuWin32 3.81) fail
  `CreateProcess(NULL, Trim ...)` (e=2): make's PATH search does not find Trim.bat through a junction
  (reproduced with a 1-line Makefile: via junction path fails, via the real path works). Fix used: a
  scratch copy of build-uefi.ps1 with $ws = worktree firmware and $edk2 = main firmware\edk2 (no
  junction); also dropped the empty and `Calibre2"` PATH entries. Build output goes to main's
  firmware/edk2/Build (shared, as instructed).
- BUILD (DEBUG, CLANGPDB, PcieDxe + memory map + SEC clear + DSDT WLAN all in the tree): exit 0,
  `- Done -`, FVMAIN_COMPACT 759104/761856 used, 2752 B free (was ~3136 B before this work);
  BEYOND0LTE_UEFI.fd sha256 4b8abc3f402d5913...0d815. Dsdt.aml contains SAMS9825 + WLST region.

## Review of commits 9e9f5dd (1/5) and 6a0dd62 (2/5)

- FOUND (1/5): the commit also contains the six verbatim origin/drivers PcieDxe files - they were
  still staged in the index from the initial `git checkout origin/drivers -- <dir>` and `git add
  <paths>` does not unstage other entries. Harm: none functional (module not in DSC/FDF until 2/5, so
  not built), but the commit message does not say it. Not rewritten (unpushed, but AGENTS §4: follow-
  ups, no folding); history now reads "verbatim port in 1/5, fixes in 2/5". Lesson: `git status` before
  every commit, not after.
- Checked mechanically: FILE_GUID ...5553 appears once in firmware/ (grep), old names
  EXYNOS_FSYS1_GPIO_SIZE / BCM4375_EXPECTED_PCI_ID have zero users in firmware/ (grep -w),
  `git diff --stat 302c285 HEAD -- Star2LtePkg Exynos9810Pkg` empty (9810 tree byte-unchanged),
  every Stall in the module re-read (only the 100 ms WLAN_EN one changed meaning).
- Checked by reading: guard sets [1]=0x68 BEFORE the first CMU_FSYS1 read (inside PcieWlanBringUp);
  [1] restored on every return path (single exit after the call); 'WLAN' only on EFI_SUCCESS, which
  needs link-up + VID 0x14E4; BAR sizing restores the original value before assigning, handles
  unimplemented (0), dead (~0), I/O, 64-bit (upper sized, BAR+1 skipped) and does-not-fit BARs;
  Cursor alignment assumes power-of-two sizes (PCI guarantees it).
- Accepted risks (recorded, not bugs): ClocksOn writes ACPM/HWACG-owned gates directly (open item
  P4); a PCIe completion error on the CFG/BAR reads could raise an SError rather than return ~0 -
  covered by the 0x68 guard (the boot after skips WLAN), not prevented.
