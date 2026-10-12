# m15 - UFS throughput research: clocks / bus / FMP-SMU / SYSMMU (2026-10-12)

Task: why Windows UFS (QD1 polled, ~20 MB/s, ~60 us per KiB, reads = writes) is ~30x slower than Linux
(TWRP dd 617 MB/s bs=1M) on the same S10e, focusing on CMU_FSYS0/CMU_TOP clocks, bus QoS/BTS/DVFS, FMP/SMU
(UFSP 0x13D70000), SYSMMU, IO coherency. Research only: no device access, no source edits, no commits.
Prior notes: notes/m15-ufs-throughput.md, notes/m15-perf.md (read in full first).
Kernel sources (untrusted data): LineageOS android_kernel_samsung_exynos9820 @ lineage-22.2, copies in
scratchpad ac99.../k/ (prior task) and b82d.../scratchpad/ufs-r1/ (this task).

## Progress log
- Start: read m15-ufs-throughput.md + m15-perf.md. Baseline facts: 4 KiB 555 us, 64 KiB ~4.0 ms, 1 MiB 53 ms
  => ~207-230 us per additional 4 KiB (linear in 4 KiB pages = linear in PRDT entries, since the miniport
  uses one 4 KiB PRDT entry per segment - note this cannot distinguish per-byte from per-PRDT-entry cost).
- CORRECTION to the task framing: on the 9820 the embedded UFS is in **BLK_FSYS1**, not FSYS0. cmucal-sfr.c:11
  SFR_BLOCK(CMU_FSYS1, 0x13c00000) (also EXYNOS_CMU_FSYS1_BASE in Exynos9820.h:532, VERIFIED); CMU_FSYS0
  0x13000000 (cmucal-sfr.c:26) holds USB/DP. clk-exynos9820.c:203,218-219,522: GATE_UFS_EMBD = UFS_EMBD_QCH,
  GATE_UFS_EMBD_FMP = UFS_EMBD_QCH_FMP, parent UMUX_CLKCMU_FSYS1_UFS_EMBD; UFS_EMBD = VCLK_CLKCMU_FSYS1_UFS_EMBD.
- UFS_EMBD clock tree (cmucal-node.c, cmucal-sfr.c; files in scratchpad ufs-r1/):
  * I_ACLK (AXI master/slave of the HCI) and I_FMP_CLK are fed by DIV_CLK_FSYS1_BUS (node.c:2051-2052);
    I_CLK_UNIPRO by MUX_CLKCMU_FSYS1_UFS_EMBD_USER (node.c:2053). SYSMMU_FSYS1 clk also DIV_CLK_FSYS1_BUS (:2042).
  * DIV_CLK_FSYS1_BUS <- MUX_CLK_FSYS1_BUS {0: MUX_CLKCMU_FSYS1_BUS_USER, 1: PLL_MMC} (node.c:600-603,1068,1344).
  * MUX_CLKCMU_FSYS1_BUS_USER {0: OSCCLK_FSYS1 26 MHz, 1: CLKCMU_FSYS1_BUS} (node.c:740-743,1175).
  * CLKCMU_FSYS1_BUS (CMU_CMU gate) <- MUX_CLKCMU_FSYS1_BUS {0 SHARED0_DIV4 533, 1 SHARED2_DIV2 400,
    2 SHARED4_DIV2 333, 3 SHARED3_DIV2 320} (node.c:420-425,1032,1622). No CMU_CMU divider on FSYS1_BUS.
  * MUX_CLKCMU_FSYS1_UFS_EMBD_USER {0 OSCCLK_FSYS1, 1 CLKCMU_FSYS1_UFS_EMBD} (node.c:756-759,1179);
    CLKCMU_FSYS1_UFS_EMBD = DIV(GATE_CLKCMU_FSYS1_UFS_EMBD(MUX_CLKCMU_FSYS1_UFS_EMBD {0 OSCCLK_CMU,
    1 SHARED0_DIV4, 2 SHARED2_DIV2, 3 SHARED4_DIV2})) (node.c:250-255,1003,1283,1621).
  * PLL rates (node.c:34-52): SHARED0 2132 MHz (DIV4 = 533), SHARED2 800 (DIV2 400), SHARED3 640 (DIV2 320),
    SHARED4 666 (DIV2 333). PLL_MMC rate table only 26 MHz (node.c:100-102).
  * Linux UFS_EMBD LUT: {177667 kHz, params {2,1}} = DIV ratio 2 (/3) of mux 1 (SHARED0_DIV4 533) = 177.667 MHz
    (cmucal-vclk.c:169-172,3436-3437; cmucal-vclklut.c:161-163).
  * Linux FSYS1 bus: MUX_CLK_FSYS1_BUS switch LUT {333000, 2, -1} (vclk.c:3744-3746, switch_vddi :3773) i.e. the
    FSYS1 bus is in the **vddi (INT) DVFS domain** (DIV_CLK_FSYS1_BUS is listed in cmucal_vclk_vddi[] vclk.c:7-69).
    The vddi LUT params (vclklut.c:6-20) do not pair sensibly with the 62-entry list by index (DIV_CLK_FSYS1_BUS
    would get 860000/650000/... which is a PLL rate) => index alignment of that table is UNVERIFIED; do not read a
    divider value from it. On 9820 the INT domain is set by ACPM (FREQ_REQ on ch 0), which we already did
    (INT 400000, m15-perf run 32) with no disk change.
