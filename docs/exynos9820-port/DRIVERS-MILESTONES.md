# Driver milestones (phase two) — USB host, hub, HID, WiFi, GPU/display, audio + buttons

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
| Phone WLAN is Broadcom **BCM4375 on PCIe** (`pcie_0@13ED0000`, Gen1 x1, INTx) — the old "no DT device node = dead end" reading was about the platform glue only and is **corrected 2026-10-11**; still no Windows driver exists for it, so it is a driver project (D4b), not a bind | `notes/m12-wifi-chip.md`, M12 in `MILESTONES.md` |
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
| D1 | DWC3 **host mode** in UEFI (the gate for everything else) | **doing** | research, code, wiring and a clean Linux verification build **done** (`BEYOND0LTE_UEFI.fd` sha `83a9986e…` carries the whole chain); device validation waits on the boot stream — and on the open unknowns in `notes/d1-usb-host.md` (clock state at DXE entry, GSNPSID rev, CC attach timing) |
| D2 | USB hub (non-powered) + keyboard/mouse in UEFI (`ConIn`) | todo | D1 |
| D3 | USB host under **Windows**: ACPI xHCI device, inbox `USBXHCI.sys` | **doing** | D1, boot stream — DSDT `XHC0` device + role-latch decision **done** (built into the FD, `PNP0D10` verified in FVMAIN); device test waits on Windows |
| D4 | WiFi: USB adapter path chosen + validated (external path) | **doing** | research + choice **done** (`notes/d4-usb-wifi.md`): no USB WiFi driver exists for ARM64 — decision is PicoPiFi (Pico W, driverless RNDIS) as the WiFi option, dock ethernet (M13) stays primary; device validation rides on D1/D3 |
| D4b | WiFi: the phone's own **BCM4375 on PCIe** — reopened by user directive 2026-10-11 | **doing** | research **done** (`notes/m12-wifi-chip.md`): chip/bus/IRQ/clocks/bring-up sequence all pinned from the vendor kernel; design/code tasks below; device rungs (link-up, firmware-up) ride on the boot stream |
| D5 | GPU/display: framebuffer deliverable decided; Mali WDDM reopened as D5b | **doing** | research + verdict **done** (`notes/d5-gpu-display.md` + addendum): shipped-code deliverable is GOP + in-box BasicDisplay; acceleration reopened after the S9 WDDM report — kbase r10p0 source is already in the 9820 kernel, feasibility inventory (D5b) is next; fb handoff validation still **needs-boot** |
| D6 | External display (DP alt mode over the combo PHY) — stretch | **doing** | research + hub ID + decision **done** (`notes/d6-external-display.md`): user's UGREEN HDMI = DP alt mode (no DisplayLink chip) -> dark under Windows until a display-only WDDM exists (D5b rung 1); DisplayLink adapter is the buy-it-now alternative; DeX-on-Android validation recorded |
| D8 | Audio (speaker/mic/volume) + hardware buttons | **doing** | research + constants + DSDT + **buttons driver code** all done: ABOX/CS47L93/CS35L40 chain identified and designed (`notes/d8-audio-design.md`); buttons = `Exynos9820Gpio` (GpioClx, code not built) + ACPI0011 in FD (inbox Hidinterrupt.sys). Device validation of both waits on Windows booting; audio rung 1 gated on the SPUS/SPUM experiment |

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
| Research: how WoA targets expose memory-mapped xHCI to Windows (ACPI `_HID`/`_CID`, `USBXHCI.sys` binding rules, Snapdragon prior art) | **done** | closed by `notes/d1b-usb-vbus-hcd.md` Q6: inbox `usbxhci.inf` matches `ACPI\PNP0D10`/`PNP0D15`; Marvell Armada70x0 EDK2 DSDT is the working memory-mapped pattern |
| Add the xHCI ACPI device to `Beyond0LtePkg/AcpiTables` (MMIO `10C00000`+0x8000, GSIV 273) | **done (code; built into the FD; Windows enumeration unvalidated)** | `Dsdt.asl` `Device (XHC0)`: `_HID "PNP0D10"`, `_CCA 1`, `Memory32Fixed(0x10C00000, 0x8000)` (xHCI window only — the DWC3 globals at +0xC100 are deliberately NOT in `_CRS` so USBXHCI never re-touches them), `Interrupt(Level, ActiveHigh, Exclusive){273}`. Verified `PNP0D10` present in the built `FVMAIN.Fv` |
| Decide the role-latching contract: firmware leaves DWC3 in host mode before `ExitBootServices` | **done — decision** | the core's PRTCAP/GCTL state is hardware latched; nothing in Windows' USBXHCI path writes DWC3 globals as long as they stay out of `_CRS` (enforced by the XHC0 design above). Residual risk recorded: a Windows filter or the SoC's own power framework poking 0x10C0xxxx is untested |
| Device test: keyboard/mouse/hub under Windows | todo | **needs-windows + needs-boot** |

