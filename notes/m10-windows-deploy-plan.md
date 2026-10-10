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

## W5 - what the 9810's PlatformBootManagerLib does to Windows (Explore subagent, 2026-10-10; cited, verify before use)
PBML = firmware/Platform/Star2LtePkg/Library/PlatformBootManagerLib/PlatformBootManagerLib.c (10 033 lines).
Trigger: ~25 gBS/gRT pointers hooked in AfterConsole (PBML:6288-6344, 9920). Patching runs inside the GetMemoryMap
hook on winload's final zero-size probe (PBML:4843-4846, 4934-4935), not at ExitBootServices; the EBS hook only logs and
scans kernel page tables at a hard-coded 0x907AA000 (PBML:4987-5074, 1214-1266). Comment PBML:4891-4932: the ntoskrnl
CNTFRQ patch and the winload timer/TLBI patch are the two the 9810 cannot boot without.
- ntoskrnl (PBML:1916-1990): found by PE export name in 0x90000000..0xBC800000 (build-independent). CNTFRQ: a
  movz/movk x8,#26000000;ret stub in .text padding, every `mrs x8,cntfrq_el0` -> `bl stub` (9810 CNTFRQ reads 0, EL1
  writes trap under uH). Fixed-RVA word table (PBML:1427-1467, 1800-1853), each guarded by an original-word check:
  sysreg rewrites, PMU init skip (0x44B7F0), PSCI_VERSION probe -> mov w0,#2 (EL3 is PSCI 0.1, 0x9CEFA0). RVAs mix
  at least two kernel builds (19041/22621). Pattern patch Star2LtePatchNtosPsciMemProtect (PBML:1638-1675): `ret`
  32 B before a 16-byte match, target unchecked.
- winload (PBML:2360-2757): ABSOLUTE 9810 physical addresses (winload at ~0x912D4000): tlbi -> nop at 4 sites (uH cannot
  emulate an MMU-off tlbi; PBML:2337-2383), brk #0xf004/#0xf003 -> branch, an MMU-off stub at 0x91489000 mapping 9810
  PRAM 0xFED14000; UNGUARDED trace branches T0-T3/T6 and a "MicrovisorMode" word = 1 at 0x91504C80. Would MISPATCH on
  the 9820 (different RAM base/size => different load address) - do not port as-is.
- Firmware shims: AllocatePages pre-reservations 0xBBC08000/0xBB752000 (PBML:5187-5205, 5814-5843); SecureBoot/
  AuditMode/DeployedMode = 0, SetupMode = 1, OsIndicationsSupported/CurrentPolicy NOT_FOUND (PBML:5882-5955,
  6578-6605); fake GetTime (PBML:6126-6146); exception handler repairing L3 translation faults (PBML:3145-3381);
  PSCI parking ON: CPU_ON only for A55 MPIDRs 1-3 into a parking stub + MADT ParkedAddress patch (PBML:8253-8577),
  because Samsung EL3 does not pass context_id in x0 and CPU_ON of an M3 core hangs the SoC.
- MuCntfrqKep.c is NOT compiled (not in the INF [Sources]) and never called - docs/map/firmware.md:15,65 is stale.
- No build pinning in firmware source; pinning lives only in the installer's firmware.json catalog (release assets,
  not in git; FirmwareCatalog.cs:12-141). Validated builds: 22621/22631.2428 and 22621.7582/22631.7584. Our ISO is
  22631.2861 (not validated anywhere).
- Beyond0LtePkg uses ArmPkg's generic PlatformBootManagerLib (Beyond0LtePkg.dsc:212) => none of the above.
Inference for the S10e: CNTFRQ_EL0 reads 26 MHz on the 9820 (m8-uefi-image.md:1376), so the CNTFRQ patches may be
unneeded; the uH tlbi-in-MMU-off-winload hang, PSCI 0.1 / context_id, and Secure Boot variables are the likely
first walls. Plan: first boot with numproc 1 and observe; port the guarded/pattern-based pieces one at a time.

