# M14 - M4 (CPUCL2) DVM / snoop coherency research

Question: does Samsung's Linux (or EL3 on PSCI CPU_ON) enable snoop/DVM for the M4 cluster,
and what M4-specific errata/sysreg setup does arm64 Linux do? Context: SoC freeze ~45-75 s after
Windows takes both M4s; KD snapshot: an M4 just after `IC IALLUIS; DSB SY` in nt!MiCopyPage.

Research only (web + source reading); no device access.

## Progress log

- 2026-10-14: notes file created. Plan: sparse-clone LineageOS android_kernel_samsung_exynos9820,
  grep cal-if/exynos9820, exynos-cpupm, sci, arch/arm64 for mongoose/M4.
- Source: LineageOS android_kernel_samsung_exynos9820, branch lineage-23.2, commit
  a412cfaf86b980ce35cae96a35f678c8ea551575 (https://github.com/LineageOS/android_kernel_samsung_exynos9820).
  Working-tree checkout fails on Windows; files read with `git show HEAD:<path>` from a depth-1 clone.
- `drivers/soc/samsung/cal-if/exynos9820/flexpmu_cal_cpu_exynos9820.h` has two halves:
  - `#ifndef ACPM_FRAMEWORK` (kernel side). CPU on/off only touches PMU interrupt enables
    (GRP2_INTR_BID_* at 0x15870000, CLUSTERn_CPUm_INT_EN at 0x15860000). Only CPUCL2 has a
    cluster `on` seq:
    `PMUCAL_SEQ_DESC(PMUCAL_WRITE, "SYSREG_CPUCL2_BUS_COMPONENT_DRCG_EN", 0x1D100000, 0x0104, (0xffffffff << 0), (0xffffffff << 0), ...)`
    i.e. SYSREG_CPUCL2 + 0x104 = 0xFFFFFFFF (dynamic root clock gating enable for the CPUCL2 bus
    components). Not a snoop/DVM bit; a clock-gating enable.
    cluster status: CLUSTER2_NONCPU_STATUS = 0x15860000 + 0x1604 bit0. (CLUSTER1 status is a
    copy of CLUSTER0 0x1204 -> A75+A55 are one DSU "cluster0" in PMU terms.)
  - `#else` (ACPM_FRAMEWORK, the APM/ACPM firmware's table - runs in the APM co-processor, not
    Linux): core on = `CPUCL2_HCHGEN_CLKMUX` 0x1d120000+0x850 bit4 =1, EXT_FUNC
    "cluster2_cpu_up" (0x41, core#), bit4 = 0. Cluster on = EXT_FUNC "cluster2_noncpu_up" (0x48)
    - body not in source (ACPM firmware). Cluster off:
    ```
    CPUCL2_HCHGEN_CLKMUX 0x1d120000+0x0850 bit4 = 1
    CRP_PM_CC1_0 0x1A000000+0x01A4 [1:0] = 3
    CRP_PM_CC1_3 0x1A000000+0x021C [1:0] = 3
    WAIT CRP_PM_CC1_0 bit16 == 0 ; WAIT CRP_PM_CC1_3 bit16 == 0
    EXT_FUNC cluster2_noncpu_down (0x49)
    CHECK_SKIP CPUCL2_OPTION 0x15860000+0x1c8c bit0 == 0
    CPUCL2_HCHGEN_CLKMUX bit4 = 0
    ```
    So before powering CPUCL2 down, something at 0x1A000000 ("CRP", PM_CC1_*) is handshaked. 0x1A000000
    is also in pmucal_p2v_list. What CRP is: TBD (see exynos-sci.c / DT next). The matching
    ON-side (undo of PM_CC1) is hidden inside EXT_FUNC 0x48 -> not visible.
- `include/soc/samsung/exynos-sci.h` (same commit): `#define SCI_BASE 0x1A000000` - so the
  "CRP_PM_CC1_*" registers above live in the **SCI (System Coherency Interconnect)**. Other SCI
  offsets defined there: ArrDbgCntl 0x05C, CCMControl1 0x0A8, PM_SCI_CTL 0x140 (LLC_En_Bit 25,
  DisableLlc_Bit 9). `drivers/soc/samsung/exynos-sci.c` is LLC-only (llc_invalidate/flush/region
  via ACPM IPC channel 10, LLC dump, error dump: SCI_ErrCnt/ErrStat/ErrAddr). It writes **no**
  snoop/DVM enable. DT `arch/arm64/boot/dts/exynos/exynos9820.dts` ~L6118: exynos-sci node,
  acpm-ipc-channel 10, initial_llc_region 1 (LIT_MID_ALL), llc_enable 1.
- Interpretation (UNVERIFIED, inferred from naming + usage): CRP_PM_CC1_n = SCI "coherent
  request port" power-management register for CC1 (= the big cluster's port; 9810 uses the same
  CC1_0 for its M3 cluster1). Writing [1:0]=3 then waiting bit16==0 looks like a
  "disconnect snoop+DVM / drain" handshake (2 request bits, 1 status bit). There are two ports
  for CPUCL2 on 9820 (CC1_0 at 0x1A4 and CC1_3 at 0x21C), only one on 9810.
- Cross-check with 9810 (`cal-if/exynos9810/flexpmu_cal_cpu_exynos9810.h`, ACPM half,
  cluster1 = M3 cluster): cluster1_on is fully visible (no EXT_FUNC) and does **not** touch
  CRP_PM_CC1_0 at all - it only does HCHGEN_CLKMUX, NONCPU_CONFIGURATION write-wait 0xF, BISR/OTP,
  and ETC_RESET releases. cluster1_off does CRP_PM_CC1_0 [1:0]=3 + WAIT bit16==0 before
  NONCPU_CONFIGURATION=0. => On Exynos the SCI port reconnect on power-up is either automatic in
  hardware or the PM_CC bits are reset with the cluster; the kernel never explicitly "enables"
  snoop/DVM for the big cluster. Nothing in Linux runs at boot for an already-powered cluster.
- `cal-if/exynos9820/flexpmu_cal_system_exynos9820.h` kernel half, `pmucal_lpm_init[]` (runs once
  at Linux boot via pmucal system init): only clock-gating setup - *_HCHGEN_CLKMUX, *_CLKDIV2PH,
  *_SHORTSTOP, `CPUCL2_CTRL_EXT_EVENT` 0x1D120000+0x838 mask 0x3fffff = 0x3FF181, and
  SYSREG_*_BUS_COMPONENT_DRCG_EN = 0xFFFFFFFF for every block incl. CPUCL2 (0x1D100000+0x104).
  No SCI/CRP/snoop/DVM writes anywhere in the kernel half (grep CRP|0x1A000000|SNOOP|DVM -> none
  outside the ACPM cluster2_off seq). Sleep seqs set QCH_CON_PPC/PPMU_CPUCL2_{0,1}_QCH
  (CMU_CORE 0x1a030000 + 0x30bc/0x30c0/0x30e8/0x30ec) = 6 / 4 - Q-channel for the CPUCL2 bus
  perf monitors, not coherency.
- **STRONG LEAD - Samsung 9820-only "dummy TLBI" workaround in arm64 core code** (same commit;
  `git grep -n vaale1is` -> exactly these 6 sites, all `#ifdef CONFIG_SOC_EXYNOS9820`; the
  beyond0lte (S10e) defconfig sets CONFIG_SOC_EXYNOS9820=y, EVT0 not set):
  - `arch/arm64/include/asm/tlbflush.h:102-111` local_flush_tlb_all():
    ```
    dsb(nshst);
    __tlbi(vmalle1);
    #ifdef CONFIG_SOC_EXYNOS9820
    __tlbi(vaale1is, 0);
    #endif
    dsb(nsh);
    isb();
    ```
  - `arch/arm64/kernel/head.S:690-694` __enable_mmu: `ic iallu` / `tlbi vaale1is, xzr // dummy TLBI` / `dsb ish` / `isb`
  - `arch/arm64/kernel/head.S:866-870` (__primary_switch KASLR relocate): `tlbi vmalle1` / dummy / `dsb nsh`
  - `arch/arm64/kernel/head.S:875-879`: `ic iallu` / dummy / `dsb ish`
  - `arch/arm64/mm/proc.S:190-194` __idmap_cpu_set_reserved_ttbr1: `tlbi vmalle1` / dummy / `dsb nsh`
  - `arch/arm64/mm/proc.S:439-443` __cpu_setup (runs on EVERY CPU incl. secondaries, at bring-up
    before MMU on): `tlbi vmalle1 // Invalidate local TLB` / dummy / `dsb nsh`
  Pattern: every *local* (non-broadcast) TLBI VMALLE1 or IC IALLU is followed by a broadcast
  `TLBI VAALE1IS, XZR` (VA 0, all ASIDs, last level, Inner Shareable) **before** the DSB.
  Broadcast-only paths (flush_tlb_all/mm/page/range, which already use *IS ops) are unchanged.
  No commit message available (LineageOS shallow history). Meaning (UNVERIFIED): a 9820 erratum in
  which a local-only invalidate + DSB does not complete / is not ordered correctly unless a DVM
  (IS) op is also outstanding - i.e. the DSB's completion tracking on this SoC is tied to the DVM
  path. Which core type needs it is not stated (applied to all CPUs unconditionally).
  Upstream Linux has no such workaround (to check on lore). Windows/HAL will not do this.
- `arch/arm64/mm/cache.S:153-158, 252-257` (same commit): `#ifndef CONFIG_SOC_EXYNOS9810 ... dc cvau
  #else dc civac` - a 9810 (M3) workaround turning clean-to-PoU into clean+invalidate-to-PoC in
  __flush_cache_user_range and __clean_dcache_area_pou. NOT enabled for 9820 (S10e defconfig has
  CONFIG_SOC_EXYNOS9810 not set). Also hibernate-asm.S:102 same guard.
- Linux full-icache invalidate is `ic ialluis; dsb ish; isb` (cacheflush.h:134 __flush_icache_all,
  cache.S flush_cache_all/louis) - no 9820 guard; but Linux uses it rarely (normal path is IC IVAU
  by VA in flush_icache_range). Windows MiCopyPage using IC IALLUIS + DSB SY is a much hotter
  path than anything Linux does => Linux may simply never hit the window.
- arch/arm64/kernel/cpu_errata.c: no Samsung MIDR entries; cputype.h defines
  MIDR_CHEETAH (SEC 0x53, part 0x003), MEERKAT 0x002, MONGOOSE 0x001 but MIDR_CHEETAH is unused in
  arch/arm64. No ACTLR/CPUECTLR-style writes anywhere in arch/arm64 or drivers/soc/samsung.
- M4 IMPDEF error-syndrome regs (EL1-readable per Samsung's own code,
  `drivers/soc/samsung/debug/exynos-helper.c` exynos_dump_info ~L427-475, for MONGOOSE/MEERKAT/CHEETAH):
  FEMERR0SR S3_1_C15_C2_0, FEMERR1SR S3_1_C15_C2_4, LSMERR0SR S3_1_C15_C2_1, LSMERR1SR S3_1_C15_C2_5,
  TBWMERR0SR S3_1_C15_C2_2 / 1SR C2_6, L2MERR0SR S3_1_C15_C2_3 / 1SR C2_7, L3 bank select
  `msr S3_1_C15_C7_1, bank` then L3MERR0SR S3_1_C15_C3_0 / 1SR C3_4. Diagnostic only.
- `drivers/soc/samsung/exynos-cpupm.c:288-360`: CPU hotplug online -> if first CPU of the
  (v)cluster: `cluster_enable()` = `cal_cluster_enable(id); big_reset_control(1);` then
  `cal_cpu_enable(cpu)`. Offline mirror: `big_reset_control(0); cal_cluster_disable(id)`.
  `pmucal_cpu.c:135` pmucal_cpu_cluster_enable just runs the kernel-half `on` seq (for CPUCL2 =
  the DRCG_EN write). These run only on hotplug transitions, not for already-powered clusters.
  => Linux never writes snoop/DVM controls; EL3 (PSCI CPU_ON) + ACPM own CPU/cluster power.
- 2026-10-14 coordinator re-prioritisation: freeze is TIME-triggered (41-75 s after KD connect,
  ~1 min into Windows), independent of workload; IC IALLUIS/TLBI IS from the M4 in the park stub
  complete fine. New focus: what in EL3/ACPM/Linux PM acts on CPUCL2 ~1 min after boot unless the
  OS does something (cpu_inform hints, cpupm, ACPM governors/heartbeat, boot QoS timeouts, EMA,
  watchdogs, TMU).
- **WATCHDOG LEAD.** Stock DT `arch/arm64/boot/dts/exynos/exynos9820.dts:4486-4507`: two WDTs -
  `watchdog_cl0@10050000` "samsung,exynos9-v1-wdt", INTREQ__WDT_CLUSTER0, timeout-sec 30, index 0;
  `watchdog_cl2@10060000` "samsung,exynos9-v2-wdt", INTREQ__WDT_CLUSTER2, timeout-sec 30, index 1,
  `use_multistage_wdt; /* Use FIQ debug watchdog */`. Our firmware (Beyond0LteBootLogDxe.c
  OnExitBootServices, ~L866-907) only touches cl0 (0x10050000: WTCON=0 at EBS unless
  BEYOND0LTE_WINDOWS_WATCHDOG). **Nothing in the repo touches 0x10060000** (grep). If sboot (or
  EL3) leaves WDT_CLUSTER2 running and its reset/IRQ path is gated by the CPUCL2 power state, it
  would bite only once the M4s are powered - see driver next.
- `drivers/watchdog/s3c2410_wdt.c` (same commit): `#define EXYNOS_CLUSTER2_NONCPU_INT_EN (0x1644)`
  (cl0: 0x1244), `drv_data_exynos9_v2 = { .noncpu_int_en = 0x1644, .mask_bit = 2, .rst_stat_bit = 23 /* CLUSTER2 WDTRESET */ }`.
  Probe (~L1395-1420) for EVERY wdt incl. cl2: auto_disable_func (none for v1/v2), then
  `s3c2410wdt_stop_intclear(wdt)` (WTCON &= ~(ENABLE|RSTEN), WTCLRINT=1) - i.e. Linux **stops a
  WDT_CLUSTER2 left running by the bootloader** - then for non-multistage wdts sets
  NONCPU_INT_EN bit2. cl2 has use_multistage_wdt -> it is re-armed by
  s3c2410wdt_multistage_wdt_start() as an **interrupt-only** pre-watchdog (INTEN set, RSTEN
  cleared, count = 70% of cl0's, MULTISTAGE_WDT_RATIO 70) and kicked together with cl0 on every
  keepalive. RST_STAT bit 23 = "CLUSTER2 WDTRESET" exists => a CPUCL2 watchdog reset is a
  distinct reset source (cluster-scoped?).
  Whether sboot leaves WDT_CL2 running: UNKNOWN (sboot not read for 0x10060000 yet; repo has
  no reference to 0x10060000 at all).
- Repo check: `firmware/tools/bootshim/bootshim.S:67-76` already writes WTCON=0 to BOTH
  0x10050000 and 0x10060000 (comment calls the second "cl1"; it is cl2) as its first action.
  So WDT_CL2 is stopped at our entry; the WDT lead only holds if something later re-arms it
  (no evidence). Downgraded.
- `drivers/cpufreq/exynos-acme.c:1112-1130` set_boot_qos(): min+max pm_qos pinned to boot freq
  for **40 s** (`pm_qos_update_request_timeout(..., boot_qos, 40 * USEC_PER_SEC)`), then
  released -> real DVFS starts. Purely a Linux-side timer (no firmware counterpart seen); DT
  exynos9820.dts ~L5377.. per-domain "boot_qos_timeout, boot_freq". `exynos-emc.c:1680`
  "emc.blocked = 1; /* it is released after boot lock time */" (Linux mode changer).
  Relevance: Linux at 40 s starts changing CPUCL2 freq; Windows never does - so this is NOT a
  "firmware expects OS" timer. But it means Linux's first minute is fixed-freq too.
- CPUCL2 GCU (Global Control Unit) at **0x1E4C0000** (exynos-hiu.h `GCU_BASE`, exynos-ocp.c
  `GCU_BASE_ADDR`; both via ioremap, not in our Exynos9820.h -> UNVERIFIED for us): GCUCTL 0x22C,
  GCUERR 0x26C, HIU at +0xE00.., OCPTHROTTCTL 0x9F0, OCPTOPPWRTHRESH 0x83C, OCPTHROTTCNTA 0x938.
  IRQ INTREQ__CPUCL2_GCUIRQ shared by exynos-hiu / exynos-hiu-tb / exynos-ocp DT nodes.
  OCP = HW over-current throttle of CPUCL2 (PMIC S2MPS19 OCP_WARN -> GCU throttles; kernel only
  clips max freq & releases after 15 ms when util < 53%). HIU = HW DVFS/turbo-boost unit that the
  kernel enables (`hiu_control_mailbox` ENB_ACPM_COMM, HIUTOPCTL1 0xE00 bit0); off unless Linux
  turns it on. Neither looks like it would freeze the SoC if the OS ignores it (throttling, not
  stopping), but OCP throttling while nobody services GCUIRQ is UNVERIFIED territory.
- CPU_INFORM (power-mode hint read by EL3/flexpmu): `cal-if/exynos9820/flexpmu_cal_powermode_exynos9820.h`
  `PMUCAL_CPU_INFORM(n, 0x15860000, 0x0860 + 4*n)` for cpu0..7 (cpu6/7 = M4: PMU+0x878 / +0x87C).
  Values (`flexpmu_cal_define_exynos9820.h`): C1 0, C2 1, CPD 2, SICD 3, SLEEP 4, SLEEP_USBL2 5,
  STOP 6. `cal-if/pmucal_powermode.c` pmucal_powermode_hint() writes it for the current cpu before
  idle entry; pmucal_powermode_hint_clear() writes 0 (also on cluster enable); 
  pmucal_is_lastcore_detecting() treats CPD/SICD hints on an online CPU as "last core". Linux
  writes these only around PSCI CPU_SUSPEND. If Windows never calls CPU_SUSPEND the hint stays at
  whatever was there (0 = C1 from reset) - harmless unless something non-zero was left.
- `drivers/soc/samsung/acpm/acpm.c`: TIMER_APM at 0x15830000 (DT acpm node), TCVR +0x8, used only
  as a timestamp source (exynos_get_peri_timer_icvra), cleared at probe. No AP-heartbeat /
  APM-watchdog logic found in the kernel ACPM driver (acpm.c, acpm_ipc.c).
- EMA: `cal-if/fvmap_asv.c` asv_ema_init() is an empty function and nothing calls
  cal_dfs_set_ema() in this tree -> no kernel-applied EMA for 9820. Cold-temp: acpm_dvfs.c
  exynos_acpm_set_cold_temp() sends COLDTEMP_REQ to ACPM only on TMU_COLD events (OS-driven,
  default = not cold). Neither is a "firmware waits for OS" timer.
- DT (exynos9820.dts:375-420, 595-640): cpu6/7 "arm,cheetah", idle state CHEETAH_CPU_SLEEP
  psci-suspend-param 0x0010000 (entry 235 us, exit 220 us, min-res 3500 us). psci node:
  cpu_suspend 0xC4000001, cpu_off 0x84000002, cpu_on 0xC4000003. cpupm: cpd_cl0 psci-index 128
  siblings 0-5; **cpd_cl1 psci-index 128 siblings 6-7**; **sicd psci-index 256, siblings 0-7,
  entry-allowed "0-5", system-idle** -> Linux only ever requests SICD (system idle clock down,
  ACPM drops bus/MIF clocks) from cpus 0-5, and only when the M4 cluster is already CPD'd
  (exynos-cpupm checks all siblings 0-7 idle). If OUR ACPI _LPI tables let Windows request a
  system/cluster-level PSCI state while the M4s are running, ACPM/EL3 would be doing something
  Linux never does. => check our _LPI / PSCI suspend params (next).
- Our ACPI (firmware/Platform/Beyond0LtePkg/AcpiTables): no _LPI anywhere, parking protocol,
  FADT PSCI flag off -> Windows idles with WFI only, never PSCI CPU_SUSPEND. So the
  SICD/CPD/CPU_INFORM path is NOT exercised under Windows. SICD lead closed.
- **LLC lead** (`drivers/soc/samsung/exynos-sci.c` probe ~L880-920 + DT exynos-sci node): at boot
  Linux (1) asks ACPM (IPC channel 10) for the plugin's initial LLC region
  (SCI_LLC_REGION_ALLOC, GET), (2) **sets region LLC_REGION_LIT_MID_ALL (=1)** (DT
  use_init_llc_region=1, initial_llc_region=1), (3) **SCI_LLC_EN SET 1** (llc_enable=1). Region
  enum (exynos-sci.h): 0 DISABLE, 1 LIT_MID_ALL, 2 CPU_ALL, 3 CPU_2_GPU_2, 4 BIG_ALL, 5 GPU_ALL,
  6 LIT_MID_GPU_SHARE, 7 CPU_GPU_SHARE, 8 BIG_GPU_SHARE. I.e. stock Android runs with the **M4
  cluster excluded from LLC allocation**. Under Windows nobody sends these, so LLC on/off and the
  region are ACPM's defaults (unknown; not printed by print_sci_data). IPC encoding
  (exynos_sci_base_cmd): cmd[0] = idx[5:0] | data[11:6] | dir<<12 (dir 1 = SET, 0 = GET);
  idx: 0 LLC_INVAL, 1 LLC_FLUSH, 2 LLC_REGION_ALLOC, 3 LLC_EN; reply error = cmd[1] bits[15:13]
  (0 OK, 1 INVAL, 2 BUSY), GET data = cmd[1] bits[11:6]. response=true, indirection=false.
  Not time-triggered per se, but M4-specific and something Linux always does and we do not.
- **TIMER LEAD (strongest for a time-triggered freeze).** DT exynos9820.dts:926-938:
  ```
  timer { compatible = "arm,armv8-timer"; interrupts = PPI 13, 14, 11, 10 (LEVEL_LOW, all 8 cpus);
          clock-frequency = <26000000>; use-clocksource-only; use-physical-timer; };
  mct@10040000 { compatible = "samsung,exynos4210-mct"; ... use-clockevent-only; L0..L7 local timers }
  ```
  `drivers/clocksource/arm_arch_timer.c:1212-1216` ("Exynos Specific Device Tree Information"):
  `use-clocksource-only` -> arch_timer_use_clocksource_only = true; then
  arch_timer_starting_cpu() (:827-840) and arch_timer_dying_cpu() (:962-975) `goto
  skip_clockevent_setup` - "it doesn't need to setup clockevent configuration. this is only for
  exynos soc". => On stock Android **no core ever uses the generic timer's compare/IRQ
  (CNTP/CNTV_CVAL/TVAL, PPI 30/27)**; only CNTVCT is read. All per-CPU ticks come from the Exynos
  MCT (0x10040000, local timers L0-L7). Same in exynos9825-r.dts:921/949. Windows uses the GTDT
  generic timer as its per-CPU clock event on every core - a path Samsung's stack never exercises
  on the M4. (A55/A75 evidently cope; M4 behaviour UNVERIFIED.)
  Arithmetic worth checking: 26 MHz counter, 2^31 ticks = 82.6 s, 2^32 = 165.2 s - a 32-bit
  (signed TVAL / CVAL-compare) problem would fire at a FIXED counter value, not a fixed time after
  KD connect.
- Mainline Exynos 990 DT review (https://lkml.iu.edu/hypermail/linux/kernel/2410.1/10760.html,
  per WebFetch summary): timer node carries `clock-frequency = <26000000>` because "broken stock
  Samsung bootloader does not configure CNTFRQ_EL0"; also "no PMU model for cluster2 (Mongoose
  cores)". CNTFRQ_EL0 is per-core and written only at EL3 - whether Samsung EL3 sets it on an M4
  brought up by PSCI CPU_ON is UNVERIFIED (read CNTFRQ_EL0 in the park stub on cpu6/7).
- TMU (DT exynos9820.dts:5654-5666, ~5715-5775): tmuctrl_0 "BIG" @0x10090000 (id 0, sensors
  0x7C0), hotplug_enable, hotplug_in 91 / out 96 C; big thermal zone polling 1000 ms / passive
  50 ms, passive trip 83 C, hot 115 C. Linux hot-unplugs the M4s at 96 C; Windows has no TMU
  driver. A ~1 min thermal run-up remains possible but the coordinator saw the freeze at 520 MHz
  too (voltage there is ACPM's choice - UNVERIFIED).

## Summary of leads (2026-10-14)

1. Generic timer never used as clockevent on stock Android (use-clocksource-only, MCT local
   timers instead) - Windows' per-CPU tick on the M4 is untested territory. Time-triggered fits.
   Experiments: (a) at each KD snapshot record CNTVCT_EL0 / CNTPCT and CNTP_CVAL/CNTV_CVAL on cpu6/7;
   see if freezes cluster at a fixed counter value (0x8000_0000 = 82.6 s, 0x1_0000_0000 = 165 s at
   26 MHz) rather than at a fixed delay. (b) UEFI test: an M4 in the park stub programs CNTP/CNTV
   CVAL repeatedly (incl. across 2^31/2^32 counter values, and TVAL negative) for > 3 min.
   (c) read CNTFRQ_EL0 on cpu6/7 in the stub (Samsung EL3 known not to set it on 990).
2. Samsung 9820 "dummy TLBI" (`tlbi vaale1is, xzr` after every local TLBI VMALLE1 / IC IALLU,
   6 sites, CONFIG_SOC_EXYNOS9820) - an undocumented DVM/DSB erratum workaround Windows lacks.
   Experiment: the park stub / any EL1 code we own already follows it? Windows' own local
   TLBI/IC IALLU cannot be patched, but a hang in a local-invalidate+DSB NSH would match.
3. LLC: Linux sets SCI LLC region LIT_MID_ALL (M4 excluded) + LLC_EN via ACPM IPC ch 10 at boot;
   we send nothing. Experiment: from UEFI send SCI_LLC_REGION_ALLOC SET 1 then SCI_LLC_EN SET 1
   (cmd[0] = 0x1000 | (1<<6) | 2 and 0x1000 | (1<<6) | 3) on ACPM channel 10 - or LLC_EN SET 0
   (0x1000 | 3) to rule LLC out entirely.
4. Watchdog cl2 (0x10060000, multistage FIQ wdt on stock): our bootshim already writes WTCON=0;
   only relevant if something re-arms it. Low.
5. Nothing in Linux enables snoop/DVM for CPUCL2 (EL3/ACPM do it on power-up; Linux has no
   SCI coherency writes; the only SCI CRP_PM_CC1_* handshake is in ACPM's cluster2_off).
