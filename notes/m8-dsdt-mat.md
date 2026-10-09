# M8 — the DSDT CPU sub-device `_MAT` buffers: what the bytes actually say

Task: the §5 hazard "DSDT CPU sub-device buffers — the 9810's `_MAT` encoding
cannot describe 6+2 clusters". Before re-deriving anything, establish
mechanically what the current buffers contain.

## Step 1 — the buffers, dumped (not read by eye)

Script over both `Dsdt.asl` files: every `_MAT` buffer is 80 bytes
(`Buffer (0x50)`), and **the 9820's eight buffers are byte-identical to the
9810's**. So the port carried them unchanged; nothing was retargeted.

Bytes that differ across the eight CPUs (all other offsets are constant):

| Offset | CPU0 | CPU1 | CPU2 | CPU3 | CPU4 | CPU5 | CPU6 | CPU7 |
|---|---|---|---|---|---|---|---|---|
| 4 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| 8 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| 68 | 0 | 1 | 2 | 3 | **0** | **1** | **2** | **3** |
| 69 | 0 | 0 | 0 | 0 | **1** | **1** | **1** | **1** |
| 76 | **1** | **1** | **1** | **1** | **0** | **0** | **0** | **0** |

Constant bytes worth noting: `0x0B, 0x50` at 0/1 (a type byte and the buffer
length), `0x01,0,0,0` at 12, `0x20,0x10,0x10` at 33-35, `0x60,0x10,0x10` at
41-43, `0x40,0x10,0x10` at 49-51, `0x19` at 56.

## Step 2 — what that proves

* The 9810 encodes **two groups of four**: offset 68 is the core index inside a
  group (0..3), offset 69 is the group number (0 for CPU0-3, 1 for CPU4-7).
* Offset 76 is a per-group flag (1 for group 0, 0 for group 1) — its meaning is
  **not established**.
* The 9820's topology is **not** 4+4: the MADT carries two MPIDR clusters sized
  **6+2** (`cpu@0000`–`cpu@0005`, `cpu@0100`–`cpu@0101`), and the PPTT models
  **three efficiency classes 4+2+2** (LITTLE 4 / MID 2 / BIG 2, the decision
  recorded in `notes/m8-acpi.md`). A 4+4 split matches neither, so the buffers
  are wrong for this SoC — confirmed by the bytes, not by the comment.

## Step 3 — the DSDT's own comment is wrong about its own bytes

`Dsdt.asl:34-42` claims the "cluster/core field" reads
`0x01,0x00 / 0x01,0x00 / 0x02,0x00 / 0x03,0x00` for CPU0-3. The actual varying
fields are at offsets 68/69 (core index, group) and the values are
`0,0 / 1,0 / 2,0 / 3,0`. The comment's pairs do not appear anywhere in the
buffers. Flagged; the comment gets corrected in its own commit.

## Step 4 — what is needed to close the hazard, and what blocks it

Re-deriving the buffers needs the **ACPI Processor Sub-Device spec** (the
`_MAT` = "Processor Group Membership And Topology" structure). It is **not in
this checkout**: `rg -i "subdevice|sub_device"` over `firmware/edk2` finds only
an unrelated PCI file, and `developer.arm.com/documentation/den0101/` returns
403 from here. Without the field map, choosing bytes at offsets 68/69/76 for a
4+2+2 or 6+2 grouping would be a guess — the repo's rule forbids that.

**Blocked on:** fetching the spec (try a mirror of Den0101 / the ARM
"Processor Sub-Device" document, or Mu-Silicium's `SiliconPkg` DSDT which
implements it). Not blocked on the phone — the MADT already carries the
topology, so no MPIDR read is needed to decide the grouping (the earlier
suggestion of an on-device MPIDR read is superseded by
`notes/dock-desktop.md` §16 / `Madt.aslc`).

## Step 5 — RESOLVED (the gap closed without the Arm spec)

The field map was already in this checkout, and the `_MAT` semantics were
provable from the 9810 itself — see `notes/claude-mat-layout.md` for the
browser-model lead and the verification against primary sources:

* `EFI_ACPI_6_3_GIC_STRUCTURE` (`firmware/edk2/MdePkg/Include/IndustryStandard/Acpi63.h:520-539`)
  is 80 bytes wide, MPIDR at 68, `ProcessorPowerEfficiencyClass` at 76 — the
  only layout that reproduces every byte of the 9810 buffers, including the
  GICC/GICH/GICV addresses at 32/48/40 and the VGIC GSIV 25 at 56.
* The 9810 DSDT buffers equal its own MADT GICC entries field for field
  (`Star2LtePkg/AcpiTables/Madt.aslc:133-141`), so `_MAT` = "the MADT GICC entry
  with the matching `_UID`" is observed behaviour, not an assumption.
* Therefore the 9820 buffers are a mechanical mirror of `Madt.aslc:149-163`.

Rewritten by script (`%TEMP%\opencode\rewrite_mat.py`, table keyed by UID, not
by line position), then verified in the **built image**, not in the source:

```
Build\...\FV\Ffs\11111111-...5521AcpiTables\...SEC1.3.raw  (DSDT, 712 B)  8 hits
  uid 0 mpidr 0x0   eff 2      uid 4 mpidr 0x4   eff 1
  uid 1 mpidr 0x1   eff 2      uid 5 mpidr 0x5   eff 1
  uid 2 mpidr 0x2   eff 2      uid 6 mpidr 0x100 eff 0
  uid 3 mpidr 0x3   eff 2      uid 7 mpidr 0x101 eff 0
Build\...\same dir\...SEC2.1.raw  (MADT, 1417 B)           8 hits, same values
```

DSDT and MADT now agree in the shipped FV. Build: FVMAIN_COMPACT 99% full,
696560 used / 3856 free (inside the known ±24 B drift band), FVMAIN 1159016
used, FD 7340032 B.

## Residual, still open

* Nothing is device-validated: Windows parsing eight ACPI0007 sub-devices with
  these buffers is untested on the S10e.
* `_MAT` Flags byte (offset 12) stays 1 = ENABLED on all eight even in a
  `BEYOND0LTE_UNIPROCESSOR_BOOT` build, where the MADT disables the seven
  secondaries — recorded in the DSDT header; only reachable in that isolation
  configuration.
* The Arm document that defines `_MAT` for a processor sub-device is still not
  in the checkout (the browser model says the ACPI spec itself, and points at
  **DEN0093 "ACPI for Arm Components"** as the Arm-side document — neither was
  opened from here).

* `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl` and
  `firmware/Platform/Star2LtePkg/AcpiTables/Dsdt.asl` — byte dump via script.
* `notes/m8-acpi.md` — the PPTT 3-efficiency-class decision and the MADT 6+2
  MPIDR clusters.
* `rg` over `firmware/edk2` for the sub-device spec — absent.
* `https://developer.arm.com/documentation/den0101/` — 403 from this network.
