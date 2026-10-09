# M8 — Beyond0LtePkg DSC + FDF port from the validated Star2LtePkg pair

Running notes. Appended as work proceeds.

## 0. Scope / inputs read

- `firmware/Platform/Star2LtePkg/Star2LtePkg.dsc` (410 lines) — read in full.
- `firmware/Platform/Star2LtePkg/Star2LtePkg.fdf` (239 lines) — read in full.
- `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` (441 lines) — the repo's
  single source of MMIO addresses for 9820.
- `firmware/Silicon/Exynos9820Pkg/Exynos9820Pkg.dec` (57 lines) — the 9820 token space
  `gExynos9820TokenSpaceGuid` and its PCDs.
- `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/{PlatformMemoryMapLib.c,ArmPlatformLib.c,PlatformMemoryMapLib.inf}` — the 9820 library that exists.
- `notes/dock-desktop.md` §17 — DRAM banks + reserved carveouts + the initrd/FD-load rationale.

Neither 9810 file was modified.

## 1. Path verification (rule 1)

Method: every `.inf` token in the 9810 DSC/FDF was extracted with a regex and checked
with `Test-Path` against BOTH `firmware/edk2/<path>` (the edk2 checkout) and
`firmware/<path>` (the repo's own `Platform/`, `Silicon/` trees). 112 edk2-relative
references were checked; **111 exist, 1 does not**:

| 9810 reference | exists? | note |
| --- | --- | --- |
| `ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf` | NO | In THIS checkout the library moved to `MdePkg/Library/ArmTransferListLib/ArmTransferListLib.inf` (verified: `Test-Path firmware/edk2/MdePkg/Library/ArmTransferListLib/ArmTransferListLib.inf` = True). Same library, drifted path — the DSC's own header note ("library .inf paths track a specific edk2 checkout … paths drift between releases") is exactly this case. The 9820 DSC uses the existing path. This is NOT a module substitution. |

The 9 repo-local references (`Platform/Star2LtePkg/...`, `Silicon/Exynos9810Pkg/...`) and
their 9820 equivalents:

| 9810 reference | 9820 equivalent | action |
| --- | --- | --- |
| `Platform/Star2LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf` | `Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf` — EXISTS | PORT (rename only) |
| `Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.inf` | `Silicon/Exynos9820Pkg/Library/Exynos9820SerialPortLib/Exynos9820SerialPortLib.inf` — EXISTS (`LIBRARY_CLASS = SerialPortLib`, consumes `gExynos9820TokenSpaceGuid.PcdSerialRegisterBase`) | PORT |
| `Platform/Star2LtePkg/AcpiTables/AcpiTables.inf` | `Platform/Beyond0LtePkg/AcpiTables/AcpiTables.inf` — EXISTS (`MODULE_TYPE = USER_DEFINED`, sources Dsdt/Fadt/Madt/Pptt/Gtdt; its own header already says the installer driver does not exist yet) | PORT |
| `Platform/Star2LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf` | none — `Test-Path firmware/Platform/Beyond0LtePkg/Drivers/SimpleFramebufferDxe/SimpleFramebufferDxe.inf` = False, and there is no `Drivers/` dir at all | DROP |
| `Platform/Star2LtePkg/Drivers/Star2LteAcpiPlatformDxe/Star2LteAcpiPlatformDxe.inf` | none (confirmed by the 9820 `AcpiTables.inf` header note) | DROP |
| `Platform/Star2LtePkg/Drivers/Star2LteRamManagerDxe/Star2LteRamManagerDxe.inf` | none | DROP |
| `Platform/Star2LtePkg/Drivers/Star2LteUfsHcDxe/Star2LteUfsHcDxe.inf` | none | DROP |
| `Platform/Star2LtePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.inf` | none | DROP |
| `Platform/Star2LtePkg/Library/Star2LteResetSystemLib/Star2LteResetSystemLib.inf` | none | DROP |

CORRECTION (the first `Get-ChildItem -Recurse` walk silently truncated its output; the
authoritative walk is `cmd /c dir /s /b`, and every decision below is a per-file
`Test-Path`, not a listing):

```
firmware\Platform\Beyond0LtePkg\AcpiTables\AcpiTables.inf
firmware\Platform\Beyond0LtePkg\AcpiTables\Beyond0LteAcpiHeader.h
firmware\Platform\Beyond0LtePkg\AcpiTables\Dsdt.asl
firmware\Platform\Beyond0LtePkg\AcpiTables\Fadt.aslc
firmware\Platform\Beyond0LtePkg\AcpiTables\Gtdt.aslc
firmware\Platform\Beyond0LtePkg\AcpiTables\Madt.aslc
firmware\Platform\Beyond0LtePkg\AcpiTables\Pptt.aslc
firmware\Platform\Beyond0LtePkg\Library\PlatformMemoryMapLib\ArmPlatformLib.c
firmware\Platform\Beyond0LtePkg\Library\PlatformMemoryMapLib\PlatformMemoryMapLib.c
firmware\Platform\Beyond0LtePkg\Library\PlatformMemoryMapLib\PlatformMemoryMapLib.inf
firmware\Silicon\Exynos9820Pkg\Exynos9820Pkg.dec
firmware\Silicon\Exynos9820Pkg\Include\Platform\Exynos9820.h
firmware\Silicon\Exynos9820Pkg\Library\Exynos9820SerialPortLib\Exynos9820SerialPortLib.c
firmware\Silicon\Exynos9820Pkg\Library\Exynos9820SerialPortLib\Exynos9820SerialPortLib.inf
```

So `AcpiTables.inf` and the 9820 `SerialPortLib.inf` DO exist (the first table above
overstated those as missing — superseded by the table below). There is still
**no `Drivers/` directory at all** in `Beyond0LtePkg`, and no
`PlatformBootManagerLib` and no reset-system library.

## 2. Consequences of the drops (recorded, not papered over)

- `SerialPortLib` survives (the 9820 instance exists), so `DebugLib` =
  `MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf` and
  `MdeModulePkg/Universal/SerialDxe/SerialDxe.inf` still resolve.
- Dropping `Star2LteResetSystemLib` removes the only `ResetSystemLib` instance. Nothing
  in the 9820 tree provides it, and the 9810's PMU-based reset path cannot be carried
  over silently (the 9820 PMU is at `0x15860000`, not the 9810's `0x14060000`). The
  `ResetSystemLib|` line is removed; `MdeModulePkg/Universal/ResetSystemRuntimeDxe`
  still builds (it does not require the lib) but PSCI/PMU reset is then whatever
  edk2's default gives.
- Dropping `PlatformBootManagerLib` removes the `PlatformBootManagerLib` instance.
  `UefiBootManagerLib` (edk2) survives; only the platform hook is gone.
- Dropping `Star2LteUfsHcDxe` removes the UFS host-controller producer, so
  `MdeModulePkg/Bus/Ufs/UfsPassThruDxe` has nothing to bind to. The rest of the storage
  chain (`ScsiBus/ScsiDisk/DiskIo/Partition/EnglishDxe/Fat`) still builds — every path
  exists in the checkout — but it will enumerate no volumes. Kept, gap recorded.
- Dropping `SimpleFramebufferDxe` removes the first-light framebuffer; the console
  stack (ConPlatform/ConSplitter/GraphicsConsole) survives with no GOP producer.
- Dropping `Star2LteRamManagerDxe` removes the extra-RAM publication driver, so only
  the single `PcdSystemMemoryBase/Size` window reaches DXE.
- Dropping `Star2LteAcpiPlatformDxe` means the `AcpiTables` FFS file (which still
  ships, its `.inf` exists) is never installed via `EFI_ACPI_TABLE_PROTOCOL`, and the
  9810's install-time GTDT patch (26 MHz frame + MCT read base) never happens. The
  9820 `Gtdt.aslc` already reflects this: `GTDT_CNT_CONTROL_BASE`/`GTDT_CNT_READ_BASE`
  are `0xFFFFFFFFFFFFFFFF` placeholders.
- `STAR2LTE_USB_DEBUG_MODE` / `STAR2LTE_RAM_STAGE`: grepped the whole `firmware/` tree.
  `STAR2LTE_RAM_STAGE` is consumed by NO C source in this checkout (only the 9810 DSC and
  `firmware/tools/build-uefi.ps1`); `STAR2LTE_USB_DEBUG_MODE` is consumed only by
  `Star2LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c`,
  `PlatformBootManagerLib.c`, `UsbDebug.c/h`, `UsbDebugHid.c` — all 9810 modules that are
  dropped. The 9820 `PlatformMemoryMapLib.c` greps clean for both. So both defines and the
  two `-D…` CC_FLAGS lines are dropped; the CLANGPDB **DLINK** line and its rationale are kept.

## 3. Every address written into the new DSC/FDF, and where it came from

Rule 4 says the single source of MMIO addresses is `Exynos9820.h`; the 9810 DSC already
restated a small set of them, so those are the only ones re-derived. Line numbers below
are in `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` unless stated.

| DSC/FDF PCD | value written | provenance | status |
| --- | --- | --- | --- |
| `gArmTokenSpaceGuid.PcdSystemMemoryBase` / `gExynos9820TokenSpaceGuid.PcdSystemMemoryBase` | `0x0000000080000000` | `EXYNOS_DRAM_BASE`, Exynos9820.h:79; same value in `Exynos9820Pkg.dec:29`; `PlatformMemoryMapLib.c:157` publishes it | VERIFIED (vendor DT `memory@80000000 reg = <0x0 0x80000000 0x80000000>`) |
| `…PcdSystemMemorySize` | `0x0000000050000000` | `EXYNOS_DRAM_SIZE`, Exynos9820.h:81; `.dec:38`; `PlatformMemoryMapLib.c:159` | DERIVED, not a read: it is `EXYNOS_RSVD_MODEM_SPARE_BASE (0xD0000000, Exynos9820.h:96) − EXYNOS_DRAM_BASE`. The header labels it "CONSERVATIVE: stops at the first reserved region" |
| `gExynos9820TokenSpaceGuid.PcdSerialRegisterBase` | `0x0000000010440000` | `EXYNOS_UART_BASE`, Exynos9820.h:120; `.dec:26` | VERIFIED (`uart@10440000`, live S10e + vendor DT) |
| `gArmTokenSpaceGuid.PcdGicDistributorBase` | `0x0000000010101000` | `EXYNOS_GICD_BASE`, Exynos9820.h:139; `.dec:42` | VERIFIED (`interrupt-controller@10100000`, GICD reg) |
| `gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase` | `0x0000000010102000` | `EXYNOS_GICC_BASE`, Exynos9820.h:141; `.dec:45` | VERIFIED (GICC reg; identical to the 9810's, header says the GIC does not move) |
| `gArmTokenSpaceGuid.PcdFdBaseAddress` / `PcdFvBaseAddress` | `0x0000000080080000` | Exynos9820.h:76 comment: "the DT's chosen node also puts the Android initrd at `0x84000000..0x841fffff`, which is why the firmware loads at `0x80080000`"; same claim in `notes/dock-desktop.md` §17:784-787 | **UNVERIFIED as an observed load address.** The initrd bounds are the read; 0x80080000 is the design decision that follows from them, and nothing has been proven on an S10e. The 9810's `0x90000000` is NOT carried over: its whole justification (RKP kernel zone `0x80094000-0x828DBFFF`, bootloop at 0x80080000) is a 9810 observation with no 9820 equivalent read |
| `gArmTokenSpaceGuid.PcdFdSize` / `PcdFvSize` | `0x00700000` | carried from the 9810 DSC/FDF (7 MB FV, "matches edk2-exynos ref") | CARRIED — a build parameter, not a hardware read; must equal `FD_SIZE` in the FDF |
| FDF `FD.BEYOND0LTE_UEFI` `BaseAddress` | `0x0000000080080000` | same as `PcdFdBaseAddress` above | UNVERIFIED (same reason) |
| FDF `FD…` `Size` / `NumBlocks` | `0x700000` / `0x700` (× `BlockSize 0x1000`) | arithmetic on the carried 7 MB | CARRIED; `0x700 × 0x1000 = 0x700000` checked |
| FDF FD range line | `0x00000000\|0x00700000` | an **offset inside the FD**, not a physical address — kept verbatim from the 9810 FDF | n/a |
| FDF `FVMAIN` `NumBlocks` | `0x400` (4 MB) | carried from the 9810 FDF, including its "auto-size NumBlocks=0 crashed GenFv" note | CARRIED |
| `gArmPlatformTokenSpaceGuid.PcdCoreCount` | `8` | `.dec:48` VERIFIED from `/proc/cpuinfo`; `ArmPlatformLib.c:67` `mCoresPerCluster[] = { 6, 2 }` | VERIFIED |
| `gArmPlatformTokenSpaceGuid.PcdClusterCount` | `2` | `.dec:51` VERIFIED (two MPIDR clusters, 6+2) — the 9810's 2×4 does NOT hold; the 9820 lib replaces the division with the explicit table | VERIFIED |
| `gArmTokenSpaceGuid.PcdArmPrimaryCore` | `0x0` | `ArmPlatformLib.c:160-171` comment: "Almost certainly LITTLE cluster 0 core 0 … has not been observed on this SoC" | INFERENCE |
| `gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize` | `0x10000` | carried from the 9810 DSC | CARRIED (size, not an address) |
| `gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize` | `0x08000000` | carried from the 9810 DSC | CARRIED (size, not an address) |
| `gArmTokenSpaceGuid.PcdArmArchTimer{Sec,Intr,Virt,Hyp}IntrNum` | `29/30/27/26` | `Gtdt.aslc:35-38` `GTDT_*_GSIV`, which the 9820 table itself marks "UNVERIFIED (architectural default, not read on S10e)" | UNVERIFIED on this SoC (the 9810 DSC claimed "VERIFIED from DT" — that claim does NOT transfer) |

Deliberately NOT written anywhere in the 9820 DSC/FDF: every other MMIO address in
`Exynos9820.h` (PMU, SPEEDY, CMU banks, UFS windows, USB DRD/PHY, HSI2C, GPIO, DECON/DSIM,
WDT, framebuffer base). They live only in the header and in
`PlatformMemoryMapLib.c`'s `mMmioRegions[]`, which is the repo's single-source rule.
The 9810 DSC/FDF never restated them either, so nothing had to be re-derived.

## 4. Naming / GUID decisions

- `PLATFORM_NAME = Beyond0LtePkg`, `OUTPUT_DIRECTORY = Build/Beyond0LtePkg`,
  `FLASH_DEFINITION = Platform/Beyond0LtePkg/Beyond0LtePkg.fdf`.
- FD section renamed `FD.STAR2LTE_UEFI` → `FD.BEYOND0LTE_UEFI`.
- `PLATFORM_GUID = 11111111-2222-3333-4444-555555555502`, deterministic: the 9810
  platform's stem `…555555555500` with the low pair set to `02`, i.e. the 9820
  generation digit this tree already uses (`Exynos9820Pkg.dec …5520`,
  `AcpiTables …5521`, `Exynos9820SerialPortLib …5522`,
  `Beyond0LtePlatformMemoryMapLib …5531`). Checked against the full GUID list in
  `firmware/` (grep of `5555555555[0-9a-f]{2}` over `*.inf/*.dsc/*.fdf/*.dec`):
  `…5502` is used by nothing in either tree, so it cannot collide.
- Comment style: the 9810 pair uses `## @file … ##` blocks with `#` body lines and
  `//` inside nothing (DSC/FDF are `#`-comment files); mirrored exactly.

## 5. Files written

- `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.dsc` — 485 lines.
- `firmware/Platform/Beyond0LtePkg/Beyond0LtePkg.fdf` — 281 lines.
- Neither 9810 file was opened for writing; a post-write scan of `firmware/Platform/Star2LtePkg`
  and `firmware/Silicon/Exynos9810Pkg` found no file with a modification time on 2026-10-09.

## 6. Post-write verification

- **Path existence:** every `.inf` token in the two new files was re-extracted and
  `Test-Path`ed against `firmware/edk2/<path>` and `firmware/<path>`. 125 unique tokens;
  the only one that does not resolve is
  `ArmPkg/Library/ArmTransferListLib/ArmTransferListLib.inf`, which appears **only inside
  the DSC's comment** explaining the correction (DSC line 74). The live reference is
  DSC line 237, `MdePkg/Library/ArmTransferListLib/ArmTransferListLib.inf`, which exists.
- **DSC/FDF sync:** FDF places 29 modules; DSC lists 31 components. The two DSC-only
  entries are `Silicon/Exynos9820Pkg/.../Exynos9820SerialPortLib.inf` (a library class —
  correctly not placed in the FV) and `Platform/Beyond0LtePkg/AcpiTables/AcpiTables.inf`
  (a regex artifact: it IS in the FDF, as the `INF RuleOverride = ACPITABLE` line).
  Nothing is in the FDF that the DSC omits.
- **No live 9810 references:** every `Star2LtePkg` / `Exynos9810` / `STAR2LTE` token in
  the new DSC sits on a comment line.
- **FD arithmetic checked:** `NumBlocks 0x700 × BlockSize 0x1000 = 0x700000 = $(FD_SIZE)`;
  `FD_BASE_ADDRESS` equals `PcdFdBaseAddress`/`PcdFvBaseAddress` in the DSC;
  `PcdFdSize`/`PcdFvSize` equal `FD_SIZE`.
- **PeilessSec XIP check (why the FD base is legal):** `ArmPlatformPkg/PeilessSec/PeilessSec.c:11`
  defines `IS_XIP()` as FD-above-`mSystemMemoryEnd` OR FD-below-`PcdSystemMemoryBase`.
  With FD `0x80080000..0x80780000` inside DRAM `0x80000000..0xD0000000`, `IS_XIP()` is
  FALSE, i.e. the same non-XIP/relocate path the 9810 build took (FD base == system base
  there). `MemoryInitPeiLib.c:134` then carves the FD out of the memory HOB. Consistent,
  not asserted-by-construction: nothing has run on an S10e.

## 7. Dead ends / open questions (do not re-search)

- No 9820 `Drivers/` directory exists, so the four 9810 drivers cannot be renamed-ported;
  they must be written from scratch, which is out of scope for this task.
- `docs/exynos9820-port/HANDOFF.md:135` confirms M8 is unfinished and lists the DSC/FDF
  as the next items — consistent with what was created here.
- `firmware/tools/build-uefi.ps1` still passes `-D STAR2LTE_RAM_STAGE` /
  `-D STAR2LTE_USB_DEBUG_MODE` and has no `beyond0lte`/`9820` branch (grep found none).
  The 9820 DSC ignores both switches (harmless: they become unused `-D` flags), but the
  build script will need a 9820 platform branch before this can be built. NOT changed —
  out of scope, and it would touch the 9810 build path.

## Sources

Files actually consulted (all local, no web fetches in this task):

- `firmware/Platform/Star2LtePkg/Star2LtePkg.dsc`
- `firmware/Platform/Star2LtePkg/Star2LtePkg.fdf`
- `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h`
- `firmware/Silicon/Exynos9820Pkg/Exynos9820Pkg.dec`
- `firmware/Silicon/Exynos9810Pkg/Exynos9810Pkg.dec` (read only, to mirror which PCDs the DSC overrides)
- `firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib/{PlatformMemoryMapLib.c,ArmPlatformLib.c,PlatformMemoryMapLib.inf}`
- `firmware/Platform/Beyond0LtePkg/AcpiTables/{AcpiTables.inf,Gtdt.aslc}`
- `firmware/Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib/Exynos9810SerialPortLib.inf`
- `firmware/edk2/ArmPlatformPkg/PeilessSec/PeilessSec.c`, `firmware/edk2/ArmPlatformPkg/MemoryInitPei/MemoryInitPeiLib.c`
- `notes/dock-desktop.md` §16, §17, §18
- `docs/exynos9820-port/HANDOFF.md`, `docs/exynos9820-port/MILESTONES.md`
- `firmware/tools/build-uefi.ps1` (grep only)
