# m15 - UFS research: descriptor / DMA / driver side (why ~30x slower than Linux)

Task (research only, no device, no flash, no commit): field-by-field Linux 9820 UTRD/UCD/PRDT vs our
Windows miniport (drivers/Exynos9820Ufs) and UEFI HC (Beyond0LteUfsHcDxe); per-command fixed costs;
device-side command shape; vendor HCI regs tied to PRDT granularity; other open implementations.
Inputs: notes/m15-ufs-throughput.md, notes/m15-perf.md (read in full first).
Kernel copies: scratchpad ac99dbea.../k/ (prior task), new fetches in scratchpad b82d8ce8.../ufs-r2/.

## Progress log
- 2026-10-12: read m15-ufs-throughput.md + m15-perf.md. Baseline: 4 KiB 555 us, 64 KiB ~4.0 ms, 1 MiB 53 ms,
  ~60 us/KiB, link clean (UECxx = 0), DEVICE time dominates; Linux dd 617 MB/s.
- Linux UTRD/UCD (k/ufshcd.c:4404-4437, ufshci.h:42,435-501): QUIRK_PRDT_BYTE_GRAN (set ufs-exynos.c:585) ->
  DW6 response_upiu_length = ALIGNED_UPIU_SIZE = 1024 BYTES, response_upiu_offset = 1024 bytes, DW7
  prd_table_offset = 2048 bytes (UCD = cmd[1024] | rsp[1024] | prd_table[SG_ALL * sg_entry_size]).
  Per command (ufshcd.c:2633-2660): 9820 prd_table_length = nseg*sizeof(struct ufshcd_sg_entry)(=16) + 16
  (#if CONFIG_SOC_EXYNOS9820), other byte-gran SoCs nseg*sg_entry_size; entries strided by sg_entry_size =
  sizeof(struct fmp_table_setting) = 128 (ufs-exynos-fmp.c:229; fmp.h:140-198 UFS variant = 32 dwords);
  prd->size = len-1, reserved(DW2)=0. FMP bypass (no bio crypt, e.g. dd on by-name in TWRP) = exynos_fmp_bypass
  (ufs-exynos-fmp.c:108-117). Crypto path: IV = dun + sg index (ufs-exynos-fmp.c:135) and data unit 4096
  (crypto_modes_supported, :204) -> with FBE every PRDT entry is 4 KiB on Linux too.
  FMP UFS-variant des3 = FAS[29:28] DAS[31:30] LENGTH[25:0] (fmp.h:146-158) = the same dword as prd->size.
- OUR Windows (Exynos9820Ufs.h:861-891,1365-1386; .c:6055-6100 UfsProgramPrdt, 6172-6283 UfsBuildCommand):
  DW6 = 0x04000400 (len 1024 B, off 1024 B) == Linux. DW7 offset 2048 B == Linux. DW7 length = Entries*128
  (Linux 9820: Entries*16+16) -> 1 entry: 128 vs 32; 16 entries: 2048 vs 272. Entry stride 128 == Linux,
  DW3 = (len-1)&0x3FFFF == Linux (FMP LENGTH is 26 bits, our 18-bit mask harmless <256K), DW1/DW2 = 0, tail 0
  (UCD zeroed per command). Segment forced 4096. UCD 4 KiB -> PRDT region 2048 B -> 16 entries -> 64 KiB max.
- Linux exynos_ufs_init_host (k/ufs-exynos.c:455-487) TXPRDT = PRDT_PREFECT_EN|PRDT_SET_SIZE(12), RXPRDT =
  PRDT_SET_SIZE(12) (ufs-exynos.h:26-27,143-145: PRDT_SET_SIZE(x) = x & 0x1F, no unit documented). Linux quirks
  (ufs-exynos.c:585-588) PRDT_BYTE_GRAN | UNRESET_INTR_AGGR | BROKEN_REQ_LIST_CLR | BROKEN_CRYPTO. Linux
  max segment = PRDT_DATA_BYTE_COUNT_MAX 256 KiB (ufshcd.c:5743), no Exynos override (grep "segment" in
  ufs-exynos.c: none). Vendor regs never written by Linux: MAX_DOUT_DATA_SIZE 0x64, WRITE_DMA_CTRL 0x74,
  INVALID_PRDT_CTRL 0x130, DBR_TIMER 0x140.. (ufs-exynos.h:50,57,112-114).
- UEFI HC (Beyond0LteUfsHcDxe.c:3141-3176, 3453-3555): converts EDK2 dword DW6/DW7 to bytes, splits EDK2's
  16 B PRDT into <=4 KiB page chunks re-strided at 128 B, PRDT length = Out*128 (+UTP_PRDT_9820_EXTRA = 0).
  Comment there (B81) cites edk2-exynos Exynos9820Pkg ExynosUfsLib/ufs.c:451-463 prdt_len = segs*sizeof(prdt)
  with 4 KB segments and TX/RXPRDT = UFS_SG_BLOCK_SIZE_BIT (:1370-1371), and boot #23: with +16 a 4-entry
  read never completed. boot #24: 4-entry 16 KiB read only works at 128 B stride. => the UEFI side already
  measured "nseg*16+16" FAILS on our setup while Linux uses it. Contradiction to resolve (see analysis below).
- KEY HISTORY FINDING (notes/m8-uefi-image.md B79-B85): Linux's 9820 formula nseg*16+16 was tested ONLY in boot #23
  (B81) while the PRDT stride was still EDK2's 16 B; it failed (16 KiB split read never completed). B83/boot #24
  then showed the stride must be 128 B (DESCTYPE-3 set by our own SMC FMP_SECURITY CFG_DESCTYPE_3,
  Beyond0LteUfsHcDxe.c:357 - same as Linux) and switched to length n*128 at the same time. The Linux pair
  (stride 128 + length n*16+16) has NEVER been run on this phone. The n*128 length is inherited from the 9810
  "WOAXFER PrdtL=0x80/entry" capture (a 1-entry dump; 0x80 = 1*128 = also 7*16+16, so it cannot discriminate).
  Windows driver copied n*128 from the UEFI (Exynos9820Ufs.h:1371-1386).
- edk2-exynos Exynos9820Pkg ExynosUfsLib/ufs.c (Samsung LK port; fetched to scratchpad ufs-r2/ufs.c, 2227 l):
  __utp_map_sg :309-325 every entry = UFS_SG_BLOCK_SIZE(4 KiB) with size = 4096-1; __utp_write_utrd :446-466
  prdt_len = nseg*sizeof(struct ufs_prdt), prdt_off = 2*ALIGNED_UPIU_SIZE; rsp_upiu_len = ALIGNED_UPIU_SIZE
  (:1346); ufs_vendor_setup :1361-1376 TX/RXPRDT = UFS_SG_BLOCK_SIZE_BIT (no prefetch bit), REORDER 0xA,
  NEXUS 0xFFFFFFFF; also vs+0x114 = 0x22, vs+0x110 = 1 ("TODO: cport", :1352-1353). struct ufs_prdt size is in
  LK <dev/ufs.h> (not in that repo) -> UNVERIFIED whether 16 or 128.
- MAINLINE Linux (torvalds master, fetched ufs-r2/ml-*): ufshcd.c:2721-2723 byte-gran prd_table_length =
  nseg * ufshcd_sg_entry_size(hba) (= OUR formula, no +16); ufs-exynos.c:1340-1347 struct fmp_sg_entry = 128 B,
  :1394-1405 DESCTYPE-3 SMC "needed to make the hardware use the larger PRDT entry size" -> sg_entry_size 128;
  :31 DATA_UNIT_SIZE 4096; :1180-1191 TXPRDT = ilog2(DATA_UNIT_SIZE) (|PREFETCH only with crypto cap), RXPRDT =
  ilog2(4096) = 12; **:1715 host->max_segment_size = DATA_UNIT_SIZE for every Exynos variant**; :1597
  dma_alignment = 4095; :1491 FMP requires each segment == exactly one data unit.
  => PRDT_SET_SIZE(12) = log2(bytes per PRDT entry) = 4 KiB. That alone explains the 8 KiB-entry HOST_FATAL
  (controller configured for 4 KiB entries; Exynos9820Ufs.h:876-891 measurement is consistent with it). Our
  4 KiB/entry split is the SAME shape mainline uses on every Exynos => 4 KiB entries are NOT inherently slow
  (gs101/exynosauto reach full speed with them). The Samsung 4.14 kernel's 256 KiB max segment is moot in
  practice: page-cache pages are rarely physically contiguous (no IOMMU in the DT path), so Linux on the
  phone also issues mostly 4 KiB entries at 617 MB/s (~6.6 us per entry vs our ~250 us per entry).
- RE-READ of the perf numbers (m15-perf.md runs 24/29): the cost is NOT linear in bytes across sizes.
  Run 29 boot DEVICE total 29.6 s; the raw test alone = 2048 x 64 KiB (4.19 ms) + 256 x 1 MiB (16 x 64 KiB, 53 ms)
  = 6144 x 64 KiB commands = ~22 s wall => the other ~230k boot commands (avg ~8-10 KiB) shared <= ~7.5 s =
  ~30 us device time each. Small transfers are Linux-class fast; 64 KiB (16-entry) transfers take ~4 ms.
  File random 4K read = 2900 IOPS (345 us end-to-end) while raw sequential 4 KiB = 555 us. => look for a
  SIZE / ENTRY-COUNT THRESHOLD, not a per-byte cost. Things that change with size: PRDT entry count n, the
  declared PRDT length (n*128, Linux-9820 n*16+16), credit/flow-control windows on the link, DATA IN UPIU count.
- Command shape (Q3) == Linux ufshcd_prepare_utp_scsi_cmd_upiu: byte0 0x01, flags 0x40 R / 0x20 W (task attr
  SIMPLE=0), LUN 0, tag 0 (UFS_TRANSFER_SLOT), CommandSet 0, EHS 0, DataSegLen 0, EDTL = DataTransferLength BE
  (Exynos9820Ufs.c:6264-6278), CDB copied verbatim from classpnp except MODE SENSE(6)->(10) (:6123-6170). No
  FUA is ADDED by the driver; classpnp sets FUA only for SL_WRITE_THROUGH (s10eperf seq-write opens
  FILE_FLAG_WRITE_THROUGH, S10ePerf.c:105-106 - so the "seq write" figure includes FUA writes; raw reads and seq
  reads do not, S10ePerf.c:122,163-164). UTRD DW0 = CT 1 | DD (no I bit, polled) == Linux with intr aggr; DW2 OCS
  0xF == Linux; DW1/DW3 0 == Linux (no crypto). Write RTT/DATA OUT flow is handled by the HCI in both; neither
  writes HCI_MAX_DOUT_DATA_SIZE. => no command-shape difference that would make big transfers slow.
- 9810 Samsung kernel (LineageOS android_kernel_samsung_exynos9810 @ lineage-20, ufs-r2/9810-ufshcd.c:1677-1680,
  9810-ufshci.h:363-397): prd_table_length = nseg * sizeof(struct ufshcd_sg_entry) where that struct INCLUDES the
  28 FMP dwords under CONFIG_EXYNOS_FMP => n*128 is the **9810** encoding. The 9820 kernel replaced it with an
  explicit `#if defined(CONFIG_SOC_EXYNOS9820)` n*16+16 branch (k/ufshcd.c:2634-2643) while keeping the 128 B
  stride - a deliberate per-SoC change for HCI hw-rev UFS_VER_0006. Our n*128 = the 9810 value carried over
  (UEFI B83 "WOAXFER PrdtL=0x80/entry" was captured on the 9810 kernel). Mainline (gs101 etc.) also n*128, so
  newer IP accepts it, but that is not evidence for the 9820 rev. VERDICT: our DW7 length is UNVERIFIED-for-9820
  and is the #1 descriptor-side difference; it cannot explain the 8 KiB-entry HOST_FATAL (that is PRDT_SET_SIZE
  12 = 4 KiB/entry, see mainline :1715) but it is a plausible entry-count-dependent slow path.
