# D1b — HSI2C to the MAX77705, host-role VBUS sequence, UEFI xHCI prior art

Follow-up to [`notes/d1-usb-host.md`](d1-usb-host.md). Findings appended **as discovered**;
fact/inference marked; sources are URLs; local kernel copies live under `/tmp/opencode/d1/`
(fetched in the D1 pass — grep those first).

## Questions

1. **HSI2C for MAX77705** — which controller is `hsi2c_45`, its MMIO base/size/node name;
   minimal polled master-transfer init (registers, dividers, clock source; is the hsi2c
   clock gate ACPM-owned too?).
2. **MAX77705 host-role VBUS** — exact register sequence for 5 V boost + CC Rp sourcing;
   any force-host/force-otg path that bypasses CC detection; CHG_CNFG_00 0xE vs 0.F rails.
3. **UEFI xHCI on MMIO** — the protocol UsbBusDxe consumes; EDK2 non-PCI XHCI prior art
   (Marvell/NXP/Hisilicon/Renegade); wrapper-vs-own-HCD verdict; DWC3 root-hub port count.

## Progress log

(append below)

### Q1 — HSI2C bus for the MAX77705

- *(fact)* `hsi2c_45` node (`arch/arm64/boot/dts/exynos/exynos9820.dts:3418`, LineageOS
  `android_kernel_samsung_exynos9820` lineage-23.2):
  `hsi2c_45: hsi2c@15D60000`, `compatible = "samsung,exynos5-hsi2c"` +
  **`samsung,usi-i2c-v2`**, `reg = <0x0 0x15D60000 0x1000>`, `default-clk = <200000000>`
  (200 MHz ipclk), `interrupts = <0 INTREQ__USI_CMGP3 0>`,
  `clocks = <&clock DOUT_CLK_USI_CMGP3>, <&clock GATE_USI_CMGP3>`
  (`ipclk_hsi2c`, `gate_hsi2c_clk`), pinctrl `hsi2c45_bus`, fallback bit-bang pins
  `gpio_scl=<&gpm12 0 0x1>` / `gpio_sda=<&gpm13 0 0x1>`, props
  `samsung,check-transdone-int` + `samsung,scl-clk-stretching`, `status="disabled"` in the
  SoC dts (board enables it — the max77705 child lives under `&hsi2c_45` in
  `exynos9820-beyond0lte_if-pmic-max77705.dtsi`).
- *(fact)* **Device-confirmed live**: `notes/s10e-live-reads.md` §2 lists `15d60000` among
  `/sys/devices/platform/` nodes on the shipping S10e — the bus is registered (hence
  clocked+pinned) under Android, so at our handoff the block is very likely already
  enabled. *(inference)* same "leave clocks alone" strategy as the USB block.
- *(fact)* Clocks are ACPM-owned vclk/QCH like USB: `drivers/clk/samsung/clk-exynos9820.c:110`
  `HWACG_VCLK(GATE_USI_CMGP3, USI_CMGP3_QCH, "GATE_USI_CMGP3", "UMUX_CLKCMU_CMGP_BUS", 0,
  VCLK_GATE, NULL)`; `:456` `VCLK(DOUT_CLK_USI_CMGP3, DIV_CLK_USI_CMGP3, …)`. No direct CMU
  gate write from the AP.
- *(fact)* Driver = `drivers/i2c/busses/i2c-exynos5.c` (1515 l, Samsung downstream; local
  copy `/tmp/opencode/d1/i2c-exynos5.c`). Register map (offsets from 0x15D60000):
  `CTL 0x00, FIFO_CTL 0x04, TRAILING_CTL 0x08, CLK_CTL 0x0C, CLK_SLOT 0x10, INT_EN 0x20,
  INT_STAT 0x24, ERR_STAT 0x2C, FIFO_STAT 0x30, TX_DATA 0x34, RX_DATA 0x38, CONF 0x40,
  AUTO_CONF 0x44, TIMEOUT 0x48, MANUAL_CMD 0x4C, TRANS_STATUS 0x50, TIMING_HS1/2/3 0x54/58/5C,
  TIMING_FS1/2/3 0x60/64/68, TIMING_SLA 0x6C, ADDR 0x70, USI_CON 0xC4, USI_OPTION 0xC8`.
  Key bits: `CTL`: MASTER=1<<3, RXCHON=1<<6, TXCHON=1<<7, SW_RST=1<<31;
  `CONF`: AUTO_MODE=1<<31, HS_MODE=1<<29; `AUTO_CONF`: READ_WRITE=1<<16,
  STOP_AFTER_TRANS=1<<17, MASTER_RUN=1<<31, length=bits 15:0; `ADDR`:
  master addr `(x&0x3ff)<<10`, slave addr bits 9:0, master-id bits 31:24;
  `INT_STAT`: TRANSFER_DONE=1<<7 (also TX_ALMOSTEMPTY bit0, RX_ALMOSTFULL bit1, TRAILING
  bit6, NO_DEV bit10); `FIFO_STAT`: TX level bits 6:0, TX_EMPTY bit8, RX level bits 22:16,
  RX_EMPTY bit24; `TRANS_STATUS`: MASTER_BUSY bit17, master state bits 3:0
  (IDLE=0…LOSE=0xc). FIFO depth 16 (`EXYNOS5_FIFO_SIZE`).
