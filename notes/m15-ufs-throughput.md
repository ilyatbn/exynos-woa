# m15 - UFS throughput (Windows ~16-25 MB/s vs Linux 400-600 MB/s)

Task: diff Samsung 9820 Linux ufs-exynos init vs Beyond0LteUfsHcDxe.c + Exynos9820Ufs.c to find what
throttles data movement (~64 ns/byte, 64 KiB read = ~4 ms doorbell->completion at HS-G3x2 RateB).
Sources: LineageOS android_kernel_samsung_exynos9820 @ lineage-22.2
(drivers/scsi/ufs/ufs-exynos.c/.h, ufs-cal-9820.c/.h, ufs-exynos-fmp.c, drivers/crypto/fmp/fmp.c,
arch/arm64/boot/dts/exynos/exynos9820.dts). No phone access in this task; no commits.

## Progress log
- 2026-10-11: fetched kernel sources (ufs-exynos.c 1298 l, ufs-exynos.h 675 l, ufs-cal-9820.c 1347 l,
  ufs-exynos-fmp.c, fmp.c, ufshcd.c) to scratchpad.
- DT (exynos9820.dts:987-1083): reg HCI 0x13D60000/0x200, **vendor 0x13D61100/0x200** (HCI_* vendor
  offsets are relative to 0x13D61100), UNIPRO 0x13D50000/0x8000, UFSP 0x13D70000/0x100, PHY 0x13D64000/0x800,
  ufs-io-coherency 0x13C10700 mask=bits=BIT22|BIT23, ufs-cport 0x13D68000/0x804; pmd lane 2 gear 3;
  ufs-pm-qos-int = 400000; dma-coherent; fmp-id 0, smu-id 0; hw-rev UFS_VER_0006; evt-ver 1.
- Linux exynos_ufs_init_host (ufs-exynos.c:455-487): auto_hci_clk off (FORCE_HCS bit4 clr);
  V2P1_CTRL |= IA_TICK_SEL(bit16); 1US_TO_CNT_VAL = mclk/1e6; DATA_REORDER=0xA; TXPRDT=0x8000000C;
  RXPRDT=0x0000000C (**Linux also does NOT set RX prefetch** - our live values match Linux);
  UTRL/UTMRL_NEXUS_TYPE=0xFFFFFFFF; AXIDMA = WLU_EN|BURST_LEN(3) = 0x98000003 (matches live);
  IOP_ACG_DISABLE bit0 cleared.
  => TXPRDT/RXPRDT/REORDER/AXIDMA are NOT the suspect (all equal Linux).
- Per-request: set_nexus_t_xfer_req (ufs-exynos.c:826) sets bit<tag> in UTRL_NEXUS_TYPE for SCSI cmds,
  clears for dev-mgmt (query/NOP).
- Clock control: pre_setup_clocks(on) (692): auto_hci_clk off, gate_clk(false) = CLKSTOP_CTRL &= ~0x17,
  hwacg off (UFS_ACG_DISABLE bit0 = 1). link_startup PRE: ERROR_EN_DL/N/T, ctrl_clk(true) =
  FORCE_HCS |= 0xDE0 (CLK_STOP_CTRL_EN_ALL), gate_clk false, hwacg off.
