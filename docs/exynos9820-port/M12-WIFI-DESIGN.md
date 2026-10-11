# M12/D4b — WiFi design: the S10e's own BCM4375 on Windows ARM64

Plan-of-record: [`DRIVERS-MILESTONES.md`](DRIVERS-MILESTONES.md) §D4b.
Evidence for every hardware claim: [`../../../notes/m12-wifi-chip.md`](../../notes/m12-wifi-chip.md)
(cited as "W§" below). Nothing here is validated on the device; the rungs in §7 say
what observation closes each.

## 1. What the hardware is (settled by research, 2026-10-11)

- **Chip:** Broadcom **BCM4375** (802.11ax WLAN/BT combo, chip id `0x4375`; the S10e's
  Wi-Fi class per the LineageOS device page), a **PCIe endpoint**.
- **Root port:** Samsung `pcie_0` at ELBI `0x13ED0000` — DesignWare-based, **Gen1 x1**,
  **INTx only** (no MSI on 4375, and DT `use-msi="false"`), PERST# `gpf2-2`,
  CLKREQ# `gpf2-0`, chip power pin WLAN_EN `gpg0-5`, host-wake `gpa0-7` (W§2/W§8).
- **Windows:** ELBI/PHY/PCS/DBI/IA/SYSREG/CFG + MEM iATU window `0x14000000`+0xFF0000,
  CFG0 window `0x14FFE000`+0x2000 — all in `Exynos9820.h` now (W§2).
- **IRQ:** SPI 253 → **GSIV 285** (level-high, W§2).
- **Clocks:** CMU_FSYS1 gates + QCH, MMIO offsets VERIFIED; ACPM/HWACG owns them in
  Android, but sboot never enables PCIe, so **we gate them on by MMIO** (W§clocks).
- **Firmware:** no open 4375 firmware exists anywhere (W§5). The working blobs are the
  device's own `/vendor/firmware/bcmdhd_sta.bin` + `nvram_net.txt` (exact names
  needs-phone). Policy: **never commit blobs** — the installer copies them from the
  device's own vendor partition at install time, same as the D8 audio rule.
- **Driver references:** vendor **bcmdhd** (GPLv2+linking-exception, the only complete
  4375 implementation) and mainline **brcmfmac** (ISC, cleaner architecture, supports
  the 43751/43752 cousins but not 4375) (W§4).

## 2. Architecture decision: ACPI-described device, not PCI-enumerated

Two ways to present the endpoint to Windows:

| | A: real PCI enumeration | B: ACPI-described MMIO device (**chosen**) |
|---|---|---|
| How | DSDT `PNP0A08` root bridge + 2 MB ECAM; Windows `pci.sys` binds the EP | DSDT `Device (WLAN)` with `_HID "SAMS9825"`; our miniport maps the windows itself |
| Cost | iATU CFG0 gives ONE 4 KiB window per bus/dev/func match; a 2-bus ECAM (bus0 RC + bus1 EP) needs either ATU bus-match support (ip-ver 0x982000, unknown) or per-access ATU reprogramming + a custom `PciSegmentLib` in UEFI **and** a matching ECAM contract in Windows | none — the driver maps CFG0/MEM windows directly, exactly like `USBXHCI` over `PNP0D10` works today (D3) |
| Precedent in this repo | none | XHC0/PNP0D10 role-latch pattern (`notes/d1b-usb-vbus-hcd.md` Q6) |

**Decision: B.** The firmware trains the link, programs iATU (MEM + CFG0), assigns the
EP's BAR0 = `0x14000000`, sets `PCI_COMMAND = MSE|BME`, and latches that state across
`ExitBootServices`. The Windows driver binds `ACPI\SAMS9825`, maps the CFG0 window (for
the handful of config registers it owns: COMMAND, INTx-disable) and the MEM window (the
chip's BAR0 backplane), and never needs `pci.sys`. Residual risk (same class as D3's):
nothing else in Windows may poke the controller — the ELBI/DBI/PHY windows are
deliberately **not** in `_CRS`.