- *(fact)* **Minimal polled master init** (from `exynos5_i2c_reset`/`_init`/`_set_timing`,
  lines 376-600):
  1. `USI_CON(0xC4) = 0` — clear USI_RESET (active-high; must be 0 before any transaction).
  2. SW reset: `CTL |= 1<<31`, then `CTL &= ~1<<31`.
  3. Timing (per-mode formulas at 200 MHz ipclk): FS (400 kHz, the default init mode):
     `div = ipclk/(op_clk*15) & 0xFF` → 33; `TIMING_FS3[23:16]=div`;
     `TSCL_H = ((9*ipclk_MHz)/((div+1)*10))` → `TIMING_FS2[7:0] = mask-of-ones(TSCL_H)`;
     `TIMING_FS1[23:16] = mask-of-ones(TSCL_H-1)`. (STAND 100 kHz uses `/16` and `25*`
     constants → div=125; FS+ 1 MHz uses `/15` and `4*`; HS mode HS1/2/3 with `7*`
     constants.)
  4. `CTL = HSI2C_MASTER`; `TRAILING_CTL(0x08) = 0xFFFFFF`;
     `CONF |= AUTO_MODE (1<<31)`; (HS only: set MASTER_ID in ADDR + HS_MODE).
- *(fact)* **Polled transfer** (`exynos5_i2c_xfer_msg`, lines 686-919, POLLING branch):
  `TIMEOUT &= ~(1<<31)`; `FIFO_CTL = RX_EN|TX_EN|trig levels`;
  write: `CTL |= TXCHON, &= ~RXCHON`; `AUTO_CONF &= ~READ_WRITE`;
  `ADDR = (0x7<<24) | (slave<<10)`; `INT_EN = TRANSFER_DONE(1<<7)` (the driver sets this
  bit even in polling mode; the loop reads `INT_STATUS`, and no IRQ handler is registered
  when `samsung,polling-mode` is in effect); `AUTO_CONF = (AUTO_CONF & ~0xFFFF) | len`;
  `AUTO_CONF |= MASTER_RUN(1<<31)`; then loop: while `FIFO_STAT TX level < 16` write bytes
  to `TX_DATA(0x34)`; then poll `INT_STAT` until `TRANSFER_DONE && TX_FIFO_EMPTY`, ack by
  writing back `INT_STAT`. Read: `CTL |= RXCHON`, `AUTO_CONF |= READ_WRITE`, poll
  `FIFO_STAT & RX_EMPTY==0` → drain `RX_DATA(0x38)` until len bytes. Timeout 1 s; on
  timeout dump + full re-init.
- *(fact)* **Our Windows driver already speaks this exact block**:
  `drivers/Exynos9810Hsi2c/exynos9810hsi2c.h` defines the same map (`EHI2C_*` 0x00-0x70 +
  `EXYNOS_USI_CONTROL 0xC4`/`USI_OPTION 0xC8`) and `controller.cpp` implements the same
  divider math (`highShift/startShift/highMask/startMask` → TIMING_FS1/2/3, lines ~340-365)
  plus a snapshot/restore of TIMING/FIFO/CONF/CTL on reset. *(inference)* the DXE
  "tiny polled-write" driver can be a ~150-line transliteration of `xfer_msg`'s POLLING
  branch; even simpler, it only ever needs **write transactions** (PMIC registers) at FS —
  and can copy the boot-timed TIMING_* values verbatim since the block is live at handoff.
