# S10e partition table — what is a fact, what is derived, what is still missing

Read-only `adb` pass on `RF8M21HYDSZ` (2026-10-09). No write, no flash, no
partition touched.

## Fact — name → partition index (`ls -l /dev/block/by-name`, symlink targets readable)

```
boot      sda14   bota0 sda1   bota1 sda2   bota2 sda8    bota3 sda24
cache     sda28   cp_debug sda30 cpefs sdd1  dqmdbg sda22  dtb  sda12
dtbo      sda13   efs     sda3   keydata sda21 keyrefuge sda20 keystorage sda6
misc      sda19   nad_refer sda17 omr sda29  param sda4    persistent sda18
product   sda27   radio   sda16  recovery sda15 sec_efs sda7 steady sda9
system    sda25   uh      sda10  uhcfg sda11   up_param sda5  userdata sda31
vbmeta    sda23   vendor  sda26
```

`sda` = 124 944 384 KiB (249 888 768 LBAs), 31 partitions on `sda`
(sda16…sda31 are on kernel major 259, i.e. they are part of the same container).

## Fact — sizes (`/proc/partitions`, 1 KiB blocks → LBAs = ×2)

| part | KiB | LBAs | | part | KiB | LBAs |
|---|---|---|---|---|---|---|
| sda1 | 4 608 | 9 216 | | sda17 | 1 024 | 2 048 |
| sda2 | 7 680 | 15 360 | | sda18 | 512 | 1 024 |
| sda3 (EFS) | 20 480 | 40 960 | | sda19 | 1 024 | 2 048 |
| sda4 (param) | 8 192 | 16 384 | | sda20 (keyrefuge) | 16 384 | 32 768 |
| sda5 (up_param) | 8 192 | 16 384 | | sda21 (keydata) | 16 384 | 32 768 |
| sda6 (keystorage) | 512 | 1 024 | | sda22 (dqmdbg) | 16 384 | 32 768 |
| sda7 (sec_efs) | 20 480 | 40 960 | | sda23 (vbmeta) | 64 | 128 |
| sda8 (bota2) | 8 192 | 16 384 | | sda24 (bota3) | 1 472 | 2 944 |
| sda9 (steady) | 4 096 | 8 192 | | sda25 (system) | 5 836 800 | 11 673 600 |
| sda10 (uh) | 2 048 | 4 096 | | sda26 (vendor) | 1 126 400 | 2 252 800 |
| sda11 (uhcfg) | 4 096 | 8 192 | | sda27 (product) | 634 880 | 1 269 760 |
| sda12 (dtb) | 8 192 | 16 384 | | sda28 (cache) | 614 400 | 1 228 800 |
| sda13 (dtbo) | 8 192 | 16 384 | | sda29 (omr) | 51 200 | 102 400 |
| sda14 (**boot**) | 56 320 | 112 640 | | sda30 (cp_debug) | 5 120 | 10 240 |
| sda15 (**recovery**) | 66 048 | 132 096 | | sda31 (**userdata**) | 116 334 592 | 232 669 184 |
| sda16 (radio) | 51 200 | 102 400 | | | | | |

Sum of partitions = 249 870 336 LBAs; the container is 249 888 768 → **18 432
LBAs (9 MiB) unaccounted**, which is the GPT plus alignment padding.

## Derived (NOT a read) — cumulative starts, assuming index order and no gaps

Under that assumption: `boot` (sda14) ≈ LBA 209 920, `recovery` (sda15) ≈ 322 560,
`system` (sda25) ≈ 663 552, `vendor` (sda26) ≈ 12 337 152, `userdata` (sda31) ≈
17 201 152 .. 249 870 335. These are arithmetic, and `Exynos9820Ufs.h:975-978`
says explicitly the block must be replaced **from a real GPT read, not from
arithmetic** — so they are recorded as a shape check only, and are NOT used to
edit the driver.

## What this proves about the driver's constants

The 9810 table in `drivers/Exynos9820Ufs/Exynos9820Ufs.h:929-1002` is wrong for
this device in a way that is now **confirmed by names, not guessed**:

| role | 9810 | S10e (fact) |
|---|---|---|
| BOOT (UEFI) | sda10 | **sda14** |
| RECOVERY (TWRP) | sda11 | **sda15** |
| EFS | sda3 | sda3 (same index; range unconfirmed) |
| Windows volume | sda18 SYSTEM | **sda31 userdata** candidate (the header already says this) |
| VENDOR..USERDATA protected run | sda19..sda25 | **sda26 vendor, sda27 product, sda28 cache, sda29 omr, sda30 cp_debug, sda31 userdata** — and sda19..sda25 here are misc/keyrefuge/keydata/dqmdbg/vbmeta/bota3, a completely different set |

So `UFS_PROTECTED_PARTITION_*` and `UFS_WRITE_FENCE_FIRST_LBA/LAST_LBA`
(`Exynos9820Ufs.h:980-998`) protect the wrong ranges on an S10e. That is the
single most dangerous remaining constant in the tree: a wrong fence admits writes
into real partitions.

## Still missing, and why `adb` cannot get it

`/sys/block/sda` is `Permission denied` and `/sys/block/sda/sdaN/start` likewise,
so exact starts are unread from this shell. The honest routes are: a root shell on
the device, or the driver's own LBA-1 (GPT header) read once Windows runs, or
Samsung's block-mapper dump from the same Android release.

## Sources

* `adb` on `RF8M21HYDSZ`: `ls -l /dev/block/by-name`, `cat /proc/partitions`,
  `ls /sys/block` (works), `cat /sys/block/sda/sda10/start` (Permission denied).
* `drivers/Exynos9820Ufs/Exynos9820Ufs.h:929-1002` — the 9810 table this is
  compared against.
* `notes/dock-desktop.md` §13 — the earlier arithmetic pass this supersedes.
