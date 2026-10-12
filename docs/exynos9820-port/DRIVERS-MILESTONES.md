# Driver milestones (phase two) â€” USB host, hub, HID, WiFi, GPU/display

This file is the plan of record for the **driver work stream**, run in parallel with
the boot work stream (M8/M9 in [`MILESTONES.md`](MILESTONES.md)). It expands the
M10â€“M15 rows of that file into buildable tasks with a fixed order of attack.

Scope, in the user's own words (2026-10-10): make **keyboard and mouse work over
USB-C**, support a **non-powered USB hub** for more USB2 ports, get **WiFi**, and get
**GPU support** â€” building them one by one, using research from Linux/Android
solutions for the Galaxy S10e (`beyond0lte`, SM-G970F).

## What phase one already settled (do not re-litigate, build on it)

| Fact | Where |
|---|---|
| DWC3 core at `0x10C00000`+0x10000, **identical between 9810 and 9820**; exposes a standard xHCI register interface | `notes/dock-desktop.md` Â§4/Â§5, `Exynos9820.h:395-396` |
| USB PHY `10B00000`+0x200, combo PHY `10AE0000`+0x2800, PCS `10AF0000`+0x800 â€” all VERIFIED; `has_combo_phy=1` (USB3 + DisplayPort share the PHY) | `Exynos9820.h:397-402`, `notes/dock-desktop.md` Â§11 |
| Shipping DT says `dr_mode = "otg"` â€” host mode is a **firmware decision**, not a Windows one | `notes/dock-desktop.md` Â§8 |
| USB IRQs: DWC3 SPI 241 â†’ **GSIV 273**; PHYs SPI 277/278 | `Exynos9820.h:522-523` |
| CMU windows for USB resolved from Samsung cal-if: `CMU_PERIC0 0x10400000+0x8000`, `CMU_FSYS0A 0x10a00000+0x8000`, gates IPCLK `0x2094`/PCLK `0x2098`/RSTNSYNC `0x2054`/OSCCLK `0x2008`, QCH `0x3038` | `notes/s10e-live-reads.md` Â§7.5 |
| Mu-Silicium `UsbfnDwc3Dxe` exists but USB Host Mode is âŒ on every Exynos target â€” nobody finished it | `notes/dock-desktop.md` Â§3 |
| Our 9810 `UsbDebugHid.c` is a full DWC3 register vocabulary but programs **peripheral** mode, and is compiled off | `notes/dock-desktop.md` Â§9 |
| EDK2 USB stack (`UsbBusDxe`, `UsbKbDxe`, `UsbMassStorageDxe`) is **commented out** in both platform FDFs | `Star2LtePkg.fdf:167-169`, `Beyond0LtePkg.fdf:259-261` |
| Phone WLAN is Broadcom `bcmdhd` with no Windows driver and no DT device node â€” vendor-WiFi on Windows is a **dead end**; USB WiFi adapter is the fallback | M12 in `MILESTONES.md`, `notes/dock-desktop.md` Â§6 |
| **Safety:** powered hub + host mode "can irreversibly damage your device"; the user's hub is **non-powered**, which is the safe configuration | `MILESTONES.md` rule 7, woa-mh2lm5g materials.md |
| One USB-C connector: host mode and charging compete for the same port | `notes/dock-desktop.md` Â§8 |

## The two halves of every USB task

USB host on this platform has **two independent halves**, and they are verified at
different times:

