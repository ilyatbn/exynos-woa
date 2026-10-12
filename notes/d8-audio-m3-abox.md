# D8 audio M3 — ABOX UAIF1 tone without the Calliope DSP (research)

Scope: can the AP alone (UEFI, later Windows) push a 48 kHz tone out of ABOX UAIF1 to the two
CS35L40 amps, with the ABOX DSP cores held in reset? Research only: no device access, no build,
no commit. `AB` = scratchpad\abox (files fetched raw from GitHub LineageOS/android_kernel_samsung_exynos9820
branch lineage-22.2, treated as data), `DTS` = scratchpad\dt\out_0_0.dts (decompiled stock DTBO),
`K` = scratchpad\k. Prior work: notes/d8-audio-linux.md Q2 (driver architecture), notes/d8-audio-design.md §2.

## Progress log

- 2026-10-12 start: notes file created; prior notes read (linux Q2 says vendor AP never writes
  RDMA BUF_STR; UAIF CTRL0/1 are AP-written). Fetching abox/*, cal-if/exynos9820/*, clock bindings.

- 2026-10-12 fetched into AB (all OK): abox/{abox.c 3093, abox.h, abox_if.c 885, abox_soc.c 1923, abox_soc.h 841, abox_util.*, abox_rdma.c 2472, abox_wdma.c, abox_core.*, abox_gic.*, abox_cmpnt_v10/v20.c, dbg, failsafe, qos, Kconfig}, beyond_madera.c, include/sound/samsung/abox.h (as abox_ipc_h.h), drivers/soc/samsung/exynos-pd.c, cal-if/exynos9820/{cmucal-node/qch/sfr/vclk/vclklut, flexpmu_cal_local/system/p2vmap, cal_data.c} (prefixed cal_), cal-if/{pmucal_local.c, pmucal_rae.c, cal-if.c} (prefixed calif_), dt-bindings/clock/exynos9820.h (dtb_clock_exynos9820.h), arch/arm64/boot/dts/exynos/exynos9820.dts.
- 2026-10-12 read abox_soc.h, abox_soc.c regmap/defaults/patch, abox_if.c fmt/hw_params/trigger, abox_cmpnt_v20.c routing enums → F1-F4.
- 2026-10-12 power/clock: fetched pm-domains/sysmmu/pmucal-dbg dtsi, exynos-el3_mon.c, linux/smc.h → F5-F7.
- 2026-10-12 abox.c enable path, clock math, DAI fmt parse, RDMA/SysMMU; fetched exynos990 ABOX (AB/e990) for DMA field layout → F8-F11.
- 2026-10-12 BCLK q-channel keep-alive, reset-default routing, DMA-free sources (codec tone loopback), gpb1 regs → F12-F13.
- 2026-10-12 wrote proposed sequence, hazards, open questions. Research complete (no device access, no build).
- 2026-10-12 self-review: corrected abox_if.c line refs and the SIFS_OUT_SEL field width (3-bit, not 4).

## Facts (with citations)

### F1 SoC version and which code paths apply
- CONFIG_SND_SOC_SAMSUNG_ABOX_VERSION defaults to 0x20001 for SOC_EXYNOS9820 (0x20000 only for
  EVT0) — AB/Kconfig:19-20. So every `#if ABOX_SOC_VERSION(2,0,1) > VERSION` branch is the
  *else* branch on our phone; abox_cmpnt_v20.c is the component built (AB/Makefile:3, Kconfig:32-36).

### F2 SFR map (ifr = 0x18C50000, EXYNOS_ABOX_IFR_BASE, Exynos9820.h:737) — AB/abox_soc.h
- System: VERSION 0x04, SYSPOWER_CTRL 0x10 / STATUS 0x14, SYSTEM_CONFIG0 0x20, REMAP_MASK 0x24,
  REMAP_ADDR 0x28, DYN_CLOCK_OFF 0x30, DYN_CLOCK_OFF1 0x34, QCHANNEL_DISABLE 0x38,
  ROUTE_CTRL0/1/2 0x40/0x44/0x48, TICK_DIV_RATIO 0x50, MO_CTRL 0x54 (lines 27-41).
- ROUTE_CTRL0 (2.x): ROUTE_SPUSM bits 27:24, ROUTE_DSIF 23:20, ROUTE_UAIF_SPK(x) = 4-bit field at
  bits 4x+3:4x (BASE 0, ITV 4) → **UAIF1 SPK source = ROUTE_CTRL0[7:4]** (lines 64-77).
- SPUS: CTRL0 0x200, CTRL1 0x204, CTRL2 0x208, CTRL3 0x20C, CTRL4 0x210, CTRL5 0x214,
  CTRL_FC1 0x218, SBANK_RDMA(x) 0x220+4x, SBANK_ASRC(x) 0x250+4x (2.x), SBANK_MIXP 0x280,
  SIFS_CNT(x) 0x290+4x (lines 106-131). SPUS_CTRL0 (2.0.1): per-RDMA chain field ITV 5 bits:
  bit 5x = ASRC enable, bits 5x+3:5x+1 = OUT (SIFS select), bit 5x+4 = IN (0 RDMA / 1 SIFSM)
  (lines 133-153); channels 6-11 live in SPUS_CTRL_FC1 0x218 with the same layout x-6
  (abox_cmpnt_v20.c:3172-3184, 3255+).
- SPUS_CTRL1 (2.0.1): SIFSM_IN_SEL 11:8; SIFS_OUT_SEL(x) = 3-bit field at bits (8+4x+2):(8+4x) (BASE 8, ITV 4, H=L+2, lines
  167-176) ; MIXP_FORMAT 4:0 (177-179). NOTE: SIFS_OUT_SEL(1) = bits 15:12 (8+4*1), SIFS_OUT_SEL(0)
  would overlap SIFSM_IN_SEL 11:8 — SIFS0 has no OUT_SEL mux (sifs0 only has an "out" switch,
  abox_cmpnt_v20.c:3327), i.e. SIFS0 is the mixer (MIXP) output. (inference from the macro + texts)
- SBANK value = (size_bytes << 16) | start_byte, fields SIZE 29:20, STR 13:4 → 16-byte units
  (lines 297-302; values in patch_9820, abox_soc.c:1818-1872).
- UAIF(x): CTRL0 0x500+0x10x, CTRL1 +4, STATUS +0xC → **UAIF1 CTRL0 = 0x18C50510, CTRL1 = 0x18C50514,
  STATUS = 0x18C5051C** (lines 304-309).
  CTRL0: START_FIFO_DIFF_MIC 31:28, START_FIFO_DIFF_SPK 27:24, DATA_MODE 4, IRQ_MODE 3,
  MODE 2 (1 = ABOX master, see F4), MIC_ENABLE 1, SPK_ENABLE 0 (lines 310-331).
  CTRL1: FORMAT 28:24, BCLK_POLARITY 23, WS_MODE 22, WS_POLAR 21, SLOT_MAX 20:18, SBIT_MAX 17:12,
  VALID_STR 11:6, VALID_END 5:0 (lines 332-356). STATUS: ERROR_OF_MIC 1, ERROR_OF_SPK 0 (357-363).
- RDMA(x) 0x1000+0x100x: CTRL0 +0 (BURST_LEN 22:19, ENABLE 0), CTRL1 +4, BUF_STR +8, BUF_END +0xC,
  BUF_OFFSET +0x10, STR_POINT +0x14, VOL_FACTOR +0x18 (23:0), VOL_CHANGE +0x1C, SBANK_LIMIT +0x20,
  STATUS +0x30 (PROGRESS 31, RBUF_OFFSET 27:12 on 2.0.1, RBUF_CNT 11:0) (lines 407-450).
- SPUS ASRC(x) 0x2000+0x100x (CTRL, IS/OS_PARA, DITHER_CTRL +0x20, SEED_IN/OUT, FILTER_CTRL)
  (452-463); WDMA base 0x3000 on 2.x (466-470); SPUM ASRC 0x4000 (508); CA32 core regs 0x5000/0x5080,
  CA32_STATUS 0x5100 (625-633); APF coef 0x7000 (636-640); MAX_REGISTERS 0x7170 (672-676).
- SYSREG (0x18C10000): L2_CACHE_CON 0x0000, MISC_CON 0x0504 on 9820 (lines 679-682).
- IRQ table (ABOX GIC numbering) includes IRQ_UAIF1_SPEAKER, IRQ_RDMAx_BUF_EMPTY, IRQ_RDMAx_ERR
  (lines 688-805) — these go to the ABOX-internal GIC (DSP side), not the AP GIC (inference: enum is
  used with abox_gic_register_irq_handler).

### F3 The router is AP-programmed through a cached MMIO regmap (corrects d8-audio-linux.md Q2)
- abox_soc_get_regmap: devm_regmap_init_mmio(sfr_base) with REGCACHE_RBTREE, reg_defaults_9820 and,
  right after init, regmap_multi_reg_write(patch_9820) (AB/abox_soc.c:1874-1923).
- The ABOX ALSA component binds that regmap (abox_cmpnt_v20.c:4095-4102: snd_soc_component_init_regmap),
  and the routing kcontrols are **plain SOC_DAPM_ENUM on SFRs**: "UAIF1 SPK" mux =
  SOC_ENUM_SINGLE(ABOX_ROUTE_CTRL0, ROUTE_UAIF_SPK_L(1)) with texts
  {RESERVED, SIFS0, SIFS1, SIFS2, SIFS3, SIFS4, RES, RES, RES, RES, RES, RES, SIFMS} (abox_cmpnt_v20.c:3354-3369);
  "SPUS INx" mux {RDMA, SIFSM} on SPUS_CTRL0 (3128-3184); "SPUS OUTx" demux on SPUS_CTRL0 OUT
  field, texts for 2.0.1 = {SIFS1, SIFS0, SIFS2, RESERVED, SIFS3, RESERVED, SIFS4} (3206-3212);
  "SIFSx" mux {SPUS OUT0..11} on SPUS_CTRL1 SIFS_OUT_SEL(x) (3301-3325); SPUSM {..UAIF0..3 at 8..11}
  on ROUTE_CTRL0 27:24 (3290-3299); RSRC/NSRC demuxes on ROUTE_CTRL2/1 (3405-3440).
  → DAPM writes these registers from the AP; the DSP is not in the routing write path.
- On runtime suspend/resume the AP restores all of them itself: regcache_cache_only + mark_dirty
  then regcache_cache_only(false) + regcache_sync (abox.c:2134-2141).
- reg_defaults_9820 (abox_soc.c:1290+; line = 1289 + row): 0x0000 = 0x41424F58 ("ABOX" IP index),
  0x0020 = 0x4444, 0x0024 = 0xFFF00000, 0x0028 = 0x18D00000, 0x0030 = 0x7FFFFFFF (all dyn-clock-off on),
  0x0034 = 0x1F, 0x0038 = 0, ROUTE_CTRL0/1/2 = 0 (UAIF1 SPK = RESERVED at reset), TICK_DIV 0x4E1,
  SPUS_CTRL4 = 0x32100000, SPUS_CTRL5 = 0xBA987654, UAIFx CTRL0 = 0x01000010 / CTRL1 = 0,
  RDMA0 CTRL0 = 0x00400000 (BURST_LEN 8), WDMA0 CTRL = 0x00400000. These are the driver's cache
  seeds; that they equal silicon reset values is INFERENCE (read IP_INDEX 0x0 == 0x41424F58 to confirm
  the block is alive before anything else).
- patch_9820 writes SPUM_CTRL0 = RSRC_RECP bit and every SBANK: RDMA(n) = 512 B at n*512,
  SPUS ASRC banks at 0x1800..0x2800, MIXP 128 B at 0x3000, SPUM RSRC/NSRC/RECP/ASRC banks
  (abox_soc.c:1818-1872). These SBANKs are ABOX-internal SRAM-buffer partitions — they must be
  written by us too (they are not reset values; reset = 0, abox_soc.c defaults rows 0x220-0x2xx).

### F4 UAIF driver (AP-only writes) — AB/abox_if.c
- startup: clk_enable(bclk) + clk_enable(bclk_gate) (abox_if.c:39-49).
- set_fmt (248-313): DSP_A → WS_MODE=1; I2S → 0. NB_NF → BCLK_POLARITY=1, WS_POLAR=0;
  IB_NF → BCLK_POLARITY=0, WS_POLAR=0 (so "bitclock-inversion" = BCLK_POLARITY 0).
  CBM_CFM (codec master) → MODE=0; CBS_CFS (codec slave, ABOX master) → MODE=1.
- hw_params (346-417): clk_set_rate(bclk, rate*channels*width); SBIT_MAX = width-1;
  non-I2S → VALID_STR = VALID_END = width-1 (I2S → 0/0); SLOT_MAX = channels-1;
  FORMAT = abox_get_format(width, channels) = (channels-1) | (w16:1, w24:2, w32:3) << 3 (abox.h:419-440).
- trigger START → CTRL0.SPK_ENABLE=1 (419-463); shutdown waits 600-1200 us then SPK_ENABLE=0 (465-482).
- set_tristate toggles QCHANNEL_DISABLE bit for ABOX_BCLK_UAIFx (316-343, abox.c:241-247);
  enum qchannel order: CCLK_CA7=0, ACLK=1, BCLK_UAIF0=2, BCLK_UAIF1=3, ... DSIF=6, ATB=7, ASB=8
  (abox.h:157-167) → UAIF1 bclk q-channel = QCHANNEL_DISABLE bit 3 (ABOX_L(QCHANNEL_DISABLE,0,3)=3).

### F5 Power domain pd-aud (PMU) — the only power switch on the AP side
- DT: `pd_aud: pd-aud@15861900 { reg = <0x15861900 0x20>; cal_id = <0xB1380000>; need_smc = <0x18c62000>;
  power-down-ok = <PD_OK_AUD>; }` (AB/exynos9820-pm-domains.dtsi:18-25). The abox node itself has no
  power-domains property (AB/exynos9820.dts:1235-1260); pd-aud is referenced by devfreq_aud
  (exynos9820.dts:5539-5540) — the vendor gets runtime power through the PD framework (INFERENCE:
  the abox platform device is attached to pd_aud elsewhere, e.g. by exynos-pd's device list; not traced).
- pmucal sequence (AB/cal_flexpmu_cal_local_exynos9820.h): aud_on = WRITE PMU+0x1900 (AUD_CONFIGURATION)
  bit0=1, WAIT PMU+0x1904 (AUD_STATUS) bit0==1 (lines 3-6); aud_off = write 0 / wait 0 (104-107);
  aud_status = read 0x1904 bit0 (109-111). PMU base 0x15860000 = EXYNOS_PMU_BASE (Exynos9820.h:280, VERIFIED).
  → AUD_CONFIGURATION = 0x15861900, AUD_STATUS = 0x15861904.
- pmucal_local_enable (AB/calif_pmucal_local.c:24-60): run aud_on, then **if need_smc:
  exynos_pd_tz_restore(0x18c62000)**, then restore the aud_save list. exynos_pd_tz_restore =
  exynos_smc(SMC_CMD_PREAPRE_PD_ONOFF = 0x82000410, EXYNOS_WAKEUP_PD_DOWN = 1, addr, RUNTIME_PM_TZPC_GROUP = 2)
  (AB/exynos-el3_mon.c:239-245; constants AB/inc_exynos-el3_mon.h:23-26; SMC id AB/smc_h.h:41-42
  = include/linux/smc.h). Disable does exynos_pd_tz_save (= arg 0) before aud_off (calif_pmucal_local.c:98-104).
  0x18c62000 is inside the AUD block (CMU_AUD 0x18C00000 .. pinctrl 0x18C60000) — by name it is the
  AUD D-TZPC; after a power-up its protection settings must be restored by EL3, otherwise (INFERENCE)
  the ABOX SFRs may be secure-only and an AP non-secure access would fault.
- aud_save list = what the vendor considers AUD-domain state lost on power-down
  (cal_flexpmu_cal_local_exynos9820.h:8-102): PLL_AUD0/1 CON0-4 + LOCKTIME, all CMU_AUD DIV/MUX
  registers (0x1000-0x101C, 0x1800-0x1838), QCH_CON 0x3000-0x3074, AUD_CMU_CONTROLLER_OPTION 0x0800,
  HCHGEN_CLKMUX 0x0850, SYSREG_AUD (0x18C10000) BUS_COMPONENT_DRCG_EN 0x104, MEMCLK 0x108, SNPS_EMA 0x300.
  Entries carrying (0x15860000, 0x1904, bit0) are conditional on AUD_STATUS on.
- Is accessing an AUD SFR with pd-aud off a bus hang? No direct statement in the code. Evidence the
  vendor never does it: aud_save's conditional entries are guarded by AUD_STATUS (above); abox.c switches the
  regmap to cache-only before suspend and only syncs after resume (abox.c:2134-2141). Generic Exynos
  behaviour (INFERENCE, consistent with our USB/UFS experience in this repo): an access to a powered-down
  block returns a bus error/SError or stalls the interconnect → **treat as a hang; always read
  AUD_STATUS (0x15861904 bit0) first, and touch nothing in 0x18C00000-0x18EFFFFF if it is 0.**

### F6 CMU_AUD (0x18C00000, cal-if SFR_BLOCK(CMU_AUD, 0x18c00000, 0x8000), AB/cal_cmucal-sfr.c:6)
- NOT in Exynos9820.h yet (only cmucal source) → UNVERIFIED for our header; add it only after a live read.
- PLL_AUD0: PLL_CON0 0x120 (ENABLE 31, M 25:16, P 13:8, S 2:0; STABLE 29 per WAIT in aud_save line 43),
  PLL_CON3 0x12C (K 15:0) (cmucal-sfr.c:46-47, 2711-2716). Parent OSCCLK_AUD (cmucal-node.c:130).
  Rate table: 1179648040 Hz = M181 P4 S0 K31740 "0ppb" (cmucal-node.c:5-16) = 48 x 24.576 MHz.
- Tree (cmucal-node.c:1242-1256): DIV_CLK_AUD_AUDIF (0x1800, 9-bit) <- PLL_AUD0; DIV_CLK_AUD_UAIF1
  (0x1830, 9-bit) <- DIV_CLK_AUD_AUDIF; MUX_CLK_AUD_UAIF1 (0x1010, SELECT bit0) parents
  {0: DIV_CLK_AUD_UAIF1, 1: CLKIO_AUD_UAIF1 (external)} (cmucal-node.c:178-181, 987; sfr 2848);
  DIV_CLK_AUD_BUS (0x1804, 3-bit) <- PLL_AUD0; DIV_CLK_AUD_CNT (0x180C) <- AUDIF; DIV_CLK_AUD_MCLK (0x1824) <- AUDIF.
- Gates: GOUT ABOX BCLK_UAIF1 = 0x2024 (CG_VAL bit21, MANUAL bit20) parent MUX_CLK_AUD_UAIF1
  (cmucal-sfr.c:549, 3970-3971; node.c:1443); RSTNSYNC_CLK_AUD_UAIF1 0x20A8 (sfr.c:561); SMMU_AUD gate
  parent DIV_CLK_AUD_BUS (node.c:1472). DT: abox_uaif_1 clocks = DOUT_CLK_AUD_UAIF1 + GATE_ABOX_BCLK1
  (exynos9820.dts:1520-1531); abox clocks = PLL_OUT_AUD0, PLL_OUT_AUD1, DOUT_CLK_AUD_AUDIF, _DMIC, _BUS, _CNT
  (exynos9820.dts:1252-1254).
- QCH_CON_ABOX_QCH_BCLK1 = CMU_AUD+0x3028, ACLK +0x3020 (aud_save lines 89-92).

### F7 UAIF1 pins live in the AUD block
- aud_i2s1_bus = gpb1-0..3, function 2, pud 1 (k/arch_arm64_boot_dts_exynos_exynos9820-pinctrl.dtsi:193-197);
  bank gpb1 belongs to `pinctrl@18C60000` "/* AUD */" (same file 143-160). So the pinmux for UAIF1 also
  requires pd-aud on and the GPIO_AUD pclk gate (CMU_AUD 0x205C). 0x18C60000 is NOT in Exynos9820.h.

