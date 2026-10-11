# M15 - performance: clocks, storage, display (2026-10-11)

User (boot #58, 6 cores + 5.2 GB): "everything is still kinda slow tbh maybe because of no gpu?" ... "any chance
you can get some basic graphics acceleration going? ... switch the screen orientation to landscape ... dynamic with
a tool from within windows".

## GPU verdict (drivers branch notes/d5-gpu-display.md, the user's research agent)
No Mali-G76 WDDM exists anywhere; options are a display-only WDDM (DECON) or a multi-person-month panfrost port.
Shipped = GOP framebuffer + inbox BasicDisplay + WARP (CPU rendering). => speed has to come from elsewhere first.

## Suspects for "slow", to MEASURE before changing anything
1. CPU clocks: sboot may leave the clusters at a boot frequency; Linux raises them via ACPM DVFS (channel 5).
   drivers branch notes/cpu-driver-9820.md F2/F3/F9/F11: domains idx 0 MIF, 2 LITTLE (442 MHz-1.95 GHz), 3 MID
   (-2.4 GHz), 4 BIG (-2.08 GHz build cap); FREQ_REQ 0 / FREQ_GET 1, reply cmd[1] = kHz.
2. Storage: Exynos9820Ufs is queue depth 1 and bounces every transfer.

## Progress log
- Boot #59 (built): Beyond0LteTouchPowerDxe also does a READ-ONLY FREQ_GET for idx 0/2/3/4 on ACPM channel 5
  (live DT acpm_dvfs = 5) and logs "TouchPower: DVFS <name> (idx n) = <kHz>". FVMAIN_COMPACT auto-sized.
- s10erotate.exe (drivers/S10eDiag/S10eRotate.c): ChangeDisplaySettingsExW orientation 0/90/180/270/cycle/query,
  logs to %TEMP%\s10erotate.log; desktop launchers "Rotate landscape/portrait/next". Whether BasicDisplay allows
  rotation is unverified - the exit code says (DISP_CHANGE_BADMODE = -2 if not).
- extra-perf.cmd: winsat disk seq/ran read, mem, cpu compression -> out\last\perf.
- Run 21 (boot #59, 15:00, back 198 s, collect + flushreset return clean): serialized parking "mark after 1 ms"
  for all 5 cores. DVFS read did NOT run: ACPM ch id 5 has type 2 (TYPE_BUFFER), ap_poll 0, q_len 1 - the client
  refuses non-queue channels ("layout implausible"). With q_len 1 the kernel's own queue-full test
  (tmp_index != rear) can never pass, so ch5 must be driven through a path I have not traced - left to the
  user's CPU-research branch (drivers: notes/cpu-driver-9820.md) instead of guessing. winsat -> banner only as
  SYSTEM. Next: s10eperf.exe (own cpu/mem/disk numbers), run 22.