- Q2 per-command fixed costs (Windows miniport, all OUTSIDE the DEVICE window unless noted):
  * UfsBuildCommand zeroes UTRL 1 KiB + UCD 4 KiB every command (Exynos9820Ufs.c:6193-6194; also 5288-5289, 6404-6405, 8158-8159, 9332-9333) on the MmNonCached
    (Device) workspace mapping (:4852-4856): 640 x 8-byte stores (w33 wide). Run 24 measured ~63 ns per uncached
    4-byte store (ZERO 165 us for ~10.5 KiB reads) => ~40 us per command. Only ~100 B + n*16 B change per command;
    the 128 B FMP tails are written by nobody else and could be zeroed once at init.
  * Precondition MMIO reads per command (:7867-7875: IE, UTRLBA lo/hi, RUN_STOP, DOORBELL) + NEXUS write and
    readback (:7905-7913) ~7 HCI reads ~= a few us.
  * Poll (:6300-6345): 10 us StorPortStallExecution per pass + 1 HCI read (UTRLDBR, never clears on 9820) + 1
    uncached UTRD OCS read; avg ~5-6 us granularity loss. Doorbell retire by UTRLCLR (W27) every command.
  * IS read + W1C write (:650-680); response UPIU parse from uncached UCD.
  * Read copy-back: cached view + DC CIVAC per line (w34), ~39 us avg. WRITE staging still copies into the
    Device-memory view (UfsCopyToUncached :6226, :6232): ~63 ns/8 B => ~0.5 ms per 64 KiB write - a real per-byte
    write cost (not in DEVICE ticks).
  * Loops over pages: only UfsProgramPrdt (n x 3 stores, trivial) and the DC CIVAC range. Nothing per-PRDT-entry
    or per-4 KiB in our code is anywhere near 200 us. => the 64 KiB slowness is in the controller/link/device.
  * Estimated fixed CPU per 4 KiB read ~70-90 us of the 345 us random-4K I/O; zeroing is the biggest piece.
