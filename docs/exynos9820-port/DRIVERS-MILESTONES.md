# Driver milestones (phase two) — USB host, hub, HID, WiFi, GPU/display

This file is the plan of record for the **driver work stream**, run in parallel with
the boot work stream (M8/M9 in [`MILESTONES.md`](MILESTONES.md)). It expands the
M10–M15 rows of that file into buildable tasks with a fixed order of attack.

Scope, in the user's own words (2026-10-10): make **keyboard and mouse work over
USB-C**, support a **non-powered USB hub** for more USB2 ports, get **WiFi**, and get
**GPU support** — building them one by one, using research from Linux/Android
solutions for the Galaxy S10e (`beyond0lte`, SM-G970F).

## What phase one already settled (do not re-litigate, build on it)

| Fact | Where |
|---|---|
| DWC3 core at `0x10C00000`+0x10000, **identical between 9810 and 9820**; exposes a standard xHCI register interface | `notes/dock-desktop.md` §4/§5, `Exynos9820.h:395-396` |
| USB PHY `10B00000`+0x200, combo PHY `10AE0000`+0x2800, PCS `10AF0000`+0x800 — all VERIFIED; `has_combo_phy=1` (USB3 + DisplayPort share the PHY) | `Exynos9820.h:397-402`, `notes/dock-desktop.md` §11 |
| Shipping DT says `dr_mode = "otg"` — host mode is a **firmware decision**, not a Windows one | `notes/dock-desktop.md` §8 |
| USB IRQs: DWC3 SPI 241 → **GSIV 273**; PHYs SPI 277/278 | `Exynos9820.h:522-523` |
| CMU windows for USB resolved from Samsung cal-if: `CMU_PERIC0 0x10400000+0x8000`, `CMU_FSYS0A 0x10a00000+0x8000`, gates IPCLK `0x2094`/PCLK `0x2098`/RSTNSYNC `0x2054`/OSCCLK `0x2008`, QCH `0x3038` | `notes/s10e-live-reads.md` §7.5 |
| Mu-Silicium `UsbfnDwc3Dxe` exists but USB Host Mode is ❌ on every Exynos target — nobody finished it | `notes/dock-desktop.md` §3 |
| Our 9810 `UsbDebugHid.c` is a full DWC3 register vocabulary but programs **peripheral** mode, and is compiled off | `notes/dock-desktop.md` §9 |
| EDK2 USB stack (`UsbBusDxe`, `UsbKbDxe`, `UsbMassStorageDxe`) is **commented out** in both platform FDFs | `Star2LtePkg.fdf:167-169`, `Beyond0LtePkg.fdf:259-261` |
| Phone WLAN is Broadcom `bcmdhd` with no Windows driver and no DT device node — vendor-WiFi on Windows is a **dead end**; USB WiFi adapter is the fallback | M12 in `MILESTONES.md`, `notes/dock-desktop.md` §6 |
| **Safety:** powered hub + host mode "can irreversibly damage your device"; the user's hub is **non-powered**, which is the safe configuration | `MILESTONES.md` rule 7, woa-mh2lm5g materials.md |
| One USB-C connector: host mode and charging compete for the same port | `notes/dock-desktop.md` §8 |

## The two halves of every USB task

USB host on this platform has **two independent halves**, and they are verified at
different times:

1. **Firmware half (UEFI):** a DWC3 host-mode driver inside `BEYOND0LTE_UEFI.fd` +
   the EDK2 USB stack. This is testable **as soon as UEFI boots** — a keyboard
   attached through the hub should reach `BdsDxe`/`ConIn` before Windows exists.
2. **Windows half:** an ACPI-enumerated xHCI device so Windows' inbox `USBXHCI.sys`
   binds the same MMIO window, plus whatever role/VBUS switching the firmware must
   leave latched. Mu-Silicium has never finished this on Exynos.

Every USB task below is split `fw` / `win` accordingly. The firmware half is the
risk-reducer: if keyboard+hub work in UEFI, the DWC3 init is proven and only the
ACPI/Windows half remains.

