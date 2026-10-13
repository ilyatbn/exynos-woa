# R02 — read the CP/mailbox observation lines on the debug serial

Status: open — **needs-boot** (Windows/UEFI boots — satisfied, confirmed
2026-10-13; the driver agent runs this on the next FD flash)

## Goal

Close D9 risks P1/P2 and rung R1's pre-read + R2's observation half with the
`Beyond0LteModemDxe` slice **already in the FD** (FFS `11111111-2222-3333-4444-55555555554A`):
establish what state sboot leaves the integrated CP + mailbox in when our UEFI
runs. Zero writes by this module — safe on any boot.

## Done when

The debug serial shows both lines:

```
[CP] R1 pre: CONFIGURATION=<x> STATUS=<x> IN=<x>
[MBOX] R2: issr0..3=<x> <x> <x> <x> intsr0=<x>
```

and the raw values are pasted into `notes/modem-wwan.md` under a dated heading.

## Do this

Nothing new to flash if the current FD already carries the slice (check the
build's FFS list for `…554ABeyond0LteModemDxe`). Just capture serial from
sboot handoff onward and grep:

```
grep -E '\[CP\]|\[MBOX\]' <serial log>
```

## What it proves / if it fails

| Output | Meaning | Next step |
|---|---|---|
| `STATUS=0 IN=0`, issr all 0 | CP **off** at DXE entry (expected — Android boots it via kernel+cbd) | P2 closes "no pre-boot"; flip `BEYOND0LTE_MODEM_INIT=1` (rung R1 proper) in the next FD |
| `STATUS=1` or nonzero `issr3` | sboot **pre-boots** the CP / mailbox alive | P2 closes "yes" — sequencing shrinks; go straight to R3 (SBD link) |
| `[CP]` line, no `[MBOX]` line, boot continues | mailbox window read returned but block unresponsive? (reads are Device-mapped, no fault expected) | note values; cross-check `mcu_ipc` clock state at R3 |
| **hang/reset right after `[CP]` appears** | reading `0x15920000` faults in our stage-2/clock state — the one hazard the source review said was unlikely | gate investigation: read PMU-only build, bisect |
| no lines at all | module not in the flashed FD, or serial level too quiet | verify FFS GUID in the built FD; check `DEBUG_INFO` reaches the console |

## Known

- Module performs **zero MMIO writes** (AP2CP_CFG path compiled out,
  `BEYOND0LTE_MODEM_INIT 0`) — reviewed 2026-10-12.
- Vendor kernel reads this mailbox with **no clock enable at all** (`mcu_ipc.c`
  has no clk handling; DT node has no `clocks`) — design §2.
- ISSR slot map + `sbi_cp_status` field (0xf@1 in issr3): notes T1/T6.

## Next actions

1. Paste the two lines verbatim into `notes/modem-wwan.md`.
2. If CP off: driver agent flips `BEYOND0LTE_MODEM_INIT` to 1 in their FD build
   and re-runs — expect `[CP] R1 ap2cp_cfg readback=1` (that IS rung R1).
3. Then R3 slice (SBD `g_desc` create + readback at `0xFB200000`) — constants
   all VERIFIED, see design §2.

## Rollback / Do NOT

Read-only module; a reboot returns everything. No media writes, no partition
touching. This task is safe on the only device.
