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
| VCELL | `0x09` | mV, vendor formula verbatim: `((w&0xFFF)*78125)/1e6 + (((w>>12)&0xF)*78125/1e6)*16` — integer math makes the low 12 bits contribute 0 and each high-nibble step 1.25 V; reproduce verbatim, do not "fix" |
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

### 2026-10-10 — bus correction (harsh-review follow-up)

The gauge is **not** on the touch bus. *(fact)* Touch is `hsi2c@104B0000`
(HSI2C10/USI03, `Exynos9810.h:185`, DSDT `SAMS9810` `_UID 10` in
`Star2LtePkg/AcpiTables/Dsdt.asl:204`); the MAX77705 clients are on
**hsi2c_45 @ 0x15D60000** (d1b notes, verified live on device). The 9810 DSDT
declares only the touch client (0x48) — no gauge entry exists in any shipping
firmware here.

Consequence for the Windows design: the SpbCx client needs a controller
instance on **hsi2c_45**, not the existing HSI2C10 one. That is *easier* than
the touch path, not harder: `Pmic.c:15-18` establishes that hsi2c_45's clocks
are ACPM-owned, left running by the bootloader, and we **never write CMU or
GPIO** for it. So the controller driver for it is the EHI2C ISR/DPC transfer
engine with the CMU/QCH/GPIO bring-up compiled out — a flag or a thin variant
of `Exynos9820Hsi2c`, not a new driver family. *(inference)*

### 2026-10-10 — Windows driver design (the "see % in Windows" path)

Architecture (all pieces already exist in some form):

1. **Firmware**: M8 DSDT must declare (a) an HSI2C controller device for
   hsi2c_45 — `_HID SAMS9820`, `_UID 45`, `Memory32Fixed(0x15D60000, 0x1000)`,
   no CMU/GPIO resources (ACPM-owned) — and (b) the gauge as its child
   `_HID SAMBAT00` with `I2cSerialBusV2(0x0036, ..., "\\_SB.I2C45")`.
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

Task order: (a) UEFI PoC read of `SOCREP(0x06)` on hsi2c_45 — the polled
master path to that bus already exists in `Pmic.c`, so this is a few lines,
and it validates the gauge address + conversions against Android's own %,
(b) DSDT entries (`SAMS9820` `_UID 45` + `SAMBAT00`) in M8, (c) hsi2c_45
controller variant for Windows (CMU/GPIO bring-up off) + SpbCx battery
class-extension driver skeleton, (d) validate % in the Windows tray against
the UEFI PoC reading.

### 2026-10-10 — UEFI PoC written (task a); build + device run pending

Implemented in `Beyond0LteUsbHostDxe` (the only driver that already owns the
hsi2c_45 polled-I2C path):

- `UsbHw.h`: gauge client `0x36` + registers `STATUS/REMCAP_REP/SOCREP/
  TEMPERATURE/VCELL/AVG_CURRENT/FULLCAP_REP` with `[K]` cites;
  `MAX77705_FG_RESISTOR = 2` marked **UNVERIFIED on device** (affects mA/mAh
  only, never %).
