# T05 — Re-derive the timeout clock from the 9820 tick rate

Status: **done** (2026-10-10, on-device read)

## Goal

`EXYNOS_ARCH_TIMER_FREQ_HZ` is `26000000u` in `Exynos9820.h:220`, marked
UNVERIFIED (9810 value). Every timeout in the UFS path and the watchdog servicer
is computed from it, so a wrong rate silently changes every wait — too short is
the dangerous direction.

## Known

- On the 9810 `CNTFRQ_EL0` read **0** and was RO at EL1, handled by a
  workspace-local fallback in `ArmPkg/Library/ArmGenericTimerPhyCounterLib`
  (`Beyond0LtePkg.dsc:396-399` says to re-check that fallback on the S10e).
- The 9820 kernel computes the CMU/UNIPRO rate as `mclk_rate/1000000 & 0x3FF`
  from the UNIPRO clock; **166 MHz** is what Mu-Silicium uses for 9810 AND 9830
  (`MILESTONES.md:156`).
- The live DTB has `clock-controller@0x1a240000` (`0x1A240000 + 0x8000`,
  `notes/s10e-dtb-mmio.md` S02) and the `ufs` node's `freq-table-hz` is **16 zero
  bytes** — i.e. the DTB does not carry the rate for this boot.

## Next actions

1. Read the rate from the hardware, not from a header: the UNIPRO/CMU clock
   register the kernel reads (`mclk` source in `drivers/soc/samsung`), or
   `CNTFRQ_EL0` from EL1 in our own firmware with a printed value.
2. Until that read exists, keep the macro marked UNVERIFIED and make the
   timeout code print the rate it assumed on the first boot.
3. Cross-check against the 9820 open-source kernel's `samsung,exynos9820` DT
   binding (`include/dt-bindings/...`) if it declares the tick rate.

## Done when

A printed register read names the rate, and the macro cites that read.

## Read attempt (2026-10-10) - blocked, task stays open

FACT the S10e is attached in normal Android (`adb devices` -> `RF8M21HYDSZ device`), and the
adb shell runs as root, but **DTB property contents are still `Permission denied`**:
`od /proc/device-tree/fixed-rate-clocks/oscclk/clock-frequency` and
`/proc/device-tree/ufs@0x13D60000/{freq-table-hz,clocks,clock-names}` all deny. Node **names**
are readable (`fixed-rate-clocks/oscclk`, `ufs@0x13D60000`), contents are not - the same
SELinux wall as `notes/s10e-live-reads.md` §9. `freq-table-hz` is 16 bytes (the zero table the
notes already recorded).
CONCLUSION: the rate is NOT closable by a pre-boot read. `EXYNOS_ARCH_TIMER_FREQ_HZ` stays
UNVERIFIED (26 MHz, 9810 value). The first boot already surfaces the assumed rate:
`Beyond0LteAcpiPlatformDxe.c:238/279/288` publish it into the GTDT CNTFID0 frame and the PRAM
hex, so the boot observation closes T05 - it is a boot-time task, not a pre-boot one. Do NOT
mark it done until that read is seen.

## Closed (2026-10-10) - read on the device by the boot shim

Boot #11's shim painted CNTFRQ_EL0 as a binary strip; decoded from the photo it is **0x018CBA80 =
26 000 000 Hz** (notes/m8-uefi-image.md B47). The carried 26 MHz is right; `Exynos9820.h`
`EXYNOS_ARCH_TIMER_FREQ_HZ` now cites that read instead of saying UNVERIFIED. (edk2-exynos'
exynos9820.dsc also sets PcdArmArchTimerFreqInHz 26000000 - prior art, consistent.)
