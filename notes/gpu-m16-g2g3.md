# M16 G2/G3 — G3D power + clock recipe (source) and the UEFI GPU probe DXE

Scope: G2 = research only, from Samsung/LineageOS 9820 kernel source (fetched raw from GitHub
LineageOS/android_kernel_samsung_exynos9820 branch lineage-22.2 into
`scratchpad\g3d`, treated as data; `G` below). G3 = `Beyond0LteGpuProbeDxe`, read-only by default.
No device access in this session (no adb, no flashing); the main session boots the build.
Register offsets for the GPU itself: `notes/gpu-panfrost-repos.md` (panfrost_regs.h, clones in
`C:\Users\ilya\code\mali`).

## Progress log

- 2026-10-12 start: worktree fast-forwarded to main 97cf148. Read MILESTONES M16 G1-G12,
  notes/gpu-panfrost{,-repos}.md, notes/d8-audio-m3-abox.md F5 (pd-aud pattern: PMU CONFIG/STATUS,
  need_smc -> exynos_pd_tz_restore SMC). Fetching G3D sources next.
- 2026-10-12 fetched into `scratchpad\g3d` (all OK, line counts): plat_gpu_control.c 598,
  plat_gpu_notifier.c 421, plat_gpu_dvfs_*.c, plat_mali_kbase_platform.{c,h}, plat_gpu_integration_callbacks.c 714,
  cal_flexpmu_cal_local_exynos9820.h 1197, cal_cmucal-{sfr.c 11115,node.c 2845,vclk.c,qch.c},
  cal_acpm_dvfs_exynos9820.h 38, calif_{cal-if.c 487,pmucal_local.c 250,acpm_dvfs.c},
  exynos9820.dts 6297, exynos9820-pm-domains.dtsi 231, exynos9820-mali_common.dtsi 80,
  dtb_clock_exynos9820.h 545, exynos-pd.c 495. (el3_mon/smc headers reused from scratchpad\abox.)
- 2026-10-12 read: mali node, pd-embedded_g3d, pmucal embedded_g3d on/off/save/status, kbase
  gpu_is_power_on, cal_pd_control, cal_dfs_set_rate, CMU_G3D/PLL_G3D SFRs -> F1-F6 below.
- 2026-10-12 G2 done: F1-F6 written, MILESTONES G2 -> done; committed as the G2 commit.

## Findings (G2)

### F1 Mali node (`G\exynos9820-mali_common.dtsi:14-23,40-47`, included from `exynos9820.dts:36`)
- `mali: mali@18500000 { compatible "arm,mali"; reg = <0x0 0x18500000 0x5000>;
  interrupts = <0 288 0>, <0 287 0>, <0 289 0>; interrupt-names = "JOB","MMU","GPU";
  g3d_cmu_cal_id = <ACPM_DVFS_G3D>; samsung,power-domain = <&pd_embedded_g3d>; }`
  -> GIC SPI 288/287/289 = INTID 320 (JOB) / 319 (MMU) / 321 (GPU). Trigger field 0 (as DT, level/edge
  flags not decoded here). **No `clocks` property** — the GPU clock is driven only via ACPM DVFS.
- `gpu_pmu_status_reg_offset = <0x2104>`, `gpu_pmu_status_local_pwr_mask = <0x1>` (:40-41) — the
  kbase "is the GPU powered" check = PMU+0x2104 bit0.
- DVFS table 702000..156000 kHz (:29-37), `gpu_dvfs_start_clock = 260000` (:45),
  `gpu_default_voltage = 800000` uV (:47).

