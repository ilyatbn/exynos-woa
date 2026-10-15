# Bluetooth on the S10e — research + Windows driver rebuild

Task: give the Beyond0LTE (SM-G970F) port a Bluetooth stack path. Research the chip,
find Linux/open-source drivers, rebuild a Windows equivalent.
Started 2026-10-12. Follows M12/D4b (WLAN = BCM4375 on PCIe0, `notes/m12-wifi-chip.md`).

## Progress log

### 2026-10-12 — B1: chip identity from local dumps (fact)

Sources: `dumps/hhxj6/base.dts`, `dumps/hhxj6/ov00.dts` (decompiled dtbo),
`dumps/hhxj6/kernel` (strings), `dumps/hhxj6/vendor/vendor.raw` (ext2, read via `debugfs`).

- **The BT side of the same BCM4375 combo chip** the WLAN work found (M12). The
  `bluetooth` DT node: `compatible = "samsung,bcm43xx"` (ov00.dts:2156), status okay.
- Kernel has a **built-in vendor driver `bcm43xx_bluetooth`** — strings
  `[BT] bcm43xx_bluetooth_probe.`, `bcm43xx Bluetooth`, `bbd_patch`, `bbd_urgent_patch`,
  `bthostwake_gpio`, `btwake_gpio`, `bus:hostwake_oob`. (EDL strings belong to UFS, not BT.)
- **Transport = UART**, not PCIe-attached and not SDIO:
  - alias `usi_uart_bt = "/usi@10821000"` (base.dts:10409) — USI v2 mode register, 4 bytes.
  - `uart@10840000` (base.dts:7854): `compatible = "samsung,exynos-uart"`,
    `samsung,usi-serial-v2`, `samsung,separate-uart-clk`, `samsung,rts-gpio-control`,
    `samsung,rts-trig-level = <1>`, `samsung,fifo-size = <0x100>` (256-byte FIFO),
    reg size 0x100, pinctrl names `default/rts/tx_dat/tx_con`. Alias `uart1`/`serial_1`.
    This is the **PERIC1 BT UART** — matches kernel clock strings
    `CLK_BLK_PERIC1_UID_RSTNSYNC_CLK_PERIC1_UART_BT`, `GATE_UART_BT`,
    `DBG_NFO_QCH_CON_UART_BT_QCH` (QCH = query/change handshake, same pattern as UFS/HSI2C).
  - base status is `disabled`; the dtbo overlay sets `usi_v2_mode = "uart"` + okay
    (ov00.dts fragment@63 — target phandle garbled, but the i2c/spi/uart triple at
    fragments 61-63 is the USI mode overlay set).
- **Wake GPIOs** (from the model overlay fragment):
  - `bt-hostwake`: pinctrl@15850000, pin **gpa2-3** (ov00.dts:1943) — chip→host wake.
  - `bt-btwake`: pinctrl@10430000, pin **gpg2-0** (ov00.dts:2146) — host→chip wake.
  - `bluetooth` node `gpios` property is garbled in this decompile (phandle confusion);
    the real vendor DTS must be read from source (next task) — UNVERIFIED which entries
    are REGON/REGOFF vs clock.
- **Patchram files live on the phone** at `/vendor/firmware/` (ext2 listing via debugfs):
  - `bcm4375B0_murata.hcd` (50,723 B), `bcm4375B0_semco.hcd` (53,793 B),
    `bcm4375B1_murata.hcd` (176,645 B), `bcm4375B1_semco.hcd` (176,645 B),
    `bcm4375B1_semco_sem.hcd` (176,658 B).
  - Extracted copies: `/tmp/opencode/bt/*.hcd` (session scratch — decide later whether to
    commit them under `firmware/` or `dumps/`).
  - **Not Intel-HEX**: the file starts with an ASCII `BRCMcfg` container header
    (`BRCMcfgS`/`BRCMcfgD` sections) and embeds `BCM4375B1 Beyond E43 Ant0 [Baseline: 0173]`
    and `BCM4375B0 Samsung UART 37.4 MHz ...` — i.e. Samsung/Broadcom binary patch format,
    NOT the Intel-HEX .hcd that mainline `btbcm` patchram expects. Format analysis next.
  - S10e module vendor: the "Beyond E43" strings point at **SEMCO E43** (Murata 1RH
    variants exist for other SKUs). WiFi nvram strings name `SWB-E43` SEMCO module.
- **Chip rev**: BCM4375 **B0 and B1** patch files both present; which one the S10e part is
  comes from the chip ID read at runtime (LMP subversion / `bcm4375B1` baseline strings
  suggest B1 for beyond0lte EU — UNVERIFIED, confirm from `bts_update`/nvram or live read).

### 2026-10-12 — B2: vendor driver source found; patch file format decoded (fact)

Kernel source mirror: `GoRhanHee/android_kernel_samsung_exynos9820` branch `gorhanhee`
(S10/S10+/S10e/BeyondX exynos9820 kernel release). Key files:

