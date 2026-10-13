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
| D4c | Bluetooth: the same **BCM4375's BT side, over UART1@10840000** (H4 + patchram) | **doing** | research **done** (`notes/bt-driver.md` B1-B4): chip/GPIOs/IRQ/clocks/patch-format/open-source-equivalents all pinned from the shipped DTBs + vendor kernel mirror + WDK sample; design/code tasks below; device rungs ride on the boot stream |
| D5 | GPU/display: framebuffer deliverable decided; Mali WDDM reopened as D5b | **doing** | research + verdict **done** (`notes/d5-gpu-display.md` + addendum): shipped-code deliverable is GOP + in-box BasicDisplay; acceleration reopened after the S9 WDDM report — kbase r10p0 source is already in the 9820 kernel, feasibility inventory (D5b) is next; fb handoff validation still **needs-boot** |
| D6 | External display (DP alt mode over the combo PHY) — stretch | **doing** | research + hub ID + decision **done** (`notes/d6-external-display.md`): user's UGREEN HDMI = DP alt mode (no DisplayLink chip) -> dark under Windows until a display-only WDDM exists (D5b rung 1); DisplayLink adapter is the buy-it-now alternative; DeX-on-Android validation recorded |
| D8 | Audio (speaker/mic/volume) + hardware buttons | **doing** | research + constants + DSDT + **buttons driver code** all done: ABOX/CS47L93/CS35L40 chain identified and designed (`notes/d8-audio-design.md`); buttons = `Exynos9820Gpio` (GpioClx, code not built) + ACPI0011 in FD (inbox Hidinterrupt.sys). Device validation of both waits on Windows booting; audio rung 1 gated on the SPUS/SPUM experiment |
| D9 | WWAN: mobile data (Exynos 9820 integrated CP, project S5000AP) | **doing** | research + constants + design **done** (`notes/modem-wwan.md` T0–T16, `D9-MODEM-DESIGN.md`); boot+link planes pinned, control plane being cracked in gitignored `re/`; **device rungs now runnable** (Windows boots — confirmed 2026-10-13; driver agent executes `tasks/R02-*.md`, this stream feeds it PoCs) |
| D10 | Video codec (MFC): the 9820's offload engine | **doing** | opened 2026-10-13 from a third-party tree pointer (`ianmacd/d2s`); framing + task file done (`notes/video-mfc.md` T0, `tasks/V01-mfc-live-reads.md`); identity, constants, the Windows contract and prior art all open (U1–U8) |
| D11 | Thermal management: sensors, trip points, cooling | **doing** | opened 2026-10-13; framing done (`notes/thermal.md` T0 — the DT zones/trips are already in `notes/cpu-driver-9820.md` F6), task file `tasks/TH01-thermal-live-reads.md`; the safety question (a hardware trip with no OS) is the headline open item |

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
| Windows half: `drivers/Exynos9820Wlan/` NDIS miniport skeleton — ACPI bind, windows, chip identity, fw/rings staged | **done (code; not built — needs-windows)** | NDIS 6.80 miniport binding `ACPI\SAMS9825`; maps CFG0+MEM windows from `_CRS`, runs the R3 handshakes (EP VID/DID via CFG0, ChipCommon chipid via BAR0+0x3000 == 0x4375); sends honestly NOT_ACCEPTED until R4; every later stage carries a NOT-IMPLEMENTED marker naming the reference file + rung. License posture (brcmfmac ISC primary / bcmdhd reference-doc) in README |
| DSDT: `Device (WLAN)` `_HID SAMS9825` + CFG/MEM windows + GSIV 285 | **done (built into the FD; Windows enumeration unvalidated)** | `_CRS` order is a binding contract with the driver's resource indices; ELBI/DBI/PHY/IA deliberately absent (role-latch, D3 pattern) |
| Device rungs: R1 link-up, R2 firmware-up (UEFI), R3 Windows enumeration, R4 scan | todo | **needs-boot** — each rung is a device observation, nothing Ready before it |

### D4c — Bluetooth: BCM4375 BT over UART1