### F2 Power domain pd-embedded_g3d (`G\exynos9820-pm-domains.dtsi:77-83`)
- `pd-embedded_g3d@15862100 { reg = <0x0 0x15862100 0x20>; cal_id = <0xB1380007>; need_smc = <0x18410204>; }`
- `need_smc` is set → power-on runs `exynos_pd_tz_restore(0x18410204)` = **SMC**
  `SMC_CMD_PREAPRE_PD_ONOFF 0x82000410 (EXYNOS_WAKEUP_PD_DOWN=1, 0x18410204, RUNTIME_PM_TZPC_GROUP=2)`
  after the pmucal on-sequence (`G\calif_pmucal_local.c:37-52`; SMC wrapper `scratchpad\abox\exynos-el3_mon.c:239-245`).
  0x18410204 is inside the G3D block (CMU_G3D 0x18400000, SYSREG_G3D 0x18420000): by name of the
  gate `CLK_BLK_G3D_UID_D_TZPC_G3D` (`G\cal_cmucal-sfr.c:1246`) a D_TZPC_G3D at 0x18410000 is
  INFERENCE (address not named in cmucal).
- cal_pd_control index 7 additionally calls `bts_pd_sync` (bus traffic shaper QoS, `G\calif_cal-if.c:192-197`).

### F3 pmucal sequences (`G\cal_flexpmu_cal_local_exynos9820.h`)
PMU = 0x15860000 (EXYNOS_PMU_BASE, VERIFIED). CMU_CMU = 0x1A240000 (= EXYNOS_CMU_TOP_BASE, VERIFIED).
- `embedded_g3d_on` (:555-567), in order:
  1. WRITE CMU_CMU+0x148 `PLL_CON2_PLL_G3D` = 0x30000003 (full mask)
  2. WRITE CMU_CMU+0x140 `PLL_CON0_PLL_G3D` bit4 = 1 (bit 4 has no SFR_ACCESS name in cmucal-sfr.c:2760-2765,
     which names only ENABLE 31 / STABLE 29 / P 13:8 / M 25:16 / S 2:0 — so bit 4's meaning is UNVERIFIED;
     by analogy with Exynos PLL_CON0 MUX_SEL = FOUT-vs-OSCCLK, INFERENCE)
  3. WRITE PMU+0x2380 `G3D_CONFIGURATION` bit0 = 0; WAIT PMU+0x2384 `G3D_STATUS` bit0 == 0
  4. SET_BIT_ATOMIC PMU+0x2120 `EMBEDDED_G3D_OUT` value 0x2 → written to PMU+(0x2120|0xC000) = 0x1586E120
     (`scratchpad\abox\calif_pmucal_rae.c:145-151`); DELAY 0x118 = 280 us (`rae.c:356-357` udelay)
  5. WRITE G3D_CONFIGURATION bit0 = 1; WAIT G3D_STATUS bit0 == 1; DELAY 280 us
  6. WRITE PMU+0x2100 `EMBEDDED_G3D_CONFIGURATION` bit0 = 1; WAIT PMU+0x2104 `EMBEDDED_G3D_STATUS` bit0 == 1
  WAIT timeout = 2000 x udelay(1) (`rae.c:102-116`).
- then (pmucal_local_enable, `G\calif_pmucal_local.c:37-60`): **SMC DTZPC restore (need_smc 0x18410204)**,
  then restore the save list.
- `embedded_g3d_save` (:569-584): CMU_G3D (0x18400000) DIV_CLK_G3D_BUSP 0x1804, DIV_CLK_G3D_BUSD 0x1800,
  MUX_CLK_G3D_BUSD 0x1000, PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER 0x100, ..._G3D_BUS_USER 0x120,
  ..._G3D_SWITCH_USER 0x140, G3D_SHORTSTOP 0x820, G3D_CLKDIV2PH 0x824, QCH_CON_BUSIF_HPMG3D_QCH 0x303C,
  EMBEDDED_CMU_G3D_CONTROLLER_OPTION 0x804, CMU_G3D_CONTROLLER_OPTION 0x800; SYSREG_G3D (0x18420000)
  BUS_COMPONENT_DRCG_EN 0x104; HPM (0x18430000) THRV_DD 0x80, CONFIG 0x0.
  ⇒ CMU_G3D and SYSREG_G3D are **inside** the domain (their state is lost at power-down): no CMU_G3D read
  while the domain is off. CMU_CMU (PLL_G3D) is outside it (TOP, always on while the AP runs — INFERENCE).
