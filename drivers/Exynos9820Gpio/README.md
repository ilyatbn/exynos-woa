# Exynos9820Gpio — ALIVE GPIO controller (GpioClx provider)

Windows KMDF driver exposing the Exynos 9820 "ALIVE" pinctrl block
(`pinctrl@15850000`, ACPI `_HID SAMS9821`) to the in-box GPIO framework
extension (GpioClx / `GPIOClx0101.sys`).

Why it exists: the DSDT's `Device (BTNS)` (`ACPI0011`, wired in
`firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl`) is serviced by the
**in-box** `Hidinterrupt.sys`, which consumes `GpioInt` resources — and
those need a GpioClx provider for this on-SoC controller. That gives the
S10e's four hardware buttons (volume up/down, power, Bixby) under Windows
with no custom HID code. The same provider later serves the CS47L93 codec
IRQ (gpa2-2) and the CS35L40 amp IRQs for the D8 audio tasks.

## Hardware facts (all VERIFIED — derivation in `notes/d8-audio.md` §3)

- Window `0x15850000 + 0x1000`; banks gpa0..gpa4 at 0x000/0x020/0x040/
  0x060/0x080, per-bank CON/DAT/PUD/PDN at +0x00/+0x04/+0x08/+0x0C,
  4-bit pin fields (2 pins on gpa4).
- Wakeup EINT (child `samsung,exynos7-wakeup-eint`): EINT_CON @0x700 +
  4*bank (4-bit trigger field: low=0 high=1 fall=2 rise=3 both=4),
  FLTCON @0x800 + 8*bank (8-bit/pin, FLTCON1 = +4 for pins 4-7,
  EN|SEL = 0xC0), MASK @0x900 + 4*bank (1=masked), PENDING @0xA00 +
  4*bank (write-1-clears).
- One GIC SPI per EINT pin: `INTREQ__ALIVE_EINTn = 5 + n` → GSIV 37..70,
  listed in the DSDT `_CRS` in pin order.

## The pseudo-bank model

GpioClx assigns `_CRS` interrupt entries to **banks** in order, but the
Exynos has one interrupt per **pin**. The driver therefore presents 34
banks of 1 pin (`NumberOfPinsPerBank = 1`), so bank B == pin B == EINT B
== the B-th `_CRS` entry; physical word/bit are recovered as B/8, B%8.
Because several pseudo-banks share one physical register word, every
register RMW takes a driver-global `WDFSPINLOCK` (GpioClx's per-bank lock
does not cover that).

ActiveBoth (required for the button device) is handled by GpioClx
emulation (`EmulateActiveBoth = TRUE`): level-low/level-high + dynamic
polarity via `CLIENT_ReconfigureInterrupt` — both natively supported by
the EINT_CON encodings above. Software debounce is also requested
(`EmulateDebouncing = TRUE`) to match the 10 ms the Android DT uses.

## Status — honest

- **Not built** (no WDK on the Linux box; build with
  `msbuild drivers\Exynos9820Gpio\Exynos9820Gpio.vcxproj /p:Configuration=Release /p:Platform=ARM64`
  on the Windows machine, same toolchain as the HSI2C driver).
- **Not loaded, not device-validated.** Register semantics are VERIFIED
  from the vendor driver; the GpioClx callback contract is implemented
  against the documented DDI and the official SimGpio sample, but the
  pseudo-bank mapping and ActiveBoth emulation are **inference-grade
  until the device test** (bar: press volume-up under Windows and see the
  system volume change; power button → screen off / power dialog).
- Known open unknowns to close on device: whether GpioClx accepts
  `NumberOfPinsPerBank = 1` (docs allow it; unexercised), and the GIC
  type/polarity Windows programs for the 34 Shared level entries.

## Sources

- `notes/d8-audio.md` §3 (register map, GSIVs), §5 (architecture decision)
- `notes/d8-audio-windows.md` Q3 (ACPI0011/Hidinterrupt.sys/GpioClx)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/general-purpose-i-o--gpio-
- https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/acpi-button-device
- https://github.com/microsoft/Windows-driver-samples/tree/main/gpio/samples/simgpio