- Q4 vendor regs vs PRDT granularity: TX/RXPRDT_ENTRY_SIZE[4:0] = log2(bytes per PRDT entry) (mainline
  ufs-exynos.c:1180-1191 ilog2(DATA_UNIT_SIZE)); bit31 = PRDT prefetch (TX only on Linux 9820, k/ufs-exynos.c:468-470).
  Our UEFI comment "12 DWORDs/entry" (Beyond0LteUfsHcDxe.c:556) is WRONG about the meaning (value is right).
  HCI_PRDT_HIT_RATIO 0xC4 (k/ufs-exynos.h:94) implies a PRDT cache; INVALID_PRDT_CTRL 0x130, INVALID_UPIU_CTRL
  0x10, INVALID_UTR/DIN_OFFSET 0x20/0x24, VENDOR_SPECIFIC_IS 0x38, DBR_DUPLICATION_INFO 0x120, DBR timers
  0x140-0x17C (ufs-exynos.h:31-36,37,111-127) are never read by our drivers and undocumented bit-wise -> reads below.
  DATA_REORDER / AXIDMA burst are byte-stream settings, not PRDT-granularity-dependent; already == Linux.
- Doorbell never auto-retires on our setup (W27, Beyond0LteUfsHcDxe.c:3859-3866) even for queries without PRDT,
  while Linux relies on it (ufshcd_transfer_req_compl). Linux runs interrupt aggregation ON (UTRIACR 0x81000F01),
  we run it OFF (UFS_HC_UTRIACR_CFG 0, Beyond0LteUfsHcDxe.c:586-587). Not a throughput proof, but another place
  our completion FSM differs from Linux.

