# T03 — Resolve the UFS extent disagreement (live DTB vs vendor DT)

Status: **open**

## Goal

`Exynos9820.h:319-327` calls the UFS window sizes VERIFIED from the **vendor**
DT. The live DTB declares the same **bases** with different **sizes**. One of the
two reads is describing a different thing; until that is settled the windows
cannot be trusted.

## The two data sets

| window | vendor DT (as committed) | live DTB (`adb pull`, `notes/s10e-dtb-mmio.md` S02) |
|---|---|---|
| HCI `0x13D60000` | `+0x2000` | `+0x200` |
| vendor-specific HCI `0x13D61100` | (inside HCI) | `+0x200` |
| UNIPRO `0x13D50000` | `+0x8000` | `+0x800000` |
| UFSP `0x13D70000` | `+0x1000` | `+0x1000` ✓ |
| PMA `0x13D64000` | `+0x800` | **not in the live list at all** |
| cport `0x13D68000` | present | not present |
| io-coherency `0x13C10700` | present | not present (but `ufs@0x13D60000/ufs-io-coherency` exists as a child) |

The live `ufs` node also declares `#address-cells = 2`, `#size-cells = 1`
(read back as `00 00 00 02` / `00 00 00 01`), so the 12-byte-per-range decode is
right, not a mis-parse.

## Next actions

1. Re-read the **stock** `dtbo` node list (already in `notes/exynos9820-research.md:77`,
   which says the UFS protector is `0x13D70000 + 0x100`) and compare with both
   tables — three data points, not two.
2. Decide which one the driver maps. Mapping **more** than the hardware has is
   harmless (Device attribute); mapping **less** can hide registers the driver
   needs, so the safe interim is the union, marked as such.
3. Keep the disagreement visible in the header until it is closed.

## Done when

One table is named as authoritative with the read that says so, and the macros
carry that source instead of a comment claiming both.
