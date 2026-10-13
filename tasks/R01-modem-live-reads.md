# R01 — live reads: which modem DT nodes are enabled, and what the running kernel exposes

Status: open — **needs-phone** (`adb devices` empty on the Linux box, 2026-10-12; the S10e is
`RF8M21HYDSZ` / `SM_G970F`, adb on this box is `/usr/bin/adb`)

## Goal

Establish from the *live* device tree and kernel which modem-interface device nodes are actually
enabled on this board, which IRQs the `mif` driver holds, and which device nodes userspace uses —
closes U1 (node identity) and U4 (userspace path) in `notes/modem-wwan.md`.

## Done when

The output block below is captured and pasted into `notes/modem-wwan.md` — in particular
`/proc/interrupts` shows at least one line naming the modem/mif driver with a nonzero count.

## Do this

```
adb shell 'ls /proc/device-tree/ | grep -iE "modem|mif|s5100|cp[0-9_]"'
adb shell 'for n in /proc/device-tree/*modem* /proc/device-tree/*mif* /proc/device-tree/*s5100*; do echo "== $n"; ls -l "$n"; done'
adb shell 'cat /proc/interrupts | grep -iE "modem|mif|s5100|shannon"'
# keep the WHOLE line: the second column (edge/level) is the DSDT Interrupt()
# polarity evidence — the vendor kernel forces no trigger type
# (IRQF_NO_SUSPEND|IRQF_NO_THREAD only, modem_ctrl_s5000ap.c:671; mcu_ipc
# requests with flags 0, mcu_ipc.c:472), so the live GIC config is the only
# source for it (second review, 2026-10-12).
adb shell 'cat /proc/devices | grep -iE "modem|mif|s5100|cp"'
adb shell 'ls /dev/ | grep -iE "modem|mif|cp|smd|shm"'
adb shell 'ls /vendor/firmware/ | grep -iE "cp|modem|shannon|s5100"'   # names only
adb shell 'getprop | grep -iE "ril|modem|cp.version"'
```

If a TWRP root shell is available, additionally (contents, not just names):

```
adb shell 'od -t x4 /proc/device-tree/<modem-node>/reg'
adb shell 'od -t x4 /proc/device-tree/<modem-node>/interrupts'
```

## What it proves / if it fails

| Output | Meaning | Next step |
|---|---|---|
| a `/proc/device-tree/<node>` whose name matches the mif driver + an `/proc/interrupts` line for it | **U1/U4 close**: node identity + GSIV (SPI+32) become VERIFIED; the line's edge/level column closes the DSDT `Interrupt()` polarity for the future WWAN device | constants pass into `Exynos9820.h` (done) → DSDT task |
| device-tree names but no `/proc/interrupts` match | driver named differently — grep `kallsyms.log` for the driver owning those IRQs | notes U1 stays open, refine from mirror |
| no modem nodes in `/proc/device-tree` at all | modem is described only in a dtbo we mis-resolved, or under another name | walk `dumps/hhxj6/dts/dtb*.dtb` with `fdt.py` (already queued in the dump pass) |

## Known

- The shipped kernel contains the `mif` IPC driver (ring descriptors, SBD_IPC shmem, DIT handlers)
  — `dumps/hhxj6/kernel` strings, `notes/modem-wwan.md` T0.
- Modem memory carve-outs (modem_if/modem_if2/cp2cp_shm/s5100_msi) are VERIFIED vendor-DT reads —
  `Exynos9820.h:155-164`; note (T7): the live S10e reserves only `modem_if`/`modem_spare`/
  `cp_ram_logging` — the external-S5100 carve-outs are not enabled on this board
  (`notes/s10e-live-reads.md:125-127`).
- `pcie_1` (modem doorbell) is disabled on this board — `notes/m12-wifi-chip.md:88`.
- AP↔CP GPIO handshake pinctrl states exist (`s5100_evt1_dual_*`) — `dumps/hhxj6/base.dts:803-850`.

## Next actions

1. Paste raw output into `notes/modem-wwan.md` under a dated heading, verbatim.
2. Cross-check the node names against the dtbo walk; whichever source is primary, cite it.
3. Convert node `interrupts` cells to GSIVs (SPI + 32) and queue the constants task.

## Rollback / Do NOT

Read-only: `ls`, `cat`, `od`, `getprop` only. No partition writes, no flashing, nothing under
`/dev/block/**` touched. This task is safe on the only device.