## Milestones

| # | Milestone | State | Blocked on |
|---|-----------|-------|------------|
| D1 | DWC3 **host mode** in UEFI (the gate for everything else) | todo | boot stream (validation only — research and coding can start now) |
| D2 | USB hub (non-powered) + keyboard/mouse in UEFI (`ConIn`) | todo | D1 |
| D3 | USB host under **Windows**: ACPI xHCI device, inbox `USBXHCI.sys` | todo | D1, boot stream |
| D4 | WiFi: USB adapter path chosen + validated (vendor bcmdhd stays closed) | todo | D3 |
| D5 | GPU/display: research verdict on Mali-G76, then DECON GOP in UEFI | todo | — (research can start now) |
| D6 | External display (DP alt mode over the combo PHY) — stretch | todo | D1, D5 |

## Task list

### D1 — DWC3 host mode in UEFI

| Task | State | Notes |
|------|-------|-------|
| Research: how Linux/Android on the S10e brings up DWC3 (exynos-dwc3 glue, USBFN, PHY init order, VBUS/ID role) | **done** | `notes/d1-usb-host.md` Q1/Q2: full chain pinned from `LineageOS/android_kernel_samsung_exynos9820` — glue `dwc3-exynos.c`, PHY `phy-exynos-usbdrd.c` (PMU de-isolate `0x1586072C` bit0 / DP `0x15860704`), USBCON reg map, `dwc3_core_init` HOST path with absolute offsets (GCTL `0x10C00C110`, PRTCAPDIR=1<<12), xHCI MMIO = same window +0x0..0x7fff. **Trap: USB clock gates are ACPM-owned vclk/QCH, not CMU MMIO** — strategy is audit-then-rely-on-sboot-state |
| Research: Mu-Silicium `UsbfnDwc3Dxe` + edk2-exynos 9820 USB prior art — what exists, what is broken | **done** | `notes/d1-usb-host.md` Q3/Q4: `UsbfnDwc3Dxe` is **device-mode only** (produces EFI_USB_FUNCTION_IO), Host ❌ on every Samsung target because 9820/9810 use the NULL `PlatformUsbfnLib`; but `S5E9830Pkg/Library/PlatformUsbfnLib/BoardInit.c` + `PlatformUsbRegisters.h` are a directly reusable UEFI-native PHY bring-up (same PMU offsets/bits; **9830 block bases differ — do not paste**). edk2-exynos has **zero** USB controller code. VBUS = MAX77705 boost via I2C (`CHG_CNFG_00` OTG modes, `CHG_CNFG_02` OTG_ILIM 500 mA–1.5 A); SoC has no VBUS pad, no OVC input. Windows half: inbox `usbxhci.inf` binds `ACPI\PNP0D10` — D3 is an ACPI table task, not a driver task |
| Research follow-up: hsi2c_45 base + MAX77705 CC-role (Rp sourcing) init + UEFI xHCI-on-MMIO binding prior art (edk2 XhciDxe is PCI-bound) | **done** | `notes/d1b-usb-vbus-hcd.md`: `hsi2c_45 = 0x15D60000`+0x1000 (live on device, clocks ACPM-owned like USB — same leave-alone strategy), polled master sequence pinned from `i2c-exynos5.c` and already implemented in our Windows HSI2C driver; MAX77705 sub-addresses charger **0x69** / CCIC **0x25**, VBUS = `CNFG_00` mode 0x4→1 ms→0xE, `CNFG_02` OTG_ILIM; CCIC is firmware-loaded and already DRP (CCCTRL1=0x19) so a dumb hub attaches us as source — just poll `CC_STATUS0.CCStat==2`; **UEFI verdict: `RegisterNonDiscoverableMmioDevice(Xhci, NonCoherent, init-cb, 0x10C00000, 0x8000)` + `NonDiscoverablePciDeviceDxe` + stock `XhciDxe`** (NXP Layerscape is the exact precedent; XhciDxe polls, no IRQ needed) |
| Add the USB host hardware constants to the platform (USBCON/DWC3-global register map, PMU isolation offsets) | **done** | `Drivers/Beyond0LteUsbHostDxe/UsbHw.h` — every bit from the LineageOS kernel headers or Mu's 9830 map, cited per line; `Exynos9820.h` gains `EXYNOS_PMIC_HSI2C_*` (0x15D60000, DT + live node) and `EXYNOS_USBDRD_GSIV 273`; memory map gains the HSI2C45 entry. Three transcription errors caught in review (FIFO enables bits 0/1 not 29/30, USI_RESET bit0 not 13, FS timing 0xF0/0xE0 not 0x0F/0x1F) and the `utmi_clk` DT tune entry proven IGNORED by the kernel parser — HSP_TUNE is now a masked RMW pair. Syntax-checked against real EDK2 headers (`HEADER_SYNTAX_OK`) |
| Write `Beyond0LteUsbHostDxe`: PHY de-isolation + USBCON USB2 init + DWC3 core HOST-path init (the 7-step sequence in notes §"What this means") | **done (code; not built, not booted)** | `Drivers/Beyond0LteUsbHostDxe/` — `UsbPhy.c` is the kernel/Mu-agreed UTMI sequence on 9820 bases (delays included); `Dwc3.c` mirrors `dwc3_core_init` HOST path with **runtime GSNPSID revision gating** (the 9820 rev is unread — no guessed branch), no DCTL CSFTRST (kernel-faithful), Xilinx PORTSC power-off WA. Review caught: the notes' "skip FORCE_VBUSVALID in host" was inference contradicted by the kernel's unconditional write — kept, since `is_not_vbus_pad=1` means the PHY must be told VBUS is valid |
| VBUS enabler: DXE HSI2C driver for the MAX77705 bus + OTG boost writes | **done (code; not built, not booted)** | `Pmic.c` — polled master transliterated from `i2c-exynos5.c`'s POLLING branch (write-then-restart-read for PMIC reads, master-idle wait between xfers); `Max77705VbusOn` = OTG_ILIM 500 mA → mode 0x4 → 2 ms → 0xE; gated on `CC_STATUS0.CCStat==SOURCE` (CCIC left DRP by bootloader). OTG_ILIM 500 mA is the non-powered-hub-safe setting |
| xHCI host controller DXE driver on the MMIO window (produce EDK2's HCD protocol; XhciDxe is PCI-bound so needs a platform wrapper or a minimal driver) | **done — decided: wrapper, not own driver** | entry calls `RegisterNonDiscoverableMmioDevice(Xhci, NonCoherent, NULL, 1, 0x10C00000, 0x8000)`; stock `XhciDxe` + `NonDiscoverablePciDeviceDxe` do the rest (NXP precedent). Own-HCD rejected: ~6k lines of ring/scheduler reimplementation |
| Wire EDK2 USB stack into `Beyond0LtePkg.dsc/.fdf` (`UsbBusDxe`, hub logic, `UsbKbDxe`, `UsbMouseDxe`) | **done (wired; build pending)** | DSC gains `UefiUsbLib` + `NonDiscoverableDeviceRegistrationLib` instances and 7 components; FDF lines replace the old commented block. Correction to the plan: current edk2 has **no `UsbHubDxe` module** — hub enumeration lives inside `UsbBusDxe` (`UsbHub.c`) |
| Build clean (host-mode USB in the FD, FD size still fits `FVMAIN_COMPACT`) | **done (Linux verification build; the flashable FD stays the Windows CLANGPDB one)** | `notes/d1-build-env.md`: full Linux recipe + gotchas. Result: build exit 0, `BEYOND0LTE_UEFI.fd` 7 340 032 B sha `83a9986e…`, all six USB FFS present (`…5548Beyond0LteUsbHostDxe`, XhciDxe, UsbBusDxe, UsbKbDxe, UsbMouseDxe, NonDiscoverablePciDeviceDxe). `FVMAIN_COMPACT` auto-sized to 417 792 B (648 B free — auto-sized FVs are always ~full; the FD has MBs of slack, so the earlier "968 B free" space hazard was **not** a blocker). gcc found two real `maybe-uninitialized` spots in my `Pmic.c` that the syntax-only pass missed — fixed in source |

### D2 — Hub + keyboard/mouse in UEFI

| Task | State | Notes |
|------|-------|-------|
| Confirm EDK2 `UsbHubDxe` handles a non-powered hub's power budget (100 mA upstream → per-port accounting) | todo | the user's hub is USB2 non-powered — the safe config |
| Keyboard reaches `ConIn` on device (type at the BDS prompt through hub) | todo | **needs-boot** — first device-visible win of phase two |
| Mouse enumerated (UEFI pointer protocol; not required for Windows, nice-to-have in UEFI) | todo | |

### D3 — USB host under Windows

| Task | State | Notes |
|------|-------|-------|
| Research: how WoA targets expose memory-mapped xHCI to Windows (ACPI `_HID`/`_CID`, `USBXHCI.sys` binding rules, Snapdragon prior art) | todo | subagent → `notes/d3-usb-windows.md` |
| Add the xHCI ACPI device to `Beyond0LtePkg/AcpiTables` (MMIO `10C00000`+0x10000, GSIV 273) | todo | |
| Decide the role-latching contract: firmware leaves DWC3 in host mode before `ExitBootServices` | todo | |
| Device test: keyboard/mouse/hub under Windows | todo | **needs-windows + needs-boot** |

### D4 — WiFi

| Task | State | Notes |
|------|-------|-------|
| Keep vendor bcmdhd closed (M12 verdict) — do not spend tasks on it | **done — decision** | no Windows driver, no DT node; reopening only on new evidence |
| Research: USB WiFi adapters with **inbox or ARM64** Windows drivers (which chips does Windows ARM64 support without vendor drivers?) | todo | subagent → `notes/d4-usb-wifi.md` |
| Pick the adapter for the donation build and record it | todo | depends on the research verdict |

### D5 — GPU / display

| Task | State | Notes |
|------|-------|-------|
| Research: Mali-G76 on Windows ARM64 — is any WDDM driver obtainable? (ARM's Windows Mali driver availability, prior art on any Exynos WoA display) | todo | subagent → `notes/d5-gpu-display.md` |
| Research: edk2-exynos DECON/display bring-up for exynos9820 (they light the S10 panel in UEFI today) | todo | same notes file |
| Verdict task: GPU acceleration on Windows is either a real driver project or a documented dead end — write the decision down | todo | honest bar: no fake `Ready` |
| Firmware GOP driver (DECON scanout at the S10e's 1440×3040) so UEFI has real graphics and Windows can use a framebuffer | todo | fb base for the FD is still `UNVERIFIED` (`Exynos9820.h`); the M8 fb-overlap task interacts |

### D6 — External display (stretch)

| Task | State | Notes |
|------|-------|-------|
| Research: DP alt mode on the 9820 combo PHY (SMV/HPD, what DeX does at `displayport@130B0000`) | todo | `notes/dock-desktop.md` §11 is the starting point |
| Decide: DP alt mode vs DisplayLink dock (DisplayLink is the funded fallback) | todo | |

## Working rules for this file

- Same ground rules as `MILESTONES.md`: one commit per task, `todo → doing → done`
  moves in the same commit, nothing device-validated until it is actually observed.
- Research tasks go to a **subagent** that appends findings (with source URLs) to
  the named notes file **as it works**; the implementation pass reads only that file.
- No MMIO constant enters code unless it is `VERIFIED` in `Exynos9820.h` or a notes
  file; anything else stays `UNVERIFIED` and named.
- The non-powered hub is a **hard requirement** of the donation build until a spare
  unit exists to test powered-hub host mode.
