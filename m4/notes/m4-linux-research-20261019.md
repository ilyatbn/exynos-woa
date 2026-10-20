# M4 (Mongoose) on Samsung Linux - what the stock 9820 kernel does specially (research 2026-10-19)

Read-only web research. Source tree: LineageOS/android_kernel_samsung_exynos9820 (cloned shallow into scratchpad).
Inferences are marked **(inference)**.

## Progress log

- started; cloning tree (sparse) for local grep.
- tree: https://github.com/LineageOS/android_kernel_samsung_exynos9820 branch lineage-23.2, HEAD a412cfaf86b9 (shallow clone, grep via `git grep HEAD`). Line numbers below are for that commit.

## Q1. CONFIG_SEC_ARM64_LSE_ATOMICS (and its sibling SEC_ARM64_LSE_SPINLOCK)

It does NOT disable LSE. It makes LSE **static** (no LL/SC fallback, no alternatives patching) - every
in-kernel atomic is an LSE instruction unconditionally.

- `arch/arm64/Kconfig:1048`
  ```
  config SEC_ARM64_LSE_ATOMICS
  	bool "Use LSE atomic instructions"
  	default n
  	select ARM64_LSE_ATOMICS
  	help
  	  Say Y here to make use of these instructions for the in-kernel
  	  atomic routines not dynamically but statically.

  config SEC_ARM64_LSE_SPINLOCK
  	bool "Use LSE atomics for spinlock"
  	depends on !SEC_ARM64_LSE_ATOMICS
  	default y
  	help
  	  Say Y here to make use of LSE instructions for spinlocks regardless
  	  of config ARM64_LSE_ATOMICS.
  ```
- `arch/arm64/Makefile:47` - with clang: `lseinstr := -DCONFIG_AS_LSE=1`, `KBUILD_CFLAGS += -march=armv8-a+lse`
  (so the compiler itself may also emit LSE for C atomics / builtins).
- `arch/arm64/include/asm/lse.h:16` (asm) `alt_lse llsc, lse` -> just `\lse`; `:44` (C)
  `#define ARM64_LSE_ATOMIC_INSN(llsc, lse) lse` instead of `ALTERNATIVE(llsc, lse, ARM64_HAS_LSE_ATOMICS)`.
- `arch/arm64/lib/Makefile:14` `ifndef CONFIG_SEC_ARM64_LSE_ATOMICS` -> out-of-line `atomic_ll_sc.o` is not even built.
- `arch/arm64/include/asm/spinlock.h:30-36` - the telling comment:
  ```
  /* Please try to use LSE Atomics unless you are sure LL/SC is safe. */
  #ifdef CONFIG_SEC_ARM64_LSE_SPINLOCK
  #define SEC_ARM64_LSE_ATOMIC_INSN(llsc, lse)	lse
  #else
  #define SEC_ARM64_LSE_ATOMIC_INSN(llsc, lse)	ARM64_LSE_ATOMIC_INSN(llsc, lse)
  #endif
  ```
  used by arch_spin_lock (ticket increment = `ldadda` instead of `ldaxr/stxr` loop) etc.
  So even in configs where SEC_ARM64_LSE_ATOMICS is off, Samsung forces LSE spinlocks by default (default y).
- **(inference)** Samsung treats LL/SC (LDXR/STXR exclusive loops) as potentially unsafe on this SoC family -
  the kernel never executes an LL/SC atomic. Remaining LDXR/STXR users in the kernel would only be code that
  hand-codes them (futex/cmpxchg paths are also LSE via lse.h). Whether Windows uses exclusives or LSE on a given
  path is unknown (we do not disassemble Windows - rule 11). Candidate behavioural test: bare-metal LL/SC contention
  (LDAXR/STLXR ticket lock) between an M4 and A55/A75 cores, vs LDADDA.

## Q2. Code keyed on the Samsung MIDR

- `arch/arm64/include/asm/cputype.h:84,94-97,153-154`: `ARM_CPU_IMP_SEC 0x53`, `ARM_CPU_PART_MONGOOSE 0x001`,
  `MEERKAT 0x002`, `CHEETAH 0x003`; `MIDR_MEERKAT`, `MIDR_CHEETAH`. **`MIDR_CHEETAH` / `MIDR_MEERKAT` are defined
  but referenced nowhere** (git grep over the whole tree: only the defines). No Samsung entry in cpu_errata.c /
  cpufeature.c / proc.S / entry.S / mm.
