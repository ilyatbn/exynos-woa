# Windows deployment on the S10e - how the S9+ flow does it, and what that means here

Research summary (2026-10-10, Explore subagent over the repo, citations kept; verify before acting).

## The S9+ flow (installer/src/S9Woa.Installer.Core)
- No repartitioning: Windows goes into USERDATA, the ESP is CACHE reformatted FAT32, UEFI goes to BOOT,
  RECOVERY keeps TWRP (Deploy/PartitionMap.cs:25-41). Only GPT edit: CACHE's type GUID -> ESP in both
  copies with CRCs recomputed, original sectors saved and restored on mismatch (Deploy/GptTypeService.cs:8-14).
  CACHE formatted from TWRP with mkfs.fat / newfs_msdos -F 32 -S 4096 (Deploy/BootFilesService.cs:26-30,73-75).
- Image: PC builds a dynamic VHDX mirroring the phone disk (4Kn), ESP 260 MB + MSR 16 MB + NTFS at exactly
  WindowsOffset/WindowsBytes (Image/VhdxImageBuilder.cs:67-86); DISM /Apply-Image + /Add-Driver (refuses
  zero drivers, Image/ImageBuilder.cs:53-67); unattend.xml; edition pref IoT Enterprise > Pro > Home
  (Image/WindowsMedia.cs:197-212); bcdboot /f UEFI then BCD repointed at the phone's GPT disk/partition
  GUIDs (Deploy/BootConfiguration.cs:6-13). Transfer: NTFS volume exported raw (used clusters), staged in
  TWRP /tmp, dd'd into USERDATA, optional SHA-256 readback (Deploy/TransferService.cs:9-46).
- Beyond0lte stage status: image/backup/partition Experimental; unlock/twrp/transfer/firstboot Guided;
  uefi NotImplemented (Stages/StageCatalog.cs:67-80,108-111,137-149).

## S10e facts (notes/s10e-root-reads.md, notes/pit-geometry.md)
- 4096-byte sectors. USERDATA sda31 LBA 2 151 168..31 234 815 (offset 8 811 184 128 B, 119 126 642 688 B).
  CACHE sda28 LBA 1 983 488..2 137 087 (600 MiB). BOOT sda14 27264..41343. RECOVERY sda15 41344..57855.
  Disk 127 943 049 216 B. Encoded at PartitionMap.cs:78-82.
- GPT non-standard (entry name at +56, HeaderSize 512 which Windows rejects, no backup GPT on disk).
- Second LUN (T04): cpefs on sdd1; Windows/UEFI UFS code assumes one sda.

## Hazards for this device
- DESTRUCTIVE: USERDATA (all Android user data) and CACHE are overwritten. Needs the user's explicit
  consent and a backup first; rollback to Android afterwards = restore stock boot.img + format userdata
  (data loss) or Odin full reflash. HANDOFF §2 / AGENTS.md §8: nothing destructive without consent.
- Windows needs a STORAGE driver to see the UFS disk after boot (else INACCESSIBLE_BOOT_DEVICE): the
  repo's Exynos9820 Windows UFS driver (notes/m8-ufs-hc.md, T04) is unvalidated.
- RAM: UEFI publishes bank0 only (~0.9 GiB, B63) - Windows needs more (follow-up in MILESTONES).
- The GPT HeaderSize 512 quirk: Windows rejects it - check how the S9+ handled it before deploying.

## Order (once UEFI sees the UFS disk)
1. UEFI BlockIo on the UFS LUN with the Android GPT visible (validates the UFS fix).
2. Decide RAM (bank0 + more) and the Windows UFS driver path.
3. With consent + backup: CACHE -> ESP, USERDATA <- Windows image, BCD repointed; then boot.

## Decisions (2026-10-10, user)
- Consent: "Yes, wipe it" - USERDATA + CACHE may be overwritten when we get there (confirm again right
  before the write).
- Windows media: C:\Users\ilya\code\Windows11_22631.2861_Pro_arm64.iso (Windows 11 23H2 Pro ARM64).
- ISO checked (mounted at F:, DISM 10.0.26100.5074, elevated shell = True): sources\install.wim
  5 095 179 610 B; index 1 Windows 11 Home, 2 Home Single Language, 3 Windows 11 Pro (22.4 GB
  expanded). Use index 3 (Pro) - the installer's preference order is IoT Ent > Pro > Home.