## 3. Firmware half — `Beyond0LtePcieDxe` (UEFI)

Transliteration of `exynos_pcie_poweron` + `exynos_pcie_establish_link` (W§8), in order:

1. CMU_FSYS1 gates ON (MANUAL|CG_VAL convention from §7.5 of `notes/s10e-live-reads.md`),
   QCH regs per kernel; FSYS1 PHY_REFCLK first.
2. PMU `+0x71C` ← 1 (PHY control). PMU wakeup-mask step: skipped (9820 guard in kernel).
3. GPIO: WLAN_EN (gpg0-5, PERIC0 block) ← 1, wait 100 ms; pinmux per DT pinctrl states.
4. ELBI soft core reset 0x1D0: 0 → 20 µs → 1. PERST# (gpf2-2, FSYS1 block) ← 1, wait 20 ms.
5. ELBI 0xF4 |= HW-L1-exit|L1-NAK-master; 0x1B8 ← manual linkdown reset; 0x2C8 QCH ungating.
6. PHY clocks; `phy_all_pwrdn_clear` then `phy_config` — both fully specified by
   `pci-exynos9820_cal.c` ch0 path incl. the 48-word CMN + 80-word TRSV PHY tables
   (notes W§P1) — + the **I/A microprogram** (SQ0..SQ11, verbatim from the kernel,
   W§8 step 2) + ELBI 0x018 bus-number-enable.
7. DBI setup_rc: DBI_RO_WR_EN, LNKCAP x1/Gen2, aux clock 26 MHz, L1SS durations,
   PM_CTRL clear, LNKCTL retrain, LNKCTL2 TLS=Gen1.
8. LTSSM enable (ELBI 0x02C ← 1); poll ELBI 0x074 bits[4:0] ∈ [0x0d..0x14], 10 µs × 2000;
   on failure: PERST low, LTSSM off, phy clk off, retry ×10 (kernel-faithful).
9. Link up: iATU region1 CFG0 → bus1dev0 @ `0x14FFE000`; region0 MEM → `0x14000000`.
10. EP bring-up: read VID/DID via CFG0 (expect `14e4:4475` — **the UNVERIFIED constant's
    read**), write BAR0 = `0x14000000`, `PCI_COMMAND = MSE|BME`, clear INTx-disable,
    enable ELBI INTx pulse routing to GSIV 285.
11. Print `[PCIe] link up 14e4:xxxx Gen1 x1` on the serial console — **rung R1**.

No EDK2 PCI stack involvement; the driver is self-contained (like `Beyond0LteUsbHostDxe`).

## 4. Windows half — `drivers/Exynos9820Wlan/` (NDIS miniport)

