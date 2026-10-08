# Repository map

Quick reference for navigating this tree. Generated 2026-10-08; see the per-subsystem
maps in [`docs/map/`](docs/map/).

**Project:** run Windows 11 ARM64 on the Samsung Galaxy S9+ **SM-G965F** (Exynos 9810,
`star2lte`). One validated device; unfinished stages are gated, never guessed.

```
stock Android ──► unlock ──► TWRP (RECOVERY+BOOT) ──► backup ──► write Windows (USERDATA)
   └─ PC side: build VHDX image (apply+drivers+slim+OOBE+bcdboot) ─► windows.img + esp
                                                    UEFI (BOOT) ─► first boot ─► Windows desktop
```

## Components

| Path | What | Language | Size | Map |
|------|------|----------|------|-----|
| `firmware/` | EDK2 UEFI for star2lte (Platform + Silicon packages, boot shim, build/pack scripts) | C / AArch64 asm / DSC+FDF | ~22.6 kLOC, 67 files | [docs/map/firmware.md](docs/map/firmware.md) |
| `drivers/` | Windows WDK drivers: UFS storage, HSI2C bus controller, S6SY761 touchscreen | C / C++ | ~27.7 kLOC | [docs/map/drivers.md](docs/map/drivers.md) |
| `installer/` | WinUI 3 installer (`S9Woa.Installer.App`) over a C# engine (`S9Woa.Installer.Core`) + xUnit tests | C# | ~19.8 kLOC | [docs/map/installer.md](docs/map/installer.md) |
| `tools/twrp-winre/` | WinRE-look TWRP re-skin: Python reference builder, theme XML, original art, GPL kernel modules, `/sbin` scripts | Python / XML / shell / C | ~6.7 kLOC | [docs/map/tools.md](docs/map/tools.md) |
| `tools/release/` | Release importer, leak scanner, payload scripts | Python / PS1 | (incl. above) | [docs/map/release.md](docs/map/release.md) |
| `docs/` | Maintainer docs: installer architecture, on-device validation | Markdown | — | — |
| `release/` | Import manifest, provenance, scan allow-list (release pipeline inputs) | JSON / txt | — | [docs/map/release.md](docs/map/release.md) |

413 files total. Licenses: BSD-2-Clause-Patent everywhere except
`drivers/S6SY761Touch/` (GPL-2.0-only) and `drivers/Exynos9810Hsi2c/` (BSD-2-Clause-Patent AND MS-PL).

## The install flow (single source of truth)

`installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs` — 11 stages:

| # | id | stage | availability | destructive |
|---|----|-------|--------------|-------------|
| 1 | `host` | Check this PC | Ready | |
| 2 | `identify` | Identify the phone | Ready | |
| 3 | `media` | Get Windows | Ready | |
| 4 | `image` | Build the Windows image (PC-side) | Ready | |
| 5 | `unlock` | Unlock the bootloader | **Guided** | ✔ |
| 6 | `twrp` | Install TWRP | Ready | ✔ |
| 7 | `backup` | Back up EFS/modem/GPT | Ready | |
| 8 | `partition` | Prepare partitions | Ready | ✔ |
| 9 | `transfer` | Copy Windows to the phone | Ready | ✔ |
| 10 | `uefi` | Install UEFI | Ready | ✔ |
| 11 | `firstboot` | First boot | Ready | |

Availability enum: `Ready` / `Guided` / `Experimental` / `NotImplemented`. Nothing is
experimental today (all validated end to end on the reference phone).

## Device targets (validated layout, `Deploy/PartitionMap`)

| Partition | Role |
|-----------|------|
| `USERDATA` | Windows NTFS volume (only used clusters written, via `$Bitmap`) |
| `BOOT` | UEFI image (replaces TWRP at the UEFI stage) |
| `RECOVERY` | TWRP / WinRE-look recovery (kept after UEFI stage) |
| `CACHE` / `SYSTEM` | FAT32 ESPs (4096-byte sectors); boot files + BCD copy |

