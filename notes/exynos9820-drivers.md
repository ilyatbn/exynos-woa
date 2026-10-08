# Exynos 9820 (Galaxy S10e / "d17") — driver-porting research notes

Started: 2026-10-08. Task: gather driver/hardware details needed to port the three
Exynos 9810 Windows drivers (UFS storage, HSI2C "SpbCx" bus controller, S6SY761Touch)
to Exynos 9820 / Galaxy S10e.

Status: IN PROGRESS — findings appended incrementally.

---

## 1. Upstream repos identified (web)

- The public repo this tree is generated from is **`ntdevlabs/exynos9810-woa`** (GitHub).
  Confirmed via search: "This repository holds the firmware, drivers and a graphical
  installer … drivers/S6SY761Touch/ Touchscreen driver, derived from TheMorc's S6SY761Touch".
  URL: https://github.com/ntdevlabs/exynos9810-woa
- TheMorc's touch driver: **`TheMorc/S6SY761Touch`** (GPL-2.0). Forked to
  **`yatli/S6SY761Touch`** which targets **OnePlus 7 Pro / 7T Pro** (also s6sy761!).
  yatli README: "originally forked from https://github.com/edk2-porting/nt36xxx_win,
  but now uses the vhidmini KMDF source driver as the base." So the touch lineage is:
  Gustave Monce `edk2-porting/nt36xxx_win` -> TheMorc `S6SY761Touch` -> this repo.
  URLs: https://github.com/TheMorc/S6SY761Touch , https://github.com/yatli/S6SY761Touch
- TheMorc profile has 86 repos; the 22 "source" repos shown are unrelated (trackers,
  NixOS, LS2009, etc.). No S10/S10e/S20 Windows-driver repo seen under TheMorc. His
  Exynos/Windows work is the S6SY761Touch only. (Inference: filtered list may omit forks;
  will verify with a targeted search.)

## 2. Exynos 9820 / d17 kernel + UEFI repos found (web)

- **`sonic011gamer/edk2-exynos`** — "aims to provide a usable EDK2 UEFI environment for
  modern Samsung SoCs … boot manager for multi-booting mainline Linux …". MOST relevant.
  URL: https://github.com/sonic011gamer/edk2-exynos
- **`sonic011gamer/Mu-Samsung`** — "Project Mu UEFI Implementation for Exynos based Samsung
  devices … brings UEFI Firmware on your Device (if supported) to boot OSs like Windows or Linux".
  URL: https://github.com/sonic011gamer/Mu-Samsung
- **`CruelKernel/samsung-exynos9820`** — "Unified kernel tree for S10 and Note10. Supported:
  G970F/N, G973F/N, G975F/N, G977B/N, N970F, N975F, N971N, N976B/N." (Android kernel source.)
  URL: https://github.com/CruelKernel/samsung-exynos9820
- **`ravindu644/android_kernel_aosp_exynos9820`** and `ravindu644/samsung_exynos9820_stock`
  — S10/N10 AOSP/stock kernel trees. URL: https://github.com/ravindu644
- **`V0lk3n/nethunter_kernel_samsung_exynos9820`** — NetHunter kernel port for S10 (Beyond1lte).
- KernelSU Next device page lists exynos9820 maintainers (ravindu644). URL:
  https://kernelsu-next.github.io/webpage/pages/devices.html

## 3. Mu-Silicium (Project-Silicium/Mu-Silicium) — THE key project

`Mu-Samsung` (sonic011gamer) says "Development has been Moved to Mu-Silicium". Mu-Silicium
is Project Mu UEFI for ARM64 devices (BSD-2-Clause; GPL drivers in GPLDrivers dirs). This repo's
firmware already credits Mu-Silicium. URL: https://github.com/Project-Silicium/Mu-Silicium

### Device packages under Platforms/Samsung (from tree listing)
Confirmed packages: a10, a52q, a52sxq, a7, b4q, b5q, **beyond1lte**, c1s, dm2q, e1s, gta4l,
gta9pwifi, gtaxllte, gts8p, gts9wifi, q2q, r0q, r8q, r8s, r9qb2, **starlte**, starqltechn, x1s.
- **`beyond1ltePkg`** = Galaxy S10 (Exynos 9820). This is the direct 9820 target package.
  URL: https://github.com/Project-Silicium/Mu-Silicium/tree/main/Platforms/Samsung/beyond1ltePkg
- `starltePkg` = Galaxy S9 (Exynos 9810) — closest analog to this repo's star2lte (S9+).
- `x1sPkg` = Galaxy S20 5G (Exynos 9830) — Active, Internal Storage works.

### Status.md facts (Exynos 9820 / 9810 / 9830)
- **Exynos 9820 — Galaxy S10, codename `beyond1lte`**: State Inactive; needs BIT-16; "Only 23H2
  Windows Builds and Lower are able to Boot." UEFI: Display ✅, Internal Storage ❌, Side Buttons ✅.
  (No S10e-specific entry; S10e = "d17" in Samsung ESP naming, but Mu-Silicium uses beyond1lte.)
- **Exynos 9810 — Galaxy S9, codename `starlte`**: State Active; needs BIT-17; Windows Boot ✅
  but "Only Windows PE boots"; Internal Storage ❌ at OS level.
- **Exynos 9830 — S20 5G `x1s`**: Active; Internal Storage ✅ (UEFI), but Windows OS storage ❌.
- **Exynos 9830 — Note20 5G `c1s`, S20 FE `r8s`**: Inactive.
- **Exynos 2400 — S24 `e1s`**: Inactive.

