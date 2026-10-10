# S10e live DTB — MMIO node inventory (the closable UNVERIFIED extents)

Started: 2026-10-10. Device-read task, no code changes yet. Phone `RF8M21HYDSZ` in
TWRP (root shell). Reads only; nothing written to the phone.

## Why this file exists

Earlier sessions could only read `/proc/device-tree/<node>/` **names**, so every
MMIO *extent* in `Exynos9820.h` stayed `UNVERIFIED (extent not read)`. In a
**TWRP root shell the property contents are readable** (proven in
`notes/K10-rkp.md` T16-T18), so the extents are now closable.

## Progress log

### S01 — the live DTB's `@`-named nodes (451 children at the root)

`ls /proc/device-tree | grep "@"` on the phone. The node **name suffix is the
MMIO base**, and these are the ones the 9820 silicon header cares about:

| DTB node name | base implied by the name | repo macro |
|---|---|---|
| `speedy@15970000`, `speedy1@15980000` | `0x15970000`, `0x15980000` | `EXYNOS_SPEEDY_BASE/SIZE`, `EXYNOS_SPEEDY1_*` (both SIZE = 0, "extent not read") |
| `decon_f@0x19030000`, `decon_t@0x19050000` | `0x19030000`, `0x19050000` | `EXYNOS_DECON_T_SIZE` (0x10000, 9810 extent) |
| `dsim@0x19080000` | `0x19080000` | `EXYNOS_DSIM_SIZE` (0x10000, unread) |
| `watchdog_cl0@10050000`, `watchdog_cl2@10060000` | `0x10050000`, `0x10060000` | `EXYNOS_WDT_BASE/SIZE` — bases already VERIFIED, **size unread** |
| `system-controller@15860000` | `0x15860000` | `EXYNOS_PMU_BASE` (used by the reset lib), `EXYNOS_PMU_SIZE` = 0x10000 (9810 extent) |
| `clock-controller@0x1a240000` | `0x1A240000` | the CMU bank question (§5: which bank feeds PERIC0/FSYS0A) |
| `sysreg-controller@13020000`, `sysreg-controller@13C10000` | two windows | the 9810's `SYSREG_PERIC0` has no 9820 equivalent at the 9810 address |
| `interrupt-controller@10100000`, `abox-gic@18cf0000` | GIC | MADT GIC bases |
| `ufs@0x13D60000` | `0x13D60000` | the UFS driver's own window — a second, DTB-side read of it |
| `pd-fsys0a@15862200`, `pd-dpu@15861e00`, `pd-npu0@15861d00`, `pd-g2d@15862300`, … | all inside `0x15860000` | **INFERENCE (mine, to check): every power-domain node lives inside the `system-controller` window at `0x15860000`, which is where the reset library's PMU base points** |
| `mct@10040000`, `phy@10B00000`, `pinctrl@…`, `hsi2c@…`, `spi@…`, `uart@…` | many | HSI2C/PMU pin windows the ACPI tables use |

Next: read the `reg` of the rows above (extent = the size cell) and write them
into `Exynos9820.h`, replacing the 9810-carried extents with reads.

### S02 — `reg` + identity props pulled for the UFS/PMU/CMU/watchdog nodes (byte-exact)

All `adb pull` of `/proc/device-tree/<node>/<prop>` into
`C:\Users\ilya\AppData\Local\Temp\opencode\dtb2\`; decoded big-endian. The `ufs`
node declares `#address-cells = 2`, `#size-cells = 1` (read back as
`00 00 00 02` / `00 00 00 01`), so its `reg` is 4 ranges of 12 bytes.

