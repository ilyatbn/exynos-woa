# M8 — port the small ACPI tables 9810 → 9820 (`beyond0lte`, Galaxy S10e)

Task: create `AcpiTables.inf`, `Fadt.aslc`, `Gtdt.aslc`, `Pptt.aslc` in
`firmware/Platform/Beyond0LtePkg/AcpiTables/` by porting the 9810 counterparts in
`firmware/Platform/Star2LtePkg/AcpiTables/`. The 9820 `Dsdt.asl`, `Madt.aslc`,
`Beyond0LteAcpiHeader.h` already exist and are not redone. Nothing under
`Star2LtePkg` or `Silicon/Exynos9810Pkg` is touched.

## Ground truth already in the tree (read before writing)

- **Topology (VERIFIED, from the already-ported `Madt.aslc` + `notes/dock-desktop.md` §16):**
  8 cores, **two MPIDR clusters sized 6 + 2** — `cpu@0000`..`cpu@0005` (Aff1=0) and
  `cpu@0100`,`cpu@0101` (Aff1=1). Inside them are **three efficiency classes**:
  ananke/LITTLE (UID 0–3, A55, part 0xd05), prometheus/MID (UID 4–5, part 0xd0a),
  cheetah/BIG (UID 6–7, Samsung implementer 0x53 part 0x003). `PcdClusterCount = 2`
  (`Exynos9820Pkg.dec:57`). The MADT's MPIDR column is 0x000..0x005 then 0x100,0x101.
- **GIC (VERIFIED, identical to 9810):** GICD/GICC/GICH/GICV = 10101000/10102000/10104000/10106000,
  GICv2, but the 9820 node is `interrupt-controller@10100000` compatible
  "arm,cortex-a15-gic","arm,cortex-a9-gic" — NOT "arm,gic-400" (`Exynos9820.h:125-146`).
- **PMU MOVES (base VERIFIED, offset not read):** 9820 PMU = `system-controller@15860000`
  (`EXYNOS_PMU_BASE 0x15860000`, `Exynos9820.h:185`); the 9810's 0x14060000 is absent.
  The 9810 FADT's SWRESET address 0x14060400 = PMU base + 0x400. The +0x400 SWRESET
  offset is NOT a read on the 9820 → UNVERIFIED.
- **Arch timer freq UNVERIFIED on 9820:** `EXYNOS_ARCH_TIMER_FREQ_HZ 26000000u` is flagged
  "UNVERIFIED (9810 value)" in `Exynos9820.h:169`. Whether sboot programs CNTFRQ_EL0 is unknown.
- **No `Beyond0LteAcpiPlatformDxe` exists yet** (only `Star2LteAcpiPlatformDxe`), so the
  install-time GTDT counter patch (26 MHz frame + MCT read base 0x10040100) is NOT ported;
  the GTDT counter bases stay as placeholders.

## Decision log

### PPTT cluster level = the three efficiency classes, not the two MPIDR clusters
Rule 1 says "PPTT levels/handles and cache entries follow the three efficiency classes."
The PPTT type-0 node has no MPIDR field — MPIDR lives only in the MADT. The PPTT joins to
the MADT by `AcpiProcessorId == AcpiProcessorUid` (leaf N ↔ GICC N), so the cluster
grouping is a policy. Grouping by efficiency class is the ONLY grouping where the
`IDENTICAL_IMPLEMENTATION` (BIT4) flag is honest: cluster 0 (MPIDR Aff1=0) mixes ananke+prometheus
so it is NOT internally identical, whereas each efficiency class (4 A55 / 2 prometheus / 2 cheetah)
IS. So the PPTT models package → 3 efficiency-class clusters → 8 cores. This is consistent with
the MADT via the UID join and the EFF_LITTLE/MID/BIG field; it deliberately diverges from the
MADT's 2 MPIDR affinity clusters, which is a separate concern (PSCI/OS internals).

### No ACPI type-1 cache nodes added
The 9810 PPTT ships only type-0 nodes. No cache geometry (size/associativity/line size) has been
read on the S10e anywhere in the tree, so adding type-1 cache structures would be pure fabrication
(frown of rule 2). "cache entries follow the three efficiency classes" is read as "the per-node
entries follow the three efficiency classes" = the three cluster nodes. Recorded as a dead end:
searched `Exynos9820.h`, `Exynos9820Pkg.dec`, `notes/*` for cache/L1/L2 geometry — none present.

## Files created

### AcpiTables.inf — created
Ported from `Star2LtePkg/AcpiTables/AcpiTables.inf`. Changes vs 9810:
- Header comment retargeted to Beyond0LtePkg / Exynos 9820 / S10e.
- `FILE_GUID` changed `...555555555501` → `...555555555521` (must differ from the 9810's and from
  the package GUID `...5520`; it is a build identity, not a read — flagged in a comment that the
  future `Beyond0LteAcpiPlatformDxe` must match it).
- `[Sources]` comments updated: MADT node is cortex-a15/a9-gic not arm,gic-400 (VERIFIED,
  `Exynos9820.h:125-146`); PPTT is "3 efficiency-class clusters"; FADT/GTDT flagged UNVERIFIED.
- `[Packages]` `Silicon/Exynos9810Pkg/Exynos9810Pkg.dec` → `Silicon/Exynos9820Pkg/Exynos9820Pkg.dec`
  (exists: `firmware/Silicon/Exynos9820Pkg/Exynos9820Pkg.dec`).
- Kept the 9810's TODO lines (Dbg2, Spcr) and the `[BuildOptions]` stub verbatim.