- User instruction (2026-10-10): "follow this repo's way of modifying windows, same as the installer
  does" -> use S9Woa.Installer.Core's own pipeline (VhdxImageBuilder/ImageBuilder: DISM Apply-Image +
  Add-Driver (>=1 driver required) + unattend; bcdboot + BootConfiguration repoint to the phone's GPT
  GUIDs; GptTypeService CACHE->ESP; BootFilesService FAT32 format; TransferService raw dd via TWRP).
  No hand-rolled replacement steps.

## Readiness audit (2026-10-10, Explore subagent; verify each before acting)
Windows UFS driver drivers/Exynos9820Ufs (Storport miniport, ACPI\EXYN9820, build-ufs.ps1 cl/link,
UFS_FULL_WINDOWS_MODE 1, write fence LBA 1983488..31236090): ALREADY has the UEFI fixes - DW6
0x04000400 (1024 B resp @1024), DW7 n*128 | 2048<<16 (h:1348-1363); resp/PRDT offsets 1024/2048;
non-GOOD status returned with sense regardless of OCS, Response byte ignored (c:6567-6572, 6718-6730);
4 KB PRDT entries at 128 B stride with 28 FMP dwords (h:851,873,1426-1432), max 16 entries (64 KB);
vendor regs only VERIFIED not written (c:5108-5118); requires the controller warm from UEFI.
HAZARD: PRAM at 0xFED14000/0xFED13000 (h:91,364) mapped + written unconditionally (c:2126,1480,
4700-4704) - 9810 address, inside the 9820's debug_snapshot region. Single LUN, QD 1.
ACPI (DSDT/FADT/MADT/GTDT/PPTT): GICv2 bases, 8 cores 6+2 MPIDRs, timer GSIVs 29/30/27/26, UFS _CRS
consistent. Efficiency classes inverted (BIG=0) - 9810 booted with it. No DBG2/SPCR/CSRT/SMBIOS.
Stale comments (Gtdt 26 MHz unverified, bank0 to 0x100000000, FbText 1440x3040).
RAM: 9810 = 0x90000000+0x2C800000 + bank1 0xC0080000+0xBF80000 and 0xCD044000+0x12FBC000 (~1.2 GB).
S10e options: (A) bank1 minus seclog 0xC0000000+0x80000, FB 0xCA000000..0xCA965000, modem
0xD0000000..0xD9801000; (B) bank2 0xE1900000..0xF0000000 (before modem_if/CP/debug_snapshot);
(C) banks 3/4 need proof EL1 may touch them (B62/B63). Build the UFS driver with -DmaWindow if bank1 is RAM.
BIGGEST RISK: the 9810 needed Star2LtePkg's ~10k-line PlatformBootManagerLib (winload/ntoskrnl patching
for Samsung uH/RKP at EL2: TLBI traps, PSCI, kernel word patches, MuCntfrqKep) - Beyond0LtePkg uses
ArmPkg's generic one. Expect uH-related hangs after ExitBootServices. Keep a uniprocessor option.
Installer: PartitionMap has beyond0lte geometry; other gaps per notes/installer-s10e-gaps.md.
## Order of work
1. Windows UFS driver: PRAM breadcrumbs off on the 9820 (no 0xFED1xxxx writes); rebuild.
2. RAM: publish bank1/bank2 sub-ranges from PlatformMemoryMapLib; boot UEFI to prove EL1 can use them
   (DXE allocates top-down, so a bad range resets immediately - same test as B62).
3. Deploy with the installer's Core pipeline; first Windows boot; then attack uH hangs with the 9810's
   BDS patch set as the reference.

