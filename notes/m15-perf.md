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
- Run 24 (re-run, 15:15, back 244 s): booted fine (0xED did not repeat). UfsDiag.exe perf counters (w32):
  READS 149444 (1.5 GB), WRITES 43657 (343 MB); PER_READ_US DEVICE 131, ZERO 165, COPYBACK 286 => the uncached
  bounce copies cost 3.7x the device time (PERF_COPY_PCT_OF_DEVICE=372). Bounce = MmNonCached = Device memory
  here (each access a bus transaction). MAX_TRANSFER 64 KiB, QD1, polling (17 s of 10 us polls over 193k cmds).
  Write verify is compiled out in the full-Windows build (counter only) - no double writes.
  disk: seq write 16.5 MB/s, seq read 11.2, random 4K 1422 IOPS.
- Driver w33 (s10e-artifacts\ufs-w23\Exynos9820Ufs-w33.sys, sha256 d709738e...): UFS_WIDE_UNCACHED_ACCESS 1
  (8-byte bounce copies), new UFS_ZERO_BOUNCE_BEFORE_READ 0 (skip the 165 us pre-read zero). Swapped into
  DriverStore\FileRepository\exynos9820ufs.inf_arm64_4efdc4525d11d997 (only copy; ImagePath points there) after
  fsck.ntfs (dirty from the hard-reset return). Rollback = w32 (e378cee0...). Run 25 measures.
- __sys / _ReadStatusReg encodings VERIFIED by dumpbin on a test object (%TEMP%\dctest): SYSINSTR(op1,Cn,Cm,op2)
  = (op1<<11)|(Cn<<7)|(Cm<<3)|op2 -> (3,7,14,1) "dc CIVAC", (3,7,10,1) "dc CVAC", (0,7,6,1) "dc IVAC";
  ARM64_SYSREG(3,3,0,0,1) -> "mrs CTR_EL0".
- Driver w34 (Exynos9820Ufs-w34.sys, sha256 8ea93ce8...): UFS_CACHED_BOUNCE_READ 1 - a cacheable MmMapIoSpaceEx
  view of the bounce (firmware-reserved pool only), read copy-back = DSB, DC CIVAC over the range (line from
  CTR_EL0), DSB, RtlCopyMemory; writes/commands keep the Device-memory view. dumpbin of the .sys shows
  "dc CIVAC,x8" and "mrs x8,CTR_EL0". Early mark "=CACHED BOUNCE line=" records line size + mapping success.
- Run 25 (w33, 15:21, back 208 s): disk seq write 26.2 MB/s (was 16.5), seq read 20.1 (11.2), random 4K 2314
  IOPS (1422); PER_READ_US DEVICE 128, ZERO 0, COPYBACK 126 (was 286); desktop at 84 s (was ~104). Next: w34.
- Run 26 (15:25): the w34 swap FAILED silently - /tmp/win missing in TWRP (tmpfs cleared), ntfs mount failed,
  adb push landed in TWRP's tmpfs. Run 26 = w33 again (a repeat measurement). Swap procedure must mkdir the
  mount point and refuse to push unless the rw mount is confirmed.
- Run 27 (w34 cached read copy-back, 15:28, back 198 s): boots/runs normally (reads correct). PER_READ_US DEVICE
  124, COPYBACK 39 (w33 126); PERF_COPY_PCT_OF_DEVICE 44 (112); random 4K 2924 IOPS, seq read 23.1 MB/s, seq
  write 25.0 MB/s; desktop at 78 s. => copies no longer dominate; DEVICE time does: ~2.8 ms per 64 KiB read ->
  suspect the UFS link power mode (gear/lanes) left slow by sboot/UEFI. Next: read PA_PWRMODE / gear / lanes.