### UFS storage guide (relevant to porting the UFS driver)
- Flip5 status links: "Internal Storage — Needs Special Setup" ->
  https://github.com/Project-Silicium/Guides/blob/main/Vendors/Samsung/Modding-UFS/README.md
  This is the Samsung UFS "modding" guide — likely the same RWD/rwd storage approach.
- Tab A gtaxllte links Windows drivers: https://github.com/AistopGit/windows_oem_samsung_gtaxllte

### Related repos
- **`Project-Silicium/Silicium-ACPI`** — ACPI/DSDT generation (the source of ACPI device names
  like `SAMS9810`, `SCSY0761`). URL: https://github.com/Project-Silicium/Silicium-ACPI
- **`Project-Silicium/Device-Binaries`** — referenced as Binaries submodule.
- **`Project-Silicium/Guides`** — porting guides incl. Vendors/Samsung.

## 4. Mu-Silicium silicon packages are organized BY SoC (Samsung naming)

From `Silicon/Samsung/` tree: `S5E7870Pkg`, `S5E7885Pkg`, **`S5E9810Pkg`**, **`S5E9820Pkg`**,
`S5E9830Pkg`, `S5E9945Pkg`, `SamsungNullPkg`, `SamsungPkg`.
- Samsung SoC naming: **S5E9820 = Exynos 9820** (the S10/S10e target). S5E9810 = Exynos 9810 (S9/S9+).
- `beyond1lte.dsc` does `!include S5E9820Pkg/S5E9820Pkg.dsc.inc`. So the S10 platform pulls the
  Exynos-9820 silicon package. Framebuffer 1440x3040 (S10 panel). SMBIOS "Galaxy S10"/beyond1lte.
- beyond1ltePkg has: Include, Library (MemoryMapLib, KeypadDeviceLib), DeviceBuild.py, .dec/.dsc/.fdf.
  Its AcpiTables.inf is COMMENTED OUT in the dsc ("#beyond1lte/AcpiTables.inf") — consistent with
  "Inactive" status. USE_CUSTOM_DISPLAY_DRIVER=0.
- **`Silicon/Samsung/S5E9820Pkg`** is the single most important tree for the port: it should hold the
  9820 MMIO bases for HSI2C, UFS, GPIO, PMU, etc. URL:
  https://github.com/Project-Silicium/Mu-Silicium/tree/main/Silicon/Samsung/S5E9820Pkg

### Silicium-ACPI Platforms/Samsung (device ACPI/DSDT sources)
Present: a52q, a52sxq, b4q, b5q, gtaxllte, gts8p, q2q, r0q, r9qb2, **starlte**, x1s.
- NOTE: **no `beyond1lte`** in Silicium-ACPI — S10 has no published DSDT yet (explains Inactive).
  But `starlte` (S9, Exynos 9810) and `x1s` (S20, Exynos 9830) DO have ACPI. Those are the closest
  references for the HSI2C/touch ACPI device names (`SAMS9810`, `SCSY0761`) and MMIO layout.
  URL: https://github.com/Project-Silicium/Silicium-ACPI/tree/main/Platforms/Samsung/starlte

---

## 0. Baseline: what the existing 9810 drivers depend on (read from repo)

These are the device-specific constants that must be re-derived for 9820. Read directly
from `/home/ilya/exynos9810-woa/drivers` (facts, not web).

### drivers/Exynos9810Hsi2c (HSI2C bus controller, "SpbCx")
- Binds to ACPI device name **`ACPI\SAMS9810`** (from README). Touch is a separate
  `ACPI\SCSY0761` function device, NOT advertised as HID-over-I2C.
- Maps MMIO resources (from `exynos9810hsi2c.h`):
  - HSI2C10 physical base `0x104B0000`, len `0x1000`
  - PERIC0 CMU base `0x10400000`, len `0x4000`
  - PERIC0 SYSREG base `0x10411000`, len `0x1000`
  - PERIC0 GPIO base `0x10430000`, len `0x1000`
- Source clock **200 MHz**; default bus speed 400 kHz; FIFO trigger 8.
- Uses **USI03** I2C mode; GPIO **GPP1_4 (SCL) / GPP1_5 (SDA)**.
- Register layout uses Samsung "EHI2C" / USI-style HSI2C (control, FIFO, trailing,
  interrupt, auto-config). This is Samsung's HSI2C ("hsi2c-gen3"-era) controller.

### drivers/S6SY761Touch (touchscreen, from TheMorc; ports Linux sec_ts/s6sy761)
- Binds to **`ACPI\SCSY0761`** (s6sy761 digitizer). GPL-2.0.
- **10 contacts** max, **64-byte** HID input reports (report id 0x54), 6-byte input points.
- Uses a "STAR2_ALIVE_GPIO" legacy interrupt bank at physical `0x14050000` (GPA1 + EINT regs)
  for demand-start / interrupt ownership.
- Derived from Microsoft vhidmini2 KMDF sample; fork of Gustave Monce's nt36xxx_win.

### drivers/Exynos9810Ufs (UFS storage driver, BSD)
- Samsung **UFS** storage (NOT NVMe). Maps:
  - UFS HCI base `0x11120000` len `0x2000`
  - UFS UNIPRO base `0x11110000` len `0x8000`
  - UFS PMA base `0x11124000` len `0x1000`
  - UFS UFSP base `0x11130000` len `0x1000`
- Uses Samsung **RWD1** ("rwd") record structures (`include/Rwd1Constants.h`), cookie "YLAV" (0x594C4156).
- Block device is `sda` with **4096-byte logical blocks**; GPT partition table; USERDATA NTFS.
- PMU/WDT watchdog region at `0x14060000`; WDT base `0x10050000`.

---
