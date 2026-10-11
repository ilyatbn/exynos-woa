# M12/D4-reopen — the S10e's own WLAN chip: identity, bus, firmware, Windows driver path

Plan-of-record lines: `MILESTONES.md` M12, `DRIVERS-MILESTONES.md` D4.
User directive 2026-10-11: **work research + driver to get WiFi working on the S10e itself** —
find the chip, any open-source firmware/drivers online, and make a Windows-compatible driver.
This reopens the M12 "vendor bcmdhd is a dead end" verdict **on new evidence**.

## Why the old verdict was incomplete (fact)

`notes/dock-desktop.md` §6 proved only that the DT `wlan` node has **no `reg`** — that node
is Samsung's platform glue (power GPIO + host-wake). It never established **what bus the
Broadcom chip itself hangs off** (PCIe / SDIO / USB), which is the thing a Windows driver
would bind to. The D4 scan (USB *adapters*) does not speak to the internal chip at all.

## Session state

- 2026-10-11: `adb devices` → empty list. Phone not attached right now → live reads
  (`/sys/bus/pci/devices`, DT `pcie` node, dhd module params) are **needs-phone**, queued below.

## Questions

- W1. Exact WLAN chip on beyond0lte (BCM4375? variant?) — from vendor kernel defconfig/DTS/dhd source.
- W2. What bus: PCIe (which controller, what addresses) or SDIO (which host)?
- W3. PCI vendor/device ID the chip enumerates as (for INF binding).
- W4. Open-source driver reference: bcmdhd (Android, in vendor kernel) vs brcmfmac (mainline) —
  which supports this chip on this bus; license.
- W5. Firmware blobs: linux-firmware `brcmfmac4375-pcie.*` availability + license; the device's own
  `/vendor/firmware` names; nvram file.
- W6. Windows prior art: any Broadcom NDIS driver for this chip (any arch), any ARM64 one,
  any open-source Windows port.
- W7. 9820 PCIe controller bring-up: DT node, clocks/resets/PHY, prior art in edk2 (edk2-exynos, Mu).
- W8. How does the chip get powered/reset (wlanen GPIO, LDOs, clocks) — the bring-up sequence a
  driver must reproduce.

## Progress log

### 2026-10-11 — W1/W2: the chip is BCM4375 on **PCIe** — the old dead-end verdict is overturned (fact)

From the shipping-lineage vendor defconfig for this exact model,
`LineageOS/android_kernel_samsung_exynos9820` branch `lineage-23.2`,
`arch/arm64/configs/exynos9820-beyond0lte_defconfig`
(https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/configs/exynos9820-beyond0lte_defconfig):

- `CONFIG_BCMDHD_PCIE=y` — Broadcom DHD is built in **PCIe mode** (no SDIO WLAN path).
- `CONFIG_PCIE_DW=y`, `CONFIG_PCIE_DW_HOST=y` — Synopsys **DesignWare PCIe host controller** (root port).
- `CONFIG_EXYNOS_PCIE_IOMMU=y`, `CONFIG_PCIE_IOMMU_HISTORY_LOG=y` — Exynos PCIe SMMU is in play.
- `CONFIG_WLAN_BCM4375_ENHANCED_FEATURES` / `CONFIG_WLAN_BCM4375_ADVANCED_FEATURES` entries exist
  (both not set here) → the driver family is explicitly **BCM4375**.
- `CONFIG_BCMDHD_OOB_HOST_WAKE=y`, `CONFIG_BCMDHD_GET_OOB_STATE=y` — out-of-band wake via the
  `wlan_host_wake` GPIO already found in the DT pinctrl.
- Firmware paths `CONFIG_BCMDHD_FW_PATH="/etc/wifi/bcmdhd_sta.bin"`,
  `CONFIG_BCMDHD_NVRAM_PATH="/etc/wifi/nvram_net.txt"` (same as dock-desktop §1).

**Consequence:** the §6 "nothing to attach to" argument was about the *platform glue node* only.
The real device is a PCIe endpoint behind a DesignWare root port — it has BARs, a VID/DID, and a
standard config space. A Windows driver CAN bind to it (as a PCI device or via an ACPI-described
MMIO window). This reopens M12.

