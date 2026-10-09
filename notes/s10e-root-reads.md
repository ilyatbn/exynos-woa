# S10e read as root in TWRP — the live GPT, the DT `reg` values, and the framebuffer base

Run 2026-10-09 with the phone in TWRP (`RF8M21HYDSZ   recovery`, `uid=0(root)`).
Everything below is a **read**; nothing was written to any block device. The only
writes were to `/tmp` (RAM) for the dd capture.

## 1. Method

TWRP's `/system/bin/sgdisk` exists but will not exec (Android binary, its
`/system/bin/linker64` is absent in TWRP). So the GPT was captured directly:

```
mknod /tmp/sda b 8 0                 # /dev/block has no whole-disk node in TWRP
blockdev --getsize64 /tmp/sda        -> 127943049216
dd if=/tmp/sda of=/tmp/sda-gpt.img  bs=4096 count=34            # head
dd if=/tmp/sda of=/tmp/sda-tail.img bs=4096 count=34 skip=31236062  # backup GPT
```

`blockdev --getsize64` = **127 943 049 216 bytes** — the exact value the PIT
implied (`notes/pit-geometry.md`), confirmed on the device.

## 2. The live GPT (parsed from the capture)

Header at LBA 1 (sector size **4096**), entries at LBA 2, 128 entries × 128 B.
Samsung's entries are **non-standard**: no 4-byte flags field — type GUID at
entry+0, unique GUID at +16, **start LBA at +32, end LBA at +40** (inclusive),
UTF-16 name at +80.

* Disk signature (header+56, 16 ASCII) = **`ANDROID MMC DISK`** →
  `PartitionMap.DiskGuid {52444e41-494f-2044-4d4d-43204449534b}` is **correct for
  the S10e**.
* Unique GUIDs are ASCII **`ANDROID ` + the lower-case partition name**, e.g.
  `ANDROID userdata` → `{52444e41-494f-2044-7573-657264617461}`. The repo's
  `UserdataGuid/CacheGuid/SystemGuid` are the **upper-case** forms
  (`ANDROID USERDATA`) — correct for star2lte, **wrong for beyond0lte**.
  Independently confirmed by the kernel's own cmdline:
  `root=PARTUUID=52444E41-494F-2044-7379-7374656D0000` = "ANDROID system".
* Type GUID for all 31 entries = `{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}` (Basic
  Data).

Partition table (LBA units = 4096 B; bytes inclusive):

| name | start LBA | end LBA | bytes |
|---|---|---|---|
| bota0 | 1024 | 2175 | 4 194 304..8 912 895 |
| bota1 | 2176 | 4095 | 8 912 896..16 777 215 |
| efs | 4096 | 9215 | 16 777 216..37 748 735 |
| param | 9216 | 11263 | 37 748 736..46 137 343 |
| up_param | 11264 | 13311 | 46 137 344..54 525 951 |
| keystorage | 13312 | 13439 | 54 525 952..55 050 239 |
| sec_efs | 13440 | 18559 | 55 050 240..76 021 759 |
| bota2 | 18560 | 20607 | 76 021 760..84 410 367 |
| steady | 20608 | 21631 | 84 410 368..88 604 671 |
| uh | 21632 | 22143 | 88 604 672..90 701 823 |
| uhcfg | 22144 | 23167 | 90 701 824..94 896 127 |
| dtb | 23168 | 25215 | 94 896 128..103 284 735 |
| dtbo | 25216 | 27263 | 103 284 736..111 673 343 |
| **boot** | 27264 | 41343 | 111 673 344..169 345 023 |
| **recovery** | 41344 | 57855 | 169 345 024..236 978 175 |
| radio | 57856 | 70655 | 236 978 176..289 406 975 |
| nad_refer | 70656 | 70911 | 289 406 976..290 455 551 |
| persistent | 70912 | 71039 | 290 455 552..290 979 839 |
| misc | 71040 | 71295 | 290 979 840..292 028 415 |
| keyrefuge | 71296 | 75391 | 292 028 416..308 805 631 |
| keydata | 75392 | 79487 | 308 805 632..325 582 847 |
| dqmdbg | 79488 | 83583 | 325 582 848..342 360 063 |
| vbmeta | 83584 | 83599 | 342 360 064..342 425 599 |
| bota3 | 83600 | 83967 | 342 425 600..343 932 927 |
| system | 83968 | 1543167 | 343 932 928..6 320 816 127 |
| vendor | 1543168 | 1824767 | 6 320 816 128..7 474 249 727 |
| product | 1824768 | 1983487 | 7 474 249 728..8 124 366 847 |
| cache | 1983488 | 2137087 | 8 124 366 848..8 753 512 447 |
| omr | 2137088 | 2149887 | 8 753 512 448..8 805 941 247 |
| cp_debug | 2149888 | 2151167 | 8 805 941 248..8 811 184 127 |
| **userdata** | 2151168 | 31234815 | 8 811 184 128..127 937 806 335 |

