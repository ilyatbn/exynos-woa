# Installer: what the S9+ flow does reversibly, and exactly what the S10e flow is missing

Question answered here: "one-way" was never meant as *irreversible* — it meant the
install overwrites partitions that hold Android's data. The repo already has a
reversibility path for the 9810; the task is to find out whether it can carry the
9820, and what is missing if not.

## 1. What "one-way" actually touches (from the code, not from memory)

| Stage | Writes to | Source |
|---|---|---|
| `unlock` | "Wipes Android" (bootloader OEM/Download unlock) | `StageCatalog.cs:47` (`Destructive: true`) |
| `twrp` | RECOVERY | `PartitionMap.cs:31`, `TwrpFlasher.cs` |
| `partition` | reformats **CACHE** as FAT32 ESP, touches SYSTEM copy | `PartitionMap.cs:38,46`, `BootFilesService.cs:40` |
| `transfer` | **USERDATA** (the whole Windows volume) | `PartitionMap.cs:28`, `TransferService.cs` |
| `uefi` | **BOOT** (the UEFI image replaces Android's boot) | `PartitionMap.cs:25` |

So the destroyed set is BOOT + CACHE + USERDATA + (Android's SYSTEM FAT copy).
EFS, PARAM, STEADY, KEYSTORAGE, PERSISTENT, CPEFS are **not** written — they are
backed up instead, which is what keeps the IMEI/MACs and makes a stock reflash
possible.

## 2. The reversibility path that already exists, and whether it works on the S10e

`BackupService.cs:36-84` is **device-agnostic**: it enumerates the phone's own
`/dev/block/by-name` links (`TwrpClient.cs:36 ByName`), backs up
`PartitionMap.IdentityBackup` = `EFS, CPEFS, PARAM, UP_PARAM, STEADY, KEYSTORAGE,
PERSISTENT`, `dd`s each into TWRP's RAM, hashes on device and on PC, refuses a
mismatch, and writes `backup-manifest.json`. `RequiredBackup` = `["EFS"]`.

Checked against the S10e's own by-name list (`notes/s10e-partitions.md`, read
2026-10-09): **every one of those names exists** — `efs`→sda3, `cpefs`→sdd1,
`param`→sda4, `up_param`→sda5, `keystorage`→sda6, `steady`→sda9,
`persistent`→sda18. So the backup stage needs no new logic; it is blocked today
only by `StageCatalog.cs:71` marking `["backup"] = NotImplemented` for
`beyond0lte`, which is a *policy* flag, not a missing feature.

There is **no restore stage in the repo at all** — not for the 9810 either. The
9810's return-to-stock is done outside the installer, with Samsung's/ODIN payload
reflash. So "same functionality" for the S10e means the same two ingredients:
a verified identity backup (already available) + the stock payload set for
SM-G970F (the user has offered to supply it).

## 3. The real gaps — hard-coded 9810 geometry, in order of blast radius

| Where | 9810 value | S10e value | Status |
|---|---|---|---|
| `PartitionMap.cs:67 DiskBytes` | 63 963 136 000 | **127 940 489 472** (= 124 944 384 KiB, read) | known |
| `PartitionMap.cs:69 WindowsBytes` | 57 004 785 664 | **119 126 620 160** (= sda31 userdata, 116 334 592 KiB, read) | known |
| `PartitionMap.cs:70 CacheBytes` | 629 145 600 | **629 145 600** (cache = 614 400 KiB) | **identical** — the ESP is the same size on both |
| `PartitionMap.cs:68 WindowsOffset` | 6 951 534 592 | **unknown** | needs the GPT |
| `PartitionMap.cs:58-61` GPT GUIDs | name-derived ("ANDROID MMC DISK", "ANDROID USERDATA", "ANDROID CACHE", "ANDROID SYSTEM") | unconfirmed for beyond0lte | needs the GPT |
| `PartitionMap.cs:25/31` BOOT/RECOVERY | sda10 / sda11 on the 9810 | **sda14 / sda15** here | known by name; the code uses by-name links, so only the *comments* are stale |
| `Toolset/Validation.cs:9 RecoveryPartitionBytes` | 16 638 × 4096 = 68 148 992 | 66 048 KiB = **67 632 896** | known, differs |
| `Toolset/Validation.cs:10 BootPartitionBytes` | 14 080 × 4096 = 57 671 680 | 56 320 KiB = **57 671 680** | **identical** |
| `ToolsetManager.cs:84,89` + `ToolDefinitions.cs:88` | TWRP payload `twrp-3.7.0_9-0-star2lte.img` | needs a **beyond0lte/SM-G970F TWRP build** | missing artefact |
| `Exynos9820Ufs.h:980-998` fence + protected LBAs | 9810 ranges | 9810 ranges are **wrong** here (confirmed by names) | needs the GPT |
| `VhdxImageBuilder.cs:71-90` | builds the virtual disk to `DiskBytes` and demands the partition land at exactly `WindowsOffset`/`WindowsBytes`, else `throw 'The Windows partition does not match USERDATA'` | follows the constants | follows |
| `InstallPage.xaml.cs:642` | verifies the phone's USERDATA size == `WindowsBytes` and CACHE size == `CacheBytes` | would **refuse** on the S10e (118.9 GiB ≠ 53.1 GiB) — it fails safe, it does not mis-write | good |

The two sizes that match by accident (CACHE 600 MiB, BOOT 55 MiB) are why the
9810 flow "looks" transferable; the ones that do not are USERDATA (2.2× bigger),
RECOVERY, and the whole disk.

## 4. What is needed to close it, in the order it would be applied

1. **The S10e's own GPT** — exact start/size per partition, plus the four
   name-derived GUIDs. Any of: `sfdisk -d /dev/block/by-name/sda` (or `fdisk -l`)
   from TWRP or a root shell; the raw first 33 sectors of `sda`; or Samsung's
   partition-table file (the PTL/"Partition Table for Odin" that ships inside the
   OFFLINE_IMAGE package, which lists name + offset + size per partition).
   This single artefact closes `WindowsOffset`, the GUIDs, and the UFS fence
   constants at once.
2. **The stock ODIN payload set for SM-G970F** (the `.img` per partition plus its
   PTL) — that is the restore-to-stock path, and it also carries the exact
   partition geometry the vendor intended, so it double-checks (1).
3. **A TWRP build for SM-G970F** — the backup/transfer path runs through TWRP's
   shell (`dd`, `sha256sum`); the 9810 uses an official star2lte build and the
   toolset validates the filename, so a beyond0lte build is a required artefact.
4. Only then the code change: make `PartitionMap` and `Validation.cs` per-device
   (keyed by `DeviceEligibility.cs:19`'s `beyond0lte`), and re-enable the
   `StageCatalog` overrides as each stage is actually observed.

## 5. What is NOT missing

Nothing about the *firmware* blocks this: the ACPI/DSDT/MADT work is done, and
`DeviceEligibility.cs:19` already carries `SM-G970F / beyond0lte / exynos9820 /
G970FXXSGHWC1, Validated: false`.

## Sources

* `installer/src/S9Woa.Installer.Core/Deploy/{PartitionMap,BackupService,BootConfiguration,BootFilesService,BootRouteService}.cs`
* `installer/src/S9Woa.Installer.Core/{Stages/StageCatalog.cs,Device/DeviceEligibility.cs,Toolset/{ToolDefinitions,ToolsetManager,Validation}.cs,Image/VhdxImageBuilder.cs}`
* `installer/src/S9Woa.Installer.App/Pages/InstallPage.xaml.cs:634-651`
* `notes/s10e-partitions.md` — the by-name + size facts read from the phone
* `drivers/Exynos9820Ufs/Exynos9820Ufs.h:929-1002` — the 9810 fence table
