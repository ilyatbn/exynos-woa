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
- Run 22 (s10eperf, 15:06, back 244 s): cpu0-3 (A55) ~1.02-1.05 GHz, cpu4-5 (A75) ~1.22 GHz (dependent XOR/ADD
  chain - an estimate; DT ceilings 1.95 / 2.4 GHz) => CPUs at roughly half speed. memcpy 3330 MB/s (fine).
  DISK: seq write 15.5 MB/s, seq read 11.2 MB/s, random 4K 1172 IOPS (QD1) - UFS 2.1 does hundreds of MB/s =>
  storage is the dominant bottleneck. Suspect: the bounce buffer is mapped MmNonCached (UfsMapFirmwareReserved-
  Workspace) and UFS_WIDE_UNCACHED_ACCESS=0 copies it with 4-byte volatile accesses; max transfer 64 KiB.
  Run 23 adds UfsDiag.exe (perf counters: device vs zero-fill vs bounce in/out ticks) to prove the split first.
- Run 23 (15:12, back 111 s): Windows bugchecked EARLY (before S10eDiag): =UFSBUGCHECK BC=0xED
  UNMOUNTABLE_BOOT_VOLUME P1 ffffb701573f8060 P2 C0000010 (STATUS_INVALID_DEVICE_REQUEST); boot pair -> TWRP.
  PRAM ring held run 22's lines with bit-decay. fsck.ntfs -n afterwards: $MFT/$MFTMirr OK, ro mount OK.
  Re-run unchanged (run 24) to see whether it repeats; suspects if it does: NTFS state after many flushreset
  returns + installer ntfsfix journal resets; an early UFS driver failure.