1. **Firmware half (UEFI):** a DWC3 host-mode driver inside `BEYOND0LTE_UEFI.fd` +
   the EDK2 USB stack. This is testable **as soon as UEFI boots** â€” a keyboard
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
| D1 | DWC3 **host mode** in UEFI (the gate for everything else) | **doing** | research, code, wiring and a clean Linux verification build **done** (`BEYOND0LTE_UEFI.fd` sha `83a9986eâ€¦` carries the whole chain); device validation waits on the boot stream â€” and on the open unknowns in `notes/d1-usb-host.md` (clock state at DXE entry, GSNPSID rev, CC attach timing) |
| D2 | USB hub (non-powered) + keyboard/mouse in UEFI (`ConIn`) | todo | D1 |
| D3 | USB host under **Windows**: ACPI xHCI device, inbox `USBXHCI.sys` | **doing** | D1, boot stream â€” DSDT `XHC0` device + role-latch decision **done** (built into the FD, `PNP0D10` verified in FVMAIN); device test waits on Windows |
| D4 | WiFi: USB adapter path chosen + validated (vendor bcmdhd stays closed) | **doing** | research + choice **done** (`notes/d4-usb-wifi.md`): no USB WiFi driver exists for ARM64 â€” decision is PicoPiFi (Pico W, driverless RNDIS) as the WiFi option, dock ethernet (M13) stays primary; device validation rides on D1/D3 |
| D5 | GPU/display: framebuffer deliverable decided; Mali WDDM reopened as D5b | **doing** | research + verdict **done** (`notes/d5-gpu-display.md` + addendum): shipped-code deliverable is GOP + in-box BasicDisplay; acceleration reopened after the S9 WDDM report â€” kbase r10p0 source is already in the 9820 kernel, feasibility inventory (D5b) is next; fb handoff validation still **needs-boot** |
| D6 | External display (DP alt mode over the combo PHY) â€” stretch | **doing** | research + hub ID + decision **done** (`notes/d6-external-display.md`): user's UGREEN HDMI = DP alt mode (no DisplayLink chip) -> dark under Windows until a display-only WDDM exists (D5b rung 1); DisplayLink adapter is the buy-it-now alternative; DeX-on-Android validation recorded |
| D8a | Audio milestone 1: read the CS47L93 / CS35L40 IDs from our UEFI; milestone 2: codec tone on the jack; milestone 3: tone on the loudspeaker (amps via ABOX UAIF1) | **doing** | m1/m1b/m2 device-validated (m2 heard by the user 2026-10-12); m3 research running (`notes/d8-audio-m3-abox.md`, `notes/d8-audio-m3-amp.md`). User requirements: speakers are the default output, the jack only when headphones are plugged in; **the volume buttons change the volume** (BTNS/Hidinterrupt already sends volume keys - needs an endpoint with a volume control) |

## Task list

### D1 â€” DWC3 host mode in UEFI