### F8 What the vendor abox_enable (runtime resume) does — AB/abox.c:2371-2448
Order: abox_gic_enable_irq; abox_enable_wdt; cpu gear request (ACPM AUD DVFS via pm_qos);
`writel(1, sysreg + MISC_CON 0x504)` (2385); L2_CACHE_CON only with the SHARE_VTS_SRAM quirk (2386-2387 —
9820 DT has only "try to asrc off", exynos9820.dts:1244, so not done); clk_enable(clk_cpu) if any and
clk_enable(audif) (2390-2401); abox_cfg_gpio("default") (UAIF pins); abox_pad_retention(false) =
exynos_pmu_update(0x1920, 0x00000800, 0x00000800) i.e. **PMU 0x15861920 bit11 = 1 (pad "normal")**
(abox.c:77-78, 2105-2116; retention(true) writes nothing); abox_restore_register = regcache_sync of all SFRs
incl. routes (2138-2142); if the ABOX timer was not running (cold): abox_gic_init_gic + download firmware;
abox_request_dram_on → regmap_write(SYSPOWER_CTRL 0x10, 1) (956-996); then (cold) abox_cpu_power(true) =
abox_core_power → PMU core0 0x15862D80 bit0 (prior notes; DT exynos9820.dts:1262-1264) and
abox_cpu_enable(true) is a no-op (2123-2130).
- Needed with the DSP held in reset (INFERENCE, to verify on device): pd-aud on (F5), AUD clocks
  (audif + bus + UAIF1 bclk), pinmux gpb1 func 2, pad retention released, SFR regmap contents
  (patch_9820 SBANKs + routes + UAIF CTRL0/1), SYSPOWER_CTRL=1 only if the RDMA reads DRAM.
  NOT needed: GIC init, firmware download, abox_core_power (PMU 0x2D80), WDT, IPC, cpu gear/ACPM DVFS
  (the CA32 clock only), SYSREG L2_CACHE_CON. MISC_CON (sysreg 0x504) = 1 is written unconditionally
  by the vendor; its meaning is undocumented → UNVERIFIED whether needed; copy it.

