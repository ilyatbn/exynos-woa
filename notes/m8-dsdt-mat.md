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

## Sources (local evidence actually consulted)

* `firmware/Platform/Beyond0LtePkg/AcpiTables/Dsdt.asl` and
  `firmware/Platform/Star2LtePkg/AcpiTables/Dsdt.asl` — byte dump via script.
* `notes/m8-acpi.md` — the PPTT 3-efficiency-class decision and the MADT 6+2
  MPIDR clusters.
* `rg` over `firmware/edk2` for the sub-device spec — absent.
* `https://developer.arm.com/documentation/den0101/` — 403 from this network.