- Register map for the live check (offsets cmucal-sfr.c; fields SFR_ACCESS):
  CMU_CMU (0x1A240000; NOTE Exynos9820.h maps only 0x1000 of it as EXYNOS_CMU_TOP_SIZE - cal-if says +0x8000):
   0x1A241068 CLK_CON_MUX_MUX_CLKCMU_FSYS1_BUS      SELECT[1:0] (sfr.c:138, :2983)
   0x1A242000 CLK_CON_GAT_CLKCMU_FSYS1_BUS          CG_VAL b21, MANUAL b20, AUTO b28 (sfr.c:728, :4507-4509)
   0x1A241078 CLK_CON_MUX_MUX_CLKCMU_FSYS1_UFS_EMBD SELECT[1:0] Linux 1 (sfr.c:109, :2896)
   0x1A241864 CLK_CON_DIV_CLKCMU_FSYS1_UFS_EMBD     DIVRATIO[2:0] Linux 2 (sfr.c:393, :3506)
   0x1A242080 CLK_CON_GAT_GATE_CLKCMU_FSYS1_UFS_EMBD CG_VAL b21 (sfr.c:727, :4504)
   0x1A240160/1A0/1C0/1E0 PLL_CON0_PLL_SHARED0/2/3/4: ENABLE b31 STABLE b29 M[25:16] P[13:8] S[2:0]
     (sfr.c:53-60, :2730-2753); expect M/P/S 328/4/0, 400/13/0, 320/13/0, 333/13/0.
  CMU_FSYS1 (0x13C00000, VERIFIED base):
   0x13C00100 PLL_CON0_MUX_CLKCMU_FSYS1_BUS_USER    MUX_SEL b4 (1 = CLKCMU_FSYS1_BUS, 0 = OSCCLK 26 MHz), BUSY b7
   0x13C00180 PLL_CON0_MUX_CLKCMU_FSYS1_UFS_EMBD_USER MUX_SEL b4 (1 = CLKCMU_FSYS1_UFS_EMBD) (sfr.c:231-240,:3196-3210)
   0x13C01000 CLK_CON_MUX_MUX_CLK_FSYS1_BUS          SELECT b0 (0 = BUS_USER, 1 = PLL_MMC) (sfr.c:174, :3091)
   0x13C01800 CLK_CON_DIV_DIV_CLK_FSYS1_BUS          DIVRATIO[3:0] (sfr.c:454, :3689)
   0x13C001A0 PLL_CON0_PLL_MMC (ENABLE b31; if MUX_CLK_FSYS1_BUS=1 this PLL is the bus source) (sfr.c:76,:2797-2802)
   0x13C02094/2098/209C GOUT UFS_EMBD I_ACLK / I_CLK_UNIPRO / I_FMP_CLK (CG_VAL b21, MANUAL b20) (sfr.c:1157-1159)
   0x13C02080 GOUT SYSMMU_FSYS1 clk (sfr.c:1148)
   0x13C03070/3074/3060 QCH_CON UFS_EMBD / UFS_EMBD_FMP / SYSMMU_FSYS1: ENABLE b0 CLOCK_REQ b1
     IGNORE_FORCE_PM b2 EXPIRE_VAL[25:16] (sfr.c:2268-2273, :9450-9473)