- CAL (ufs-cal-9820.c): DT evt-ver=1, brd-for-cal=1 (SMDK) -> init_cfg_evt1 (pre-link, l.226),
  post_init_cfg_evt1_g3 (post-link, l.388; max_gear=min(PA_MaxRxHSGear, 3)), calib_of_hs_rate_b
  (pre-PMC l.549) + post_calib_of_hs_rate_b (l.574), post_h8_enter (l.587) / pre_h8_exit (l.599).
  Unipro MIBs written: pre-link 0x155E=0, 0x3000=0 (N_DeviceID), 0x3001=1 (N_DeviceID_valid),
  0x4021=1 (T_PeerDeviceID), 0x4020=1 (T_ConnectionState); UNIPRO 0x44 = UNIPRO18_MCLK_PERIOD
  (16e9/mclk). post-link: 0x15D2/0x15D3 adapt length fixup, 0x9529=1/0x15A4=0x3E8/0x9529=0,
  0xA006=0x80000000 ... 0xA006=0 around PMA writes; then calib_hibern8: peer PA_TActivate(0x15A8)
  = local+1 if >= RX 0x8F cap, local PA_Hibern8Time(0x15A7) += 1.
  pre-PMC HS rate B: 0x15D4=1, DL_FC0ProtectionTimeOutVal 0x2041=8064, DL_TC0ReplayTimeOutVal
  0x2042=28224, DL_AFC0ReqTimeOutVal 0x2043=20160, PA_PWRModeUserData0..2 (0x15B0..2)=12000/32000/16000,
  UNIPRO APB 0x7888/0x788C/0x7890/0x78B8/0x78BC/0x78C0 = same values; PMA TRSV 0x418=0x3C (G3 L2),
  0x4E8=0x03; post-PMC: EMB_CDR_WAIT 0x7b8 mask 0x08, 0x4E8=0x01.
  PMA TRSV lane stride = 0x400 in CAL (PHY_PMA_TRSV_ADDR) vs 0x140 in ufs-exynos.h (older).
- PRDT (ufshcd.c:2634-2660): 9820 uses QUIRK_PRDT_BYTE_GRAN: prd_table_length = nseg*16 + 16 (bytes,
  CONFIG_SOC_EXYNOS9820 special case), entries strided by hba->sg_entry_size = sizeof(struct
  fmp_table_setting) when CONFIG_SCSI_UFS_EXYNOS_FMP (ufs-exynos-fmp.c exynos_ufs_fmp_config), each
  entry size = len-1, max segment 256 KiB (PRDT_DATA_BYTE_COUNT_MAX); fmp bypass clears FAS/DAS in des3.
  UTRD offsets in bytes (ufshcd.c:4424). Interrupt aggregation on (cnt nutrs-1, timeout 0x01).
  FMP: exynos_fmp_sec_cfg -> SMC FMP_SECURITY(fmp 0, CFG_DESCTYPE_3) + SMC SMU_INIT(smu 0) (fmp.c:704).
- PMA access (ufs-exynos.h:615-633): phy_pma_writel clears HCI_CLKSTOP_CTRL bit3 (MPHY_APBCLK_STOP), writes
  reg_pma(0x13D64000)+addr, sets bit3 again. CAL lane stride PHY_PMA_TRSV_ADDR = reg + 0x400*lane
  (ufs-cal-9820.c:63) - so lane1 regs live at 0x13D64400+ (e.g. lane1 0x418 -> 0x13D64818).
  ufs-exynos.h:452 has a stale 0x140 stride macro, but the CAL (which does all the writes) redefines... NOTE: CAL
  file defines its own PHY_PMA_TRSV_ADDR under #ifndef _UFS_CAL_ - check which one wins when included.