### 2026-10-11 — W2/W7: the PCIe root port is `pcie_0` at 0x13ED0000 — full register map (fact, VERIFIED from vendor DT)

From `LineageOS/android_kernel_samsung_exynos9820` @ `lineage-23.2`,
`arch/arm64/boot/dts/exynos/exynos9820.dts` lines 4380-4418
(https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/exynos/exynos9820.dts):

`pcie_0: pcie0@13ED0000`, `compatible = "samsung,exynos-pcie"`, `device_type = "pci"`:

| Window | Address | Size | reg-name |
|---|---|---|---|
| ELBI (port logic glue) | `0x13ED0000` | 0x1000 | elbi |
| PHY (PMA) | `0x13EF0000` | 0x1000 | phy |
| SYSREG | `0x13C11044` | 0x10 | sysreg |
| DBI (RC's own config via DesignWare) | `0x13F00000` | 0x1000 | dbi |
| PHY PCS | `0x13EE0000` | 0x1FC | pcs |
| Config space window (CFG0 iATU target) | `0x14FFE000` | 0x2000 | config |
| I/A sequencer | `0x13EC0000` | 0x1000 | ia |

- Outbound MEM iATU range (DT `ranges`): CPU `0x14000000` ↔ PCI `0x14000000`, size `0xFF0000`.
- `interrupts = <0 INTREQ__PCIE_WIFI0 0>`; `INTREQ__PCIE_WIFI0 = 253`
  (`include/dt-bindings/interrupt-controller/exynos9820.h:267`). Calibration: `INTREQ__USB31DRD_GIC_0 = 241`
  (line 255) equals the repo's VERIFIED DWC3 SPI 241 (`Exynos9820.h:403`), so **SPI = INTREQ** and
  **GSIV = INTREQ + 32 = 285** for PCIe0. interrupt-map type `0x4` = level-high.
- `num-lanes = <1>`, `max-link-speed = LINK_SPEED_GEN1` (Gen1 x1), `ip-ver = 0x982000`,
  `use-sysmmu`/`use-cache-coherency` flipped true by the board dts, `use-msi = "false"` (INTx only),
  `use-ia = "true"`.
- PERST# GPIO: `gpf2-2` (pcie_0 node `gpios`). Board `exynos9820-beyond0lte_common.dtsi:2315-2324`
  enables it: `status="okay"`, `ep-device-type = <EP_BCM_WIFI>`, pinctrl `pcie0_clkreq pcie0_perst pcie_wake cfg_wlanen wlan_host_wake`.
- WLAN glue pins (board dts `wlan` node, lines 718-723): **WLAN_EN = gpg0-5**, **WLAN_HOST_WAKE = gpa0-7**.
- `pcie_1` (0x13120000, Gen3 x2) is the S5100 modem doorbell — disabled on this board, not ours.
- PCIe SysMMU: `sysmmu_pcie: sysmmu@13C20000` +0x9000 (FSYS1), IRQ `INTREQ__SYSMMU_FSYS1_NS`.

### 2026-10-11 — W8: the exact bring-up sequence Samsung runs (fact, from the shipping driver)

`drivers/pci/dwc/pcie-exynos-host-v0.c` (binds `samsung,exynos-pcie`; 3210 lines) + `pcie-exynos-host-v0.h`:

1. Power: set **WLAN_EN (gpg0-5) high, wait 100 ms** (`store_pcie` case 0/1; `pcie,wlan-gpio`), then
   `exynos_pcie_poweron()` (line 2447):
   - enable PCIe clocks (DT `clocks`),
   - PMU reg `PCIE_PHY_CONTROL = 0x071C` (+ch*4 → **PMU+0x71C** for ch0), mask 0x1, write 1
     (PMU base on 9820 = `0x15860000`, already VERIFIED in this repo for UFS),
   - PMU wakeup-mask step is **skipped on 9820** (`#if !defined(CONFIG_SOC_EXYNOS9820)` — "not able to
     use current 9820 SW PMU"),
   - SysMMU enable (board sets use-sysmmu=true), pinctrl default state,
   - `phy_all_pwrdn_clear` (PHY/PCS/SYSREG ops),
   - ELBI history-buffer regs (`PCIE_STATE_HISTORY_CHECK 0x274` ← 0 then ← HISTORY_BUFFER_ENABLE 0x3,
     `PCIE_STATE_POWER_S 0x2BC`=0x200000, `PCIE_STATE_POWER_M 0x2C0`=0xffffffff),
   - `enable_irq`, then `exynos_pcie_establish_link()`.
2. `establish_link` (line 1217), ELBI offsets from `elbi_base`:
   - PCS `IGNORE_ELECIDLE` mode,
   - soft core reset: ELBI `PCIE_SOFT_CORE_RESET 0x1D0` ← 0, udelay(20), ← 1,
   - **#PERST high, sleep 18–20 ms**,
   - ELBI `PCIE_APP_REQ_EXIT_L1_MODE 0xF4` |= bit0 (HW mode) | bit4 (L1_REQ_NAK master);
     ELBI `PCIE_LINKDOWN_RST_CTRL_SEL 0x1B8` ← `PCIE_LINKDOWN_RST_MANUAL (1<<1)`,
   - ELBI `PCIE_QCH_SEL 0x2C8`: clear PMU/APB/AXI gating masks (0xF00/0xF0/0xF) for ip_ver ≥ 0x889500,
   - `assert_phy_reset`: phy clocks, `phy_config(phy,pcs,sysreg,elbi,ch)`, then the **I/A microprogram**
     (use-ia=true): ELBI+0x34 ← 0x10 (TOE enable); IA block (0x13EC0000) base-addr regs
     0x30←0x13ed0000, 0x34←0x13ef0000, 0x38←0x13ee0000, 0x3C←0x13ec0000; IA_CNT_TRG 0x10←0xb, 0x40←0x2ffff;
     SQ0..SQ11 sequence (regs 0x100..0x15C, CDR_EN writes 0xC0→0x80→0x0 at PHY offset 0xD0, waits on
     AFC_DONE); IA enable 0x0←1; then ELBI `PCIE_SW_WAKE 0x018` clear bit1 (bus-number enable).
   - `dw_pcie_setup_rc` + `exynos_pcie_setup_rc` (DBI at 0x13F00000): DBI_RO_WR_EN, RC's own VID/DID ←
     Samsung IDs (`PCI_VENDOR_ID_SAMSUNG`, `PCI_DEVICE_ID_EXYNOS+ch` = 0xecec+0),
     LNKCAP MLW=x1 + SLS 5.0GB (ch0 on 9820), aux clock 26 MHz (`PCIE_AUX_CLK_FREQ_OFF`),
     L1 substates duration, PM_CTRL clear, LNKCTL retrain (RL), LNKCTL2 target speed from DT (Gen1).
   - LTSSM start: ELBI `PCIE_APP_LTSSM_ENABLE 0x02C` ← `PCIE_ELBI_LTSSM_ENABLE 0x1`; poll ELBI
     `PCIE_ELBI_RDLH_LINKUP 0x074` & 0x1f in [0x0d..0x14], 10 µs × 2000; on fail: PERST low, LTSSM
     disable, phy clocks off, retry ×10.
   - Link up: read `PCIE_LINK_CTRL_STAT` (DBI) bits 16-19 = current speed; ELBI `PCIE_IRQ_PULSE 0x000`
     W1C; `PCIE_IRQ_EN_PULSE 0x00C` |= `IRQ_LINKDOWN_ENABLE (1<<30)`.
   - iATU programming via DBI (standard DW ATU regs): region1 = CFG0 window (cpu `0x14FFE000` →
     bus 1:0.0), region0 = MEM (cpu `0x14000000` → bus `0x14000000`, 0xFF0000).
3. Kernel PCI core then enumerates bus 1; dhd binds as a `pci_driver`
   (`dhd_pcie_linux.c:196` id table: **vendor 0x14e4, device ANY, class NETWORK_OTHER**).
4. EP enable pattern (sysfs case 9, line ~935): write `PCI_COMMAND = 0x146` and
   `PCI_BASE_ADDRESS_0 = pp->mem_base` (0x14000000) through the CFG0 window, then `ioremap(0x14000000)`
   and access the EP's BAR0 memory directly. (0x146 decodes to MSE|Parity-Response|**INTx-Disable** —
   the test path masks INTx and never sets BusMaster; the real driver must set MSE|BME and wire INTx
   itself. Review correction: an earlier draft of this note mis-decoded 0x146 as MSE|BME|SERR.)

### 2026-10-11 — W1/W3: chip = BCM4375 (802.11ax), PCI enumeration identity (fact + one UNVERIFIED)

- LineageOS wiki for beyond0lte lists Wi-Fi 802.11 a/b/g/n/ac/**ax** — the 4375 is the ax combo chip
  (https://wiki.lineageos.org/devices/beyond0lte/).
- Vendor dhd: `BCM4375_CHIP_ID = 0x4375`, family IDs `BCM4375_D11AX_ID 0x4475` / `2G 0x4476` / `5G 0x4477`
  (`bcmdhd/include/bcmdevs.h:299-301,520`); 43751 = 0xAAE7 chip / 0x4490-0x4492 PCI.
  `dhdpcie_chipmatch()` accepts PCI device IDs 0x4475/0x4476/0x4477 and raw 0x4375 for this chip
  (`dhd_pcie.c:9167`). **Expected PCI identity: `14e4:4475`** — UNVERIFIED until read from the live
  PCI config space (needs-phone: `/sys/bus/pci/devices/*/device`, or our own DBI/config read).
- MSI: `dhdpcie_chip_support_msi()` returns FALSE for 4375 → **INTx only**, matching DT `use-msi="false"`.
- 4375 firmware RAM base: `CR4_4369_RAM_BASE = 0x170000` (`bcmdhd/include/sbchipc.h:3285`; 4375 shares
  the 4369 value per `dhd_pcie.c:1600-1602`). Mainline brcmfmac agrees for the 43751/43752/4377 family
  (rambase 0x170000, `chip.c:741-743`).

### 2026-10-11 — W4: open-source driver references and their licenses (fact)

- **Vendor bcmdhd** (the driver that actually runs this chip on this board): full source in the shipping
  kernel, `drivers/net/wireless/broadcom/bcmdhd/` (~11.7k lines in `dhd_pcie.c` alone).
  License: **GPLv2 + linking exception** ("Broadcom-WL-IPTag/Open", header of `dhd_pcie.c`).
  https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/net/wireless/broadcom/bcmdhd/dhd_pcie.c
- **Mainline brcmfmac**: **ISC-licensed**, cleaner architecture (common ring + msgbuf + flow rings),
  but **does NOT support chip 4375** — its devid table has 43752 (`0x449d`) but not 4375
  (`pcie.c:2735-2765`, `brcm_hw_ids.h:98`). Use as architecture/protocol reference; bcmdhd is the only
  complete 4375 reference.
- No open-source Windows port of either exists (search: only Linux backports, e.g. reMarkable's). (fact — sampled)

### 2026-10-11 — W5: firmware blobs (fact + needs-phone)

- **linux-firmware has NO `brcmfmac4375-pcie.bin`** — checked WHENCE of the kernel-firmware mirror
  (only `brcmfmac4373.bin` + cyfmac4373-sdio in the 437x space; 4375 absent, consistent with mainline
  not supporting it). https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/WHENCE
- The working firmware for THIS board is on the device: `CONFIG_BCMDHD_FW_PATH="/etc/wifi/bcmdhd_sta.bin"`,
  `CONFIG_BCMDHD_NVRAM_PATH="/etc/wifi/nvram_net.txt"` (defconfig, §W1 above); on Android 12 the real
  directory is `/vendor/firmware/`. Exact filenames/variants on this unit: **needs-phone**
  (`ls /vendor/firmware | grep -iE 'bcmdhd|nvram|4375'`).
- Policy (same as D8 audio rung 2): **never commit the blobs**; the installer copies them from the
  device's own /vendor at install time. Samsung's blobs are proprietary — they stay on-device.

### 2026-10-11 — W6: Windows driver prior art — none exists (fact)

- Microsoft Update Catalog "Broadcom 802.11 network adapter": 110 updates, sampled 6 detail pages —
  Architecture **X86/AMD64 only** (e.g. updateID 2261020f-508f-4dfb-9040-6ec8cb38e556 = Broadcom 5.30.21.0
  for `pci\ven_14e4&dev_4324...`, 2008, X86). No ARM64 Broadcom WLAN package; no 4375 support anywhere.
  https://www.catalog.update.microsoft.com/Search.aspx?q=Broadcom%20802.11%20network%20adapter
- Broadcom's Windows WLAN distribution (Dell/Lenovo BCM_WLAN packs) is x86/x64 only and covers
  PC chips (4350/43602/4366 class), not the 4375. (fact — sampled)
- No open-source Windows/NDIS port of brcmfmac/bcmdhd exists. (fact — searched)
- **Verdict: the Windows driver must be written — an ARM64 NDIS 6.8x Native 802.11 (WDI) miniport whose
  bus+firmware+datapath layers are a port of the bcmdhd/brcmfmac PCIe protocol.** Test-signing is
  acceptable here (unlike the D4 dongle case) because this driver ships inside OUR donation image, the
  same way the HSI2C/UFS/touch drivers are test-signed (M7) — the recipient never installs a driver.

### 2026-10-11 — clocks: PCIe GEN2 gates are CMU_FSYS1 MMIO but ACPM/HWACG-managed (fact + strategy)

- `GATE_PCIE_GEN2_MSTR/PCS/PHY/DBI/APB/SOCPLL` = `CLK_FSYS1_BASE+7..12`
  (`include/dt-bindings/clock/exynos9820.h:206,214-219`).
- The 9820 clock driver is a cal-if/HWACG shim: every PCIe clock is an **ACPM-owned vclk**
  (`drivers/clk/samsung/clk-exynos9820.c:201-208`, `exynos9820_fsys1_hwacg_vclks[]`) — same trap as USB (D1).
- BUT the underlying gate registers are plain CMU MMIO, offsets resolved from
  `cal-if/exynos9820/cmucal-sfr.c` `SFR()` table (method identical to §7.5 of `notes/s10e-live-reads.md`):
  - `CMU_FSYS1 = 0x13C00000 +0x8000` (cmucal-sfr.c:11)
  - PHY_REFCLK_IN gate `+0x2000`, DBI_ACLK `+0x2030`, IEEE1500 SCL_APB_PCLK `+0x2034`, MSTR_ACLK `+0x2038`,
    DRIVER_APB_CLK `+0x203C`, PIPE2 APB `+0x2040`, SLV_ACLK `+0x2044` (lines 1126-1151)
  - QCH regs: APB `+0x3034`, DBI `+0x3038`, MSTR `+0x303C`, PCS `+0x3040`, PHY `+0x3044` (lines 2255-2260)
  - parent muxes in CMU_CMU (0x1A240000): `MUX_CLKCMU_FSYS1_BUS +0x1068`, `MUX_CLKCMU_FSYS1_PCIE +0x1070`
    (lines 135-138); FSYS1 bank's own `MUX_CLK_FSYS1_BUS +0x1000`, USER overrides `+0x100/+0x108` (lines 173-233)
- Manual-override bit convention already verified for this SoC: `_MANUAL` bit 20, `_CG_VAL` bit 21 (§7.5).
- Strategy: same as D1 — audit state at entry; PCIe clocks are NOT enabled by sboot (Android's kernel
  enables them via ACPM at runtime), so our firmware/driver must gate them on via the CMU_FSYS1 MMIO
  writes above. Whether ACPM HWACG objects is UNVERIFIED — first device run answers it.

### 2026-10-11 — P1 closed: the 9820 PCIe PHY ops live in `drivers/pci/dwc/pci-exynos9820_cal.c` (fact)

`exynos_pcie_phy_init()` registers `phy_config/phy_all_pwrdn/phy_all_pwrdn_clear/
phy_check_rx_elecidle` (lines 396-412). For **ch0 (the WiFi port)** the sequence is
(lines 31-127, 177-394), with bases phy=0x13EF0000, pcs=0x13EE0000, sysreg=0x13C11044, elbi=0x13ED0000:

- `phy_all_pwrdn_clear`: PCS+0x100 clear bit6; PCS+0x104 clear bit7; PHY+(0x20*4) ← 0xC0;
  PHY+(0x57*4) ← 0x7E. (ch1 early-return; `exynos_100Mhz_from_socpll_off` is `#ifdef SOCPLL_GATING`
  = off, "already set in bootloader".)