### F9 Rates — vendor clock math (exynos9820 values)
- AUD PLL target for 48 kHz family = 1179648040 Hz (abox.h:54) = cmucal table "0ppb" entry (F6).
- uaif_max_div = 512 in DT (exynos9820.dts:1255) → abox_register_bclk_usage takes the `else` branch:
  audif_rate = 384000 * 96 * 2 = 73,728,000 Hz for 48 kHz (abox.c:697-711); clk_set_rate(audif) (722).
  → DIV_CLK_AUD_AUDIF ratio 1179.648/73.728 = 16 (INFERENCE from the arithmetic).
- UAIF bclk = rate * channels * width (abox_if.c:372) on DOUT_CLK_AUD_UAIF1 (child of AUDIF).
- CMU divider encoding: ra_set_div_rate writes DIVRATIO = ratio - 1 (AB/calif_ra.c:256-291).
  → AUDIF DIVRATIO = 15; UAIF1 DIVRATIO = 11 for 6.144 MHz, 23 for 3.072 MHz (INFERENCE: arithmetic).
- What the stock amps expect: DTBO sound node `cirrus,ampclk = <0x0 0x0 0x5dc000>` (DTS:1480) =
  PLLSRC_SCLK, 6,144,000 Hz (notes/d8-audio-m3-amp.md:43-54, K/cs35l41.c:135, 1452-1476)
  → stock UAIF1 frame = 128 BCLK at 48 kHz; with bclk = rate*channels*width the stock config is
  4 x 32-bit (or 8 x 16) — INFERENCE; 4 x 32 is the natural fit (2 playback slots + amp IV slots).