- `arch/arm64/kernel/perf_event.c:1180` `armv8_m3_pmu_init` ("armv8_exynos_m3", upstream M1-M3 event map) - PMU naming only.
- `drivers/soc/samsung/debug/exynos-helper.c:427-460` (panic/debug dump only) - Samsung-core IMPDEF memory-error
  syndrome registers, read on MONGOOSE/MEERKAT/CHEETAH:
  `S3_1_C15_C2_0/4` FEMERR0SR/1SR, `S3_1_C15_C2_1/5` LSMERR0SR/1SR, `S3_1_C15_C2_2/6` TBWMERR0SR/1SR,
  `S3_1_C15_C2_3/7` L2MERR0SR/1SR, L3: `msr S3_1_C15_C7_1, 0 ; isb` (bank select) then `S3_1_C15_C3_0/4` L3MERR0SR/1SR.
  -> diagnostic lead: dump these on the M4 (from our firmware, or after a reset if they survive warm reset) to see
  whether the hang leaves a cache/TLB-walk memory error syndrome.
- `drivers/soc/samsung/debug/exynos-coresight.c:120-160` (exynos_cs_get_pc, used by the lockup/"hardlockup" dumper):
  for CHEETAH (and A75/A55="ANANKE") it reads the hung core's PC externally via the debug APB:
  `DBG_UNLOCK(base); dbg_os_unlock(base); DBG_UNLOCK(base + PMU_OFFSET); val = CS_READQ(base + PMU_OFFSET, PMUPCSR);`
  ns = bit63, el = bits62:61. -> diagnostic lead: an A55 can sample the M4's PC/EL during the 30-75 s window
  via CoreSight external PMU PCSR (needs the per-cpu debug base addresses from the 9820 DT `coresight` node).
  Addresses from `arch/arm64/boot/dts/exynos/exynos9820-debug.dtsi:131-215` (exynos-ehld / coresight nodes, DT
  data, **not yet in Exynos9820.h - UNVERIFIED for our use**): cs_base 0x1E000000; per-cpu dbg-offset cpu0-3
  0x810000/0x910000/0xA10000/0xB10000, cpu4-5 0xC10000/0xD10000, **cpu6 0x410000, cpu7 0x510000** (CL2 = M4);
  `arch/arm64/include/asm/core_regs.h:113-115` `PMU_OFFSET 0x20000`, `PMUPCSR 0x200`, LAR 0xFB0.
  So M4 core 6 PCSR would be at 0x1E000000+0x410000+0x20000+0x200 = 0x1E430200 (inference from DT+header).
  `drivers/soc/samsung/debug/exynos-ehld.c:180` (Exynos "early hard lockup detector") samples this periodically.
- `arch/arm64/include/asm/tlbflush.h:102-110` - a 7th site of the known dummy-TLBI workaround (not head.S/proc.S):
  `local_flush_tlb_all(): dsb(nshst); tlbi vmalle1; #ifdef CONFIG_SOC_EXYNOS9820 tlbi vaale1is, xzr #endif dsb(nsh); isb`.
- `arch/arm64/mm/cache.S:154,253` `#ifndef CONFIG_SOC_EXYNOS9810` -> on 9810 only, `dc cvau` is replaced by `dc civac`
  in __flush_cache_user_range / similar. Not active on 9820 (noted as a 9810-M3 erratum precedent).
- `drivers/clocksource/arm_arch_timer.c`: no Samsung/Exynos workaround; only upstream FSL/HiSilicon/858921/1188873
  OOL workarounds and the `use-clocksource-only` DT flag (:1213, already known).
- `drivers/irqchip/irq-gic-v3.c`, `irq-gic.c`, `irq-gic-common.c`: no Exynos/Samsung strings.
- `arch/arm64/kernel/psci.c:80` cpu_psci_cpu_die: if `exynos_cpuhp_last_cpu(cpu)` -> CPU_OFF with power_state
  affinity level 1 (cluster down) instead of 0. (PSCI power_state is ignored by most firmware for CPU_OFF; inference.)
- `arch/arm64/kernel/Makefile:61` topology.o excluded on ARCH_EXYNOS (Samsung has its own topology/EMS) - scheduling only.

## Q4. Speculation (Spectre v2 / BHB / SSBD) on the M4

