# Vendor DT `reserved-memory` values — from the browser model, with its cited sources

Question sent through the logged-in Chrome/CDP session (`claude.ai/chat/53802365-…`):
the beyond0lte `reserved-memory` children with `reg` values. Needed because the
phone itself cannot give them (`notes/s10e-live-reads.md` §9.1 — every `reg`
read is `Permission denied`).

**Status: AI-sourced, NOT verified here.** Claude said it could not find a
`beyond0lte` board DTS at all; what it has is the **shared exynos9820 SoC tree**
plus **beyond1lte (S10) boot logs**. Treat the SoC-tree row set as the
strongest available, and the modem rows as a different board.

## The table it returned (`reg = <hi lo size>`, address-cells 2 / size-cells 1)

| name | address | size |
|---|---|---|
| `debug_snapshot/header` | `0xFD900000` | `0x10000` |
| `debug_snapshot/log_kernel` | `0xFD910000` | `0x200000` |
| `debug_snapshot/log_platform` | `0xFDB10000` | `0x400000` |
| `debug_snapshot/log_sfr` | `0xFDF10000` | `0x100000` |
| `debug_snapshot/log_cachedump` | `0xFE010000` | `0xA00000` |
| `debug_snapshot/log_etm` | `0xFEA10000` | `0x100000` |
| `debug_snapshot/log_bcm` | `0xFEB10000` | `0x400000` |
| `debug_snapshot/log_llc` | `0xFEF10000` | `0x400000` |
| `debug_snapshot/log_dbgc` | `0xFF310000` | `0xF0000` |
| `debug_snapshot/log_pstore` | `0xEB308000` | `0x8000` |
| `debug_snapshot/log_kevents` | `0xFF400000` | `0xC00000` |
| `debug_snapshot/log_s2d` | `0xEB310000` | `0x1500000` |
| `debug_snapshot/log_arraydump` | `0xEC810000` | `0xA00000` |
| `seclog_mem` | `0xC0000000` | `0x80000` |
| `mm_debug` | `0x91200000` | `0x1000` |
| `sec_debug_magic` | `0x80000000` | `0x1000` |
| `sec_rdx_bootdev` | `0x880000000` | `0xAD00000` |
| `modem_spare` (beyond1lte log only) | `0xD0000000` | `0x1600000` |
| `modem_if` (beyond1lte log only) | `0xF0000000` | `0xB900000` |
| `cp_ram_logging` (beyond1lte log only) | `0xFB900000` | `0x2000000` |

It explicitly did **not** find `el2_code`, `el2_earlymem`, or any
framebuffer / sboot / simple-framebuffer node, and said so rather than guess.

## Corroboration — two rows are already in this checkout, independently

* `Exynos9820.h:69-70` (VERIFIED from the board file) has
  `modem_spare 0xD0000000 + 0x1600000` and `modem_if 0xF0000000 + 0xB900000` —
  identical to the AI's beyond1lte rows.
* `Beyond0LteResetSystemLib.c:75` and `Exynos9820.h` record
  `debug_snapshot.log_bcm = 0xFEB10000 + 0x400000` — identical to the AI's SoC-tree
  row.

Two independent agreements on a table of twenty is meaningful, but it is still
not a primary source opened from here.

## What this closes, and what it does not

**Closes (as a decision, not a read):** the PRAM breadcrumb candidate. The
`debug_snapshot` chain runs contiguously `0xFD900000 → 0x100000000`
(header + kernel + platform + sfr + cachedump + etm + bcm + llc + dbgc +
kevents, each ending exactly where the next begins), and `cp_ram_logging`
ends at `0xFD900000`, so `0xFD900000..0xFFFFFFFF` is fully occupied — the
9810's `0xFED14000` sits inside `log_bcm`/`log_llc`, which is why it is
unusable. The gap that is **not** claimed by anything is
`0xD9801000 .. 0xF0000000` (right after `s5100_msi`, whose base and size are
already VERIFIED in `Exynos9820.h:102-103`), and it is above the point the
memory map publishes (`bank0` stops at `0xD0000000`), so Windows never sees it
as RAM. That is the address to use if the breadcrumb channel is ever enabled.

**Deliberately not enabled now:** `BEYOND0LTE_PRAM_DIAG` stays `0`. It is a
diagnostics channel, not on the boot path, and the first boot wants the fewest
variables. Enabling it touches `ResetSystemLib.c`, `PlatformMemoryMapLib.c`,
`ArmPlatformLib.c` and the DSC — a separate task, after the boot works.

**Does not close:** the framebuffer base (no fb/sboot node found anywhere —
`EXYNOS_FB_BASE` stays `0`), and the partition LBAs (a DT/log question, not a
memory-map one).

## Sources

* `claude.ai/chat/53802365-c15b-439c-8c77-e149dfdde392` — the answer above.
* Sources Claude itself used (not opened from here): CruelKernel
  `samsung-exynos9820` → `arch/arm64/boot/dts/exynos/exynos9820.dts`; two
  beyond1lte dmesg gists (kreatoo).
* Local corroboration: `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:69-107`,
  `firmware/Platform/Beyond0LtePkg/Library/Beyond0LteResetSystemLib/Beyond0LteResetSystemLib.c:75`.
