# UFS async / multi-slot plan (Exynos9820Ufs, w38 base) - 2026-10-19

Written by a read-only planning agent from the current tree (Exynos9820Ufs.c 9479 lines); "inferred" = not measured
or read from code. Context: notes/app-launch-slowness.md (measurements, rollback design).

## Findings that change the premise

1. No write read-back runs today: UFS_FULL_WINDOWS_MODE 1 (Exynos9820Ufs.h:1044) compiles UfsVerifyWrittenBlocks
   (.c:6376) out (.c:6356); "layer 6" (.c:8099-8128) only counts. Write protection = pre-issue gates (CDB allowlist,
   write fence, GPT guard, protected partitions, crash lockout/latch, PRAM write trail).
2. The poll runs at DIRQL: SynchronizationModel = StorSynchronizeHalfDuplex (.c:8511). MS Learn HW_STARTIO: StartIo
   IRQL "is either DISPATCH_LEVEL (full-duplex mode) or DIRQL (half-duplex mode)" -> a 1 MiB read (~20 ms) polls
   with the interrupt lock held.
3. Interrupt completion needs firmware: DSDT UFS0 uses a deliberately dead GSIV 259; the real line is GSIV 274 /
   SPI 242 (Dsdt.asl UFS0 _CRS). The firmware DMA pool is 0x40000 with 0x24000 published to the driver
   (Beyond0LteBootLog.h:59-64) -> multiple slots need a bigger pool.

## Current path (all synchronous in StartIo)

- DriverEntry .c:8407: MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE, NeedPhysicalAddresses TRUE,
  TaggedQueuing/MultipleRequestPerLu TRUE. FindAdapter .c:8462: MaxTransfer 64 KiB, breaks 33, AlignmentMask 3,
  HalfDuplex, InterruptSupportNone, MaxNumberOfIO = MaxIOsPerLun = InitialLunQueueDepth = 1.
- Workspace (UfsAllocateWorkspace .c:4983): firmware pool 0xA7400000 ('DMP1' at PRAM+0x3F54): UTRL 1 KiB (room for
  32 TRDs), UCD 4 KiB (cmd 0, resp 1024, PRDT 2048 = 16 x 4 KiB), bounce 128 KiB + cacheable view.
- UfsHwStartIo 9154 -> UfsExecuteScsi 7255: vendor CDBs, state checks, classify/validate (5698/5521/5595), write gates
  (7621-7793), per-command MMIO preconditions (7874-7908), UfsBuildCommand 6182 (zeroes UTRL 1 KiB + UCD 4 KiB
  uncached 6193-6194; write staging via Device view; PRDT 4 KiB entries), NEXUS write+readback (7929-7943), doorbell
  7963, UfsPollForCompletion 6292 (10 us stalls, W27 UTRLCLR retire, 5 s -> containment/FatalError), IS ack 650,
  UfsFinishScsiCommand 6810 (framing checks with TaskTag == slot, residual, MODE SENSE fix-up, READ CAPACITY,
  evidence first 256, copy-back DC CIVAC + memcpy 7185-7200, GPT translation on media reads 7222, VPD 0xB0), write
  tail 8099-8158, second UTRL+UCD zero 8183-8184. UfsHwInterrupt 9260 returns FALSE (line parked). Only timer:
  UfsWitnessTimer (1 s, stops after 90 s). No DPCs.
- Per 64 KiB (w37 counters): device ~123 us, copy-back ~317 us, ~370 us elsewhere.

## Cheap savings (no safety layer touched)

| cost | where | fix | 4 KiB | 64 KiB |
|---|---|---|---|---|
| two uncached zero passes UTRL 1 KiB + UCD 4 KiB (~40 us each) | 6193-6194, 8183-8184 | drop the post pass; pre-zero only TRD 32 B, cmd UPIU 32 B, resp header+sense 64 B, PRDT DW0-3; FMP tails zeroed once at init (+ sampled tail check) | -70..80 us | -70..80 us |
| 10 us poll granularity | 6340 | spin ~50 us before stalling | -3..5 us | -3..5 us |
| NEXUS write+readback per command | 7929-7943 | skip when unchanged | -1..2 us | -1..2 us |
| copy-back ~317 us / 64 KiB | 7185-7200 | split counters first; removed by direct DMA (stage 6) | 0 | (-300 us) |

