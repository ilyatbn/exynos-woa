# App launch slowness (user 2026-10-19: "opening steam or brave (for the first time, sometimes later too) takes a lot
# of time ... closing and launching again feels faster ... slow reads from disk into memory? latency?")

## Progress log

- Tools: tools/phone/perf/launchtime.ps1 (user session: ms to first window, ms to app idle, disk MB/reads, app CPU),
  tools/phone/perf/diskbench.ps1 (uncached FILE_FLAG_NO_BUFFERING reads: sequential 1 MiB, random 4 KiB QD1).
- State: Defender real-time protection ON; 6 cores; 3.2 of 5.8 GiB free.
- **diskbench (03:45, uncached)**: edgehtml.dll seq 1 MiB = 61.3 MiB/s, rand 4 KiB QD1 = 3579 IOPS avg 0.276 ms
  p99 0.313 ms; mshtml.dll seq 62.2 MiB/s, rand 3269 IOPS avg 0.302 ms. => small-read latency is fine, LARGE reads are
  capped near 60 MiB/s (UFS 2.1 is capable of several hundred MiB/s). Cold app starts read hundreds of MB of DLLs.
- Installed UFS driver = w38 (4dab7b8f, notes/thermal-clocks.md:235) = w37 throughput fixes + watchdog-feed change.
  Prior UFS perf history: notes/m15-ufs-throughput.md (w37: 1 MiB 117 MB/s, 64 KiB 988 us; levers left: DMA into
  the SRB buffer instead of the bounce copy, >64 KiB transfers, queue depth > 1, interrupt completion).
- diskbench size sweep (uncached, QD1, Windows.UI.Xaml.dll): 4 KiB 331 us (11.8 MiB/s), 16 KiB 409 us, 64 KiB 630 us
  (99 MiB/s), 256 KiB 2.7 ms (91 MiB/s), 1 MiB 20 ms (50 MiB/s). => ~300 us fixed cost per command (UFS 4 KiB
  device time is well under 100 us) + per-byte cost; large requests get WORSE per 64 KiB chunk (1.25 ms in a 1 MiB
  request vs 0.63 ms alone) - open.
- Driver design (Exynos9820Ufs.c): MaxNumberOfIO = 1, one slot, StartIo builds the command, rings the doorbell and
  BUSY-POLLS the doorbell every 10 us (UfsPollForCompletion, StorPortStallExecution) until done, then copies the
  bounce into the SRB buffer -> every disk command spins one CPU at DISPATCH for its whole duration, fully serial.
- **launchtime.ps1 results** (window = first visible window, idle = app CPU < 3 % for 2 s; disk = system-wide):
  cold (after reboot, +90 s settle): explorer window 2.3 s / idle 8.4 s; brave window 6.6 s / idle 17.1 s
  (376 MB disk, app CPU 14 s); steam no window detected in 150 s (script then only matched steam.exe - fixed to
  include steamwebhelper), 599 MB / 23.8 k reads.
  warm: explorer 0.9 s / 5.1 s (0.6 MB); brave 1.2 s / 13.7 s and 1.6 s / 5.8 s (~1 MB disk, app CPU 7-8 s);
  (one "warm" brave right after the steam run: 6.1 s and 342 MB - disk activity from something else in that window).
- **iotop.ps1** (per-process ReadTransferCount + PerfDisk): idle 15 s = 0.1 MB. Warm brave 20 s: 2.7 MB physical.
  **Steam (warm!) 45 s: steam.exe 534.5 MB in 259 k read ops, physical disk 519 MB in 130 k reads (~4 KiB each)** -
  steam re-reads ~0.5 GB with small reads on EVERY start, and they reach the device. 130 k x ~0.3 ms (our QD1
  4 KiB latency) = ~40 s = most of steam's ~50 s start.
- CONCLUSION so far: cold starts = bulk-read throughput (~60-100 MB/s vs Linux 400-600 MB/s on the same chip);
  Steam = small-read latency x count at queue depth 1. Both are the UFS miniport's synchronous single-slot design.
  App CPU time (brave 7-8 s warm on 6 cores without the M4s) is the second, smaller factor.
- 2026-10-19 user: "multithread it like you did with the gpu driver" -> UFS miniport: completion by interrupt + more
  than one command in flight (+ larger transfers, direct DMA). Planning agent writes notes/ufs-async-plan.md.
- SAFETY DESIGN (decided before any code): the UFS driver is the boot disk. A failed boot can leave NTFS dirty, and
  TWRP's ntfs-3g then refuses rw (never fsck), so "swap the .sys back from TWRP" is NOT a reliable rollback. Rule for
  this work: ONE binary carries the proven w38 path and the new path; the new path is used only when (a) enabled
  and (b) the previous boot did not die (UEFI BootLog already detects unexplained Windows deaths - progress
  prev=0x72 without a restart marker - and can publish a "UFS safe mode" bit in the PRAM/handoff page the driver
  already reads). A bad new path then costs one failed boot and comes back on the old path by itself.
- Safe-mode source found (no firmware change needed): BootLogDxe keeps an unexplained-death counter in progress word
  [12] of the page at 0x8FE0F000 ('DE' << 16 | n, cleared by a clean OS restart / cold power-on;
  Beyond0LteBootLogDxe.c:743-756), and the UFS driver already maps that page (UfsBringupProgress, Exynos9820Ufs.c:8284).
  New-path gate: (Progress[12] >> 16) == 0x4445 -> previous boot died -> use the proven path this boot.