Generic upstream backport logic only, nothing Samsung-specific (`arch/arm64/kernel/cpu_errata.c`):
- `spectre_v2_safe_list` (:527) = A35/A53/A55 only. A75 and M4 (no CSV2) fall to `detect_harden_bp_fw()` (:196):
  SMCCC_ARCH_FEATURES(ARCH_WORKAROUND_1) via SMC; if 0 -> per-cpu bp-hardening callback
  `call_smc_arch_workaround_1()` (:162) = `smc ARM_SMCCC_ARCH_WORKAROUND_1` on the hardening paths
  (user-space branch to kernel address, context switch).
- SSBD (:328-349): `ARCH_WORKAROUND_2` SMC per cpu if firmware supports it (or PSTATE.SSBS if present).
- BHB (:752) generic `is_spectre_bhb_affected` / loop or firmware WORKAROUND_3.
- **(inference)** No reason to think Linux does something here Windows would not; Windows also uses the SMCCC
  workaround calls. Only relevant if Samsung's EL3 mishandles ARCH_WORKAROUND_1/2 on CL2 - untested.

## Q3. Big-cluster (CPUCL2, cpus 6-7) specific setup

### HIU / HAFM - hardware DVFS + "turbo boost" unit on the M4 cluster (CONFIG_EXYNOS_PSTATE_HAFM=y in beyond0lte_defconfig:758)
- `drivers/soc/samsung/exynos-hiu.h:7` `#define GCU_BASE (0x1E4C0000)` (GCU = CPUCL2 "global control unit",
  inference from IRQ name INTREQ__CPUCL2_GCUIRQ). Regs: GCUCTL 0x22C, GCUERR 0x26C, HIUTOPCTL1 0xE00 (ACTDVFS
  [29:24], SR1INTR bit15, mbox err [14:11], ENB_SR1INTR bit5, err-irq enables [4:1], ENB_ACPM_COMM bit0),
  HIUTOPCTL2 0xE04 (SEQNUM [28:26], LIMITDVFS [19:14], REQDVFS [13:8], REQPBL [7:0]), HIUTBCTL 0xE10 (BOOSTED
  bit31, B1..B3_INC, TBDVFS [13:8], PC_DISABLE bit1, TB_ENB bit0), HIUTBPSCFG 0xE14+4n (8 "power state configs"),
  HIUTBPWRTHRESH 0xE34+4n.
- DT `arch/arm64/boot/dts/exynos/exynos9820.dts:856-880`:
  ```
  exynos_hiu: exynos-hiu { compatible = "samsung,exynos-hiu";
      interrupts = <0 INTREQ__CPUCL2_GCUIRQ 0>; boot-freq = <1378000>; boost-threshold = <2080000>;
      cal-id = <ACPM_DVFS_CPUCL2>; sw-pbl = <0x57>; bl1/2/3-inc = <0>; tb-enabled = <1>; pc-enabled = <1>;
      operation-mode = <0>  /* polling */; sibling-cpus = "6-7";
      config-table = < PB BL PBL PWR_THR_INC x8 >; bl1/bl2-tbpwr-threshold = <0 0 3 0xFFFF>; };
  ```
- `drivers/soc/samsung/exynos-hiu.c`:
  - The big cluster's frequency is NOT set through plain ACPM DVFS: `exynos_hiu_set_freq()` (:312) writes
    REQDVFS/REQPBL into HIUTOPCTL2 ("SR0"), then polls until HIUTOPCTL1.SR1INTR and ACTDVFS == requested level;
    a mailbox error -> `hiu_mailbox_err_handler(); BUG_ON(1);` (errors: SR1 unexpected, SR1 bad sequence num,
    "TIMEOUT on SR1 write", "SR0 read twice or more").
  - :323-336 comment + code:
    ```
    /*
     * If HIU H/W communicates with ACPM depending on middle core's p-state,
     * there might be ITMON error due to big core's cpd and releated register access at the same time
     * These codes below prevent that case.
     */
    if (req_freq >= data->boost_threshold && !data->cpd_blocked) {
        data->cpd_blocked = true; ...
        disable_power_mode(cpumask_any(&data->cpus), POWERMODE_TYPE_CLUSTER);
    ```
    i.e. at >= boost-threshold (2.08 GHz) **CL2 cluster power-down is forbidden** because concurrent HIU<->ACPM
    traffic and a CL2 CPD cause an interconnect (ITMON) error.
  - `__exynos_hiu_update_data()` (:497) at cpufreq registration: set LIMITDVFS, TBDVFS, boost increments,
    8 TB power-state configs, PC enable, TB enable, power thresholds, enable error interrupts, then
    `hiu_control_mailbox(1)` = ENB_SR1INTR + **ENB_ACPM_COMM** (HIU talks to ACPM by itself from then on), then two
    "dummy" requests (boost_threshold then cur_freq) "to give plugin threshold" (i.e. to tell the ACPM plugin).
  - `check_hiu_need_register_restore()` (:270): if ENB_SR1INTR reads 0 (HIU regs lost, e.g. after CL2 power-down)
    the whole config is re-written before the next request.