Steam caveat: if its reads are QD1 serial, stage 1 saves ~130k x 80 us = ~10 s of ~40 s; measure its queue depth
before stage 4. Interrupt completion frees the CPU but does not lower QD1 latency.

## Stages

- **0 groundwork**: Adapter->IoMode (0 today .. 6 direct DMA) from Parameters\Device\UfsIoMode (StorPortRegistryRead
  at FindAdapter, inferred OK), forced to 0 when the previous boot died (progress word [12] 'DE' counter at
  0x8FE0F000, already mapped as UfsBringupProgress). Refactor the pre-issue gates (7282-7908) into one shared
  UfsExecutePrepare (then re-validate mode 0). Counters: StartIo->completion ticks, copy maintenance vs memcpy.
  Tools: hashcheck.ps1 (SHA-256 of fixed files read uncached at several sizes/QDs vs a w38 baseline), raw
  PhysicalDrive0 GPT/ESP compare, diskbench -QD.
- **1 cheap savings** (table above) + every-1024th tail check that drops to mode 0 on mismatch.
- **2 FullDuplex, still polling**: StorSynchronizeFullDuplex -> StartIo at DISPATCH, interrupts no longer masked
  during the poll (ISR is a no-op on a parked line). Test also that the witness timer still fires.
- **3 interrupt completion, one slot**: DSDT UFS0 Interrupt(Level, ActiveHigh, Exclusive){274}. ORDER: new driver
  first, then firmware; rolling the driver back to w38 needs the firmware rolled back too (w38 ISR returns FALSE and
  firmware hands off IE 0x30EF5 incl. auto-hibernate bits -> level storm, inferred). ISR: unarmed -> W1C all, return
  TRUE; armed -> W1C own, latch fatal, StorPortIssueDpc. DPC/timer via passive init. IE = UTRCS | fatal mask after
  UfsProgramTransferList; TRD IOC bit (verify bit positions vs UFSHCI 2.1). UTRIACR stays 0 (warm check needs 0).
  Mode 2 observe (poll, count IRQs; >= 90 % of first 64 -> mode 3 allowed), mode 3 async (complete in DPC, move
  watchdog pet / write tail there), backstop timer 20 ms, 5 s containment unchanged, ResetBus completes outstanding.
  MS Learn quotes: HW_INTERRUPT "should return within 50 microseconds ... deferred to the HwStorDpcRoutine";
  StorPortIssueDpc ignores a queued DPC (reap all slots per run); StorPortInitializeDpc "must be called ... from
  within ... HwStorPassiveInitializeRoutine"; after RequestComplete "the port driver owns the request".
- **4 multiple slots for reads** (writes drained to depth 1): firmware pool 0x200000 + new 'DMP2' record (keep
  'DMP1' for w38). Per-slot UCD + 64 KiB bounce, UFS_SLOT[] with tag = slot in UPIU, per-slot response check,
  NEXUS all bits once, doorbell 1 << slot, UTRLCLR per completed slot. MaxNumberOfIO = MaxIOsPerLun = NumSlots (<= 8),
  StorPortSetDeviceQueueDepth on the first INQUIRY. HIGHEST RISK: UTRLCLR bit polarity with other slots in flight
  (W27) -> stage 4a self-test: two reads on slots 0/1, retire 0, slot 1 must still complete with correct data.
- **5 writes at depth > 1**: pattern-file write/flush/hash tests, clean reboot, re-verify in Windows and TWRP
  (ntfs-3g read-only + sha256sum), chkdsk /scan.
- **6 direct DMA + larger transfers**: READs whose SG elements are all 4 KiB-aligned (FMP data-unit rule,
  .h:876-911) and that avoid the GPT header LBAs DMA straight into the SRB buffer (bounce otherwise); then writes;
  then 256 KiB transfers (UCD 12 KiB per slot, 64 PRDT entries), pool ~N x 268 KiB.

