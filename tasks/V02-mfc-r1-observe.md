# V02 — read the MFC identity line on the debug serial (zero writes)

Status: open — **needs-boot** (the FD must carry `Beyond0LteMfcDxe`; that module is **not written
yet** — this task is queued behind the firmware-half task, not behind the device).

## Goal

Close D10 rung **R1** and risk **P1**: prove the MFC's SFR is reachable in our boot state with the
clock off, exactly as the vendor's own comment claims (`mfc_pm.c:48-52`), and read the IP version
without writing a single register.

## Done when

The debug serial shows:

```
[MFC] R1: version=0x1300
```

and the raw value is pasted into `notes/video-mfc.md` under a dated heading.

## Do this

Nothing new to flash if the current FD already carries the slice — check the build's FFS list for
`…554BBeyond0LteMfcDxe` first. Then capture serial from the sboot handoff onward and grep:

```
grep -E '\[MFC\]' <serial log>
```

## What it proves / if it fails

| Output | Meaning | Next step |
|---|---|---|
| `version=0x1300` | **R1 passes** — the block is present and readable in our boot state; `EXYNOS_MFC_IP_VER` becomes a device-confirmed value | build R2 (gate 422 on, re-read) |
| `version=0x00000000` or `0xFFFFFFFF` | the window is not mapped, the block is powered down, or the read is being held at the bus | check the memory-map entry (`MFC` in `PlatformMemoryMapLib`) and then the `pd-mfc` power domain (P2) before touching clocks |
| **hang or reset right after the `[MFC]` line appears** | reading `0x188D0000` faults in our stage-2/clock state — the one hazard this design calls out | bisect: build with the read compiled out, then with the window mapped but unread |
| no line at all | module not in the flashed FD, or serial too quiet | verify the FFS GUID in the built FD; check `DEBUG_INFO` reaches the console |

## Known

- The window and the register offset are VERIFIED: `0x188D0000 + 0xF028`
  (`base.dtb`, `mfc_regs.h:51`) — `Exynos9820.h`, "Video codec / MFC (D10)" block.
- The expected value `0x1300` is VERIFIED as a DT property (`ip_ver`) and as the mirror's dtsi
  value; the driver refuses to run on a mismatch (`mfc_run.c:110-116`).
- The module performs **zero MMIO writes** by construction (same gate as the D9 ModemDxe slice).

## Next actions

1. Paste the line verbatim into `notes/video-mfc.md`.
2. If R1 passes: the driver agent builds R2 (clock gate 422) — the first write in this subsystem.
3. If R1 fails on a hang: stop; this is a boot hazard, not a driver bug. Record it and move D10
   behind the power-domain question (P2).

## Rollback / Do NOT

Read-only module; a reboot returns everything. No media writes, no partition touching, no blob
is read by this slice. Safe on the only device.