**Every start and end equals the PIT's values** → the phone's live GPT equals
stock, which is what `notes/pit-geometry.md` had left open. USERDATA partition
size = 29 083 648 LBAs = **119 126 642 688 bytes** (the earlier 119 126 622 208
from `/proc/partitions` is the *filesystem* size — TWRP's fstab carries
`length=-20480`, i.e. 5 LBAs of tail).

## 3. The framebuffer base — closed

The kernel's own cmdline (`/proc/device-tree/chosen/bootargs`) contains:

```
s3cfb.bootloaderfb=0xca000000
```

So **`EXYNOS_FB_BASE = 0xCA000000`** (the 9810's was `0xCC000000`). At
1440 × 3040 × 4 = 17 510 400 bytes the region is `0xCA000000..0xCA10B29FF`.
A scan of every `reg` property at DT depth ≤ 3 finds **no node reserving
0xCA000000** — it is ordinary bank0 DRAM that the bootloader uses as its scanout,
exactly the situation the 9810 had, so the memory map must publish it as
reserved rather than as RAM.

Other useful values from the same cmdline: `ess_setup=0xfd900000` (the
debug_snapshot header), `sec_avc_log=0x40000@0x91202000`,
`sec_tsp_log=0x40000@0x91244000`, `sec_disp_log=0x100000@0x91286000`,
`sec_debug_next=0x800000@0x91388000`, `androidboot.bootloader=G970FXXSGHWC1`
(matches `DeviceEligibility.cs:19`), `androidboot.dram_info=01,07,00,6G`,
initrd at `0x89000000..0x89a67000`.

## 4. DT `reg` values, now readable as root (big-endian `<addr size>`)

| node | address | size |
|---|---|---|
| `reserved-memory/el2_code` | `0x87000000` | `0x2000` |
| `reserved-memory/el2_earlymem` | `0x97E000000` | `0x2000000` |
| `reserved-memory/kaslr` | `0x80001000` | `0x1000` |
| `reserved-memory/mm_debug` | `0x91200000` | `0x1000` |
| `reserved-memory/sec_debug_magic` | `0x80000000` | `0x1000` |
| `reserved-memory/seclog_mem` | `0xC0000000` | `0x80000` |
| `reserved-memory/sec_rdx_bootdev` | `0x880000000` | `0xAD00000` |
| `reserved-memory/camera` | `0x99000000` | `0xE400000` |
| `reserved-memory/camera_contig` | `0x98200000` | `0xE00000` |
| `reserved-memory/secure_camera` | `0x96000000` | `0x2200000` |
| `reserved-memory/cp_ram_logging` | `0xFB900000` | `0x2000000` |
| `reserved-memory/modem_if` | `0xF0000000` | `0xB900000` |
| `reserved-memory/modem_spare` | `0xD0000000` | `0x1600000` |
| `abox-rmem` | alloc-ranges `0xC0000000`, size `0x3C00000`, alignment `0x400000` | |

`debug_snapshot/*` (all confirmed on the device, matching the AI table in
`notes/vendor-dt-reserved.md` exactly): header `0xFD900000+0x10000`,
log_kernel `0xFD910000+0x200000`, log_platform `0xFDB10000+0x400000`,
log_sfr `0xFDF10000+0x100000`, log_cachedump `0xFE010000+0xA00000`,
log_etm `0xFEA10000+0x100000`, **log_bcm `0xFEB10000+0x400000`** (the value
already VERIFIED in `Exynos9820.h`), log_llc `0xFEF10000+0x400000`,
log_dbgc `0xFF310000+0xF0000`, log_pstore `0xEB308000+0x8000`,
log_kevents `0xFF400000+0xC00000`, log_s2d `0xEB310000+0x1500000`,
log_arraydump `0xEC810000+0xA00000`.

Consequence for the PRAM breadcrumb: `0xFED14000` sits inside
`log_bcm`/`log_llc` (0xFEB10000..0xFF400000 is fully occupied by logs), so the
decision in `notes/vendor-dt-reserved.md` (use the unclaimed gap
`0xD9801000..0xF0000000`) stands, now confirmed from the device rather than from
the browser model.

## 5. What this closes

* `EXYNOS_FB_BASE` — 0xCA000000, from the device's own kernel cmdline.
* The live-GPT confirmation the PIT analysis left open (starts == PIT).
* The beyond0lte **unique GUIDs** (lower-case derived) — the repo's upper-case
  constants are wrong for this phone.
* `WindowsBytes` must be the partition size 119 126 642 688, not the
  filesystem size.
* The vendor DT values the browser model had only *claimed* — now read on-device.

## 6. Still open after this pass

* The UFS driver's fence/protected LBAs — the numbers are now known (§7 below) but
  the block has to be replaced in one step.
* `ToolsetManager.cs:84-89` payload names/messages (still say star2lte).
* Nothing is validated by booting yet: this pass read the device, it did not run
  the firmware on it.

## 7. The one code task left, with the numbers ready

`drivers/Exynos9820Ufs/Exynos9820Ufs.h:929-1130` is a single interlocking block
(the header forbids a half-edit because the `C_ASSERT`s below tie the constants
together). Everything in it is the 9810's own table. The S10e replacements, in the
driver's own 4096-byte LBA units, all read from the live GPT above:

| constant | 9810 | S10e |
|---|---|---|
| `UFS_WRITE_FENCE_FIRST_LBA` | 68 736 (sda18 SYSTEM) | **2 151 168** (USERDATA start) |
| `UFS_WRITE_FENCE_LAST_LBA` | 15 615 994 | **31 234 815** (USERDATA end = LastUsableLBA) |
| `UFS_PROTECTED_PARTITION_FIRST_LBA` | 1 195 136 (sda19 VENDOR) | **1 543 168** (vendor) |
| ESP (`sda21 CACHE`) | 1 526 912..1 680 511 | **1 983 488..2 137 087** |
| Windows volume (`sda25 USERDATA`) | 1 697 152..15 614 335 | **2 151 168..31 234 815** |
| scratch (one LBA past the volume) | 15 614 336..15 614 591 | **31 234 816..31 235 071** |
| EFS | 4096..9215 (512 B units in that comment) | **4096..9215 in 4096-byte LBAs** (bytes 16 777 216..37 748 735) |
| BOOT (UEFI) | sda10 | **sda14, LBA 27 264..41 343** |
| RECOVERY (TWRP) | sda11 | **sda15, LBA 41 344..57 855** |

Role change worth noting: on the 9810 the Windows volume was the **SYSTEM** slot
(sda18) and USERDATA was a protected partition; on the S10e the Windows volume **is**
USERDATA (sda31, the last partition), so the "protected run" is now the set *before*
the fence (vendor/product/cache/omr/cp_debug) and the ESP (cache) sits **below** the
fence's lower bound — i.e. the ESP is outside the writable window by construction,
which is the opposite of the 9810's layout where CACHE had to be released from the
protected range. The `FULL_WINDOWS_MODE` and `READOUT_*` comments all speak of
sda18/sda21/sda25 and have to be re-written, not just re-numbered.

Not attempted this pass: the block is ~200 lines of interlocking constants,
comments and `C_ASSERT`s, and the driver cannot be built or hardware-validated
from here (M7 is `needs-windows`). Half-editing it is exactly what the header's
`C_ASSERT`s exist to catch, so it stays one deliberate step.

* The UFS driver's fence/protected LBAs (`Exynos9820Ufs.h:980-998`) — the numbers
  are now known, the block still has to be replaced in one step.
* `ToolsetManager.cs` payload names/messages (still star2lte).
* Nothing is validated by booting yet: this pass read the device, it did not run
  the firmware on it.

## 8. Resume pass — the header's own geometry fields, decoded from the capture

The session crashed mid-edit of `Exynos9820Ufs.h` (the fence block was half-replaced
in the working tree, `git status` showed the file modified and the `C_ASSERT`s at
`Exynos9820Ufs.h:1157` then fail: `31236090 < 15615995`). Nothing was committed for
that edit, so the block is re-done in one step from here.

The head capture (`%TEMP%\opencode\s10e-gpt.img`, 139 264 B = 34 LBAs, LBA 1 at
offset 4096) was re-parsed with a field dump rather than only the entry array:

| offset | bytes | value |
|---|---|---|
| +0x00 | 8 | `EFI PART` |
| +0x08 | 4 | `0x00010000` revision |
| +0x0C | 4 | HeaderSize = **512** (the value Windows rejects, see the in-flight translation note) |
| +0x10 | 4 | HeaderCRC = `0x4035E98A` |
| +0x14 | 4 | 0 |
| +0x18 | 8 | MyLBA = **1** |
| +0x20 | 8 | AltLBA = **31 236 095** (= media LBAs 31 236 096 − 1, so the backup header is *declared* at the last LBA) |
| +0x28 | 8 | **6** — not the spec's MaxLBA; it is the head region's LBA count (LBA 0..5) |
| +0x30 | 8 | **31 236 090** = AltLBA − 5, i.e. **LastUsableLBA** |
| +0x38 | 16 | ASCII `ANDROID MMC DISK` (the MediaId GUID slot, name not GUID) |

So Samsung's header deviates from UEFI Table 5-5 at +0x28 and +0x38, and the field
that carries LastUsableLBA at +0x30 is the same field the 9810's header carried as
15 615 994 (its MaxLBA 15 615 999 − 5). The geometry the driver needs is therefore,
in the driver's own 4096-byte LBAs:

* media LBAs = 127 943 049 216 / 4096 = **31 236 096** → `UFS_EXPECTED_LAST_LOGICAL_BLOCK` = **31 236 095**
* backup region = the 5 LBAs **31 236 091 .. 31 236 095** → `UFS_GPT_BACKUP_FIRST_LBA` = **31 236 091**
* `LastUsableLBA` = **31 236 090** → the fence's upper bound, matching the 9810's design
  (its fence ended at its own LastUsableLBA)

Dead end recorded: the capture files `s10e-gpt-tail.img` (34 LBAs at skip 31 236 062,
all zero → **no backup GPT actually present**) and `s10e-tail2.img` (8 388 608 B =
2048 LBAs, exactly one non-zero LBA at capture-offset 763) are both on disk, but the
`skip` used for `s10e-tail2.img` was never written down, so the claim that the last
non-zero byte on the disk sits at LBA 31 234 811 is **unproven from the notes**. It is
re-read on the device below (§9) rather than trusted.

## 9. Tail read closed on-device (the claim the fence comment makes)

The fence comment wants to know whether the unallocated tail is really free, so it
was measured rather than assumed. `/tmp/sda` (the `mknod` from §1) is still present
in this TWRP boot; reads only, nothing written to the block device:

```
dd if=/tmp/sda of=/tmp/sda-tail3.img bs=4096 count=512  skip=31234560   # 2.0 MB
dd if=/tmp/sda of=/tmp/sda-tail4.img bs=4096 count=1024 skip=31235072   # 4.0 MB
```

Parsed locally (`%TEMP%\opencode\s10e-tail3.img`, `s10e-tail4.img`):

* `s10e-tail3.img` covers **LBA 31 234 560 .. 31 235 071** and has exactly **one**
  non-zero LBA in it: **31 234 811** (4096 B of high-entropy data, the last block of
  the USERDATA filesystem's own tail).
* `s10e-tail4.img` covers **LBA 31 235 072 .. 31 236 095** (the very end of the media)
  and is **all zero**.
* Together with §1's `s10e-gpt-tail.img` (31 236 062 .. 31 236 095, all zero) this
  proves: **every LBA from 31 234 812 to 31 236 095 reads back zero**, so the media
  carries **no backup GPT** even though the primary header declares
  `AltLBA = 31 236 095`.

What is NOT claimed: nothing here scans the interior of USERDATA, so the sentence
"the last non-zero byte anywhere on the disk" is not what was measured. The honest
form, and the one the header comment uses, is "every LBA from 31 234 812 to the end
of the media reads back zero; the last non-zero LBA in that measured run is 31 234 811,
inside USERDATA".

## Sources

* `adb` on `RF8M21HYDSZ` in TWRP: `id`, `blockdev --getsize64`, `dd` of the head
  and backup-GPT regions (`%TEMP%\opencode\s10e-gpt.img`, `s10e-gpt-tail.img`),
  `cat /proc/device-tree/chosen/bootargs`, the `reg` dumps
  (`%TEMP%\opencode\s10e-dt-regs.txt`, `s10e-dt-regs3.txt`).
* `%TEMP%\opencode\s10e-gpt.img` re-parsed field-by-field in this pass (the +0x28
  and +0x30 values above).
* `%TEMP%\opencode\s10e-tail3.img` (LBA 31 234 560 +512) and `s10e-tail4.img`
  (LBA 31 235 072 +1024), read from the device in this pass and parsed locally.
* `notes/pit-geometry.md` — the PIT table this confirms.
* `notes/vendor-dt-reserved.md` — the AI table this confirms.