- Live hint already in hand: UNIPRO 0x13D50044 = 0x60 (m15-ufs-throughput run 33) and our code never writes it
  (Beyond0LteUfsHcDxe.c has no 0x44 UNIPRO write) => sboot programmed 16000/mclk_MHz = 96 => sboot's UniPro mclk
  ~166.5 MHz (= SHARED4_DIV2 333 / 2) vs Linux 177.667 MHz. HCI 1US_TO_CNT 0xA6 is OUR hardcoded constant
  (Beyond0LteUfsHcDxe.c:573 HCI_1US_CNT, "UNVERIFIED on 9820") - it carries no information about the live clock.
  A 6 % UniPro clock difference cannot explain 30x. The bus (ACLK) clock is the open question.
- Linux ufs-exynos.c clock handling (ufs-r1/ufs-exynos.c): NO clk_set_rate anywhere; only clk_get_rate(clk_unipro)
  (:377, :772) -> 1US_TO_CNT = mclk/1e6 (:390) and the CAL mclk period; clocks GATE_UFS_EMBD / UFS_EMBD (:563-566;
  DT exynos9820.dts:5873-5874 clocks <0x2 0x135 0x2 0x138>, freq-table-hz all 0). pm_qos: only
  PM_QOS_DEVICE_THROUGHPUT (= INT) = ufs-pm-qos-int 400000 while clocks on (:722, :1121, :1195);
  ufs-pm-qos-fsys0 parsed (:1124) but absent from the 9820 DT and never requested. => Linux does NOT program the
  FSYS1 bus / UFS_EMBD dividers itself; whatever they are comes from sboot (+ ACPM INT DVFS). Since our
  ACPM INT=400000 request changed nothing, the INT-domain bus is not the limiter UNLESS CMU_FSYS1's local
  muxes (BUS_USER / MUX_CLK_FSYS1_BUS) are on OSCCLK/PLL_MMC(26 MHz), which ACPM DVFS would not fix.
  The CMU_FSYS1 reads above settle it.
- MIF: ruled out as a 30x cause - Windows memcpy 4314 MB/s (m15-perf run 32) goes through the same MIF/DRAM.
- SYSMMU: the only FSYS1 SYSMMU is sysmmu@13C20000 compatible "samsung,pcie-sysmmu", port-name "PCIe", ch-num 0,
  clock 0x132 (k/exynos9820.dts:9167-9178; Exynos9820.h:522 EXYNOS_PCIE0_SYSMMU_BASE). The UFS node has NO
  `iommus` (exynos9820.dts:5864-5910) => Linux never translates UFS DMA; nothing in the UFS bring-up touches a
  SYSMMU. Its CMU gate is 0x13C02080 / QCH 0x13C03060. Whether UFS traffic even traverses it is UNVERIFIED (the
  9820 FSYS1 D-path has XIU_D_FSYS1 + LHS_ACEL_D_FSYS1 + SYSMMU_FSYS1 + VGEN_LITE_FSYS1 all on DIV_CLK_FSYS1_BUS,
  cmucal-node.c:2025-2043). A per-transaction page walk would cost ~1 us/4 KiB, not 200 us. Low suspicion.