## Ranked suspects (descriptor / DMA / driver side)
1. DW7 PRDT length = n*128 (9810 encoding) instead of the 9820 kernel's n*16+16 (k/ufshcd.c:2634-2643).
   Entry-count dependent, never tested with the 128 B stride (B81 confound). UNVERIFIED either way.
   Confirm (read-only, s10ediag peeks): clear-read then after a raw 64 KiB read loop: 0x13D61130 INVALID_PRDT_CTRL,
   0x13D61138 VENDOR_SPECIFIC_IS, 0x13D61110/14/18/1C/20/24 INVALID_*, 0x13D61120 DBR_DUPLICATION_INFO,
   0x13D6115C RX_UPIU_MATCH_ERROR_CODE; sample 0x13D611C0 FSM, 0x13D611C4 PRDT_HIT_RATIO, 0x13D611C8-D4 DMA0/1
   MONITOR, 0x13D611D8/DC DMA DOORBELL_DEBUG repeatedly DURING the loop.
   Experiment: build flag (e.g. UFS_PRDT_LENGTH_9820) changing UFS_UTRD_PRDT_WORD_FOR (Exynos9820Ufs.h:1385-1386)
   to ((Entries*16 + 16) | (2048 << 16)), applied to DataIn only first (call sites .c:6268 and :6423), keep the C_ASSERT pins at Exynos9820Ufs.h:1483-1484 updated;
   raw 4K/64K/1M reads. Optionally mirror in UEFI later (Beyond0LteUfsHcDxe.c:3549-3553,
   UTP_PRDT_9820_EXTRA :3176 with per-entry 16 not 128). Risk: read-only; a failure shows as OCS error/timeout ->
   containment -> early bugcheck -> TWRP. Do NOT enable for writes until reads verify byte-exact.