- `embedded_g3d_off` (:586-594): QCH_CON_BUSIF_HPMG3D_QCH bit2 = 0; EMBEDDED_G3D_CONFIGURATION = 0, wait
  status 0; DELAY 0x8A; EMBEDDED_G3D_USER mux bit4 = 0; PLL_CON0_PLL_G3D bit4 = 0; PLL_CON2_PLL_G3D = 0x10000001.
  (Note: the off sequence does NOT touch G3D_CONFIGURATION 0x2380.)
- `embedded_g3d_status` (:596-598): READ PMU+0x2104 bit0 — same register as the kbase DT
  `gpu_pmu_status_reg_offset` (F1) and kbase `gpu_is_power_on()` (`G\plat_gpu_control.c:79-89`).

### F4 Who powers it on Android (order)
- kbase `gpu_power_on` → `pm_runtime_get_sync` (`G\plat_gpu_notifier.c:99-133`) → genpd
  `exynos_pd_power_on` (`G\exynos-pd.c:99-135`: idle-ip status + `exynos_bts_scitoken_setting` pre-hook (the latter only if the pd has a devfreq sync index, :64-71),
  then `cal_pd_control(0xB1380007, 1)`) → `pmucal_local_enable(7)` (F3) + `bts_pd_sync` (`G\calif_cal-if.c:182-197`).
- then `pm_callback_runtime_on` (`G\plat_gpu_notifier.c:253-278`) → `gpu_set_target_clk_vol(gpu_dvfs_start_clock
  = 260000)` → `gpu_set_dvfs_using_calapi` (`G\plat_gpu_control.c:119-161`), which refuses if
  `gpu_is_power_on()` is false, then `cal_dfs_set_rate(ACPM_DVFS_G3D, clk*KHZ)` (KHZ = 1000, `G\plat_mali_kbase_platform.h:45`; clk in kHz → rate in Hz).
- Mali-internal L2/shader/tiler power (GPU_PWRON regs) is kbase's generic code after this — G4.

### F5 Clock/voltage = ACPM, not direct CMU
- `ACPM_DVFS_G3D = 0x0B04000A` (`G\dtb_clock_exynos9820.h:507`); IS_ACPM_VCLK (type|subtype == 0x0B040000,
  `G\calif_cmucal.h:37-39,58,214-217`) → `cal_dfs_set_rate` → `exynos_acpm_set_rate(GET_IDX = 10, rate)`
  (`G\calif_cal-if.c:57-77`) = ACPM IPC on channel **5** (`acpm_dvfs` node, `G\exynos9820.dts:2072-2075`)
  with cmd[0]=10 (dvfs_g3d, `G\cal_acpm_dvfs_exynos9820.h:13`), cmd[1]=rate, cmd[2]=FREQ_REQ 0
  (`G\calif_acpm_dvfs.h:19`), cmd[3]=0 (`G\calif_acpm_dvfs.c:23-45`). APM firmware sets PLL_G3D **and the
  G3D voltage** (buck via SPEEDY, INFERENCE — no regulator_set_voltage in kbase platform; `g3d_m_regulator`
  is only an extern, `plat_gpu_control.c:42`). `exynos-devfreq` `dvfs_gpu available = "false"` (`exynos9820.dts:4865-4870`).
