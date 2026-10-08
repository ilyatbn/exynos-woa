# Map: installer/ — WinUI 3 installer + C# engine

WinUI 3 desktop app (`S9Woa.Installer.App`) over a UI-free engine (`S9Woa.Installer.Core`),
xUnit-tested. ~19.8 kLOC C#. BSD-2-Clause-Patent.
Maintainer doc: [`docs/installer/architecture.md`](../docs/installer/architecture.md).

```
installer/
  S9Woa.Installer.sln
  src/S9Woa.Installer.Core/        engine, no UI dependency (testable/scriptable)
  src/S9Woa.Installer.App/         WinUI 3 shell
  tests/S9Woa.Installer.Core.Tests/  19 xUnit test files
```

## Core engine — by directory

### Stages/ — flow control (start here)
| File | Lines | What |
|------|-------|------|
| `StageCatalog.cs` | 140 | **single source of truth**: 11 `StageDefinition`s (id, title, availability, destructive). See MAP.md table |
| `StageProgress.cs` | 149 | resumable `InstallState` persisted as JSON in the data folder |

### Device/ — ADB-facing device layer
| File | Lines | What |
|------|-------|------|
| `AdbClient.cs` | 109 | adb process wrapper |
| `DeviceSnapshot.cs` | 114 | model/firmware/bootloader state over ADB |
| `DeviceEligibility.cs` | 127 | SM-G965F-only gate |
| `DeviceActions.cs` | — | reboot-to-X actions |

### Host/ — PC preflight (admin, disk space, adb, Samsung USB driver)