- OUR UEFI PMA (Beyond0LteUfsHcDxe.c:984,1170-1230 Beyond0LtePmaInit; 1550-1850 / 3750-3850 HS cal): ports the
  **exynos9810** ufs-cal-9810 PMA tables (COMN 0x8C/0x74, TRSV 0x0C8/0x0F0/0x110/0x120/0x128/0x12C/0x134/
  0x164/0x16C/0x178/0x1B0/0x0E0, lane stride 0x140). The 9820 CAL (ufs-cal-9820.c) uses a DIFFERENT PHY map:
  COMN 0x000-0x3FF (latch 0xFC: 0x10/0x18/0x00), TRSV lane0 0x400-0x7FF, lane1 +0x400. => every 9810 TRSV
  write we do lands in the 9820 PHY's COMMON block (0x0C8..0x2F0), and none of the 9820 TRSV tuning is applied.
  (Suspect #1 candidate: PHY mistune -> DL retries/replays, or RX CDR margin.)
- OUR UEFI pre-link DME (Beyond0LteUfsHcDxe.c:1337-1359) = 9810 table: PA_DBG_CLK_PERIOD(0x9514)=ClkPrd(read back
  from sboot, ~10), PCS RX 0x12/TX 0xAA = ClkPrd, RX 0x5C=0x38, 0x0F=0, 0x65=1, 0x69=1, 0x21=0, 0x22=0, 0x84=1,
  TX 0x04=1, TX 0x8F=0x3E, **0x9536 (PA_DBG_AUTOMODE_THLD)=0x4E20, 0x9564 (PA_DBG_OPTION_SUITE)=0x2E820183 (9810
  value)**, then 0x155E/0x3000/0x3001/0x4021/0x4020 like Linux. Post-link (1377-1387): 0x15A4=0xFA (Linux 0x3E8),
  RX 0x35=5, 0x73=1, 0x41=2, 0x42=0xAC (9810). Linux 9820 init_cfg_evt1 instead: UNIPRO APB 0x44 = 16e9/mclk,
  RX 0x12/TX 0xAA = round(1e9/mclk), TX 0xA9=2, TX 0xAB..0xAD = TX line-reset period, RX 0x11=0, RX 0x1B..0x1D =
  RX LR detect period, RX 0x2F=0x79, RX 0x84=1, TX 0x04=1, RX 0x25=0xF6; NO 0x9536/0x9564 writes.
  Missing vs Linux: TX 0xA9, LR periods, RX 0x2F, RX 0x25, RX 0x11, UNIPRO APB 0x44, 0x15D2/0x15D3 adapt fixup,
  0xA006 around PMA, all 9820 PMA rows.
- DL timers / PWRModeUserData (UEFI 1403-1408, 1573-1598, 3768-3779) MATCH Linux calib_of_hs_rate_b values.
- **IO coherency mismatch**: UEFI Beyond0LteUfsHcDxe.c:672-675 (PreHce) CLEARS 0x300 and :483 SETS 0x300
  (BIT8|BIT9) at EXYNOS_UFS_IOCOH_BASE 0x13C10700 - that is the **9810** mask (0x11010700 BIT8|9). 9820 DT
  (exynos9820.dts:1069-1078, and live DT) = mask/bits BIT22|BIT23 (0x00C00000). So on the 9820 we never enable
  UFS IO coherency and we flip two unrelated FSYS0 sysreg bits. Correctness/coherency item; not obviously a
  30x throughput cause (coherent DMA is not faster), but it is a real 9810 leftover.
- HWACG / clock-stop: UEFI matches Linux (FORCE_HCS=0xDE0 = Linux CLK_STOP_CTRL_EN_ALL; UFS_ACG_DISABLE|=1;
  IOP_ACG_DISABLE bit0 cleared; CLKSTOP_CTRL &= ~0x17). Not a suspect.
- Nexus type: Windows sets InitialNexus|slot bits (Exynos9820Ufs.c:5355/6447/7904) = Linux semantics. OK.
- Linux debug dump list (ufs-exynos-dbg.c:27-260) = the exact registers Linux itself reads: std HCI, VS HCI
  0x00..0x180 incl FSM_MONITOR 0xC0, PRDT_HIT_RATIO 0xC4, DMA0/1_MONITOR 0xC8-0xD4, AXI_DMA_IF_CTRL 0xF8;
  UFSP 0x000/0x008/0x010/0x100/0x108/0x200..; UNIPRO 0x7850.., 0x7B00/7B04/7B20/7B24, 0x7C00 (DME_DBG_OPTION_SUITE),
  0x7D00/7D14/7D18; PMA COMN 0x114/0x118/0x11C, TRSV_L0 0x714/0x7B0-0x7BC/0x548/0x54C/0x550/0x564/0x7C8,
  TRSV_L1 0xB14/0xBB0-0xBBC/0x948/0x94C/0x950/0x964/0xBC8 (confirms lane1 = +0x400); DME attrs 0x1520..0x15D5,
  0x9500/9501/9502/9504/9564/956A/956D/9570/9595-9597, 0x2047/0x2067, 0xA000/A005/A007/A010/A011/A020-A025.
- Windows PRDT length (Exynos9820Ufs.h:1385 UFS_UTRD_PRDT_WORD_FOR = Entries*128 bytes) vs Linux 9820 special
  case ufshcd.c:2635-2639 `prd_table_length = sg_segments*sizeof(struct ufshcd_sg_entry) + 16` (= nseg*16+16)
  while entries are still strided by sg_entry_size = 128 (defconfig exynos9820-beyond0lte: SCSI_UFS_EXYNOS_FMP=y,
  SCSI_UFS_EXYNOS_SMU=y, EXYNOS_FMP=y). So Linux tells the 9820 HCI (nseg+1)*16; we tell it nseg*128 (8x). The
  Windows header comment says it copies Linux here; it missed the CONFIG_SOC_EXYNOS9820 branch. Also Linux keeps
  max segment 256 KiB (ufshcd.c:5743) while our driver needs one 4 KiB entry per PRDT (an 8 KiB entry ->
  HOST_FATAL after 4 KiB, Exynos9820Ufs.h:876-891) - possibly the same mismatch. Not a proven 30x cause.
- Windows UTRD: no interrupt bit, polled (fine); UCD zeroed per command (4 KiB uncached, fixed cost, not per-byte).
- Windows driver never reads UECPA/UECDL (0x38/0x3C), so the latches are informative from s10ediag.
- Linux unipro.h confirms DL ids: 0x2002 DL_TC0RxInitCreditVal, 0x2040 DL_TC0TXFCThreshold, 0x2041/2/3 timers,
  0x2044 DL_AFC0CreditThreshold, 0x2045 DL_TC0OutAckThreshold, 0x2047 DL_PeerTC0RxInitCreVal; 0x155E =
  PA_Local_TX_LCC_Enable; 0x15A4 PA_SaveConfigTime. ufshci.h: UECPA 0x38, UECDL 0x3C (b1 TCx replay timer,
  b2 AFC timer, b3 FC prot timer, b5 RX_BUF_OF, b13 PA_INIT, b0 NAC, b31 valid), UECN 0x40, UECT 0x44, UECDME 0x48.

## Ranking (reasoning)
Measured: cost linear in bytes (~60 us per KiB), reads AND writes equally slow, data correct, link HS-G3x2B.
=> a per-byte, direction-symmetric, transparent mechanism: DL-layer error recovery (NAC/replay/PA line-reset
re-init) on a marginal PHY is the best fit. HCI vendor DMA regs (PRDT/REORDER/AXI/ACG/clock-stop) all equal Linux.
1. PHY: UEFI applies the 9810 PMA+PCS cal (wrong register map, wrong lane stride, wrong COMN regs) instead of
   ufs-cal-9820 init_cfg_evt1 / post_init_cfg_evt1_g3 / calib_of_hs_rate_b / post_calib_of_hs_rate_b /
   pre_h8_exit. Confirm with UECPA/UECDL latches around a big raw read + PMA readbacks.
2. 9810 UniPro/PA vendor debug MIBs written on a UniPro 1.8 IP: 0x9564=0x2E820183, 0x9536=0x4E20, 9810 PCS RX rows
   (0x5C/0x0F/0x65/0x69/0x21/0x22, TX 0x8F, post 0x35/0x73/0x41/0x42), 0x15A4=0xFA; and Linux's UNIPRO APB 0x44
   period never written. Confirm: DME_GET 0x9564/0x9536, peek 0x13D57C00, 0x13D50044.
3. Windows PrdtLength encoding (nseg*128 vs Linux nseg*16+16) + forced 4 KiB PRDT segmentation.
4. IO coherency bits 0x300 vs 0x00C00000 (correctness; not a speed lever by itself).
No code change prepared: none of these is proven, and #1's fix is a full 9820 CAL port of a boot-critical path.

## Confirming reads (all addresses = ones Linux itself writes or dumps)
A. Link errors (s10ediag, 32-bit): peek 0x13D60038 (UECPA) and 0x13D6003C (UECDL) [read-to-clear: peek once to
   clear, run the raw 64 KiB read test, peek again]; also 0x13D60040/44/48 (UECN/UECT/UECDME), 0x13D60020 (IS, bit2
   UE), UNIPRO 0x13D57B00 / 0x13D57B04 / 0x13D57B20 (DME_INTR_STATUS_LSB/MSB, DME_INTR_ERROR_CODE).
   bit31 set in UECDL with b0/b1/b2/b13, or UECPA 0x8000001x after the test => link retries => suspect 1/2.
   All zero => link clean; move to DMA side: 0x13D611C0 FSM_MONITOR, 0x13D611C4 PRDT_HIT_RATIO, 0x13D611C8/CC
   DMA0_MONITOR_STATE/CNT, 0x13D611D0/D4 DMA1, 0x13D611F8 AXI_DMA_IF_CTRL (sample several times mid-transfer).
B. Debug MIBs: peek 0x13D57C00 (UNIP_DME_DBG_OPTION_SUITE; 0x2E820183 = our 9810 write landed), 0x13D50044
   (UNIPRO18 mclk period; Linux writes 16e9/mclk_rate), 0x13D6110C (1US_TO_CNT = mclk MHz on Linux), 0x13D6118C
   (V2P1_CTRL bit16), 0x13C10700 (expect bits 22|23 set as on Linux; bits 8|9 = our 9810 write).
   DME_GET via Beyond0LteDme: 0x9564, 0x9536, 0x9514, 0x15A4 (Linux 0x3E8), 0x15A7, 0x15A8, 0x2002, 0x2040,
   0x2044, 0x2045, 0x2047; PCS (gate 0x200=0x40 like Linux): RX sel 4/5 0x2F (Linux 0x79), 0x25 (0xF6), 0x11 (0),
   0x84 (1), 0x12; TX sel 0/1 0xA9 (2), 0x04 (1), 0xAA.
C. PMA (needs HCI_CLKSTOP_CTRL 0x13D611B0 bit3 MPHY_APBCLK_STOP CLEAR during the access, exactly as Linux
   phy_pma_writel/readl does; peeking a gated APB slave may stall the bus): Linux-written values after PMC
   (lane1 = +0x400): TRSV 0x13D64418 =0x3C, 0x13D644E8 =0x01, 0x13D646D8 =0xC0, 0x13D64404/408 =0x06,
   0x13D64410 =0x01, 0x13D64414 =0x11, 0x13D6464C =0x06, 0x13D64548/54C/550 =0x2D/0xB7/0x00, 0x13D64460 =0x18,
   0x13D64558/568 =0x00; COMN 0x13D64000 =0xC6, 0x13D64004 =0x0C (ours writes 0x00 last), 0x13D64018 =0xE0,
   0x13D6409C/0A0 =0x11, 0x13D6410C =0x00. Linux dump regs: 0x13D64114/118/11C, 0x13D64714, 0x13D647B0..7BC.

## Read A/B results (run 33, 2026-10-11 16:20, boot #63 + w34)
- UECPA 0x13D60038, UECDL 0x3C, UECN 0x40, UECT 0x44, UECDME 0x48 = 0 before AND after the raw-read burst
  (4 KiB 602 us, 64 KiB 5.0 ms, 1 MiB 53 ms); IS 0x20 = 0x1 (UTRCS) both times => NO link-layer errors/retries.
  Suspects 1/2 (PHY tables, UniPro debug writes) are unlikely to be the throughput cause.
- 0x13D57B00 = 0, 0x13D57B04 = 0x00050000, 0x13D57B20 = 0, 0x13D57C00 = 0x2 (not the 9810 0x2E820183),
  UniPro clock period 0x13D50044 = 0x60 and HCI 1US_TO_CNT 0x13D6110C = 0xA6 (166 MHz; 16000/166 ~ 96 = 0x60 -
  consistent with Linux's formula), IO coherency 0x13C10700 = 0x311 (9810 bits 8/9 set, 9820 bits 22/23 clear).
- Still open: DMA side - PRDT length encoding (suspect 3), a SYSMMU in front of FSYS0 (iomem /sysmmu@13C20000),
  DMA engine state mid-transfer (0x13D611C0..F8).

## 2026-10-12 evening: probe step u2 (read-only, built, not yet run)
- s10eperf gains `sweep <vol>` (512 unbuffered reads per size, 4..64 KiB in 4 KiB steps) and `loop <vol> <s>`
  (64 KiB background load for register sampling). Built into s10e-artifacts\s10ediag-m18 (build.bat rc 0).
- drivers/S10eDiag/esp/extra-ufsprobe2.cmd: iocoh 0x13C10700; CMU_FSYS1 0x13C00100/180/1A0, 0x13C01000, 0x13C01800;
  CMU_TOP 0x1A241068/078/864, 0x1A242000; UFSP 0x13D70000/08/10; HCI VS 0x13D61110-124/130/138/15C; sweep;
  DMA monitors 0x13D611C0-DC/F8 x6 under load; UEC + INVALID_PRDT/VS_IS after. Addresses from
  notes/m15-ufs-research-clocks.md (cmucal-sfr.c) and m15-ufs-research-dma.md (ufs-exynos-dbg.c). No PMA reads.
- Separate finding (debug_history, 2026-10-12): entry 717 at power-on 16:13 records "(S) OCP_B1,OCP_B5" - the
  phone that went unreachable after the KD try (boot #104) had powered OFF on a sub-PMIC buck over-current
  trip. Which rails sub bucks 1/5 feed is not named in the dtbo (s_vdd_buck1..). Recorded in HANDOFF §32.
- UEFI speed split: Beyond0LteBootLogDxe BEYOND0LTE_UFS_SPEED_TEST (commit "m15 UFS: BEYOND0LTE_UFS_SPEED_TEST") times
  whole-disk ReadBlocks 4 KiB..1 MiB at ReadyToBoot -> "UfsSpeed:" lines in UEFILOG. Images (built from main, = #107 +):
  boot109-ufsspeed-v2.img (4f738af3…, speed test 1) and boot108-iocoh-v2.img (7a138c7e…, speed test 1 + IOCOH_9820 1;
  replaces the earlier 59ed2abe… #108). FVMAIN_COMPACT 1272 B free. Review: read-only, LBA = I*Size/BlockSize from 0,
  max 16 MiB span, page-aligned buffer; Status is always set (Reads >= 16).

## 2026-10-12 ~21:00: u2/u3 results and w35
- Boot #109/#110 UEFI UfsSpeed (same controller, same UEFI config Windows inherits): 4 KiB 237 us, 8 KiB 266, 16 KiB
  404, 32 KiB 432, 64 KiB 733 us (89 MB/s), 128 KiB 1163 us (113 MB/s), 1 MiB 11.2 ms (94 MB/s). 64 KiB into a high
  buffer (0x97CAD2000), a bank0 buffer (0x9FFF0000) and the Windows UFS DMA pool (0xA7428000): all 733 us => the pool
  region is not slow. UEFI uses the same DW7 n*128 / 4 KiB-entry encoding (Beyond0LteUfsHcDxe.c:3184-3191, 3576) =>
  the PRDT length suspect does not explain Windows (it may explain UEFI vs Linux 617 MB/s; separate).
- u2 Windows probes (fresh install, background I/O): iocoh 0x311; CMU_FSYS1 BUS_USER mux 0x10 (bus from CMU_TOP, not
  OSCCLK), UFS_EMBD_USER 0x10, PLL_MMC 0x801F0110, MUX_CLK_FSYS1_BUS 0 (BUS_USER), DIV_CLK_FSYS1_BUS 1 (/2);
  CMU_TOP MUX_CLKCMU_FSYS1_BUS 1 (400 MHz => FSYS1 bus 200 MHz), MUX_UFS_EMBD 3 (Linux 1), DIV_UFS_EMBD 1 (Linux 2),
  GAT_FSYS1_BUS 0x00200000. UFSP 0x00000000/0x00000004/0xFFFA6492 (DESCTYPE[20:19]=3). HCI VS: 0x118 0xBEADCAFE,
  0x120 0x8000, 0x124 0x10000, 0x138 VS_IS 0x00080000, 0x15C 1. DMA monitors static (0x1C4 0xAB01FFFF, 0x1D8
  0x04000800). UEC* all 0 after load. sweep (noisy, first-boot background I/O): 4 KiB 1.3-2 ms ... 64 KiB 8.7 ms.
- u3 (w34 UfsDiag counters around a 15 s 64 KiB loop): loop 4152 x 64 KiB; counter deltas READS +34057, bytes
  +281.4 MB, DEVICE +3.89 s, BOUNCE_IN +1.30 s. => each 64 KiB request reaches the miniport as ~8 commands of ~8 KiB
  (device 114 us + copy 38 us each) plus ~290 us per command outside device+copy. MAX_TRANSFER 0x10000, BREAKS 33,
  DMA32=1, DMA64=0, NeedPhysicalAddresses TRUE, MapBuffers ALL. Cause (inferred, UNVERIFIED until w35 runs): with
  Dma32 only, StorPort double-buffers SRB buffers above 4 GB through HAL map registers and splits them.
- w35 = w34 + UFS_DMA64_WITH_RESERVED_WORKSPACE 1 (Exynos9820UfsDiag.h): ConfigInfo->Dma64BitAddresses =
  SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED only when the firmware-reserved workspace was mapped (the miniport never
  DMAs from Srb->DataBuffer; the StorPortGetUncachedExtension fallback keeps Dma32). Exact build OK, signed with
  WDKTestCert 5DD5DEE1… (same signer as w34), s10e-artifacts\ufs-w23\Exynos9820Ufs-w35.sys sha 3ed49b6a….
  Installed by esp/extra-ufsw35.cmd inside Windows (rename w34 to .w34, copy w35), measured next boot by ufsprobe3.
- w35 measured (boot w35-measure3): Dma64 advertised (DMA64=0x2) but 4151 x 64 KiB volume reads still = 34074
  commands => Dma32 map registers were NOT the cause (w35 kept; harmless). u4: StorageAdapterProperty on
  PhysicalDrive0 and C: = MaximumTransferLength 0x10000, MaximumPhysicalPages 17, AlignmentMask 3; PhysicalDrive0
  loop also ~8.2 KiB per command => the split is in classpnp/disk, not volume filters. Linux (TWRP) queue limits:
  max_hw_sectors_kb 512, max_segments 128, max_segment_size 4096, lbs 4096; Linux exposes no vpd_pgb0.
- u5 (s10eperf vpd, SCSI pass-through): VPD 0x00 = 00 80 83 87 b0 b1 b2; **0xB0 Block Limits: MAXIMUM TRANSFER
  LENGTH 0, OPTIMAL TRANSFER LENGTH 2 blocks (8 KiB), granularity 2**, max unmap LBA count 0xFFFFFFFF; 0xB1 rate 1.
  ROOT CAUSE of the 8 KiB split: classpnp splits to the device's OPTIMAL TRANSFER LENGTH.
- w36 = w35 + UFS_CLEAR_VPD_OPTIMAL_TRANSFER (zero bytes 12..15 of the VPD 0xB0 reply), sha 070ae94c…, installed by
  esp/extra-ufsswap.cmd (swap in Windows + clean `shutdown /r` + measure on the 2nd pass; TWRP ntfs-3g refuses rw
  on the flushreset-dirty volume and the fsck fallback is what corrupted NTFS before). Results (boot w36):
  C: 64 KiB loop 4899 in 10 s (32 MB/s; was 4151 in 15 s = 18 MB/s), commands 5624 for 4899 requests (~1:1),
  raw C: 4 KiB 316 us (was 555), 64 KiB 2188 us (was ~4000), 1 MiB 49.2 MB/s (was 19.7).
  Counters per 64 KiB command: DEVICE ~128 us (!), BOUNCE_IN copy-back ~274 us => ~1.6 ms per request is spent
  outside device+copy (completion is synchronous in StartIo, :8195). PhysicalDrive0 loop oddly slow: 1423 x 64 KiB
  in 10 s with 1686 commands (~7 ms/request) - open.
- Next levers: (1) find the ~1.6 ms per request outside the miniport (StorPort/MDL mapping/upper stack - needs a
  StartIo-entry-to-completion counter or a KD/ETW trace); (2) copy-back 274 us per 64 KiB (DC CIVAC over 1024 lines +
  memcpy) - DMA straight into the SRB buffer would remove it; (3) raise MaximumTransferLength past 64 KiB (UCD/PRDT
  region); (4) UEFI vs Linux gap (UEFI 733 us per 64 KiB vs Windows device 128 us - UEFI has its own overheads).