| Task | State | Notes |
|------|-------|-------|
| Research: chip, transport, GPIOs, IRQ, clocks, patch format, open-source equivalents, Windows binding path | **done** | `notes/bt-driver.md` B1-B4: BT function of the same **BCM4375** (B0/B1 patch files, SEMCO E43 module); **UART1@10840000** (USI-v2, GSIV 440 (DT cell = SPI 408; corrected 2026-10-13), fifo 256, RTS-as-GPIO), USI mode reg 0x10821000=1, CMU_PERIC1 gates 0x22a/0x23c, pins gpq0-0..3 (ALIVE), bt_wake gpg2-0, bt_hostwake gpa2-3, bt_en = PCAL6524@hsi2c_25(0x10930000) pin 7 (rev-25 variant: gpm8); patch files = raw HCI stream (0xFC4C ×1260 + 0xFC4E) exactly what mainline `btbcm_patchram` replays; Windows path = **serialhcibus** (BthxDDI, MS-PL) + **cywbtserialbus** (MS-PL) as prior art, `hci_bcm`/`btbcm` as behavioral reference (GPL, reference-only) |
| Constants: UART_BT/USI windows, GSIV 440 (DT cell = SPI 408; the value was 408 until the 2026-10-13 audit), pin mux values, clock ids into `Exynos9820.h` + memory map | **done (built)** | see `notes/bt-driver.md` B3; every value carries its read; Linux verification build exit 0 (moved late — review note in `notes/bt-driver.md`) |
| Design doc: ACPI-described BT device + BthxDDI transport + H4/patchram ladder | **done** | `docs/exynos9820-port/D4c-BT-DESIGN.md`: UEFI role-latch (CMU/pinctrl/USI mode) + Windows direct-MMIO UART (no serial stack — Samsung UART is not 16550); 6-rung ladder R1..R6; license posture serialhcibus/cywbtserialbus MS-PL adoptable, hci_bcm/btbcm GPL reference-only, blobs device-copied |
| Windows half: `drivers/Exynos9820Bt/` skeleton — ACPI bind, MMIO UART, H4 framing, patchram staged | **done (code; not built — needs-windows)** | KMDF skeleton binding `ACPI\SAMS9826`; implemented: UART/USI programming, H4 TX framing, `.hcd` record walker (matches the byte-exact parse); every later stage carries a NOT-IMPLEMENTED marker naming rung + reference file; license posture (serialhcibus/cywbtserialbus MS-PL adoptable, GPL refs only, blobs device-copied) in README |
| Firmware half: `Beyond0LteBtUartDxe` — CMU_PERIC1 ungates (QCH), gpq0 mux, USI mode=1, then R1 loopback + R2 ROM-HCI prints | todo | the role-latch the Windows half depends on for clocks/pads (design §3); gate-offset math = M12's CMU_FSYS1 pattern with ids 0x22a/0x23c |
| DSDT: `Device (BTH0)` + UART window + GSIV 440 (corrected from 408 on 2026-10-13 — the DT cell is the SPI number) + hostwake GpioInt | **done (built into the FD; Windows enumeration unvalidated)** | `_HID SAMS9826`, `_CRS` order = driver binding contract; USI-mode/CMU/pin-mux absent by role-latch; `Dsdt.aml` carries `BTH0`+`SAMS9826` (grep of build output) |
| Device rungs: R1 UART loopback/echo, R2 ROM HCI reply, R3 patchram-up, R4 Windows enumeration + LE scan | todo | **needs-boot** |

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

### D9 — WWAN: mobile data (Shannon S5100)

