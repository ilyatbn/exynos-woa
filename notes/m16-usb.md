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

### 2026-10-11 — review of d5c4370 (import) → task 2
Checked: shared-file hunks (`git show d5c4370 -- Dsdt.asl dsc fdf PlatformMemoryMapLib.c`); MMIO table is
sized from `MMIO_REGION_COUNT` (the extra HSI2C45 entry fits).
Found:
1. **XHC0 `_STA` returned 0x0F unconditionally**: Windows would load USBXHCI on every boot even when UEFI
   skipped/failed the bring-up → a working Windows could hang on gated USB clocks. Fixed in task 2.
2. DSDT comment cited `Exynos9820.h:522`; the GSIV define is at :460. Fixed in task 2 (cites the name).

### Task 2 — guard + flag + gated XHC0
- `BEYOND0LTE_USB_HOST` (Beyond0LteUsbHostDxe.c, default 1).
- Progress 0x67 while PHY/DWC3/PMIC bring-up runs; previous boot ended at 0x67 → skip (one-shot, the
  following boot retries).
- Status word `[2]` of the progress page (0x8FE0F008): SEC clears it (ArmPlatformLib.c next to the 0x60
  write), the driver writes 'USBH' 0x48425355 only after RegisterNonDiscoverableMmioDevice succeeds.
- DSDT XHC0: `OperationRegion (USBS, SystemMemory, 0x8FE0F008, 4)`; `_STA` = 0x0F iff USBH matches.
  Same page S10eDiag already maps from Windows (EfiRuntimeServicesData), so the AML read is a known-safe map.
- Verified: build green; compiled Dsdt.aml carries `USBS` and the compare `55 53 42 48`; the .efi has the
  skip string.
- Not covered: a hang later in XhciDxe (BDS connect) is not under 0x67. If PHY/DWC3 reads pass, the
  clocks are on, and XhciDxe's own loops have timeouts.

### Review of 587024e (task 2)
- Watchdog check: `BEYOND0LTE_EARLY_WATCHDOG 1` (ArmPlatformLib.c:25) arms WTCON 82 s with INFORM3=recovery
  in SEC, so a USB hang in DXE lands in **TWRP after ~82 s**, not in a loop. The 0x67 skip only fires if
  progress[0] survives TWRP's Linux boot; if it does not, each USB-hang boot simply costs 82 s → TWRP.
- AML read risk (unverified): if ACPI.sys refuses the SystemMemory region on RuntimeServicesData, `_STA`
  fails → XHC0 absent (or an ACPI bugcheck caught by S10eDiag → TWRP with the code). First run tells.
- No code change from this review.

### Image for the first device run
- `s10e-artifacts\boot66-v2.img` sha256 c0a99c03167bf9300b99f7f8f724b5b438a88d94c1a03382f415c96b44fd7e20
  = boot #65 config + USB stack + guard (587024e). Phone not on adb at pack time (in Windows, stay.flg).
- Run plan: dock unpowered, plugged in before power-on, a USB keyboard + mouse on it. Collect UEFILOG.TXT
  ("UsbHost:", "Beyond0LteUsbHostEntry", XHCI lines, fuel-gauge report) and, from Windows, pnputil
  /enum-devices for ACPI\PNP0D10 + USB\ devices.

### Run 1 (boot #66) — flashed 2026-10-11
- TWRP: boot66-v2.img dd'd to by-name/boot, first 4457x4096 B sha256 c0a99c03… = image. S10eDiag from
  `s10e-artifacts\s10ediag-m16` (adds s10etouchlog.exe), extra.cmd = extra-usb.cmd, stay.flg kept.
- Gotcha: Git Bash rewrites `/tmp/x` adb args into a Windows temp path → `MSYS_NO_PATHCONV=1`.
  TWRP busybox `stat` has no `-c`; use the fixed count 4457.
- Installer review: its `sed 's/^M*$/^M/'` lines hold literal CR bytes (od -c) — correct, not the old bug.
