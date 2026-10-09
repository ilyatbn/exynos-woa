# M8 — the HSI40/GPIO-handoff subsystem: is it on the boot path at all?

Question that started the task: `MILESTONES.md` lists "the HSI40/GPIO-handoff
subsystem" as **missing and on the boot path** for the 9820. Check that before
porting ~90 KB of C.

## Findings (all mechanical, from the build files — not from prose)

1. **Neither package builds these drivers.**
   `rg "Hsi40|GpioHandoff|Uart1Handoff"` over
   `Star2LtePkg.dsc`, `Star2LtePkg.fdf`, `Beyond0LtePkg.dsc`, `Beyond0LtePkg.fdf`
   returns **nothing**. The 9810's own validated build does not contain
   `Star2LteHsi40HandoffDxe` or `Star2LteGpioHandoffDxe`.
2. **Neither package builds the ASLs either.**
   `Star2LtePkg/AcpiTables/AcpiTables.inf` `[Sources]` is
   `Dsdt/Fadt/Madt/Pptt/Gtdt` + two TODOs; `GpioHandoffDevices.asl` and
   `Hsi40Devices.asl` are **not listed**. `Beyond0LtePkg/AcpiTables/AcpiTables.inf`
   matches the same shape. So those two ASL files are orphan sources in the
   9810 tree too.
3. **Size of the orphan subsystem** (so a future port is scoped honestly):
   `Star2LteHsi40HandoffDxe` = 18 files, ~78 KB of C (largest `Hsi40ColdInit.c`
   13 684 B); `Star2LteGpioHandoffDxe` = 5 files, ~30 KB (largest
   `GpioHandoffDxe.c` 19 666 B); `GpioHandoffDevices.asl` 9 083 B,
   `Hsi40Devices.asl` 1 128 B. `docs/map/firmware.md:52` describes the HSI40
   driver as "HSI40 (I2C) census/cold-init/handoff trials + Win11 authority/
   runtime; feeds `Hsi40Devices.asl`" — i.e. a **trial/diagnostic** subsystem.
4. **What the 9820 actually knows about the device side** (`Exynos9820.h`):
   the touch bus is **HSI2C**, not HSI40 — `EXYNOS_TOUCH_HSI2C_*`, and the DSDT
   already carries the HSI2C10/SEC_TS path with the GSIV renumbered 436→420
   (`MILESTONES.md`, `notes/m8-dscfdf.md`). The GPIO windows are
   `EXYNOS_PERIC0_GPIO_BASE 0x10430000` and `EXYNOS_ALIVE_GPIO_BASE 0x15850000`
   (both VERIFIED; ALIVE moved from the 9810's 0x14050000). There is no HSI40
   window in `Exynos9820.h` at all.

## Decision

The HSI40/GPIO-handoff subsystem is **not on the boot path for either device** —
it is not built by the 9810 package either. Porting it is therefore not a
phase-one (M3–M9) requirement, and the milestone line that calls it "on the boot
path" is wrong. The honest state is: **not applicable to this phase**, and if it
is ever wanted it must first be established that the 9810 build ever used it.

Related dead end already recorded elsewhere: the CMU bank→power-domain map
(`EXYNOS_CMU_FSYS0A_*`/`EXYNOS_CMU_PERIC0_*` stay 0 because Samsung's cal-if
headers give QCH ids as enum positions, not addresses) — `notes/m8-libs.md`.

## Sources (local evidence actually consulted)

* `rg` over the four DSC/FDF files and both `AcpiTables.inf` `[Sources]` blocks.
* `firmware/Platform/Star2LtePkg/Drivers/Star2LteHsi40HandoffDxe/` and
  `Star2LteGpioHandoffDxe/` file listing with sizes.
* `docs/map/firmware.md:52-53` — what the subsystem is.
* `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` — HSI2C/GPIO
  windows (VERIFIED), absence of any HSI40 window.
* `notes/m8-libs.md` — the CMU bank dead end.
