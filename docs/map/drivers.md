# Map: drivers/ — Windows WDK drivers (ARM64)

All three are **test-signed**; the installer enables test signing in the image it builds.
Installed on the phone's Windows by the image-build stage (drivers injected into the VHDX).

```
drivers/
  Exynos9810Ufs/        UFS storage driver (C)            ~19.0 kLOC
  Exynos9810Hsi2c/      HSI2C10 bus controller (C++)      ~3.8 kLOC
  S6SY761Touch/         S6SY761 touchscreen (C)           ~4.7 kLOC (incl. tests)
  include/Rwd1Constants.h   shared record definitions (firmware <-> UFS driver)
```

Install order on Windows: **HSI2C controller before the S6SY761 function driver**
(the touch device is `ACPI\SCSY0761`, not HID-over-I2C).

## Exynos9810Ufs — storage driver (BSD-2-Clause-Patent)

| File | Lines | What |
|------|-------|------|
| `Exynos9810Ufs.c` | 9,092 | the driver: UFS HCI/UNIPRO/PMA/UFSP at 0x11110000-0x11131000; portable-kernel contract (`UFS_PORTABLE_KERNEL_CONTRACT` resolved to 1 at import) |
| `Exynos9810Ufs.h` | 1,852 | driver header; includes `../include/Rwd1Constants.h`; PRAM breadcrumb console at 0xFED14000 (survives warm reset, readable via `/sys/fs/pstore/pmsg-ramoops-0` over adb) |
| `Exynos9810UfsDiag.h` / `UfsDiag.c` | 1,091 / 6,610 | diagnostic companion (`UfsDiag.exe` helper) |
| `build-ufs.ps1` | 176 | fixed cl/link command lines, no WDK MSBuild toolset; never signs/installs |
| `Exynos9810Ufs.inf` / `.vcxproj` | — | INF + VS project |

**Released binary reproduction:** `build-ufs.ps1 -SourceDir drivers\Exynos9810Ufs -OutDir <new> -WideUncached -DmaWindow`
reproduces it byte for byte (unsigned SHA-256 `a80b2229…8eaa7`).

## Exynos9810Hsi2c — HSI2C10 SpbCx controller (BSD-2-Clause-Patent AND MS-PL)

Binds `ACPI\SAMS9810`. Framework scaffolding from Microsoft's SkeletonI2C sample
(`driver.h`, `i2ctrace.h` carry Microsoft copyright; `driver.cpp`/`device.cpp`/`controller.cpp`/`internal.h`
conservatively treated as sample-derived → MS-PL, see `MS-PL.txt`). Hardware engine (clocking, FIFO, recovery) is original.

| File | Lines | What |
|------|-------|------|
| `driver.cpp` | 216 | SpbCx entry, WDF ISR/DPC pair |
| `device.cpp` | 1,465 | device: validates/maps HSI2C10, PERIC0 CMU/SYSREG/GPIO resources |
| `controller.cpp` | 1,068 | USI03 I2C mode, clock path, GPP1_4/GPP1_5; 100/400 kHz from fixed 200 MHz source clock; FIFO transfers; repeated-start (no STOP on non-final); reset after cancel/timeout/NACK/abort |
| `exynos9810hsi2c.h` | 142 | the exact DSDT resources the firmware must provide |
| `internal.h` / `device.h` / `controller.h` | 188/117/… | framework headers |
| `Exynos9810Hsi2c.inx` | — | INF directive file |
| `resource.rc` | — | message catalog |

Build: `msbuild Exynos9810Hsi2c.vcxproj /p:Configuration=Release /p:Platform=ARM64` (WDK 10.0.26100).
Released `.sys` provenance: Authenticode SHA-256 `9897e4e2…` (source build) vs `6ef2e601…` (released, PDB path normalized + re-signed).

## S6SY761Touch — touchscreen (GPL-2.0-only, own LICENSE governs)

Derived from TheMorc's S6SY761Touch (ports Linux `sec_ts`/`s6sy761` by Andi Shyti;
built on Microsoft vhidmini2 + Gustave Monce's nt36xxx_win). **Not hardware-qualified** —
a bounded gesture-delivery update to the retained v24 driver (forked from
`..\S6SY761TouchGpio\RetainedV24`, baseline frozen; starting Device.c SHA-256 `3a6027c6…`,
INF version `6.0.5058.3`).

| File | Lines | What |
|------|-------|------|
| `Device.c` | 3,159 | the driver: retains `ACPI\SCSY0761` binding, SPB transport, legacy interrupt ownership, HID descriptor, 64-byte reports, demand-start, KMDF 1.15 |
| `Device.h` / `common.h` / `TouchDriverUpdate.h` | 298/120/… | headers |
| `tests/TouchGestureTests.c` | 304 | host mock tests: 9 groups (down coords/timestamps, 3 s hold, 64-FIFO transitions, coalescing, short buffers, overload recovery…); negative control against untouched v24 (5 must fail) |
| `Build-Touch.ps1` | — | builds production C through WDF/SPB host mocks → ARM64 SYS + unsigned catalog; never signs/installs |

## drivers/include/Rwd1Constants.h

Shared by the firmware and the UFS driver: RWD1 startup-record layout
(64 B at `0xFED13D80`, cookie `0x594C4156` "VALID", magic `RWD1`/`!RWD1`, checksum seed)
— the same records `BootRouteService` and the GPL kernel modules read/clear.
