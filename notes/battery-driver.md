# Battery driver — inventory of what exists and what is missing

Started 2026-10-10, from the question "do we have a driver that checks the battery?"

## Progress log

### 2026-10-10 — repo-wide grep for battery/UPS/fuel-gauge/charger

**Verdict: there is no battery driver anywhere in the repo** — not in `drivers/`
(Windows), not in `firmware/Platform/Beyond0LtePkg` (UEFI). Confirms
`notes/dock-desktop.md` §8 item 4 and MILESTONES M14 ("Decide the battery policy
under Windows on ARM — partly done").

What the grep *does* find, and what each thing actually is:

- `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteUsbHostDxe/Pmic.c` +
  `UsbHw.h` — MAX77705 **charger-block** code, but only enough to turn OTG/VBUS
  **on** for host mode (mode 0xE, `CNFG_00`, OTG_ILIM). It never reads battery
  state. This is the closest thing to "battery code" we have.
- `drivers/Exynos9820Hsi2c` (+ `Exynos9810Hsi2c`) — working Windows HSI2C host
  controller driver. The fuel gauge hangs off an I2C bus, so this is the
  transport a battery driver would use. **Key building block, already done.**
- `tools/twrp-winre/validate.py` mentions `minimum_battery`/`battery_level` —
  that is the Android/TWRP side flashing gate, not a Windows driver.
- `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:407` — names
  hsi2c_45 as the bus carrying the MAX77705 (charger client).

### Hardware facts already verified (from notes/d1-usb-host.md, notes/d1b-usb-vbus-hcd.md)

- PMIC: **MAX77705** on HSI2C bus 45, `hsi2c_45 = 0x15D60000` + 0x1000
  (live on device; clocks ACPM-owned — same leave-alone strategy as USB).
- I2C clients (7-bit): PMIC misc `0x66`… charger **`0x69`**, **fuel gauge `0x36`**
  (`0x6C>>1`, from the vendor DT). *(fact, cited in d1b notes from vendor kernel
  `drivers/battery_v2/`)*
- The vendor fuel-gauge driver lives in Samsung's downstream
  `drivers/battery_v2/` — **not vendored in this repo**, so the fuel-gauge
  register map (SOC %, VBAT, IBAT, temperature) is **UNVERIFIED** here.

### Gap analysis — what a "battery checker" would take, at each layer

1. **UEFI PoC (cheapest, no Windows needed):** polled I2C reads of fuel gauge
   `0x36` on hsi2c_45, mirroring the already-proven polled master sequence in
   `Beyond0LteUsbHostDxe/Pmic.c`. Would print SOC/voltage at boot and close the
   "invisible battery" gap for firmware-level decisions (e.g. the low-battery
   boot gate in the M15 design rules).
2. **Windows driver:** a KMDF **battery class extension** (not inbox — Windows
   has no ACPI battery here because our ACPI tables (`Beyond0LteAcpiPlatformDxe`)
   declare no battery device). Needs: bind to `Exynos9820Hsi2c`, poll gauge
   `0x36`, implement `IOCTL_BATTERY_*`/WMI so `BatteryClass.sys` surfaces it in
   the UI, plus a `_BST`-free active-report path. No prior art in this repo.
3. **Prerequisite research:** vendor `battery_v2` gauge register map + which
   gauge chip sits behind 0x36 (MAX77705 is the interface PMIC; the gauge may be
   a separate Maxim/Sec part). Close by reading the vendor kernel source or the
   DT node names on-device when adb is reachable.

### Dead ends / do-not-repeat

- No charging while docked (vendor-stated, dock-desktop §10) — a battery driver
  reports state, it cannot make the battery charge.
- `sec_thermistor@*` / Samsung thermal stack has no Windows equivalent (M14,
  needs-phone).

### 2026-10-10 — fuel-gauge register map VERIFIED from vendor kernel source

Pulled Samsung's downstream `drivers/battery_v2/max77705_fuelgauge.c` and
`include/linux/mfd/max77705-private.h` from the winnerx kernel mirror
(Exynos9810/9820-era tree). The gauge is a Maxim **ModelGauge m5**-class part
(MAX17260/MAX77825-style register set) at I2C **8-bit 0x6C = 7-bit 0x36**,
16-bit word registers, little-endian (`lo<<8|hi` from `bulk_read(reg,2)`).

Registers we need (all `[K]` = verified in vendor source):

| Reg | Addr | Read (vendor conversion) |
|---|---|---|
| SOCREP | `0x06` | SOC 0.1% = `hi*100 + lo*100/256`, clamp 1000 |
| REMCAP_REP | `0x05` | remaining mAh = word × `fg_resistor/2` |
| FULLCAP_REP | `0x35` | full mAh = word × `fg_resistor/2` |
| VCELL | `0x09` | mV: `(w&0xFFF)*78125/1e6 + ((w>>12)&0xF)*78125/1e6<<4` (12-bit × 78.125µV + 4-bit × 1.25V) |
| AVG_CURRENT | `0x0B` | mA: signed-16 two's complement × `15625*fg_resistor/100000` (5 mΩ sense → 312.5 µA/LSB) |
| TEMPERATURE | `0x08` | °C×10: `hi` signed, LSB 0.039 °C (`lo*39/10`) |
| STATUS | `0x00` | bit3 of low byte = battery absent |

**SOC for Windows = `SOCREP(0x06)` read directly** — the gauge's m5 engine
already computes RepSOC = REMCAP_REP/FULLCAP_REP; no arithmetic on our side
beyond the 0.1%→% conversion.

Still UNVERIFIED (reads that close them):
- `fg_resistor` — vendor default comment says "standard value is 2 which means
  5mOhm"; confirm from the S10e DT battery node (only needed for current, not %).
- Samsung's UI scaling `capacity_min`/`capacity_max` (their Android % is scaled:
  `(raw − min)*1000/(max − min)`). For v1 we report raw gauge SOC and note the
  delta vs Android's number.
