# TH02 — read the TMU state on the debug serial (zero writes)

Status: open — **needs-boot** (the FD must carry `Beyond0LteThermalDxe`; that module is **not
written yet** — this task is queued behind the firmware-half task).

## Goal

Close D11 rung **R1** and risk **P1**: establish what the vendor's own thermal engine left behind
when our UEFI runs — is the TMU's trip engine armed, are thresholds programmed, what do the raw
sensor codes read — **without writing a single register**. This is the measurement that answers
"what protects the phone if no thermal driver runs under Windows?".

## Done when

The debug serial shows one line per TMU window:

```
[TMU] R1: top con=0x… thd0=0x… pend=0x… code0=0x… -> NN C
[TMU] R1: sub con=0x… thd0=0x… pend=0x… code0=0x… -> NN C
```

and the raw values are pasted into `notes/thermal.md` under a dated heading.

## Do this

Nothing new to flash if the FD already carries the slice — check the build's FFS list for
`…554CBeyond0LteThermalDxe` first. Then capture serial from the sboot handoff onward and grep:

```
grep -E '\[TMU\]' <serial log>
```

The module reads, at each window: `CONTROL (0x50)`, `THD0 (0xD0)`, `INTPEND0 (0xF8)`,
`CUR_TEMP0 (0x84)` and `TRIMINFO(0..4)`, and prints the calibrated °C using
`°C = code − (TRIMINFO(n) & 0x1FF or 55) + 25`.

## What it proves / if it fails

| Output | Meaning | Next step |
|---|---|---|
| `con` has **bit 12 set** (`TRIP_EN`) and `pend` is 0 | the TMU trip engine **is armed** by sboot/ACPM and nothing has fired | the hardware side is alive; our job is only to report temperature to Windows |
| `con` bit 12 clear | nothing is armed in our boot state — the SoC is running with no thermal countermeasure at all | `_CRT`/`_HOT` in the DSDT become the only hard stop; raise D11's priority |
| `code0 = 0x000` or `0x1FF` | the sensor is not converting, or the window is not readable in our state | check the memory-map entries (`TMU_TOP`/`TMU_SUB`) before believing any number |
| the printed °C is wildly off (e.g. −40 or 300) | the calibration branch is wrong (two-point trimming, or `temp_error1` reads 0) | compare against `tasks/TH01-thermal-live-reads.md` — that is rung R2 |
| **hang/reset right after the `[TMU]` line** | reading `0x10090000` faults in our stage-2/clock state | stop; treat as a boot hazard, bisect with the read compiled out |

## Known

- Windows, sizes, GSIVs and every register offset are VERIFIED — `Exynos9820.h`, "Thermal (D11)"
  block, sourced in `notes/thermal.md` T1/T2.
- The vendor's own debugfs reads temperatures exactly this way (`exynos_tmu.c:1218-1224`), so the
  read itself is not exotic.
- The ACPM owns the sensors (IPC channel 9); this module deliberately **never** touches the ACPM.

## Next actions

1. Paste both lines verbatim into `notes/thermal.md`.
2. If `TRIP_EN` is clear: the DSDT thermal-zone work moves ahead of everything else in D11.
3. Then R2: compare the printed °C with the Android reading from `TH01`.

## Rollback / Do NOT

Read-only module; a reboot returns everything. **No deliberate over-temperature test, ever** — the
hard-stop rung (R5) is verified with an injected value, not with heat. No media writes, nothing
under `/dev/block/**` touched. Safe on the only device.