- Run 28 (boot #60): UEFILOG "UfsHc: link after PMC: PWRMode 0x11 UPMS 1 HCS 0x10F RxGear 3 TxGear 3 RxLanes 2
  TxLanes 2 HSSeries 2" = HS-G3 x2 Rate B - the link is at full speed (not the bottleneck). AHIT already 0 (UEFI).
  Disk: seq read 22.8 MB/s, write 25.8, random 2829 IOPS. Counter math: DEVICE total 23.7 s / ~238k commands =
  ~100 us each, yet the 1 MiB file reads take ~2.7 ms per 64 KiB -> most time is outside device+copy. Completion
  is synchronous in StartIo; poll timing accurate (11.5 us per 10 us stall). Suspect the benchmark file is
  fragmented (NTFS issuing small requests; boot-wide average read is 8 KB) -> s10eperf raw \.\C: (read-only,
  4 KiB / 64 KiB / 1 MiB requests from offset 0) in run 29.
- Run 29 raw \.\C: (read-only): 4 KiB 561 us/req (7.3 MB/s), 64 KiB 4186 us (15.7 MB/s), 1 MiB 53 ms (19.7
  MB/s). Counters: ~6144 x 64 KiB commands in the test account for ~25 s of the boot's 29.6 s DEVICE time => a
  64 KiB command really spends ~4 ms in the controller/device (~15 MB/s, ~64 ns/byte) although the link is
  HS-G3 x2 Rate B. Not the driver (copy 40 us/read, completion synchronous), not fragmentation.
  Leading suspect: bus clocks left at boot level (FSYS/INT/MIF - Linux raises INT/MIF via ACPM DVFS + pm_qos
  while UFS is busy); others: HCI AXI burst config (AXIDMA 0x98000003), FMP inline-crypto path. Same lever as
  the CPU clocks (~1.05/1.22 GHz) => ACPM DVFS (channel 5 is TYPE_BUFFER; protocol = drivers-branch research).
- Summary of m15 so far: w33+w34 = disk ~2x (seq read 11 -> ~23-25 MB/s, random 1422 -> ~2850 IOPS), desktop
  ~104 s -> ~78 s. Next big lever: DVFS (CPU + bus clocks).
- DVFS channel traced (LineageOS 9820 kernel): drivers/soc/samsung/cal-if/acpm_dvfs.c exynos_acpm_get/set_rate send
  on acpm_dvfs.ch_num, set by exynos_acpm_set_device(np) from cal_if_init(np) in clk-exynos9820.c
  (CLK_OF_DECLARE "samsung,exynos9820-clock"). The samsung,exynos-acpm-dvfs node's channel (5) is only the MIF
  notification channel (TYPE_BUFFER, ACPM -> AP). Live DT: clock-controller@0x1a240000 acpm-ipc-channel = 0
  (VERIFIED, TWRP). Boot #61: read-only FREQ_GET on channel 0 for MIF/INT/LITTLE/MID/BIG.
- Run 30 (boot #61): ACPM ch 0 = type 1 queue, q_len 15, elem 16, ap_poll 1; FREQ_GET replies with matching seq
  but 0 kHz for MIF/INT/LITTLE/MID/BIG (nothing has requested a rate yet - Linux cpufreq goes straight to
  FREQ_REQ). Boot #62: FREQ_REQ LITTLE (idx 2) = 1 456 000 kHz (exact DT ladder step), behind a progress-0x66
  self-healing guard (a boot after a hang there skips it), 10 ms settle, then FREQ_GET readback.
- Run 31 (boot #62): FREQ_REQ LITTLE 1456000 -> reply echoes 0x163780; Windows s10eperf: cpu0-3 ~1.45 GHz (was
  ~1.05), cpu4-5 1.22 GHz unchanged; collect started at 66 s (was 78). => ACPM DVFS from UEFI WORKS and persists
  into Windows. FREQ_GET still returns 0 (not the current rate on this firmware).
  Exact ladders (TWRP kernel sysfs, same SoC tables): cpu0 1950000 1846000 1742000 1586000 1456000 1300000
  1157000 1053000 949000 806000 650000 546000 442000; cpu4 2314000 2210000 2106000 2002000 1898000 1794000
  1690000 1586000 1508000 1404000 1222000 1066000 962000 845000 754000 650000 507000 (1.22 GHz measured = 1222000
  step); devfreq_mif 421000..2093000 (cur 2093000 in TWRP), devfreq_int 100000 200000 267000 400000 534000.
  Boot #63: LITTLE 1456000, MID 1794000, INT 400000 (MIF untouched).
