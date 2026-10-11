# M16 — USB host (dock: keyboard/mouse) + battery, on main

User request (2026-10-11): "start working on usb … see the drivers branch … take battery related stuff.
if we could run my ugreen type c dock it would be awesome since it would give me keyboard/mouse access too."

Dock (from `notes/d6-external-display.md`): UGREEN = Genesys GL352 hub + ASIX AX88179B GbE; HDMI is
DP alt mode → stays dark (no display driver). Measured worst-case draw 482 mA vs MAX77705 OTG_ILIM 500 mA.

Usage rules carried from `notes/d1-usb-host.md` (safety review):
- Dock **unpowered** (no PD charger into the dock) — host mode + powered hub is the documented damage risk.
- Plug the dock **before** power-on; the boost never turns off on detach in UEFI, no hot-swap.

## Progress log

### 2026-10-11 — import (task 1)
- Git identity: repo-local config was `ntdevlabs`; set to `ilyatbn` per drivers-branch `notes/git-identity.md`
  (now also imported). AGENTS.md §10 on drivers says the same; main's AGENTS.md not yet merged.
- Imported from `origin/drivers` (tip 419fc03), final state, not commit-by-commit (the only cherry-pick
  conflict was DRIVERS-MILESTONES.md missing on main):
  - new files: `Drivers/Beyond0LteUsbHostDxe/*` (UsbPhy.c, Dwc3.c, Pmic.c, FuelGauge.c, UsbHw.h,
    UsbHostInternal.h, .c, .inf), `DRIVERS-MILESTONES.md`, notes d1-usb-host, d1b-usb-vbus-hcd,
    d1-build-env, battery-driver, d6-external-display, git-identity.
  - shared files: `git diff a8d8a62 origin/drivers` of DSC, FDF, Dsdt.asl, Exynos9820.h,
    PlatformMemoryMapLib.c applied with `git apply -3` — all clean.
- Build 1 failed: `GenFv: ERROR 2000: Invalid parameter` on FVMAIN. Cause: **duplicate FILE_GUID**
  `11111111-2222-3333-4444-555555555548` — drivers' UsbHostDxe and main's BootLogDxe both took it.
  (…543 also appears twice but the other copy is Star2LtePkg, a different image.) UsbHostDxe → …552.
- Build 2 (CLANGPDB): green, FD 7 340 032 B.
- Mishap recorded: a `git reset --hard main` during the trial cherry-pick dropped the user's uncommitted
  `.gitignore` line (`info/*`); restored byte-exact from blob 32d32a4 (hash verified). Never reset with a
  dirty tree again — use a worktree for trials.

### Hazard before the first boot (task 2)
- USB31DRD clock gates are ACPM-owned (`notes/d1-usb-host.md` l.228). If sboot leaves them off, the first
  DWC3/PHY MMIO read can hang the bus rather than return 0/0xFFFFFFFF (the driver's only check). A hang in
  DXE with the watchdog would loop the phone through the same hang → needs a progress-code guard (0x67)
  and a `BEYOND0LTE_USB_HOST` flag before any device run.