- `phy_config` ch0: SYSREG+0x0 |= bit1 (PCS_HIGH_SPEED); SYSREG+0xC =
  ((v & ~(0xF<<4) & ~(0xF<<2)) | (3<<2)) & ~(1<<1); SYSREG+0xC |= bit12 (lane0 enable);
  ELBI+0x288 pulse 1→0→1 (10 µs steps, pcs_g_rst); write the **48-word CMN table** to PHY+0x0..0xBC
  and the **80-word TRSV table** to PHY+0xC0..0x1FC (values verbatim in the file, "26MHz Gen2 Ch0 HS1");
  optional OTP tune (magic 0x5030) — skipped, driver only applies it if OTP parse succeeds;
  PCS+0xF8 ← 0x70 (tx latency), PCS+0x100 ← 0x87, PCS+0x104 ← 0x50 (refclk out ctrl),
  PCS+0xC: clear bit1 set bit4 (L1SS timeout); ELBI+0x290 pulse (PCIE_MAC rst);
  ELBI+0x28C pulse 1→0→1 with 100 µs tail (PHY PCS&CMN rst); PCS+0xD0 bits[7:6]: 0x3 → 0x2 → 0x0 (CDR rst).
- `phy_check_rx_elecidle(val)`: PCS+0xEC bit3 ← val.
- quirks (line 414+): `device_disable_async_suspend` + `set_dma_ops(exynos_pcie_dma_ops)` — the
  SysMMU DMA layer, our P5 open item.