### Fadt.aslc — created
Ported from `Star2LtePkg/AcpiTables/Fadt.aslc`. Changes vs 9810:
- Includes `Beyond0LteAcpiHeader.h` and `<Platform/Exynos9820.h>` (the 9810 FADT hardcoded the
  PMU address; here the verified base `EXYNOS_PMU_BASE` is used symbolically so the read vs the
  guess is visible).
- `STAR2LTE_PMU_SWRESET_ADDRESS 0x14060400` → `BEYOND0LTE_PMU_SWRESET_ADDRESS (EXYNOS_PMU_BASE + 0x400)`.
  Base 0x15860000 is a VERIFIED read (`Exynos9820.h:185`); the +0x400 SWRESET offset is the 9810's,
  NOT read on the S10e → **UNVERIFIED**.
- Header comment retargeted to 9820; the "confirm PSCI via EL3 monitor" warning kept and made
  explicit that the DT's `enable-method = "psci"` is the Android/kernel view, not proof of our
  monitor's PSCI.
- UNVERIFIED markers added on: RESET_REG_SUP flag, ResetValue (=1, 9810 value), ArmBootArch
  PSCI_COMPLIANT. PreferredPmProfile TABLET kept as a policy declaration (not a hardware read).
- All the zeroed PM-block fields, NULL_GAS registers, reserved bytes kept verbatim.

### Gtdt.aslc — created
Ported from `Star2LtePkg/AcpiTables/Gtdt.aslc`. Changes vs 9810:
- Includes `Beyond0LteAcpiHeader.h`.
- The four timer GSIVs (29/30/27/26) kept as architectural defaults but each marked **UNVERIFIED**
  — they are not read against the 9820 DT timer node, and `Exynos9820.h` carries no timer PPIs.
- Header comment rewritten: the 9810 install-time counter-frame patch (26 MHz frame + MCT read base
  0x10040100) is described as NOT established on the S10e — `EXYNOS_ARCH_TIMER_FREQ_HZ` is UNVERIFIED
  (`Exynos9820.h:169`), the MCT address was never read here, and there is no `Beyond0LteAcpiPlatformDxe`
  to apply it. The all-ones `GTDT_CNT_CONTROL_BASE`/`GTDT_CNT_READ_BASE` placeholders stay.
- `GTDT_GTIMER_FLAGS`, `GTDT_TIMER_DISABLED_FLAGS`, PlatformTimerCount/Offset kept verbatim.

### Pptt.aslc — created
Ported from `Star2LtePkg/AcpiTables/Pptt.aslc`. Changes vs 9810:
- `STAR2LTE_*` → `BEYOND0LTE_*`; `Cluster[2]` → `Cluster[3]` (`BEYOND0LTE_NUM_CLUSTERS 3`), cores stay 8.
- **Cluster level = the three efficiency classes** (LITTLE 4 / MID 2 / BIG 2), NOT the two MPIDR
  clusters. Justification recorded in the file header: the PPTT joins to the MADT by
  `AcpiProcessorId == AcpiProcessorUid` (leaf N ↔ GICC N), which is independent of intermediate
  grouping; and IDENTICAL_IMPLEMENTATION (BIT4) is only honest per efficiency class, because MPIDR
  cluster 0 mixes ananke+prometheus. This is the crux of rule 1 ("PPTT ... follow the three
  efficiency classes").
- Leaf order/UIDs match `Madt.aslc` exactly: LITTLE UID 0–3, MID UID 4–5, BIG UID 6–7.
- IDENTICAL_IMPLEMENTATION set on each of the 3 clusters; package still not flagged identical.
- `PPTT_REVISION 0x03` kept from 9810 but marked **UNVERIFIED** (not read on this SoC).
- No type-1 cache nodes added — no cache geometry read on the S10e (dead end recorded above).
- OFFSET_OF/PPTT_OFF_CLUSTER mechanism kept verbatim (works for 3 clusters).

## Constants left UNVERIFIED (summary for the report)
- FADT: `BEYOND0LTE_PMU_SWRESET_ADDRESS` +0x400 offset (base 0x15860000 IS a read); RESET_REG_SUP
  flag; ResetValue (=1); ArmBootArch PSCI_COMPLIANT.
- GTDT: the four timer GSIVs 29/30/27/26 (architectural, not read on S10e); the counter-frame
  frequency (26 MHz) and MCT read base (0x10040100) are 9810 values and there is no 9820 patching
  driver — bases left as all-ones placeholders.
- PPTT: `PPTT_REVISION 0x03` (9810 value).
- AcpiTables.inf: `FILE_GUID` is a build identity (must match the future installer), not a read.

## Sources (local evidence actually consulted)
- `firmware/Platform/Star2LtePkg/AcpiTables/{AcpiTables.inf,Fadt.aslc,Gtdt.aslc,Pptt.aslc}` — the 9810 originals being ported.
- `firmware/Platform/Beyond0LtePkg/AcpiTables/{Madt.aslc,Beyond0LteAcpiHeader.h,Dsdt.asl}` — already-ported 9820 files (topology authority + naming).
- `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h` — PMU base (VERIFIED), timer freq (UNVERIFIED), GIC windows (VERIFIED).
- `firmware/Silicon/Exynos9820Pkg/Exynos9820Pkg.dec` — `PcdCoreCount 8`, `PcdClusterCount 2`.
- `notes/dock-desktop.md` §15/§16 — CPU topology (2 MPIDR clusters 6+2, 3 efficiency classes).
- `docs/exynos9820-port/MILESTONES.md` — M8 task list, MADT/DSDT port notes.
- `firmware/Platform/Star2LtePkg/Drivers/Star2LteAcpiPlatformDxe/Star2LteAcpiPlatformDxe.c` — the 9810 GTDT install-time counter patch (26 MHz, MCT 0x10040100).
