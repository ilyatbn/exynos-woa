# M16 G9 (UEFI half) - power the G3D domain on, request 260 MHz via ACPM, leave it on for Windows

Scope: `Beyond0LteGpuProbeDxe` gets a real power-on path behind `BEYOND0LTE_GPU_POWER_ON` (default **0** in
the committed code; the main session flips it for the test image). No device access in this session (no adb,
no pack, no flash) - build only. Sources: LineageOS android_kernel_samsung_exynos9820 lineage-22.2, the same
files fetched for G2 (`notes/gpu-m16-g2g3.md`), copied into
`scratchpad\g3don` (`C:\Users\ilya\AppData\Local\Temp\claude\C--Users-ilya-code-exynos-woa\b82d8ce8-...\scratchpad\g3don`,
`S` below; ACPM IPC sources in `%TEMP%\acpm-src`, notes/m12-acpm.md).

## Progress log

- 2026-10-12 start: worktree fast-forwarded a8d8a62 -> main 53e3587 (it was behind; GpuProbeDxe only exists on main).
  Read notes/gpu-m16-g2g3.md (F1-F7, G3 design/build), m12-acpm.md, m15-perf.md, d8-audio-m3-abox.md F5,
  Beyond0LteGpuProbeDxe.c/.inf/GpuProbeHw.h, Beyond0LteTouchPowerDxe.c (ACPM client), Beyond0LteParking.c (SMC).
- 2026-10-12 research H1-H7 below (all re-checked by grep in `S`).

## Findings

### H1 ACPM channel for DVFS is **0**, not 5 (the task text and MILESTONES G2 row say 5)
- `S\exynos9820.dts:942-945` clock-controller@1a240000 (`samsung,exynos9820-clock`) `acpm-ipc-channel = <0>`;
  `:2072-2075` acpm_dvfs `acpm-ipc-channel = <5>`. notes/m15-perf.md (2026-10-11): cal_if_init(np) ->
  exynos_acpm_set_device -> `acpm_dvfs.ch_num` = the **clock-controller's** channel; ch 5 is the MIF-notification
  channel (TYPE_BUFFER, q_len 1 - TouchPower refused it as "layout implausible", run 21).
  Live DT clock-controller acpm-ipc-channel = 0 **VERIFIED** (TWRP, m15); FREQ_REQ on ch 0 **device-validated**
  (runs 31/32: LITTLE/MID/INT replies echo the request and Windows measures the new CPU clocks).
  => G3D FREQ_REQ goes on channel 0 through the same client code (`ACPM_DVFS_IPC_CHANNEL 0u`,
  Beyond0LteTouchPowerDxe.c:123). The "ch 5" in MILESTONES G2 is wrong - corrected in the G9 row text.

### H2 Command layout and dfs index
- `S\calif_acpm_dvfs.c:23-47` exynos_acpm_set_rate: cmd[0] = id, cmd[1] = (u32)rate, cmd[2] = FREQ_REQ,
  cmd[3] = 0, response = true; sent with acpm_ipc_send_data_lazy (`%TEMP%\acpm-src\..._acpm_ipc.c:639-649` =
  the same __acpm_ipc_send_data, only the wait style differs).
- `S\calif_acpm_dvfs.h:19` FREQ_REQ 0.
- `S\calif_cal-if.c:57-77` cal_dfs_set_rate: IS_ACPM_VCLK(id) -> exynos_acpm_set_rate(GET_IDX(id), rate)
  (the hiu branch is CPU-only). `S\calif_cmucal.h:37-39,58,214,217`: GET_IDX = id & 0xFFFF.
- `S\dtb_clock_exynos9820.h:507` ACPM_DVFS_G3D = 0x0B04000A -> **idx 10** = `dvfs_g3d` (11th entry of
  `enum acpm_dvfs_id`, `S\cal_acpm_dvfs_exynos9820.h:2-18`, dvfs_mif = ACPM_VCLK_TYPE = idx 0).

### H3 Rate unit: **kHz, 260000** - by rate, not by level
- The request is by **rate** (cmd[1]); there is no level index in the protocol. APM maps it to its own table.
- Shipped glue (bv_r32p1 + gpex, F7 of g2g3): `S\gpex_gpexbe_clock.c:75-78` gpexbe_clock_set_rate(clk) =
  cal_dfs_set_rate(cal_id, **clk**) - no scaling; `S\gpex_gpex_clock.c:109-121` builds the clock table by
  comparing the CAL/ACPM ASV-table rate **equal** to the DT gpu_dvfs_table clock (`S\exynos9820-mali_common.dtsi:29-37`,
  kHz: 702000..156000) -> the ACPM rate unit is **kHz**. 260000 is a DT table row (:35) and
  `gpu_dvfs_start_clock = <260000>` (:45).