https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/pci/dwc/pci-exynos9820_cal.c

## Review of the Beyond0LtePcieDxe commit (2026-10-11)

Checked the transliteration line-by-line against v0.c:1217-1341/2447-2506 and
cal.c:31-394: word-vs-byte indexing (Phy[I] = +i*4 ✓, TRSV at word 0x30+I =
byte (0x30+i)*4 ✓), PWRCTL offsets 0x80/0x15C = 0x20*4/0x57*4 ✓, IA base-addr
values ✓, QCH_SEL mask 0xFF0|0xF0|0xF ✓ (PMU/APB/AXI), L1-exit bits 0x1|0x10 ✓,
LNKCAP field masks (SLS 0xF, MLW 0x3F0, L1EL 0x7<<15) ✓, TLS Gen1=1 ✓, aux
26MHz=0x1A ✓, L1SS 0xEA ✓, RL bit5 ✓, ATU region1 CFG0 / region0 MEM with CR2
last ✓, link-up poll window [0x0d..0x14] × 2000 × 10 µs ✓, retry teardown
(PERST low, LTSSM 0) ✓, W1C IRQ_PULSE + linkdown enable ✓.
Found and fixed in-review: (1) `phy_all_pwrdn_clear` was mis-placed inside
establish_link — kernel runs it in poweron BEFORE soft-reset/PERST; moved to
`PowerOnPrep` with the history-arm writes. (2) `MicroSecondDelay` is UefiLib,
not linked — switched to `gBS->Stall` (repo UsbHostDxe convention). (3) CMU
gate/QCH writes were plain writes — now `MmioOr32` (RMW) matching the repo
HSI2C SetRegisterBits convention so autogating bits survive. (4) stray label
typo in PciePhy.c removed.
Deliberate deviations (recorded in-file): no OTP PHY retune (no OTP reader),
no SysMMU (P5), direct CMU gates instead of ACPM RPC (P4), RC's own VID/DID
not rewritten to Samsung IDs (kernel does it for its enumeration bookkeeping),
EP COMMAND = MSE|BME without the test path's INTx mask, PM_CTRL upper half not
zeroed (kernel zeroes the whole dword; conservative here).

