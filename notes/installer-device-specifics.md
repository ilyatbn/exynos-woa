# Installer device-specifics inventory (star2lte/S9+ -> beyond0lte/SM-G970F)

Goal: every place `installer/src/**` hardcodes the star2lte / S9+ device, so the app can become device-generic.

## Progress log

- Started: located `installer/src` tree (S9Woa.Installer.App + S9Woa.Installer.Core). Reference notes read next: `notes/s10e-root-reads.md`, `notes/pit-geometry.md`, `notes/m9-installer.md`, `installer/src/S9Woa.Installer.Core/Deploy/PartitionMap.cs`.

## 0. Existing per-device mechanism (group 7) - found first, it frames everything else

- `Deploy\PartitionMap.cs:70-71` `GeometryType Star2Lte = new(63_963_136_000, 6_951_534_592, 57_004_785_664, 629_145_600, 57_671_680, 68_148_992, DiskGuid, CacheGuid, UserdataGuid)` - the S9+ byte geometry record.
- `Deploy\PartitionMap.cs:81` `GeometryType Beyond0Lte = new(127_943_049_216, 8_811_184_128, 119_126_642_688, 629_145_600, 57_671_680, 67_633_152, DiskGuid, "{52444e41-494f-2044-6361-636865000000}", "{52444e41-494f-2044-7573-657264617461}")` - the S10e geometry ALREADY EXISTS here (lower-case-derived GUIDs).
- `Deploy\PartitionMap.cs:86-91` `GeometryType For(string? codename) => (codename?.ToLowerInvariant() switch { "beyond0lte" => Beyond0Lte, _ => Star2Lte })` - THE EXISTING PER-DEVICE KEYING MECHANISM, keyed on the codename string; default falls back to Star2Lte.
- `Deploy\PartitionMap.cs:95` `sealed record GeometryType(long DiskBytes, long WindowsOffset, long WindowsBytes, long CacheBytes, long BootBytes, long RecoveryBytes, string DiskGuid, string EfiPartitionGuid, string WindowsPartitionGuid)`.

## 1. Device identity / detection

### 1a. How the app learns the connected device - the ACTUAL call path (adb + getprop, not Download mode)
- `S9Woa.Installer.App\AppServices.cs:89` `public static AdbClient? Adb { get; private set; }`
- `S9Woa.Installer.App\AppServices.cs:101` `Adb = AdbPath is null ? null : new AdbClient(AdbPath, Runner, Privacy.AddSerial);` - the adb binary path comes from `AppServices.AdbPath` (resolved in HostPreflight, see below).
- `S9Woa.Installer.Core\Device\AdbClient.cs:38` `adb devices -l` -> `ParseDevices` (`AdbClient.cs:61-91`) -> `AdbDevice(serial, state, model, product)`; `AdbClient.cs:88` `list.Add(new AdbDevice(parts[0], state, Field("model"), Field("product")))`.
- `S9Woa.Installer.Core\Device\AdbClient.cs:53` `adb -s <serial> shell getprop` -> `ParseGetprop` (`AdbClient.cs:96`) -> flat property dictionary.
- `S9Woa.Installer.Core\Device\DeviceSnapshot.cs:83-113` `FromAdb(...)`: the property keys that identify the phone - `:103` Model = `ro.boot.em.model` / `ro.product.model` / `ro.product.system.model` (fallback: adb `model` with `_`->`-`); `:104` Codename = `ro.product.device` / `ro.product.vendor.device` (fallback adb `product`); `:105` Hardware = `ro.boot.hardware` / `ro.hardware`; `:106` Bootloader = `ro.boot.bootloader` / `ro.bootloader`; `:109` `sys.oem_unlock_allowed`; `:110` `ro.boot.flash.locked`; `:111` `ro.boot.verifiedbootstate`; `:112` `ro.boot.warranty_bit`; `:89` `ro.twrp.version` decides Recovery mode.
- UI entry points that run that path: `Pages\PhonePage.xaml.cs:67` ListDevicesAsync -> `:88` GetPropertiesAsync -> `:90` FromAdb -> `:91` DeviceEligibility.Evaluate; `Pages\ToolsPage.xaml.cs:25/29/30`; `Pages\InstallPage.xaml.cs:221/236/237/243` and `:433/443/444`; `Device\DeviceActions.cs:51/54`.
- Download mode is NOT probed: `DeviceSnapshot.cs:70-81` `InDownloadMode(knownSerial, knownBootloader)` reuses the previously identified phone; `AppServices.cs:121` `return (true, DeviceSnapshot.InDownloadMode(State.DeviceSerial, State.DeviceBootloader));` - i.e. in Download mode the model/codename/hardware are the CACHED ones, matched by `SamsungBuild.TryParse(knownBootloader, t.ModelCode)`.

### 1b. Hardcoded identity constants
- `Device\DeviceEligibility.cs:9` `GalaxyS9Plus = new("SM-G965F", "G965F", "star2lte", "exynos9810", "G965FXXUHFVG4")` - model, model code, codename, hardware token, validated bootloader string.
- `Device\DeviceEligibility.cs:19` `GalaxyS10e = new("SM-G970F", "G970F", "beyond0lte", "exynos9820", "G970FXXSGHWC1", Validated: false)` - the S10e equivalent ALREADY EXISTS (bootloader string read on RF8M21HYDSZ per notes/m9-installer.md:10-11).
- `Device\DeviceEligibility.cs:21` `All = [GalaxyS9Plus, GalaxyS10e]` - the supported list; adding a device means adding to this list.
- `Device\DeviceEligibility.cs:46` `targets.FirstOrDefault(t => string.Equals(t.Model, d.Model, ...))` - model-string equality is the ONLY match key; codename is never matched directly.
- `Device\DeviceEligibility.cs:49-50` `d.Model.Contains("G960") || d.Model.Contains("G965")` - hardcoded S9-family sniff used only to pick the error text.
- `Device\DeviceEligibility.cs:54-55` user-facing "Only the Exynos Galaxy S9+ (SM-G965F) and the Galaxy S10e (SM-G970F) are recognised today. Snapdragon models and the S9 (SM-G960F) are not." / "This installer only supports the Samsung Galaxy S9+ (SM-G965F) and the Galaxy S10e (SM-G970F)."
- `Device\DeviceEligibility.cs:76` `SamsungBuild.TryParse(target.ValidatedBootloader, target.ModelCode)` - the firmware-version gate is per-target already (group 5).
- `Device\DeviceSnapshot.cs:7` doc comment hardcodes the example build string `G965FXXUHFVG4`.
- `Device\DeviceEligibility.cs:66-73` chipset check compares `d.Hardware` against `target.HardwareToken` ("exynos9810" / "exynos9820") - already per-device.