## Progress log
- 2026-10-19: plan written (agent); implementation order: stage 0 safe-mode gate + counters, stage 1, stage 2.
- w39 = w38 + per-boot UfsIoMode (UFS_IO_MODE_DEFAULT 0x3: FAST_ZERO + FULL_DUPLEX), forced 0 when progress word
  [12] holds the BootLog death counter ('DE'); EarlyMark "=IOMODE mode/deaths". FAST_ZERO: TRD slot 32 B + command
  64 B + response 128 B + each used PRDT entry (128 B) instead of UTRL 1 KiB + UCD 4 KiB before and after every
  command. FULL_DUPLEX: StorSynchronizeFullDuplex (MS Learn lock table: HwStorTimer/HwStorResetBus hold the StartIo
  lock in both modes, + Interrupt only in half duplex; AdapterControl holds none either way).
  Built with build-ufs.ps1 -DmaWindow (= w38 flags), signed with the WDKTestCert 5DD5DEE1..., sha256 898a61b4...,
  s10e-artifacts/ufs-w39/Exynos9820Ufs.sys.
- Integrity baseline on w38 (tools/phone/perf/hashcheck.ps1, uncached 4 KiB / 64 KiB / 1 MiB reads agree):
  edgehtml 0855A9E8B67145EC, mshtml 4D5CF1DA5D9145A1, Windows.UI.Xaml C268FA69C99CF44B, ntoskrnl 6B97225E78F9EB50,
  shell32 3D45B4D59F8D0DB8, Brave chrome.dll C40B133551FC72EA.
- diskbench on w38 is noisy run to run (64 KiB 630 vs 1290 us, 1 MiB 20 vs 8.4 ms) -> compare medians of 3+ runs.
- **w39 VALIDATED on device (2026-10-19 ~14:10)**: deployed from TWRP with sys-deploy.sh (w38 kept as sys-prev),
  firmware boot-host-r2-wdt.img for the first boot (hang -> watchdog -> death counter -> mode 0 next boot).
  Windows up in 66 s. hashcheck identical to the w38 baseline (6 files x 3 request sizes, fc: no differences).
  diskbench x3: 4 KiB seq 236 / 195 / 196 us (w38 303-331), rand 4 KiB QD1 3425 / 4882 / 5035 IOPS (w38 3288-3579,
  avg 0.19-0.20 ms vs 0.28-0.30), 64 KiB ~1.23 ms and 1 MiB ~19 ms (unchanged, inside the w38 run-to-run range
  0.63-1.29 ms / 8.4-20 ms - large requests are dominated by copy-back and vary by run; which core runs the copy is
  an inferred cause).
  Steam (launchtime.ps1): first start after reboot window 42 s / idle 64 s; warm: window 1.2 s / idle 33.5 s with
  121 k disk reads (w38 warm: idle 49.8 s, 138 k reads) -> ~-33 %.
- Next: per-command latency (~195 us vs device ~100 us: 6 precondition MMIO reads, NEXUS write+readback, IS ack,
  10 us poll stalls), then stage 3 (interrupts; needs the DSDT GSIV 274 change, driver first) for depth > 1.
- UfsDiag (w39, cumulative 60 k reads, avg ~26 KB): PERF_PER_READ_US DEVICE 180, COPYBACK 221 -> the copy-back
  (DC CIVAC per line over the cacheable bounce view + memcpy) costs more than the device.
- IO coherency: firmware sets the 9810 bits (BIT8|9 = PCIe ch0 on the 9820); UFS bits BIT22|BIT23 only behind
  Beyond0LteUfsHcDxe BEYOND0LTE_UFS_IOCOH_9820 (A/B, never validated in Windows). boot-iocoh1-wdt.img e86f4271 (host +
  IOCOH_9820 1): sysreg 0x13C10700 = 0x00C00311, Windows booted, hashcheck identical, diskbench noisy as before.
- **REVERTED after ~8 min (12:17-12:25)**: with coherent UFS DMA, a device read of the bounce (WRITE command) snoops
  the CPU caches, and the current driver leaves CLEAN lines of the bounce in the cache after every read copy-back
  while it stages writes through the uncached (Device) alias -> the snoop could supply stale data to the disk
  (mismatched-attribute alias; Arm gives no coherency guarantee there). Read-only hashcheck cannot see that.
  Checks after the revert: `chkdsk C: /scan` = no problems; `sfc /verifyonly` flagged only Windows Defender
  definition files under Definition Updates\Default\ (folder empty; live copies in the GUID folder, base files dated
  10/14, deltas updated after the revert) - no file written in the window flagged. No evidence of damage.
  RULE for the coherent mode (stage "C"): enable the 9820 iocoh bits ONLY together with a driver that never touches
  bounce data through the Device alias (stage writes AND read copy-back through the cached view, no per-line CIVAC),
  and verify writes (pattern file write/flush/reboot/hash) before daily use. Firmware and driver ship as a pair.