- FMP/UFSP (fmp.c, ufs-exynos-fmp.c, k/ufs-exynos.h:235-248):
  * Linux exynos_fmp_sec_cfg (fmp.c:704-731): SMC FMP_SECURITY(0, fmp_id 0, CFG_DESCTYPE_3) then SMC SMU(SMU_INIT,
    smu 0) at init (SMU_RESUME on resume). UFSPRSECURITY 0x13D70010: DESCTYPE[20:19], NSSMU b14, AxPROT groups
    (ARPROTPRDT[17:16], ARPROTDESC[10:9], ARPROTDATA[7:6], AWPROTDESC[4:3], AWPROTDATA[1:0]).
  * Linux bypass (fmp.c:680-701, 640-649): per PRDT entry SET_FAS(0)/SET_DAS(0) = des3 bits 29:28 / 31:30 clear
    (fmp.h:146-158: UFS des3 = size[25:0], FKL b26, DKL b27, FAS[29:28], DAS[31:30]). sg_entry_size = 128
    (ufs-exynos-fmp.c exynos_ufs_fmp_config). DISK_KEY_STORED/SET only for EXYNOS_FMP_DISK_ENC requests
    (fmp.c:233-262, :405-412) - never at probe.
  * OUR UEFI Beyond0LteUfspConfig (Beyond0LteUfsHcDxe.c:286-418, called :1488) issues FMP_SECURITY(DESCTYPE 3),
    SMU_INIT, **SMU_RESUME, SMU_DUMP x2, FMP_DISK_KEY_STORED (fixed 32-byte key mA119FmpDiskKey :217-226),
    FMP_DISK_KEY_SET, UFS_LOG** - a 9810-era (A113-A120) superset of what Linux does. The extra disk-key SMCs
    load a global FMP disk key that Linux never loads at boot. Data reads are correct, so DAS=0 bypass holds;
    whether a loaded disk key changes the FMP pipeline timing is UNVERIFIED (no source describes it).
  * OUR miniport PRDT (Exynos9820Ufs.c:6056-6100 UfsProgramPrdt): 128-byte entries, des3 = (len-1) & 0x3FFFF, FAS/
    DAS/FKL/DKL = 0, tail zero; TRD DW0 has no crypto-enable bit. => FMP is bypassed per entry exactly like Linux.
    Differences that remain: PrdtLength = Entries*128 (Exynos9820Ufs.h:1385) vs Linux 9820 nseg*16+16
    (ufshcd.c:2634-2639); one 4 KiB entry per segment (an 8 KiB entry -> HOST_FATAL after 4 KiB,
    Exynos9820Ufs.h:876-891) whereas Linux allows 256 KiB entries (ufshcd.c:5743, ufshci.h:424) => our HCI/FMP
    does NOT behave like Linux's on the descriptor path. Most likely cause of BOTH symptoms: the PrdtLength encoding
    (HW in DESCTYPE 3 evidently derives the entry count from (PRDTL-16)/16, so Entries*128 declares 8n-1 entries) -
    UNVERIFIED; the HW semantics of PRDTL under DESCTYPE 3 are not in any source read.