- `drivers/bluetooth/bcm43xx.c` (GPL-2.0, Samsung+Google 2011) — the `samsung,bcm43xx`
  platform driver. **It is only GPIO + rfkill + LPM glue, not an HCI transport:**
  - DT `gpios` order: **[0] = bt_en (power enable), [1] = bt_wake (host→chip, output),
    [2] = bt_hostwake (chip→host, input + IRQ)**.
  - Power on = rfkill `set_block(false)`: `bt_en=1`, `msleep(100)`; power off = `bt_en=0`.
  - LPM: `s3c2410_serial_wake_peer[BT_UPORT=1] = bcm_bt_lpm_exit_lpm_locked` — the s3c
    serial driver calls this before TX to pulse `bt_wake` high; a 1 s hrtimer drops it.
    `BT_UPORT 1` = **UART port 1** = `uart1` = `uart@10840000` — confirms the BT UART.
  - hostwake ISR: `bt_hostwake` rising-edge IRQ, re-arms on both edges; hostwake asserted
    = chip has data pending (classic Broadcom out-of-band wake).
- **`CONFIG_BT_HCIUART is not set`** in `arch/arm64/configs/exynos9820-beyond0lte_defconfig`
  (only `CONFIG_BT=y`, `CONFIG_BT_BCM43XX=y`). So on stock Android the HCI transport runs
  in **userspace** (Samsung's BT HAL opens the ttySAC1-equivalent directly). The kernel
  never speaks HCI to the chip.
- **Patch file format decoded** (`bcm4375B1_semco.hcd`, 176,645 B): a raw stream of
  HCI command records `opcode u16 LE | plen u8 | params` — 1260 × `0xFC4C`
  (Broadcom WRITE_RAM: params = `addr u32 LE` + ≤251 B data) then one `0xFC4E`
  (LAUNCH_RAM, addr `0xFFFFFFFF`) as the last record. First two WRITE_RAM records load
  a `BRCMcfgS`/`BRCMcfgD` config blob at 0x00240000. **This is exactly the stream format
  mainline `btbcm_patchram()` replays** (drivers/bluetooth/btbcm.c:211) — after it sends
  `0xFC2E` (start download / minidriver) itself and before its 250 ms post-launch delay.
  So the phone's own `.hcd` files are directly consumable by a btbcm-style loader.
  (Corroborated in the wild: linux-firmware `brcm/BCM4375C2.hcd` is the same BRCMcfg style
  — https://forum.khadas.com/t/bluetooth-hardware-broken/22404)

### What is NOT yet known (each needs a source, not a guess)

- B1-1: exact vendor DTS for `bluetooth` + `uart@10840000` (clean, not garbled):
  regulator names, clock (37.4 MHz XTAL seen in patch string), baud rates, flow control.
- B1-2: UART interrupt GSIV for uart1/PERIC1 (garbled `interrupts` in decompile).
- B1-3: the power-on sequence (REGON/REGOFF/LDOs, clock enable, reset timing).
- B1-4: `BRCMcfg` patch file format (how `bbd_patch` consumes it).
- B1-5: HCI framing used by Samsung's stack: H:4 vs H:5 (hostwake_oob hints OOB wake,
  not necessarily H:5), and the baud-rate switch command after patchram.
- B1-6: whether Windows 11 ARM64's Bluetooth stack can bind a UART HCI transport at all
  (it can — WDK `uartbtx` sample exists; confirm the interface name).

**Closed by B2/B3 below** — B1-1/B1-2/B1-3 closed by the raw-DTB walk + `bcm43xx.c`;
B1-4 closed by the record-chain parse; B1-5 closed by mainline `hci_bcm` (H4 + 0xFC18);
B1-6 closed by `serialhcibus` + `cywbtserialbus` (BthxDDI).

### 2026-10-12 — B3: exact platform constants (fact, from raw DTB walk + vendor drivers)

Method: wrote a minimal FDT walker (`/tmp/opencode/bt/dtbwalk.py`) to read the shipped
binaries directly — the repo's `.dts` dumps garble phandles, the raw DTBs do not.

- **BT UART controller**: `uart@10840000`, reg **0x10840000 size 0x100**,
  `samsung,usi-serial-v2`, fifo-size 0x100, `samsung,rts-gpio-control`,
  `samsung,rts-trig-level=1`. **Interrupt `<0 0x198 0>` = GSIV 408** (same 3-cell
  format as PCIe0's `<0 0x11d 0>` = GSIV 285, M12).
- **USI v2 mode register**: `usi@10821000`, reg **0x10821000 size 4**; select UART by
  writing **1** (`UART_SW_CONF = 1<<0`, `drivers/soc/samsung/usi_v2.c`:36,100).
- **USI UART quirks** (`include/linux/serial_s3c.h` + `drivers/tty/serial/samsung.c`):
  `USI_CON` at UART base **+0xC4**: write 1 (SET_RESET) then 0 (release reset) before
  use; `USI_OPTION` at **+0xC8**: set bit1 `CLKREQ_ON` (clear bit2 CLKSTOP_ON).
  Register layout otherwise the classic s3c map the SerialPortLib already uses
  (ULCON 0x00, UCON 0x04, UFCON 0x08, UTRSTAT 0x10, UERSTAT 0x14, UTXH 0x20,
  URXH 0x24, UBRDIV 0x28, + UFRACVAL).
- **Clocks** (cells in `uart@10840000 clocks`, clock-controller phandle 0x2):
  `gate_uart_clk1` = **CMU_PERIC1 gate id 0x22a (554)**, `ipclk_uart1` = **0x23c (572)**.
  CMU_PERIC1 base 0x10800000 (already in `Exynos9820.h` §CMU list). Gate-offset math is
  the same id→(bank,bit) pattern M12 used for CMU_FSYS1.
- **Pin mux** (all from base.dtb pinctrl states):
  - UART signals on **ALIVE block pinctrl@15850000** (`EXYNOS_ALIVE_GPIO_BASE`):
    `uart1-bus` = **gpq0-3, gpq0-2, gpq0-1, gpq0-0** function **2** (UART), pull-up;
    `uart1-bus-rxd` = gpq0-0 (input); `uart1-bus-rts` = **gpq0-2 function 1 (GPIO out),
    pin-val 1** — the `rts-gpio-control` pin; tx_dat/tx_con/tx_input all on gpq0-1
    (Samsung's TX contention scheme — for a first bring-up, function 2 is enough).
  - `bt-btwake` (host→chip wake) = **gpg2-0** on PERIC0 `pinctrl@10430000`, function 1,
    `pin-con-pdn = 3` (deep-sleep pad control).
  - `bt-hostwake` (chip→host wake) = **gpa2-3** on ALIVE `pinctrl@15850000`.
- **BT power-enable (bt_en)**: in OUR firmware's model overlay (dtb00/01) `bluetooth`
  `gpios[0]` = **PCAL6524 I2C GPIO expander, address 0x22 on HSI2C bus 25**
  (`__fixups__ [hsi2c_25]` covers `fragment@120:target:0`; base alias
  `hsi2c25 = "/hsi2c@10930000"` — the same HSI2C family the `Exynos9820Hsi2c` Windows
  driver already serves; D8 audio used hsi2c_38@109A0000), **expander pin 7**
  (`gpio_start 0x12c=300`, so Linux gpio 307). Expander reset pin = **gpg4-0**
  (PERIC0, out, val 1, via `__fixups__ [gpg4]`).
  Older kernel-source overlays (mirror rev 25) instead fix `gpios[0]` to symbol
  **gpm8** (block `pinctrl@15C30000`) and hang the expander on **speedy** — variant
  difference; our firmware's dtb00 is authoritative for this phone.
- `bluetooth` `gpios[1]` (bt_wake) is **0xffffffff with no fixup in our dtb00** — the
  AP-side glue never drives it (its pinctrl state gpg2-0 is still applied via
  `pinctrl-0`). `gpios[2]` (bt_hostwake) **is** fixed up: `__fixups__ [gpa2]` covers
  `bluetooth:gpios:24` → `<&gpa2 3 0xf>` = gpa2-3 on ALIVE. (Inference, marked: the
  `pin-con-pdn` + SSP strings suggest the sensor-hub MCU may own the wake pads during
  Android sleep; not needed for Windows.)
- **GPS red herring closed**: `bbd_patch`/`bbd_urgent_patch` kernel strings belong to
  `drivers/sensorhub/brcm/bbdpl` — the **BCM4772 GNSS** boot-download over the sensor
  hub SPI, not Bluetooth.

### 2026-10-12 — B4: the open-source equivalents + Windows path (fact)

Linux (reference for behavior, GPL — do NOT copy code into the Windows driver):
- `drivers/bluetooth/hci_bcm.c` — BCM43xx-over-UART: **H4 framing** (`BT_HCIUART_BCM
  select BT_HCIUART_H4`), RTS/CTS flow control required, power via GPIO/regulator,
  `bcm_setup` → `btbcm_initialize` → patchram → init/oper speed → optional hostwake IRQ
  + sleep. Baud switch: **0xFC45** (1-byte clock setting, type=48 MHz) when >3 Mbaud,
  then **0xFC18** with `u16 zero + u32 baud`, then host UART re-set (hci_bcm.c:178-226).
- `drivers/bluetooth/btbcm.c:211 btbcm_patchram()` — sends **0xFC2E** (start download),
  replays the .hcd as `opcode u16 LE | plen u8 | params` records, 50 ms after 0xFC2E,
  250 ms after completion. Our phone's `.hcd` files are exactly this stream (B2).

Windows (open source, MS-PL — code MAY be used):
- **`microsoft/Windows-driver-samples/bluetooth/serialhcibus`** — KMDF bus driver
  implementing the **Bluetooth Extensibility transport DDIs** (`bthxddi.h`:
  IOCTL_BTHX_SET_VERSION / GET_VERSION / QUERY_CAPABILITIES / READ_HCI / WRITE_HCI;
  BTHX_CAPABILITIES, BTHX_HCI_READ_WRITE_CONTEXT, BTHX_HCI_PACKET_TYPE) over a UART
  speaking H4. bthport.sys (the in-box stack) binds the PDO. Vendor hooks live in
  `WDK/device.c`: `DeviceInitialize/DeviceEnable/DevicePowerOn/DeviceEnableWakeControl`.
  INF binds a hardware ID like `ACPI\<vendor>_BTH0` from the DSDT `_HID`.
- **`worproject/cywbtserialbus`** (MS-PL) — serialhcibus adapted for Cypress/Broadcom
  combo chips: loads `BCMxxxxx.hcd` from the driver package, `BcmHciEnterFwDownloadMode`
  (0xFC2E), replays records, `BcmHciSetBaudRate` (0xFC18, baud u32 at Command[5..8], i.e. param offset 2 after the u16 zero),
  registry `BaudRate`/`SkipFwDownload`. **Caveat**: it drives the UART through the
  Windows serial stack (`SERIAL_BAUD_RATE` IOCTL) — impossible for us because the
  Samsung UART is not 16550-compatible and has no in-box serial function driver.
  Our design: the transport driver owns the UART **MMIO directly** (same pattern as
  Exynos9820Wlan mapping CFG0/MEM windows from `_CRS`).

### 2026-10-12 — review of the research commit (80e8aa6)

Checked mechanically against the binaries/sources cited. Found and fixed (this commit):
- **substantive**: "PCAL6524 on speedy" was wrong for OUR firmware — that was the
  mirror's rev-25 `__fixups__`; our dtb00 `__fixups__ [hsi2c_25]` covers
  `fragment@120:target:0` → bus is **hsi2c@10930000**. Also split the gpios verdict:
  `gpios[2]` IS fixed up (gpa2), `gpios[1]` is not (stays 0xffffffff).
- citations: `btbcm_patchram` is btbcm.c:**211** not 213; cywbtserialbus baud sits at
  Command[5..8] (param offset 2), not "offset 3".
Verified unchanged: GSIV 0x198=408 (dtbwalk of base.dtb); USI mode=1 (usi_v2.c:36);
USI_CON/OPTION +0xC4/+0xC8 (serial_s3c.h:46-47); 1260×0xFC4C + 1×0xFC4E record chain
(python parse, end offset == file size exactly); defconfig `# CONFIG_BT_HCIUART is not
set`; gpio_start 0x12c; expander reset gpg4-0 via `__fixups__ [gpg4]`.

### 2026-10-12 — B5: session commit ladder + per-commit review log

| Commit | Task | Review verdict |
|---|---|---|
| `80e8aa6` | research notes + D4c milestone rows | 3 findings → fixed in `5f6931a` (see review section above) |
| `5f6931a` | research review fixes | re-verified: `__fixups__` entries re-grepped from dtb00, no further issues |
| (constants) | `Exynos9820.h` BT block + 3 memory-map entries | verified each macro against its cited read (dtbwalk outputs above); Linux verification build exit 0, FD 7 340 032 B, FVMAIN_COMPACT 1 296 B free; found: milestone row for constants was not moved in the same commit → corrected late in the design commit (rule 5 note) |
| (design) | `D4c-BT-DESIGN.md` | cross-checked: EINT19/GSIV 56 for gpa2-3 against the DSDT GPIO table; `hci_uart_has_flow_control` refusal in `bcm_open` ✓; USI_CON/OPTION inside the 0x100 window ✓ |
| (DSDT) | `Device (BTH0)` | iasl 0 errors; `Dsdt.aml` contains `BTH0`+`SAMS9826` (byte grep); review found 10 ms debounce on the wake line → fixed in follow-up (`0xFFFF`), IRQ trigger convention comment added |
| (skeleton) | `drivers/Exynos9820Bt/` | self-review before commit caught: invented `WdfDeviceQueryResourceList`/`WdfDeviceSetManagedHardwareIntervalsDefault` calls and a double-`WdfDeviceCreate` retry path — all removed; INF registry section fixed to `[Bt_Install.NT] AddReg`; `BtUartInit` guarded on `UartWindow != NULL` so the skeleton can never touch an unmapped window; not compiled (needs WDK) — recorded as such |

Follow-ups opened by reviews (all in the D4c table): firmware-half DXE row
added; Q1 live DT read; R1 closes the UART source-clock UNVERIFIED.

### Sources (B3/B4)

- local: `dumps/hhxj6/base.dtb`, `dumps/hhxj6/dts/dtb0*.dtb` (via `dtbwalk.py`),
  `dumps/hhxj6/vendor/vendor.raw` (debugfs), extracted `/tmp/opencode/bt/*.hcd`
- https://raw.githubusercontent.com/GoRhanHee/android_kernel_samsung_exynos9820/gorhanhee/drivers/bluetooth/bcm43xx.c
- .../drivers/bluetooth/Makefile, Kconfig; .../arch/arm64/configs/exynos9820-beyond0lte_defconfig
- .../drivers/soc/samsung/usi_v2.c; .../include/linux/serial_s3c.h; .../drivers/tty/serial/samsung.c
- .../drivers/sensorhub/brcm/bbdpl (GPS, not BT); .../drivers/gpio/gpio-pcal6524.c
- https://raw.githubusercontent.com/torvalds/linux/master/drivers/bluetooth/btbcm.c
- https://raw.githubusercontent.com/torvalds/linux/master/drivers/bluetooth/hci_bcm.c
- https://github.com/microsoft/Windows-driver-samples/tree/main/bluetooth/serialhcibus
- https://github.com/worproject/cywbtserialbus
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/bthxddi/
- https://forum.khadas.com/t/bluetooth-hardware-broken/22404 (BRCMcfg format corroboration)
- https://github.com/CruelKernel/samsung-exynos9820 (rev-25 DTS variant, gpm8 bt_en)

### Open questions carried into the design (each has a named closing read)

- Q1 which model overlay is active on our phone (dtb00/01 = PCAL6524 pin 7 on hsi2c_25;
  dtb02-06 = all three gpios cells null) — live read of
  `/proc/device-tree/bluetooth/gpios` (24 bytes) + `/proc/device-tree/__symbols__`
  presence, TWRP root. Until then the design must not hardcode the bt_en line: the
  first slice probes ROM HCI at 115200 assuming the chip is already powered, and the
  expander path is a named follow-up rung, not a guess.
- Q2 init/oper baud for this module: start 115200, **do not** switch oper baud in the
  first slice (UNVERIFIED; hci_bcm DT variants use 3_000_000/3_686_400 for 43xx).
- Q3 does the ROM answer HCI at all before patchram (SkipFwDownload path) — device rung.
- Q4 SCO/eSCO over UART (BCMs support HCI_SCO over UART via 0xFC1C/PCM config) — later
  rung; first slice is LE + ACL only.
- Q5 who owns gpg2-0/gpa2-3 pads at Windows boot (sboot/SSP) — pad retention
  `pin-con-pdn` says the pad config changes in sleep; our driver re-muxes explicitly.

### 2026-10-15 — B6: resume on main; clock + pin constants for the UEFI half (fact)

Sources: LineageOS android_kernel_samsung_exynos9820 @ lineage-22.2, raw files fetched 2026-10-15
(include/dt-bindings/clock/exynos9820.h, drivers/clk/samsung/clk-exynos9820.c, drivers/soc/samsung/cal-if/
exynos9820/cmucal-sfr.c + cmucal-node.c, drivers/pinctrl/samsung/pinctrl-exynos-arm64.c).
- DT clock cells: 0x22a = GATE_UART_BT (CLK_PERIC1_BASE 530 + 24; HWACG_VCLK on UART_BT_QCH), 0x23c =
  DOUT_CLK_PERIC1_UART_BT (530 + 42; VCLK_DIV_CLK_PERIC1_UART_BT) - the divider is the UART's rate source.
- CMU_PERIC1 (0x10800000) registers: PLL_CON0_MUX_CLKCMU_PERIC1_UART_BT_USER +0x1C0 (MUX_SEL bit4, BUSY bit7;
  parents [0] OSCCLK_PERIC1, [1] CLKCMU_PERIC1_IP), CLK_CON_DIV_DIV_CLK_PERIC1_UART_BT +0x1814 (DIVRATIO [3:0],
  BUSY bit16), GOUT UART_BT IPCLK +0x2098, PCLK +0x209C, RSTNSYNC UART_BT clk +0x2020 (CG_VAL bit21, MANUAL bit20),
  QCH_CON_UART_BT_QCH +0x3038 (ENABLE bit0).
  => Mux 0 + divider /1 clocks the UART from OSCCLK = 26 MHz (the same oscillator as CNTFRQ/WDT), so the UEFI half
  needs no source-clock measurement: 115200 = UBRDIV 13, UFRACVAL 2 (26e6/16/115200 = 14.106).
- Pin banks (exynos9820_pin_banks*): ALIVE pinctrl@15850000: gpa2 +0x040, gpq0 +0x0A0 (bank_type_alive);
  CMGP pinctrl@15C30000: gpm8 +0x100 (alive type, 1 pin); PERIC0 pinctrl@10430000: gpg2 +0x0C0 (bank_type_off).
  Register layout per bank: CON +0x0 (4 bits/pin), DAT +0x4 (1 bit/pin), PUD +0x8 (4 bits/pin), DRV +0xC.
- bt_en on THIS phone = gpm8-0 (Q1 live read, parkinglot/bluetooth-d4c.md), not the PCAL6524 expander.
- ACPI ID: SAMS9826 is BAT0 on main -> BT moves to SAMS9829 (DSDT BTH0 + Exynos9820Bt.inf).
- GSIV: 440 (DT SPI 408 + 32; corrected on origin/drivers 2026-10-13); this file's B3 "408" is the raw cell.
- Beyond0LteBtUartDxe (BEYOND0LTE_BT_UART_PROBE, default 0; progress guard 0x6B): CMU_PERIC1 UART_BT mux->OSC,
  div /1, IPCLK/PCLK/RSTN gates MANUAL|CG_VAL, QCH ENABLE|CLOCK_REQ; gpq0-0..3 fn2, gpg2-0 out high; USI mode 1,
  USI_CON 1->0, USI_OPTION CLKREQ_ON; 115200 8N1 (UBRDIV 13 / UFRACVAL 2), FIFOs on, polling, UMCON RTS asserted.
  R1 = internal loopback of 32 bytes, timed against CNTVCT (2778 us expected = checks the 26 MHz clock).
  R2 = bt_en gpm8-0 low 20 ms -> high, 150 ms, then HCI_RESET + HCI_READ_LOCAL_VERSION, raw replies hex-logged.
  Builds with the switch 0 and 1. Device run next (image boot-bt1).
- **R1 + R2 PASSED on the device (boot-bt1.img 39712cfb, 2026-10-15 12:42):** CMU before: mux 0x10 (TOP source),
  gates CG_VAL only, QCH 0x3F0002 -> after: mux 0 (OSC), gates 0x300000, QCH 0x3F0003. gpq0 con already 0x02220000
  (pins 1-3 fn2 from sboot? pin 0 was 0), pud 0x03331111. R1 loopback 32/32 bytes, 2780 us vs 2778 expected ->
  UART clock = 26 MHz CONFIRMED (closes BT_UART_SRC_CLK_HZ). R2 after bt_en gpm8-0 pulse: no spontaneous bytes;
  HCI_RESET -> 04 0E 04 01 03 0C 00 (status 0); READ_LOCAL_VERSION -> 04 0E 0C 01 01 10 00 09 00 00 09 0F 00 11 11
  = HCI ver 9 (BT 5.0), rev 0, LMP ver 9, manufacturer 0x000F (Broadcom), LMP subver 0x1111 (ROM, unpatched).
  UMSTAT 0x11 after power-up (CTS asserted by the chip). => Q1 (bt_en = gpm8-0) and Q3 (ROM answers pre-patch)
  CLOSED on hardware. Flow control was off (UMCON RTS asserted manually) and 115200 worked.
- Patch files pulled to s10e-artifacts/bt-hcd (outside the repo, never committed): B0 murata/semco, B1 murata,
  B1 semco ("BCM4375B1 Beyond E43 Ant0 [Baseline: 0173]", 176645 B, 1260x 0xFC4C + 0xFC4E, parses cleanly),
  B1 semco_sem ("BCM4375B1 Beyond SEMCO BT EPA E43 Ant0"). No /vendor/etc/bluetooth config on this firmware; which
  of the two semco B1 files Android picks is still unknown - R3 starts with bcm4375B1_semco.hcd.
- R3 in the DXE: at ReadyToBoot (ESP readable) load \S10EDIAG\bt\bcm4375B1_semco.hcd, HCI_RESET, 0xFC2E, 50 ms,
  each record as a command waiting for Command Complete/Status, 250 ms, HCI_RESET, READ_LOCAL_VERSION. WDT reloaded
  every 100 records. Builds with the probe on.
- Windows half plan (2026-10-15): adopt microsoft/Windows-driver-samples bluetooth/serialhcibus (Microsoft copyright
  headers kept; repo MS-PL) and replace only its lower edge (map: subagent read of the sample, fetched 12:51):
  - drop: FdoOpenDevice / WdfIoTarget (resource-hub UART connection), ReadH4Packet + completion pump, ReadRequest/
    ReadBuffer, FdoWriteToDeviceSync, CR_WriteDeviceIO / write cancel handshake.
  - FdoFindConnectResources -> take the _CRS Memory (MmMapIoSpaceEx) + Interrupt (WdfInterruptCreate ISR+DPC).
  - DeviceInitialize (WDK/device.c vendor hook, runs in PrepareHardware before the RX pump) -> UART set-up (USI,
    115200 8N1, FIFOs) + R2 probe + R3 patchram with our own sync TX/RX (the UEFI DXE's proven sequence).
  - FdoWriteDeviceIO -> copy H4 bytes (&Ctx->Type, 1 + DataLen) into the TX FIFO, complete the request.
  - RX: ISR drains the FIFO into a ring, DPC feeds ReadH4PacketReassemble (kept unchanged, DISPATCH-safe), which
    completes READ_HCI requests through ReadRequestComplete (kept).
  - D0Entry/D0Exit/SelfManagedIoInit -> enable/disable RX interrupt, ReadSegmentStateSet(GET_PKT_TYPE) + RX flush.
  - kept as is: PDO child SerialBusWdk\UART_H4 with compatible ID MS_BTHX_BTHMINI (in-box BT stack binds it),
    IOCTL_BTHX_* handlers in FdoIoQuDeviceControl, capabilities from HlpInitializeFdoExtension.
  - INF: bind ACPI\SAMS9829 (DSDT BTH0, MEM 0x10840000 + Interrupt 440; no GpioInt - main has no GPIO controller).
- W2 (Windows lower edge, builds): BtUart.c/.h - _CRS memory (MmMapIoSpaceEx, >= 0x100) + interrupt (WdfInterruptCreate
  in PrepareHardware); BtUartBringUp in the DeviceInitialize hook: polling-mode UART init (UEFI-proven values),
  HCI_RESET, READ_LOCAL_VERSION, patchram from %12%\bcm4375B1_semco.hcd only while LMP subver = 0x1111, then UCON
  0x3C5 / UFCON 0x111 (Samsung S5PV210 defaults: level irqs, RX timeout + error irqs, trigger 4). ISR drains the RX
  FIFO into a 4 KiB ring, DPC feeds the sample's ReadH4PacketReassemble (parser reset on errors like the sample).
  FdoWriteDeviceIO -> BtUartTx + complete with output ULONG DataLen. D0Entry/SelfManagedIoInit -> BtUartRxStart,
  D0Exit -> BtUartRxStop, ReleaseHardware -> unmap. No idle-to-D3. The .hcd ships in the package (build script
  copies it from s10e-artifacts/bt-hcd; never committed). TX is synchronous polling at 115200 (slice 1).
- bt5 (13:15): driver package installed (oem30.inf) but no ACPI\SAMS9829 device: mkimg's sed for
  BEYOND0LTE_BT_UART_PROBE still matched the old "0$" form, so the DXE stayed inert (no role latch, BTH0 hidden).
- **bt6 (b3d747f6, 13:22, BT role + probe on): W4 R4 reached.** KD log (DbgPrintEx): DeviceAdd, PrepareHardware,
  UART window 0x10840000+0x100 mapped, UART init (umstat 0x11), HCI_RESET 00, version subver 1111, patchram
  176645 B, minidriver 00, 1261 records 0 failed, HCI_RESET 00, version still subver 1111 (patched FW may keep
  the ROM subver - check READ_LOCAL_NAME next), child PDO SerialBusWdk\UART_H4 / MS_BTHX_BTHMINI created, D0Entry,
  SelfManagedIoInit; bthport: GET_VERSION, SET_VERSION 1, child PrepareHardware/D0Entry, QUERY_CAPABILITIES, then
  43 WRITE_HCI commands answered by 43 Command Complete events through the RX interrupt -> ISR ring -> DPC -> H4
  reassembler -> READ_HCI completion. 0 out-of-sync, 0 errors. => the BthxDDI transport WORKS end to end.
- bt6 collected state (pnputil, 13:24): ACPI\SAMS9829\0 "Samsung Exynos9820 BCM4375 Bluetooth Transport" Started
  (oem30.inf, service Exynos9820Bt); child SerialBusWdk\UART_H4\...&4097 "Bluetooth Radio" Started (bth.inf,
  service BthMini); BTH\MS_BTHBRB "Microsoft Bluetooth Enumerator", BTH\MS_RFCOMM (RFCOMM TDI) and BTH\MS_BTHLE
  "Microsoft Bluetooth LE Enumerator" all Started. => **R4 DONE: Windows has a working Bluetooth radio.**
  (radios.txt failed: PowerShell 5.1 WinRT async call syntax - fixed in bt-scan.ps1.)
- R5 probe: drivers/S10eDiag/esp/bt-scan.ps1 (Radio API state, BluetoothAdapter caps, 30 s DeviceWatcher for
  unpaired LE + classic devices) via extra-bt-scan.cmd.
- R5 run 1 (bt6 image, 13:30): Windows Radio API: "Bluetooth kind=Bluetooth state=On" (and WiFi On);
  BluetoothAdapter: LE True, classic True, central role True, advertisement offload False. The adapter address is a
  Broadcom default derived from the chip name, not the phone's own address (Android writes the real one at boot with
  a vendor command; to do, without recording any address). The scan itself failed: PowerShell 5.1 cannot subscribe to
  WinRT events (DeviceWatcher) -> bt-scan.ps1 now uses DeviceInformation.FindAllAsync (inquiry / LE scan inside).
- R5 run 2 (btmfc1, FindAllAsync): 0 LE, 0 classic devices in two 30 s passes. KD: 68 HCI commands, 72 events =
  64 Command Complete + 4 Command Status + 4 Inquiry Complete (0x01) and NO Inquiry Result (0x02/0x22/0x2F) and NO LE
  Meta (0x3E): bthport ran 4 classic inquiries that found nothing (no discoverable device nearby, or the radio does
  not transmit), and FindAllAsync never started an LE scan (MS docs: unpaired discovery wants DeviceWatcher).
  Open: after patchram the LMP subversion is still 0x1111 -> verify the patch runs (Read Local Name / Broadcom
  verbose config 0xFC79 before and after), then an LE advertisement watcher (C++/WinRT, PS cannot do WinRT events).
- Patch check (da8fca3, 13:43): before patch READ_LOCAL_NAME "BCM4375B1", BCM verbose config 95 fe ff ff ...;
  after patch READ_LOCAL_NAME "BCM4375B1 Beyond E43 Ant0 [Baseline: 0173]" (= the .hcd's own name string) and verbose
  config 95 fe ad 00 ad 00 (firmware build 0xFFFF -> 0x00AD). => **the patch RUNS**; this firmware simply keeps the
  ROM's LMP subversion 0x1111. Q3 closed.
- R5 tool: tools/phone/btscan (C++/WinRT ARM64: BluetoothLEAdvertisementWatcher active scan + classic DeviceWatcher,
  counts + names only, no addresses), run from extra-bt-scan.cmd.
- **R5 RX observed (13:44-13:53, same driver):** Windows started an LE scan by itself (bt-scan / stack) and the KD log
  filled with 8156 LE Meta events (0x3E, 41-byte LE Advertising Reports) + 55 Command Complete, 2 Command Status,
  2 Inquiry Complete, 57 commands. => the radio RECEIVES nearby LE advertisers through the transport.
  Side effect: the sample traced every packet at LEVEL_INFO via DbgPrintEx; each print is a KD round trip, so the
  advert flood drowned Windows (ping/SSH gone for minutes, looked like a crash) and the slowed DPC let the 4 KiB RX
  ring overflow -> 37 out-of-sync parser resets. Fix: BT_TRACE_MAX_LEVEL = LEVEL_ERROR (bring-up lines stay).
  To do: RTS flow control (deassert when the ring fills) so a slow DPC never drops bytes.
- **R5 PASSED (14:00, btscan.exe from a normal Windows process):** LE active scan 20 s -> 62 advertisements from 2
  distinct nearby advertisers (both with local names, best RSSI -88 dBm); classic DeviceWatcher 20 s -> 0 (nothing
  discoverable nearby, consistent with the empty inquiries). => Windows apps can scan over this Bluetooth stack.
  Pairing (R5 second half) needs a device in pairing mode next to the phone - waits for the user.
- Remaining D4c work: phone's own BD address (vendor write at bring-up; read from the device, never recorded),
  RTS flow control, BT role latch + driver in the default image, power management (no idle yet), higher baud.
- **R5 pairing PASSED (2026-10-15 16:40, vidc2 image with the BT role latch, Exynos9820Bt driver):** the PC was
  made the peer: its Bluetooth radio was Off in Settings (Radio API SetStateAsync On), tools/phone/btpeer (x64
  Win32: discoverable + incoming connections + BluetoothRegisterForAuthenticationEx auto-accept) ran on the PC;
  tools/phone/btpair (ARM64 C++/WinRT) on the phone. The phone's classic inquiry found the PC by name. From the SSH
  session PairAsync returned AccessDenied (CanPair 0); from the desktop session (runuser2.ps1) it paired:
  phone "pairing request: kind 8 (ConfirmPinMatch) -> accept, pair result: Paired", PC "numeric comparison ...
  accept -> 0", same 6-digit value on both sides; the phone lists the PC as paired. => classic discovery + Secure
  Simple Pairing work end to end over the BthxDDI transport. (Cleanup: btpair unpair <name>, PC Settings.)
- **RFCOMM data PASSED (2026-10-15 17:03, Exynos9820Bt 1.0.0.3):** tools/phone/btspp (Win32, x64 PC server / ARM64
  phone client; custom service UUID in SDP; names only, no addresses). History: 1.0.0.1 connected (SDP + RFCOMM
  ~2.5 s) but the echo stalled at exactly 3030 B with "H4 out of sync"; 1.0.0.2 added UMCON AFC (hardware RTS/CTS,
  serial_s3c.h:143 S3C2410_UMCOM_AFC, trigger AFC_63) + counter logging -> still 3030 B, log showed RX ring
  overflows 8611 and 0 UART line errors: the 4 KiB software ring overflowed because the DPC was starved (BtUartTx
  busy-waits per byte at DISPATCH during a sustained send) while the ISR kept emptying the FIFO into the full ring.
  1.0.0.3: ring full -> ISR stops draining + masks RX, bytes wait in the FIFO, AFC drops RTS, the DPC unmasks after
  draining (16 KiB ring). Result: 262144 B sent, 262144 B echoed, 0 mismatched, 24.4 s = 10.5 KiB/s each way
  concurrently (= the 115200 baud ceiling), 1 throttle event, no sync loss. Open: higher baud (BCM vendor command +
  UBRDIV), interrupt-driven TX (the busy-wait is why the DPC starves).
