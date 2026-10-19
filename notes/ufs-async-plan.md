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