- `cal_dfs_get_rate` for G3D = `vclk_recalc_rate` = CPU-side read of the CMU tree (`calif_cal-if.c:106-116`),
  so PLL_G3D/dividers are readable directly. PLL_G3D (`G\cal_cmucal-node.c:138`): PLL_0818X, parent OSCCLK_CMU,
  rate table (:58-63) {245.76 MHz M430 P13 S0, 650 MHz M100 P1 S2, 320 MHz M160 P13 S0, 160 MHz M80 P13 S0}.
  With a 26 MHz OSCCLK (UNVERIFIED here) 650/320/160 check out as 26*M/(P*2^S); the 245.76 entry does not
  (26*430/13 = 860) — the table is not authoritative; APM owns the real G3D rates (702..156 MHz).
- PLL_G3D: CMU_CMU+0x008 LOCKTIME, +0x140 CON0 (ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0),
  +0x148 CON2 (no SFR_ACCESS) (`G\cal_cmucal-sfr.c:63-64,2760-2765`).
- CMU_G3D 0x18400000 +0x8000 (`G\cal_cmucal-sfr.c:22`): PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER 0x100
  (MUX_SEL 4, BUSY 7), G3D_BUS_USER 0x120, G3D_SWITCH_USER 0x140, MUX_CLK_G3D_BUSD 0x1000,
  DIV_CLK_G3D_BUSD 0x1800, DIV_CLK_G3D_BUSP 0x1804, CONTROLLER_OPTION 0x800/0x804 (bits 29 PM, 28 auto-CG),
  gate CLK_BLK_G3D_UID_GPU_IPCLKPORT_CLK **0x2004** (CG_VAL 21, MANUAL 20, auto-CG 28; `sfr.c:1242,6049-6051`),
  CMU_G3D pclk gate 0x2000, D_TZPC_G3D pclk gate 0x203C (`sfr.c:1239,1246`).
  Gate semantics (`scratchpad\abox\calif_ra.c:415-437`): MANUAL=1 → CG_VAL is the clock (1 pass, 0 gated);
  MANUAL=0 → automatic clock gating. ⇒ "GPU clock forced off" = MANUAL=1 && CG_VAL=0.

### F6 Verdict for G3 / G4
- **Power-on needs an SMC** (DTZPC restore, F2) and the voltage/frequency go through **ACPM** (F5).
  The PMU/CMU writes themselves are plain MMIO (pmucal uses __raw_writel), but without the TZPC restore
  the G3D SFRs may be secure-only after a power cycle (INFERENCE, same as pd-aud in d8-audio-m3-abox F5).
  ⇒ G3's `BEYOND0LTE_GPU_POWER_ON` stays an **UNVERIFIED stub** (task rule: only if no SMC and no PMIC write).
- Read-only probe gate: PMU+0x2104 bit0 (kbase's own check) **and** PMU+0x2384 bit0 (the on-sequence
  waits for both); CMU_G3D reads only inside the gate; GPU MMIO reads only if additionally the GPU clock gate
  is not forced off (F5). PMU and CMU_CMU reads are always safe-by-construction (always-on blocks).
- Does sboot leave G3D on at UEFI entry? Unknown — that is exactly what the probe's first line answers.
- Residual hazard even with the domain on: the G3D D_TZPC state (if sboot/EL3 left it secure, a NS read of
  0x18500000 is an external abort). Not decidable from source; the progress word brackets each read.

## Review of 934d8ab (G2)
- Checked mechanically (grep in `G`): every pmucal line range (555/569/586/596), pm-domains :77-83, mali dtsi
  :14-23/:40-47, cal-if.c :57-77/:106-116/:182-197, cmucal.h masks, dts :2072-2075 and :4865-4870, rae.c
  :102-116/:145-151/:356-357, ra.c :415-437 gate semantics, SPI→INTID (+32). plat_gpu_control.c range was
  off by 2 (117 → 119-161), fixed before the commit.
- Found: (1) F4 stated the `exynos_bts_scitoken_setting` pre-hook as unconditional — it runs only when
  `devfreq_index >= 0` (`exynos-pd.c:64-71`); (2) "clk*1000" asserted the KHZ constant without a citation.
  Both fixed in the follow-up commit.