## Review of the Exynos9820Wlan skeleton (commit 5746999)

Checked the NDIS 6.20 shape against the WDK model (DriverEntry ->
NdisMRegisterMiniportDriver -> MiniportInitializeEx -> NdisMInitializeAdapter
-> per-adapter Initialize sets REGISTRATION_INFO + GENERAL_INITIALIZATION_
PARAMETERS -> Halt/Unbind/Shutdown), the BAR0 constants against pcie_core.h/
sbchipc.h, and the OID paths for buffer-size honesty. Found and fixed
(follow-up commit): (1) `PNDIS_OID Oid = (PNDIS_OID)...Oid` treated a value
as a pointer (would not compile clean); (2) driver-level ResumeHandler
returned NOT_SUPPORTED, which would wedge any pause/resume cycle before R4 —
now re-checks the handshake state. Recorded, not fixed (needs-windows build
will arbitrate): exact OPTIONAL-parameter shapes of NdisMGetDeviceProperty /
NdisMCompleteRequest were written from memory of the netmini sample; the
first WDK compile is the check. Skeleton honesty checks passed: sends fail
NOT_ACCEPTED, 802.11 OIDs NOT_SUPPORTED, no fake media connect.

## Live reads still queued (needs-phone)

- `ls /sys/bus/pci/devices/` + `cat vendor device` → confirm `14e4:4475` (or 4476/4477).
- `ls /vendor/firmware | grep -iE 'bcmdhd|nvram|4375'` → exact blob names.
- `cat /proc/interrupts | grep -i pcie` → confirm SPI 253 wiring.
- `ls /sys/devices/platform/13ed0000.pcie0/` → link state, L1ss knobs (read-only).

