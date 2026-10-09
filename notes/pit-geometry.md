# The S10e's real partition geometry — from Samsung's own PIT, cross-checked against the phone

Source artefacts (all read here, nothing fetched):

* `C:\Users\ilya\code\Dumps\BEYOND0LTE_EUR_OPEN.pit` — 6 224 bytes, 41 entries.
* `C:\Users\ilya\code\Dumps\pit_data_extracted.txt` — PIT Magic v1.3.10's dump. **Its
  field decode is wrong** (it prints `Block Size: 109 / 129` and leaves `Block Size`
  blank for BOOTLOADER); the binary was parsed directly instead.
* `C:\Users\ilya\code\Dumps\twrp_device_tree\recovery.fstab` — a beyond0lte TWRP
  device tree; it addresses everything through
  `/dev/block/platform/13d60000.ufs/by-name/<name>`, i.e. the same UFS platform
  node `Exynos9820.h:268` VERIFIEDs (`0x13D60000`).
* The phone's own sizes from `/proc/partitions` (`notes/s10e-partitions.md`).

## PIT layout decode (validated, not assumed)

Header `<II>`: magic `0x12349876`, entry count 41. Entries start at **0x1C**, stride
**0x84** (132 B) — confirmed by PIT Magic's own "Entry Memory Address" values
(0x1C, 0xA0, 0x124, 0x1A8 → all 0x84 apart). Fields:

| off | field |
|---|---|
| 0x00 | binary type |
| 0x04 | device type |
| 0x08 | **volume/index id** (== the `sda<N>` index: EFS=3, BOOT=14, RECOVERY=15, USERDATA=31 — matches the phone's by-name links exactly) |
| 0x0C / 0x10 | attribute / update attribute |
| 0x14 | **start, in 4096-byte units** |
| 0x18 | **length, in 4096-byte units** (0 = "to the end") |
| 0x1C / 0x20 | file offset (obsolete) / file size |
| 0x24 | partition name [32] |
| 0x44 | flash file name [32] |

The unit is 4096, not 512 — proved by contiguity: every entry's `start` equals the
previous entry's `start + length` (KEYREFUGE 71296+4096 = KEYDATA 75392; … BOTA3
83600+368 = SYSTEM 83968; CACHE 1983488+153600 = OMR 2137088; CP_DEBUG 2149888+1280
= USERDATA 2151168). It is also proved against an independent source: `n_sec × 4096`
reproduces the phone's `/proc/partitions` sizes **exactly for 30 of 31 partitions**
(the exception is CPEFS, which lives on `sdd1`, a different container; USERDATA is
0-length in the PIT). BOOT 14080×4096 = 57 671 680 = the phone's 56 320 KiB; RECOVERY
16512×4096 = 67 633 152 = 66 048 KiB; CACHE 153600×4096 = 629 145 600 = 614 400 KiB.

## The S10e geometry the installer needs (bytes)

