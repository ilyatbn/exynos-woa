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

### Run 1 results (boot #66, UEFILOG boot 121; artifacts s10e-artifacts\run-m16-1)
- No hang. `UsbPhyInit: USBCON CTRL_VER = 30000` (PHY block alive, clocks on).
- `Dwc3HostInit: GSNPSID = 33313130 — not DWC_usb3` → bring-up stopped → XHC0 hidden (USB status word
  read from Windows `008fe0f008: 00000000`) → guard worked as designed; Windows booted normally.
  **0x3331xxxx = DWC_usb31** (the block is USB31DRD). The imported code rejected it although
  notes/d1-usb-host.md Q2 step 1 already records the kernel rule (revision from VER_NUMBER 0xC1A0).
- `Hsi2cXfer: timeout addr=36 reg/first=0 INT_STAT=0` / `FuelGaugeReport: gauge 0x36 not answering` →
  the HSI2C45 path is the next blocker: VBUS needs the same bus (CCIC 0x25, charger 0x69).
  INT_STAT=0 for a full second: either the block is unclocked/not in I2C mode, or a NACK that we cannot
  see — Hsi2cXfer enables ONLY TRANSFER_DONE in INT_EN (our working Windows HSI2C driver enables the
  error set and acks INT_STAT before each transfer).
- Live DT (TWRP): hsi2c@15D60000 status okay, `samsung,usi-i2c-v2`, `samsung,check-transdone-int`,
  `samsung,scl-clk-stretching`, single child `max77705@66` (FG/CHG/CCIC are dummy clients 0x36/0x69/0x25).
  No `usi@` mode node for the CMGP USIs among /proc/device-tree top-level names (only usi@1042xxxx PERIC0).
- Windows side: `pnputil` USB/keyboard/mouse empty (expected, XHC0 hidden); Net shows only KDNIC.
- Touch log: `C:\S10eTmp\req\touchlog.txt` absent → s10etouchlog never ran (user held on desktop/taskbar).
- "Reboot to TWRP" icon: Explorer error "Windows cannot access the specified device, path or file";
  agent log shows no request → the .cmd never executed. Launcher bytes verified correct (CRLF, path).
  Open; asked the user whether other icons (Task Manager, Touch log) open.

### Task 3 — accept DWC_usb31 (kernel encoding)
- GSNPSID[31:16]==0x3331 → Rev = VER_NUMBER(0xC1A0) | 0x80000000 (DWC3_REVISION_IS_DWC31), as 4.14
  compares: all ">= usb3 rev" branches taken, all "<= 2.10a" workarounds skipped.
- DWC31 ≥1.70a GUCTL3 USB20_RETRY_DISABLE: bit not in our notes → not written, logged as UNVERIFIED.
- Build green. (Python heredoc mangled `\n` + em-dashes again; repaired byte-wise with a scratchpad
  script; verified by re-reading.)

### Task 4 — HSI2C45 diagnostics (run 2 instrument)
- Hsi2cDump: CTL/FIFO_CTL/TRAIL/INT_EN/INT_STAT/ERR(0x2C)/FIFO_STAT/CONF/AUTO_CONF/TIMEOUT/TRANS_STATUS/
  FS1-3/ADDR/USI_CON/USI_OPT at handoff, after init, on the first transfer timeouts (max 6 dumps).
- INT_EN now TRANSFER_DONE | ABORT(8) | NO_ACK(9) | NODEV(10) | TIMEOUT(11); INT_STAT acked (0xFFF)
  before each kick. Bits from drivers/Exynos9820Hsi2c/exynos9820hsi2c.h (device-validated on the touch bus).
- SW reset now an explicit pulse (set, clear) like exynos5_i2c_reset and our Windows driver.
- HSI2C_TIMEOUT_MS 1000 → 50 (software poll; a PMIC transfer is < 1 ms).
- PmicBusProbe: reg 0 of 0x66 (DT max77705@66), 0x25 CCIC, 0x69 charger, 0x36 FG; CC attach poll only
  if the CCIC answered (a dead bus would have cost ~30 x timeout).
- Build green.

### Review of db8cee8 → task 5 (follow-up)
- Found: after an error the controller was not reset (our Windows driver resets after every error,
  controller.cpp:1014) → one NACK could wedge the remaining probes with MASTER_BUSY.