- **(inference)** The M4 cluster has autonomous hardware ("turbo boost"/"power borrowing", TB_ENB, PC = power
  control?) that, once enabled, exchanges messages with ACPM. Under Windows nobody programs HIU, so it stays at
  its sboot/reset state. Whether sboot leaves TB/ACPM_COMM enabled is unknown - a read of 0x1E4C0E00/E04/E10 from
  our firmware (with CL2 powered) would show it. A HIU<->ACPM mailbox stall/timeout with nobody servicing
  GCUIRQ is a plausible "silent hang after N s" mechanism, but unproven.

### exynos-cpupm + PMUCAL: Linux itself writes PMU/SYSREG registers around every core/cluster power transition
- `drivers/soc/samsung/exynos-cpupm.c:289-312`: `cpu_enable/disable -> cal_cpu_enable/disable(cpu)`,
  `cluster_enable(cpu) -> cal_cluster_enable(vcluster_id); big_reset_control(1)`, `cluster_disable` the reverse.
  Called from: hotplug states (`:988-1004`, `CPUHP_AP_EXYNOS_CPU_UP_POWER_CONTROL` -> `cpuhp_cpupm_online()` :326:
  if no cpu of the cluster is online yet -> `cluster_enable(cpu)`; then `cpu_enable(cpu)`), and every cpuidle
  entry/exit (`exynos_cpu_pm_enter()` :702 -> `cpu_disable(cpu)` + maybe `cluster_disable` and OR the PSCI
  index; `exynos_cpu_pm_exit()` :732 -> `exit_mode` (cluster_enable) + `cpu_enable(cpu)`).
- `drivers/soc/samsung/cal-if/pmucal_cpu.c:16-80`: enable = `pmucal_powermode_hint_clear()` (CPU_INFORM := 0) +
  run `pmucal_cpu_list[cpu].on`; disable = `pmucal_powermode_hint(cpu_inform_c2)` (CPU_INFORM := 1 = C2) + `.off`;
  cluster disable (:180) hint `cpu_inform_cpd` (2).
- CPU_INFORM registers (`cal-if/exynos9820/flexpmu_cal_powermode_exynos9820.h:1-10`): PMU 0x15860000 + 0x860 + 4*cpu
  (cpu6 0x878, cpu7 0x87C). Values (`flexpmu_cal_define_exynos9820.h:1-5`): C1=0, C2=1, CPD=2, SICD=3, SLEEP=4.
  `pmucal_powermode_hint()` writes the **calling** cpu's slot (`smp_processor_id()`).
- The sequences Linux uses (`cal-if/exynos9820/flexpmu_cal_cpu_exynos9820.h`, `#ifndef ACPM_FRAMEWORK` branch,
  lines ~1-120) - for CL2:
  ```
  core20_on:  GRP2_INTR_BID_ENABLE 0x15870000+0x200 bit6 := 0
              GRP2_INTR_BID_CLEAR  0x15870000+0x20C bit6 := 1 (CLEAR_PEND, pend reg +0x208)
              CLUSTER2_CPU0_INT_EN 0x15860000+0x1544 bit3 := 0
  core20_off: GRP2_INTR_BID_ENABLE +0x200 bit6 := 1
              GRP1_INTR_BID_CLEAR  +0x10C bit6 and bit14 := 1 (pend +0x108)
              CLUSTER2_CPU0_INT_EN +0x1544 bit3 := 1
  core21_*:   same with bit7 / bit15 and INT_EN 0x15860000+0x15C4
  core2x_status: CLUSTER2_CPU0/1_STATUS 0x15860000+0x1504 / +0x1584 bit0
  cluster2_on: SYSREG_CPUCL2_BUS_COMPONENT_DRCG_EN 0x1D100000+0x104 := 0xFFFFFFFF
  cluster2_status: CLUSTER2_NONCPU_STATUS 0x15860000+0x1604 bit0
  ```
  (clusters 0/1 have no on/off sequence on the AP side - only CL2 has `cluster2_on`.)
  The 6 ARM cores get exactly the same per-core pattern (bits 0-5), so the per-core INT_EN/BID writes are not
  M4-specific; **the CL2-only item is the DRCG_EN write on cluster power-up**.