## What this means (design conclusions, marked as inference)

1. The Windows driver binds to the **BCM4375 PCI device** (`PCI\VEN_14e4&DEV_4475` + class fallback).
   For that, the PCIe link must already be trained and the iATU windows programmed — that is the
   **firmware half** (a `Beyond0LtePcieDxe`), exactly the D1 two-halves pattern.
2. Firmware half = transliterate `exynos_pcie_poweron` + `establish_link` (sequence §W8) into a DXE
   driver, then let EDK2's generic PCI stack (PciBusDxe + ECAM or the CFG0 window) enumerate the
   endpoint, so Windows' `pci.sys` sees it natively. The DT's config window `0x14FFE000` is a
   Type-0 CFG0 iATU for dev 1:0.0 — for full ECAM we instead program iATU region1 as CFG0 over a
   4 KiB ECAM window (standard DW practice) and publish it in the DSDT `_CBA` / a PciRoot(0) hierarchy.
3. Windows half = NDIS miniport (WDI ExtSTA) implementing the 4375 PCIe protocol: BAR0 backplane
   window (PCIEBAR0Window indirection), chip/core enumeration (CC + I-PMU + ARMCR4 + SOCRAM + PCIe core),
   firmware+nvram download to RAM base 0x170000, common ring 0, msgbuf control channel, flow-control
   data rings, INTx interrupt. Reference: bcmdhd `dhd_pcie.c` (4375-complete) + brcmfmac (architecture).