- Linux CMU boot code (ufs-r1/cal_data.c:81-177, 229-268): exynos9820_cal_data_init only ioremaps CMU_FSYS1 as
  "cmu_mmc" (0x13C00000) for PLL_MMC SSC/enable helpers (PLL_CON0..3 0x1A0-0x1AC; used by the SD card path).
  exynos9820_vclk_init is empty (clk-exynos9820.c:548-551). => the Linux kernel never reprograms the FSYS1 bus or
  UFS_EMBD dividers/muxes at boot or UFS probe; sboot + ACPM own them. (Question 1 answer: Linux does not
  "raise" anything in CMU_FSYS1 itself; if the live bus is slow under Windows, it is because sboot/ACPM left it
  so and Linux's INT DVFS (ACPM) moved it.)
- Stock DT (Dumps/extracted/dts/dt.img.0.dts, Odin AP G970FXXSGHWC1): devfreq_int (:9008-9025) acpm-ipc-channel 1,
  dm-index 4, dfs_id 0xB040001, **boot_info <0x28 0x825F0> = INT boot level 534000 kHz**, freq_info max 534000,
  min 100000; devfreq_mif boot_info 2093000, bts_update "true" only for MIF (:8975-9006). No exynos-bts node in
  the 9820 DT (only gpu_bts_support :4672) => Linux programs NO FSYS-specific BTS/QoS register for UFS
  (Question 2: only PM_QOS_DEVICE_THROUGHPUT = INT >= 400 MHz while UFS clocks are on, MIF at 2093 MHz by boot).
  The vddi DVFS clock list (cmucal-vclk.c:7-69) contains DIV_CLK_FSYS1_BUS and PLL_MMC, i.e. ACPM's INT DVFS is
  what sets the FSYS1 bus divider. Our ACPM INT request (ch 0, idx per m15-perf) got an echo reply but its effect
  on the clock tree is UNVERIFIED (FREQ_GET returns 0; nobody read a CMU register after it). The stock DT uses
  acpm channel 1 for devfreq INT/MIF, while our request went on ch 0 (clock-controller acpm-ipc-channel 0) -
  both channels exist; which one ACPM honours for INT DVFS is UNVERIFIED (close: read 0x13C01800 / 0x13C01000 /
  0x13C00100 before and after the request).
- Odin dump: only DT/dtbo text was checked (kernel image not inspected - not needed, LineageOS cal-if matches
  the stock DT clock ids 0x135/0x138).
- QCH/HWACG: GATE_UFS_EMBD / GATE_UFS_EMBD_FMP are HWACG_VCLK(... VCLK_GATE) (clk-exynos9820.c:218-219) without
  VCLK_QCH_EN/DIS => cal_vclk_enable/disable return immediately (composite.c:1375-1376, 1405-1406) and
  cal_vclk_qch_init does nothing (composite.c:1581-1590). Linux never touches QCH_CON_UFS_EMBD(_FMP) or the
  FSYS1 CLK_CON auto-gating bits => sboot's QCH/HWACG state is identical under Linux and Windows. Not a suspect.
  Our UEFI touches no CMU register for UFS (only Beyond0LtePcieDxe writes CMU_FSYS1 PCIe gates, PcieLink.c:96-100).
- IO coherency (question 5): SYSREG_FSYS1+0x700 is the per-master SHARABILITY_CTRL; PCIe ch0 = bits [9:8]
  (Linux pcie-exynos v0.c:1885 / v0.h:297-300 as cited in our PcieLink.c:118-130), UFS_EMBD = bits [23:22]
  (DT mask/bits 0x00C00000, exynos9820.dts:5900-5906). Linux (ufs-exynos.c:512-547 exynos_ufs_init_system ->
  modify_sysreg) sets 22/23 and BUG()s if dma-coherent is missing. Our UEFI clears/sets **0x300 = the PCIe ch0
  field** (Beyond0LteUfsHcDxe.c:672-675 clear, :483 set; its comments still say "kernel-faithful coherent UFS DMA" /
  "0x311 => coherent DMA ON" - false on 9820). Live 0x13C10700 = 0x311 (run 33) => UFS master runs
  NON-shareable; this is one of the very few VERIFIED live Linux/Windows differences on the UFS data path. Speed
  effect UNVERIFIED: if the field also drives the master's AxCACHE (non-modifiable/non-bufferable when 0) the
  interconnect cannot merge/post the DMA bursts and every burst waits for a DRAM response - a latency-bound,
  per-byte, direction-symmetric slowdown, i.e. the measured shape. No source read defines the field beyond
  "sharability" - UNVERIFIED. Windows-side safety of turning it on: the miniport already does full cache
  maintenance (DSB + DC CIVAC before read copy-back, Device-memory view for writes/descriptors, m15-perf w34),
  which stays correct under coherent DMA (maintenance is a superset).
- Prior CMU_FSYS1 readings: none recorded (notes grep; Beyond0LtePcieDxe logs CMU_FSYS1+0x2000 but no value was
  ever captured in notes). CMU_FSYS1 IS readable from NS EL1 (PcieLink.c / Beyond0LtePcieDxe.c:92-103 read it).

## Ranked suspects (this task's scope: clocks / bus / FMP-SMU / SYSMMU / coherency)
Shape to explain: ~230 us per extra 4 KiB (= per PRDT entry AND per 4 KiB of data - indistinguishable with
4 KiB entries), reads = writes, no link errors, data correct. Linux ~8 us per 4 KiB.

1. **UFS IO-coherency/sharability field never set (0x300 PCIe ch0 bits written instead of 0x00C00000).**
   VERIFIED difference (live 0x13C10700 = 0x311; Linux sets bits 23:22). Mechanism for speed UNVERIFIED (field
   semantics beyond "sharability" not in any source read).
   Confirm: peek 0x13C10700 (now 0x311; Linux-equivalent = 0x00C00011 if sboot's low bits are kept). Then A/B: UEFI
   Beyond0LteUfsHcDxe.c:483 set 0x00C00000 (and :672-675 clear 0x00C00000 instead of 0x300; stop touching 0x300,
   which belongs to PCIe ch0 and is owned by Beyond0LtePcieDxe CacheCoherencyEnable) -> rerun s10eperf raw
   4K/64K/1M. Minimal fix = those two constants. Hazard: boot-critical UFS path - keep the change behind a build flag
   and a TWRP rollback image; the miniport's cache maintenance remains correct under coherent DMA.
2. **FSYS1 bus clock (I_ACLK / I_FMP_CLK / SYSMMU / XIU = DIV_CLK_FSYS1_BUS) on a slow source or divider.**
   Linux never programs it (no clk_set_rate; cal_data_init/vclk_init do nothing for it); it is set by sboot and
   by ACPM's INT (vddi) DVFS, whose LUT lists DIV_CLK_FSYS1_BUS and PLL_MMC. Stock INT boot level = 534000
   (dt.img.0.dts devfreq_int boot_info) => sboot most likely leaves it fast; our ch-0 INT request's effect was
   never measured. Medium-low prior, but 8 peeks settle it.
   Confirm (32-bit peeks; expected "Linux-like" in brackets):
     0x13C00100 PLL_CON0_MUX_CLKCMU_FSYS1_BUS_USER  bit4 [1 = CLKCMU_FSYS1_BUS; 0 = OSCCLK 26 MHz => FOUND]
     0x13C01000 CLK_CON_MUX_MUX_CLK_FSYS1_BUS       bit0 [0 = BUS_USER; 1 = PLL_MMC]
     0x13C001A0 PLL_CON0_PLL_MMC                    (only matters if the mux above = 1; M/P/S and ENABLE b31)
     0x13C01800 CLK_CON_DIV_DIV_CLK_FSYS1_BUS       [3:0] (bus = src/(n+1)); Linux value UNVERIFIED - read the
                Linux reference from TWRP (root): mount debugfs, cat /sys/kernel/debug/clk/clk_summary | grep -i fsys1
                (UMUX_CLKCMU_FSYS1_BUS / GATE_UFS_EMBD rates) - non-destructive.
     0x1A241068 CLK_CON_MUX_MUX_CLKCMU_FSYS1_BUS    [1:0] {533,400,333,320 MHz}
     0x1A242000 CLK_CON_GAT_CLKCMU_FSYS1_BUS        b21 CG_VAL / b20 MANUAL
     0x13C00180 PLL_CON0_MUX_CLKCMU_FSYS1_UFS_EMBD_USER bit4 [1]; 0x1A241078 MUX_CLKCMU_FSYS1_UFS_EMBD [1:0] [1];
     0x1A241864 DIV_CLKCMU_FSYS1_UFS_EMBD [2:0] [2] (=> 177.667 MHz; sboot's 0x13D50044=0x60 implies ~166.5 MHz).
   Bus clock = 26 MHz (BUS_USER=0, or MUX=1 with PLL_MMC off/26 MHz) or a divider >= 8 => this is it. Minimal fix:
   UEFI sets PLL_CON0_MUX_CLKCMU_FSYS1_BUS_USER bit4=1 / MUX_CLK_FSYS1_BUS=0 / DIV to the TWRP-read value
   (wait BUSY b7 / b16 clear), or an ACPM INT request on the channel ACPM honours (stock DT devfreq uses ch 1).
3. **FMP/UFSP and descriptor path not in Linux's state.** (a) UEFI issues extra secure calls Linux never makes at
   probe: SMU_RESUME after SMU_INIT, FMP_DISK_KEY_STORED (fixed key) + FMP_DISK_KEY_SET, UFS_LOG
   (Beyond0LteUfsHcDxe.c:357-379); (b) PrdtLength = Entries*128 vs Linux 9820 (nseg*16)+16 (ufshcd.c:2634-2639);
   (c) our HCI rejects >4 KiB entries while Linux uses up to 256 KiB. FAS/DAS are 0 (bypass) like Linux.
   Confirm: peek 0x13D70010 UFSPRSECURITY (expect DESCTYPE[20:19]=3, i.e. 0xEFFA6492 as on the 9810 WOADUMP) ,
   0x13D70000/0x13D70008; sample 0x13D611C0 FSM_MONITOR, 0x13D611C4 PRDT_HIT_RATIO, 0x13D611C8/CC/D0/D4 DMA0/1
   MONITOR during a 1 MiB raw read (a DMA monitor stuck in a PRDT-fetch state => descriptor path). A/B 1: UEFI
   without the DISK_KEY_STORED/SET/SMU_RESUME/UFS_LOG calls. A/B 2: miniport PrdtLength = n*16+16 with one
   entry per physically contiguous run (if >4 KiB entries then work, the 4 KiB-entry "FMP data unit" theory in
   Exynos9820Ufs.h:876-891 was an artefact of the length encoding).
4. SYSMMU: low. sysmmu@13C20000 is "samsung,pcie-sysmmu" (PCIe port), the UFS node has no iommus; Linux never
   configures a SYSMMU for UFS. Optional peek of its CTRL/CFG (offsets not read in any source this task) - skip.
5. Ruled out: MIF/DRAM (memcpy 4.3 GB/s), BTS (no BTS node in the 9820 DT; Linux sets none for FSYS), UniPro mclk
   (sboot ~166.5 vs Linux 177.667 MHz = 6 %), QCH/HWACG (Linux leaves sboot's state), INT pm_qos (already 400 MHz).
Out of scope but fits the shape (symmetric, per-byte, no error counters): DL flow-control credit/ack thresholds
- DME_GET 0x2002/0x2040/0x2044/0x2045/0x2047 vs Linux defaults (listed in m15-ufs-throughput read B, never run).