- Which physical HSI2C bus carries 0x36: the vendor DT names it `hsi2c_45`
  (d1b notes); our Windows HSI2C driver drives USI03 = HSI2C10 (touch). The
  MAX77705 PMIC is a known touch-bus neighbour in the 9810 DSDTs — **must be
  confirmed on-device** (cheap: `i2cdetect`-style read of 0x36 from UEFI PoC,
  or DT symlink target read via adb).

### 2026-10-10 — Windows driver design (the "see % in Windows" path)

Architecture (all pieces already exist in some form):

1. **Firmware**: M8 DSDT must declare the gauge as an I2C child of the HSI2C10
   controller — `_HID SAMBAT00`, `_ADR 0x36`, `SB`/_HID PNP0A05 battery container
   is *not* needed for a class extension.
2. **Driver**: KMDF kernel driver, binds `ACPI\SAMBAT00`, talks to SpbCx with
   the exact `WdfIoTargetCreate/Open/SendIoctlSynchronously` pattern already
   proven in `drivers/S6SY761Touch/Device.c:1627,2922,3004,3083`.
3. **Battery surface**: register as **battery class extension**
   (`BatteryClassExtensionRegister` → `BCLENTRYPOINT_UPDATE` →
   `BATTERY_REPORTING_STATE` with `BATTERY_STATUS`/`BATTERY_INFORMATION`), so
   `battery.dll`/`BatteryClass.sys` shows it in the tray **without any ACPI
   battery object**. Inbox reference shape: Microsoft's `classext` WDK sample.
4. **v1 scope**: read-only — percentage, voltage, discharge rate, estimated
   runtime; no charge state (device runs on battery while docked, dock-desktop §10).

Task order: (a) confirm gauge visibility on the touch bus (UEFI PoC read of
`SOCREP` — reuses `Pmic.c` polled-master code), (b) DSDT `_HID SAMBAT00` entry
in M8, (c) driver skeleton + class-extension loop, (d) validate % against
Android's own battery reading on the same device.

## Sources

- notes/dock-desktop.md §8 (repo grep result: no battery/UPS driver)
- notes/d1-usb-host.md:334-368, notes/d1b-usb-vbus-hcd.md:96-164 (MAX77705,
  charger 0x69, fuelgauge 0x36, hsi2c_45 base) — themselves cited from Samsung
  downstream kernel `drivers/battery_v2/`, `include/linux/mfd/max77705-private.h`
- docs/exynos9820-port/MILESTONES.md M14
- https://github.com/ianmacd/winnerx/blob/master/drivers/battery_v2/max77705_fuelgauge.c
  (SOC/voltage/current/temperature conversions, 2026-10-10)
- https://github.com/ianmacd/winnerx/blob/master/include/linux/mfd/max77705-private.h
  (fuel-gauge register addresses, 8-bit slave addrs 0x6C/0xD2/0xCC/0x4A, 2026-10-10)
