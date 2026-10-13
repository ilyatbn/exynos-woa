# V01 — live reads: does the S10e's kernel expose the MFC, and under what name

Status: open — **needs-phone** (`adb devices` empty on this Linux box, 2026-10-13; adb is
`/usr/bin/adb` and on PATH here). The S10e is `RF8M21HYDSZ` / `SM_G970F`, the only device.

## Goal

Establish from the **running** device which video-codec device nodes exist, which driver owns them,
which IRQ line the codec holds, and which firmware blob the vendor loads — closing U1 (is the block
enabled on this board) and U4 (blob name) in `notes/video-mfc.md` without guessing either.

## Done when

`/proc/interrupts` shows at least one line naming the codec driver (expected name family: `mfc` /
`s5p-mfc` / `mfc_*`), **and** the `/proc/device-tree` listing shows the codec node's real name. Both
outputs pasted into `notes/video-mfc.md` under a dated heading.

## Do this

```
adb shell 'ls /proc/device-tree/ | grep -iE "mfc|codec|video|m2m"'
adb shell 'for n in /proc/device-tree/*mfc* /proc/device-tree/*codec*; do echo "== $n"; ls -l "$n"; done'
adb shell 'cat /proc/interrupts | grep -iE "mfc|codec|video"'
adb shell 'cat /proc/devices | grep -iE "mfc|video|media"'
adb shell 'ls -l /dev/video* /dev/media* /dev/mediacodec* 2>/dev/null'
adb shell 'cat /proc/modules | grep -iE "mfc|video|codec"'
adb shell 'ls /vendor/firmware | grep -iE "mfc|vidc"'          # names only — never commit a blob
adb shell 'ls /sys/class/video4linux 2>/dev/null'
```

Keep the **whole** `/proc/interrupts` line: the second column (edge/level) is the DSDT `Interrupt()`
polarity evidence for any future codec device, the same trick used for the modem (`tasks/R01-*.md`).

If a **TWRP root shell** is available, additionally (contents, not just names — an unprivileged
shell is SELinux-blocked from property and `reg` contents):

```
adb shell 'od -t x4 /proc/device-tree/<mfc-node>/reg'
adb shell 'od -t x4 /proc/device-tree/<mfc-node>/interrupts'
adb shell 'od -t x4 /proc/device-tree/<mfc-node>/clocks'
adb shell 'ls -l /proc/device-tree/<mfc-node>/iommu'
```

## What it proves / if it fails

| Output | Meaning | Next step |
|---|---|---|
| a `/proc/device-tree/<node>` for the codec + an `/proc/interrupts` line for it | **U1 closes**: the block is enabled on this SKU; the GSIV (SPI+32) and the trigger column become VERIFIED from the live device | constants pass (`notes/video-mfc.md`), then the DSDT decision |
| node present, no `/proc/interrupts` line | driver present but idle/never opened, or it names the IRQ differently | grep `dumps/hhxj6/kallsyms.log` + `strings` for the driver owning those IRQs |
| `/dev/video*` exists but no DT node name matches | the codec is described only in a dtbo overlay | walk `dumps/hhxj6/dts/` + `dtbo.img` with `tools/dump-tools/fdt.py` (primary source) |
| nothing at all — no node, no device, no IRQ | this SKU may ship the codec disabled, or the block is named something else entirely | do **not** conclude "no MFC": check the vendor dtsi in a mirror and the defconfig first |

## Known

- The shipped Android 12 dump is `dumps/hhxj6/` (`G970FXXUHHXJ6`): `base.dtb`/`base.dts`, the `ov*.dts`
  overlays, `kernel` (Image), `kallsyms.log`, `vendor/vendor.raw`. Samsung's DTB layout needs
  `tools/dump-tools/fdt.py` (plain `dtc` garbles phandles — `notes/modem-wwan.md` T1).
- The GPU is a **different** block: `mali@18500000` (`notes/d5-gpu-display.md`). Do not read a
  `drm`/`mali` line as the codec.
- What an unprivileged shell can and cannot read is already established: `/proc/device-tree/<node>/`
  names and symlink targets yes, property contents no — `notes/s10e-live-reads.md` §9.

## Next actions

1. Paste the raw output into `notes/video-mfc.md` under a dated heading, verbatim.
2. Cross-check the node name against the dtbo walk; whichever source is primary, cite it.
3. Convert the `interrupts` cells to a GSIV (SPI + 32) and queue the constants task.

## Rollback / Do NOT

Read-only: `ls`, `cat`, `od`, `getprop` only. No partition writes, no flashing, nothing under
`/dev/block/**` touched. Entering TWRP is by key combo (Vol Up + Bixby + Power) so the reset history
stays clean. This task is safe on the only device.
