# Claude (via CDP) on the `_MAT` layout — AI answer, NOT yet verified

Asked through the logged-in Chrome tab (`claude.ai/chat/53802365-…`, "ACPI
_MAT buffer byte layout"), driven by `%TEMP%\opencode\claude_ask.py`
(`Input.insertText` + `element.click()` on `[data-testid="chat-input-send"]`;
CDP `Input.dispatchMouseEvent` and Enter-key attempts did **not** submit — the
JS click did). Page text captured to `%TEMP%\opencode\claude-page.txt`.

## The claim (mark: AI inference, unverified)

1. There is **no** "Processor Group Membership And Topology" structure in the
   ACPI spec — `_MAT` on a processor sub-device **returns a MADT entry**, and
   on Arm that entry is the **GICC** (Generic Processor Context, MADT type
   `0x0B`), length **82** in ACPI 6.5, all little-endian.
2. Field table it gave (offsets/widths):

   | off | size | field |
   |---|---|---|
   | 0 | 1 | Type = `0x0B` |
   | 1 | 1 | Length = `0x52` (82) |
   | 2 | 2 | Reserved |
   | 4 | 4 | CPU Interface Number |
   | 8 | 4 | ACPI Processor UID (matches `_UID`) |
   | 12 | 4 | Flags (bit0 Enabled, bit1 Perf IRQ mode, bit2 VGIC IRQ mode, bit3 Online Capable) |
   | 16 | 4 | Parking Protocol Version |
   | 20 | 4 | Performance Interrupt GSIV |
   | 24 | 8 | Parked Address |
   | 32 | 8 | Physical Base Address |
   | 40 | 8 | GICV |
   | 48 | 8 | GICH |
   | 56 | 4 | VGIC Maintenance Interrupt |
   | 60 | 8 | GICR Base Address |
   | 68 | 8 | **MPIDR** |
   | 76 | 1 | **Processor Power Efficiency Class** (lower = more efficient) |
   | 77 | 1 | Reserved |
   | 78 | 2 | SPE Overflow Interrupt |
   | 80 | 2 | TRBE Interrupt (ACPI 6.5+) |

3. Cluster topology is **not** in `_MAT` — it is **PPTT** (Processor Hierarchy
   Node type 0), and efficiency class is the single byte at GICC offset 76.

## Why this is a strong lead, not a guess

It lines up **exactly** with the mechanical byte dump in
`notes/m8-dsdt-mat.md`: the only bytes that vary across the eight 9810 buffers
are at 4/8 (UID), 68, 69 and 76. Under this table those are CPU-interface +
UID, MPIDR byte 0 (core index within the cluster), MPIDR byte 1 (cluster
group), and the efficiency-class byte. A layout invented to fit four offsets
would not have predicted the MPIDR/efficiency-class pairing at 68/76.

## Still open after verification

* The **semantics** claim — that a processor sub-device's `_MAT` must be the
  MADT entry for the same device — is supported by the 9810 byte equality above,
  but not yet by the Arm Processor Sub-Device document itself (still not in the
  checkout; DEN0101 is ADAC, see `notes/info-den0101-adac.md`). A follow-up
  question to Claude asked which document number defines it.
* Nothing here is device-validated: the rewrite is byte-correct against the
  MADT, but Windows accepting the DSDT sub-devices is untested.

## Verification — the layout is NOT from the AI, it is in this checkout

Claude's table was checked against primary sources before being used:

* `firmware/edk2/MdePkg/Include/IndustryStandard/Acpi63.h:520-539` —
  `EFI_ACPI_6_3_GIC_STRUCTURE` field order/widths: `Type,Length,Reserved,
  CPUInterfaceNumber,AcpiProcessorUid,Flags,ParkingProtocolVersion,
  PerformanceInterruptGsiv,ParkedAddress,PhysicalBaseAddress,GICV,GICH,
  VGICMaintenanceInterrupt,GICRBaseAddress,MPIDR,ProcessorPowerEfficiencyClass,
  Reserved2,SpeOverflowInterrupt`. Summing the widths gives **80 = 0x50**, i.e.
  the 6.3 shape **without** the ACPI 6.5 TRBE pair — exactly the `Buffer (0x50)`
  the DSDT emits and the `sizeof (EFI_ACPI_6_3_GIC_STRUCTURE)` the MADT uses.
  So the AI's 82-byte variant is the 6.5 form; this platform is on 6.3.
* Decoding the eight 9810 buffers with that table reproduces every byte:
  `0x0B,0x50` type/length, `@4` CPUInterfaceNumber = UID, `@8` UID, `@12`
  Flags = 1 (ENABLED), `@33-35` = `0x20,0x10,0x10` → PhysicalBaseAddress
  `0x10102000` = GICC, `@41-43` → GICV `0x10106000`, `@49-51` → GICH
  `0x10104000`, `@56` = `0x19` = 25 = VGIC maintenance GSIV, `@68..75` = MPIDR,
  `@76` = efficiency class.
* `firmware/Platform/Star2LtePkg/AcpiTables/Madt.aslc:133-141` — the 9810 MADT
  GICC entries are `MPIDR_CORE(0,0..3)` with `EFF_LITTLE`=1 and
  `MPIDR_CORE(1,0..3)` with `EFF_BIG`=0, i.e. MPIDRs `0x00..0x03` /
  `0x100..0x103` and class `1` / `0` — **byte-for-byte the same values the DSDT
  `_MAT` buffers carry at 68 and 76**. That is the mechanical proof that
  `_MAT` = the MADT GICC entry of the same sub-device, and it does not rely on
  the AI at all.

## Consequence — the hazard is closable now, no external spec needed

The 9820 MADT (`Madt.aslc:149-163`) already carries the authoritative per-core
values, so the DSDT buffers are a mechanical rewrite:

| CPU | MPIDR `@68..75` | Eff. class `@76` |
|---|---|---|
| 0-3 | `0x00`…`0x03` | `EFF_LITTLE` = 2 |
| 4-5 | `0x04`,`0x05` | `EFF_MID` = 1 |
| 6-7 | `0x100`,`0x101` | `EFF_BIG` = 0 |

GIC addresses need no change: `Exynos9820.h:139-146` marks GICC/GICH/GICV
VERIFIED **identical to the 9810**.

## Sources

* `claude.ai/chat/53802365-c15b-439c-8c77-e149dfdde392` — the answer above
  (AI-generated; used as a lead only).
* `firmware/edk2/MdePkg/Include/IndustryStandard/Acpi63.h:520-539` — the field
  table (primary).
* `firmware/Platform/Star2LtePkg/AcpiTables/Madt.aslc:97-141` — 9810 MADT↔DSDT
  equality (primary).
* `firmware/Platform/Beyond0LtePkg/AcpiTables/Madt.aslc:64-163` — 9820 MPIDRs +
  three efficiency classes.
* `firmware/Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h:125-146` — GIC
  windows VERIFIED identical.