## W6 - elevated session: Hyper-V module (2026-10-10)
Session elevated (IsInRole Administrator = True). Enabling Microsoft-Hyper-V-Management-PowerShell alone is refused
("parent features are disabled"). Enabling Microsoft-Hyper-V-All + Tools-All + Management-PowerShell (no -All flag)
ENABLED THE WHOLE Hyper-V SET, hypervisor and services included (DISM enables a container's children) -
RestartNeeded = True, -NoRestart given, nothing restarted. New-VHD is still not available before the reboot.
Undo if wanted: Disable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -NoRestart (also needs a reboot).

## W7 - installer bug: ISO attach (2026-10-10)
After the PC reboot: elevated, New-VHD = Hyper-V module, vmms Running. First `S10eHarness image` run failed at
WindowsMedia.MountIsoScript: "Mount-DiskImage : Parameter set cannot be resolved" - `-InputObject` and
`-StorageType ISO` are in different parameter sets. Fix: drop -StorageType (Get-DiskImage -ImagePath already
identifies the ISO). Verified by hand on this PC (mounted as F:, dismounted) and the unit test updated
(asserts no -StorageType); dotnet test --filter OpensAnIsoReadOnly: 1 passed.

## W8 - S10e PSCI is 0.1 (stock dt.dtb read, 2026-10-10)
/psci: compatible 'arm,psci' (no psci-0.2/1.0), method smc, cpu_suspend 0xC4000001, cpu_off 0x84000002,
cpu_on 0xC4000003 - the same legacy PSCI 0.1 IDs the 9810 had. So the 9810's reasons for PSCI_VERSION -> 2
(ntoskrnl) and for CPU parking (context_id not passed in x0) plausibly apply here too (unproven on the 9820).
First boot: numproc 1 avoids CPU_ON; PSCI_VERSION behaviour of Samsung EL3 on 0x84000000 is UNVERIFIED.
Image build running: install.wim index 3 reports build 22621.2861 (22631 = enablement package).

## W9 - image build run (2026-10-10 22:08-)
S10eHarness image (ISO fix W7): VHDX created 22:08:58; DISM apply index 3 done by 22:12:23 (~3.5 min); Add-Driver
Exynos9820Ufs ok; Lite profile (inbox apps removed, telemetry/ads policies, BypassNRO=1); unattend (local admin
S10e, empty password, autologon, SkipMachineOOBE/SkipUserOOBE, en-US, UTC, GALAXY-S10E, 275 DPI); bcdboot + BCD
repoint done 22:14:35; export of the 110 GB volume started 22:14:39.
BCD (s10e-artifacts\win-w5\out\esp\EFI\Microsoft\Boot\BCD) as built: testsigning Yes, numproc 4, hypervisorlaunchtype
Off, timeout 0, devices "unknown" on this PC (expected: qualified GPT GUIDs of a disk not attached). Changed by hand
for the first boot: `bcdedit /store <BCD> /set {default} numproc 1` (avoid PSCI CPU_ON, W5/W8).
Phone: in TWRP per the user but NOT visible over USB after the PC reboot (adb devices empty, no Samsung PnP device).

## W10 - PartitionMap.Beyond0Lte WindowsBytes was 5 LBAs too large (2026-10-10)
`S10eHarness check` (phone back on USB after a replug): userdata 119 126 622 208 B vs geometry 119 126 642 688 B ->
MISMATCH. Re-read three ways on the phone: raw GPT entry 30 "userdata" LBA 2 151 168..31 234 815 (29 083 648 LBAs);
sysfs sda31 start 17209344 / size 232669184 (512 B units); blockdev --getsize64 119126622208. All agree:
119 126 622 208 B = exactly 113 608 MiB. The old comment ("622 208 is the filesystem") was wrong - blockdev reports
the partition. Other values re-checked: disk 249888768 x 512 = 127 943 049 216 OK; offset 2151168 x 4096 =
8 811 184 128 OK; cache 629 145 600 OK; recovery 67 633 152 OK; GPT header size field read as well.
Impact: the first image had NTFS 5 LBAs larger than USERDATA (backup boot sector past the end); TransferService would
have refused it (image > partition). Build killed at 53.5/113.6 GiB exported; VHDX detached. Fixed PartitionMap +
tests (194 passed). Rebuild next.

## W11 - identity backup done (2026-10-10 22:19)
S10eHarness gained `backup` (BackupService over PartitionMap.IdentityBackup, as RunBackupAsync). Run against the phone
(TWRP): efs 20480 KiB (20107ff20016…), cpefs 6144 KiB (84e28cbb3bb1…), param 8192 KiB, up_param 8192 KiB, steady
4096 KiB, keystorage 512 KiB, persistent 512 KiB - each hashed on phone and PC and matched. Folder + manifest:
C:\Users\ilya\code\s10e-artifacts\backup-RF8M21HYDSZ. Image rebuild (fixed geometry) started 22:18.
User consent for the transfer (2026-10-10 ~22:20): "Yes, write Windows" - USERDATA overwrite + CACHE->FAT32 ESP, start automatically once the image is built.

## W12 - image built (fixed geometry), transfer started (2026-10-10)
windows.img 119 126 622 208 B (= USERDATA) sha256 fbf9672db29b8ff6e81b59b0a978d5fcce82851c30d29129058229bbe7b96ad1.
winload.efi sha256 7106b82fb7e0f25072e8b9d9c3c7cfd5ea1cffb006bcd26e3f07527737ab0ff5
ntoskrnl.exe sha256 db84703895bbc9f10771dabc117f7b3d3a7f8bc66d0d3e1918f01983e77e7aad
ESP tree s10e-artifacts\win-w5\out\esp (EFI\Boot\bootaa64.efi 1 799 024 B present); BCD numproc set to 1 again after the
rebuild (testsigning Yes, hypervisorlaunchtype Off). VHDX detached. Transfer (S10eHarness transfer, verify on) next.

## W13 - Windows written to the phone (2026-10-10 22:26-22:32)
S10eHarness transfer: "Writing 12024 MiB of 113608 MiB to userdata: NTFS free space is skipped", every 128 MiB chunk
read back + sha256-matched, ~41 MB/s, done 22:31:54. cache formatted FAT32 (ESP), EFI tree copied, "Windows and boot
files written and verified on the phone", EXIT 0. Post-check from TWRP: cache mounts vfat, EFI/Boot/bootaa64.efi +
EFI/Microsoft/Boot/{BCD,BCD.LOG*,...} present; userdata LBA0 = NTFS boot sector, bytes/sector 0x1000; BOOT still
boot #26 (sha 200bd7e1…). Android user data is gone from here on. Next: adb reboot -> UEFI BDS -> bootaa64.efi.

## W14 - first Windows boot attempt OBSERVED: synchronous exception in UEFI context (2026-10-10 ~22:35)
Photo notes/img-win1-sync-exception.jpg (user). "Synchronous Exception at 0x00000000B8C02158"; ELR 0xB8C02158,
SP 0xBAAFF760, SPSR 0xA0000205 (EL1h), ESR 0x96000006 (EC 0x25 data abort same EL, DFSC 6 = translation fault L2),
FAR 0x0 -> a NULL-pointer load. The console wrapped: the dump's first lines (X registers and the "PC ... [n] <image>"
line naming the module) were overwritten by the stack dump, so the faulting image is unknown from this photo.
Stack holds pointers 0xB8B2E260/0xB8B2F630/0xB9C98718/0xB97FF698/0xB9E63658 and ASCII fragments
(0x66676D69 = "imgf", "60)/RFS(" , "0x0 0x0y") - consistent with bootmgfw (1.8 MB, mapped around 0xB8B0xxxx..)
or a firmware driver servicing it; UNVERIFIED. No SMBIOS in either package (9810 booted without it).
Next read: a video of the boot to catch the "Loading driver at ... bootmgfw" line and the PC/image line before it wraps.

