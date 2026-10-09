# M8 — RamManager decision + port (`Star2LteRamManagerDxe` → `Beyond0LteRamManagerDxe`)

Task: decide whether the 9820 still needs a RAM manager now that the high DRAM
banks are in the platform memory map (`9339b0c`), and port it if it does.

## Step 1 — the decision (read before writing any code)

The two mechanisms are **not** the same thing:

* `PlatformMemoryMapLib.c` `ArmPlatformGetVirtualMemoryMap()` returns
  `ARM_MEMORY_REGION_DESCRIPTOR`s consumed by the ARM page-table setup
  (`ArmPkg/Library/ArmMmuLib` via `ArmPlatformLib`). It decides **which
  addresses are cached vs Device**, i.e. what is *addressable*.
* `Star2LteRamManagerDxe.c:55` calls `gDS->AddMemorySpace (
  EfiGcdMemoryTypeSystemMemory, …)` — that contributes to the **DXE Global
  System Configuration (GCD) memory map**, which is what the DXE core allocates
  from and what `GetMemoryMap()`/`MemoryTypeInformation` ultimately report to
  Windows.

Publishing a bank in the page tables does not make it GCD system memory, so the
manager is **still needed** for the high banks. Confirmed by the DSC's own note
(`Beyond0LtePkg.dsc:42-44`: "Multi-region HOB publication of the extra DRAM
banks is gone, so only the single PcdSystemMemoryBase/Size window reaches DXE").

## Step 2 — what the 9810 manager does, and what carries

* Two hardcoded bank-1 ranges (`0xC0080000+0xBF80000`, `0xCD044000+0x12FBC000`)
  — 9810 DRAM-shape facts, with the seclog and framebuffer carveouts subtracted.
  **Do not carry.** The 9820 equivalents are the macros:
  `EXYNOS_DRAM_BANK3_BASE/SIZE` (0x880000000, 2 GiB) and
  `EXYNOS_DRAM_BANK4_BASE/SIZE` (0x900000000, 2 GiB) — both VERIFIED reads
  (`Exynos9820.h:87-90`, `notes/s10e-live-reads.md` §7.2).
* `EXYNOS_DRAM_BANK1_*` (0xC0000000) and `BANK2_*` (0xE1900000) have VERIFIED
  bases but **UNVERIFIED sizes** (`Exynos9820.h:84,86` say size not read). They
  are skipped, exactly as `PlatformMemoryMapLib.c:190` skips zero-size windows,
  so no guessed extent enters the GCD map.
* `STAR2LTE_RAM_STATUS_ADDR 0xFED17F60` — the same PRAM hazard: inside bank0 on
  this SoC. Compiled out (`BEYOND0LTE_RAM_STATUS_DIAG 0`), not retargeted.
* `ArmSetMemoryAttributes (base, len, EFI_MEMORY_WB, 0)` before
  `AddMemorySpace` — kept; it is the 9810 flow's ordering guard ("map before
  publishing, because AddMemorySpace may allocate from the newly added range
  immediately"), not a SoC constant.
* The rollback path (`RemoveMemorySpace` on failure) is kept verbatim.

## Step 3 — identity

* `FILE_GUID 11111111-2222-3333-4444-555555555547` — checked free (in use:
  5500,5501,5502,5510,5520,5521,5522,5530,5531,5540,5541,5542,5543,5544,5545,
  5546,5550). Build identity, not a read.
* `MODULE_TYPE = DXE_DRIVER` (a library would not run at boot).

## Step 4 — wiring + build (done)

DSC: the DROPPED header note (`dsc:42-44`) rewritten, and the `[Components.common]`
"RAM discovery" line (`dsc:493`) now lists
`Platform/Beyond0LtePkg/Drivers/Beyond0LteRamManagerDxe/…inf`.
FDF: same in the header (`fdf:32-34`) and the RAM-discovery section (`fdf:201`).

First build failed: `fatal error: 'Library/ArmMmuLib.h' file not found`. The
header lives in **UefiCpuPkg** (`firmware/edk2/UefiCpuPkg/Include/Library/ArmMmuLib.h`),
not ArmPkg — that is why the 9810 `.inf` packages `UefiCpuPkg/UefiCpuPkg.dec`.
Restoring that package fixed it; the drop was my mistake, not a 9820 fact.

Second build: **exit 0**, 39 s (`%TEMP%\m8-build-25.log`).

| Metric | Before the manager | After |
|---|---|---|
| `FVMAIN_COMPACT` total | 696320 (0xaa000) | **700416 (0xab000)** — it auto-grew one 0x1000 block |
| `FVMAIN_COMPACT` free | 968 | 3864 |
| `FVMAIN` used | 1108224 (26%) | 1159016 (27%) |
| FD | 7 340 032 B | **7 340 032 B unchanged**, sha256 `c72b3490…` |

The FD is fixed (`NumBlocks = 0x700`, `fdf:76`) so the growth is absorbed inside
it — the FD size the loader expects did not change.

## Sources (local evidence actually consulted)

* `firmware/Platform/Star2LtePkg/Drivers/Star2LteRamManagerDxe/Star2LteRamManagerDxe.c` (137 lines, read in full) + `.inf`.
* `firmware/edk2/UefiCpuPkg/Include/Library/ArmMmuLib.h` — where the ArmMmuLib header actually lives.
* `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c` — the page-table map, incl. the high-bank entries added in `9339b0c`.
* `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:79-90` — bank bases/sizes with VERIFIED/UNVERIFIED markers.
* `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.dsc:42-44` — the DROPPED RamManager note.
* `notes/s10e-live-reads.md` §7.2 — the vendor DT `memory@880000000` / `memory@900000000` reg lists.