### D4 — WiFi

| Task | State | Notes |
|------|-------|-------|
| Keep vendor bcmdhd closed (M12 verdict) — do not spend tasks on it | **done — decision** | no Windows driver, no DT node; reopening only on new evidence |
| Research: USB WiFi adapters with **inbox or ARM64** Windows drivers (which chips does Windows ARM64 support without vendor drivers?) | **done** | `notes/d4-usb-wifi.md` (342 lines): **none exist.** Inbox = zero USB WLAN on any arch for ARM64 (Microsoft's own FOD table); Microsoft Update Catalog chip-by-chip scan = Realtek USB WiFi (1030.x bundle + Wi-Fi 6 5001.x), AR9271, Ralink, MTK all x86/x64-only; positive ARM64 controls (Realtek GbE, ASIX) prove it is vendor choice, not a WU limitation — and that same control is primary-source support for the M13 LAN-first plan |
| Pick the adapter for the donation build and record it | **done — decision** | **No retail USB WiFi dongle.** WiFi option = **Raspberry Pi Pico W + PicoPiFi** (~$7, WiFi→RNDIS bridge: driverless inbox USB ethernet class, community-confirmed on WoA, ~40-60 mA so it fits the non-powered hub budget next to keyboard/mouse; 2.4 GHz only — stated limitation). Primary network path stays the dock's USB ethernet (M13). A community ARM64 WLAN miniport would need test-signing — disqualified for a donation build by rule |

### D4b — WiFi: the phone's own BCM4375 (reopened)

| Task | State | Notes |
|------|-------|-------|
| Research: identify chip + bus + controller map + bring-up + clocks + firmware + prior art | **done** | `notes/m12-wifi-chip.md` W1-W8: BCM4375 on `pcie_0@13ED0000` (ELBI/PHY/PCS/DBI/IA/config/MEM windows VERIFIED from vendor DT), GSIV 285, Gen1 x1 INTx, RAM base 0x170000, full power-on/link-up sequence from `pcie-exynos-host-v0.c`, CMU_FSYS1 gate offsets from cal-if, no linux-firmware blob (device blobs only), no Windows driver on any arch |
| Constants: PCIe0 windows, GSIV, CMU_FSYS1 gates, PMU PHY control, WLAN GPIOs into `Exynos9820.h` | **done (built)** | all seven pcie_0 windows + MEM/CFG iATU + GSIV 285 + CMU_FSYS1 gate/QCH offsets + PMU 0x71C + the three GPIO blocks (gpf2→FSYS1@13C40000 NEW, gpg0→PERIC0, gpa0→ALIVE) + BCM4375 chip constants; `BCM4375_EXPECTED_PCI_ID 0x4475` stays **UNVERIFIED** until a live config-space read; memory map gains 7 PCIE0/FSYS1 entries; Linux verification build exit 0, FVMAIN_COMPACT 3440 B free |
| Design doc: two-halves architecture + risk ladder | **done** | `docs/exynos9820-port/M12-WIFI-DESIGN.md`: **ACPI-described device chosen over PCI enumeration** (single 4 KiB CFG0 iATU makes a 2-bus ECAM contract the hard part; the XHC0/PNP0D10 pattern is the proven one); 11-step UEFI bring-up; 5-layer Windows miniport; license posture (brcmfmac ISC primary, bcmdhd as reference doc); blob policy (device-copied, never committed); P1 (9820 PHY ops) closed same pass — `pci-exynos9820_cal.c` |
| Firmware half: `Beyond0LtePcieDxe` — power-on + link-up + iATU + EP BAR0 in UEFI | **done (code; built into the FD; not booted)** | transliteration of `pcie-exynos-host-v0.c` poweron/establish_link + `pci-exynos9820_cal.c` ch0 PHY (CMN/TRSV tables verbatim) + I/A microprogram; deliberately **no EDK2 PCI root bridge / ECAM** (design §2 decision: ACPI-described Windows half); prints `[PCIe] R1: endpoint vid:did` = the P2 read. Linux verification build exit 0, FFS `…5549Beyond0LtePcieDxe` present, FD 7 340 032 B sha `141939da…`; deviations recorded in-file (no OTP retune, no SysMMU, direct CMU gates, RC VID/DID not rewritten) |
| Windows half: `drivers/Exynos9820Wlan/` NDIS miniport skeleton — PCI bind, BAR0 backplane window, chip identity, firmware download, common ring | todo | port of bcmdhd/brcmfmac PCIe protocol; license posture recorded in design doc |
| Device rungs: R1 link-up, R2 firmware-up (UEFI), R3 Windows enumeration, R4 scan | todo | **needs-boot** — each rung is a device observation, nothing Ready before it |

### D5 — GPU / display

| Task | State | Notes |
|------|-------|-------|
| Research: Mali-G76 on Windows ARM64 — is any WDDM driver obtainable? (ARM's Windows Mali driver availability, prior art on any Exynos WoA display) | **done** | `notes/d5-gpu-display.md`: **no Mali WDDM exists or ever shipped** — Update Catalog zero hits, Arm's downloads are Linux/Android-only, Mu-Silicium GPU ❌ on every Samsung target, and no Snapdragon ever used Mali so there is no ecosystem to piggyback |
| Research: edk2-exynos DECON/display bring-up for exynos9820 (they light the S10 panel in UEFI today) | **done** | Mu's whole Samsung display story is `SimpleFbDxe` over a bootloader-lit fb — structurally identical to our `SimpleFramebufferDxe`; edk2-exynos has no extra display code to mine. Bonus read: the 9820 GPU is `mali@18500000` with ARM's open Bifrost kbase `b_r10p0` (max 702 MHz) in the LineageOS kernel |
| Verdict task: GPU acceleration on Windows is either a real driver project or a documented dead end — write the decision down | **reopened 2026-10-10** | original verdict (no Mali WDDM exists → framebuffer display is the deliverable, `notes/d5-gpu-display.md`) stands **for shipped code**, but the user reports the S9 WoA author has **WDDM running** (unreleased — unverified), and the addendum records the real ladder: (1) DECON display-only WDDM, (2) open kbase `b_r10p0` (already in the 9820 kernel source!) → DXGK + **panfrost/Mesa** userspace, (3) closed DDK port = dead end (Arm EULA no-RE) |
| D5b: Mali/WDDM feasibility inventory | todo | (a) size the kbase r10p0 source in the LineageOS 9820 kernel (files, DRM interfaces, GPL terms); (b) locate the G76 firmware blob (vendor image / ko-embedded) — a device read; (c) **needs-user**: what exactly did the S9 person get running — display-only miniport or full GPU? any link/channel; (d) check panfrost-on-Mesa-Windows-ARM64 build status |
| Firmware display follow-ups (re-scoped from "GOP driver"): validate the sboot fb survives into Windows, close the fb RAM/DEV overlap (M8 task), DECON blank/backlight sanity at handoff | todo | the UEFI half already exists; what is unproven is **handoff survival** — does the fb/DECON state our shim inherits still be drawing at `ExitBootServices`, and does Windows' BasicDisplay keep it — **needs-boot** |

### D6 — External display (stretch)

| Task | State | Notes |
|------|-------|-------|
| Identify what the user's hub's HDMI actually is (DP alt mode vs DisplayLink) | **done — from live descriptors** | UGREEN hub on this PC right now: Genesys Logic GL352 hub chips + **ASIX AX88179B** GbE, **no `0x17e9` DisplayLink device** -> HDMI is **DP alt mode**. Measured worst-case draw 482 mA fits the 500 mA budget. `notes/d6-external-display.md` |
| Research: DP alt mode on the 9820 combo PHY (SMV/HPD, what DeX does at `displayport@130B0000`) | **done** | `notes/d6-external-display.md` P1-P7: full kernel chain pinned (`displayport_drv.c` in dpu20, max77705 `max77705_alternate.c` PD state machine, three board GPIOs incl. gpg0-4 PHY_CON, `LANE_MUX_SEL_DP=0xc`, PMU 0x15860704 gates the PHY, RBR/HBR/HBR2 only); DECON_T confirmed `0x19050000` `out_type=2`(DP) with SPIs 195-197 + SYSMMUs; **no UEFI prior art exists** (edk2-exynos has none); DisplayLink driver 12.3 (Sep 2026) = WHQL + Win11 ARM64 |
| Decide: DP alt mode vs DisplayLink dock (DisplayLink is the funded fallback) | **done — decision** | For THIS hub: HDMI is DP-alt, which under Windows needs our own display-only WDDM (D5b rung 1 + DP/PHY/CCIC halves) — **not funded now**. If an external display is wanted before that, a DisplayLink adapter/dock (~$50-150) works with the WHQL ARM64 driver on top of D1 alone. Zero-risk hardware validation first: DeX on Android with this hub + monitor |

### D7 — CPU driver (all three core types)

| Task | State | Notes |
|------|-------|-------|
| Research the DVFS/clock/power mechanism from the Android 12 ODIN dump | **done** | `notes/cpu-driver-9820.md` F1–F11. The 9820 does NOT DVFS via CMU MMIO: CPU clusters are driven by the **ACPM coprocessor** — IPC mailbox `0x15900000` (doorbell `INTGR0=(1<<ch)<<16`), rings in ACPM SRAM `0x2039000+0x30000` (framework @+0x7000), 4×u32 message `{idx|seq<<16, kHz, FREQ_REQ=0/FREQ_GET=1, 0}` on **channel 5**, idx 2/3/4 = LITTLE/MID/BIG. OPP+voltages are plain memory (fvmap, plugin base + `fvmap_offset 0xa400`). Protocol verified in the shipped kernel binary (F11). IRQ SPI 40 (GSIV 72) |
| Research PSCI vs the ACPI parking protocol for Windows core bring-up (WoA prior art + Samsung monitor) | **done** | `notes/psci-parking.md` §A–C. Shipped 9820 monitor = PSCI-0.2 IDs over SMC under a bare `arm,psci` DT binding — the documented Samsung pattern (Exynos7420 downgrade commit `e1e47fbca668`: SYSTEM_OFF/RESET absent). Renegade/Surface Duo model (FADT PSCI+SMC, GICC PPV=0) is the design to copy; the **ACPI parking protocol** exists in Windows' world (Lumia 950/8992 stock WP10 PPV=1 tables boot desktop Windows, though PSCI is also declared there so the active mechanism is not isolated — see notes §C6.3) = candidate EL3-independent Plan B; UEFI in no port ever onlines APs — Windows does. Open: probe PSCI_VERSION/FEATURES/AFFINITY from our own UEFI (C5) before trusting 0.2+; MIDR 0xd0a-vs-0xd09 conflict flagged (C4) |
| Close the shipped OPP+voltage ladder values | **todo — one safe read** | pstore `console-ramoops` from TWRP (kernel prints `CPUFREQ of domain…` at boot) or 143 KB `by-name/ACPM` dump; both read-only, F10 |
| Design doc: Windows CPU driver (KMDF, polling-mode ACPM IPC, fvmap read at init, thermal clamp from DT trips) | **todo** | one uniform protocol covers all three core types — the M4/prometheus/ananke differences are hidden inside the ACPM; PPTT performance ranks from F5 (272/531/785 DMIPS) |
| Implement + device-validate | **todo** | bar: Windows can set/get each cluster's frequency; nothing is Ready until observed |

### D8 — Audio (speakers, mic, volume) + hardware buttons

| Task | State | Notes |
|------|-------|-------|
| Research: identify the audio hardware + buttons from the shipped Android 12 dump (DT, vendor firmware, kernel strings) | **done** | `notes/d8-audio.md` §1: ABOX audio DSP (`abox@18c50000`, 2×CA32 "Calliope" cores, 12 RDMA/8 WDMA/4 UAIF/1 DSIF), sound card `samsung,beyond-madera`, codec **Cirrus CS47L93 on spi_2** (irq gpa2-2, reset gpg1-0), **2× CS35L40 SmartAmp on hsi2c_38** @0x40/0x41 (bot=right/bottom spk, rcv=left/receiver), DMICs into CS47L93 IN3L/IN4L, mics/headphone/jack all codec-side; buttons = `gpio-keys` on ALIVE GPIO: VOLUP gpa0-3, VOLDN gpa0-4, POWER gpa2-4, BIXBY gpa0-6 |
| Research: Linux/Android driver stack (abox, beyond_madera, madera, cs35l40) + mainline status | **done** | `notes/d8-audio-linux.md`: full source map + line refs; ABOX **not mainlined** (Samsung's 2025 upstream attempt = ExynosAuto only); madera mainline (CS47L93 via cs47l92.c); cs35l40 folded into mainline cs35l41 family; **no firmware-free playback mode exists in the vendor driver** — the DSP programs the DMA buffers; UAIF format/enable IS AP-owned |
| Research: Windows prior art (portcls/ACX on ARM64, Cirrus Windows drivers, WoA audio status, button mechanisms) | **done** | `notes/d8-audio-windows.md`: portcls works non-PCI (SimpleAudioSample ROOT device); ACX = modern class extension built for DSP+codec+amp stacks; **Mu-Silicium audio ❌ on every Exynos target — no prior art to copy**; Cirrus Windows packs binary-only behind click-through, no CS47L93/CS35L40 pack; buttons: **ACPI0011 + inbox Hidinterrupt.sys + GpioClx** is the native path (exact _DSD format captured); usbaudio2.sys inbox on ARM64 |
| Fix: Beyond0 touch port carried 9810 ALIVE-GPIO constants (base 0x14050000, GPA1_CON 0x040) — probe would fail on the S10e | **done** | found while pinning the EINT map (`notes/d8-audio.md` §4); base→0x15850000, CON/PUD→0x020/0x028; EINT constants proven correct for 9820 |
| Constants: ALIVE GPIO bank/EINT register map + key GSIVs + ABOX windows into `Exynos9820.h` | **done** | all VERIFIED this session from vendor pinctrl + dt-bindings (`notes/d8-audio.md` §3): banks gpa0..gpa4 @0x000..0x080, EINT_CON 0x700+4/bank, FLTCON 0x800+8/bank, MASK 0x900, PENDING 0xA00; keys = GSIV 40/41/43/57 |
| DSDT: ALIVE GPIO controller device + `Device (BTNS)` ACPI0011 with 4 GpioInt + _DSD button usages | **done (built into the FD; Windows enumeration unvalidated)** | format = learn.microsoft.com ACPI0011 desktop sample; Power=0x01/0x81, VolUp=0x0C/0xE9, VolDn=0x0C/0xEA, Bixby=0x0C/0x221; verified in `FVMAIN.Fv` (`notes/d8-audio.md` §6) |
| Windows driver: `Exynos9820Gpio` — GpioClx provider for the ALIVE block (SimGpio skeleton, Exynos bank/EINT registers) | **done (code; not built, not booted)** | unblocks Hidinterrupt for buttons AND the codec/amp/jack IRQs for D8-audio later; ActiveBoth via GpioClx level+polarity emulation; **pseudo-bank model** (34 banks of 1 pin) reconciles per-pin EINT SPIs with GpioClx's per-bank interrupt mapping — `notes/d8-audio.md` §6 |
| Device test: volume/power/Bixby buttons under Windows | todo | **needs-windows + needs-boot** |
| Audio rung 0 (interim): USB-C audio dongle under XHC0 — zero driver work | **done — decision** | usbaudio2.sys inbox ARM64 (`notes/d8-audio-windows.md` Q4); rides entirely on D1/D3 |
| Audio rung 1 (the driver project): portcls WaveRT miniport (SimpleAudioSample skeleton) + ABOX UAIF/RDMA programmed directly (SPUS/SPUM router RE from `calliope2.dt`) + CS47L93 (needs an Exynos SPI/USI master driver) + CS35L40 over our HSI2C driver | **design done** | full design + bring-up sequence + dependency list + go/no-go gate in `notes/d8-audio-design.md`; blockers tracked there (§3/§4): spi_2 driver, SPUS/SPUM direct-routing experiment, hsi2c_38 instance |
| Audio: 3.5 mm jack feasibility + scoping | **done — feasible, and it is the SMALLEST rung-1 slice** | jack = CS47L93 HPOUT2L/R, no external amp; jack detect = codec impedance HPDET/MICDET (+moisture) on codec IRQ GSIV 55; Windows side = KSJACK_DESCRIPTION2 + IPortEvents (eConnTypeMiniPhoneJack). Chain skips amps/amp-fw/hsi2c_38 entirely — `notes/d8-audio.md` §7, design §1a; the only shared blocker stays the SPUS/SPUM direct route |
| Audio rung 2 (parity): Calliope IPC (rings 0x850000/0x854000, SGI doorbell 0x18CF1F00) + load calliope_*.bin from the device's own /vendor at install time — never commit the blobs | todo | proprietary-firmware posture + licensing in `notes/d8-audio-windows.md` Q5; ABOX GIC may be EL3-secured (boot hazard note in linux notes) |

## Working rules for this file

- Same ground rules as `MILESTONES.md`: one commit per task, `todo → doing → done`
  moves in the same commit, nothing device-validated until it is actually observed.
- Research tasks go to a **subagent** that appends findings (with source URLs) to
  the named notes file **as it works**; the implementation pass reads only that file.
- No MMIO constant enters code unless it is `VERIFIED` in `Exynos9820.h` or a notes
  file; anything else stays `UNVERIFIED` and named.
- The non-powered hub is a **hard requirement** of the donation build until a spare
  unit exists to test powered-hub host mode.