- **(inference)** On Linux every time CL2 comes up (boot-time hotplug of cpu6, and every exit from CL2 cluster
  power-down) the AP rewrites `SYSREG_CPUCL2 + 0x104 = 0xFFFFFFFF` (dynamic root clock gating for the CL2 bus
  components). SYSREG state is lost on CL2 non-CPU power-down; sboot may not restore it. Windows only does PSCI
  CPU_ON/CPU_SUSPEND, so on Windows this register keeps its reset value. Read 0x1D100104 under Android (needs root -
  not possible over our shell) or from our firmware after PSCI CPU_ON of cpu6 to see whether it differs.
  Effect of DRCG off is normally "clocks always on" (power), not a hang - weak lead, but it is the only CL2-only
  AP write in the power path.
- ACPM_FRAMEWORK branch of the same header (what ACPM itself runs, for reference): CL2 cpu on/off wrap the ACPM
  ext funcs with `CPUCL2_HCHGEN_CLKMUX 0x1D120000+0x850 bit4 := 1 ... := 0`; `cluster2_off` also does
  `CRP_PM_CC1_0 0x1A000000+0x1A4 [1:0] := 3`, `CRP_PM_CC1_3 0x1A000000+0x21C [1:0] := 3`, waits bit16 == 0
  on both, then `cluster2_noncpu_down`, `CHECK_SKIP CPUCL2_OPTION 0x15860000+0x1C8C bit0`. `ops_l3flush_status`
  = CLUSTER2_NONCPU_INT_IN 0x15860000+0x1640 bit2 (L3 flush status lives on CL2 - inference: the M4 cluster
  holds the L3 control).
- `cpupm` DT (`exynos9820.dts:603-640`): `cpd_cl1` (cluster power-down for cpus "6-7", psci-index 128,
  entry-allowed "6-7"); `sicd` (system idle, psci-index 256) `siblings = "0-7"; entry-allowed = "0-5"` ->
  **an M4 core is never allowed to be the cpu that enters SICD (system-level idle)**; the last-man for SICD
  must be an A55/A75. (`can_enter_power_mode()` :616: `if (!cpumask_test_cpu(cpu, &mode->entry_allowed)) return 0;`)
  -> lead: if our ACPI _LPI tables expose a system/package-level state to cpus 6-7, Windows can request it from
  an M4 (PSCI CPU_SUSPEND with a system-level power_state) - a combination Samsung never exercises.
- `drivers/soc/samsung/exynos-reboot.c:196` `big_reset_control()` - DFD (debug dump) reset-disable bits for the big
  cores, only when `RESET_SEQUENCER_CONFIGURATION & DFD_EDPCSR_DUMP_EN`; debug-only.
- `cal-if/exynos9820/cal_data.c:360-403` `exynos9820_set_cmu_smpl_warn()`: enables SMPL_WARN clock-halving per
  cluster, for CL2 via `CMU_CPUCL2 0x1D120000 + 0x838 (CTRL_EXT_EVENT)` bit0 := 1 (and bit7 MASK_SMPL_INT := 0).
  Hardware throttle on PMIC brown-out warning - inference: irrelevant unless the warning fires.

### OCP (over-current protection) on CL2 - `drivers/soc/samsung/exynos-ocp.c` (CONFIG_EXYNOS_OCP=y, defconfig:4883)
- Same GCU block: `GCU_BASE_ADDR 0x1E4C0000` (:33); OCPTHROTTCTL 0x9F0 (TEW bit2, TRW [7:3], BPC [20:16], TCRST
  [23:21], DPM_THROTTSET bit24, OCPBPCHIT bit26, OCPTHROTTERRA bit27), GCUCTL 0x22C (OCPBPCEN bit25,
  OCPTHROTTERRAEN bit24), OCPTOPPWRTHRESH 0x83C (IRP [29:24]), OCPTHROTTCNTA 0x938 (TDC [15:4]).