## Build commands (all Windows-hosted)

```powershell
# Installer (needs .NET 8 + Windows App SDK)
cd installer
dotnet build S9Woa.Installer.sln -c Release -p:Platform=x64
dotnet test tests\S9Woa.Installer.Core.Tests\S9Woa.Installer.Core.Tests.csproj -p:Platform=x64

# Firmware (EDK2 BaseTools + CLANGPDB/LLVM)
firmware\tools\build-uefi.ps1          # full FD; -Module <inf> for one component
firmware\tools\pack-uefi.ps1           # [bootshim][FD] -> flashable boot.img

# Drivers
drivers\Exynos9810Ufs\build-ufs.ps1 -SourceDir ... -OutDir ... -WideUncached -DmaWindow
msbuild drivers\Exynos9810Hsi2c\Exynos9810Hsi2c.vcxproj /p:Configuration=Release /p:Platform=ARM64
drivers\S6SY761Touch\Build-Touch.ps1 -OutputDirectory <new folder>

# Release tooling
python tools\release\scan.py                       # leak gate, must be clean
python -m pytest -q tools\release\tests
tools\release\make-release-extras.ps1 ...          # installer zips + prebuilt recovery
tools\release\make-payload.ps1 ...                 # firmware.json, drivers.zip, SHA256SUMS
```

CI (`.github/workflows/ci.yml`, windows-latest): leak scan → importer tests → Core
tests → x64 app build. All four must pass.

## Key entry points

| Concern | File |
|---------|------|
| Install flow / stage gating | `installer/src/S9Woa.Installer.Core/Stages/StageCatalog.cs` |
| Stage runner wiring | `installer/src/S9Woa.Installer.App/Pages/InstallPage.xaml.cs` (`RunStageAsync`) |
| Wizard shell | `installer/src/S9Woa.Installer.App/MainWindow.xaml.cs`, `Wizard.cs` |
| First-run tool acquisition | `installer/src/S9Woa.Installer.Core/Toolset/ToolsetManager.cs` |
| Device-facing deploy engine | `installer/src/S9Woa.Installer.Core/Deploy/` |
| Samsung Download-mode protocol (built-in flasher) | `installer/src/S9Woa.Installer.Core/Deploy/Odin/OdinSession.cs` |
| WinRE recovery build (C#) | `installer/src/S9Woa.Installer.Core/Twrp/WinReTwrpBuilder.cs` |
| Python reference builder | `tools/twrp-winre/build.py` |
| UEFI platform description | `firmware/Platform/Star2LtePkg/Star2LtePkg.dsc` + `.fdf` |
| Boot shim (sboot → 0x90000000) | `firmware/tools/bootshim/bootshim.S` |
| Release import / leak gate | `tools/release/import_sources.py`, `tools/release/scan.py` |

## Things that are NOT in this repo

- Windows media / built images (never redistributed; `*.iso`, `*.wim`, `*.vhdx` gitignored)
- The private research repository (this public tree is **generated** from it — see
  [docs/map/release.md](docs/map/release.md))
- `.release-patches/` and `.release-denylist.txt` (private scrub inputs, back up separately)
- Segoe UI fonts and Microsoft's `UpdateOS-GearAnimation.gif` (come from the builder's PC at build time)
- Release *assets* themselves (on GitHub Releases; `release/` holds only pipeline inputs)

## Device traps (measured, not guessed)

Concentrated in `tools/twrp-winre/README.md` (TWRP XML quirks) and `docs/installer/architecture.md`
(boot/deploy traps). Highlights: P3 startup record halts UEFI at the Samsung logo unless
all-zero; MISC `boot-recovery` breaks Download mode (−1); ntfs-3g FUSE self-deadlock needs an
init-service watchdog; USERDATA must be an Android sparse image ("Invalid Magic Code!").