### F10 DAI format for UAIF1 (the DTBO link) — fact chain
- DTBO: `uaif1 { format = "dsp_a"; bitclock-inversion; cpu=abox_uaif_1; codec = <amp_r 0 amp_l 0> }`
  with **no bitclock-master / frame-master** (DTS:1738-1746).
- snd_soc_of_parse_daifmt: neither property → SND_SOC_DAIFMT_CBS_CFS (codec slave) (AB/soc-core.c:4160-4190);
  bitclock-inversion → IB_NF (same function, inversion section).
- abox_uaif_set_fmt (F4): DSP_A → WS_MODE=1; IB_NF → BCLK_POLARITY=0, WS_POLAR=0; CBS_CFS → MODE=1 (ABOX master).
- hw_params, DSP_A, 32-bit, N ch: SBIT_MAX=31, VALID_STR=31, VALID_END=31, SLOT_MAX=N-1,
  FORMAT = (N-1) | 3<<3.
- **UAIF1 CTRL1 (computed, INFERENCE from the code above):**
  4 ch x 32: FORMAT 0x1B<<24 | BCLK_POL 0 | WS_MODE 1<<22 | WS_POLAR 0 | SLOT_MAX 3<<18 | SBIT_MAX 31<<12 |
  VALID_STR 31<<6 | VALID_END 31 = 0x1B000000|0x400000|0xC0000|0x1F000|0x7C0|0x1F = **0x1B4DF7DF**.
  2 ch x 32: FORMAT 0x19, SLOT_MAX 1 → 0x19000000|0x400000|0x40000|0x1F000|0x7C0|0x1F = **0x1944F7DF**.
- **UAIF1 CTRL0**: reset 0x01000010 (START_FIFO_DIFF_SPK=1, DATA_MODE=1); set MODE (bit2)=1 → 0x01000014,
  then SPK_ENABLE (bit0) → 0x01000015 to start. (DATA_MODE/IRQ_MODE are only touched for capture,
  abox_if.c:51-55.)