- w40 18037e9a (UFS_IO_COHERENT_BOUNCE: when SYSREG_FSYS1+0x700 bits 22|23 are on, writes staged and reads copied
  through the cacheable view only, no per-line CIVAC; otherwise = w39). On daily firmware (bits off): hashcheck =
  w38, pattern files (tools/phone/perf/writetest.ps1, written under w39) verify. With boot-iocoh1-wdt (0x00C00311):
  hashcheck = w38, old patterns verify, new patterns written+verified (wt2). But copy-back unchanged (~137 us for
  ~15 KB = ~110 MB/s) -> the firmware-pool "cacheable" view behaves uncached (inferred: Windows maps that non-RAM
  range uncached whatever is asked); per-line maintenance was not the cost.
- **w41 3f4b4da4**: coherent mode moves the bounce into cached Windows RAM (MmAllocateContiguousMemorySpecifyCache,
  < 4 GiB, page aligned; both views point at it -> no alias); iocoh on but no RAM bounce -> cacheable view dropped
  (all-uncached). On boot-iocoh1-wdt: BOUNCE_PA 0xFD8D0000 (RAM), hashcheck = w38, wt + wt2 verify, wt3 written +
  verified. UfsDiag: BOUNCE_IN 0.08 s per 26 k reads (~3-4 us per read, was 137-221 us).
  diskbench x3: 4 KiB 144 / 174 / 146 us; rand 4 KiB 6374 / 4436 / 5163 IOPS; 64 KiB 224 / 260 / 248 us
  (279 / 241 / 252 MiB/s, was 50-113); 1 MiB 3.2 / 5.5 / 3.4 ms (310 / 181 / 298 MiB/s, was 50-138).
  PAIRING RULE: the iocoh firmware (BEYOND0LTE_UFS_IOCOH_9820 1) requires driver >= w40 (w41 recommended); rolling
  the driver back below w40 requires the non-iocoh firmware first.
- After a reboot: wt2/wt3 (written in coherent mode) verify. launchtime: Brave first start after boot window 2.7 s
  (w38 6.6 s), Steam warm idle 30.3 / 32.1 s (w38 49.8, w39 33.5) - Steam is bound by ~135 k QD1 4 KiB reads now
  (~145 us each) -> needs queue depth > 1 (stage 3/4).
- Daily firmware boot-host-r3.img d4d33853 = host + BEYOND0LTE_UFS_IOCOH_9820 1 (now the source default, with the
  pairing rule in the comment). Phone: r3 + w41, 0x13C10700 = 0x00C00311, BOUNCE_PA in RAM, wt3 verifies.
- tools/phone/perf/qdepth.ps1 (raw PerfDisk counters over the launch window): Steam warm 35 s: avg queue 0.42, disk
  idle 57.9 %, 116.8 k reads, never >= 2 queued -> Steam's I/O is strictly serial (QD1); a deeper queue cannot help
  it. Per-command latency (~125 us now) and Steam's own CPU (x86 emulation) are what is left.
- tools/phone/perf/qdepth.ps1, diskbench -Only rand|sweep|aligned (-Bs), adapterprop.ps1 (StorageAdapterProperty).
- Pure 4 KiB QD1 (5000 reads, UfsDiag deltas): device 115 us per read (incl. 10 us poll granularity), copy 2 us,
  rest of the stack ~40-45 us (user p50 159 us) -> 4 KiB is near the device's QD1 latency.
