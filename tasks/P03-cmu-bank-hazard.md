# P03 — Which CMU bank feeds PERIC0 (touch bus) and FSYS0A (USB)

Status: **open — half resolved**

## What is resolved

The **addresses** are a read from Samsung's own calibration table
(`drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c`, `cmucal_sfr_block_list[]`,
34 entries) — recorded in `Exynos9820.h:264-282`:

```
CMU_PERIC0 0x10400000 +0x8000     CMU_FSYS0A 0x10a00000 +0x8000
CMU_FSYS0  0x13000000 +0x8000     CMU_PERIC1 0x10800000 +0x8000
CMU_BUSC   0x1a200000 +0x8000     CMU_CMU    0x1a240000 +0x8000
CMU_MIF..MIF3 = 0x1bc00000/0x1bd00000/0x1be00000/0x1bf00000
```

So the DT's five-window `cmu` node is **not** the whole CMU set, and PERIC0 sits
at the same base the 9810 used (but with a full `0x8000` block, not `0x4000`).

## What is still open

The **parentage**: which CMU block is the power/controller parent for the touch
bus and for USB, i.e. what the DSDT should declare. The 9810's `11000000` does
not exist on this SoC at all.

## The read that closes it (found, not yet taken)

The live DTB carries power-domain nodes **inside** the `system-controller`
window at `0x15860000` (`notes/s10e-dtb-mmio.md` S01):
`pd-fsys0a@15862200`, `pd-dpu@15861e00`, `pd-npu0@15861d00`, `pd-g2d@15862300`,
`pd-isphq@15862400`, … — and the `ufs` node declares
`clocks = <0x2 0x135, 0x2 0x138>` (two phandles). Resolving those phandles to
node names is the missing link. BusyBox `od`/`grep` quoting through `adb shell`
proved unreliable for this; pull the `phandle` files and decode locally
(the method that worked in `notes/K10-rkp.md` T17).

## Done when

The DSDT's CMU parent for PERIC0/FSYS0A is named by a DTB read, not by the
9810's shape.