## W15 - FB console exception mode; boot #27 built (2026-10-10)
Beyond0LteFbSerialPortLib: a SerialPortWrite containing "Exception at 0x" (CpuExceptionHandlerLib DumpCpuContext
banner, AArch64/CpuExceptionHandlerLib.c:307) clears the panel, restarts at row 0 and stops wrapping (drop output once
the last row is used), so the "PC 0x... (base+off) [ 0] <image>" line (:326) and the X registers stay on screen.
Mode flags live in pixels 0/1 of scanline 2279 (text area ends at row 71*32 = 2272; STATIC_ASSERT) - not in the
state record past the FB, which survives resets; sboot repaints the scanout buffer each boot.
Build: build-uefi.ps1 -Platform Beyond0LtePkg exit 0, FVMAIN_COMPACT 2808 B free. pack-uefi-beyond0lte.ps1 (-Ref
ref-recovery.img) exit 0; build-bootimg-v2.py --dtb stock dt.dtb -> boot #27 s10e-artifacts\boot27-v2.img, 7 587 840 B,
sha256 e256d8327c29633cdc38e6297dc3c1344039c36de45cbb1cb5baadd3d936881e. vs boot #26: header bytes 0..575 identical
(576.. = id/sha), shim 208 B identical. Nothing else changed vs boot #26 (bank1/bank2 RAM, UFS).
W15 flash: boot #27 pushed (sha e256d832 on device), sda14 before = boot #26 (200bd7e1), dd conv=fsync, readback
e256d8327c29…881e. Review of f79271f: frozen mode exits only via a new boot (sboot repaint); banner scan per write is
O(n); last-row overflow sets FULL via FbNewLine and the loop breaks - no wrap path left in frozen mode. adb reboot next.