- Found (more important): PmicBusInit did SW_RST, then rewrote only ONE byte-field each of FS1/FS2/FS3
  from UNVERIFIED `HSI2C_TIMING_FS*_BOOT` values. Linux re-runs exynos5_hsi2c_clock_setup after every
  exynos5_i2c_reset and our Windows driver re-runs ControllerSetBusTiming → SW_RST evidently clears the
  timing registers → run 1 likely ran with mostly-zero timing = the INT_STAT=0 timeout. Unproven until
  run 2's "at handoff"/"after init" dumps show it.
- Fix: snapshot FS1/FS2/FS3/SLA(0x6C)/CONF/TIMEOUT at handoff (bootloader = working reference), Hsi2cReset()
  = SW_RST pulse + restore + kernel init writes + INT ack; used at init and after timeout/error/busy.
  The `_BOOT` constants stay in UsbHw.h but are no longer used.

### Run 2 (boot #67) — flashed 2026-10-11
- boot67-v2.img sha256 239bb3a0…18db (verified on the partition) = 524e8ab: DWC_usb31 accepted, HSI2C45
  timing preserved across reset, register dumps, client probe, CC poll gated on the CCIC answering.
- Expect in UEFILOG: `Hsi2c45 at handoff/after init` dumps, `PmicBusProbe: client …`, `Dwc3HostInit:
  DWC_usb31 GSNPSID 33313130 VER_NUMBER …`, then either `CC source attach … VBUS boost on` or the reason.
- Open for later: charger register protection (kernel unlocks CHG_CNFG_06 CHGPROT before CNFG writes) -
  read CNFG_02 back after the OTG_ILIM write once the bus answers.

### Run 2 result (boot #67): synchronous exception during UEFI (user saw the dump before the Windows logo)
- No UEFILOG (crash before the ESP filesystem exists; UEFILOG.TXT still boot 121).
- Cause found by reading the newly reached path (run 1 stopped at the identity check, run 2 passed it):
  `Dwc3RootHubPowerOff` computed `PortScBase = MmioRead32 (DWC3) + 0x400` - a 32-bit read of
  CAPLENGTH|HCIVERSION<<16 (HCIVERSION typ. 0x0110) → PORTSC at ~0x10C00000+0x01100420, unmapped →
  synchronous abort. The comment said "byte read". Not yet confirmed by a dump; the fix is right anyway.

### Task 6 — PORTSC base fix
- `(MmioRead32 (DWC3) & 0xFF) + 0x400` (32-bit read + mask like Linux HC_LENGTH, no byte access to
  device registers) and a bound check: every PORTSC inside the 0x8000 xHCI window, else skip + log.

### Task 7 — DWC3 event buffer (found while hunting the run 2 crash)
- Was `AllocateAnyPages(EfiBootServicesData)` + `GEVNTADRHI = 0`: (a) with the high banks published the
  allocator may return >4 GiB → truncated DMA address; (b) BootServicesData is reclaimed by Windows while
  the core still points at it. Now `AllocateMaxAddress 0xFFFFFFFF`, `EfiReservedMemoryType`, HI written.

### Task 8 — XHC0 `_CCA 0` (import review finding, deferred until now)
- The drivers-branch DSDT declared `_CCA 1` while its own comment says the DT node is NOT dma-coherent
  and the UEFI side is NonCoherent. `_CCA 1` = Windows assumes snooping, no cache maintenance → stale
  data. Changed to `_CCA Zero`, as UFS0 (which Windows runs with today).

### Run 3 (boot #68) — flashed 2026-10-11
- boot68-v2.img sha256 cb25db01…d465 verified on the partition = cceb4ee (tasks 6-8 on top of run 2).
- If it crashes again: ask the user for a photo of the exception screen (PC + image name); the
  exception flush cannot reach the ESP this early.

### Run 3 (boot #68) — user report
- Booted into Windows (the PORTSC crash is gone). Dock: no light → VBUS still off (logs pending).
- "Reboot to TWRP.cmd" fails, every other .cmd launcher works. User: make it a small C app.

### Task 9 — s10ereboot.exe
- drivers/S10eDiag/S10eReboot.c: creates C:\S10eTmp\req\recovery.req (Users-writable, no elevation),
  waits up to 10 s for the agent to delete it, message box if not (agent down → key-combo hint).
- Desktop: `Reboot to TWRP.exe` (the exe itself, no .cmd in between); desktop.cmd deletes the old .cmd
  and its idempotency check now keys on the .exe; the installer also places it directly on the NTFS.
- build.bat → S10EDIAG_BUILD_OK (W4/WX).