| node | raw `reg` | decoded |
|---|---|---|
| `watchdog_cl0@10050000` | `00 00 00 00 10 05 00 00 00 00 01 00` | `0x10050000 + 0x1000` — **`EXYNOS_WDT_SIZE` now VERIFIED** (was "9810 extent, unread") |
| `watchdog_cl2@10060000` | `00 00 00 00 10 06 00 00 00 00 01 00` | `0x10060000 + 0x1000` |
| `system-controller@15860000` | `00 00 00 00 15 86 00 00 00 01 00 00` | `0x15860000 + 0x10000` — **`EXYNOS_PMU_SIZE` VERIFIED** (the carried 9810 value happens to be right) |
| `speedy@15970000` | `00 00 00 00 15 97 00 00 00 00 20 00` | `0x15970000 + 0x2000` — was `SIZE = 0` ("extent not read") |
| `speedy1@15980000` | `00 00 00 00 15 98 00 00 00 00 20 00` | `0x15980000 + 0x2000` |
| `decon_t@0x19050000` | `00 00 00 00 19 05 00 00 00 01 00 00` | `0x19050000 + 0x10000` |
| `dsim@0x19080000` | `00 00 00 00 19 08 00 00 00 00 03 00` + `00 00 00 00 19 16 10 00 00 00 08 00` + `00 00 00 00 19 16 00 00 00 00 01 00` | **three** ranges: `0x19080000 + 0x300`, `0x19161000 + 0x800`, `0x19160000 + 0x100` |
| `clock-controller@0x1a240000` | `00 00 00 00 1A 24 00 00 00 00 80 00` | `0x1A240000 + 0x8000` — matches cal-if's `CMU_CMU 0x1a240000 +0x8000`; the repo's `EXYNOS_CMU_TOP_*` uses the **`cmu_ewf`** node (`+0x1000`) instead |
| `sysreg-controller@13020000` | `00 00 00 00 13 02 00 00 00 00 12 00` | `0x13020000 + 0x1200` |
| `interrupt-controller@10100000` | 4 ranges | `0x10101000 + 0x1000`, `0x10102000 + 0x1000`, `0x10104000 + 0x2000`, `0x10106000 + 0x2000` |
| `mct@10040000` | `00 00 00 00 10 04 00 00 00 00 08 00` | `0x10040000 + 0x8000` |
| `ufs@0x13D60000` `reg` | 4 ranges | `0x13D60000 + 0x200`, `0x13D61100 + 0x200`, `0x13D50000 + 0x800000`, `0x13D70000 + 0x1000` |

**UFS node identity props (live):** `hw-rev = 0x00000006` (confirms the DTS's
`UFS_VER_0006` on the phone, not just in source), `evt-ver = 0x01`,
`brd-for-cal = 0x10` (the DTS said `1` for SMDK — live S10e says `0x10`),
`fmp-id = 0`, `smu-id = 0`, `freq-table-hz` = 16 zero bytes, `dma-coherent` and
`ranges` are empty flag properties, `compatible = "samsung,exynos-ufs"`,
`clocks = <0x2 0x135, 0x2 0x138>` (two phandles — resolving them answers the
§5 "which CMU bank feeds the PHY" question), `interrupts = <0, 0xF9, 0>`.

**Discrepancy to resolve, not to paper over:** the live TWRP-boot DTB's UFS
**sizes** (`+0x200`, `+0x200`, `+0x800000`, `+0x1000`) do NOT match the values
`Exynos9820.h:319-327` calls VERIFIED from the *vendor* DT (`HCI +0x2000`,
`UNIPRO +0x8000`, `UFSP +0x1000`). The bases agree everywhere; the extents do
not. Either sboot ships a trimmed DTB for this boot, or the earlier read came
from the `dtbo` overlay. The read that separates them: the stock-Android
`dtbo.img` node list, which is in `notes/exynos9820-research.md:77`
(`UFS protector 13D70000 +0x100` there vs `+0x1000` live).

### S03 — harsh review of the extents commit

Checked mechanically: `build-uefi.ps1 -Platform Beyond0LtePkg -Target DEBUG` exit 0,
`FVMAIN_COMPACT` 700416 total / **3096 free** (was 3104 before the two new entries);
shim re-assembled exit 0 at 288 B; `git diff --stat` on the 9810 tree still empty.
`PlatformMemoryMapLib.c:196-203` confirms the loop skips `Size == 0`, so the claim
"the two skipped entries are now published" is true, not assumed.

One defect: `PlatformMemoryMapLib.c:84-85` still carried the inline comment
"size unread -> skipped" for exactly the two entries that are now read and
published. Fixed in its own commit.

### S04 — review of the map-carve commit (arithmetic checked by hand, not by reading)

Bank0 `Start 0x80000000`, `End 0xBAB00000`; reservations sorted ascending, so
the single-pass loop is valid. Emitted gaps:

| gap | length | page-multiple? |
|---|---|---|
| `0x80001000 .. 0x80100000` | `0xFFE000` | yes (`0xFFE` pages) |
| `0x80200000 .. 0x87000000` | `0x6E00000` | yes |
| `0x87200000 .. 0xBAB00000` | `0x33900000` | yes |

`el2_earlymem` (`0x97E000000`) is skipped in bank0 and bank3 (the guard is
`ResBase >= End`) and lands in bank4: gaps `0x900000000+0x7E000000` and
`0x97E200000+0x1E00000`. Bank3 emits one full `0x80000000` entry. RAM entries
total 6, capacity `MMIO_REGION_COUNT + 12` ✓, and the terminator index is
ASSERTed.

**Pre-existing, not introduced here:** the framebuffer (`0xCA000000`, published
`MMAP_DEV`) sits inside the bank0 RAM gap `0x87200000..0xBAB00000`, so the map
still contains a RAM/DEV overlap. It was there before this commit and is the
same class of hazard the reservation carve fixes — it belongs in the same
follow-up, not in this one.

## Sources

- Device: `adb shell` on `RF8M21HYDSZ   recovery` (TWRP root), `ls /proc/device-tree`.