2. Unidentified size threshold (small commands ~30 us device, 64 KiB ~4 ms). Measure before changing more:
   (a) per-entry-count histogram in UFS_PERF_INSTRUMENTATION (add Ticks/Count[17] indexed by LastPrdtEntries
   next to PerfDeviceTicks, .c:7975-7984; print in UfsDiag.c:370-395); (b) s10eperf raw sweep 4,8,12,16,24,32,48,
   64 KiB (S10ePerf.c RawRead :147-180 only does 4K/64K/1M); (c) discriminator: same 16 KiB read as 8 x 2 KiB
   entries (UFS_PRDT_SEGMENT_SIZE 2048, reads only) vs 4 x 4 KiB. Time follows entries -> descriptor side (#1);
   follows bytes -> link flow control (hand to PHY/UniPro research: DME_GET 0x2002/0x2044/0x2045/0x2047, PA_Granularity
   0x15AA + PA_TActivate 0x15A8 local+peer, PA_SaveConfigTime 0x15A4 ours 0xFA vs Linux 0x3E8).
3. IO-coherency sysreg 0x13C10700 = 0x311: we set 9810 bits 8|9 (meaning on 9820 UNVERIFIED) and leave the 9820
   UFS bits 22|23 clear (Beyond0LteUfsHcDxe.c:483, 672-675). Experiment: leave the register at sboot's 0x11
   (no write) - one-line UEFI change; non-coherent bounce stays correct because Windows does explicit cache
   maintenance (w34) / Device mapping.
4. Per-command CPU overhead (helps 4K IOPS ~10-20 %, not the 30x): zero only the TRD (32 B), command UPIU
   (32 B), response header (~64 B) and rewrite DW0-3 of n PRDT entries (.c:6193-6194, :6404-6405); spin-poll
   OCS without the 10 us stall for the first ~50 us; drop steady-state precondition reads; stage writes through
   the cached bounce view + DC CVAC instead of Device-memory stores (.c:6226, :6232).
5. Max transfer 64 KiB (UCD 4 KiB -> 16 entries, Exynos9820Ufs.h:876-895). After #1/#2, grow UFS_UCD_REGION_SIZE
   (e.g. 16 KiB -> 112 entries = 448 KiB, bounce/workspace grow accordingly, UFS_WORKSPACE_SIZE 0x24000) to
   amortize per-command cost. Not a cause by itself.
Ruled out (== Linux): DW6 (1024 B len + 1024 B off), PRDT offset 2048 B, 128 B stride, DBC = len-1, FMP tail 0
(bypass), 4 KiB per entry (mainline does the same; PRDT_SET_SIZE 12), CDB/EDTL/flags/LUN, no added FUA,
TX/RXPRDT/REORDER/AXIDMA values. Note s10eperf "seq write" uses FILE_FLAG_WRITE_THROUGH (FUA writes).
- Self-review of this note (2026-10-12): line citations re-checked with rg (UfsBuildCommand zeroing is
  .c:6193-6194, write staging .c:6226/6232, PRDT word stores .c:6268/6423, PerfDeviceTicks .c:7979, macro
  .h:1385 with C_ASSERT pins .h:1483-1484 that a length-formula flag must update). The "small commands ~30 us"
  figure is DERIVED (boot DEVICE total minus raw-test wall time), not a direct measurement - suspect #2(a)
  is what turns it into one. Mainline facts are from torvalds master fetched 2026-10-12 (untrusted copy).