| Task | State | Notes |
|------|-------|-------|
| Research: how Linux/Android on the S10e brings up DWC3 (exynos-dwc3 glue, USBFN, PHY init order, VBUS/ID role) | **done** | `notes/d1-usb-host.md` Q1/Q2: full chain pinned from `LineageOS/android_kernel_samsung_exynos9820` â€” glue `dwc3-exynos.c`, PHY `phy-exynos-usbdrd.c` (PMU de-isolate `0x1586072C` bit0 / DP `0x15860704`), USBCON reg map, `dwc3_core_init` HOST path with absolute offsets (GCTL `0x10C00C110`, PRTCAPDIR=1<<12), xHCI MMIO = same window +0x0..0x7fff. **Trap: USB clock gates are ACPM-owned vclk/QCH, not CMU MMIO** â€” strategy is audit-then-rely-on-sboot-state |
| Research: Mu-Silicium `UsbfnDwc3Dxe` + edk2-exynos 9820 USB prior art â€” what exists, what is broken | **done** | `notes/d1-usb-host.md` Q3/Q4: `UsbfnDwc3Dxe` is **device-mode only** (produces EFI_USB_FUNCTION_IO), Host âŒ on every Samsung target because 9820/9810 use the NULL `PlatformUsbfnLib`; but `S5E9830Pkg/Library/PlatformUsbfnLib/BoardInit.c` + `PlatformUsbRegisters.h` are a directly reusable UEFI-native PHY bring-up (same PMU offsets/bits; **9830 block bases differ â€” do not paste**). edk2-exynos has **zero** USB controller code. VBUS = MAX77705 boost via I2C (`CHG_CNFG_00` OTG modes, `CHG_CNFG_02` OTG_ILIM 500 mAâ€“1.5 A); SoC has no VBUS pad, no OVC input. Windows half: inbox `usbxhci.inf` binds `ACPI\PNP0D10` â€” D3 is an ACPI table task, not a driver task |
| Research follow-up: hsi2c_45 base + MAX77705 CC-role (Rp sourcing) init + UEFI xHCI-on-MMIO binding prior art (edk2 XhciDxe is PCI-bound) | **done** | `notes/d1b-usb-vbus-hcd.md`: `hsi2c_45 = 0x15D60000`+0x1000 (live on device, clocks ACPM-owned like USB â€” same leave-alone strategy), polled master sequence pinned from `i2c-exynos5.c` and already implemented in our Windows HSI2C driver; MAX77705 sub-addresses charger **0x69** / CCIC **0x25**, VBUS = `CNFG_00` mode 0x4â†’1 msâ†’0xE, `CNFG_02` OTG_ILIM; CCIC is firmware-loaded and already DRP (CCCTRL1=0x19) so a dumb hub attaches us as source â€” just poll `CC_STATUS0.CCStat==2`; **UEFI verdict: `RegisterNonDiscoverableMmioDevice(Xhci, NonCoherent, init-cb, 0x10C00000, 0x8000)` + `NonDiscoverablePciDeviceDxe` + stock `XhciDxe`** (NXP Layerscape is the exact precedent; XhciDxe polls, no IRQ needed) |
| Add the USB host hardware constants to the platform (USBCON/DWC3-global register map, PMU isolation offsets) | **done** | `Drivers/Beyond0LteUsbHostDxe/UsbHw.h` â€” every bit from the LineageOS kernel headers or Mu's 9830 map, cited per line; `Exynos9820.h` gains `EXYNOS_PMIC_HSI2C_*` (0x15D60000, DT + live node) and `EXYNOS_USBDRD_GSIV 273`; memory map gains the HSI2C45 entry. Three transcription errors caught in review (FIFO enables bits 0/1 not 29/30, USI_RESET bit0 not 13, FS timing 0xF0/0xE0 not 0x0F/0x1F) and the `utmi_clk` DT tune entry proven IGNORED by the kernel parser â€” HSP_TUNE is now a masked RMW pair. Syntax-checked against real EDK2 headers (`HEADER_SYNTAX_OK`) |
| Write `Beyond0LteUsbHostDxe`: PHY de-isolation + USBCON USB2 init + DWC3 core HOST-path init (the 7-step sequence in notes Â§"What this means") | **done (code; not built, not booted)** | `Drivers/Beyond0LteUsbHostDxe/` â€” `UsbPhy.c` is the kernel/Mu-agreed UTMI sequence on 9820 bases (delays included); `Dwc3.c` mirrors `dwc3_core_init` HOST path with **runtime GSNPSID revision gating** (the 9820 rev is unread â€” no guessed branch), no DCTL CSFTRST (kernel-faithful), Xilinx PORTSC power-off WA. Review caught: the notes' "skip FORCE_VBUSVALID in host" was inference contradicted by the kernel's unconditional write â€” kept, since `is_not_vbus_pad=1` means the PHY must be told VBUS is valid |
| VBUS enabler: DXE HSI2C driver for the MAX77705 bus + OTG boost writes | **done (code; not built, not booted)** | `Pmic.c` â€” polled master transliterated from `i2c-exynos5.c`'s POLLING branch (write-then-restart-read for PMIC reads, master-idle wait between xfers); `Max77705VbusOn` = OTG_ILIM 500 mA â†’ mode 0x4 â†’ 2 ms â†’ 0xE; gated on `CC_STATUS0.CCStat==SOURCE` (CCIC left DRP by bootloader). OTG_ILIM 500 mA is the non-powered-hub-safe setting |
| xHCI host controller DXE driver on the MMIO window (produce EDK2's HCD protocol; XhciDxe is PCI-bound so needs a platform wrapper or a minimal driver) | **done â€” decided: wrapper, not own driver** | entry calls `RegisterNonDiscoverableMmioDevice(Xhci, NonCoherent, NULL, 1, 0x10C00000, 0x8000)`; stock `XhciDxe` + `NonDiscoverablePciDeviceDxe` do the rest (NXP precedent). Own-HCD rejected: ~6k lines of ring/scheduler reimplementation |
| Wire EDK2 USB stack into `Beyond0LtePkg.dsc/.fdf` (`UsbBusDxe`, hub logic, `UsbKbDxe`, `UsbMouseDxe`) | **done (wired; build pending)** | DSC gains `UefiUsbLib` + `NonDiscoverableDeviceRegistrationLib` instances and 7 components; FDF lines replace the old commented block. Correction to the plan: current edk2 has **no `UsbHubDxe` module** â€” hub enumeration lives inside `UsbBusDxe` (`UsbHub.c`) |
| Build clean (host-mode USB in the FD, FD size still fits `FVMAIN_COMPACT`) | **done (Linux verification build; the flashable FD stays the Windows CLANGPDB one)** | `notes/d1-build-env.md`: full Linux recipe + gotchas. Result: build exit 0, `BEYOND0LTE_UEFI.fd` 7 340 032 B sha `83a9986eâ€¦`, all six USB FFS present (`â€¦5548Beyond0LteUsbHostDxe`, XhciDxe, UsbBusDxe, UsbKbDxe, UsbMouseDxe, NonDiscoverablePciDeviceDxe). `FVMAIN_COMPACT` auto-sized to 417 792 B (648 B free â€” auto-sized FVs are always ~full; the FD has MBs of slack, so the earlier "968 B free" space hazard was **not** a blocker). gcc found two real `maybe-uninitialized` spots in my `Pmic.c` that the syntax-only pass missed â€” fixed in source |

### D2 â€” Hub + keyboard/mouse in UEFI

| Task | State | Notes |
|------|-------|-------|
| Confirm EDK2 `UsbHubDxe` handles a non-powered hub's power budget (100 mA upstream â†’ per-port accounting) | todo | the user's hub is USB2 non-powered â€” the safe config |
| Keyboard reaches `ConIn` on device (type at the BDS prompt through hub) | todo | **needs-boot** â€” first device-visible win of phase two |
| Mouse enumerated (UEFI pointer protocol; not required for Windows, nice-to-have in UEFI) | todo | |

### D3 â€” USB host under Windows

| Task | State | Notes |
|------|-------|-------|
| Research: how WoA targets expose memory-mapped xHCI to Windows (ACPI `_HID`/`_CID`, `USBXHCI.sys` binding rules, Snapdragon prior art) | **done** | closed by `notes/d1b-usb-vbus-hcd.md` Q6: inbox `usbxhci.inf` matches `ACPI\PNP0D10`/`PNP0D15`; Marvell Armada70x0 EDK2 DSDT is the working memory-mapped pattern |
| Add the xHCI ACPI device to `Beyond0LtePkg/AcpiTables` (MMIO `10C00000`+0x8000, GSIV 273) | **done (code; built into the FD; Windows enumeration unvalidated)** | `Dsdt.asl` `Device (XHC0)`: `_HID "PNP0D10"`, `_CCA 1`, `Memory32Fixed(0x10C00000, 0x8000)` (xHCI window only â€” the DWC3 globals at +0xC100 are deliberately NOT in `_CRS` so USBXHCI never re-touches them), `Interrupt(Level, ActiveHigh, Exclusive){273}`. Verified `PNP0D10` present in the built `FVMAIN.Fv` |
| Decide the role-latching contract: firmware leaves DWC3 in host mode before `ExitBootServices` | **done â€” decision** | the core's PRTCAP/GCTL state is hardware latched; nothing in Windows' USBXHCI path writes DWC3 globals as long as they stay out of `_CRS` (enforced by the XHC0 design above). Residual risk recorded: a Windows filter or the SoC's own power framework poking 0x10C0xxxx is untested |
| Device test: keyboard/mouse/hub under Windows | todo | **needs-windows + needs-boot** |

### D4 â€” WiFi

| Task | State | Notes |
|------|-------|-------|
| Keep vendor bcmdhd closed (M12 verdict) â€” do not spend tasks on it | **done â€” decision** | no Windows driver, no DT node; reopening only on new evidence |
| Research: USB WiFi adapters with **inbox or ARM64** Windows drivers (which chips does Windows ARM64 support without vendor drivers?) | **done** | `notes/d4-usb-wifi.md` (342 lines): **none exist.** Inbox = zero USB WLAN on any arch for ARM64 (Microsoft's own FOD table); Microsoft Update Catalog chip-by-chip scan = Realtek USB WiFi (1030.x bundle + Wi-Fi 6 5001.x), AR9271, Ralink, MTK all x86/x64-only; positive ARM64 controls (Realtek GbE, ASIX) prove it is vendor choice, not a WU limitation â€” and that same control is primary-source support for the M13 LAN-first plan |
| Pick the adapter for the donation build and record it | **done â€” decision** | **No retail USB WiFi dongle.** WiFi option = **Raspberry Pi Pico W + PicoPiFi** (~$7, WiFiâ†’RNDIS bridge: driverless inbox USB ethernet class, community-confirmed on WoA, ~40-60 mA so it fits the non-powered hub budget next to keyboard/mouse; 2.4 GHz only â€” stated limitation). Primary network path stays the dock's USB ethernet (M13). A community ARM64 WLAN miniport would need test-signing â€” disqualified for a donation build by rule |

### D5 â€” GPU / display

| Task | State | Notes |
|------|-------|-------|
| Research: Mali-G76 on Windows ARM64 â€” is any WDDM driver obtainable? (ARM's Windows Mali driver availability, prior art on any Exynos WoA display) | **done** | `notes/d5-gpu-display.md`: **no Mali WDDM exists or ever shipped** â€” Update Catalog zero hits, Arm's downloads are Linux/Android-only, Mu-Silicium GPU âŒ on every Samsung target, and no Snapdragon ever used Mali so there is no ecosystem to piggyback |
| Research: edk2-exynos DECON/display bring-up for exynos9820 (they light the S10 panel in UEFI today) | **done** | Mu's whole Samsung display story is `SimpleFbDxe` over a bootloader-lit fb â€” structurally identical to our `SimpleFramebufferDxe`; edk2-exynos has no extra display code to mine. Bonus read: the 9820 GPU is `mali@18500000` with ARM's open Bifrost kbase `b_r10p0` (max 702 MHz) in the LineageOS kernel |
| Verdict task: GPU acceleration on Windows is either a real driver project or a documented dead end â€” write the decision down | **reopened 2026-10-10** | original verdict (no Mali WDDM exists â†’ framebuffer display is the deliverable, `notes/d5-gpu-display.md`) stands **for shipped code**, but the user reports the S9 WoA author has **WDDM running** (unreleased â€” unverified), and the addendum records the real ladder: (1) DECON display-only WDDM, (2) open kbase `b_r10p0` (already in the 9820 kernel source!) â†’ DXGK + **panfrost/Mesa** userspace, (3) closed DDK port = dead end (Arm EULA no-RE) |
| D5b: Mali/WDDM feasibility inventory | todo | (a) size the kbase r10p0 source in the LineageOS 9820 kernel (files, DRM interfaces, GPL terms); (b) locate the G76 firmware blob (vendor image / ko-embedded) â€” a device read; (c) **needs-user**: what exactly did the S9 person get running â€” display-only miniport or full GPU? any link/channel; (d) check panfrost-on-Mesa-Windows-ARM64 build status |
| Firmware display follow-ups (re-scoped from "GOP driver"): validate the sboot fb survives into Windows, close the fb RAM/DEV overlap (M8 task), DECON blank/backlight sanity at handoff | todo | the UEFI half already exists; what is unproven is **handoff survival** â€” does the fb/DECON state our shim inherits still be drawing at `ExitBootServices`, and does Windows' BasicDisplay keep it â€” **needs-boot** |

### D6 â€” External display (stretch)

| Task | State | Notes |
|------|-------|-------|
| Identify what the user's hub's HDMI actually is (DP alt mode vs DisplayLink) | **done â€” from live descriptors** | UGREEN hub on this PC right now: Genesys Logic GL352 hub chips + **ASIX AX88179B** GbE, **no `0x17e9` DisplayLink device** -> HDMI is **DP alt mode**. Measured worst-case draw 482 mA fits the 500 mA budget. `notes/d6-external-display.md` |
| Research: DP alt mode on the 9820 combo PHY (SMV/HPD, what DeX does at `displayport@130B0000`) | **done** | `notes/d6-external-display.md` P1-P7: full kernel chain pinned (`displayport_drv.c` in dpu20, max77705 `max77705_alternate.c` PD state machine, three board GPIOs incl. gpg0-4 PHY_CON, `LANE_MUX_SEL_DP=0xc`, PMU 0x15860704 gates the PHY, RBR/HBR/HBR2 only); DECON_T confirmed `0x19050000` `out_type=2`(DP) with SPIs 195-197 + SYSMMUs; **no UEFI prior art exists** (edk2-exynos has none); DisplayLink driver 12.3 (Sep 2026) = WHQL + Win11 ARM64 |
| Decide: DP alt mode vs DisplayLink dock (DisplayLink is the funded fallback) | **done â€” decision** | For THIS hub: HDMI is DP-alt, which under Windows needs our own display-only WDDM (D5b rung 1 + DP/PHY/CCIC halves) â€” **not funded now**. If an external display is wanted before that, a DisplayLink adapter/dock (~$50-150) works with the WHQL ARM64 driver on top of D1 alone. Zero-risk hardware validation first: DeX on Android with this hub + monitor |

### D7 â€” CPU driver (all three core types)

| Task | State | Notes |
|------|-------|-------|
| Research the DVFS/clock/power mechanism from the Android 12 ODIN dump | **done** | `notes/cpu-driver-9820.md` F1â€“F11. The 9820 does NOT DVFS via CMU MMIO: CPU clusters are driven by the **ACPM coprocessor** â€” IPC mailbox `0x15900000` (doorbell `INTGR0=(1<<ch)<<16`), rings in ACPM SRAM `0x2039000+0x30000` (framework @+0x7000), 4Ã—u32 message `{idx|seq<<16, kHz, FREQ_REQ=0/FREQ_GET=1, 0}` on **channel 5**, idx 2/3/4 = LITTLE/MID/BIG. OPP+voltages are plain memory (fvmap, plugin base + `fvmap_offset 0xa400`). Protocol verified in the shipped kernel binary (F11). IRQ SPI 40 (GSIV 72) |
| Research PSCI vs the ACPI parking protocol for Windows core bring-up (WoA prior art + Samsung monitor) | **done** | `notes/psci-parking.md` Â§Aâ€“C. Shipped 9820 monitor = PSCI-0.2 IDs over SMC under a bare `arm,psci` DT binding â€” the documented Samsung pattern (Exynos7420 downgrade commit `e1e47fbca668`: SYSTEM_OFF/RESET absent). Renegade/Surface Duo model (FADT PSCI+SMC, GICC PPV=0) is the design to copy; the **ACPI parking protocol** exists in Windows' world (Lumia 950/8992 stock WP10 PPV=1 tables boot desktop Windows, though PSCI is also declared there so the active mechanism is not isolated â€” see notes Â§C6.3) = candidate EL3-independent Plan B; UEFI in no port ever onlines APs â€” Windows does. Open: probe PSCI_VERSION/FEATURES/AFFINITY from our own UEFI (C5) before trusting 0.2+; MIDR 0xd0a-vs-0xd09 conflict flagged (C4) |
| Close the shipped OPP+voltage ladder values | **todo â€” one safe read** | pstore `console-ramoops` from TWRP (kernel prints `CPUFREQ of domainâ€¦` at boot) or 143 KB `by-name/ACPM` dump; both read-only, F10 |
| Design doc: Windows CPU driver (KMDF, polling-mode ACPM IPC, fvmap read at init, thermal clamp from DT trips) | **todo** | one uniform protocol covers all three core types â€” the M4/prometheus/ananke differences are hidden inside the ACPM; PPTT performance ranks from F5 (272/531/785 DMIPS) |
| Implement + device-validate | **todo** | bar: Windows can set/get each cluster's frequency; nothing is Ready until observed |

### D8 audio â€” milestone 1: talk to the audio chips (`notes/d8-audio-m1.md`)

Bar: our UEFI reads the CS47L93 codec ID (spi_2, expect 0x6371) and both CS35L40 amp DEVIDs
(hsi2c_38 @0x40/0x41, expect 0x35A40/0x35B40) and logs them to UEFILOG. Not done until the
UEFILOG of a real boot shows the reads.

| Task | State | Notes |
|------|-------|-------|
| Pin bus/pin/reset/ID facts for codec + amps from the vendor sources | **done** | `notes/d8-audio-m1.md` Â§1 (file:line cites, lineage-22.2 + cal-if) |
| Port the origin/drivers D8 constants (f22718a: ALIVE banks/EINT, key GSIVs, ABOX windows) to `Exynos9820.h` | **done** | cherry-picked (header only, applied clean; no duplicate #defines - checked with grep) |
| Add the audio-bus constants (spi_2, hsi2c_38, USI mode regs, PERIC0/1 pin banks, reset pins, CMU diag offsets) to `Exynos9820.h` | **done** | all from Â§1, VERIFIED tags with cites; INTREQ__USI17_I2C 428 re-read from the dt-bindings header |
| Map the new MMIO windows in `PlatformMemoryMapLib` (SPI2, PERIC0/1 USI pages, PERIC1 GPIO, HSI2C38, CMU_PERIC1) | **done** | device memory only; platform build exit 0 (FD 7 340 032 B) |
| `Beyond0LteAudioProbeDxe`: polled SPI + HSI2C ID reads, save/restore of every touched register, progress guard 0x69, flag `BEYOND0LTE_AUDIO_PROBE` | **done (code + build; device run pending)** | build exit 0, module `...555554 Beyond0LteAudioProbeDxe` in `Guid.xref`; FD sha256 `67f009abâ€¦` |
| Log the codec rails (S2MPS19 LDO17M/LDO18M CTRL) in `Beyond0LteTouchPowerDxe`'s read pass | **done (code + build)** | DCVDD = LDO18M is not boot-on in the vendor DT; read-only PmicRead of 0x56/0x57 at ReadyToBoot, build exit 0 |
| Device test: UEFILOG shows codec/amp IDs | **ran (boot #88) â€” bar not met** | amps NACK (resets never released: probe rule bug), codec ID 0 (DCVDD = LDO18M off) -> milestone 1b below |
| Milestone 2 groundwork: smallest audible path (codec + 3.5 mm jack) â€” exact task list | **done â€” design** | `notes/d8-audio-m1.md` Â§4: codec tone generator -> OUT2 -> HPOUT2 -> jack needs no ABOX and no Windows driver (registers pinned); portcls outline; 7-task list |

### D8 audio â€” milestone 1b: codec DCVDD on, amps out of reset (`notes/d8-audio-m1.md` Â§5)

Bar: the same UEFILOG lines as milestone 1, now with `CS47L92/93 ID OK` and both `CS35L40 ID OK`.

| Task | State | Notes |
|------|-------|-------|
| Pin LDO18M voltage/enable, madera power-up order, CS35L40 reset/supplies from the vendor sources + stock dtbo | **done** | Â§5.1: vsel 0x28 = 1.200 V (group 3: 700 mV + n x 12.5 mV), enable 0xC0/0xC0, 5 ms ramp; amps: no supply props (fixed rails), NACK = reset never released (probe rule bug) |
| AudioProbeDxe: amp reset release when the pin is not driven high (vendor low -> 1 ms -> high -> 2 ms) | **done (code + build)** | build exit 0; pins restored to handoff afterwards (amps back in reset) |
| TouchPowerDxe: LDO18M on (flag `BEYOND0LTE_AUDIO_DCVDD`, vsel 0x28 check, 5 ms ramp), signal the codec-power event group | **done (code + build)** | **PMIC write** (0x57 enable 0xC0/0xC0, refused unless vsel 0x28); build exit 0 |
| AudioProbeDxe: codec reset asserted at entry, codec probe deferred to the event group, BOOT_DONE poll | **done (code + build)** | build exit 0, FD sha256 `645c8328â€¦`; codec reset restored to handoff afterwards |
| Device test: UEFILOG shows codec + amp IDs | **done (device-validated, boot #92)** | `notes/d8-audio-m1.md` "RESULT milestone 1b": codec 6371 HW_REV 1, both amps 35A40 |

### D8 audio â€” milestone 2: codec tone on the 3.5 mm jack (`notes/d8-audio-m1.md` Â§6)

Bar: UEFILOG proves FLL1 lock, SYSCLK on, tone -> OUT2 mixers, HP2 enable done, no short; the user hears a
~1 kHz tone for ~2 s in wired headphones at boot (flag `BEYOND0LTE_AUDIO_TONE`, off once validated).

| Task | State | Notes |
|------|-------|-------|
| Research: jack output (OUT1 vs OUT2), SYSCLK source without AP clocks, FLLHJ values, HP enable sequence, patch | **done** | Â§6.1: HEADPHONE <- HPOUT2L/R (stock dtbo routing); FLL1 <- MCLK2 32 kHz (N 375, FB_DIV 4), fallback MCLK1 26 MHz; cs47l92 patch verbatim |
| AudioProbeDxe: codec SPI write (16-bit + 32-bit regmap) and 32-bit read helpers | **done (code + build)** | `AudioCodec.h`: CodecWrite / CodecUpdate / CodecRead32 / CodecWrite32; build exit 0 (private workspace w8c) |
| AudioProbeDxe: tone sequence (patch, 32k clk, FLL1, SYSCLK, tone1 -> OUT2 -> HPOUT2 2 s, teardown), flag `BEYOND0LTE_AUDIO_TONE` | **done (code + build)** | `AudioTone.c`; build exit 0, FD sha256 `4f3f0c9bâ€¦`; SPI writes to the codec only, no AP clock/PMIC/ABOX write |
| Device test: UEFILOG lock/enable lines + tone heard | **done (device-validated)** | boot #93 UEFILOG (FLL1 lock 3 ms, OUT_EN1 C, SC 0; HP2 EN_DONE read 0) + **the user heard the beep** in wired headphones 2026-10-12 (pitch "somewhere in the middle") |

### D8 audio â€” Windows driver M1: loudspeaker render endpoint + volume (`notes/d8-audio-win-m1.md`)

Bar: Windows shows a "Speaker" endpoint bound to `ACPI\SAMS9827`, the volume keys and slider move its volume,
streams run (timer clock) and the run log shows the ABOX audit + amp IDs; sound itself waits on the stage-1/2
sequences from the UEFI m3 work, which plug into `AboxHw.c` / `AmpI2c.c`.

| Task | State | Notes |
|------|-------|-------|
| A1: `drivers/Exynos9820Audio` ACX 1.0 skeleton: render circuit `Speaker0`, 48 kHz 16/32-bit stereo, contiguous cached ring + clean, timer clock, run log, ABOX/amp layers with stage TODO stubs | **done (code + build; device run pending)** | build exit 0 /W4 /WX; ACX 1.0 because the phone's 22621 image ships only `Acx01000.sys`; no volume node by default (audiodg software volume = keys work) |
| B: port the boot #99 validated speaker sequence (`AboxHw.c`, `AmpI2c.c`) + 4 x 32-bit DMA ring expanded from the stereo packets, RDMA frame-counter clock (`Render.c`) | **done (code + build; device run pending)** | B1/B2/B3 commits; order RDMA-with-data before amp power-up; mono on the loudspeaker by default (`Parameters\Stereo`) |
| A2: DSDT `Device (AUD0)` `SAMS9827` with the eight-window `_CRS` | todo | |
| A3: INF (Class MEDIA, KS interfaces) + test-signed package `s10e-artifacts\audio-a1` | todo | |

## Working rules for this file

- Same ground rules as `MILESTONES.md`: one commit per task, `todo â†’ doing â†’ done`
  moves in the same commit, nothing device-validated until it is actually observed.
- Research tasks go to a **subagent** that appends findings (with source URLs) to
  the named notes file **as it works**; the implementation pass reads only that file.
- No MMIO constant enters code unless it is `VERIFIED` in `Exynos9820.h` or a notes
  file; anything else stays `UNVERIFIED` and named.
- The non-powered hub is a **hard requirement** of the donation build until a spare
  unit exists to test powered-hub host mode.