- Contradiction recorded: the older b_r10p0 glue multiplies by KHZ (`S\plat_gpu_control.c:121`
  `g3d_rate = clk * KHZ`, KHZ 1000 `S\plat_mali_kbase_platform.h:45`) - that glue is NOT built for beyond0lte
  (defconfig selects R32P1, g2g3 F7). The CPU FREQ_REQs that work on this phone are in kHz (m15 run 31: 1456000).
  => cmd = {10, 260000, 0, 0}. G2 F4's "clk*1000" describes the unshipped glue only.

### H4 SMC (DTZPC restore)
- `S\calif_pmucal_local.c:37-52`: after the on-sequence, if need_smc: exynos_pd_tz_restore(need_smc); a non-zero
  return is "DTZPC restore smc error" and pmucal_local_enable returns the error (the domain is left ON by the
  kernel - it does not power it back off; our "refused -> power off" is stricter, per task).
- `S\exynos-el3_mon.c:239-245` = exynos_smc(SMC_CMD_PREAPRE_PD_ONOFF, EXYNOS_WAKEUP_PD_DOWN, addr,
  RUNTIME_PM_TZPC_GROUP); `S\smc_h.h:42` 0x82000410; `S\inc_exynos-el3_mon.h:24,26` WAKEUP_PD_DOWN 1, TZPC_GROUP 2.
  `S\exynos9820-pm-domains.dtsi:77-83` need_smc = 0x18410204.
  exynos_smc (`scratchpad\abox\exynos-smc.c:111-122`) passes x0..x3 and returns x0 -> ArmCallSmc with Arg0..3,
  result Arg0 (same pattern as Beyond0LteParking.c PSCI CPU_ON). exynos_smc keeps the result in an `int32_t`
  (`exynos-smc.c:113`) -> success test = low 32 bits == 0; SMCCC "unknown function" = -1 = 0xFFFFFFFF.
- Disable path (`S\calif_pmucal_local.c:77-104`) does exynos_pd_tz_save (:99; arg1 = EXYNOS_GET_IN_PD_DOWN 0,
  `S\inc_exynos-el3_mon.h:23`) before the off sequence. After a refused restore we do NOT issue the save SMC (nothing was restored;
  a second secure call into a monitor that just refused is the riskier choice) - documented deviation.

### H5 Restore list after the SMC: nothing to do on a first power-on
- `S\calif_pmucal_local.c:54-55` pmucal_rae_restore_seq(save list); `scratchpad\abox\calif_pmucal_rae.c:237-256`
  writes a SAVE_RESTORE entry only if `need_restore` (set by a prior save). First power-on since reset (our
  case: sboot left it off, or we would not touch it) -> nothing restored. Skipped, by construction.

### H6 What the kernel does that we skip
- `bts_pd_sync` (`S\calif_cal-if.c:192-197`, BTS QoS) - not done (no BTS driver in UEFI).
- exynos-pd pre-hook `exynos_bts_scitoken_setting` only for pds with a devfreq sync index (`S\exynos-pd.c:64-71`).
- `SMC_DRM_G3D_POWER_ON` (gpex, only with CONFIG_EXYNOS_CONTENT_PATH_PROTECTION, g2g3 F7) - not done.
- `gpex_pm` sets the clock only while gpexbe_pm_get_status says on (`S\gpex_gpex_clock.c:135-140`) -> our
  FREQ_REQ runs only after EMBEDDED_G3D_STATUS bit0 reads 1.

### H7 Map / timing / guard
- Every touched MMIO window is already mapped: PMU 0x15860000+0x10000 covers the atomic-set alias 0x1586E120;
  CMU_TOP, CMU_G3D, G3D (G3), ACPM_MBOX + ACPM_SRAM (m12). 0x18410204 is only an SMC argument (never accessed).
  => no new mMmioRegions entry (boot #95 lesson checked, nothing missing).
- Delays: GpuProbeDxe's depex is only gEfiCpuArchProtocolGuid -> gBS->Stall may not have a Metronome yet;
  use TimerLib MicroSecondDelay (as Beyond0LteParking.c). The copied ACPM client is switched to it too.
- Guard: the existing 0x6A word (mProgress[1]) is set before step 1 of the power-on and restored only after
  the re-probe -> a hang anywhere in power-on/SMC/ACPM/re-probe makes the next boot skip the whole driver.