Model: **NDIS 6.8x Native 802.11 (WDI) miniport, ExtSTA**, ARM64, KMDF-free
(plain NDIS driver entry, like the repo's other drivers). Bind: `ACPI\SAMS9825`.

Layering (mirrors brcmfmac's clean split, ported to NDIS idioms):

| Layer | Responsibility | Reference |
|---|---|---|
| `PcieHw` | window mapping, CFG0 COMMAND/INTx, ELBI IRQ_PULSE ack, linkdown watch | `pcie-exynos-host-v0.c` |
| `Chip` | BAR0 window indirection (`PCIEBAR0Window`), core enumeration (CC/PMU/ARMCR4/SOCRAM/PCIe core), chip id 0x4375 check | bcmdhd `siutils.c`, brcmfmac `chip.c` |
| `Firmware` | TCM/RAM download to `BCM4375_FW_RAM_BASE 0x170000`, nvram write, shared-area (`pciedev_shared_t`) handshake, `hndrt`/trap watch, `cons` console pull | bcmdhd `dhd_pcie.c` fw path |
| `Rings` | common ring 0 (precommon→common), msgbuf control channel (ioctl/event), flow-control h2d/d2h descriptor rings, 2-byte indices flag | brcmfmac `commonring.c`/`msgbuf.c` (ISC) first; bcmdhd for 4375 deltas |
| `Wlan` | NDIS WDI: 802.11 OIDs (scan/join/auth), `NDIS_OBJECT_TYPE_802_11_*`, media state, send path with flow header, recv path with rx frame descriptor | Microsoft WDI docs + brcmfmac cfg80211 mapping |

**License posture:** the port is written against **brcmfmac (ISC)** as the primary
source; bcmdhd (GPLv2) is used as a *reference document* for 4375-only facts
(constants, quirks, register values — facts are not expression). Any function that
ends up transliterated from bcmdhd rather than brcmfmac gets a source comment, and
the package LICENSE note records the GPLv2 exposure. This keeps the repo's MIT parts
clean and makes the boundary auditable.

**Signing:** test-sign, same as the M7 driver set — the driver ships inside the
donation image; the recipient installs no drivers (this was the D4 disqualifier for
*retail* dongles, it does not apply to a driver we ship in the image).

## 5. Firmware blob handling

- Installer stage (new, `NotImplemented` until device-validated): read
  `bcmdhd_sta*.bin` + `nvram_net.txt` (+ per-revision variants named by
  `bcm4375_naming_table`) from the device's own `/vendor/firmware` over adb, place
  under `\S9WOA\wifi\` on the Windows partition.
- Driver loads them via `NDIS`-side file read at init (path from a registry/INF
  parameter), never embedded.
- `nvram` is board-calibration data: **must** come from this very device (regulatory
  + performance), which is also why a generic download can't substitute.

## 6. Open items (each names the read that closes it)

| # | Unknown | How it closes |
|---|---|---|
| P1 | ~~the 9820 `phy_config`/`phy_all_pwrdn_clear` bodies~~ **CLOSED** | `drivers/pci/dwc/pci-exynos9820_cal.c` — full ch0 PHY/PCS/SYSREG/ELBI sequence + 48-word CMN + 80-word TRSV tables (notes W§P1) |
| P2 | live PCI device ID (expect `14e4:4475`) | adb `ls /sys/bus/pci/devices/*/device` under Android, or the R1 console print |
| P3 | exact firmware blob names/variants on this unit | adb `ls /vendor/firmware` |
| P4 | whether direct CMU_FSYS1 gate writes work while ACPM/HWACG owns them | R1 on device (link-up or not) |
| P5 | SysMMU: board enables `use-sysmmu=true` for pcie_0 — DMA addresses are I/O VA under Android. We will run **without** SysMMU (identity) in our stack; whether the EP's DMA then works | R2/R4 (firmware-up + traffic) — if DMA faults, program SysMMU 1:1 |
| P6 | L1SS/ASPM: kernel enables L1.2 40 s after boot; our first cut leaves ASPM off | R4 stability; power budget for the donation build |
| P7 | host-wake EINT GSIV for gpa0-7 (ALIVE EINT numbering is not linear — D8 lesson) | vendor pinctrl EINT table read (source) |

## 7. Risk ladder — what "done" means per rung (nothing is Ready before its observation)

| Rung | Observation | Where |
|---|---|---|
| R1 | `[PCIe] link up 14e4:xxxx` on serial in UEFI | needs-boot |
| R2 | firmware-up in UEFI: shared-area version handshake + `"wlc_ver"` event string on serial | needs-boot (UEFI-side fw loader, separate task) |
| R3 | Windows enumerates `ACPI\SAMS9825` and the miniport binds | needs-windows + needs-boot |
| R4 | `ndisasm`/Event Log: media connect to an AP; ipconfig gets a lease | device |
| R5 | Native 802.11 OIDs pass the WDI verifier basics; scan list non-empty | device |

R1/R2 are deliberately in **firmware**, where a failure is a serial line, not a
driver install. The Windows half starts only after R1 is a fact.

## 8. What this does NOT change

- D4 (PicoPiFi RNDIS bridge) and M13 (dock ethernet) stay as the network plan —
  D4b is the "phone's own WiFi" stretch the user asked for, not a replacement.
- Nothing in the 9810 tree moves (repo rule).
