# M8 — building the 9820 (`beyond0lte`) UEFI firmware on Windows

Everything below was run on this machine, 2026-10-09, in the restarted admin session.
Findings are appended as discovered.

## 1. What had to be installed first (facts)

The 9810 flow's `firmware/tools/build-uefi.ps1` assumes an EDK2 checkout, LLVM, GNU make
and `iasl`. None of that was present:

| Piece | How it got here | Result |
|---|---|---|
| EDK2 tree | `git clone --depth 1 https://github.com/tianocore/edk2` into `firmware/edk2` | ok. A previous attempt had left a half-cloned `edk2/` at the **repo root** pointing at `github.com/tiancore/edk2` (typo, "tiancore") — deleted, since it was only a `.git` with no working tree |
| clang/lld | `winget install LLVM.LLVM` | `C:\Program Files\LLVM\bin\clang.exe` present |
| GNU make | `winget install GnuWin32.Make` | `C:\Program Files (x86)\GnuWin32\bin\make.exe` |
| `iasl` | `iasl-win-20260408.zip` from open-acpica, expanded to `%TEMP%\acpica-iasl-20260408\` | the script's own auto-detect pattern finds it |
| BaseTools host binaries | `BaseTools/Bin/Win32` is **not in the repo** (checked with `git ls-files` on master and on the `edk2-stable202211` tag: only `GccLto`, `GnuNoteBti.bin`, the two ext-dep yamls). Built them instead: `python BaseTools\Edk2ToolsBuild.py -t VS2026` after `pip install edk2-pytool-extensions edk2-pytool-library regex` | 25 files in `BaseTools\Bin\Win32`; **`BrotliCompress` failed** (`BrotliCompress.c(21)` cannot open `./brotli/c/common/constants.h` — the brotli tree is a submodule that `--depth 1` did not pull). Nothing in this platform needs it, so the build proceeds |
| `Conf/*.txt` | `build.py` needs `WORKSPACE/Conf/{tools_def,build_rule,target}.txt`; only the `.template` files exist | copied `tools_def.template -> tools_def.txt`, `build_rule.template -> build_rule.txt`, `target.template -> target.txt` (naming the third `build.conf` was wrong and produced `error 0001: File open failure / build_rule.txt`) |
| edk2 submodules | `git submodule update --init` | `MdePkg.dec:33` needs `MdePkg/Library/MipiSysTLib/mipisyst/library/include`, which is a submodule — without it the build dies at `error 000E` |

## 2. DSC/FDF fixes needed to make the 9820 platform resolve (facts)

Each of these is an edk2-master drift or a missing instance, not a 9820 hardware question:

* `LzmaDecompressLib` — no lib in this checkout declares that `LIBRARY_CLASS`
  (both `LzmaCustomDecompressLib.inf` and `LzmaArchCustomDecompressLib.inf` are
  `LIBRARY_CLASS = NULL`). Line dropped.
* `GptLib` — implicit library class of `PartitionDxe` in this checkout; the 9810 DSC
  predates the split. Instance added: `MdeModulePkg/Library/GptLib/GptLib.inf`.
* `PlatformBootManagerLib` — `BdsDxe` requires it and the 9820 tree has no port of the
  9810's 6000-line library. Used edk2's generic `ArmPkg/Library/PlatformBootManagerLib`.
* `ResetSystemRuntimeDxe` — needs `ResetSystemLib`, which has **no 9820 instance**
  (the 9810's is PMU-based at `0x14060000`; this SoC's PMU is `0x15860000` and the
  intra-block layout is unverified). Module dropped from DSC and FDF, so
  reset-to-recovery is simply not offered by this build.
* `gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType|4` — ArmPkg's
  `PlatformBm.c:770` has a `static_assert(... == 4)` (TTYTERM); edk2's default 0
  fails to compile. Declared in `MdePkg.dec:2633`, **not** `MdeModulePkg.dec`
  (the first attempt used the wrong token space and died at `error F001`).

## 3. Build result (fact)

```
firmware/tools/build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG
exit code 0
...
Generating FVMAIN FV
FVMAIN_COMPACT [99%Full] 692224 (0xa9000) total, 688888 (0xa82f8) used, 3336 (0xd08) free
FVMAIN [24%Full] 4194304 (0x400000) total, 1011160 (0xf6dd8) used, 3183144 (0x309228) free
- Done -
```

Artifact: `firmware/edk2/Build/Beyond0LtePkg/DEBUG_CLANGPDB/FV/BEYOND0LTE_UEFI.fd`,
**7 340 032 bytes**, sha256 `ef4381b7585c333e2498e09d3de6863cfa1b627ac85d8b4e7a9a3d602302d04a`.

Modules that built for AARCH64 include `PeilessSec`, `DxeCore`, `BdsDxe`,
`Beyond0LteSimpleFramebufferDxe`, `AcpiTableDxe`, `PartitionDxe`, `DiskIoDxe`,
`ScsiBus`/`ScsiDisk`/`UfsPassThruDxe`, `SerialDxe` (fed by
`Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib`).

Inference (mine): "compiles and links" is the whole claim here. Nothing about this
image has been observed on an S10e, so under the repo's golden rule the boot it
produces is **Guided**, not Ready. `FVMAIN_COMPACT` at 99% full is a warning sign for
any future module addition.

## 4. What this build does NOT contain (fact, from the DSC/FDF DROPPED blocks)

No 9820 equivalent exists yet for: `Star2LteAcpiPlatformDxe` (so the ACPI tables
**ship but are never installed** — Windows would get no MADT/DSDT),
`Star2LteRamManagerDxe`, `Star2LteUfsHcDxe` (UEFI-side storage bring-up),
`Star2LteResetSystemLib`, the HSI40/GPIO-handoff subsystem, and the 9810's
`PlatformBootManagerLib`. The first two of those are on the boot path: without an
ACPI installer the platform cannot present the CPU/RAM topology the port's bar
depends on.

## 5. Sources

* `https://github.com/tianocore/edk2` (cloned `--depth 1`, submodules initialised)
* `https://github.com/open-acpica/acpica/releases/download/20260408/iasl-win-20260408.zip`
* local: `firmware/edk2/BaseTools/...`, `firmware/Platform/Beyond0LtePkg/...`,
  build log `%TEMP%\m8-build-14.log`


## 6. The ACPI installer ported, and the platform rebuilt (2026-10-09)

The first build shipped the tables but had no consumer for them, so Windows would have
received no MADT/DSDT — i.e. the port's own bar ("Windows sees disk, RAM and CPU cores")
could not be met. Ported `Star2LteAcpiPlatformDxe` ->
`firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteAcpiPlatformDxe/{.c,.inf}` and added it
to both the DSC `[Components.common]` and the FDF.

* The FFS GUID it looks for is now `11111111-2222-3333-4444-555555555521`, i.e. the
  9820 `AcpiTables.inf` FILE_GUID — the two ends were already in step.
* The module's own `FILE_GUID` is `...555555555543`, a build identity, not a read.
* Two channels are **compiled out**, not retargeted:
  - the PRAM breadcrumb (`0xFED14000`, plus a `0xFED13F00` scratch block) — proven to sit
    inside bank0 and inside the vendor's `debug_snapshot.log_bcm` on this SoC
    (`notes/s10e-live-reads.md` §7.3), so no safe 9820 address exists to move it to;
  - the GTDT memory-mapped counter frame: `BEYOND0LTE_GTDT_MMIO_COUNTER 0`, so the 26 MHz
    frequency and the MCT read base `0x10040100` stay 9810 values and `Gtdt.aslc` keeps
    its all-ones placeholders.
* `EXYNOS_ARCH_TIMER_FREQ_HZ` (26 MHz, `Exynos9820.h:169`) is now referenced symbolically
  instead of a local literal, and `Silicon/Exynos9820Pkg/Exynos9820Pkg.dec` was added to
  the module's `[Packages]`.

Rebuild: `exit 0`, `- Done -`,
`FVMAIN_COMPACT [99%Full] 692224 total, 689240 used, 2984 free` (was 3336 free — the
installer consumed 352 bytes of a 99%-full volume, so headroom is now 2984 bytes),
`FVMAIN [24%Full]`. Artifact `BEYOND0LTE_UEFI.fd`, **7 340 032 bytes**, sha256
`821af51ca475b9d1405fa6e2bb26ced42a54f8005ce20f1cb4478d70680a1d6d`
(previous content, without the installer, hashed `ef4381b7585c333e2498e09d3de6863cfa1b627ac85d8b4e7a9a3d602302d04a`).

Inference (mine): the FD staying exactly 7 340 032 bytes is the FDF's fixed size; the
content changed (hash moved). The 2984 bytes of free space in `FVMAIN_COMPACT` is the
thing to watch before adding any further module.

## 6. ACPI installer ported (closes the §4 gap)

Created `firmware/Platform/Beyond0LtePkg/Drivers/Beyond0LteAcpiPlatformDxe/`
(`.c` + `.inf`), ported from the 9810's `Star2LteAcpiPlatformDxe` (source left
untouched). This removes the §4 blocker "the ACPI tables ship but are never
installed". Not yet added to the DSC/FDF, and not built/observed — Guided.

- Reads the FFS by `mBeyond0LteAcpiTablesFileGuid` = `11111111-2222-3333-4444-555555555521`
  (the AcpiTables module's FILE_GUID; a build identity, not a read). Module's own
  FILE_GUID `...555555555543` is also a build identity.
- PRAM breadcrumb channel compiled out: `#define BEYOND0LTE_PRAM_DIAG 0` + `#if`/`#endif`
  around the four `AcpiPram*` helpers, every call site, and the `0xFED13F00` scratch
  block. Mirrors `PlatformMemoryMapLib.c:48`. Reason: `0xFED14000` is inside DRAM bank0
  (0x80000000+0x80000000 → 0x100000000) and inside `debug_snapshot.log_bcm`
  (0xfeb10000+0x400000) — `notes/s10e-live-reads.md` §7.3. Functions kept compiled-away,
  not pointed at a guessed address.

Constants left UNVERIFIED on the 9820 (do not treat as facts):
- `EXYNOS_ARCH_TIMER_FREQ_HZ` = 26000000 (from `<Platform/Exynos9820.h>`; `Exynos9820.h:169`
  says it is the 9810's value, unconfirmed — first on-device CNTFRQ_EL0 read decides it).
- `BEYOND0LTE_MCT_CNTCV_PA` = `0x10040100` (9810 MCT G_CNT read base; no 9820 constant exists,
  kept as a local placeholder).
- The whole GTDT MMIO-counter path is dead at runtime (`BEYOND0LTE_GTDT_MMIO_COUNTER 0`,
  kept at 0 to match the Mu-Silicium "Allow Windows Boot" path); the ext39 experiment note
  and the CNTFRQ_EL0=0 explanation are 9810 observations, not 9820.