### 1c. Other identity gates
- `Toolset\ToolDefinitions.cs:68` `TwrpPage = "https://twrp.me/samsung/samsunggalaxys9plus.html"` - HARDCODED star2lte TWRP download page. beyond0lte equivalent URL is NOT in this repo (would be https://twrp.me/samsung/samsunggalaxys10.html - unverified here).
- `Toolset\ToolDefinitions.cs:88-90` tool row name "TWRP recovery for star2lte" and purpose text naming "twrp-3.7.0_9-0-star2lte.img".
- `Toolset\ToolsetManager.cs:17` `ReleaseRepo = "ntdevlabs/exynos9810-woa"` - the GitHub release repo the payloads are downloaded from; per-chipset, not per-model, but still 9810-named. beyond0lte equivalent: none in repo (the repo itself is `exynos-woa`; `firmware/Silicon/Exynos9820Pkg` exists).
- `Toolset\ToolsetManager.cs:57` `UfsInf = "Exynos9810Ufs.inf"` - the built UFS driver package INF the toolset demands. beyond0lte equivalent: `drivers/Exynos9820Ufs/` exists in this repo (see notes/exynos9820-drivers.md) -> would be `Exynos9820Ufs.inf`.
- `Toolset\ToolsetManager.cs:55-56` `SamsungUsbService = "dg_ssudbus"`, `SamsungSigner = "Samsung Electronics"` - Samsung-generic, not model-specific.
- `Toolset\Validation.cs:71` `HardwarePrefix = "VID_04E8&PID_685D"` - hardcoded Samsung Download-mode USB VID/PID for the Zadig/Heimdall check. Same VID for both phones; PID 685D not verified per-model in this repo.
- `Recovery\RecoveryTicketProtocol.cs:40` `Subcode = 0x9810` and `:32/:35` doc "Exynos9810Ufs clean recovery (UCR1)" - the UFS driver CDB subcode, hardcoded to the 9810 driver. beyond0lte equivalent: `drivers/Exynos9820Ufs/Exynos9820UfsDiag.h` (check for its own subcode).
- `Recovery\RecoveryTicketService.cs:14` doc "Drives the Exynos9810 UFS clean recovery (UCR1) ticket"; `:130` user text "with the Exynos9810 storage driver installed."; `:137` restart reason "Restarting to TWRP recovery (Galaxy S9+ Windows installer)."
- `Toolset\ReleaseClient.cs:32` UA "S9WoaInstaller"; `Stages\StageCatalog.cs:138` / `Image\UnattendXml.cs:42` (`HKU\S9WoaDefaultUser`) / `S9Woa.Installer.App.csproj:8` `<Product>Galaxy S9+ Windows Installer</Product>` - product-name strings, not device lookups.
## 2. Geometry / offsets / sizes

- `Deploy\PartitionMap.cs:70-71` `Star2Lte` geometry: DiskBytes 63_963_136_000, WindowsOffset 6_951_534_592, WindowsBytes 57_004_785_664, CacheBytes 629_145_600, BootBytes 57_671_680, RecoveryBytes 68_148_992. beyond0lte equivalents ALREADY at `:81-84`: 127_943_049_216 / 8_811_184_128 / 119_126_642_688 / 629_145_600 / 57_671_680 / 67_633_152 (notes/pit-geometry.md:44-51, notes/s10e-root-reads.md §2). NOTE: notes/installer-s10e-gaps.md:48-49 still lists the OLD (filesystem) values 127_940_489_472 / 119_126_620_160 - the source is now the corrected one.
- `Deploy\PartitionMap.cs:70-71` `Star2Lte` passes `DiskGuid, CacheGuid, UserdataGuid` (upper-case name-derived). `:82-84` `Beyond0Lte` passes `DiskGuid` + the two lower-case GUIDs.
- `Toolset\Validation.cs:9` `RecoveryPartitionBytes = 16_512L * 4096` (= 67_633_152) - ALREADY switched to the beyond0lte (smaller) RECOVERY; comment says star2lte is 16,638 x 4096. This is a MIN-OF-BOTH gate, not per-device.
- `Toolset\Validation.cs:12` `BootPartitionBytes = 14_080L * 4096` (= 57_671_680) - comment asserts BOOT is the same on BOTH phones (notes/pit-geometry.md:50 confirms identical).
- `Twrp\AndroidBootImage.cs:20` `RecoveryPartitionBytes = 68_149_248` - HARDCODED star2lte RECOVERY size. CORRECTED after the pass that produced this note: 68_149_248 / 4096 = **16_638** exactly, so it IS the star2lte PIT value (the earlier arithmetic here said 16_656, which is wrong). The odd value is `PartitionMap.cs:70` `RecoveryBytes = 68_148_992`, which is NOT a whole number of LBAs (16_637.9375) - a transcription slip, fixed to 68_149_248. beyond0lte equivalent = 67_633_152 (16_512 x 4096).
- `Twrp\WinReTwrpBuilder.cs:64` `OfficialTwrpSha256 = "f674dab0...bcd835e1"` - SHA-256 of the official twrp-3.7.0_9-0-star2lte.img. `:67` `OfficialTwrpBytes = 42_670_080` - its exact length. beyond0lte equivalent: NOT in repo (needs a beyond0lte TWRP build; notes/installer-s10e-gaps.md:56, notes/s10e-root-reads.md:154).
- `Twrp\WinReTwrpBuilder.cs:160-178` `MatchesOfficial` / `OfficialPayload` gate on those two constants; `:192-197` the refusal text names star2lte.
- `Twrp\WinReTwrpBuilder.cs:42` doc "verifies the base is exactly TWRP 3.7.0_9-0 for star2lte"; `:11` enum `OfficialTwrp` doc "The official TWRP 3.7.0_9-0 for star2lte".
- `Twrp\PowerOffRoutePatch.cs:18` `KernelSha256 = "1228ba84...7ef594"` - the star2lte TWRP 3.7.0_9-0 recovery kernel hash. `:20-22` `SecRebootOffset = 0x00A912CC`, `SecPowerOffOffset = 0x00A916C4`, `RestartCallOffset = 0x00A91ACC` - ABSOLUTE kernel offsets. `:23-24` `RestartCallBefore = 0xD63F0040` (blr x2), `RestartCallAfter = 0x97FFFE00` (bl sec_reboot) - an encoded branch displacement only valid for those two offsets. `:26-29` `SecRebootSig` / `SecPowerOffSig` byte signatures. All per-kernel; a beyond0lte TWRP kernel has different values. beyond0lte equivalent: NOT in repo.
- `Twrp\PowerOffRoutePatch.cs:12` doc "refuses anything but the known-good star2lte TWRP 3.7.0_9-0 kernel"; `:55` user text "is not the known star2lte TWRP kernel; refusing to patch."
- `Deploy\Gpt.cs:16-17` `NameOffset = 56`, `NameBytes = 72` - the GPT entry field offsets the installer reads the partition NAME at. notes/s10e-root-reads.md:27 records the S10e entries as non-standard with the UTF-16 name at entry+80, so these offsets are a device/layout assumption, not a GPT-spec constant (the spec name field is at +80, 64 bytes). VERIFY against the 9810 capture before changing.
- `Deploy\Gpt.cs:47-49` `header.EntrySize < 128 || header.EntryCount is < 1 or > 1024` - generic bounds.
- `Deploy\GptTypeService.cs:39` `lastLba = size * 512 / sectorSize - 1` and `:45-49` the hard requirement primary.MyLba == 1 && primary.EntriesLba == 2 && primary.AlternateLba == lastLba && backup.MyLba == lastLba && backup.EntriesLba + backup.EntrySectors == lastLba. notes/s10e-root-reads.md:235-258 proved the S10e media has NO backup GPT (every LBA 31_234_812..31_236_095 reads zero) while the header declares AltLBA 31_236_095 -> on the S10e this guard throws "does not have the expected GPT layout ... not editing it". Device-specific failure, not a constant.
- `Deploy\GptTypeService.cs:21-22` `^/dev/block/(sd[a-z])[0-9]+$` - assumes the UFS unit is named sdX. beyond0lte: notes/s10e-root-reads.md shows sda plus CPEFS on sdd1 (a different container).
- `Deploy\BootConfiguration.cs:65-67` writes g.DiskGuid / g.EfiPartitionGuid / g.WindowsPartitionGuid into the BCD - already per-device via the GeometryType g parameter (group 7).
- `Image\VhdxImageBuilder.cs:36` `private GeometryType _geometry = PartitionMap.Star2Lte;` and `:48` `_geometry = geometry ?? PartitionMap.Star2Lte;` - the DEFAULT geometry is the S9+ one; a caller that passes nothing silently builds 9810 geometry. `:74` `-SizeBytes {DiskBytes}`, `:82` `-Offset {WindowsOffset} -Size {WindowsBytes} -Alignment 4096 -GptType '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'`, `:83` the throw "The Windows partition does not match USERDATA" guard, `:92-93` the second guard, `:153` `new PartitionExtent(_geometry.WindowsOffset, _geometry.WindowsBytes)`.
- `Image\VhdxImageBuilder.cs:74` `-BlockSizeBytes 32MB -LogicalSectorSizeBytes 4096 -PhysicalSectorSizeBytes 4096` - the 4096 sector size is the phone UFS sector size (notes/s10e-root-reads.md:24 says 4096 on the S10e too).
- `Deploy\BootFilesService.cs:21-24` `FormatCommand` hardcodes `-S 4096` (and `-c 1` / `-s 1`) - FAT32 with 4096-byte sectors, "the UFS block size". Same on both phones.
- `Deploy\BootFilesService.cs:10-11` doc "keeps even the 600 MiB CACHE above FAT32 minimum cluster count" - a size claim about the reference phone.
- `Deploy\BootFilesService.cs:26-29` `MountsOf` hardcodes TWRP mount points `/cache` for the ESP partition and `/system_root`, `/system` for the secondary - per-device TWRP fstab.
- `Deploy\BootFilesService.cs:64` FAT volume labels "ESP" / "SYSTEM".
- `Deploy\BootFilesService.cs:15` `Mount = "/s9woa_esp"` - installer-private, device-agnostic.
- `Pages\InstallPage.xaml.cs:672-679` `"/sdcard"` unmount and `mountToEnsureUnmounted: "/data"` - TWRP mount points.
- `Deploy\TwrpClient.cs:34` `RamStagingDir = "/tmp/s9woa"`, `:36` `ByName = "/dev/block/by-name"` - device-agnostic (notes/pit-geometry.md:11-12 confirms beyond0lte TWRP uses the same by-name view).
- `Deploy\TwrpClient.cs:209-226` `bs=4194304` / `bs=1048576` dd block sizes - generic.
- `Deploy\TransferPlanner.cs:8` `ChunkMiB = 128` - generic.
- `Deploy\OdinSession.cs:49-53` `FilePartSize = 128 * 1024` default, `:74-76` v2 -> `1024*1024`, `PartsPerSequence = 30` - protocol-derived, generic.
- `Deploy\OdinTwrpFlasher.cs:223-224` doc "a 65 MiB RECOVERY image start from the 55 MiB BOOT partition" - S9+ sizes in a comment; `:164` the gate `bootLength > BootImage.BootPartitionBytes` is per-device-ish (BootPartitionBytes is identical on both).
- `Deploy\OdinTwrpFlasher.cs:196` comment "Observed on the SM-G965F: ... Only official released binaries are allowed to be flashed (RECOVERY)" - a device-specific observation behind a generic catch.
- `Stages\StageCatalog.cs:47` backup stage summary "Copy EFS, modem calibration and the partition table to this PC" - names the 9810 set.

## 3. GPT GUIDs / partition names

- `Deploy\PartitionMap.cs:61` `DiskGuid = "{52444e41-494f-2044-4d4d-43204449534b}"` ("ANDROID MMC DISK"). CONFIRMED correct for the S10e too (notes/s10e-root-reads.md:29-31, read from the live GPT header MediaId). Used by BOTH geometries.
- `Deploy\PartitionMap.cs:62` `UserdataGuid = "{52444e41-494f-2044-5553-455244415441}"` ("ANDROID USERDATA", upper case) - correct for star2lte, WRONG for beyond0lte. beyond0lte equivalent ALREADY in the file at `:84`: `"{52444e41-494f-2044-7573-657264617461}"` ("ANDROID userdata"), confirmed notes/s10e-root-reads.md:32-37.
- `Deploy\PartitionMap.cs:63` `CacheGuid = "{52444e41-494f-2044-4341-434845000000}"` ("ANDROID CACHE") - wrong for beyond0lte; equivalent at `:83`: `"{52444e41-494f-2044-6361-636865000000}"` ("ANDROID cache").
- `Deploy\PartitionMap.cs:64` `SystemGuid = "{52444e41-494f-2044-5359-5354454d0000}"` ("ANDROID SYSTEM") - the ONLY use is the doc comment at `:41-46`; nothing references `SystemGuid` in code (verified: rg "SystemGuid" hits only the declaration). The S10e lower-case form would be "ANDROID system" = `{52444e41-494f-2044-7379-7374656d0000}`, which notes/s10e-root-reads.md:36-37 already read from the kernel cmdline `root=PARTUUID=52444E41-494F-2044-7379-7374656D0000`.
- `Deploy\PartitionMap.cs:55-59` the doc comment asserting "Samsung derives GPT identifiers from names ... the same on every star2lte" plus the CONFIRMED caveat for beyond0lte lower-case names.
- `Deploy\PartitionMap.cs:25` `UefiTarget = "BOOT"`, `:28` `WindowsTarget = "USERDATA"`, `:31` `RecoveryTarget = "RECOVERY"`, `:38` `EfiSystemPartition = "CACHE"`, `:46` `SecondaryEfiSystemPartition = "SYSTEM"`, `:52` `Misc = "MISC"` - NOT per-device today; the names happen to exist on both phones (notes/installer-s10e-gaps.md:53, notes/s10e-root-reads.md §2 table has boot/recovery/userdata/misc/cache/system lower case) and `TwrpClient.ResolvePartitionNameAsync` matches them case-insensitively. THE ROLE IS NOT THE SAME: on star2lte SYSTEM (sda18) is a ~55 MiB FAT copy; on beyond0lte `system` is a 5.98 GiB ext4 Android system partition (notes/s10e-root-reads.md:69, LBA 83968..1543167). `BootFilesService.cs:40` reformats BOTH `EfiSystemPartition` and `SecondaryEfiSystemPartition` to FAT32, and `InstallPage.xaml.cs:686` types `SecondaryEfiSystemPartition` as ESP - on the S10e that would reformat Android system. Highest-risk semantic gap, not a constant.
- `Deploy\PartitionMap.cs:16-19` `IdentityBackup = ["EFS","CPEFS","PARAM","UP_PARAM","STEADY","KEYSTORAGE","PERSISTENT"]`, `:22` `RequiredBackup = ["EFS"]` - all seven names exist on the S10e (notes/installer-s10e-gaps.md:31-36, notes/s10e-partitions.md); device-agnostic in practice. beyond0lte has extra identity partitions the list does not cover: `sec_efs`, `keyrefuge`, `keydata`, `uh`, `uhcfg`, `nad_refer`, `vbmeta` (notes/s10e-root-reads.md §2 table).
- `Deploy\Gpt.cs:14` `EfiSystemType = c12a7328-f81f-11d2-ba4b-00a0c93ec93b` - standard ESP type GUID, device-agnostic.
- `Image\VhdxImageBuilder.cs:82` `-GptType {ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}` - Basic Data type GUID, matches notes/s10e-root-reads.md:38 (all 31 S10e entries are this type).
- `Deploy\Gpt.cs:16-17` `NameOffset = 56`, `NameBytes = 72` - see group 2 note: the S10e entry name is at +80 (notes/s10e-root-reads.md:27). `Gpt.SetPartitionType`/`PartitionType` match entries BY NAME, so a wrong offset means the ESP typing silently finds zero or several matches (`:94-97` throws "Expected exactly one GPT entry named {name}, found {count}").
- `Deploy\Odin\OdinSession.cs:188-189` `RequiresSparseImage` = `partitionName.Equals("USERDATA")` - the sparse-image rule keyed to the USERDATA name; on the S10e Windows IS USERDATA, so the rule still applies but the reason (filesystem-backed partition) differs.
- `Deploy\Odin\OdinTwrpFlasher.cs:155-157` `Pit.Find(pit, "RECOVERY")` / `Pit.Find(pit, "BOOT")` - upper-case PIT names; `Pit.cs:61` matches case-insensitively, and notes/pit-geometry.md:25 shows the beyond0lte PIT uses upper-case names (EFS, BOOT, RECOVERY, USERDATA) - OK.
- `Deploy\BootConfiguration.cs:16` `BootManagerId = "{9dea862c-5cdd-4e70-acc1-f32b344d4795}"` - the standard Windows BCD {bootmgr} id, not device-specific.
## 4. Toolset payload file names + user-facing messages

- `Toolset\ToolsetManager.cs:84` `TwrpPayload = <payload>\twrp-star2lte.img` - the payload FILE NAME is device-suffixed. beyond0lte equivalent: not present; would need `twrp-beyond0lte.img` (or a device-keyed accessor).
- `Toolset\ToolsetManager.cs:89` `TwrpWinrePayload = <payload>\twrp-winre-star2lte.img` - the built WinRE recovery file name. Same problem.
- `Toolset\ToolsetManager.cs:90` `TwrpWinreInfoPath = TwrpWinrePayload + ".json"` - inherits the suffix.
- `Toolset\ToolsetManager.cs:85-86` `UefiPayload = "uefi.img"`, `DriversPayload = "drivers"` - device-agnostic names (the per-device choice is data-driven via `firmware.json`, see group 7).
- `Toolset\ToolsetManager.cs:231` "Open the TWRP page, download twrp-3.7.0_9-0-star2lte.img, then choose it here."
- `Toolset\ToolsetManager.cs:244` "Using a prebuilt WinRE-look recovery as-is. Choose the official twrp-3.7.0_9-0-star2lte.img to get this installer's recovery (repair tools and the install progress screen)."
- `Toolset\ToolsetManager.cs:306` "This is not the official TWRP 3.7.0_9-0 for star2lte. Download twrp-3.7.0_9-0-star2lte.img from twrp.me and choose it, or pick a prebuilt WinRE recovery image."
- `Toolset\ToolsetManager.cs:258-259` "WinRE recovery built from TWRP 3.7.0_9-0, with the built-in gears." / "...with the UpdateOS gears from your file." - version string, not device name, but pinned to the star2lte build.
- `Toolset\ToolsetManager.cs:345` doc "UEFI images (which also carry \"star2lte\" in their names) are skipped" - and `:351-352` the actual filter is `!name.Contains("uefi")`, so the comment describes the release file names, not a star2lte filter. Dead assumption if payloads become beyond0lte-named.
- `Toolset\ToolsetManager.cs:613-617` error "No built UFS driver package ({UfsInf} next to a .sys) found under {folder}." and `:709` "Release drivers.zip does not contain a built {UfsInf} package." - both use the 9810 INF name.
- `Toolset\ToolDefinitions.cs:88` tool row display name "TWRP recovery for star2lte"; `:89-90` purpose text "Choose the official twrp-3.7.0_9-0-star2lte.img; the installer turns it into a Windows Recovery-style recovery on this PC."
- `Toolset\ToolDefinitions.cs:68` `TwrpPage = "https://twrp.me/samsung/samsunggalaxys9plus.html"` - the "Open page" button target (SetupPage.xaml.cs:178-199 launches `Row(sender).Definition.PageUrl`).
- `Toolset\Validation.cs:43-53` `ValidateTwrp` accepts a name containing "star2lte" OR "beyond0lte" (already two-device), message: "This does not look like TWRP for the Galaxy S9+ (Exynos) or the Galaxy S10e (Exynos). Official file names end in -star2lte.img or -beyond0lte.img (not starlte, which is the S9, or star2qlte, which is the Snapdragon model)." NOTE the asymmetry: the validator is two-device but `ToolsetManager.TwrpPayload` is one-device, so a beyond0lte image copied in still lands in `twrp-star2lte.img`.
- `Toolset\Validation.cs:69-94` `DownloadModeDriver` messages "Put the phone in Download mode, open Zadig, choose Options > List All Devices, select the Samsung device, pick WinUSB and click Replace Driver." - device-generic.
- `SetupPage.xaml.cs:209-210` "(USB ID 04E8 685D)" in the Zadig instruction.
- `Pages\InstallPage.xaml.cs:519` `BackupAsync(dir, dev.Model ?? "SM-G965F", dev.Serial, log, ct)` - hardcoded fallback model written into `backup-manifest.json` (`BackupService.cs:80`).
- `Pages\InstallPage.xaml.cs:657` "Partitions verified: USERDATA (... MiB) and the CACHE boot partition match the validated layout." - partition names literal in the message while the values come from `PartitionMap.For(...)`.
- `Pages\InstallPage.xaml.cs:654` "{actual} is {size} bytes, not the validated {expected}. This phone's layout differs; stopping before any write." - generic, good.
- `Pages\InstallPage.xaml.cs:735` "UEFI installed to BOOT{note}. RECOVERY keeps TWRP." - literal partition names.
- `Pages\InstallPage.xaml.cs:579` "Identify the phone first: the Windows volume is built to the exact geometry of the phone's USERDATA..." - literal name.
- `Deploy\TwrpFlasher.cs:71-72` "Boot into TWRP now (hold Volume Up + Bixby + Power) before Android restarts, or the stock recovery will be restored."; `:103` "No phone in Download mode. Power it off, then hold Volume Down + Bixby + Power and press Volume Up at the warning."
- `Pages\InstallPage.xaml.cs:330,344,349,353,366,395,405,763,776` nine separate "Volume Down/Up + Bixby + Power" instructions; `Pages\PhonePage.xaml:67` "Hold Volume Down + Bixby and plug in the USB cable to enter Download mode." The S10e has no Bixby hard key - these key-combo strings are device-specific UI text.
- `Pages\PhonePage.xaml.cs:106` `var m when m.Contains("G965") => $"Galaxy S9+ · {m}"`; `:176` `DeviceTitle.Text = $"Galaxy S9+ · {phone.Model}"` - unconditional "Galaxy S9+" in Download mode.
- `MainWindow.xaml.cs:199` `PhoneTitle.Text = ... m.Contains("G965") ? "Galaxy S9+" : d.Model ?? "Phone"` - the only display string keyed on the model (notes/m9-installer.md:46-48 says this was left alone).
- `MainWindow.xaml:6` `Title="Galaxy S9+ Windows Installer"`, `:27` `<TitleBar Title="Galaxy S9+ Windows Installer" Subtitle="Preview">`; `S9Woa.Installer.App.csproj:8` `<Product>Galaxy S9+ Windows Installer</Product>`.
- `Pages\WelcomePage.xaml:12` "Windows on your Galaxy S9+"; `:14` "This installer takes an Exynos Galaxy S9+ (SM-G965F) from stock Android to Windows 11 on ARM..."; `:32` "80 GB free on this PC"; `:57` Knox warning.
- `Pages\AboutPage.xaml:11,13,21` "Galaxy S9+ Windows Installer" / "...for the Samsung Galaxy S9+ (Exynos 9810)..." / "...postmarketOS Exynos 9810 communities...".
- `Pages\ToolsPage.xaml:45` "When you are running Windows on the Galaxy S9+, ... It talks to the Exynos9810 storage driver directly..."; `:37` "(UEFI lives in the BOOT partition)".
- `Pages\ToolsPage.xaml.cs:56` "Looking for the Exynos9810 storage driver..."; `:69` "This only works while running Windows on the Galaxy S9+, with the Exynos9810 storage driver installed. No compatible disk answered on this PC."
- `Recovery\RecoveryTicketService.cs:130` "No disk on this PC supports Restart to TWRP. This tool only works while running Windows on the phone, with the Exynos9810 storage driver installed."; `:137` "Restarting to TWRP recovery (Galaxy S9+ Windows installer)."
- `Stages\StageCatalog.cs:42` identify stage summary "Model, firmware and bootloader state over ADB."; `:47` backup summary names "EFS, modem calibration and the partition table".
- `Toolset\ToolsetManager.cs:182` "Download \"Samsung USB Driver for Mobile Phones\" from Samsung, then choose the installer here." - generic.
- `Toolset\ReleaseClient.cs:32` UA "S9WoaInstaller"; `Toolset\ToolsetManager.cs:17` default `ReleaseRepo = "ntdevlabs/exynos9810-woa"` (user-overritable: `SetupPage.xaml.cs:214-223 OnRepoChanged`).
## 5. SHA-256 gates and version checks

- `Twrp\WinReTwrpBuilder.cs:64` `OfficialTwrpSha256 = "f674dab0134f3c929982077b6a3a8de7df209a45c76ead80bc009381bcd835e1"` - SHA-256 of twrp-3.7.0_9-0-star2lte.img. `:163` and `:174` compare it; `:192-197` refuses. beyond0lte equivalent: NOT in repo.
- `Twrp\WinReTwrpBuilder.cs:67` `OfficialTwrpBytes = 42_670_080` - the exact byte length of that file, used at `:169-178` to accept a zero-padded partition dump. beyond0lte equivalent: NOT in repo.
- `Twrp\PowerOffRoutePatch.cs:18` `KernelSha256 = "1228ba84942d1b16f565c42af242eaf1ce084eb373f71cfa40fee74a7e7ef594"` - the star2lte TWRP kernel hash; `:35` `IsPatchable` and `:52` `Patch` both refuse on mismatch. beyond0lte equivalent: NOT in repo.
- `Twrp\PowerOffRoutePatch.cs:26-29` `SecRebootSig` / `SecPowerOffSig` hex byte signatures + `:20-24` absolute offsets - the second gate besides the hash.
- `Twrp\WinReTwrpBuilder.cs:55` `BuilderVersion = "winre-2"` - builder stamp; `ToolsetManager.cs:237-247` and `:247` compare `info.Builder != WinReTwrpBuilder.BuilderVersion` and `TwrpBaseSha256() != info.BaseSha256`. A per-device builder would need its own version string.
- `Toolset\ToolsetManager.cs:247` `TwrpBaseSha256() != info.BaseSha256` - staleness gate on the base TWRP hash; `:328-340` the caching helper; `:600` `Sha256File(twrpImage) == TwrpBaseSha256()` in the build-folder import.
- `Toolset\ToolsetManager.cs:316-320` `Sha256File`; `:743` "does not match the SHA-256 in firmware.json"; `:718` doc "verified against the release's SHA256SUMS".
- `Toolset\ReleaseClient.cs:20` `ChecksumsAsset = "SHA256SUMS"` - release-level checksum manifest; `:85` streaming hash. Device-agnostic but the release itself is the 9810 repo (`ToolsetConfig.ReleaseRepo`).
- `Image\FirmwareCatalog.cs:12,17,18` `sha256` / `loaderSha256` / `kernelSha256` per UEFI image; `:81-83` `ForBootFiles` matches the built image's winload.efi and ntoskrnl.exe hashes; `:143-148` `Verify`; `:158` refusal. Device-agnostic (data-driven), but `:77` `Present(long expectedBytes)` filters by `BootImage.BootPartitionBytes` (per-device-ish, identical on both).
- `Image\VhdxImageBuilder.cs:129-130` hashes `Windows\System32\winload.efi` and `Windows\System32\ntoskrnl.exe` of the built image -> `BuiltImage.LoaderSha256/KernelSha256` (`:11-12`), consumed at `InstallPage.xaml.cs:592` and `:712`.
- `Deploy\BackupService.cs:65-75` device-vs-PC SHA-256 of each backed-up partition; `:8` `BackupEntry(..., Sha256)`; `:89` `HashFileAsync`. Device-agnostic.
- `Deploy\TransferService.cs:96-99` per-chunk write verification (`wrote ... read ...`); `:249` `HashPartitionWindowAsync`; `:260` PC-side hash. Device-agnostic.
- `Deploy\GptTypeService.cs:112-113` re-read SHA-256 of written GPT sectors. Device-agnostic.
- `Device\DeviceEligibility.cs:76-99` the FIRMWARE VERSION gate: `SamsungBuild.TryParse(target.ValidatedBootloader, target.ModelCode)` vs `SamsungBuild.TryParse(d.Bootloader, target.ModelCode)`; `:84` exact match -> Pass; `:88` `build.BinaryRevision < validated.BinaryRevision || build.ReleaseKey.CompareTo(validated.ReleaseKey) < 0` -> Blocker; else Warning. Already per-target. The S9+ gate string is `G965FXXUHFVG4` (`DeviceEligibility.cs:9`), the S10e gate string `G970FXXSGHWC1` (`:19`, read on RF8M21HYDSZ per notes/m9-installer.md:10-11 and notes/s10e-root-reads.md:101).
- `Device\DeviceSnapshot.cs:13-36` the build-string grammar: `^(?<csc>[A-Z]{2,3})(?<type>[US])(?<rev>[1-9A-Z])(?<os>[A-Z])(?<year>[A-Z])(?<month>[A-L])(?<build>[1-9A-Z])$` plus `StartsWith(modelCode)` - model-code-keyed already (`ModelCode` = "G965F" / "G970F").
- `Device\DeviceSnapshot.cs:7` doc example `G965FXXUHFVG4`.
- `Toolset\Validation.cs:24-31` `BootImage.Validate` size gate ("does not fit the {n} MiB partition") driven by the per-constant sizes in `:9-12`.
- `Stages\StageCatalog.cs:117` `SchemaVersion = 1`, `ToolsetConfig.cs:15` `SchemaVersion = 1`, `WinReRecoveryInfo.cs:11` `SchemaVersion = 1`, `FirmwareCatalog.cs:37` `Schema = "s9woa.firmware-catalog.v1"` - format versions, not device versions.

## 6. UI strings (device-specific text)

See group 4 for the payload-name messages; the pure UI/display list:
- `MainWindow.xaml:6,27` window title + title bar "Galaxy S9+ Windows Installer"; `S9Woa.Installer.App.csproj:8` `<Product>`; `MainWindow.xaml.cs:199` `PhoneTitle` keyed on "G965"; `MainWindow.xaml.cs:194-195` "No phone yet" / "Connected on step 4".
- `Pages\PhonePage.xaml.cs:106` "Galaxy S9+ · {model}" (only when the model contains G965); `:176` "Galaxy S9+ · {model}" (unconditional, Download mode); `:105` "Android device" fallback; `:111-114` TWRP/Android/Unauthorized lines; `:162-164` "Identify the phone once" Download-mode warning.
- `Pages\WelcomePage.xaml:12,14,32,57,65` - "Windows on your Galaxy S9+", "an Exynos Galaxy S9+ (SM-G965F)", "80 GB free on this PC", the Knox/Samsung Pay/Secure Folder warning.
- `Pages\AboutPage.xaml:11,13,21` - "Galaxy S9+ Windows Installer", "Samsung Galaxy S9+ (Exynos 9810)", "postmarketOS Exynos 9810 communities".
- `Pages\ToolsPage.xaml:37,43,45` - "UEFI lives in the BOOT partition", "Restart to TWRP (from Windows on the phone)", "Windows on the Galaxy S9+ ... the Exynos9810 storage driver".
- `Pages\PhonePage.xaml:67` "Hold Volume Down + Bixby ..."; `:68` "Long-press Volume Up to unlock, then confirm with Volume Up. The phone erases itself and restarts."; `:59` "Settings About phone Software information: tap Build number seven times."
- `Pages\InstallPage.xaml.cs:148` "Continue only on your own Galaxy S9+ with its backup."; `:173` "Windows is installed on your Galaxy S9+."; `:762-763,774-776` Samsung-logo / Bixby recovery instructions; `:801-804` `MaxStarts = 12` "the reference phone historically needed up to about ten"; `:806-807` `QuickBounce = 75 s` "A return to TWRP faster than this means Windows was never attempted" - measured on the reference phone.
- `Pages\InstallPage.xaml:126` "Automated, not yet validated end to end. Only with your phone's backup."; `:111,116` "TWRP is already on the phone"; `:100` "Reads each block back on the phone. A few minutes slower."
- `Pages\SetupPage.xaml:83` "Release downloads are only kept when they match the release's SHA256SUMS. The Samsung driver installer only runs when it is validly signed by Samsung Electronics."
- `Pages\ImagePage.xaml:78` `<TextBox ... Text="S9">` default user name; `Pages\ImagePage.xaml.cs:173` `Username = name.Length == 0 ? "S9" : name`; `AppServices.cs:83` `? "S9" : State.AccountName`; `Image\UnattendXml.cs:9` `Username = "S9"`; `Image\UnattendXml.cs:42` `HKU\S9WoaDefaultUser`.
- `Pages\HostPage.xaml.cs:46` `WorkDirectory = Path.Combine(result.Path, "S9WoaInstaller")`; `Stages\StageCatalog.cs:138` `%LOCALAPPDATA%\S9WoaInstaller`; `Redactor.cs:80` `"phone-" + 8 hex of SHA-256(serial)`.
- `Deploy\BootConfiguration.cs:71,97` the sentinel `S9WOA_BCD_PHONE_DEVICES_SET`; `BootRouteService.cs:401-403` sentinel `S9WOA_CLEARED`; `TwrpClient.cs:151` `s9r=$?` - installer-internal markers, not device strings.

## 7. What already picks a per-device value (the existing mechanism)

Key = the **codename string** (`"star2lte"` / `"beyond0lte"`), sourced from `DeviceSnapshot.Codename` (getprop `ro.product.device` / `ro.product.vendor.device`, fallback adb `product`).

- `Deploy\PartitionMap.cs:86-91` `GeometryType For(string? codename)` - the switch; default falls through to `Star2Lte` (so an unknown codename silently gets 9810 geometry).
- Callers of `PartitionMap.For(...)`: `Pages\InstallPage.xaml.cs:581` (image build geometry), `:647-648` (USERDATA + CACHE size verification), `:657` (the success message numbers).
- Callers of `PartitionMap.<GeometryType>` fields: `Image\VhdxImageBuilder.cs:74,82,83,92,93,145,153`; `Deploy\BootConfiguration.cs:65,67` via the `GeometryType g` parameter (`VhdxImageBuilder.cs:128` passes `_geometry`); `Pages\InstallPage.xaml.cs:639-640,678,686,733` for the target names.
- `Stages\StageCatalog.cs:64-95` `PerDevice` dictionary keyed by codename + `Resolve(id, codename)` / `Runnable(id, codename)`. NOT CALLED ANYWHERE IN `installer/src` (verified: only `installer\tests\S9Woa.Installer.Core.Tests\DeviceTests.cs:103-106` uses them). `InstallPage.xaml.cs:25` builds items from `StageCatalog.All` and `:138-145` reads `item.Definition.Availability` - the catalog's own state, never the per-device override. So today the S10e is NOT stopped by the catalog in the app; only `DeviceEligibility.cs:58-64`'s Warning is.
- `Device\DeviceEligibility.cs:5-21` `SupportedTarget(Model, ModelCode, Codename, HardwareToken, ValidatedBootloader, Validated)` + `All` - the device table; `:46` matches on `Model` only; `:66-73` checks `HardwareToken`; `:76-99` checks `ValidatedBootloader`; `:58-64` emits Warning when `Validated == false`.
- `Device\DeviceSnapshot.cs:70-81` `InDownloadMode(knownSerial, knownBootloader, targets)` - re-derives Model/Codename/HardwareToken for a Download-mode phone by trying each target's `ModelCode` against the saved bootloader string.
- `Toolset\Validation.cs:48-49` `ValidateTwrp` accepts either `star2lte` or `beyond0lte` in the file name - the only two-device-aware validator.
- `TwrpClient.cs:158-167` `ListPartitionsAsync` + `ResolvePartitionNameAsync` - the phone's OWN by-name links, matched case-insensitively; this is what makes partition addressing device-agnostic (`BackupService.cs:59`, `BootFilesService.cs:49`, `GptTypeService.cs:27`, `BootRouteService.cs:285`).
- `Toolset\ToolsetManager.cs:17` `Config.ReleaseRepo` is user-editable (`SetupPage.xaml.cs:214-223`), so the payload source is already parameterised - but only by repo, not by device.
- `Image\FirmwareCatalog.cs` (`firmware.json`) is data-driven: images are chosen by Windows build + loader/kernel SHA, NOT by device. There is no device field in `FirmwareImage` (`:9-19`).
- `Toolset\ToolDefinitions.cs:39-48` `ToolDefinition(Id, Name, Purpose, Kind, Required, Sources, WingetId, PageUrl, FilePattern)` - a natural place to add a per-device payload name/page; today `Name`/`Purpose`/`PageUrl` are single-valued.

## 8. How the app learns the connected device (the actual call path)

adb + getprop only. There is no Download-mode identification path: Download mode reuses the cached identity.

1. `Pages\PhonePage.xaml.cs:67` (or `Pages\ToolsPage.xaml.cs:25`, `Pages\InstallPage.xaml.cs:221/433`, `Device\DeviceActions.cs:51`) -> `AdbClient.cs:38` `adb devices -l` -> `AdbClient.cs:61-91 ParseDevices` -> `AdbDevice(serial, state, model, product)` (`AdbClient.cs:88`).
2. `Pages\PhonePage.xaml.cs:88` -> `AdbClient.cs:53` `adb -s <serial> shell getprop` -> `AdbClient.cs:96-107 ParseGetprop`.
3. `Pages\PhonePage.xaml.cs:90` -> `DeviceSnapshot.cs:83-113 FromAdb`: Model from `ro.boot.em.model` / `ro.product.model` / `ro.product.system.model` (`:103`); Codename from `ro.product.device` / `ro.product.vendor.device` (`:104`); Hardware from `ro.boot.hardware` / `ro.hardware` (`:105`); Bootloader from `ro.boot.bootloader` / `ro.bootloader` (`:106`); mode from `AdbState` + `ro.twrp.version` (`:89-98`).
4. `Pages\PhonePage.xaml.cs:91` -> `DeviceEligibility.cs:26 Evaluate(snap)` -> `:46` model match -> `:58-139` checks.
5. `Pages\PhonePage.xaml.cs:92` stores it in `AppServices.cs:129-139 CurrentDevice`; `AppServices.cs:187-188 SaveState()` persists `State.DeviceSerial` + `State.DeviceBootloader` (`Stages\StageCatalog.cs:118-119`).
6. Download mode: `AppServices.cs:115-122 FindDownloadModeAsync` -> `TwrpFlashService.ResolveAsync` (a COM port for `VID_04E8&PID_685D`, `OdinTwrpFlasher.cs:119` / `DownloadModePort.cs:81-96`) then `DeviceSnapshot.cs:70 InDownloadMode(State.DeviceSerial, State.DeviceBootloader)` - identity comes from the saved bootloader string, never from the phone.
7. The codename then reaches the geometry only via `PartitionMap.For(AppServices.CurrentDevice.Codename)` (`InstallPage.xaml.cs:581,647,648,657`).

## 9. Follow-up gaps (things the mechanism does NOT yet cover)

- `PartitionMap.For` default arm (`:90 _ => Star2Lte`) silently serves 9810 geometry to any unknown codename; `DeviceEligibility.cs:46` already blocks unknown models, so the default is unreachable in the install flow but reachable in tests/tools.
- `StageCatalog.Resolve/Runnable` never called from `installer/src` - the `PerDevice` overrides are dead in the app.
- `ToolsetManager.TwrpPayload` / `TwrpWinrePayload` / `UfsInf` / `TwrpPage` / `OfficialTwrpSha256` / `OfficialTwrpBytes` / `PowerOffRoutePatch` constants are single-device with no codename parameter.
- `AndroidBootImage.cs:20 RecoveryPartitionBytes = 68_149_248` is neither phone's PIT value (see group 2) - verify before touching.
- `Gpt.cs NameOffset/NameBytes` vs the S10e's name at +80 (notes/s10e-root-reads.md:27).
- `GptTypeService.cs:45-49` backup-GPT requirement vs the S10e having no backup GPT (notes/s10e-root-reads.md:255-258).
- `BootFilesService.cs:40` + `InstallPage.xaml.cs:686` treat `SYSTEM` as the second ESP - wrong role on beyond0lte.
- `HostPreflight.cs:54 RequiredFreeBytes = 80 GB` and `WelcomePage.xaml:32` "80 GB free" are sized for the 53 GB S9+ volume; the S10e volume is 119 GB.
- `WinReTwrpBuilder.cs:404 UsbAnchor "setprop sys.usb.controller 10c00000.dwc3"` - device-shaped but verified identical on the S10e (notes/dock-desktop.md:93 `getprop sys.usb.controller` on the live S10e).
## 10. Corrections + additions after mechanical verification (rg over installer/src)

- CORRECTION to group 5: `ToolsetConfig.SchemaVersion = 1` is at `Toolset\ToolsetManager.cs:15` (the class lives in that file), not a `ToolsetConfig.cs`.
- VERIFIED `SystemGuid` (`Deploy\PartitionMap.cs:64`) has exactly one occurrence in `installer/` - the declaration. Dead constant; nothing in the code path uses the SYSTEM GUID, only the doc comment at `:41-46` refers to it in prose.
- VERIFIED `PartitionMap.For` is called from exactly four places, all in `Pages\InstallPage.xaml.cs` (`:581, :647, :648, :657`). No Core-side caller passes a codename; `Image\VhdxImageBuilder.cs:36/48` defaults to `Star2Lte` instead.
- ADDITIONAL HIT `Pages\WelcomePage.xaml:59` "This is experimental: only the SM-G965F on one firmware is validated, and cellular, camera, audio and GPU acceleration don't work yet." - a hardcoded per-device validation claim in the welcome screen.
- ADDITIONAL HIT `Pages\WelcomePage.xaml:65` consent checkbox "I understand this erases my phone and that the software is experimental." (generic).
- VERIFIED the only `SM-G965F` literals in code: `DeviceEligibility.cs:9` (the table), `:54/:55` (messages), `InstallPage.xaml.cs:519` (backup-manifest fallback), `OdinTwrpFlasher.cs:196` (comment), plus `WelcomePage.xaml:14/:59`.
- VERIFIED the `S9` default-username chain: `Image\UnattendXml.cs:9` -> `AppServices.cs:83` -> `Pages\ImagePage.xaml.cs:173` -> `Pages\ImagePage.xaml:78`; persisted via `AppServices.cs:183 State.AccountName`.
- VERIFIED `HostPreflight.cs:54 RequiredFreeBytes = 80 GB` is used at `:88` and reported at `:93`; the welcome screen repeats it literally at `WelcomePage.xaml:32`.
## 11. Two RECOVERY-size constants disagree (flagged by review, becomes a follow-up)

- `Toolset\Validation.cs:9` `RecoveryPartitionBytes = 16_512 * 4096 = 67_633_152` (beyond0lte) gates the TWRP file the user picks (`:55 ValidateTwrp`, `ToolsetManager.cs:227`).
- `Twrp\AndroidBootImage.cs:20` `RecoveryPartitionBytes = 68_149_248` gates the BUILT WinRE image (`WinReTwrpBuilder.cs:358-361`).
- Corrected arithmetic: 68_149_248 = **16_638 x 4096**, i.e. it IS the star2lte RECOVERY size; `PartitionMap.cs:70` carried 68_148_992 (16_637.9375 LBAs - not integral, a slip) and is now 68_149_248. The gate is therefore exact for the S9+ and **144 LBAs (590_080 B) looser** than the S10e's 67_633_152, so a built image between 67_633_152 and 68_149_248 passes the builder and would not fit the S10e RECOVERY.

## 12. Blast radius order (mine, inference - for planning, not a claim about the code)

1. Payload/toolset identity: `ToolsetManager.cs:84/89/57`, `ToolDefinitions.cs:68/88-90`, `Validation.cs:48-53` - nothing boots without the right TWRP and driver INF.
2. `AndroidBootImage.cs:20` fit gate + `WinReTwrpBuilder.cs:64/67` + `PowerOffRoutePatch.cs:18/20-29` - the whole WinRE build path is pinned to the star2lte TWRP artefact; a beyond0lte TWRP build is a missing artefact, not a code change.
3. `BootFilesService.cs:40` + `InstallPage.xaml.cs:686` + `PartitionMap.cs:46` - the SYSTEM-as-second-ESP role.
4. `Gpt.cs:16-17` + `GptTypeService.cs:45-49` - the S10e GPT entry layout and the absent backup GPT.
5. `PartitionMap.cs` geometry/GUIDs - already ported, only the `For()` default arm and the missing callers matter.
6. UI strings and messages (group 6) - cosmetic, last.

## Sources

- `installer/src/**` (read directly; every file:line above is from this pass).
- `notes/s10e-root-reads.md` (§2 live GPT, GUIDs, §7 UFS table, §8 header fields, §9 tail).
- `notes/pit-geometry.md` (PIT decode + the S10e geometry table).
- `notes/m9-installer.md` (the DeviceEligibility/StageCatalog change and its verification).
- `notes/installer-s10e-gaps.md` (the pre-port gap table; superseded on WindowsBytes/DiskBytes by the two notes above).
- `notes/dock-desktop.md:93` (`getprop sys.usb.controller` on the live S10e = `10c00000.dwc3`).
- `drivers/Exynos9820Ufs/Exynos9820UfsDiag.h:104,117,122` (the 9820 driver keeps `0x9810` subcodes), `drivers/Exynos9820Ufs/Exynos9820Ufs.inf`, `drivers/S6SY761TouchBeyond0/S6SY761TouchBeyond0.inf` (found by listing `drivers/`).
- `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:285-297,314-335` (UFS HCI 0x13D60000, usb@10C00000) via grep.
- `tools/twrp-winre/build.py:588` (the Python twin of `UsbAnchor`).
- `installer/tests/S9Woa.Installer.Core.Tests/DeviceTests.cs:103-106` (the only users of `StageCatalog.Resolve/Runnable`).

- Finished: inventory written in sections 0-12 plus Sources. No source file touched (research only).