| Task | State | Notes |
|------|-------|-------|
| Research: modem identity (discrete vs integrated), AP↔CP interface contract (shmem rings, doorbell, GPIOs), firmware/boot path, Windows WWAN contract, prior art | **done** | `notes/modem-wwan.md` T0–T11: **integrated CP (S5000AP), not discrete S5100** (T7 correction); nodes `mif_pdata`/`mailbox_cp`/`shmem` pinned from dtbo; SBD=MIPI LLI IPC with exact struct layouts, SIPC5 framing, mailbox doorbell ISSR/INTGR1, shmem IPC base 0xFB200000, CP boot = SMC 0x82000700 + PMU + AP2CP_CFG + STD_UDL on umts_boot0; Windows requires MBIM (inbox binds USB only; non-USB = IHV lower-edge client driver over MBBCx/NetAdapterCx, raw-IP NdisMediumWirelessWan); prior art EMPTY on every Exynos target; the one closed wall = SEC-RIL control encoding (U10) |
| Live reads: enabled modem DT nodes, mif IRQs, userspace device nodes | todo | **needs-phone** — `tasks/R01-modem-live-reads.md` |
| Serial observation: `[CP] R1 pre:` + `[MBOX] R2:` from the FD's ModemDxe slice (zero writes) | todo | **needs-boot (satisfied)** — `tasks/R02-cp-serial-observe.md`; closes P1/P2, gates the INIT=1 flip |
| Constants: modem-interface windows/IRQs/GPIOs into `Exynos9820.h` + memory map | **done (built)** | CP shmem partitions (IPC `0xFB200000`+0x500000), DIT `0x1A2A0000`, SYSREG_BUSC, mailbox `0x15920000`, AP2CP_CFG, PMU CP regs, SMC `0x82000700`, GSIVs 73/98/99/100/407, RADIO LBA range; 5 memory-map entries; Linux verification build exit 0, FD sha `91068362…` — `notes/modem-wwan.md` T12 |
| Design doc: two halves + rung ladder + honest feasibility verdict | **done** | `docs/exynos9820-port/D9-MODEM-DESIGN.md`: boot+link planes fully open (R1–R6 ladder), control plane CLOSED (SEC-RIL, U10) — R7 not scheduled pending the P5 policy decision; prior art empty |
| Firmware half: `Beyond0LteModemDxe` — first slice: CP PMU status + mailbox observation (R1 pre-read / R2 observation) | **done (code; built into the FD; not booted)** | AP2CP_CFG init write compiled out behind `BEYOND0LTE_MODEM_INIT 0` (DECISION, design §7); FFS `…554A` + `[CP] R1 pre:` string verified in `FVMAIN.Fv`; Linux verification build exit 0, FD sha `cbb55d46…` — `notes/modem-wwan.md` T13. R3–R5 (SBD link, STD_UDL download, ring probe) stay todo and ride the boot stream |
| Windows half: `drivers/Exynos9820Wwan/` MBBCx skeleton + DSDT `Device (WWAN)` `SAMS9827` | todo | after the firmware half proves R3 |

### D10 — Video codec (MFC)

