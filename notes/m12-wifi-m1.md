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
