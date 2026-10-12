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

## Implementation (G9 UEFI half)
- 2026-10-12 code: `Beyond0LteGpuProbeDxe/GpuPower.c` (sequence), `GpuAcpm.c` (IPC client COPIED from the validated
  Beyond0LteTouchPowerDxe.c client: same FindChannel/CheckResponse/NextSeq/AcpmSend, prefix "GpuProbe: ACPM",
  MicroSecondDelay instead of gBS->Stall, elem size floor 16 B instead of 8 so the 4-word reply copy never reads
  past an element - ch 0 is elem 16 per m15 run 30). Constants: `Exynos9820.h` (SMC id/args + need_smc address,
  SOURCE-ONLY) and `GpuProbeHw.h` (sequence values, delays, ACPM idx/kHz). inf: + ArmSmcLib, TimerLib, BaseMemoryLib.
  Follow-up (not done here): move the duplicated ACPM client into one library used by both drivers.
- Entry flow: guard 0x6A (+dsb) -> as-found GpuProbe() -> if flag: GpuPowerOn(); if it powered the domain on ->
  GpuProbe() again -> restore guard (+dsb) -> "GpuProbe: done".
- GpuPowerOn (as of the review fix, see "Review of f378180"): EMBEDDED_G3D_STATUS bit0 = 1 -> "already ON - not
  touching"; = 0 -> steps 1-6 (outer G3D_STATUS only logged) (each write logs before -> after, each wait logs value + poll count or TIMEOUT),
  step 7 SMC (logs x0; low 32 bits != 0 -> REFUSED), step 9 ACPM FREQ_REQ (logs reply, "echo yes/NO" = reply word1
  == 260000; a FREQ_REQ reply echoed the request in m15 run 31), 10 ms settle. Step 8 (restore list) is absent by
  design (H5). Any wait timeout or a refused SMC -> PowerOffSafe (EMBEDDED_G3D_CONFIGURATION=0, wait, 138 us,
  [outer G3D_CONFIGURATION bit0=0 + wait, only if it was off at entry], PLL_CON0 bit4=0, PLL_CON2=0x10000001;
  NO CMU_G3D writes, NO tz_save SMC) and no ACPM request.
- MMIO map: nothing new needed (H7).

## Build (2026-10-12)
- Scratch copy `scratchpad\build-g9.ps1` of firmware/tools/build-uefi.ps1: `$ws` = this worktree's firmware,
  `$edk2` = main checkout's edk2, WORKSPACE = `subst V:` of `scratchpad\g9build` (Conf copied from g3build).
- Flag 0 (committed): **exit 0**, `FVMAIN_COMPACT [99%Full] 774144 (0xbd000) total, 772080 used, 2064 (0x810) free`,
  FVMAIN 33 %; FD 7 340 032 B sha256 `907f7729871897...`; GpuProbeDxe.efi 14 336 B (same as G3: LTO drops the
  power-on code when the flag is 0).
- Flag 1 (NOT committed; `#define BEYOND0LTE_GPU_POWER_ON 1`): **exit 0**, `FVMAIN_COMPACT [99%Full] 778240 (0xbe000)
  total, 774280 used, 3960 (0xf78) free` (auto-sized, grew one 4 KiB block), FVMAIN 34 %; FD 7 340 032 B sha256
  `7876c4d479cc49d9...`, copied to `scratchpad\BEYOND0LTE_UEFI-g9-flag1.fd` (not packed, not flashed);
  GpuProbeDxe.efi 31 232 B, all "GpuProbe: on[..]"/ACPM strings present.