| Task | State | Notes |
|------|-------|-------|
| Research: what the video offload block is on the 9820 and on this board — identity, DT node, clocks, SYSMMU/IOMMU, firmware blob | **doing** | `notes/video-mfc.md` T0 (framing) done; U1–U4, U8 open — dumps walk + `tasks/V01-mfc-live-reads.md` |
| Research: inventory the third-party tree `ianmacd/d2s` against this project's open subsystems (MFC first, then everything else it carries) | **done** | it is not a driver folder — it is a **complete Exynos 9820 vendor kernel** (Linux 4.14.113, Note 10 family, GPL-2.0, HEAD `d1d4fb4aa`), cloned to `/tmp/opencode/d2s` and kept out of the tree. Its MFC dtsi matches our shipped DTB cell for cell (`ip_ver 0x1300`, clock 422, GSIV 370). Also on the shelf: **five Arm kbase releases** `drivers/gpu/arm/b_r10p0…b_r19p0` (answers D5b(a)), `sound/soc/samsung/abox` (22 `.c`, D8), `drivers/misc/modem_v1{,_dual}` (D9), `drivers/iommu/exynos-iommu*` (SYSMMU), `drivers/media/platform/exynos/{fimc-is2,camera-pp,scaler,tsmux}`, `drivers/thermal/samsung/*` (D11) — `notes/video-mfc.md` T2, `notes/thermal.md` |
| Research: Windows contract for a standalone (non-GPU) M2M codec — inbox class? D3D11 video / Media Foundation / DXVA, and what each requires of a display miniport | **done** | U6 closed: **AVStream KS codec filters** (MF proxies them as MFTs; decoder/encoder/video-processor as three filters, NV12 mandatory, `KSMFT_CATEGORY_VIDEO_DECODER`, Avshws as the sample) **or** a custom driver + **proxy hardware MFT** (`MF_TRANSFORM_ASYNC`, `MFT_ENUM_HARDWARE_URL_Attribute`, dynamic format change). DXVA/D3D11 video is closed to us — it is implemented by the graphics driver (WDDM), which D5 says does not exist. D10 is therefore **not** gated on D5b — `notes/video-mfc.md` T3 |
| Research: prior art — mainline `s5p-mfc` coverage for Exynos 9820, and whether any Windows port has ever driven a Samsung MFC | **done** | mainline covers v5/v6/v7/v8/5433/v10 only — **no 9820 support**, and the 9820 uses a different vendor driver entirely; no Windows MFC driver anywhere (the "MFC" search term collides with Microsoft Foundation Classes — dead end recorded) — `notes/video-mfc.md` T4 |
| Live reads: codec DT node name, `/proc/interrupts` line, `/dev/video*`, blob names | todo | **needs-phone** — `tasks/V01-mfc-live-reads.md` |
| Constants: MFC window + IRQ + clock ids + SYSMMU into `Exynos9820.h` + memory-map entries | **done (built)** | MFC `0x188D0000+0x10000`, GSIV **370**, `ip_ver 0x1300`, gates 422/423/424, SYSMMU0/1 `0x18890000`/`0x188B0000` (+0x9000, secure aliases `0x188A0000`/`0x188C0000`, GSIVs 375-378), hwfc `0x18A28000`, mmcache `0x1A300000`, PMU `0x15862600`, regs `RISC_ON/RISC_BASE/RESET/VERSION` + R2H codes 1/12; 5 memory-map entries; verification build exit 0, FD 7 340 032 B, `FVMAIN_COMPACT` 56 B free — `notes/video-mfc.md` |
| Design doc: two halves + rung ladder + honest feasibility verdict | **done** | `docs/exynos9820-port/D10-VIDEO-MFC-DESIGN.md`: R0–R7 ladder (R1 = a zero-write read of `0xF028`, expect `0x1300`); **Windows shape chosen = AVStream minidriver with three KS filters** (MF proxies them as MFTs), `Device (VIDC)` `_HID SAMS9828` with a fixed `_CRS` order; the worst risk is named as P3 — two SYSMMUs sit between the codec and DRAM, so a wrong translation is silent memory corruption, and the design rules that **Windows owns them**; P7 states plainly that D10 is a performance/power project, not a capability one |
| Firmware half: `Beyond0LteMfcDxe` first slice — read `MFC_VERSION`, print `[MFC] R1: version=`, zero writes | todo | module not written yet; `tasks/V02-mfc-r1-observe.md` is queued behind it |
| Windows half: AVStream minidriver (`SAMS9828`) + SYSMMU programming + KS decoder/encoder/video-processor filters | todo | gated on R2/R4; P8 asks whether AVStream is still viable on WoA — the first enumeration settles it |

### D11 — Thermal management (sensors, trip points, cooling)

