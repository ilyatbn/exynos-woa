# Exynos 9820 HSI2C10 SpbCx driver

**Status: not built, not signed, not run.** This package is a port of the driver
validated on the Exynos 9810 (Galaxy S9+) to the **Exynos 9820 (Galaxy S10e,
`beyond0lte`)**. Nothing here has been executed on a 9820 device, so under this
repo's golden rule no stage of it may be called `Ready`.

This ARM64 KMDF driver exposes the Galaxy S10e HSI2C10 block to Windows through
SpbCx. It binds to `ACPI\SAMS9820`; the touchscreen remains a separate
`ACPI\SCSY0761` function device (see `../S6SY761Touch`) and is not advertised as
HID-over-I2C. The `SAMS9820` name is a decision, not a fact — the firmware DSDT
for `beyond0lte` (milestone M8) has to emit exactly this path.

The controller driver:

- validates and maps the HSI2C10, PERIC0 CMU, PERIC0 SYSREG, and PERIC0 GPIO
  resources;
- selects USI03 I2C mode, enables its clock path, and configures GPP1_4/GPP1_5;
- derives 100 kHz or 400 kHz timing from the fixed 200 MHz source clock;
- services FIFO transfers through a WDF ISR/DPC pair;
- preserves repeated-start semantics by omitting STOP on non-final sequence
  transfers;
- resets the controller after cancellation, timeout, NACK, abort, or a short
  transfer.

Install the controller package before the S6SY761 function-driver package. The
firmware DSDT must provide the exact resources documented in
`exynos9820hsi2c.h`.

## What is known about this SoC, and what is not

Verified against the shipping 9820 device tree and a live S10e
(`notes/dock-desktop.md` §12):

| Constant | State |
|---|---|
| HSI2C10 window `0x104B0000` + 0x1000 | **verified identical** to 9810 |
| source clock 200 MHz (`default-clk = 0xbebc200`) | **verified identical** |
| PERIC0 GPIO window `0x10430000` | **verified identical** |
| GPP1 pins 4/5 as SCL/SDA | **verified identical** |
| the whole EHI2C register set and the `USI03` clock names | **verified identical** |
| PERIC0 **CMU** window and its `USI03` gate/QCH offsets | **UNVERIFIED** — no 9820 node exists at the 9810 address |
| PERIC0 **SYSREG** window | **UNVERIFIED** — same reason |

The 9820's CMU banks are `1A200000`, `1BC00000`–`1BF00000` and its sysreg
controllers are at `13020000` and `13C10000`; the correct offsets must be derived
from Samsung's `cal-if` headers rather than guessed. Until that is done the two
windows keep the 9810 values and are marked `UNVERIFIED` in the header.

## Build

Requires Visual Studio with the ARM64 build tools and the WDK (10.0.26100).

```powershell
msbuild Exynos9820Hsi2c.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

The package (`.inf`, `.sys`, `.cat`) lands in `ARM64\Release\Exynos9820Hsi2c`.
Like the other drivers here it is test-signed, so Windows needs test signing
on (the installer turns it on in the image it builds).

## Provenance

This folder is a **copy of `../Exynos9810Hsi2c`** with the package and symbol
names retargeted to 9820 and the constants header annotated as above. The 9810
package's own provenance — the released `Exynos9810Hsi2c.sys`, its Authenticode
hashes and the end-to-end validation on SM-G965F — belongs to that folder and
**does not apply here**: no 9820 binary has been produced yet.

## License

Original work is under the project's **BSD-2-Clause-Patent** license (`LICENSE`).
The WDF/SpbCx framework scaffolding comes from Microsoft's SkeletonI2C sample
(`microsoft/Windows-driver-samples`, `spb/SkeletonI2C`): `driver.h` and
`i2ctrace.h` carry Microsoft's copyright, and the sample-derived structure of
`driver.cpp`, `device.cpp`, `controller.cpp` and `internal.h` is conservatively
treated as sample-derived too. Those portions are Copyright (c) Microsoft
Corporation and are used under the **Microsoft Public License** (`MS-PL.txt`),
so this folder as a whole is `BSD-2-Clause-Patent AND MS-PL`. The hardware engine
(clocking, FIFO transfers, recovery) is original.