## Expected UEFILOG (flag 1)
```
GpuProbe: start (BEYOND0LTE_GPU_POWER_ON 1)
GpuProbe: PMU EMBEDDED_G3D CONFIG .. STATUS .. OUT .. | G3D CONFIG .. STATUS .. (bit0 = on)     <- as found
GpuProbe: PLL_G3D CON0 .. (ENABLE . STABLE . M . P . S .) CON2 .. LOCKTIME ..
GpuProbe: G3D domain is OFF (...)            (or ON -> G3 probe lines, then "already ON - not touching it")
GpuProbe: power-on: G3D domain OFF at entry (EMBEDDED_G3D_STATUS .. G3D_STATUS ..) - running pmucal embedded_g3d_on
GpuProbe: on[1] PLL_CON2_PLL_G3D [1a240148] x -> 30000003
GpuProbe: on[2] PLL_CON0_PLL_G3D [1a240140] x -> x|10
GpuProbe: on[3] G3D_CONFIGURATION [15862380] .. -> ..;  on[3] wait G3D_STATUS [15862384] = .. ok after N polls
GpuProbe: on[4] EMBEDDED_G3D_OUT atomic-set 2 via [1586e120]: [15862120] .. -> ..
GpuProbe: on[5] G3D_CONFIGURATION ..;  on[5] wait G3D_STATUS .. ok
GpuProbe: on[6] EMBEDDED_G3D_CONFIGURATION [15862100] ..;  on[6] wait EMBEDDED_G3D_STATUS [15862104] .. ok
GpuProbe: on[7] SMC 82000410 (1, 18410204, 2) DTZPC restore ...
GpuProbe: on[7] SMC returned x0 0 (low32 0) -> ok
GpuProbe: power-on: PMU EMBEDDED_G3D_STATUS 1 G3D_STATUS 1 after DTZPC restore
GpuProbe: ACPM ch id 0 (idx 0) type 1 ap_poll 1 q_len 15 elem 16
GpuProbe: ACPM seq s sent 00s0000a 0003f7a0 00000000 00000000 -> reply ...
GpuProbe: on[9] ACPM FREQ_REQ G3D (idx 10) 260000 kHz -> reply .. 0003f7a0 .. .. (echo yes)
GpuProbe: power-on: done, domain left ON for the OS - re-probing
GpuProbe: PMU ... STATUS 1 ... G3D ... STATUS 1;  PLL_G3D CON0 .. (ENABLE 1 STABLE 1 M . P . S .)   <- lock
GpuProbe: G3D domain is ON - reading CMU_G3D; 9 CMU_G3D lines; GATE_GPU_CLK (MANUAL . CG_VAL .)
GpuProbe: GPU_ID [18500000] = 72?1.... (... G76 0x7001/0xF00F match: yes); 24 GPU lines; popcount line
GpuProbe: done
```
- Failure shapes: "on[n] wait ... TIMEOUT" or "SMC ... -> REFUSED" followed by "powering the G3D domain back off" +
  3-4 "on[off]" lines + "power-on FAILED"; ACPM failure -> "on[9] ... FAILED (..) - domain stays ON".
  A hang anywhere -> next boot "previous boot hung in the GPU probe (progress 0x6A) - skipping it".

## Hazards (for the boot)
- PMU writes (0x2380/0x2100 RMW, atomic-set 0x1586E120): plain MMIO per pmucal, PMU always on. 0x2380 bit0=0 at
  step 3 powers the OUTER G3D domain off first (vendor order) - harmless only if nothing else uses it (domain was OFF).
- PLL_G3D CON2/CON0 bit4 writes into CMU_TOP before the domain exists (vendor order); bit4 meaning UNVERIFIED.
- SMC 0x82000410 into Samsung's EL3 monitor: first ever from our firmware with this id. Unknown id -> -1 (REFUSED path).
  A monitor that faults/hangs on it is not recoverable by us -> 0x6A guard on the next boot.
- ACPM FREQ_REQ G3D: APM sets PLL_G3D AND the G3D buck voltage (INFERENCE, g2g3 F5). Same channel/path as the
  validated CPU requests; a wrong unit (Hz vs kHz, H3) would ask for 260 GHz/260 kHz - APM clamps to its table
  (INFERENCE) - the reply word and the re-probed PLL M/P/S show what happened.
- Leaving G3D on for Windows with no driver: idle power/heat; no thermal control (G9 row: stay at 260 MHz).
- After the domain is on, the re-probe's first NS reads of CMU_G3D/GPU depend on the DTZPC restore having worked.