- *(open)* `USI_OPTION(0xC8)` mode field (I2C vs SPI vs UART selection) is not written by
  the kernel driver's init path (only USI_CON reset) — the bus is already in I2C mode from
  the bootloader; if we ever find it in another mode, the value to write is in
  `samsung,usi-i2c-v2` handling of the USI driver (not extracted; low priority).

### Q2 — MAX77705 host-role VBUS, exact registers

- *(fact)* **I2C topology** (`drivers/mfd/max77705.c:57-61`, 8-bit addresses): the chip has
  five I2C sub-blocks on hsi2c_45 — PMIC/haptic `0xCC>>1 = 0x66` (the DT node
  `max77705@66`), **charger `0xD2>>1 = 0x69`**, fuelgauge `0x6C>>1 = 0x36`,
  **MUIC/USBC/CCIC `0x4A>>1 = 0x25`**, debug `0xC4>>1 = 0x62`. A DXE driver needs only
  0x69 (boost) and optionally 0x25 (CC state read / mailbox).
- *(fact)* **Charger block registers** (`include/linux/mfd/max77705-private.h:75-99`,
  offsets on client 0x69): `CHG_INT 0xB0, CHG_INT_MASK 0xB1, CHG_INT_OK 0xB2,
  CHG_DETAILS_00..02 0xB3-0xB5, CNFG_00 0xB7, CNFG_01 0xB8, CNFG_02 0xB9, CNFG_03 0xBA …
  SAFEOUT_CTRL 0xC6`.
- *(fact)* **`CNFG_00` (0xB7) bit layout** (`drivers/battery_v2/include/charger/
  max77705_charger.h:117-163`): `MODE[3:0]` is a bitfield, not an index:
  bit0 = CHG (charger), bit1 = OTG/UNO, bit2 = BUCK (system supply), bit3 = BOOST
  (VBUS boost), bit4 = WDTEN. So the mode constants decompose as:
  **0x4 = BUCK only** (safe intermediate), **0xA = BOOST+OTG** (boost on, no system buck),
  **0xC = BUCK+BOOST+UNO**, **0xE = BUCK+BOOST+OTG_ON — 5 V out, charger OFF**,
  **0xF = BUCK+CHG+BOOST+OTG_ON — 5 V out AND charger engine on** (the only difference
  0xE vs 0xF is bit0/charger; `MAX77705_MODE_E_BUCK_BOOST_OTG_ON` vs
  `MAX77705_MODE_F_BUCK_CHG_BOOST_OTG_ON`, lines 146-163).
