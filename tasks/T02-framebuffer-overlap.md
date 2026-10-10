# T02 — Split the framebuffer out of the bank0 RAM gap

Status: **open**

## Goal

`EXYNOS_FB_BASE` is published as a **Device** region so writes reach the panel,
but it sits inside the bank0 RAM gap the map now emits, so the memory map
contains a cached-RAM and a Device entry over the same range.

## Known

- FB base **`0xCA000000`** is a device read, not the 9810's `0xCC000000`:
  `/proc/device-tree/chosen/bootargs` carries `s3cfb.bootloaderfb=0xca000000`
  (`notes/s10e-root-reads.md` §3, `PlatformMemoryMapLib.c:167-171`).
- The bank0 RAM gap that contains it is `0x87200000..0xBAB00000`
  (`notes/s10e-dtb-mmio.md` S04, the hand-checked gap table).
- **Precedent:** the validated 9810 map has the same shape
  (`Star2LtePkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c:171-175`
  publishes FB `MMAP_DEV` right after its RAM entry). So this is not known-bad,
  it is known-unexamined.

## Next actions

1. Decide whether Windows tolerates the RAM/Device overlap — the 9810 build is
   the evidence that it does, so the honest first move is **leave it and note it
   as tested-by-precedent**, not to split speculatively.
2. If splitting: emit the bank0 gap as two ranges around `0xCA000000 +
   EXYNOS_FB_SIZE`, and re-check the table capacity (`MMIO_REGION_COUNT + 12`).

## Done when

Either the row is closed with the 9810 precedent cited, or the split is
committed and the platform rebuilds clean.