- `FuelGauge.c` (new): `Max77705FgReadWord` (reg-pointer-no-stop + 2-byte
  restart read = the vendor's `bulk_read(reg,2)`; matches the two-phase
  pattern already proven on this bus in `Pmic.c:Max77705RegRead`), plus
  SOC/VCELL/AVG_I/TEMP readers reproducing the vendor's integer math
  **verbatim** (including VCELL's low-12-bit truncation), and
  `FuelGaugeReport()` printing `[FG] SOC x.x% ...` at DEBUG_INFO.
  **Read-only: no gauge register is ever written.**
- `Pmic.c`: `Hsi2cXfer` de-`STATIC`ed (declared in `UsbHostInternal.h`).
- Entry: report runs right after `PmicBusInit()` success, `(VOID)`-cast —
  a gauge failure can never affect host bring-up.
- INF: `FuelGauge.c` added to `[Sources]`.

Verification status (rule 7 — a syntax check is not a build, a build is not
a boot):
- **done**: `gcc -fsyntax-only -fshort-wchar` with EDK2 x64 headers + stub
  AutoGen.h — all 5 modules of the driver pass. This is a syntax/prototype
  check only; the real ARM64 GCC/MSVC build has not run.
- **blocked on device**: full `Beyond0LtePkg` build + flash + boot log read.
  The S10e is in use by the Windows-boot agent (2026-10-10); when free, the
  check is one line in the boot log: `[FG] SOC` vs Android's own % on the
  same battery state.
- Harsh-review notes: temp/current/SOC conversions diffed line-by-line
  against the vendor source (all match); `Max77705FgReadWord` kept STATIC
  (single consumer in-file).

### 2026-10-10 — post-commit review of c014c66 (harsh critic)

Checked mechanically, not by re-reading:
- "read-only" claim: `grep MmioWrite|RegWrite` in FuelGauge.c → **zero hits**;
  the only `FALSE` (write) transfer is the 1-byte register-pointer phase
  without STOP, exactly the pattern `Max77705RegRead` already runs on this
  bus. ✔
- `Pmic.c` diff is exactly one line (STATIC removed from `Hsi2cXfer`); the
  one remaining `STATIC` there is `Hsi2cPollIntStat` (still file-local). ✔
- gauge STATUS absent-bit test matches vendor (`status_data[0] & 1<<3`). ✔
- `-Wall` syntax pass, no warnings. ✔
Found: nothing that changes behaviour. One accepted imprecision: the syntax
check used x64 `ProcessorBind.h` (UINTN is 64-bit on both, so prototype
compat is representative, but it is **not** the ARM64 build). Follow-up
task queued in M14 row: real build + boot-log validation when the phone is
free.

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

### 2026-10-15 user report: barely charges under Windows (parked: "fix bt first")
- User: TWRP 13 -> ~35 % in ~10 min; Windows 12 -> 13 % over a similar time. Either the charger is not configured
  under Windows (MAX77705 charger 0x69 at power-on defaults: input current limit / fast-charge current; Linux's
  max77705_charger sets them per cable type) or the Windows load (no idle states, 5 ms Wi-Fi poll) eats the input,
  or the reported % lags. Next: S10eBattery AvgCurrentMa + CHG_DETAILS/CNFG_00 under Windows vs Linux power_supply
  (current_now, input_current_limit, constant_charge_current) in TWRP; then vendor max77705_charger.c ILIM/CC regs.
- 2026-10-15 23:45 TWRP reference (cable to the PC, idle): battery uevent STATUS Charging, CHARGE_TYPE Fast, ONLINE 4
  (USB), CAPACITY 50, VOLTAGE_NOW 3.89 V, CURRENT_NOW 119 / CURRENT_AVG 77 (Samsung sysfs, mA, net into the battery),
  POWER_NOW 868, TEMP 30.3 C, TIME_TO_FULL 6191 s. User: wall charger in TWRP 13 -> 50 % in ~20 min; CPU temp jumps
  when on PC USB instead of the charger (TWRP). Windows comparison pending (S10eBattery AvgCurrentMa).
- 2026-10-16 00:35 TWRP, PC USB (battery uevent online 4 = USB, usb online 1, ac 0), 6 x 10 s: CAPACITY 47 %,
  STATUS Charging, CHARGE_TYPE Fast, CURRENT_NOW +94..+460 mA, CURRENT_AVG +167..+394 mA (net into the battery),
  VOLTAGE_NOW 3.845-3.892 V, TEMP 32.7-33.1 C, POWER_NOW 533-858. top: 83.5 % idle, /sbin/recovery (GUI) 13 % of one
  core (load average was still rising from boot). Wall charger (user): 13 -> 50 % in ~20 min ~ 1.1 Ah / 0.33 h ~ 3 A
  into the battery = Samsung AFC (9 V) negotiated by the Linux charger stack. Windows measurement next, same cable.
- Vendor charger driver (ianmacd/winnerx drivers/battery_v2/max77705_charger.c, 2026-10-16): Linux programs, per
  cable type, the CHGIN input current limit CHG_CNFG_09 = 0xC0 (25 mA steps, reg = I/25 - 1, max 3200 mA;
  max77705_set_input_current :590-625) and the fast-charge current CHG_CNFG_02 = 0xB9 (CHG_CC, 50 mA steps,
  <= 2 -> 100 mA, max 3150; max77705_set_charge_current :627-648); register addresses in max77705-private.h:77/84.
  S10eBattery reads only CHG_DETAILS_00/01 and never sets these -> under Windows the charger runs with whatever was
  left by the bootloader/power-on defaults. Next: read 0xB9 / 0xC0 under Windows (read-only) and compare.
- **2026-10-16 00:55 Windows charging regression = stale driver.** Same PC USB cable. Windows ran S10eBattery 1.0.0.1
  (no charger-WDT kick): BatteryStatus online / not charging / discharging 330-1260 mW, AvgCurrent -106 mA,
  ChgDetails 0x3BE0 = CHGIN_DTLS 3 (VBUS valid), CHG_DTLS 0xB (charger watchdog expired -> charging off) = the
  2026-10-12 bug fixed in db0c25d, but that build was no longer installed. Installed 1.0.0.4 (WDT kick + CHG_CNFG_02/09
  diagnostics): CHG_DTLS 1 (charging), CNFG_00 0x15 (WDTEN, mode 5 CHG+BUCK), WdtKicked 1, CHG_CC 0x09 = 450 mA,
  CHGIN_ILIM 0x13 = 500 mA, net battery current still -77 mA (CPU 18-22 % busy).
  => (1) Windows load ~2.5 W + 0.3 W from the battery = ~2.7 W vs TWRP ~1.1 W (+170..+390 mA net on the same 500 mA
  port). (2) CHGIN_ILIM stays 500 mA whatever the cable: Linux sets it per detected cable type (BC1.2 DCP/SDP + AFC
  9 V via MUIC) -> wall-charger fast charge works only in Linux. Next: Windows idle power (services, device idle);
  cable-type detection + ILIM/CC from the vendor dtsi charging tables (not guessed).