- **w42 2cb8a63d** (UFS_IO_LARGE_TRANSFER, default mode 0xF): coherent RAM bounce 256 KiB, UCD relocated into the
  freed firmware-pool bounce area (UCD_PA 0xA7402000, 64 PRDT entries), per-adapter MaxTransfer / PrdtMaxEntries /
  UcdRegionSize / BounceSize, ConfigInfo MaximumTransferLength 256 KiB, write block cap raised with it.
  adapterprop: MaximumTransferLength 0x40000, MaximumPhysicalPages 64. hashcheck = w38; wt/wt2/wt3 verify, wt4
  written + verified. Counter deltas: unaligned 256 KiB requests arrive as 2 commands (~122 KiB avg) - 65 pages
  for an unaligned buffer > 64; device 337 us per ~122 KiB (370 MB/s), 64 KiB 138 us (415 MB/s), ~87 us per
  command outside device + copy.
- **w42b 6b4a197b**: NumberOfPhysicalBreaks + 2 (StorPort still reports 64 pages - capped by MaxTransfer/PAGE).
  hashcheck = w38, wt4 verifies, wt5 written + verified. diskbench -Only aligned (page-aligned buffer = what paging
  I/O uses): 64 KiB 318 MiB/s (197 us), 256 KiB 325 MiB/s (770 us), 1 MiB 430 MiB/s (2.32 ms = 4 x 256 KiB at ~580 us)
  vs Linux 617 MB/s. A user-mode request costs ~190 us beyond its commands (I/O stack on the small cores).
- Next lever: overlap (queue depth > 1: the device idles ~87 us between commands) -> stage 3 interrupts (DSDT
  GSIV 274, driver first) + stage 4 slots; and trimming the per-command miniport path.
- **UFS interrupt VERIFIED (2026-10-19, TWRP /proc/interrupts): "GIC-0 281 Edge ufshcd"** -> GSIV 281 = SPI 249
  (stock DT ufs@0x13D60000 interrupts <0x0 0xf9 0x0>; the decompiler shows 0xf9 as a phandle). The DSDT comment's
  "real UFS line GSIV 274 / SPI 242" is WRONG. Linux runs it edge-triggered; StorPort hardcodes level (Dsdt.asl
  comment) -> whether the controller output is a level (IS bits held) or a pulse is UNVERIFIED -> stage 3 starts in
  observe mode (poll as today, count IRQs per command).
- **w43 50fb6d1d** (UFS_IO_IRQ_OBSERVE; ISR acks IS armed or not, latches bits for the poll path; IE armed
  0x000F0201 = UTRCS + fatal; TRD IOC when armed; precondition IE == ArmedInterruptEnable). DSDT now takes
  BEYOND0LTE_UFS_GSIV (default 259 parked; comment corrected to the verified 281).
  On boot-iocoh1-wdt (parked): CALLBACKS 0, IE_AFTER_ISR 0x000F0201, hashcheck = w38, wt5 verifies.
  **On boot-irq1-wdt.img 08ae214d (GSIV 281, Level)**: Windows up in 67 s, no storm; after 44.3 k commands
  CALLBACKS 38907, UTRCS seen by the ISR 24521 (the rest: the poll path cleared IS first) -> the real UFS interrupt
  is delivered under StorPort's level mode. Stage 3b (async completion in a DPC) can rely on it.
- **w44 65a4ae23** (UFS_IO_ASYNC: UfsFinishIssuedCommand split out of UfsExecuteScsi; PendingSrb set before the
  doorbell once the ISR proved >= 16 completions; StartIo returns SRB_STATUS_PENDING; ISR -> StorPortIssueDpc;
  UfsAsyncReap under the StartIo lock; extended backstop timer 2 ms; passive init for DPC + timer; ResetBus settles a
  pending command). On boot-irq1-wdt: the FIRST boot DIED (watchdog), the next boot came up after 180 s total in
  SAFE MODE - progress word [12] = 0x44450001 ('DE', 1 death) -> UfsIoMode 0 (IE not armed, CALLBACKS 1,
  CONTAINED 0). The per-boot safe-mode gate WORKED as designed.
  Suspect (inferred): UfsAsyncTimer called UfsAsyncReap, which acquires the StartIo lock; MS Learn documents no lock
  table for extended-timer callbacks (StorPortRequestTimer page) while the legacy HwStorTimer holds StartIo -> a
  recursive spin-lock acquire ~2 ms into the first async command.
- **w44b 3b62a710**: the timer callback only queues the completion DPC (SystemArgument1 = 1 marks a timer reap);
  the DPC (documented: no StorPort lock held) is the only place that takes the StartIo lock.