- *(fact)* **VBUS-on sequence** (`max77705_charger.c:800-856` + `:1384-1500`):
  1. (optional) mask charger interrupts: `INT_MASK(0xB1) |= CHG_I(bit4)|CHGIN_I(bit5)`,
     unmask `BYP_I(bit0)` — `max77705_update_reg(INT_MASK, CHG_IM|CHGIN_IM,
     CHG_IM|CHGIN_IM|BYP_IM)`.
  2. set OTG current limit: `CNFG_02(0xB9)` bits 7:6 `OTG_ILIM`: 0=500 mA, 1=900 mA,
     2=1.2 A, 3=1.5 A (`max77705_charger.h:182-188`).
  3. mode transition: `update_reg(CNFG_00, MODE=0x4, mask 0x0F)` → `usleep_range(1000,
     2000)` → `update_reg(CNFG_00, MODE=0xE (or 0xF), mask 0x0F)`. (The 0x4 hop + 1 ms is
     explicit in the UNO→OTG branches, e.g. `max77705_charger.c` "mode 0x4, and 1msec
     delay, and then otg on".)
  4. OTG-off: mode → 0x4/0x0, `msleep(50)`, restore INT_MASK.
- *(fact)* **CC/USBC block registers** (`max77705-private.h:160-176` +
  `include/linux/ccic/max77705.h:95-175`, client 0x25):
  `UIC_HW_REV 0x00, UIC_FW_REV 0x01, UIC_INT 0x02, CC_INT 0x03, PD_INT 0x04, VDM_INT 0x05,
  USBC_STATUS1 0x06, USBC_STATUS2 0x07, BC_STATUS 0x08, CC_STATUS0 0x0A, CC_STATUS1 0x0B,
  PD_STATUS0 0x0C, PD_STATUS1 0x0D, UIC_INT_M 0x0E, CC_INT_M 0x0F, PD_INT_M 0x10,
  VDM_INT_M 0x11`; command mailbox: AP→CCIC bulk-write at **0x21** (`OPCODE_WRITE`,
  `[opcode][data…]`, terminate with 0x00 at **0x41** `OPCODE_WRITE_END`), CCIC→AP read at
  **0x51** (`OPCODE_READ`). `CC_STATUS0` bits: `CCStat[2:0]` = attach state enum
  (`max77705_cc.h`/`ccic/max77705.h:362-371`: 0=No_Connection, 1=SINK, **2=SOURCE**,
  3=Audio_Accessory, 4=Debug_Accessory, 5=Error…), `CCVcnStat` bit3, `CCIStat[5:4]`
  (source-current advertised when sink: 0=not-in-UFP, 1=500 mA, 2=1.5 A, 3=3 A),
  `CCPinStat[7:6]`. `CC_INT` bits: CCStatI bit0, CCVcnStatI bit1, CCIStatI bit2,
  CCPinStatI bit3, VCONNSCI bit4, AttachSrcErrI bit5, VSAFE0VI bit6, VCONNOCPI bit7;
  init mask `REG_CC_INT_M_INIT = 0x20` (VCONNSCI masked).
- *(fact)* **Who sources Rp**: the CCIC is a *firmware-loaded* Type-C port controller
  (`include/linux/ccic/BOOT_FLASH_FW*.h` blobs; `cc_booting_complete` in the MFD struct).
  The AP does not poke TCPC-style termination registers directly; it sends **opcodes**
  through the mailbox. The CC-control register bits (`include/linux/ccic/max77705.h:188-194`):
  `CCCTRL1`: `CCSrcCurCh bit7, CCSrcCur[6:5] (source current level), CCSrcSnk bit4,
  CCSnkSrc bit3, CCDbgEn bit2, CCAudEn bit1, CCDetEn bit0`. At probe the driver sets
  `op_ctrl1_w = CCSrcSnk|CCSnkSrc|CCDetEn = 0x19` and pushes it with
  `OPCODE_CCCTRL1_W` (opcode enum value 0x0C; `OPCODE_CCCTRL1_R = 0x0B`,
  `max77705.h:526-537`) — i.e. **DRP toggling (Rp↔Rd) + detection enabled**
  (`max77705_usbc.c:3503`). A plain hub/dock (Rd-only UFP) therefore attaches the phone as
  **source automatically** during the Rp phase; firmware then reports
  `CC_STATUS0.CCStat = cc_SOURCE` and the AP's job is only the charger boost (above).
  `auto_vbus_en = false` (`max77705_usbc.c:3496`) — VBUS is AP-controlled, not firmware.
- *(fact)* **Attach→host flow** (`max77705_cc.c:540-730` CCStat IRQ handler +
  `:190-275 notify_dr_status`): `cc_SOURCE` → `max77705_vbus_turn_on_ctrl(ON)`
  (`max77705_pd.c:479`, → psy "otg" ONLINE=1 → `max77705_set_otg`); detach (`ccstat==0`)
  → `vbus_turn_on_ctrl(OFF)`. Data-role for PD partners: `current_dr == DFP` →
  `is_host = HOST_ON` + muic/usb notifier `USB_STATUS_NOTIFY_ATTACH_DFP`; UFP → client
  (device) mode.
- *(fact)* **Force-host paths**: there is **no `force-otg` sysfs** in the max77705 ccic
  stack (grep `-i force` over `drivers/ccic/*.c` + `drivers/muic/max77705-muic.c` finds
  only FW/trim internals; the old MUIC `force-otg` pattern is absent). The programmatic
  role-change path is the kernel TypeC `pr_set` callback →
  `max77705_power_role_change(TYPE_C_ATTACH_SRC)` →
  `max77705_send_role_swap_message(POWER_ROLE_SWAP)` (`max77705_usbc.c:374-385, 662-697`)
  — a **PD PR_Swap**, which needs a PD-capable partner; it does not help with a dumb hub.
  *(inference)* for our DXE: do NOT try to force Rp — leave the boot-time CCCTRL1=0x19 DRP
  config alone, plug the hub, poll `CC_STATUS0` until `CCStat==2`, then run the charger
  boost sequence. If we ever need source-only, the write is mailbox
  `0x21: [0x0C][value]` + `0x41: 0x00` with `value = CCSrcSnk|CCDetEn` (0x11) — untested.
- *(fact)* Mailbox mechanics (`max77705_usbc.c:1706-1746`): `max77705_i2c_opcode_write` =
  `bulk_write(muic, 0x21, len+1, [opcode|data])` then `write_reg(0x41, 0x00)`; response
  arrives via `UIC_INT(0x02) APCmdResI bit7`/`SYSMsgI bit6` + `USBC_STATUS2(0x07)` and is
  bulk-read from 0x51. Opcode enum values: `BCCTRL1_R=0x01, CTRL1_R=0x05, CCCTRL1_R=0x0B,
  CCCTRL1_W=0x0C, CCCTRL2_W=0x0E, CTRLREG3_W=0x1D, CURRENT_SRCCAP=0x30, GET_SRCCAP=0x31,
  SRCCAP_REQUEST=0x32, SET_SRCCAP=0x33, SWAP_REQUEST=0x37` (`max77705.h:521-560`).

### Q3 — UEFI xHCI on memory-mapped registers (EDK2 prior art)

- *(fact)* **Protocol chain in current edk2 master** (tianocore/edk2):
  `MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf`: `TO_START gEfiPciIoProtocolGuid`,
  `BY_START gEfiUsb2HcProtocolGuid`. `MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf`:
  `TO_START gEfiUsb2HcProtocolGuid`, `BY_START gEfiUsbIoProtocolGuid`. So the HCD contract
  UsbBusDxe consumes is **`EFI_USB2_HC_PROTOCOL`** (GUID
  `3e745226-9818-45b6-a2ac-d7cd0e8ba2bc`); the legacy `EFI_USB_HC_PROTOCOL` (1.0) is gone —
  `MdeModulePkg/Include/Protocol/UsbHc.h`/`Usb2Hc.h` **no longer exist in master** (404);
  the header now lives at `MdePkg/Include/Protocol/Usb2HostController.h`
  (https://raw.githubusercontent.com/tianocore/edk2/master/MdePkg/Include/Protocol/Usb2HostController.h).
  `EFI_USB2_HC_PROTOCOL` = 13 entry points (GetCapability/Reset/GetState/SetState/
  ControlTransfer/Bulk/AsyncInt/SyncInt/Iso/…/root-hub port GetStatus/SetFeature/
  ClearFeature) — template `gXhciUsb2HcTemplate` in `XhciDxe/Xhci.c`.
- *(fact)* **Upstream MMIO solution = NonDiscoverable PCI emulation** (this is the exact
  "thin PCI-emulation wrapper" option, already in edk2 core, BSD-2-Clause-Patent):
  - `MdeModulePkg/Include/Library/NonDiscoverableDeviceRegistrationLib.h`:
    `RegisterNonDiscoverableMmioDevice (NonDiscoverableDeviceTypeXhci, DmaType, InitFunc,
    NULL, 1, Base, Size)` installs `EDKII_NON_DISCOVERABLE_DEVICE_PROTOCOL`
    (GUID `0d51905b-b77e-452a-a2c0-eca0cc8d514a`,
    `MdeModulePkg/Include/Protocol/NonDiscoverableDevice.h`) on a new handle.
  - `MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe` binds that protocol and synthesizes a
    **full `EFI_PCI_IO_PROTOCOL`**: fake config space with class code
    `PCI_CLASS_SERIAL / PCI_CLASS_SERIAL_USB / PCI_IF_XHCI (0x0C/0x03/0x30)` for
    `gEdkiiNonDiscoverableXhciDeviceGuid` (`NonDiscoverablePciDeviceIo.c:1830-1836`),
    `Io.Read/Write` → `Mmio*`, `Map/UnMap` with cache maintenance for
    `DmaTypeNonCoherent`, `Attributes` accepted as no-ops. **Stock `XhciDxe` then binds
    unmodified** — `XhciReg.c` only ever uses `PciIo->Io.Read/Write(EfiPciIoWidthUint32)`,
    and `XhciDxe` **never registers a controller interrupt handler** (it polls USBSTS), so
    no IRQ plumbing is needed in UEFI (https://github.com/tianocore/edk2 tree, files above).
- *(fact)* **Two shipping platform precedents:**
  - **Marvell Armada 70x0/80x0/NXP CN913x**: `Platform/Marvell/Armada70x0Db/
    NonDiscoverableInitLib/NonDiscoverableInitLib.c` — `XhciInit()` callback (VBUS GPIOs)
    keyed on `NonDiscoverableDeviceTypeXhci`; registration driven by PCDs
    (`gMarvellTokenSpaceGuid.PcdPciEXhci`, bases `0xF2500000/0xF2510000`, `SIZE_16KB`,
    `DmaTypeCoherent`) — original series: "Enable two xHCI ports for Armada70x0 board"
    (https://lists.linaro.org/archives/list/linaro-uefi@lists.linaro.org/thread/YI4NWDJAR7B6MXG74IEGW3ULNTR4ULKQ/).
    Their ACPI side (Linux/ACPI boot) uses the same DSDT shape we planned for Windows:
    `Device (XHC0) { _HID "PNP0D10", _CCA 1, Memory32Fixed(0xF2500000,0x4000),
    Interrupt(Level,ActiveHigh,Exclusive){94} }`
    (https://github.com/MarvellEmbeddedProcessors/edk2-open-platform/blob/71ae395da1661374b0f07d1602afb1eee56e9794/Platforms/Marvell/Armada/AcpiTables/Armada70x0/Dsdt.asl).
  - **NXP Layerscape**: `Silicon/NXP/Drivers/UsbHcdInitDxe/UsbHcd.c:229` —
    `RegisterNonDiscoverableMmioDevice (NonDiscoverableDeviceTypeXhci,
    NonDiscoverableDeviceDmaTypeNonCoherent, InitializeUsbController(addr), NULL, 1,
    ControllerAddr, PcdUsbSize)` from an EndOfDxe callback, with controller/PHY init in the
    InitFunc — structurally identical to what our `UsbHostDxe` needs (DWC3/PHY init in the
    init callback, then let stock XhciDxe drive)
    (https://raw.githubusercontent.com/tianocore/edk2-platforms/master/Silicon/NXP/Drivers/UsbHcdInitDxe/UsbHcd.c).
- *(dead end)* **Renegade edk2-msm**: no source XHCI driver in the repo (tree search for
  `xhci` finds only ACPI tables); the sdm845 `usb.asl` shows the host stack is Qualcomm's
  proprietary **QcUsbFN** binary behind `Device(URS0){ _HID "QCOM0304"/"QCOM0305", _CID
  "PNP0CA1" (USB role switch mux), _CCA 0, Memory32Fixed(0x0A600000,0xFFFFF) }` with the
  host controller as a bus-enumerated child (`Device(USB0){_ADR 0}`)
  (https://raw.githubusercontent.com/edk2-porting/edk2-msm/master/Platform/Xiaomi/sdm845/AcpiTables/beryllium/usb.asl).
  Not reusable for Exynos; also note `_CCA 0` there (Qualcomm USB DMA is cache-incoherent).
- *(verdict, inference)* **Wrapper path wins decisively**: our DXE driver = D1's init
  sequence (steps 1-6) inside a `NON_DISCOVERABLE_DEVICE_INIT` callback + one
  `RegisterNonDiscoverableMmioDevice(NonDiscoverableDeviceTypeXhci, …, 0x10C00000, 0x8000)`
  call + 3 FDF lines (`NonDiscoverablePciDeviceDxe.inf`, `XhciDxe.inf`, `UsbBusDxe.inf` +
  class drivers). Writing our own `EFI_USB2_HC_PROTOCOL` producer means re-implementing
  rings/TRBs/transfer scheduling (Xhci.c+Sched ≈ 6k lines) — no reason.
  **DmaType**: pick `NonCoherent` — the 9820 downstream DT has `dma-coherent` on UFS/G2D
  nodes but **not** on `usb@10C00000` (grep of `exynos9820.dts`), and the wrapper's Map/
  UnMap then does the cache maintenance XhciDxe expects. *(inference)* confirm with a
  keyboard-enumeration smoke test; if flaky, try Coherent.
- *(fact)* **Root-hub port count**: mainline dwc3 `core.c` (`dwc3_read_port_info` series,
  v6.x): for **DRD** controllers (our case) `dwc->num_usb2_ports = 1; num_usb3_ports = 1`
  (multiport parsing only for host-only cores); the series' sanity check asserts
  `HCS_MAX_PORTS(HCSPARAMS1) == num_usb2 + num_usb3` → a 1×HS+1×SS DRD DWC3 reports
  **MAXPORTS = 2** (HS port + paired SS port). AMD Zynq MPSoC (DWC3-based) documents the
  same formula: `MAXPORTS = DWC_USB3_HOST_NUM_U2_ROOT_PORTS + DWC_USB3_HOST_NUM_U3_ROOT_PORTS`
  (https://docs.amd.com/r/en-US/ug1087-zynq-ultrascale-registers/HCSPARAMS1-USB3_XHCI-Register).
  Xilinx's downstream `dwc3/host.c` shows the exact reads: `HCSPARAMS1 @ xhci_base+0x04`,
  `MAXPORTS = bits 31:24`, `PORTSC(n) = HC_LENGTH(CAPCAPS) + 0x400 + 0x10*(n-1)` — and a
  useful trick for us: **power off all root-hub PORTSC bits right after setting PRTCAP=host**
  to avoid a VBUS glitch before xHCI's own reset
  (https://github.com/Xilinx/linux-xlnx/blob/master/drivers/usb/dwc3/host.c).
  *(inference)* read `0x10C00004` on device to confirm 2; if we run USB2-only (SS PHY down),
  both ports still exist — port 2 (SS) just never gets a connect.

### Sources (this pass)

- Kernel (LineageOS/android_kernel_samsung_exynos9820, lineage-23.2; local copies
  `/tmp/opencode/d1/`): `arch/arm64/boot/dts/exynos/exynos9820.dts` (hsi2c_45 @3418),
  `drivers/i2c/busses/i2c-exynos5.{c,h}`, `drivers/clk/samsung/clk-exynos9820.c`,
  `drivers/mfd/max77705.c`, `include/linux/mfd/max77705-private.h`,
  `drivers/battery_v2/include/charger/max77705_charger.h`, `drivers/battery_v2/max77705_charger.c`,
  `drivers/ccic/{max77705_cc.c,max77705_usbc.c,max77705_pd.c}`,
  `include/linux/ccic/max77705.h`, `include/linux/ccic/max77705_usbc.h`,
  `include/linux/ccic/max77705_cc.h`
- Repo-local: `drivers/Exynos9810Hsi2c/{exynos9810hsi2c.h,controller.cpp}` (already-proven
  HSI2C register programming incl. USI_CON/OPTION and divider math),
  `notes/s10e-live-reads.md` §2 (15d60000 live on device)
- EDK2: https://github.com/tianocore/edk2 master — `MdeModulePkg/Bus/Pci/XhciDxe/*`,
  `MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf`,
  `MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/*`,
  `MdeModulePkg/Include/{Protocol/NonDiscoverableDevice.h,Library/NonDiscoverableDeviceRegistrationLib.h}`,
  `MdePkg/Include/Protocol/Usb2HostController.h`
- edk2-platforms: `Platform/Marvell/Armada70x0Db/NonDiscoverableInitLib/*`,
  `Silicon/NXP/Drivers/UsbHcdInitDxe/*`
- https://lists.linaro.org/archives/list/linaro-uefi@lists.linaro.org/thread/YI4NWDJAR7B6MXG74IEGW3ULNTR4ULKQ/
  (original Marvell PciEmulation series)
- https://github.com/MarvellEmbeddedProcessors/edk2-open-platform/.../Armada70x0/Dsdt.asl
  (PNP0D10 DSDT precedent)
- https://github.com/Xilinx/linux-xlnx/blob/master/drivers/usb/dwc3/host.c
  (HCSPARAMS1/PORTSC reads, root-hub power-off-before-reset trick)
- https://docs.amd.com/r/en-US/ug1087-zynq-ultrascale-registers/HCSPARAMS1-USB3_XHCI-Register
  (MAXPORTS = U2+U3 root ports on a DWC3)
- https://www.spinics.net/lists/linux-usb/msg247620.html +
  https://lists.openwall.net/linux-kernel/2023-05-17/1091 (dwc3 multiport/HCSPARAMS1 series)
- https://raw.githubusercontent.com/edk2-porting/edk2-msm/master/Platform/Xiaomi/sdm845/AcpiTables/beryllium/usb.asl
  (Qualcomm QcUsbFN/URS ACPI — dead end for reuse)
- Dead ends this pass: `MdeModulePkg/Include/Protocol/UsbHc.h` + `Usb2Hc.h` 404 in edk2
  master (moved to MdePkg/Usb2HostController.h; legacy USB_HC path removed);
  `linaro-edk2/edk2-msm` repo path 404 (it is `edk2-porting/edk2-msm`); no source XHCI in
  edk2-msm; `drivers/muic/max77705-muic*.c` have no force-otg sysfs.
