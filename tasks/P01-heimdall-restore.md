# P01 — heimdall auto-restore/reflash path (parking lot)

Status: **parked — off the boot path**

## Goal

When the device breaks completely, restore it from a shell instead of a GUI:
build/integrate **heimdall** so "flash the stock recovery/boot slot" and "flash
our candidate" are one command each, with the size check heimdall lacks.

## Established (see `notes/heimdall-flashing.md`)

- Canonical repo https://github.com/Benjamin-Dobell/Heimdall (MIT); the build in
  use is **v2.1.0 from Grimler's fork** (`git.sr.ht/~grimler/Heimdall`).
- Works on a 2021 device (issue #547 reached "Session begun" and uploaded
  partitions; only `--repartition` failed) and **fails** on an A14 5G (issue #548,
  "Failed to confirm transfer initialisation"). The S10e is untested between them.
- No partition-size validation upstream (PR #555) — our own check must wrap it.
- Not installed on this machine (not on PATH, not in `C:\Users\ilya\code`).

## Shape

`adb reboot download` → `heimdall detect` → `heimdall print-pit` (read-only) →
`heimdall flash --RECOVERY <file> --no-reboot`.

## Done when

A local build (or vendored binary) runs `detect`/`print-pit` against this phone
and a wrapper refuses any file larger than the PIT's block count.