4. Risk ladder (each rung verifiable): R1 link-up in UEFI (read VID/DID) → R2 firmware-up in UEFI
   (shared-area version handshake, print chip event) → R3 Windows enumerates the PCI device →
   R4 Windows driver downloads firmware + scan → R5 Native 802.11 connect. R1/R2 are the cheap,
   high-information rungs and need only the boot stream.

## Sources

- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/configs/exynos9820-beyond0lte_defconfig
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/exynos/exynos9820.dts
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/dt-bindings/interrupt-controller/exynos9820.h
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/include/dt-bindings/clock/exynos9820.h
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/pci/dwc/pcie-exynos-host-v0.c (+ .h)
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/net/wireless/broadcom/bcmdhd/dhd_pcie.c (+ dhd_pcie_linux.c, include/bcmdevs.h, include/sbchipc.h, include/bcmpcie.h)
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/clk/samsung/clk-exynos9820.c
- https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-23.2/drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c (+ .h)
- https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c (+ chip.c, include/brcm_hw_ids.h)
- https://wiki.lineageos.org/devices/beyond0lte/
- https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/WHENCE
- https://www.catalog.update.microsoft.com/Search.aspx?q=Broadcom%20802.11%20network%20adapter (+ ScopedViewInline detail pages)
- https://github.com/sonic011gamer/edk2-exynos (trees API scan: zero PCIe/WLAN code)

