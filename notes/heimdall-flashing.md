# heimdall — the CLI path to flash the S10e (Odin's open-source equivalent)

Started: 2026-10-10. Research task, no device writes. Phone is being restored to
stock Android by the user; nothing was flashed from here.

## Progress log

### H01 — the tool and its versions

- Canonical repo: https://github.com/Benjamin-Dobell/Heimdall (MIT). README says
  it speaks "the custom Samsung-developed protocol typically referred to as the
  'Odin 3 protocol'" to the device's download-mode stub ("Loke"), over libusb.
  Source: https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/README.md
- Upstream's last release is **1.4.x**, but the repo's own issue thread shows a
  **v2.1.0** build in use: "Heimdall v2.1.0 … Copyright (c) 2010-2017 Benjamin
  Dobell … Copyright (c) 2021-2024 **Henrik Grimler**" — i.e. the maintained
  fork is **Grimler's** (https://git.sr.ht/~grimler/Heimdall, mirrored on GitHub;
  the `amo13/Heimdall` fork is "the original repository with a few crucial pull
  requests merged"). Source: https://github.com/Benjamin-Dobell/Heimdall/issues/547
- **FACT: v2.1.0 works on modern Samsungs.** Issue #547 is a 2021-era device
  (`A52Q_EUR_OPEN.pit`) where the run got to "Session begun" and uploaded every
  partition; the failure there was only `--repartition` ("Failed to confirm end
  of PIT file transfer"), and the reporter says "Removing `--repartition` works".
  Source: same issue.
- **Counter-evidence (newer device):** issue #548 on `SM-A146U` (A14 5G, 2025)
  fails at "Uploading boot … ERROR: Failed to confirm transfer initialisation!".
  Source: https://github.com/Benjamin-Dobell/Heimdall/issues/548
- **INFERENCE (mine):** the S10e (2019, Exynos 9820) sits between those two data
  points. Untested here. The cheap test is `heimdall detect` and
  `heimdall print-pit` — both **read-only** (they download the device's PIT,
  they write nothing).

### H02 — the CLI shape we need

From the user guide (https://github.com/jeffjose/heimdall-userguide) and the
Win32 README:

1. `adb reboot download` (or long-press VolDown+Power+Home) → download mode.
2. `heimdall detect` → "Device detected".
3. `heimdall print-pit` → the partition names and sizes **as the device
   declares them**. The guide's sample shows entries named `BOOT`
   (`Partition Block Count: 20480`) and `RECOVERY` (`20480`), with flash
   filenames `boot.img` / `recovery.img`.
4. `heimdall flash --RECOVERY <file> --no-reboot` — one partition, no
   repartition (repartitioning **wipes the device**, per the Win32 README).

### H03 — the two hazards this tool has, both relevant to our image

- **No size check.** Open PR #555 states the defect plainly: "Heimdall currently
  allows flashing files larger than the target partition with no warning. On
  some devices this silently overwrites adjacent partitions."
  Source: https://github.com/Benjamin-Dobell/Heimdall/pull/555
  Our candidate is **18 255 872 B** into a recovery slot the repo measured at
  **55 279 616 B** (`notes/s10e-partitions.md`, `notes/K10-rkp.md` T18) — a
  3.0× margin, so we are on the safe side of that defect, but the check must be
  done by hand against `print-pit` output, not assumed.
- **Block-size ambiguity.** The same PR notes the PIT's
  `blockSizeOrOffset` field is ambiguous (offset or size depending on the Loke
  version), so `print-pit` numbers must be cross-checked against the GPT reads
  already in `notes/s10e-root-reads.md` §2/§8 before any flash.

### H04 — dead ends

- grep.app `q="--dingo"` → 2 hits, both unrelated (glibc test, RabbitMQ
  autocomplete). There is **no** `--dingo` flag in heimdall; that name came from
  memory, not from a source. Do not re-search it.
- `heimdall/source/Main.cpp` → 404 (no such file); the option table lives
  elsewhere in `heimdall/source/`. Not needed for the CLI shape above, which
  came from the README and the user guide instead.

## Next read (when the phone is back and idle)

`heimdall detect` then `heimdall print-pit` — read-only, and the PIT output is
what closes the "which slot, how big" question for a `--RECOVERY` flash.

## Sources

- https://github.com/Benjamin-Dobell/Heimdall
- https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/README.md
- https://raw.githubusercontent.com/Benjamin-Dobell/Heimdall/master/Win32/README.txt
- https://github.com/Benjamin-Dobell/Heimdall/issues/547 (v2.1.0, Grimler fork, A52Q)
- https://github.com/Benjamin-Dobell/Heimdall/issues/548 (A14 5G upload failure)
- https://github.com/Benjamin-Dobell/Heimdall/pull/555 (no partition-size check)
- https://github.com/jeffjose/heimdall-userguide
- https://github.com/amo13/Heimdall (fork with crucial PRs merged)
- https://git.sr.ht/~grimler/Heimdall (named in the v2.1.0 banner and PR #555)
