# T06 — Re-read the stock recovery container geometry before repacking

Status: **open — blocked on a root shell**

## Goal

`pack-uefi-beyond0lte.ps1` wraps `[shim][FD]` in whatever container it is given
as `-Ref`: page size, section order and the `tail` come from that reference
image. The ref used so far is the **TWRP-era** recovery partition. After the
phone is restored to stock Android, the honest ref is the stock container.

## What is already enough

- `ref-boot-head.img` (4096 B, `C:\Users\ilya\AppData\Local\Temp\opencode\`) is the
  first two pages of the **stock Android** `boot` slot (`sda14`) — read while the
  phone was in TWRP but from a partition that was never TWRP-flashed. Its header
  magic is `ANDROID!` with kernel size `0x02A5300C` (44 373 772 B).
- `ref-recovery-head.img` (4096 B) is the TWRP recovery slot: same header shape,
  kernel `0x02A4F00C` (44 353 292 B), ramdisk `10 905 981` B.
- Both agree on the header layout and `page_size` 2048, which is the geometry the
  repack needs. **INFERENCE (mine):** the stock recovery container is very likely
  the same geometry; that is an inference, not a read.

## Blocked because

Reading `/dev/block/by-name/recovery` needs a root shell. With stock Android and
no TWRP there is none (`notes/K10-rkp.md` §"Rules that still bind this work" —
property contents and block devices are blocked to an unprivileged shell).

## Next actions

1. If TWRP is ever re-flashed: `dd if=/dev/block/by-name/recovery bs=2048 count=26991`
   (the exact line and SHA-1 are in `notes/m8-uefi-image.md`, HANDOFF §15 step 2)
   and re-run the packer against that ref.
2. Otherwise: proceed with the boot-slot header as the geometry witness and say
   explicitly in the commit that the ref is a **cross-checked inference**.

## Done when

A stock-recovery header is in hand, or the attempt is labelled as using the
inference above.

## Why this is now load-bearing (2026-10-10, from the first-boot failure)

The first boot (T07) failed with sboot's `dt table header check fail` -> `load_kernel: could not
do Normal boot (DT LOAD fail)`. Probing the TWRP-era ref we repacked (`notes/m8-uefi-image.md` B10):
`dt_size=1`, **no DTBH and no DTBO magic anywhere** - the TWRP-stripped recovery container carries
no device-tree blob, and sboot's Normal-boot path needs one. The **stock** recovery/boot container
carries the vendor dtb (DTBH), so repacking `[shim][FD]` into a stock-derived ref is the fix
direction. This is what T06 must close before another boot attempt. Reference stock artefacts on
the machine: `C:\Users\ilya\code\Dumps\` (AP_G970FXXSGHWC1 meta tar - stock boot/recovery).

## SUPERSEDED (2026-10-10, B11/B12) - the stock-ref premise was WRONG

Probing the STOCK containers disproved it: stock `recovery.img` and `boot.img` also have
`dt_size=1`, no DTBH/DTBO (Android-12 keeps the trees in separate `dt.img`/`dtbo.img`
partitions). So repacking against a stock container would NOT fix the DT LOAD fail. The real
cause is the **kernel Image's appended DTB** (the stock Linux kernel has one; our `[shim][FD]`
EFI does not) - see `notes/m8-uefi-image.md` B12. The fix is a new task (append a DTB to the
payload), not T06. T06 stays open only for the honest stock-geometry read; it is not the boot
blocker.