## Review of f378180 (G9 UEFI half)
- Checked mechanically: 9810 tree untouched (`git diff --stat 53e3587..HEAD -- firmware/Platform/Star2LtePkg
  firmware/Silicon/Exynos9810Pkg` empty); every new constant has exactly one definition (rg); 0x3F7A0 = 260000;
  `cal_acpm_dvfs_exynos9820.h:13` = dvfs_g3d; `exynos-smc.c:113` = `int32_t ret`; flag-0 .efi is byte-size identical
  to G3's (14 336 B) and flag-1 .efi carries every new log string (tr/grep over the .efi); no new MMIO window needed.
- Found:
  1. **Entry decision too strict.** GpuPowerOn refused ("inconsistent") when EMBEDDED_G3D_STATUS = 0 but the outer
     G3D_STATUS = 1. The vendor decides on 0x2104 alone: gpexbe_pm_pd_control (`S\gpex_gpexbe_pm.c:96-125`) ->
     cal_pd_status -> embedded_g3d_status (= PMU+0x2104 bit0 only, `S\cal_flexpmu_cal_local_exynos9820.h:596-598`),
     and the on-sequence itself switches the outer domain off first (step 3). An "outer on / embedded off" sboot
     state would have cost a boot for nothing. Fix: OFF = 0x2104 bit0 == 0 -> run the sequence; ON = 0x2104 bit0
     == 1 -> not touched (0x2384 logged either way).
  2. **Failure path changes state it did not own.** PowerOffSafe left the outer G3D on (step 5 turned it on) even
     when it was off at entry. Fix: restore the outer domain to its entry state (if it was off: the vendor's own
     step-3 operation, G3D_CONFIGURATION bit0 = 0 + wait G3D_STATUS 0).
  3. Dead constant `CMU_G3D_QCH_CON_BUSIF_HPMG3D` (defined, never used). Removed; the skipped write stays
     documented in PowerOffSafe's comment.
  4. MILESTONES G9 row and this note said "only if both PMU status bits are 0" - follows fix 1.
  All four fixed in the follow-up commit (not folded into f378180).
- Rebuild after the review fix: flag 0 **exit 0**, FVMAIN_COMPACT 774144 total / 2032 (0x7f0) free, FD sha256
  `830941a125443c4f...`, GpuProbeDxe.efi 14 336 B; flag 1 (not committed) **exit 0**, FVMAIN_COMPACT 778240 total /
  3952 (0xf70) free, FD 7 340 032 B sha256 `2eb3767214e41001...` -> `scratchpad\BEYOND0LTE_UEFI-g9-flag1.fd`
  (replaces the pre-fix copy), .efi 31 232 B. Not packed, not flashed.

## Review of 1343212 (review fix)
- Checked: `git show 1343212` - entry test now `EmbSt & 1` only; OuterWasOn captured before any write and passed on
  the single `Fail:` path; QCH constant gone (rg: 0 hits); flag back to 0 in the staged diff; both builds exit 0.
- Found: PowerOffSafe(OuterWasOn = TRUE) logged "outer G3D back to on" but did nothing - if the outer domain was on
  at entry and a wait failed between step 3 (outer off) and step 5 (outer on), it stayed OFF, contradicting the
  log and the commit message. Fixed in the follow-up commit: re-enable it with the vendor's step-5 operation when
  it was on at entry and reads off now.
- Rebuild after this fix (the FINAL numbers): flag 0 (committed) **exit 0**, FVMAIN_COMPACT 774144 total / 2024
  (0x7e8) free, GpuProbeDxe.efi 14 336 B; flag 1 (not committed) **exit 0**, FVMAIN_COMPACT 778240 total / 3928
  (0xf58) free, FD 7 340 032 B sha256 `13b5a67e54bba0bf...` -> `scratchpad\BEYOND0LTE_UEFI-g9-flag1.fd`, .efi 31 232 B.
- Next (main session): set `BEYOND0LTE_GPU_POWER_ON 1` for the test image, boot, read the "GpuProbe:" lines
  (expected shape above); then the ACPI half of G9 (Device GPU0). Follow-up task: one shared ACPM IPC library for
  TouchPowerDxe + GpuProbeDxe (today a verbatim copy).
