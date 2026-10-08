# Map: firmware/ — EDK2 UEFI for star2lte

EDK2-based UEFI that boots Windows 11 ARM64 on the Galaxy S9+ (Exynos 9810).
Lineage: Mu-Silicium (Project Silicium) reference for Exynos 9810 boot work;
structure follows WOA-Project Lumia950XLPkg / mu_andromeda_platforms; boot shim
from edk2-msm / edk2-exynos. License: BSD-2-Clause-Patent (boot shim BSD-2-Clause;
CNTFRQ_EL0 patch MIT). ~22.6 kLOC, 67 files.

## Boot chain

```
sboot loads boot.img "kernel" ─► bootshim.S (relocating stub)
   └─ copies [FD] to 0x90000000 (above RKP-monitored kernel zones 0x80094000-0x828DBFFF)
      └─ PeilessSec SEC/PEI entry ─► decompresses FVMAIN (LZMA GUIDED section)
         └─ DxeCore ─► BDS (PlatformBootManagerLib) ─► winload (CNTFRQ_EL0 patch injected)
```

## Layout

```
firmware/
  Platform/Star2LtePkg/
    Star2LtePkg.dsc          410L  platform description; AARCH64; build flags
    Star2LtePkg.fdf          239L  FD at 0x90000000, 7 MB; FVMAIN_COMPACT + FVMAIN
    AcpiTables/              DSDT/FADT/GTDT/MADT/PPTT + GPIO/HSI40 device ASLs
    Drivers/                 the platform DXE modules (below)
    Library/                 platform libraries (below)
  Silicon/Exynos9810Pkg/
    Exynos9810Pkg.dec         35L  package declaration (PCDs, includes)
    Include/Platform/Exynos9810.h  239L  SINGLE SOURCE OF TRUTH for SoC addresses/DRAM map
    Library/Exynos9810SerialPortLib/  201L  debug UART (skeleton; sboot already set clock/pinmux)
  tools/
    bootshim/bootshim.S      207L  relocating loader prepended to the FD
    build-uefi.ps1                 wraps EDK2 build.py (CLANGPDB/CLANGDWARF, PACKAGES_PATH)
    pack-uefi.ps1                  [shim][FD] -> flashable boot.img wrapped in a TWRP ref image
```

## DSC/FDF build knobs

- `STAR2LTE_USB_DEBUG_MODE`: Off (preserves ext209 control) / Snapshot (read-only CMU telemetry) / Hid (native polling).
- `STAR2LTE_RAM_STAGE`: 0 = 711 MiB baseline, 3 = deployed 1,357 MiB, 4 = adds 4 GiB bank.
- Critical link flags: `/ALIGN:0x1000` (CLANGPDB) — 4 KB PE section alignment; without it TE stripping breaks adrp/ldr after FV stripping.
- Build ONE component at a time when porting: SerialPortLib → PrePi → DxeMain.

## Drivers (DXE modules)

| Module | Lines | What |
|--------|-------|------|
| `SimpleFramebufferDxe` | 425 | GOP over the framebuffer sboot already initialized ("first light" milestone; no DECON/DSIM bring-up) |
| `Star2LteAcpiPlatformDxe` | 334 | installs the platform ACPI tables packaged as a freeform FFS |
| `Star2LteGpioHandoffDxe` | 449+149 (Core) | GPIO (PERIC0) handoff: owns ALIVE banks, USB2 access profile; feeds `GpioHandoffDevices.asl` (252L) |
| `Star2LteHsi40HandoffDxe` | ~1,600 across 14 files | HSI40 (I2C) census/cold-init/handoff trials + Win11 authority/runtime; feeds `Hsi40Devices.asl` |
| `Star2LteUart1HandoffDxe` | ~290 | UART1 census/handoff (same trial-record pattern as HSI40) |
| `Star2LteRamManagerDxe` | 137 | adds device-tree-verified DRAM bank-1 ranges at DXE (Mu-Silicium RamManagerDxe approach); PEI window stays 0x90000000..0xBC800000 |
| `Star2LteUfsHcDxe` | **4,190** | platform UFS Host Controller (HCI) driver — the big one; UTRD byte-granular conversion, DMA windows |

Stock EDK2 modules in the FDF: PeilessSec (SEC/PEI entry), DxeCore, console/serial,
AcpiTableDxe, UfsPassThruDxe + ScsiBus/ScsiDisk + DiskIo/Partition + Fat, BdsDxe.
USB (XhciDxe/UsbBusDxe) is commented out — USB is not a UEFI input path here.

## Libraries

| Library | Lines | What |
|---------|-------|------|
| `PlatformBootManagerLib` | **10,033** | BDS glue: consoles → connect controllers → enumerate UFS/USB → launch Windows. Contains `MuCntfrqKep` (283L): injects Project Silicium's CNTFRQ_EL0 shellcode into winload (Samuel Tulach / DuoWoA / Project Silicium lineage) + `UsbDebug`/`UsbDebugHid` |
| `PlatformMemoryMapLib` | 219 + 218 (`ArmPlatformLib`) | ARM memory-region descriptors; 1:1 with the memory-map doc; all bases from `Exynos9810.h` |
| `Star2LteResetSystemLib` | 440 | reset/power-off: PSCI preferred, Exynos PMU fallback (Samsung Exynos9810 kernel mechanisms) |

## ACPI tables

`Dsdt.asl` (252L) is the DSD the HSI2C Windows driver binds to (`ACPI\SAMS9810`);
`Fadt.aslc` (92L) is the firmware-side AML (handoff trials); `Gtdt/Madt/Pptt.aslc`
are GT/M/P tables. `Star2LteAcpiHeader.h` (43L) carries shared ACPI constants.

## Device constants

`Silicon/Exynos9810Pkg/Include/Platform/Exynos9810.h` is the single source of truth
for SoC MMIO bases and the DRAM map — firmware, the UFS driver (`drivers/include/Rwd1Constants.h`
is the Windows-side companion) and the boot shim all track it.

## Reproducing released UEFI images

Not scripted byte-for-byte from this tree (research lineage). Released `star2lte-uefi-<build>.img`
images are validated builds, one per Windows build, listed in `firmware.json`.
