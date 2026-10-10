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