| constant | 9810 (`PartitionMap.cs:67-70`) | **S10e** |
|---|---|---|
| `DiskBytes` | 63 963 136 000 | **127 943 049 216** (= 124 944 384 KiB, phone) |
| `WindowsOffset` | 6 951 534 592 | **8 811 184 128** (= PIT 2 151 168 × 4096) |
| `WindowsBytes` | 57 004 785 664 | **119 126 622 208** (= phone's userdata 116 334 592 KiB) |
| `CacheBytes` | 629 145 600 | **629 145 600 — identical** |
| `BootPartitionBytes` (`Validation.cs:10`) | 57 671 680 | **57 671 680 — identical** |
| `RecoveryPartitionBytes` (`Validation.cs:9`) | 68 148 992 | **67 633 152** |

Self-consistency: `WindowsOffset + WindowsBytes = 127 937 806 336`, and the disk is
127 943 049 216 → a **5 242 880-byte (1280 × 4096) tail**, which is the GPT backup
plus alignment — the same shape the 9810 had (its tail was 1659 × 512).

## What this closes

* `PartitionMap.cs:68 WindowsOffset` — the one number that was unknowable before.
* The UFS driver's fence/protected LBAs (`Exynos9820Ufs.h:980-998`), in the driver's
  own 4096-byte LBA units: Windows volume = **LBA 2 151 168 .. 31 234 810**; the
  protected Android run = **VENDOR LBA 1 543 168 .. USERDATA end 31 234 810**;
  ESP CACHE = LBA 1 983 488 .. 2 137 087.
* The stock restore path: the tars in `Dumps` are the vendor payload set
  (`AP_…` 6 339 594 430 B, `BL_…`, `CP_…`, `CSC_…`, `HOME_CSC_…`) plus the PIT that
  tells Odin where each goes — that is the return-to-stock mechanism, same as the
  S9+ flow used.

## What it does NOT close

* **The GPT GUIDs.** The PIT carries index ids, not GUIDs. `PartitionMap.cs:58-61`
  says Samsung derives them from the partition name ("ANDROID MMC DISK",
  "ANDROID USERDATA" …), and the S10e uses the same names, so the four GUIDs are
  very likely identical — but that is inference, and it must be confirmed against
  the phone's own GPT (16 bytes at the partition-entry level), which is unread
  from an unprivileged shell.
* **Whether the phone's current GPT still equals stock.** The PIT is Samsung's
  intended layout; the sizes matching the phone exactly is strong evidence it was
  never re-laid-out, but the *starts* were not independently confirmed. The
  installer's `partition` stage should therefore verify offsets, not just sizes.

## Second browser-model question — PIT vs live GPT, and the GUID scheme

Asked in the same chat; its answer (AI, with sources — **not opened from here**):

* **No documentation** says Samsung derives partition GUIDs from names. The GPT
  standard requires the per-partition **unique GUID** to be unique per partition;
  a name-derived GUID would be a Samsung-specific, non-standard choice. What *is*
  standard and comparable across models is the **type GUID** (e.g. Basic Data
  `EBD0A0A2-…`), which lives at entry offset 0 while the unique GUID is at offset
  16. So `PartitionMap.cs:58-61`'s claim ("Samsung derives GPT identifiers from
  names … the same on every star2lte") is an **observation on the 9810**, not a
  rule that may be assumed for the S10e.
* **The PIT is not authoritative for what the kernel sees.** Replicant found that
  changing the PIT is usually not enough to repartition — the GPT must be changed
  too — so the **live GPT can differ from the PIT** (a custom PIT flashed, the
  GPT edited directly, or the package PIT differing from the device's own).
* Checks it proposed: `sgdisk -i <n> /dev/block/sda` on both phones and compare
  the "Partition unique GUID" lines; `heimdall print-pit --no-reboot` to pull the
  **device's own** PIT rather than the package's.
* One unverified point it raised: on many Samsung devices the PIT's userdata entry
  is sized to fill the rest of the storage, so the live GPT's USERDATA end-LBA can
  differ between storage variants even with an identical PIT.

Consequence for this repo: the PIT numbers are safe as **stock** geometry (and the
sizes independently match the phone), but two things must come from the device
itself before any write: the live GPT's starts, and the unique GUIDs the BCD will
be pointed at.

## Sources

* `C:\Users\ilya\code\Dumps\BEYOND0LTE_EUR_OPEN.pit` — parsed directly
  (`%TEMP%\opencode\pit_geom.py`, output `pit-geom.txt`).
* `C:\Users\ilya\code\Dumps\pit_data_extracted.txt` — PIT Magic's addresses only.
* `C:\Users\ilya\code\Dumps\twrp_device_tree\recovery.fstab` — by-name addressing.
* `notes/s10e-partitions.md` — the phone's sizes and by-name links.
* `installer/src/S9Woa.Installer.Core/Deploy/PartitionMap.cs`,
  `Toolset/Validation.cs`, `drivers/Exynos9820Ufs/Exynos9820Ufs.h:929-1002`.
* `claude.ai/chat/53802365-…` — the second answer above; it cited the UEFI GPT
  specification, Replicant's repartition notes, and heimdall docs
  (none opened from here).