- DT `exynos9820.dts:1193`: `interrupts = <0 INTREQ__CPUCL2_GCUIRQ 0>; sibling-cpus = "6-7"; max-freq-wo-ocp = <2080000>;
  release-mode = <1>; release-threshold = <53>; release-duration = <15>;`
- Probe disables the OCP irq (`control_ocp_interrupt(0)`), `CONTROL_OCP_WARN(0)` on the PMIC (S2MPS19
  OCP_WARN1 over I2C) in some path, and enables `control_ocp_interrupt(1)` once cpufreq is up. IRQ handler
  (:409) clears OCPTHROTTERRA / DPM_THROTTSET / TDC and caps the CL2 max frequency for 15 ms windows.
- **(inference)** Hardware throttling ("u-throttling and clk div-2", comment :417) happens without software; the
  IRQ only adjusts the DVFS cap. Windows not handling GCUIRQ (shared by HIU + OCP, level-high) should leave it
  pending/masked at the GIC, not hang the SoC. Low lead unless the line is routed/enabled in our GIC setup.

### When the CL2 AP-side writes happen (ordering)
- `include/linux/cpuhotplug.h:122` `CPUHP_AP_EXYNOS_CPU_UP_POWER_CONTROL` sits between `CPUHP_AP_OFFLINE` (:97) and
  `CPUHP_AP_ONLINE` (:145) = an AP "STARTING" state: it runs **on the newly started core itself**, IRQs off, right
  after PSCI CPU_ON returns into `secondary_start_kernel()` -> `notify_cpu_starting()`.
  So the first instructions Linux runs on cpu6 after its MMU/caches are on include (as cpu6): CL2 `cluster2_on`
  (DRCG_EN := 0xFFFFFFFF, first core of the cluster only), CPU_INFORM[6] := 0, GRP2_INTR_BID_ENABLE bit6 := 0 +
  clear pending, CLUSTER2_CPU0_INT_EN bit3 := 0.
- `CPUHP_AP_EXYNOS_CPU_DOWN_POWER_CONTROL` (:98) - offline path (cpuhp_cpupm_offline -> cpu_disable / cluster_disable).
- **(inference)** CLUSTERx_CPUy_INT_EN bit3 + GRP2_INTR_BID look like the flexPMU "core entered WFI/power-down
  request" interrupt to APM: Linux disables it while a core is online and running, and enables it (and sets
  CPU_INFORM = C2) only right before a PSCI CPU_SUSPEND power-down. If on the Windows path these bits/CPU_INFORM
  for cpus 6-7 are left in a different state (sboot or EL3 CPU_ON leftovers) every WFI of an M4 could be treated
  as a power-down request. The 6 ARM cores get the same treatment in Linux and work under Windows, so this only
  matters if the CL2 values differ at hand-off. Cheap check from our firmware after PSCI CPU_ON of cpu6 vs cpu4:
  PMU 0x15860000 + 0x878/0x87C (CPU_INFORM 6/7) vs +0x870/0x874, +0x1544/+0x15C4 (CL2 INT_EN) vs +0x1344/+0x13C4,
  0x15870000 + 0x200/0x208/0x108 (GRP2 enable, GRP2 pend, GRP1 pend), SYSREG_CPUCL2 0x1D100104 (DRCG_EN).
  All addresses UNVERIFIED (from the kernel CAL tables, not in Exynos9820.h).

### Cluster-2 watchdog (`drivers/watchdog/s3c2410_wdt.c`, DT `exynos9820.dts:4486-4507`)
- Two WDTs: `watchdog_cl0@10050000` ("samsung,exynos9-v1-wdt", timeout 30 s, PMU CLUSTER0_NONCPU_INT_EN 0x1244 bit2,
  RST_STAT bit24) and **`watchdog_cl2@10060000`** ("samsung,exynos9-v2-wdt", timeout 30 s, PMU
  `EXYNOS_CLUSTER2_NONCPU_INT_EN 0x1644` bit2, RST_STAT bit23 "CLUSTER2 WDTRESET", `use_multistage_wdt` =
  "FIQ debug watchdog", runs at 70 % (`MULTISTAGE_WDT_RATIO`) of the CL0 period; irq 497 in debug-snapshot-soc).
- Comment :232: "MASK_RESET register was replaced by CLUSTERx_NONCPU_INT_TYPE register. DISABLE register was removed".
- **(inference)** A CL2-owned watchdog exists whose reset/interrupt routing is gated by a CL2 NONCPU PMU register.
  If sboot leaves WDT_CL2 running, its expiry routing could depend on CL2 being powered/active. 30 s timeout is in the
  observed 30-75 s window - worth one read of 0x10060000 (WTCON) / 0x10060008 (WTCNT) from our firmware/Windows to
  rule it out (WTCON bit5 = enable, bit0 = reset enable on S3C2410-style WDT).