### Image/ — PC-side Windows image build
| File | Lines | What |
|------|-------|------|
| `VhdxImageBuilder.cs` | 220 | **runs the whole host build**: diskpart ESP+MSR+NTFS (4Kn to match phone UFS) → apply edition → inject drivers → slim → OOBE → bcdboot → export |
| `ImageServicer.cs` | 366 | applies DISM ops offline (registry hives unloaded) |
| `SlimPlan.cs` / `SlimOperations.cs` | 329/— | profiles: **Stock** (drivers only) / **Lite** (consumer apps off, Update works) / **Core** (tiny11Coremaker-style: CBS removal, Edge/WebView2/OneDrive, WinRE deletion, WinSxS cut, tiny11 registry set, `/ResetBase`) |
| `UnattendXml.cs` | 182 | OOBE answer file: local admin account, skip EULA/MSA/wireless; `specialize` DPI 275 via `RunSynchronous` writes of `LogPixels`/`Win8DpiScaling` (unattend Display/DPI no longer honored) |
| `NtfsAllocation.cs` | 269 | reads NTFS `$Bitmap` → used-cluster MiBs (feeds TransferService's sparse write) |
| `RawImageExporter.cs` / `RawDeviceSource.cs` / `VolumeDiskSource` | — | exports NTFS volume → `work\out\windows.img` |
| `WindowsMedia.cs` | 212 | ISO/ESD handling (unpacks ISO when chosen) |
| `FirmwareCatalog.cs` | 165 | parses `firmware.json` (Windows build → UEFI image + hash) |

### Deploy/ — device-facing engine (all behind injectable process seams)
| File | Lines | What |
|------|-------|------|
| `TwrpClient.cs` | 459 | adb-over-TWRP primitives (list by name, dd, sha256, push/pull); payloads staged in TWRP `/tmp` never stdout; `EnsureResponsiveAsync`/`BreakNtfsDeadlockAsync` breaks ntfs-3g FUSE self-deadlock via `/proc` + OOM score + sysrq-f |
| `PartitionMap.cs` | — | validated star2lte partition names |
| `TransferService.cs` | 261 | writes `windows.img`→USERDATA: only MiBs with used clusters (+first/last MiB), ~10 GiB of 53 GiB; refuses mounted/smaller target |
| `TransferPlanner.cs` | — | chunks ≤128 MiB, staged in `/tmp`, next chunk crosses USB while current writes; *Verify every write* default (read-back SHA-256) |
| `BootFilesService.cs` | — | copies built ESP tree onto phone's FAT ESP + `/cache` BCD copy |
| `BootConfiguration.cs` | — | retargets BCD by locating `\Windows` (no GUID on raw USERDATA volume); public bcdedit only |
| `BootRouteService.cs` | 405 | **first-boot gate**: clears MISC `boot-recovery`, acks `RECOVERY_PENDING` via `rwd1_ack.ko`, reads RWD1 (64B @0xFED13D80) + P3/SMP1 (128B @0xFED13E80) via `rwd1_evidence_reader.ko`, zeroes non-clear record (`rwd1_clear_poc.ko` token-gated, `pram_smp_clear_poc.ko`), reads back; refuses restart unless clear; saves `logs\startup-records-*.txt` |
| `Gpt.cs` / `GptTypeService.cs` | 166/121 | SYSTEM partition GPT type → EFI system type (both copies + CRCs), originals saved to backup |
| `BackupService.cs` | — | verified EFS/modem/GPT backup to PC (device/PC sha256 cross-check + JSON manifest; refuses if `efs` missing) |
| `TwrpFlasher.cs` / `TwrpClient` flashers | — | `ITwrpFlasher` + `TwrpFlashService` (first available) + `HeimdallTwrpFlasher` fallback (needs Zadig WinUSB) |
| `Odin/OdinSession.cs` | 259 | **built-in flasher**: Samsung Download-mode protocol (ODIN/LOKE handshake, begin session + protocol probe, PIT in 500-B parts, 128 KiB/1 MiB file parts, end session no reboot) |
| `Odin/OdinTwrpFlasher.cs` | 246 | looks up RECOVERY+BOOT in phone's PIT by name, writes TWRP to both in one session, reboots → phone starts TWRP from BOOT itself (BOOT 55 MiB < RECOVERY 65 MiB: BOOT gets image without zero padding) |
| `Odin/Pit.cs` | — | PIT parser; `FlashPartition` refuses raw image for USERDATA (must be Android sparse — "Invalid Magic Code!") |
| `WinReStatus.cs` | — | keeps `/tmp/s9woa/status` current during writes (appended to the chunk's `dd` command, exit code preserved) |

### Twrp/ — WinRE-look recovery builder (C# port of `tools/twrp-winre`)
| File | Lines | What |
|------|-------|------|
| `WinReTwrpBuilder.cs` | 481 | **whole build, deterministic**: verifies base = official `twrp-3.7.0_9-0-star2lte.img` by SHA-256; asserts DTBH/2nd-stage/kernel unchanged (bar patch); output fits RECOVERY |
| `AndroidBootImage.cs` / `CpioArchive.cs` | 146/190 | boot image + newc ramdisk byte-for-byte round-trip (port of `bootimg.py`) |
| `Lzma/` (13 files) | ~3.4 k | vendored public-domain 7-Zip LZMA SDK — "LZMA alone" framing the kernel expects |
| `PowerOffRoutePatch.cs` | 107 | four-byte hash/pattern-gated kernel patch → USB "Turn off" powers off via Samsung hook |
| `WinReTheme.cs` / `WinReResources.cs` | 330/— | applies committed `tools/twrp-winre/theme/reskin.xml` to stock `ui.xml`/`portrait.xml`; all 57 stock bitmaps replaced; embeds WinRE pages, `/sbin` scripts, GPL modules; Segoe from builder's `%WINDIR%\Fonts` (or `fonts/` open set → stamp `fonts=open`) |
| `GifDecoder.cs` / `GearFrames.cs` | 308/264 | optional Microsoft UpdateOS GIF → white-on-black frames (never leave the built image) |

### Toolset/ — first-run setup
`ToolDefinitions.cs` (one card per item) → `ToolsetManager.cs` (826L: detect/acquire/persist `toolset.json`),
`ToolLocator` (override → app `tools\` → winget portable → PATH), `WingetClient` (success = re-detection),
`AuthenticodeVerifier` (Samsung-signed driver installer via WinVerifyTrust),
`ReleaseClient.cs` (127L: release assets only if `SHA256SUMS` match; `firmware.json` catalog + per-image hash;
archive-escape guard on `drivers.zip`; local build folder takes precedence),
`Validation.cs`, `Platform.cs` (272L).

### Recovery/ — UCR1 "restart to recovery" ticket protocol
`RecoveryTicketProtocol.cs` / `RecoveryTicketService.cs` (147L) / `ScsiPassThrough.cs` (185L).

### Misc
`CheckResult.cs`, `Redactor.cs` (serials → `phone-<8 hex of SHA-256>`, `%USERPROFILE%`, PC Windows version → `(hidden)`),
`Processes/ProcessRunner.cs`.

## App shell

`MainWindow.xaml(.cs)` (207L): step rail + content surface + footer ("Step n of 6").
`Wizard.cs` (115L): `IWizardStep` contract (`CanAdvance`, `NextLabel`, `OnAdvanceAsync`, `CanGoBack`).
`AppServices.cs` (191L): `Redact`, logging.

Pages: `WelcomePage`, `SetupPage` (226L), `HostPage`, `PhonePage` (213L), `ImagePage` (178L),
`InstallPage` (919L — `RunStageAsync` wires stages; *Run experimental steps* switch),
`ToolsPage` (146L — QoL tools e.g. "Restart to TWRP"), `AboutPage`; `DeviceCommands.cs`.
Debug-only `--unlock-all-steps` for UI work (Release ignores).

## Tests (20 files, xUnit)

BootChain, Deploy, Device, Flasher, Gpt, ImageBuilder, Image, NtfsAllocation, NtfsAndStartup,
Odin, OobeAndExport, Privacy, Progress, RecoveryAndState, TicketService, Toolset, Transfer, WinReTwrp, TestGifs.
Registry/signature/winget/HTTP behind interfaces → covered without touching PC/network.

## Data folder

`%LOCALAPPDATA%\S9WoaInstaller` unless a `data` folder sits next to the exe (portable).
Holds state, `toolset.json`, logs, payload, `backups\phone-<hash>`.