## W16 - boot #27 OBSERVED: the fault is in WINLOAD's heap code; boot #28 = bank0-only A/B (2026-10-10)
Photo notes/img-win2-bootmgr-exception.jpg. Exception mode worked: "Synchronous Exception at 0xB8BFF158", backtrace
PC B8BFF158 / B8B6B268 / B8B49494 (no PDB = image not in the UEFI debug table, i.e. loaded by bootmgr itself) ->
BdsDxe+0x60C0/+0x5D9C -> DxeCore. X19 = 0, LR = 0xB8B6B268, X10 = 0xB8D05000, SPSR 0xA0000205 (EL1h).
Identification (scripts in %TEMP%: basefind*.py): only base 0xB8B52000 for winload.efi (sha 7106b82f…) makes
LR-4 a BL and the faulting PC a load through a zero register: RVA 0x19268 = right after `bl BlFileLoad` in
OslpPreloadDrtmBlobsFromWim+0x148; fault RVA 0xAD158 `ldr x8,[x19]` (public symbol CompositeFsInitialize+0x190,
really winload's inlined heap allocator: it calls MmHapHeapAllocatorExtend / MmHapAddToFreeList). Symbols:
winload_prod.pdb from msdl (key E3B96F07D58E7B234BC5B324B82F6ED12), llvm-pdbutil dump --publics.
Control flow: ad158 is reached only with x19 != 0 (cbnz x19 at ad120; or ad13c..ad154 where `str x0,[x19]` at
ad150 succeeded) -> x19 was zeroed ACROSS `bl MmHapAddToFreeList` (ad154), a plain list function with no
sysreg/tlbi/hvc. Boot #26 crashed at B8C02158 = same RVA, base +0x3000 -> deterministic.
=> Something outside EL1 code corrupts a GPR during that call. Hypothesis (UNVERIFIED): a stage-2 abort taken
to Samsung uH on a heap page and "emulated" with a wrong register write - heap pages come from the new W2
bank1/bank2 RAM. The 9810's winload patches act later (MMU switch), and it never published bank2.
A/B: BEYOND0LTE_PUBLISH_BANK12 0 (bank0-only, as boot #25) -> boot #28 s10e-artifacts\boot28-v2.img sha256
507fd916a4a4e0a4b1e73439c89ea1e718d6ebc0b64b1abbc87f208077115dc2 (build exit 0, FVMAIN_COMPACT 2816 B free, shim
same as #27). If winload passes this point on #28 -> W2 ranges are not plain RAM for EL1; bisect bank1 vs bank2.
W16 flash: boot #28 on sda14, readback sha 507fd916…5dc2 (was #27 e256d832). adb reboot.

## W17 - boot #28 OBSERVED: same crash with bank0 only; boot #29 = memory-map dump (2026-10-10)
Photo (user): identical fault, ELR 0xB8C02158 (winload base 0xB8B55000 -> same RVA 0xAD158), X19 = 0, X0/X21/X22 =
0xB8D35028. => NOT the W2 bank1/bank2 RAM. Closer read of the callee chain: MmHapAddToFreeList (RVA 0xEBC88) ->
RVA 0xEBD90 saves x19/x20 with stp at its SP and restores them before ret; the dump shows X19 = 0 and X20 =
0xC000000D (an NTSTATUS, INVALID_PARAMETER) - i.e. that STACK SLOT was overwritten while RVA 0xEBD90 ran. Hypothesis
(UNVERIFIED): winload's heap/page allocator is handed memory that overlaps the live firmware stack (SP 0xBAAFF760,
top of bank0 0xBAB00000) because our UEFI memory map does not mark it allocated.
Boot #29: Beyond0LteAcpiPlatformDxe gains BEYOND0LTE_MEMMAP_DUMP (ReadyToBoot event): prints "MMAP n=.. sp=..", then
every descriptor "type base+pages" two per line, then holds 20 s. Build exit 0 (FVMAIN_COMPACT 2352 B free), still
bank0-only. boot29-v2.img sha256 ef9d5840709cfbe6310b24b86eebc7e762749482de090f511b9c03a6b788b5d7.

## W18 - boot #29 OBSERVED (memory map) + root-cause candidate: UFS PRDT overflow on >2 MB reads (2026-10-10)
Photo notes/img-boot29-mmap.jpg: MMAP n=37, sp=0xBAAFF810; last entry `4 BA2CB000+835` (BootServicesData up to
0xBAB00000) contains the stack -> the stack IS allocated; the W17 overlap hypothesis is wrong.
Side findings: (1) `7 80000000+10000` = 0x80000000..0x90000000 reported CONVENTIONAL with no hole: the monitor
reservations (sec_debug_magic 0x80000000, kaslr 0x80100000, el2_code 0x87000000) are carved only from the MMU table,
not from the UEFI memory map (MemoryInitPeiLib builds one resource HOB for the whole PcdSystemMemory range) -> Windows
is told uH's code is free RAM. Follow-up task. (2) Nothing above 0xBAB00000 is in the map even with BANK12=1: W2 only
mapped bank1/bank2 in the page tables, it never published them as RAM (no RamManagerDxe) - W3's "DXE used bank2"
reading was wrong. Follow-up task.
Root-cause candidate for the winload fault: Beyond0LteUfsHcDxe's doorbell hook splits every PRDT into <= 4 KB entries
at a 128 B stride, capped at UTP_PRDT_MAX_SPLIT 511 (~2 MB; DW7 PrdtL is a 16-bit byte count). For a larger transfer
it only logs "PRDT split exceeds" and leaves EDK2's packed 16 B entries while DW7 declares 128 B entries -> the
controller reads garbage descriptors and DMAs disk data anywhere. winload's OslpPreloadDrtmBlobsFromWim -> BlFileLoad
is the first large single read. ScsiDisk issues READ(10) up to 0xFFFF blocks; UfsPassThru has no BAD_BUFFER_SIZE path.
Fix (boot #30): Beyond0LteUfsHcDxe wraps the UFS EXT_SCSI_PASS_THRU.PassThru (protocol notify) and issues READ/WRITE
10/16 larger than 1 MiB as consecutive <= 1 MiB commands (blocking requests; short/failed chunk reports the bytes
moved). STATIC_ASSERT 1 MiB/4 KB + 1 <= 511. Build exit 0, FVMAIN_COMPACT 1624 B free.
Boot #30 = W18 split + reserved HOBs + dump off (bank0-only still): s10e-artifacts\boot30-v2.img sha256 e67f4e30d9c80b76022d617aaeb03702e7d5852684db50c1da38979ec9b2f06e, FVMAIN_COMPACT 2064 B free; flashed, readback verified.

## W19 - boot #30 OBSERVED (same fault) + boot log to the ESP; boot #31 (2026-10-10)
User: boot #30 (1 MiB SCSI split + reserved HOBs) shows the SAME winload fault -> the PRDT-overflow theory is not
(or not alone) the cause. The split stays (it is a real bug for > 2 MB transfers).
User asked for (1) boot text in a file read from TWRP and (2) automatic reboot to recovery after a failure.
(1) done in boot #31:
- Beyond0LteFbSerialPortLib also appends every byte to a 1 MiB RAM ring at 0x8FF00000 (Beyond0LteBootLog.h; a
  FIRMWARE-CHOSEN address in bank0 RAM below the FD, clear of the monitor reservations), reset when the MMU is off
  (SEC) and the last writer had it on (= new boot). PlatformMemoryMapLib reserves it (EfiReservedMemoryType HOB).
- New Beyond0LteBootLogDxe (FILE_GUID ...5548, depex CpuArch): at ReadyToBoot writes \UEFILOG.TXT on the FAT volume
  that holds \EFI\Microsoft\Boot\BCD (CACHE); registers its own SYNC + SERROR handler via CpuArch
  RegisterInterruptHandler: banner (keeps the panel exception mode), ESR/FAR/SP/SPSR, PC/LR + FP backtrace with
  image names (debug image info table), X0-X30, 16 code words around ELR, the loaded-image list, stack; then
  flushes UEFILOG.TXT and parks (BEYOND0LTE_AUTO_RECOVERY 0 until the reboot-reason sequence is verified).
- RISK: first UEFI WRITES to UFS ever on the 9820. Safety net: hashes of efs, sec_efs, cpefs, param, up_param,
  steady, keystorage, persistent, misc, uh, uhcfg, dtb, dtbo, recovery, boot, vbmeta, GPT (LBA 0-5) and the first
  16 MiB of userdata taken before (notes/s10e-hashes-before-boot31.txt) - compare after the boot.
  EFS hash already differed from the 22:19 backup: only the ext4 superblock (0x42C/0x430/0x434/0x578: mount and
  write time, mount count 0x2C->0x31) - TWRP mounts /efs at every start; benign.
(2) in progress: a subagent extracts the 9820 reboot-reason / SWRESET / watchdog sequence from Samsung's kernel
  (and sboot) binaries; no constant is used before that.
Build exit 0 (FVMAIN_COMPACT auto-sized, it lives in a 7 MB FD region - the "bytes free" figure is only padding).
boot31-v2.img sha256 3bf6bf1b9ba58ec58c74b8ff2c1704b0c3464e4391bcaa78489953d5703e3f2a.

## W20 - boot #31 OBSERVED via \UEFILOG.TXT: the crashing code is BOOTMGR, and it is CORRUPTED in RAM (2026-10-10)
Log pulled from TWRP (mount -t vfat by-name/cache): notes/uefilog-boot31.txt (64 KB). UEFI writes work. Hash check
vs notes/s10e-hashes-before-boot31.txt: only boot (flashed), efs (ext4 superblock mount time/count - TWRP) and param
(sboot's own per-boot history records "261010 19:32:30 ... RP ... drawLOGO" at 0x31cxxx) changed - none by UEFI.
Dump: ELR 0xB8BF3158, the image containing it is "(no pdb)" at 0xB8B1A000 size 0x1FA000 = bootaa64.efi's
SizeOfImage -> the fault is in WINDOWS BOOT MANAGER, not winload (W16's winload identification was a
coincidental fit: same base-search freedom, wrong image). RVA 0xD9158.
The 16 code words captured around ELR (D374CE80 97FFF46F 14000004 F9400A68 D374CD00 97FFEEB9 7100001F 1A95B015
>F9400273 EB16027F ...) are NOT the file's bytes at RVA 0xD9158 (F94017E8 7100011F ... AD004530); they are the file's
bytes at RVA 0xDC170 (0x3018 further). => bootmgr's image in RAM is not the file: the 1.7 MB read that loaded it
("UfsHc: split op 0x28 lba 0x1E457C 432 x 4096 B") delivered data at the wrong offsets. Every "impossible" register
value since W14 follows from executing misplaced code.
Also: the ReadyToBoot flush said Not Found - the first ReadyToBoot precedes BDS connecting the disk.
Boot #32: Beyond0LteBootLogDxe connects all controllers at ReadyToBoot, then READ TEST (BEYOND0LTE_READ_TEST 1):
reads \EFI\Boot\bootaa64.efi in one File->Read and again in 4 KB reads, logs CRC32 per 64 KB of each ("RT one|4k
<off> <crc>") for comparison with the PC copy. boot32-v2.img sha256 385f96174baf63b01ecd4ad765a0b3bb86c163edccf57d4ceedd8e1ecd58c80a.

## W21 - boot #32 read test: the disk path is CLEAN; boot #33 = mapping probe + image dump + auto-recovery (2026-10-10)
notes/uefilog-boot32.txt: bootaa64.efi (0x1B7370 B) read in one File->Read and in 4 KB reads - all 28 CRC32/64 KB
match zlib.crc32 of the PC copy in both modes. ReadyToBoot flush now Success (connect-all fix).
Crash identical (ELR 0xB8BE5158, image base 0xB8B0C000 -> RVA 0xD9158). bootmgfw.pdb from msdl (key
EA3E9F5441280B69F8A01013E9CBDBCD2): RVA 0xD9158 = MmPapAllocateRegionFromMdl+0x430, but the bytes executing there are
MmpFreeApplicationAllocations+0xb0 (file RVA 0xDC170); backtrace RVAs (LR 0x45268 = FvebpEncryptAndWrite+0x220 after
a csel, frame EfiEntry+4) are not call sites, with or without a 0x3018 shift. The bytes match a RAW-FILE-LAYOUT copy
of bootaa64.efi starting 0x2418 below the image base (page offset 0xBE8 - pool-buffer-like). SP 0xBAAFF760 is the UEFI
stack only 0xB0 below BDS's ReadyToBoot SP: the fault is very early in bootmgr (EfiEntry -> ...).
=> Not a disk problem. Either the image's pages are mapped to the wrong PA, or bootmgr's code region physically
overlaps a file-layout buffer (allocator/memory-map fault). UNVERIFIED which.
Reboot constants verified from Samsung's kernel + sboot (notes/s10e-reboot-wdt-constants.md, subagent) and added to
Exynos9820.h: INFORM3 0x80C = 0x12345670|4 (recovery), RSTSTAT 0x404, SWRESET 0x3A00 = 2 (NOT the 9810's 0x400=1).
Boot #33 (BootLogDxe): READ_TEST 0; the exception handler logs TTBR0/TCR/SCTLR and AT S1E1R -> PAR_EL1 for the image
base and ELR, writes the image region (base-64K .. +2M+128K) to \IMGDUMP.BIN on the ESP, flushes UEFILOG.TXT, then
BEYOND0LTE_AUTO_RECOVERY 1: INFORM3 = 0x12345674, RSTSTAT = 0, SWRESET = 2 -> sboot should boot TWRP by itself.
Hangs (no exception) are not covered yet - WDT next. boot33-v2.img sha256 a5f09b18e3381d5d564e33509879da05d8f3355623c2e11b9e429dfb15171386.

## W22 - ROOT CAUSE: direct UFS DMA into a non-4K-aligned buffer is corrupt; boot #34 bounces everything (2026-10-10)
Boot #33: auto-recovery WORKED - the phone was back in TWRP ~85 s after `adb reboot` with no key presses
(INFORM3 0x12345674 + SWRESET 0x3A00=2). Log notes/uefilog-boot33.txt: TTBR0 0xBAAEF000 (UEFI's own tables), AT S1E1R
of the image base 0xB8B0B000 and ELR -> PAR = same PA: identity, so the wrong bytes are physically in RAM.
IMGDUMP.BIN (image region) vs bootaa64.efi laid out by its section table: 68 of 506 pages identical; the rest are the
correct image displaced: page RVA 0x10000 holds image bytes from +0x18, 0x23000 from +0x18, 0x80000 from +0x1018,
0xD9000 from +0x3018 - i.e. every byte 0x18 low plus a whole 4 KB page lost every so often.
Mechanism: LoadImage reads the 1.7 MB file into a POOL buffer (page offset 0x18 = pool header) in one read; > 512 KB
did not fit the bounce, so Map() returned the caller's address for DIRECT DMA and the PRDT was split at page
boundaries with a 0xFE8-byte first entry. The controller evidently does not honour a non-page-aligned entry the way
the PRDT describes (data lands page-aligned, pages dropped). The W20 read test passed only because its buffer came
from AllocatePages (aligned).
Fix (boot #34): UFS_BOUNCE_BYTES 512 KB -> 1 MiB, STATIC_ASSERT split (1 MiB) <= bounce, so every READ/WRITE goes
through the page-aligned WC bounce. RISK carried to Windows: the Windows UFS driver may hit the same rule with
Storport SGLs that start mid-page - check its PRDT builder before relying on it.
boot34-v2.img sha256 290f5d5182daa647b8d393547a9a0ae2aa98fdabfd53ad7167615919eb920bdf.

## W23 - boot #34: WINDOWS KERNEL RUNS -> INACCESSIBLE_BOOT_DEVICE; autonomy for Windows runs (2026-10-10)
Photo notes/img-win-bsod-inaccessible.jpg (user): Windows 11 BSOD, stop code INACCESSIBLE_BOOT_DEVICE. So the 1 MiB
bounce fix (W22) got bootmgr -> winload -> ExitBootServices -> ntoskrnl through Samsung uH with NO winload/ntoskrnl patching
(numproc 1). The kernel does not get its boot disk from our Windows UFS driver.
Offline check of the image's SYSTEM hive: Services\Exynos9820Ufs Start=0 Type=1 Group 'SCSI Miniport', ImagePath in
DriverStore\FileRepository\exynos9820ufs.inf_arm64_4efdc4525d11d997, DriverDatabase\DeviceIds\ACPI\EXYN9820 -> oem0.inf.
The Windows driver already DMAs only through its page-aligned bounce, so the W22 rule does not explain it by itself.
Autonomy pieces (firmware boot #35):
- Beyond0LteResetSystemLib: BEYOND0LTE_RESET_HOLD 0, BEYOND0LTE_PMU_RESET 1 with the VERIFIED 9820 sequence: INFORM3 =
  0x12345670 (normal) then SWRESET PMU+0x3A00 = 2 (runtime-mapped, EfiConvertPointer); PS_HOLD 0x30C bit 8 for power off;
  PSCI SYSTEM_RESET/RESET2 calls removed (the stock kernel never uses them).
- Reserved 1 MiB at 0x8FE00000 (BEYOND0LTE_WINPRAM_BASE) for the Windows driver's PRAM ring. RAM survives resets:
  the UEFI boot-log BootCount went 1 -> 2 -> 3 across boots #31-#33.
- BootLogDxe boot pair (BEYOND0LTE_WINDOWS_PAIR): at ReadyToBoot creates \WINRUN.FLG; if the flag already exists
  (previous boot ran Windows) it deletes it, dumps 64 KB of 0x8FE00000 to \WINPRAM.BIN, flushes UEFILOG.TXT and reboots
  to recovery. Every Windows attempt therefore ends in TWRP with evidence on the ESP.
Windows driver (ufs-w23, -DmaWindow, signed 6ccf49ce...): UFS_PRAM_ENABLE 1 at 0x8FE00000 (low-WDT page stays
unmapped, UFS_LOW_WDT_PRAM_ENABLE 0), UFS_FIRMWARE_WDT_BRIDGE 0 + UFS_CONTINUOUS_HARDWARE_WATCHDOG 0 (they write the
9810's PMU WDT disable/mask 0x408/0x40C, which the 9820 lacks); handoff marker follows the ring base. Built with the
exact recipe (EXACT_BUILD_OK, 47 616 B unsigned). Deployed in place with TWRP ntfs-3g (next).
boot35-v2.img sha256 686421639720fac5b62074fb03e543bd28cf902acc9882c769efad3b5f6ef23e.

## W24 - boot #35 OBSERVED + handoff watchdog (boot #36) (2026-10-10)
User: boot #35 (new driver ufs-w23 deployed in place via TWRP ntfs-3g; old 155dd70c -> new 6ccf49ce) shows the same
INACCESSIBLE_BOOT_DEVICE and the phone did NOT reset after 'We'll restart for you' (poller saw no recovery in 540 s):
Windows' restart did not reach our ResetSystem (PSCI SYSTEM_RESET on a PSCI 0.1 monitor is the likely sink - UNVERIFIED).
The user forced TWRP; boot #35's PRAM is likely overwritten by TWRP's kernel, so the stale \WINRUN.FLG was deleted.
User: testsigning - confirmed earlier, BCD {default} testsigning Yes. User asked for Windows-side crashes to come back
to recovery with logs/dumps and for a 90 s watchdog.
Boot #36: BootLogDxe arms watchdog cl0 at ExitBootServices (BEYOND0LTE_HANDOFF_WATCHDOG): INFORM3 normal, WTCON=0,
WTCLRINT=1, PMU NONCPU_INT_EN bit 2 via the atomic-set alias (write 2 to PMU+0xD244), WTDAT=WTCNT=0xFE27, WTCON=0xFF39
= 82 s - the hardware maximum (16-bit count, prescaler <= 256, DIV128 of 26 MHz -> ~82.6 s; 90 s is not reachable).
Any hang / failed restart -> WDT reset -> UEFI sees WINRUN.FLG -> WINPRAM.BIN + UEFILOG.TXT -> TWRP. The Windows UFS driver
already has a bugcheck callback that writes '=UFSBUGCHECK BC=...' into its PRAM ring (now at 0x8FE00000).
boot36-v2.img sha256 55f9825698c6d162138dab74dd1a28258552f6bf6b61f91d50866929a2ab6171.

## W25 - unattended: dead man's switch + shim cache flush (boot #37) (2026-10-10)
Boot #36: the 82 s handoff watchdog DID reset the phone out of the Windows BSOD, but the next boot stuck on the logo
with the shim's white bands. User: 'hang in the shim after 1 reboot was always a thing' - every boot that follows our own
firmware (PMIC keys, watchdog) hangs; boots that follow Linux (TWRP/Android) work.
Facts: shim entry SCTLR_EL1 0x30C5083A (MMU/caches off, B-notes); PeilessSec invalidates D-cache only over the UEFI
region and enables the I-cache; nothing cleans/invalidates the FD region or the rest of RAM. Linux flushes caches before
every reboot (sec_reboot: flush_cache_all, llc_flush); our resets do not. Hypothesis (UNVERIFIED): stale cache lines from
the previous boot shadow the fresh FD copy once caches come on.
Boot #37: (1) shim entry: INFORM3 = 0x12345674 (recovery) + watchdog cl0 82 s, then clean+invalidate all data/unified
levels by set/way (CLIDR/CCSIDR walk, DC CISW) + IC IALLU before the copy. (2) ArmPlatformInitialize (SEC) re-arms the
same dead man's switch (BEYOND0LTE_EARLY_WATCHDOG). (3) BootLogDxe at ReadyToBoot: reason back to normal + re-arm,
again at EBS. => any hang anywhere ends in TWRP; a Windows hang comes back through UEFI's boot pair when the next
boot survives. RAM at 0x8FE00000-0x90000000 survived TWRP sessions (BootCount 1->2->3), so WINPRAM survives a detour.
boot37-v2.img sha256 781f1174f1072a1c314aa043f02c15f6aba2b22fe9ed651b68c0a4be691db128.