## Review of commit aaff4b6 (harsh-critic pass, 2026-10-11)

Checked mechanically against the fetched sources: every cited line number
(irqs9820.h:255/267, exynos9820.dts:4380, board dtsi:2315/718, dhd_pcie.c:9167,
dhd_pcie_linux.c:196, brcmf chip.c:741-743, brcm_hw_ids.h:98, cmucal-sfr.c:11,
clk9820.h:206/214-219, ELBI offsets 0x02C/0x074/0x1D0/0x2C8/0x274/0x2BC/0x2C0/0xF4/0x1B8/0x018)
and the value of every "VERIFIED" constant. Found and fixed (this commit):
1. `CR4_4369_RAM_BASE` is at sbchipc.h line **3285**, not 3284 (3284 is CR4_4362 — same value, wrong line).
2. `PCI_COMMAND = 0x146` mis-decoded in the draft as MSE|BME|SERR#; correct decode is
   MSE|Parity|**INTx-Disable** — material, because it means the kernel's test path deliberately
   masks INTx and never sets Bus Master; our driver must do both itself.
3. Calibration anchor is `Exynos9820.h:403` (USBDRD GSIV 273), not :522.
Confirmed correct: SPI=INTREQ mapping (241 anchor), GSIV 285, all seven pcie_0 windows,
MEM range 0x14000000/0xFF0000, Gen1 x1, INTx-only (use-msi=false + chip_support_msi FALSE for 4375),
WLAN_EN gpg0-5 / HOST_WAKE gpa0-7 / PERST gpf2-2, CMU_FSYS1 gate/QCH offsets, PMU 0x71C PHY control,
"9820 skips the PMU wakeup-mask step" guard, I/A microprogram register list.

## Review of commit 7ccc524 (constants)

Checked every new constant against the fetched sources: window addresses/sizes vs
exynos9820.dts:4383-4389, gate offsets vs cmucal-sfr.c:1126-1150, muxes vs :135/:138,
QCH values vs :2255-2259, PMU 0x71C vs pcie-exynos-host-v0.h:223, GPIO block ownership
via a parent-node scan of exynos9820-pinctrl.dtsi (gpf2→13C40000, gpg0→10430000,
gpa0→15850000), envelope arithmetic (0x13F01000-0x13EC0000 = 0x41000), and no
overlap among the 7 new map entries or with DRAM/FB. Found and fixed: the five QCH
**source-line citations** were shifted (values were already correct). Also tried to
close EXYNOS_FSYS1_GPIO_SIZE: pinctrl@13C40000 has **no `reg`** (banks are MFD
children; the size lives in a driver table not found this pass) — stays UNVERIFIED,
0x1000 is a conservative placeholder.

## Dead ends (do not repeat)

- linux-firmware: no 4375 PCIe firmware at all (WHENCE scan).
- GitHub `KernelLinux/linux-firmware` mirror: API/raw 404s — use the gitlab.com/kernel-firmware mirror.
- git.kernel.org cgit: Anubis bot-wall, unusable from scripts.
- grep.app code search: Vercel challenge wall.
- GitHub code search API: requires auth (401).
- CR4_4369_RAM_BASE hunt: not in siutils.h/sbhndarm.h/bcmpcie.h — it is in **include/sbchipc.h:3285**.
- edk2-exynos: no PCIe host code exists (zero hits in full tree) — no UEFI prior art for this controller.
- Windows: no Broadcom WLAN driver for ARM64 on WU (catalog sample), none for 4375 on any arch, no
  open-source NDIS port.
