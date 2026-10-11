# PSCI + CPU parking for Windows-on-ARM — what the open WoA projects do, and what Samsung's monitor speaks

Task (user, 2026-10-11): find open-source WoA projects that use a **CPU
parking protocol** (WOA-Project Lumia work, Renegade Project firmware), and
collect Exynos/mainline notes on **PSCI 0.1 on Samsung parts**. Feeds D7
(the 9820 CPU driver) and the MADT/core-online path (M1 bar: "Windows sees
CPU cores").

Findings appended as discovered; facts cite a URL, *(inference)* is mine.

## P0. The shipped 9820 monitor, from our own dump (anchor fact)

FACT (`dumps/hhxj6/base.dts`, `/psci` node of the G970FXXUHHXJ6 base DTB):

```
psci {
    compatible = "arm,psci";        /* the bare 0.1 binding */
    method = "smc";
    cpu_suspend = <0xc4000001>;     /* PSCI 0.2 FN64 id */
    cpu_off    = <0x84000002>;      /* PSCI 0.2 id  (0.1 used 0x84000001) */
    cpu_on     = <0xc4000003>;      /* PSCI 0.2 FN64 id (0.1 used 0x84000002) */
};
```

All 8 cpu nodes: `enable-method = "psci"` (F1 in `notes/cpu-driver-9820.md`).
Idle: `entry-method = "arm,psci"`, suspend-param `0x10000` (Standby).

*(inference)* Samsung's EL3 monitor implements **0.2-era function IDs** but
is advertised through the legacy 0.1 DT binding (which lets the DT supply
arbitrary IDs, so Linux is happy either way). Unknown from the dump alone:
whether the monitor answers `PSCI_VERSION` (0x84000000), `AFFINITY_INFO`,
`PSCI_FEATURES`, or SYSTEM_OFF/RESET — i.e. whether it is really 0.2/1.0
or just the three legacy functions with new IDs. That matters because
Windows discovers/uses PSCI differently from Linux (no DT on ACPI systems).

## Q5 answered locally: what this repo's UEFI already does (9810 lineage → 9820)

FACT (`firmware/Platform/Beyond0LtePkg/AcpiTables/Madt.aslc:60-110`):
- GICC entries carry `EFI_ACPI_6_3_GIC_ENABLED` for all secondaries by
  default (`BEYOND0LTE_UNIPROCESSOR_BOOT 0`), with isolation knobs
  (uniprocessor / LITTLE-only / MID-only builds) explicitly designed to
  "separate a PSCI bring-up fault from everything else" — the 9810 flow
  already anticipated exactly this experiment.
- `notes/m8-acpi.md`: the GICC `ArmBootArchFlags` = **PSCI_COMPLIANT**
  (SMC conduit) is set — that is how Windows is told to use PSCI at all
  (ACPI has no DT; the GICC arm-boot-arch flags are the discovery path).
- `notes/m8-reset-lib.md`: `Beyond0LteResetSystemLib` speaks PSCI via
  `ArmMonitorCall`: `PSCI_SYSTEM_RESET`, `PSCI_SYSTEM_RESET2` **gated by a
  `PSCI_FEATURES` probe**, `PSCI_SYSTEM_OFF` — i.e. the UEFI itself
  already probes features like a 0.2+ caller. Whether the Samsung monitor
  answers them is recorded as unverified (reset not yet device-proven).
- `ArmPkg/Library/ArmMonitorLib` is in the DSC (DSC:170) — the EL2 monitor
  plumbing exists on our image.

*(inference)* So on the 9820 the intended Windows core-online path is:
MADT GICC PSCI_COMPLIANT + ENABLED → Windows issues SMC PSCI (0.2/1.0
IDs, likely probing PSCI_VERSION first) → Samsung EL3 monitor. P0 shows
the monitor definitely answers CPU_ON/OFF/SUSPEND with 0.2 IDs. The open
risk is exactly what the WoA projects hit on Qualcomm parts whose EL3
lacks PSCI — hence the parking-protocol research (section A) and the
version/feature question (section B).

## P1. ACPI-side discovery, read from the edk2 headers + our own tables

FACT (`firmware/edk2/MdePkg/Include/IndustryStandard/Acpi63.h`):
- **PSCI is advertised in the FADT**, not the MADT: field `ArmBootArch`
  (Acpi63.h:176) with `EFI_ACPI_6_3_ARM_PSCI_COMPLIANT` (BIT0) and
  `EFI_ACPI_6_3_ARM_PSCI_USE_HVC` (BIT1) (Acpi63.h:227-228).
  Both `Star2LtePkg/Fadt.aslc:75` (9810) and `Beyond0LtePkg/Fadt.aslc:95`
  (9820) set PSCI_COMPLIANT; the 9820 comment marks it "UNVERIFIED on the
  S10e — our EL3 monitor's PSCI compliance is unconfirmed."
- **The parking protocol lives in the MADT GICC entry**:
  `EFI_ACPI_6_3_GIC_STRUCTURE` (Acpi63.h:519-539) carries
  `ParkingProtocolVersion` (u32) and `ParkedAddress` (u64) — the legacy
  ARM CPU-parking fields — plus `Flags` BIT0 `GIC_ENABLED` (online
  capable). Our `BEYOND0LTE_GICC_INIT` zeroes both parking fields
  (`(Flags), 0, (PmuIrq), 0,` = Flags, ParkingProtocolVersion=0,
  PerformanceInterruptGsiv, ParkedAddress=0) — i.e. the shipped tables
  say "no parking, use PSCI".
- Same shape in ACPI 6.4 headers (Acpi64.h:522-541) — the parking fields
  survive but are legacy.

*(inference)* Two distinct "parking protocol" worlds will show up in the
research: (1) the ACPI/ARM parking protocol these GICC fields describe —
firmware parks secondaries at `ParkedAddress`, the OS releases them via a
mailbox (what Windows RT / early-WoA Qualcomms used, and what a fallback
for us would look like if Samsung's monitor turns out PSCI-incomplete);
(2) project-specific spin-loop parking inside UEFI (MpServices-style),
which is a boot-time detail invisible to Windows. The notes must keep
them apart.