### Other checks (negative results)
- No writes to Samsung-core IMPDEF system registers anywhere (`git grep` for `msr s3_*`, `S3_x_C15`, `sys_reg(3,x,15|11,...)`
  over arch/arm64, drivers/soc/samsung, cpuidle, cpufreq, perf, clocksource, irqchip): only reads in exynos-helper.c.
- `drivers/soc/samsung/exynos-el3_mon.c`: SMCs for kernel-text protection, debug mem, SEH address, PD on/off prep -
  nothing CPU/cluster specific.
- `drivers/soc/samsung/exynos-sci.c`: LLC via ACPM IPC (already known); has LLC regions "BIG_ALL", "BIG_GPU_SHARE".
- `drivers/cpufreq/exynos-acme.c`: generic DVFS via `cal_dfs_set_rate` / ACPM; CL2 boot freq read from ACPM.
  `drivers/cpufreq/exynos-ff.c`: CL2 "frequency filter" for HAFM-TB (not enabled: CONFIG_EXYNOS_PSTATE_HAFM_TB off).
- Samsung uH/RKP (EL2 hypervisor, `CONFIG_UH`) is **not set** in the LineageOS beyond0lte defconfig and that kernel
  runs all 8 cores -> uH/RKP initialisation is not a requirement for the M4 (inference from Lineage working).
- `CONFIG_ARM64_HW_AFDBM` is `default y` in `arch/arm64/Kconfig:991` but explicitly `# ... is not set` in every 9820
  defconfig (`exynos9820-beyond0lte_defconfig:628`). So Linux on the 9820 never sets TCR_EL1.HA/HD: the M4's
  table walker never does hardware Access-flag / dirty-bit updates. No comment explains why (A55 erratum 1024718
  is separately enabled at :516). **(inference)** Windows can use hardware AF (TCR_EL1.HA) when ID_AA64MMFR1_EL1.HAFDBS
  says so; this is a path Samsung never exercises on the M4. Testable bare-metal: set TCR_EL1.HA (and HD) on the
  M4 and run the existing stress with pages whose AF=0 being touched while A55/A75 cores also touch/clear them.
  Also read ID_AA64MMFR1_EL1 on the M4 (HAFDBS field [3:0]) vs A55/A75.
- `CONFIG_ARM64_VHE` not set (kernel at EL1, like Windows here).

## Ranked leads for "Windows on the M4 hangs the SoC" (summary)
1. Hardware AF/DBM (TCR_EL1.HA/HD) - disabled by Samsung for the whole 9820 (non-default config), Windows may use it.
2. LL/SC vs LSE - Samsung forces LSE for every kernel atomic + spinlock ("unless you are sure LL/SC is safe");
   Windows code may use LDXR/STXR loops (and user-mode code certainly does). Test LL/SC contention M4 <-> ARM cores.
3. CL2 HIU/GCU (0x1E4C0000) turbo-boost/ACPM mailbox state left by sboot; Samsung forbids CL2 power-down at high
   p-state because of an ITMON (interconnect) error with concurrent HIU<->ACPM traffic. Read HIUTOPCTL1/2, HIUTBCTL.
4. CL2-only AP writes on cluster power-up: SYSREG_CPUCL2 0x1D100104 DRCG_EN := 0xFFFFFFFF; plus per-core
   CPU_INFORM / INT_EN / INTR_BID state after CPU_ON. Compare cpu6 vs cpu4 values from our firmware.
5. SICD/system idle is never entered from an M4 (`entry-allowed = "0-5"`): check our _LPI tables do not offer
   a cluster/system state to cpus 6-7 deeper than core power-down.
6. WDT_CL2 at 0x10060000 (30 s, CL2-gated routing via PMU 0x15861644 bit2) - rule out with a WTCON read.
7. Diagnostics: CoreSight PMUPCSR of cpu6 at 0x1E430200 (cs_base 0x1E000000 + dbg 0x410000 + 0x20000 + 0x200) to sample
   the M4 PC/EL from an A55 during the 30-75 s window; Samsung-core MERR syndrome regs (S3_1_C15_C2_0..7, C3_0/4).