## W1 - Windows UFS driver: PRAM breadcrumbs off (UFS_PRAM_ENABLE 0)
Exynos9820Ufs.h: UFS_PRAM_ENABLE 0 (both 9810 ramoops windows 0xFED14000 / 0xFED13000 lie in the 9820
debug_snapshot carve-out). Exynos9820Ufs.c: UfsLowWdtPramInitialize and UfsPramInitialize map only under
#if UFS_PRAM_ENABLE; every PRAM user already guards on a NULL mapping. (A plain early return tripped
C4702 unreachable-code-as-error; the #if form builds clean.)
Build: build-ufs.ps1 needs vswhere on PATH ("C:\Program Files (x86)\Microsoft Visual Studio\Installer")
and must run from bash/powershell -File (stderr from vcvars is fatal under PowerShell's ErrorAction).
Verified: default 48 128 B, -DmaWindow 48 640 B, both exit 0; llvm-objdump finds no "movk #0xfed1, lsl #16"
in either .sys. Outputs %TEMP%\ufs-b86-default, %TEMP%\ufs-b86-dmawin.

## W2 - RAM for Windows: bank1 + bank2 published (carved), unproven for EL1
PlatformMemoryMapLib: bank loop 3 -> 5 banks (+BANK1 0xC0000000+0x20000000, +BANK2 0xE1900000+0x1E700000);
sorted carve list gains seclog 0xC0000000+0x80000, FB 0xCA000000+0x965000 (page-rounded; still mapped as
DEVICE separately), modem block 0xD0000000..0xD9801000 (modem_spare live; IF2/CP2CP/MSI vendor DT), and
0xF0000000..0x100000000 (modem_if, cp_ram_logging, debug_snapshot, log_bcm). New RAM: 0xC0080000..
0xCA000000, 0xCA965000..0xD0000000, 0xD9801000..0xE0000000, 0xE1900000..0xF0000000 (~581 MB; total
~1.5 GB with bank0). Banks 3/4 stay off (B63). Build exit 0. Test = next boot: DXE allocates top-down.
Boot #26 image (W2 RAM test) %TEMP%\w2\boot26-v2.img sha256 200bd7e114a6fffb12c752ca67d2089b898f6b40fee7797336d35a01d9684fd7

## W3 - boot #26 flashed (2026-10-10, new session)
adb healthy (TWRP 3.7.0_9-2, transport recovery). Pushed s10e-artifacts\boot26-v2.img to /tmp (sha 200bd7e1 ok;
git-bash needs MSYS_NO_PATHCONV=1 for adb remote paths). sda14 before = boot #25 (head -c 7587840 sha 82045f34).
dd bs=4096 conv=fsync; readback sha 200bd7e1…4fd7 = boot #26 on BOOT. Next: adb reboot, observe whether UEFI
reaches the partition scan (bank1/bank2 RAM test) or resets (then read /proc/debug_history from TWRP).
W3 OBSERVED (user, boot #26): no reset loop; the panel log runs through the GPT scan and PartitionDxe BlockIo
installs and stops on "LastBlock : 26BFF" (= 0x26C00 x 4 KiB = 634 880 KiB = sda27 product), then stays still.
Same shape as boot #25 (UfsHc ring/done lines are capped at 40, so later FAT probes print nothing; BDS has no
ESP/BOOTAA64.EFI to load yet). => DXE survived allocating top-down from bank2/bank1: EL1 may use the W2
ranges (0xC0080000..0xCA000000, 0xCA965000..0xD0000000, 0xD9801000..0xE0000000, 0xE1900000..0xF0000000) at least
for UEFI boot-services data. Windows UFS driver to use: the -DmaWindow build (s10e-artifacts\ufs-b86-dmawin).

## W4 - driver package + Core harness (2026-10-10)
Driver package for the image: C:\Users\ilya\code\s10e-artifacts\drivers-w4\Exynos9820Ufs\ = Exynos9820Ufs.inf (repo,
ACPI\EXYN9820, boot-start SCSI miniport) + Exynos9820Ufs.sys from ufs-b86-dmawin (UFS_DMA_WINDOW_ENFORCE=1; unsigned
sha b2aaf084…2dbf), signed with the WDK test cert (thumbprint 5DD5DEE1…3E48, M7.4 mechanism) -> 50 072 B sha
155dd70c…1189. UFS only for the first boot (touch/HSI2C unvalidated; fewer boot-start hazards). No .cat: DISM
/Add-Driver /ForceUnsigned, and BCD testsigning on accepts the embedded test signature.
Harness: installer/tools/S10eHarness (verbs image / check / transfer) - calls VhdxImageBuilder.BuildAsync,
TransferService.WriteRawImageAsync, BootFilesService.WriteAsync exactly as InstallPage does, geometry
PartitionMap.For("beyond0lte"), SlimProfile.Lite, unattend Username S10e. dotnet build -c Release: 0 warnings.
BLOCKERS for `image` found: (1) this Claude session is NOT elevated (IsInRole Administrator = False) - DISM apply,
Mount-VHD and bcdboot need admin; (2) New-VHD is not installed (Hyper-V PowerShell module absent), and
VhdxImageBuilder.CreateScript uses New-VHD/Mount-VHD.