- **w44b on boot-irq1-wdt**: boot 66 s, death counter 0, CALLBACKS 46245 / UTRCS 46243 for ~46 k commands, poll
  iterations 34 (the 16 proving commands) -> async completion live. hashcheck = w38, wt5 verifies, wt6 written +
  verified. BUT QD1 latency worse: 4 KiB p50 ~230 us (w42b 155), 64 KiB aligned 233-270 us (197), 1 MiB 350-380
  MiB/s (430): interrupt + DPC dispatch ~70 us per command. Steam warm idle 47.8 s (polled ~31 s).
- **w45 78b526d2** (hybrid, UFS_ASYNC_MIN_BYTES 64 KiB): commands <= 64 KiB polled without TRD IOC (no interrupt),
  larger ones async; all request IOC until 16 completions proved the line. hashcheck = w38, wt6 verifies, wt7 written +
  verified. 4 KiB p50 155-159 us, 64 KiB aligned 186-193 us, 256 KiB 652-798 us, 1 MiB 385-391 MiB/s (async costs
  ~10 % at QD1 for big commands, frees the CPU). Steam warm 34.0 s (139 k reads) / 15.7 s (45 k reads - Steam did
  less work) ~= polled. Best configuration so far.
- Daily firmware boot-host-r4.img = host + IOCOH (default) + BEYOND0LTE_UFS_GSIV 281 (now the source default).
- **w46 1194fa25** (UFS_IO_MULTI_SLOT, mode 0x7F, UFS_MAX_SLOTS 4): per-slot TRD / UCD (in the freed pool area after
  slot 0's) / 256 KiB coherent RAM bounce, UfsSelectSlot points the existing build/finish code at a slot, UPIU task
  tag = slot (response check takes the expected tag), doorbell / UTRLCLR per slot bit (Linux
  UFSHCD_QUIRK_BROKEN_REQ_LIST_CLR = write 1 << tag), NEXUS all slot bits, precondition checks only the slot's
  doorbell bit, MaxNumberOfIO / MaxIOsPerLun / InitialLunQueueDepth = slots. Admission: async = READS > 64 KiB only
  (FLUSH completes locally, so writes must finish before returning); everything else drains all slots first
  (UfsSettleOutstanding) and runs alone in slot 0. DPC reaps every completed slot; ResetBus settles all.
  On boot-irq1-wdt: boot 66 s, death counter 0. hashcheck = w38; parhash.ps1 (4 concurrent hashcheck instances) all
  match; wt7 verifies, wt8 written + verified. diskbench aligned: 1 MiB 555-565 MiB/s (1.77-1.80 ms; w45 385-391,
  w42b 430; Linux 617), 256 KiB 285-388 MiB/s, rand 4 KiB p50 152 us / 6299 IOPS. StorPort splits a 1 MiB request into
  4 x 256 KiB SRBs which now run in parallel slots. (parhash wall 217 s vs w45 199 s: that test is dominated by
  small polled reads that drain - not a throughput test.)
- **w47 1a06a038**: NEXUS write+readback skipped while unchanged (cache reset by containment and warm-handoff
  restore), completion poll in 1 us steps for the first ~300 us (both under UFS_IO_FAST_ZERO, so safe mode reverts
  them). On boot-irq1-wdt: hashcheck = w38, wt8 verifies, wt9 written + verified. rand 4 KiB p50 147-152 us
  (w46 152) - ~3 us; 1 MiB aligned 503 MiB/s (in the w46 range). The remaining ~40 us per small read is the Windows
  I/O stack on the small cores; device ~110 us.
- App results with w46/w47 + r4 (vs w38 at the start of the day): Brave first start after boot window 2.35 s / idle
  10.7 s (6.6 / 17.1), Steam warm idle 29.6-32.0 s (49.8). Steam is bound by its ~130 k strictly serial 4 KiB reads
  (~150 us each, near the device's QD1 latency) plus its own CPU (x86 emulation).
- STATE: UFS read path near the hardware limits - QD1 4 KiB ~150 us (device ~110 us), sequential ~500-560 MiB/s
  (Linux 617 MB/s). Further disk-side gains would need the device itself to be faster at QD1, or Windows' stack.