### Run 3 UEFILOG (boot 123)
- `UsbHost: previous boot hung in USB bring-up (progress 0x67) - skipping it` → run 3 did NOT test USB;
  the guard skipped it after run 2's exception (progress survived the TWRP boot in between). The next boot
  (#68 still flashed) retries with tasks 6-8. Also: the installer had to fsck the NTFS (dirty after reset);
  TWRP auto-mounts userdata ro at /data, which blocks a second mount - umount /data first.

### Run 4 (boot #68 retried USB; UEFILOG boot 124) — user: right-click works, Reboot-to-TWRP.exe works, dock dark
- **xHCI is up in UEFI**: `DWC_usb31 GSNPSID 33313130 VER_NUMBER 3137302A` (1.70a), `CAPLENGTH 20, 2 root
  ports`, XhciDxe `HcSParams1 0x2000240` (2 ports), HCCParams 0x228FFED (AC64), Usb2Sup/Usb3Sup present, no
  DbC (DebugCapSupOffset 0xFFFFFFFF - no xHCI debug capability for KD over USB), CMD/EVENT rings created,
  `UsbRootHubInit: max speed 3, 2 ports`, `UsbBusStart`. Windows saw 'USBH' (0x8FE0F008 = 48425355).
- GUCTL3 USB20_RETRY_DISABLE quirk applies (>= 1.70a) and is NOT written (UNVERIFIED bit) - follow-up.
- **HSI2C45 dead because unconfigured**: at handoff every I2C register 0, USI_CON 1 (USI in reset),
  USI_OPT 1; after init CTL writes do not stick (MASTER not latched), USI_CON accepted 0. All 4 clients
  time out. My run-2 snapshot/restore had restored those zeros. The bootloader never set this bus up
  (the DT carries bit-bang fallback pins gpio_scl gpm12 / gpio_sda gpm13).
- Live DT: `usi_03_cmgp` -> /usi@15C22030 (exynos-usi-v2, mode "i2c"), alias pinctrl2 -> /pinctrl@15C30000;
  hsi2c45-bus pins gpm12-0/gpm13-0 function 2 (pud/drv unreadable with TWRP busybox).
- Kernel table fetched (LineageOS android_kernel_samsung_exynos9820 lineage-18.1, pinctrl-exynos-arm64.c,
  scratchpad copy): exynos9820_pin_banks2 "CMGP data": gpm12 0x180, gpm13 0x1A0, alive bank type
  CON+0/DAT+4/PUD+8/DRV+C (4-bit fields). The 9810 table has 0x140/0x160.
- The `HSI2C_TIMING_FS*_BOOT` values equal the Windows driver's ControllerSetBusTiming result for
  400 kHz @ 200 MHz (divider 0x21, masks 0xE0/0xF0) - so they are the validated formula's output.

### Task 10 — HSI2C45 bus setup (run 5 instrument + fix)
- Exynos9820.h: EXYNOS_CMGP_USI03_MODE 0x15C22030, EXYNOS_USI_V2_MODE_I2C 4, EXYNOS_CMGP_GPIO_BASE
  0x15C30000 (+0x1000), GPM12/13 CON 0x180/0x1A0, function 2 - all with their sources. Memory map: both pages.
- Hsi2cBusSetup: log mode + pin CONs, set mode 4, pins function 2 (pull/drive untouched), log again.
- Hsi2cReset mirrors the Windows ControllerReset exactly (incl. FS timing constants, TIMEOUT 0, FIFO/AUTO/
  ADDR 0); snapshot/restore removed.
- Still possible: the USI_CMGP3 clock gate is off (ACPM-owned) - run 5's dumps will show whether CTL latches.

### Run 5 (boot #69) — flashed 2026-10-11
- boot69-v2.img sha256 dbcb40b0…a54a verified = 4423d3f.

### Battery (user: "i was hoping to see the battery indicator in windows")
- What exists: only the drivers-branch UEFI PoC (FuelGauge.c) that LOGS a fuel-gauge read at boot - no
  Windows battery driver. Both depend on HSI2C45, which has never worked yet (runs 1-4: gauge 0x36 times out).
- Plan once run 5 shows the bus alive: (1) gauge values in UEFILOG cross-checked with Android;
  (2) a second instance of our Windows HSI2C SPB driver for hsi2c_45 (ACPI device, CMGP sysreg/pinctrl)
  + a KMDF battery miniclass driver (MAX77705 FG 0x36: SOC/VCELL/current) behind ACPI PNP0C0A-style
  device → the Windows battery icon. notes/battery-driver.md has the register map.

### Run 5 results (boot #69; boots 125 = Windows → "Preparing Automatic Repair", 126 = after-windows → TWRP)
- **HSI2C45 alive**: after Hsi2cBusSetup (USI_CMGP3 mode 0→4, gpm12/13 CON 1→2 i.e. GPIO→I2C) the block
  latches (CTL 8, CONF 80000000, FS1/2/3 F00000/E0/210000, TRANS_STATUS 80001). All four clients ACK.
- **Fuel gauge reads**: `[FG] SOC 99.2% VCELL 4176mV AVG_I -377mA ... FULLCAP_REP 2460` - plausible.
  TEMP prints 376.9C → formula bug in the PoC (follow-up).
- Single-byte reads all return 0 (PMIC 0x66 reg 0, CCIC 0x25 reg 0, CHG, FG reg 0) and CC_STATUS0 = 0 for
  3 s → no VBUS, dock dark. Same Hsi2cXfer path as the working FG word reads, so possibly genuine zeros;
  unproven. Next instrument: dump CCIC 0x00-0x1F (HW_REV/FW_REV must be non-zero).
- **Windows failed before starting**: nothing on C: modified during boot 125 (bootstat.dat + all recent
  files are run 4's, 14:25 UTC); no SrtTrail, no dump; PRAM marks are run 4's. progress 0x72 → died after
  ExitBootServices without ResetSystem; boot manager went to WinRE. Firmware deltas vs run 4: HSI2C45
  setup + CC poll + 2 new MMIO map entries (CMGP_USI 0x15C22000, CMGP_GPIO 0x15C30000, appended out of
  address order after HSI2C45). Repro first: re-boot the same image (boot #69) once.

### Run 5 repro + diagnosis setup
- User: Automatic Repair repeats with boot #69, also WITHOUT the dock → not dock-related.
- ESP `\EFI\Microsoft\Boot\BOOTSTAT.DAT` unchanged since 10-10, BCD last written in run 4 → nothing
  remembered; BCD already has `bootstatuspolicy IgnoreAllFailures` → this is winload's automatic failover
  on a real error in every boot (progress 0x72 = WinRE's own ExitBootServices).
- Ruled out by reading: the two new MMIO table entries feed only UEFI page tables (not the EFI memory
  map Windows gets); no MAX77705 write ran (only VbusOn writes, never reached).
- Remaining firmware deltas vs run 4: HSI2C45/USI_CMGP3/gpm12-13 configured; reads of PMIC/CCIC/FG;
  ~3 s CC poll; INT_EN left with DONE|errors on the I2C block.
- BCD edited offline (`bcdedit /store`): {default} recoveryenabled No + bootlog Yes, so boot manager
  shows the winload error code instead of WinRE. Original kept: s10e-artifacts\bcd\BCD.orig-20261011
  (sha 52093777…6ec5); edited BCD.work sha 2a81716b…2e8b on the ESP. Revert = push the .orig back.

### Run 5 BCD result: blue screen 0x14C FATAL_ABNORMAL_RESET_ERROR (user, boot #69, recovery disabled)
- So the kernel DOES start; it bugchecks early (before S10eDiag) on every boot of #69, with or without
  the dock. Microsoft: 0x14C = unrecoverable error / abnormal reset (watchdog, hang, brownout, failed
  bugcheck path), no parameters. Our GTDT declares no watchdog; no ACPI table carries reset info.
- Only firmware delta vs the good run 4 that persists into Windows: HSI2C45 left configured (USI_CMGP3
  mode 4, gpm12/13 in I2C function, USI out of reset, INT_EN set). The bootloader leaves it idle (mode 0,
  pins GPIO out, USI in reset); CMGP is the always-on domain shared with APM/CHUB. Hypothesis: the
  configured bus/pins trip a platform-level fault once Windows runs.

### Task 11 — PmicBusRelease (test of the hypothesis, and the right design regardless)
- Hsi2cBusSetup saves mode, gpm12/13 CON, USI_CON; PmicBusRelease (called from the entry wrapper after
  UsbHostBringUp on every path) disables INT_EN, acks, AUTO_CONF/CTL 0, USI_CON restored (reset), mode
  and pin functions restored, logs the result. PMIC-side state (VBUS boost) is untouched by the release.

### Run 6 (boot #70): 0x14C again → bus-state hypothesis FALSE
- UEFILOG boot 134: setup (mode 0→4, CON 1→2), probe, FG `SOC 99.6% VCELL 4232mV`, no CC attach, then
  `Hsi2c45 bus released: mode 0, gpm12 CON 1, gpm13 CON 1, USI_CON 1` - handoff state identical to the
  bootloader's, and Windows still bugchecks 0x14C.
- Decisive test next: boot #68 (the exact run-4 image that reached the desktop, sha cb25db01…d465)
  reflashed. If 0x14C persists → cause is NOT the firmware delta (something changed on the phone after
  run 4: PMIC state from the FG/CCIC reads persisting across resets, NTFS/registry, BCD edit, …).

### Run 7: boot #68 (run-4 image) ALSO bugchecks 0x14C → not the firmware delta
- Timeline: run 3 (XHC0 hidden by the 0x67 skip) booted; run 4 = FIRST boot with XHC0 visible - Windows
  installed usbxhci + root hub LIVE after boot and reached the desktop; every boot since (#69, #69, #70,
  #68) = 0x14C. What changed on the phone: usbxhci is now an INSTALLED driver for ACPI\PNP0D10 and starts
  early in boot on our controller.
- Test (boot #71, sha 176eaf5b…efaa): firmware 95c0cf2 + `BEYOND0LTE_USB_EXPOSE_TO_WINDOWS 0` - USB
  still brought up in UEFI, 'USBH' not written → XHC0 _STA 0 → usbxhci not started. Windows boots ⇒ the
  early USBXHCI start on our DWC3 is the 0x14C.
- User OK'd a Windows reflash with a backup of desktop/apps/settings if needed - not needed if this holds.

### Run 8 (boot #71, XHC0 hidden): 0x14C again → USBXHCI is NOT the cause either
- No C:\Windows\ntbtlog.txt despite BCD bootlog Yes → the kernel dies before driver loading/logging.
  LiveKernelReports empty, no MEMORY.DMP.
- Clue: .NET native images (C:\Windows\assembly\NativeImages_*) regenerated within the run-4 session
  window → Windows Update/servicing likely ran in run 4. New hypothesis: a cumulative update was staged in
  run 4 and every boot since runs the updated kernel/boot path (or the boot-time servicing step), which
  fails on our platform with 0x14C - independent of firmware image and of XHC0.
- Next reads (TWRP, NTFS ro): WinSxS\pending.xml, CBS.log tail, servicing\Packages newest, timestamps of
  ntoskrnl.exe / hal.dll / winload.efi / ci.dll vs run-4 time (~14:25 UTC).
- adb dropped mid-check; user rebooting the phone into TWRP.

### Session 2026-10-11 (after host reboot) — Windows Update hypothesis checked in TWRP: FALSE
- ntoskrnl/hal/winload.efi/ci.dll all dated 2023-12-04 (stock 22621.2506), no WinSxS\pending.xml,
  CBS.log last written 06:38 UTC, COMPONENTS 01:21, SoftwareDistribution\Download empty → no update staged.
- DumpStack.log.tmp, pagefile.sys, swapfile.sys = 14:25:11 (run 4's boot); nothing since → every later
  boot dies before smss opens the pagefile. hiberfil.sys header zero (not hibernated, no fast-startup image).
- Only hive written in run 4's session: SYSTEM (+LOG1/2) at 14:36:46-47. DRIVERS/SOFTWARE 11:00.
  → the persistent change is in the SYSTEM hive (driver/service/device config) or in firmware variables.
- Full list of files modified since 14:12 (find -mmin, TWRP): run 4's Windows session = 14:25:04..14:28:18
  (agent log 14:26:32, touchlog 14:26:37, waasmedic 14:28). NO DriverStore/INF/setupapi writes from Windows
  itself. Then the TWRP installer: 14:34:07 S10eDiag.sys, S10eAgent.exe, s10e*.exe, Reboot to TWRP.exe;
  14:36:47 SYSTEM hive (offline edit); 14:36:56 S6SY761TouchBeyond0.sys (both DriverStore copies).
  → The persistent post-run-4 change is the installer's driver/hive update, not usbxhci, not Windows Update.
- adb push keeps the SOURCE mtime, so the 14:34-14:36 times are host build/edit times, not device writes.
  Device SYSTEM hash (073a9a70…) differs from the installer's output (A0cW/SYSTEM 3878f08d…) → the hive
  was written again after the install (by a later failing boot or a later install).
- WINPRAM.BIN (15:29): mark ring empty, progress words 0x31/0x71 → not even S10eDiag (system start) or the
  UFS driver wrote a mark: death precedes our drivers.
- Registry diff (pre-run-4 / post-run-4 / now, regipy offline) started and ABANDONED at user's call:
  "sometimes things just break" → reinstall a fresh Windows and continue with drivers; revisit 0x14C only
  if it returns. Host hive work dirs kept in %TEMP%\s10ediag-hive.* (i2lM = hive run 4 booted, A0cW
  SYSTEM.orig = run 4's result) if it does.

### Fresh Windows reinstall (user, 2026-10-11 ~19:00 local)
- Plan: S10eHarness transfer of the pristine s10e-artifacts\win-w5 image (windows.img 119 GB raw, esp\) →
  USERDATA + CACHE ESP (wipes the broken install); s10e-swap-ufs.sh ufs-w23\Exynos9820Ufs-w34.sys; boot #71
  stays on BOOT (XHC0 hidden, USB bring-up in UEFI); unattended Setup (specialize + oobe, ~1.5 h on 1 core).
- Afterwards (as in §24/§25): W37 power hive edit + s10e-install-diag.sh need `reg load` = an ELEVATED shell
  (this session is not); touch/HSI2C via pnputil inside Windows; BCD numproc.
- 19:05 transfer EXIT 0 (win-w5 windows.img, 11.7 GiB written + verified; ESP files refreshed in place - old
  UEFILOG/WINPRAM/S10EDIAG\out stay). Log: s10e-artifacts\win-w5\transfer-20261011.log.
- Fresh BCD = installer default numproc 4 + testsigning. User: we had 6 cores (A55x4 + A75x2 via parking,
  mask 0x3E) → numproc must go. bcdedit /store is Access denied unelevated →
- New: firmware/tools/s10e-setupcomplete.cmd placed as C:\Windows\Setup\Scripts\SetupComplete.cmd by
  firmware/tools/s10e-provision-fresh.sh (TWRP, no elevation): bcdedit /deletevalue numproc, W37 power keys,
  S10eDiag/S10eAgent via sc create + tools, mountmgr NoAutoMount + mountvol D..Z /D, test cert + pnputil
  Exynos9820Hsi2c (touch-t2) + S6SY761TouchBeyond0 (touch-hold = 6.0.5058.5). Log C:\S10eProv\setupcomplete.log.
- Provisioned: UFS w34 on device sha 8ea93ce8… = host. First run's umount hit "busy" (set -e stopped before the
  ESP step; finished by hand) → retry loop added. ESP S10EDIAG: collect/desktop.cmd fresh, no extra.cmd, no
  WINRUN.FLG, stay.flg kept (interactive). BOOT = boot #71 (XHC0 hidden).

### Fresh install result (user, 2026-10-11 ~19:50)
- Windows installed + booted. SetupComplete log: every step OK (bcdedit numproc delete, power keys, services,
  cert, Exynos9820Hsi2c = oem2 on ACPI\SAMS9820\a, S6SY761TouchBeyond0 = oem3 on ACPI\SCSY0761\0).
- 1 CPU on the first boot = numproc removal applies from the next boot. pnp-problem: only CPU 6/7 (M4,
  not parked) code 10 = same as before. S10eDiag + S10eAgent RUNNING.
- The TWRP bounces: the user's "Reboot to TWRP" click on boot 1 (agent service did not exist yet ->
  s10ereboot's "error" after 10 s) left C:\S10eTmp\req\recovery.req; the agent acted on it after the desktop
  on later boots (agent.log "request: reboot to TWRP" 5 s after collect; the delete was lost once in the
  flushreset). Fix: S10eAgent discards request files present at its start (stale). Built (s10ediag-m17),
  S10eAgent.exe deployed (sha 80652877… = host). WINRUN.FLG removed by the stage script.

### M17 finding applied to the host path (2026-10-11 ~21:30)
- Root cause for the dark dock in runs 1-5 (INFERRED, test pending): the MAX77705 USB-C D+/D- switch is left OPEN by
  Linux's shutdown (notes/m17-muic-switch.md); M17 proved the same open switch blocked device mode. UsbHostBringUp
  now calls Max77705UsbSwitchToAp after PmicBusProbe (logged before/after CONTROL1).
- BEYOND0LTE_USB_EXPOSE_TO_WINDOWS back to 1 (run 8 showed the 0x14C persisted with XHC0 hidden, so usbxhci was not
  its cause; fresh install since). BEYOND0LTE_USB_KD_DEVICE default 0 = host image.
- boot #79 (sha cbf70f68…) = host/dock image; boot #78 = KD image. BCD debug must be OFF with #79 (the kd module
  would claim the same DWC3).