| Task | State | Notes |
|------|-------|-------|
| Research: the sensor side — TMU channels, how a temperature is actually obtained (ACPM IPC vs MMIO), the `sec_thermistor@*` chain | **done** | `notes/thermal.md` T1/T2: **five** TMU controllers (`0x10090000+0x800` TOP: BIG/MID/LITTLE, `0x100A0000+0x800` SUB: G3D/ISP), GSIVs **510/511**, sensor masks `0x7C0/0x20/0x10/0x21E/0x100`; a temperature IS readable by MMIO (`CURRENT_TEMP1_0 0x84`, 9-bit codes, one-point trim `efuse 55`/`trim 25`) but the AP's normal path is **ACPM IPC ch 9** (16-byte message, `READ_TEMP 0x02`, `AP_SUSPEND/RESUME`); three `sec_thermistor` nodes enabled in all 7 overlays — what they guard stays open |
| Research: **the safety question** — is there a hardware thermal trip (shutdown/clock-stop) that fires with no OS driver, and at what temperature | **doing** | half closed: the TMU trip engine **is** armed in hardware (`TRIP_EN` bit 12, thresholds in `THD(p)=0xD0+p*0x50`, `COUNTER_VALUE0/1 = 0x028A`) and `tmu_default_trip_temp = 115 °C`; but the DT has **no `critical` trip** and what the hardware *does* on a trip is unstated in the GPL source → closes by the zero-write DXE observation + the TMU TRM |
| Research: the vendor thermal stack — `drivers/thermal` in the Exynos 9820 kernel tree + Samsung's thermal policy data | **done** | `exynos_tmu.c` + `exynos_acpm_tmu.c`, `CONFIG_EXYNOS_THERMAL=y`/`CONFIG_EXYNOS_ACPM_THERMAL=y`, governors `step_wise`+`power_allocator`, `EMERGENCY_POWEROFF_DELAY_MS=0`; policy XMLs **not** in this dump (U8) |
| Research: Windows contract — ACPI thermal-zone objects (`_TMP`/`_PSV`/`_HOT`/`_CRT`, `_TC1/_TC2/_TSP`, `_ACx`), the WMI thermal class, what Windows can actually cool on ARM64 | **done** | `notes/thermal.md` T3: Windows' thermal model **is** the ACPI thermal zone and is built-in for SoC platforms; `_TMP` in firmware (AML) or a sensor driver via `_HID`/`_CRS`; `_PSV` → passive, `_HOT` → hibernate, `_CRT` → shutdown; **ACPI000C makes Windows park cores at `_PSV`**; driver hook = `GUID_THERMAL_COOLING_INTERFACE`; no thermal zone exists anywhere in our tables today |
| Live reads: thermal zones, idle vs loaded temperatures, battery temperature | todo | **needs-phone** — `tasks/TH01-thermal-live-reads.md` |
| Constants: sensor windows/IRQ + the trip values that enter code | **done (built)** | `Exynos9820.h` Thermal (D11) block: TMU TOP `0x10090000+0x800` / SUB `0x100A0000+0x800`, GSIVs **510/511**, the vendor register offsets, the one-point trim values (55/25/85/50) and `tmu_default_trip_temp 115 °C`, ACPM TMU channel 9; 2 memory-map entries; Linux verification build exit 0, FD 7 340 032 B, `FVMAIN_COMPACT` 24 B free — `notes/thermal.md` T4. The `0x028A` counter value is recorded but **not** defined as usable code (semantics UNVERIFIED) |
| Design doc: two halves + rung ladder + the safety verdict | **done** | `docs/exynos9820-port/D11-THERMAL-DESIGN.md`: R0–R5 ladder; **the deliverable is AML, not a driver** — `ThermalZone` + `OperationRegion` over the TMU window + `_TMP` doing the one-point calibration in AML, `_PSV` 83 °C / `_HOT` 115 °C from the DT and `_CRT` as a stated **DECISION** (the DT has no critical trip); rule set: **our code only reads the TMU**, the ACPM keeps ownership; P5 (does WoA even poll ACPI thermal zones?) is the rung that could invalidate the whole shape |
| Firmware half A: `Beyond0LteThermalDxe` — print `CONTROL`/`THD0`/`INTPEND0`/`CUR_TEMP0` + calibrated °C for both windows, zero writes | todo | module not written yet; `tasks/TH02-tmu-r1-observe.md` is queued behind it — this is the measurement that answers "what protects the phone with no thermal driver" |
| Firmware half B: ACPI `ThermalZone` objects in `Dsdt.asl` (BIG/MID/LITTLE/G3D) with `_TMP`/`_PSV`/`_HOT`/`_CRT`/`_PSL` + `ACPI000C` | todo | after R1/R2 prove the read and the math |
| Windows half | **none planned** | the in-box ACPI driver evaluates the zone objects; a driver appears only via `GUID_THERMAL_COOLING_INTERFACE` if R4 proves too coarse |

## Working rules for this file

- Same ground rules as `MILESTONES.md`: one commit per task, `todo → doing → done`
  moves in the same commit, nothing device-validated until it is actually observed.
- Research tasks go to a **subagent** that appends findings (with source URLs) to
  the named notes file **as it works**; the implementation pass reads only that file.
- No MMIO constant enters code unless it is `VERIFIED` in `Exynos9820.h` or a notes
  file; anything else stays `UNVERIFIED` and named.
- The non-powered hub is a **hard requirement** of the donation build until a spare
  unit exists to test powered-hub host mode.