### F11 RDMA / SysMMU — the DSP-only part
- The 9820 AP driver never writes RDMA BUF_STR/END/OFFSET/STR_POINT/CTRL1 or ENABLE (grep over AB/*.c: only
  CTRL0 BURST_LEN at abox_rdma.c:1444-1447, STATUS reads at 1367/1616, VOL_FACTOR at 1891). Buffer
  address handed to the DSP = IOVA_RDMA_BUFFER(id) = 0x91000000 + id*1 MiB, mapped through the ABOX
  SysMMU by abox_iommu_map (abox_rdma.c:1410-1466; abox.h:37-39).
- Newer Samsung ABOX driver (exynos990 tree, ExtremeXT/android_kernel_samsung_exynos990 main, fetched to
  AB/e990/) does program a DMA from the AP for its debug DMA: BUF_STR = iova, BUF_END = iova+bytes,
  BUF_OFFSET = period (32 KiB), STR_POINT = iova, FORMAT field, then CTRL ENABLE bit0
  (AB/e990/abox_dma.c:1101-1219). Field layout there (ABOX 2.1, AB/e990/abox_soc_21.h:535-590):
  CTRL: BURST_LEN 22:19, FUNC 18:16 (0 normal, 1 pending; abox_dma.c:396-429), PACKED 14, FORMAT 12:8
  (= abox_get_format encoding), BUF_TYPE 4, ENABLE 0; BUF_STR/END/STR_POINT 31:4 (16-byte aligned),
  BUF_OFFSET 15:4. The 2.0 header used for 9820 (AB/e990/abox_soc_2.h:413-419 and AB/abox_soc.h:424-430)
  documents only BURST_LEN and ENABLE in RDMA CTRL0 → **9820 RDMA FORMAT bit position UNVERIFIED**
  (2.0.1 vs 2.1 already differ: RBUF_OFFSET 27:12 vs 27:20).
- SysMMU (sysmmu_aud @0x18E00000, Exynos9820.h:749; DT exynos9820-sysmmu.dtsi:98-112, clock GATE_SMMU_AUD,
  secure_base 0x18E10000, "sysmmu,no-suspend"): vendor enable = CTRL(0x000) = 0x5 after PT base,
  disable = CFG=0 + CTRL = 0x3 "BLOCK_DISABLE" (AB/exynos-iommu-reg.h:439-448, 602-613; exynos-iommu.h:100-108).
  Whether CTRL=0 (reset) means translation bypass on this SysMMU version is not stated in the code →
  UNVERIFIED (classic Exynos SysMMU: disabled = pass-through; read CTRL and VERSION 0x034 first).
- abox_addr_to_phys_addr: DSP addresses < 0x80000000 are SRAM offsets (sram_base_phys + addr), >= go
  through the IOMMU domain (abox.c:249-262); REMAP_MASK/ADDR reset = 0xFFF00000 / 0x18D00000
  (abox_soc.c defaults). Whether the RDMA master can fetch from ABOX SRAM by such an address
  (bypassing the SysMMU) is not shown anywhere on the AP side → UNVERIFIED, but it is the cheapest
  experiment (AP writes the tone into SRAM 0x18D00000 + off, RDMA BUF_STR = off).

### F12 BCLK keep-alive, data path at reset values, and DMA-free sources
- beyond_madera prepare: "enable bclk" = snd_soc_dai_set_tristate(cpu_dai, 0) for UAIF0/UAIF1
  (AB/beyond_madera.c:209-211); shutdown sets tristate 1 (277-278). abox_uaif_set_tristate →
  abox_disable_qchannel(ABOX_BCLK_UAIF1, !tristate) (abox_if.c:316-343) → **ABOX QCHANNEL_DISABLE (ifr+0x38)
  bit 3 = 1 keeps UAIF1 BCLK running** (bit index from enum qchannel, abox.h:157-167; reset value 0).
- Reset-value routing (from reg_defaults_9820 = 0 for 0x40/0x200/0x204, F3): SPUS_CTRL0 chain 0 OUT field
  = 0 = "SIFS1" (2.0.1 texts, abox_cmpnt_v20.c:3210-3212), IN = 0 = "RDMA", ASRC = 0 (off);
  SPUS_CTRL1 SIFS_OUT_SEL(1) (bits 14:12) = 0 = "SPUS OUT0". So **RDMA0 → SPUS OUT0 → SIFS1 is already the
  default chain**; the only router write needed is ROUTE_CTRL0[7:4] (UAIF1 SPK) = 2 ("SIFS1",
  abox_cmpnt_v20.c:3354-3358) → ROUTE_CTRL0 = 0x00000020 (INFERENCE: defaults == silicon reset).
- For a playback BE, abox_uaif_hw_params calls abox_cmpnt_reset_cnt_val, which writes
  SPUS_CTRL_SIFS_CNT(route-1) = 0 (abox_if.c:413-414, abox_cmpnt_v20.c:4233-4272); SIFS_CNT is
  only programmed with a rate for *capture* SIFS (sifs_hw_params returns early for playback, 4274-4306).
  → playback SIFS is paced by the UAIF (INFERENCE). SIFS_CNT(1) = ifr+0x294.
- SIFS1..4 format/rate kcontrols are IPC-only (format_put_ipc writes an SFR only for SIFS0 = MIXP_FORMAT and
  PIFS0/1, abox_cmpnt_v20.c:431-470) → no AP-visible SIFS1 format register; the sample format must come
  from the RDMA side (DSP-written CTRL fields) → part of the RDMA-format unknown.
- **No built-in tone/test-pattern source exists** in the 9820 ABOX SFR map: grep for loop/test/pattern/tone/
  sine in AB/abox_soc.h finds nothing; the only generators are ASRC dither (DITHER_CTRL, abox_soc.h:569-588,
  noise added to a stream that must already flow through an ASRC). Newer IPs add SIDETONE/DUMMY_START
  (AB/e990/abox_soc_21.h:135, 199-213, 831+) — not on 2.0.1.
- DMA-free *loopback* sources that do exist: UAIF SPK mux value 12 = "SIFMS" (capture-side SIFM →
  UAIF SPK, abox_cmpnt_v20.c:3354-3358, 3512-3521, 3912-3930) and SPUS INx = "SIFSM" fed by SPUSM mux
  (ROUTE_CTRL0[27:24], values 8..11 = UAIF0..3 MIC, abox_cmpnt_v20.c:3290-3299, 3726-3740). Both need
  audio arriving on some UAIF MIC side. The CS47L93 codec on UAIF0 HAS tone generators
  (MADERA_TONE_GENERATOR_1 0x20, mixer source 0x04 "Tone Generator 1", m2src/include_linux_mfd_madera_registers.h:25,
  2322-2328; sound_soc_codecs_madera.c:1424, 1576; cs47l92.c:980-983) → a fully DMA-free tone is
  CS47L93 TONE1 → AIF1TX1 → ABOX UAIF0 MIC → SPUSM=UAIF0 → SIFSM → SPUS IN0=SIFSM → SPUS OUT0 → SIFS1 →
  UAIF1 SPK → amps (INFERENCE: path assembled from the DAPM graph; never exercised without the DSP).
  It costs the codec bring-up (SPI, FLL, AIF1) on top of the amp bring-up.

### F13 UAIF1 pins (AUD pinctrl)
- Bank gpb1 = pinctrl@18C60000 + 0x020, exynos9810_bank_type_off: CON 4-bit @+0x00, DAT 1-bit @+0x04,
  PUD 4-bit @+0x08, DRV 4-bit @+0x0C, CONPDN @+0x10, PUDPDN @+0x14 (k/drivers_pinctrl_samsung_pinctrl-exynos-arm64.c:52-55,
  590-593, AUD = instance 1 at 683-690). aud_i2s1_bus: gpb1-0..3 func 2, pud 1 (F7).
  → GPB1CON = 0x18C60020: bits 15:0 = 0x2222; GPB1PUD = 0x18C60028: bits 15:0 = 0x1111 (pull-down,
  INFERENCE from the Samsung PUD encoding 1 = pull-down). Which of gpb1-0..3 is BCLK/WS/SDO/SDI is not
  stated anywhere fetched → UNVERIFIED (not needed for muxing all four).

## Proposed sequence (UNVERIFIED unless marked)

Addresses: PMU 0x15860000 (VERIFIED, Exynos9820.h:280); ABOX ifr 0x18C50000, sysreg 0x18C10000, SRAM
0x18D00000, SysMMU 0x18E00000 (VERIFIED, Exynos9820.h:737-750); CMU_AUD 0x18C00000 and AUD pinctrl
0x18C60000 = vendor-source only (F6, F7) → must be added to Exynos9820.h as UNVERIFIED until a live read.

**Stage 0 — read-only audit (no writes; do this first, log every value):**
1. PMU AUD_STATUS 0x15861904 bit0 (pd-aud on?), AUD_CONFIGURATION 0x15861900, pad 0x15861920 bit11.
   If bit0 == 0: STOP — do not touch 0x18C0_0000..0x18EF_FFFF (F5).
2. If on: ABOX IP_INDEX ifr+0x0 (expect 0x41424F58 "ABOX", abox_soc.c defaults row 1), VERSION ifr+0x4,
   SYSPOWER_STATUS +0x14, ROUTE_CTRL0 +0x40, SPUS_CTRL0/1 +0x200/+0x204, SBANK_RDMA(0) +0x220,
   UAIF1 CTRL0/1/STATUS +0x510/+0x514/+0x51C, RDMA0 +0x1000..+0x1030, QCHANNEL_DISABLE +0x38.
   A non-"ABOX" IP_INDEX or a fault ⇒ TZPC/secure (F5 need_smc) — stop.
3. CMU_AUD: PLL_CON0_PLL_AUD0 +0x120 (ENABLE 31, STABLE 29, M/P/S), PLL_CON3 +0x12C (K), DIV AUDIF +0x1800,
   DIV UAIF1 +0x1830, MUX UAIF1 +0x1010, gate BCLK_UAIF1 +0x2024, ABOX ACLK gate +0x2010.
4. SysMMU 0x18E00000: CTRL +0x0, CFG +0x4, VERSION +0x34 (CTRL==0 ⇒ probably bypass, F11).
5. gpb1 CON/PUD 0x18C60020/0x18C60028.

**Stage 1 — clocks + pins + UAIF1 running with silence (DMA-free, validates BCLK/WS at the amps):**
6. (Only if pd-aud is OFF and we decide to power it — HAZARD, see below) PMU 0x15861900 bit0=1, poll
   0x15861904 bit0; then the TZPC SMC 0x82000410(1, 0x18c62000, 2) exactly as exynos_pd_tz_restore.
   Preferred: don't — first find out whether sboot leaves pd-aud on.
7. PLL_AUD0 = 1179648040 Hz if not already: M=181 P=4 S=0 K=31740 (F6), ENABLE=1, poll STABLE (bit29).
   (CMU write — AUD-local.) Divider: AUDIF DIVRATIO=15 (73.728 MHz), poll BUSY bit16 (F9).
8. MUX_CLK_AUD_UAIF1 SELECT=0 (internal divider), DIV_CLK_AUD_UAIF1 DIVRATIO = 11 (6.144 MHz, 4x32) or
   23 (3.072 MHz, 2x32); poll BUSY. Gate 0x2024: leave auto-gating, or set MANUAL(20)+CG_VAL(21) if the
   audit shows the gate closed.
9. gpb1 CON[15:0]=0x2222, PUD[15:0]=0x1111 (F13). If 0x15861920 bit11 == 0, set it (PMU write, F8).
10. ABOX: SYSREG MISC_CON (0x18C10504) = 1 (vendor does it, F8); QCHANNEL_DISABLE bit3 = 1 (F12);
    UAIF1 CTRL1 = 0x1B4DF7DF (4x32) or 0x1944F7DF (2x32) (F10); UAIF1 CTRL0 = 0x01000014 (MODE=master),
    then 0x01000015 (SPK_ENABLE). ROUTE_CTRL0 stays 0 (RESERVED source) ⇒ silence, BCLK/WS out.
    Check: UAIF1 STATUS ERROR_OF_SPK, amp PLL-lock / ASP status over I2C (amp notes).

**Stage 2 — tone via RDMA0 from a ring buffer:**
11. Buffer: 48 frames x N ch x 32-bit = one 1 kHz period, 16-byte aligned, either
    (a) ABOX SRAM (AP writes 0x18D00000+off; RDMA BUF_STR=off) — cheapest, avoids SysMMU, UNVERIFIED
        address space; or (b) DRAM below 4 GiB with SysMMU bypassed (CTRL==0 at audit) — BUF_STR = PA.
12. SBANKs as patch_9820 (at least SPUS_SBANK_RDMA(0) ifr+0x220 = 0x02000000), SPUM_CTRL0 = RECP bit
    (abox_soc.c:1818-1822); SYSPOWER_CTRL ifr+0x10 = 1 if DRAM (F8).
13. RDMA0: BUF_STR +0x1008, BUF_END +0x100C (= STR + size), BUF_OFFSET +0x1010 (= period bytes),
    STR_POINT +0x1014 = STR, VOL_FACTOR +0x1018 = 0x800000? (unity is UNVERIFIED — 24-bit field; leave reset 0
    and check the reset value first: 0 may mean mute), CTRL0 = BURST_LEN 8 (reset 0x00400000) | FORMAT
    (bits 12:8 = 0x1B for 4x32 per the 2.1 layout — UNVERIFIED for 2.0.1) | ENABLE.
14. Route: ROUTE_CTRL0[7:4] = 2 (SIFS1); SPUS_CTRL0 chain0 and SPUS_CTRL1 SIFS1 already select
    RDMA0→OUT0→SIFS1 at reset (F12); SIFS_CNT(1) ifr+0x294 = 0.
15. Start order (vendor DPCM order is FE prepare/trigger then BE trigger, INFERENCE): RDMA0 ENABLE, then
    UAIF1 SPK_ENABLE. Watch RDMA0 STATUS +0x1030 (PROGRESS bit31, RBUF_OFFSET 27:12 in 16-byte units).

**Fallback DMA-free audible path:** CS47L93 TONE1 → AIF1 → UAIF0 MIC → SPUSM → SIFSM → SPUS IN0 → SIFS1 →
UAIF1 SPK (F12); needs UAIF0 configured as I2S master (same F4 rules) plus codec bring-up.

## Hazards (AP writes outside ABOX)

- **PMU** (always-on, system-wide): 0x15861900 (pd-aud on/off), 0x15861920 bit11 (pad retention release),
  and — never needed for this plan — 0x15862D80/0x15862E00 (CA32 core power), 0x15862DA0 (core reset).
  Writing the wrong PMU word can kill another domain or reset the SoC.
- **EL3 SMC** 0x82000410 (TZPC restore for 0x18c62000) — only if we power pd-aud ourselves; behaviour of the
  monitor when called from our UEFI/EL2 context is unknown.
- **CMU_AUD** (0x18C00000): PLL_AUD0 CON0/CON3, DIV AUDIF/UAIF1, MUX UAIF1, gates. AUD-local, but CMU_AUD
  also clocks the AUD bus to the system interconnect (DIV_CLK_AUD_BUS); never touch DIV_CLK_AUD_BUS (0x1804),
  CPU mux/dividers (0x1000, 0x1810-0x1818), or CMU_CMU's CLKCMU_AUD_CPU (0x1A24xxxx) — those are owned by
  ACPM DVFS (dfs_id ACPM_DVFS_AUD, exynos9820.dts:5537).
- Any read of an AUD-block address with pd-aud off (F5) — treat as a hang.
- SysMMU: do not write CTRL; a block/enable without a page table stalls every ABOX master.

## Open questions (the reads that would close them)

1. pd-aud state at UEFI time (PMU 0x15861904) and whether sboot/LK powers or releases it — Stage 0 read.
2. TZPC: are ABOX SFRs NS-accessible after power-up without the SMC? Only matters if pd-aud is off.
3. Silicon reset values == reg_defaults_9820? (IP_INDEX, ROUTE, SPUS, UAIF, RDMA) — Stage 0 dump.
4. RDMA CTRL0 field layout on 2.0.1 (FORMAT position, any FUNC/BUF_TYPE bits), VOL_FACTOR unity value,
   SBANK_LIMIT meaning — candidate: dump RDMA0 SFRs on Android while a stream plays (root shell +
   /dev/mem or the vendor debugfs `abox` regmap dump; the regmap is cached so debugfs shows writes
   the AP made but the DSP's writes only if the register is volatile — RDMA regs are not in the volatile list,
   so read them through /dev/mem, not regmap debugfs).
5. RDMA address space: SRAM offsets vs IOVA vs PA; SysMMU CTRL==0 ⇒ bypass? (Stage 0 read of CTRL/VERSION;
   then the SRAM-buffer experiment.)
6. Does SIFS1 need any format/rate programming beyond the RDMA FORMAT (vendor sends it via IPC only)?
7. MISC_CON (sysreg+0x504) meaning; SYSPOWER_CTRL needed for SRAM-only data?
8. Which gpb1 pin is BCLK/WS/DOUT/DIN (only needed for scope probing).
9. Stock UAIF1 slot layout (4x32 inferred from the 6.144 MHz amp SCLK) — read UAIF1 CTRL1 on Android
   during playback (/dev/mem, root) to get the exact value; amps' RX slot config from the amp notes.
10. Whether DATA_MODE (CTRL0 bit4, reset 1) / START_FIFO_DIFF_SPK matter for playback.
- 2026-10-12 (main session): stage 0 implemented as `AudioAbox.c` AboxAudit (flag BEYOND0LTE_AUDIO_ABOX_AUDIT 1,
  called at AudioProbe entry right after AmpProbe, inside the 0x69 guard). Reads PMU 0x1900/0x1904/0x1920; only
  with AUD_STATUS bit0 = 1: IP_INDEX first (stops unless 0x41424F58), then the IFR/RDMA0/CMU_AUD/SYSREG/SysMMU/
  gpb1 list of the Stage 0 plan. New header constants are tagged SOURCE-ONLY until this read confirms them.
  Build exit 0, FVMAIN_COMPACT 360 B free. boot #95 = s10e-artifacts\boot95-v2.img sha256 ce0adb73…
- boot #95 RESULT: "exception pre-boot" (user; too fast to read), no new UEFILOG (file still from 00:22).
  Cause found in our code, not the hardware: the AUD block (CMU_AUD, SYSREG_AUD, ABOX IFR, GPIO_AUD, SysMMU) was
  never in PlatformMemoryMapLib's mMmioRegions -> the first access (ABOX IP_INDEX, only reached when pd-aud
  reads ON) is a translation fault. So pd-aud is very probably ON at DXE time (the PMU reads are mapped and come
  first) - to be confirmed by the log.
- boot #96: the five AUD regions added to mMmioRegions (+ SOURCE-ONLY sizes CMU_AUD 0x8000, GPIO_AUD 0x1000
  from exynos9820.dts:1119-1121). Build exit 0, FVMAIN_COMPACT 392 B free; boot96-v2.img sha256 bd0c44f8…,
  flashed + readback. The first boot after the fault may skip the whole audio probe (0x69 guard).
- **boot #96 RESULT (second boot; the first one skipped audio via the 0x69 guard as designed), stage 0 log:**
  - pd-aud CONFIGURATION 1 STATUS 1 = **ON at DXE time**; pad 0x860 (bit11 = normal, retention already released).
  - ABOX IP_INDEX 0x41424F58 = "ABOX" -> **non-secure AP access works** (no TZPC SMC needed). VERSION 0x02000100
    (= 2.0.1, matches F1). SYSPOWER_STATUS 0, QCHANNEL_DISABLE 0, ROUTE_CTRL0 0, SPUS_CTRL0/1 0, SBANK_RDMA0 0
    (= reg_defaults_9820 -> the driver's defaults are the silicon reset values for these).
  - UAIF1 CTRL0 0x01000010 (= the reset value in F10), CTRL1 0, STATUS 0. RDMA0 CTRL0 0x00400000 (BURST_LEN
    field 22:19 = 8), the rest 0.
  - CMU_AUD: PLL_AUD0_CON0 0xA02D0110 = ENABLE(31) + STABLE(29), M 0x2D = 45, P 1, S 0; CON3 K 0x5EFF = 24319 ->
    26 MHz x (45 + 24319/65536) / 1 = 1179.65 MHz = the 1179648040 Hz table rate (expressed with P 1 here, not the
    M181 P4 row of the research table: same frequency). DIV_AUDIF 1 (/2 -> 589.8 MHz), DIV_BUS 6 (/7),
    DIV_UAIF1 5 (/6 -> 98.3 MHz), MUX_UAIF1 0 (internal divider). Gates ACLK / BCLK_UAIF1 = 0x200000 (bit21
    CG_VAL set, MANUAL 0 = hardware q-channel control).
  - SYSREG_AUD MISC_CON 0 (vendor writes 1). SysMMU CTRL 4 (bit0 enable = 0 -> translation off, bypass
    INFERENCE), CFG 0x20000, VERSION 0x70600000 (7.6).
  - gpb1 CON 0 (all inputs - the UAIF1 pins are NOT muxed yet), PUD 0x11111111.
  - So: everything stage 1 needs is powered and reachable; stage 1 writes are only inside the AUD block
    (DIV_AUDIF/DIV_UAIF1 for the BCLK rate, gpb1 func 2, QCHANNEL_DISABLE bit3, UAIF1 CTRL1/CTRL0) plus the amp
    I2C sequence. No PMU/SMC write needed.
- 2026-10-12 **PC microphone as an unattended audio detector** (user's idea): the PC's Windows mic endpoint is
  muted (MME / DirectSound / WASAPI give exact digital silence, -96.7 / -240 dBFS); the raw WDM-KS pin
  "Microphone Array (Realtek HD Audio Mic input)" still carries the hardware signal (-68 dBFS room). Self-test:
  a 1 kHz tone on the PC speakers reads -31 dBFS over a -83..-100 dBFS floor (SNR ~52 dB).
  Tool: `tools/audio-listen/listen.py SECONDS [--selftest]` (scratchpad venv: sounddevice + numpy; run with -I);
  prints only the detected bursts (start, duration, dominant Hz, level), saves no audio.
- 2026-10-12 10:09 **mic run 1 (boot #96, headphones lying near the PC, adb reboot from TWRP at t=0):** a
  ~1000 Hz tone at t = 31.4-33.3 s (two bursts 0.8 + 1.0 s, one dip below the 15 dB threshold), peak -72 dBFS,
  SNR 16 dB over a -87 dBFS floor. => the m2 tone is **~1 kHz** (closes the UNVERIFIED pitch in §6.1 of
  d8-audio-m1.md) and plays ~31 s after the reboot command. (A 390 Hz burst at 1.8 s = room / PC noise, before
  the phone had even reset.) The earbud is quiet at the mic; the loudspeaker test will be much louder.
- boot #97 (stages 1+2 in one image, unattended, judged by UEFILOG "AudioSpk:" + the PC mic):
  AudioSpeaker.c SpeakerTonePlay inside AmpProbe (only on amps we released, both IDs OK): amp @40 table rows
  0.2-5.7 (errata, OTP unpack of otp_map_2 = AudioAmpOtp.h generated verbatim, 99 fields / 688 bits, bypass trims,
  6.144 MHz PLL code 0x28, DSP_A, RX slot 1, gain field 0x0A, muted); ABOX: DIV_UAIF1 95 (589.824 / 96 =
  6.144 MHz, AUDIF left at the sboot /2), gpb1 func 2, MISC_CON 1, SYSPOWER_CTRL 1, QCH bit3, UAIF1 CTRL1
  0x1B4DF7DF / CTRL0 0x01000014, SBANK_RDMA0 0x02000000, RDMA0 ring in DRAM (<4 GiB, cache written back,
  4800 frames x 4 x 32 bit, 1.5 kHz -12 dBFS in slot 1), VOL 0x800000, CTRL0 0x00400000|0x1B00|EN, ROUTE 0x20,
  SPK_EN; amp PLL lock / PUP_DONE / unmute -20 dB; 2 s; full teardown + restore. 1.5 kHz is chosen so the mic
  can tell it from the 1 kHz headphone tone. Also BEYOND0LTE_WINDOWS_WATCHDOG 1 (unattended session).
  Build exit 0, FVMAIN_COMPACT 1336 B free; boot97-v2.img sha256 1e71e6a0ab7c0095e0a061ed2e0b8a2ca862614bc970bcc4abfb3be3de7b2454.
- **boot #97 RESULT (run b97, 10:23):** every step clean in UEFILOG: amp OTP_BOOT_DONE, OTP unpacked (OTPID 3,
  99 fields, ended word 24 bit 0), 0 I2C errors; DIV_UAIF1 5 -> 0x5F; UAIF1 CTRL0 0x1000015 CTRL1 0x1B4DF7DF;
  RDMA0 CTRL0 0x401B01 (readback keeps the FORMAT bits), STATUS 0x80000060 -> 0x800006F0 -> 0x80000768 ->
  0x800007E8 over 2 s (PROGRESS bit31 set; bits 27:12 stay 0, bits 11:0 count slowly - meaning unknown);
  amp PLL_LOCK seen only after the 20 ms poll (STATUS3 0x2000002F at tone start), PUP_DONE 1 ms, no error
  bits during the tone; PDN_DONE 0 ms; teardown restored everything (RDMA0 STATUS 0x900 afterwards).
  **Mic: nothing**, not even the headphone tone that run 1 heard - the room floor was ~13 dB higher and the
  broadband peak detector missed it. -> listen.py rewritten as a narrowband detector (+-15 Hz around the target
  tones, >= 10 dB over that band's median for >= 300 ms; self-test SNR 56 dB at 1 kHz).
- boot #98 (run 2): three RDMA buffer variants in one boot, each 2 s with its own pitch: DRAM physical 1.5 kHz,
  ABOX SRAM by physical address 3 kHz, ABOX SRAM by DSP offset 0x10000 6 kHz (abox.c:249-262); amp digital
  volume -10 dB; RDMA0 STATUS sampled every 5 ms after each enable. ABOX SRAM added to the UEFI MMIO map.
  Build exit 0, FVMAIN_COMPACT 1000 B free, boot98-v2.img sha256 34ea311d….
- **boot #98 RESULT (run b98):** mic heard the headphone tone (1000 Hz, t = 31.6 s, 2.0 s, SNR 19 dB) - the
  narrowband detector works. Speaker: amp PLL_LOCK not within 20 ms and **PUP_DONE timed out (101 ms,
  IRQ1_STATUS1 0x400000)** -> no unmute, no variants. Difference to b97 (PUP_DONE in 1 ms): there RDMA0 was
  already enabled when UAIF1 SPK_EN was set and the amp powered up; here RDMA0 was off. Inference: UAIF1 only
  drives BCLK/LRCLK while SIFS1 has data, and PUP_DONE needs the amp clock.
- boot #99: variant 0 (DRAM) is started before the amp power-up, PLL_LOCK poll 100 ms; later variants switch
  the DMA buffer (RDMA0 disable / reprogram / enable). sha256 51a9e405….
- **boot #99 RESULT (run b99, ~10:55): LOUDSPEAKER SOUND, proven by the PC mic.**
  - Mic (narrowband): 1500 Hz at t = 17.8 s for 2.1 s, peak -50 dBFS, **SNR 44 dB** (variant DRAM physical);
    3000 Hz at t = 19.8 s for 2.2 s, peak -46 dBFS, **SNR 51 dB** (variant SRAM physical 0x18D10000);
    6000 Hz: nothing (variant SRAM by DSP offset 0x10000). Headphone tone 1000 Hz at t = 37.6 s as before.
  - => **RDMA addresses are PHYSICAL** (AUD SysMMU disabled = bypass): both DRAM (0xEFFED000) and ABOX SRAM by
    physical address play; the DSP-offset form does not. The RDMA0 FORMAT field at bits 12:8 (0x1B = 4 x 32)
    and VOL_FACTOR 0x800000 give correct audio (2.1 layout holds on 2.0.1).
  - RDMA0 STATUS bits 11:0 advance 0xF0 = 240 per 5 ms = **48 kHz frames** (RBUF_CNT counts frames, mod 4096);
    PROGRESS bit31 set while running; bits 27:12 stayed 0 (not the read offset on this version, or needs more).
  - Amp: PLL_LOCK bit still not seen within 100 ms of polling IRQ1_STATUS3 (reads 0x20000000), yet PUP_DONE came
    after 1 ms and the tone plays: the lock bit is probably latched only as an IRQ status after unmask, ignore.
  - Order that works: UAIF1 SPK_EN, RDMA0 enabled with data, then amp power-up (PUP_DONE), unmute, volume.
    Without data on RDMA0, PUP_DONE times out (b98).
  - m3 is **device-validated** (loudspeaker tone from the AP alone: no ABOX DSP, no amp DSP).
