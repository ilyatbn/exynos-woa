# T05 — Re-derive the timeout clock from the 9820 tick rate

Status: **open**

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
