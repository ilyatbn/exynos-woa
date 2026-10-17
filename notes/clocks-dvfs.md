# Dynamic CPU / GPU clocks (DVFS) + UI priority under Windows

User directive 2026-10-17 (memory clocks-directive-20261017): after alpha6 - dynamic CPU and GPU clocks (minimum with
the screen off and when idle, spin up on touch so the UI is smooth), find the highest usable clocks assuming the two
big (cheetah/M4) cores never work, same for the GPU, and a scheme where the UI gets real-time performance while idle
and background work gets lower priority. Standing rule (memory thermals-before-clocks): no clock INCREASE before
Windows reads temperatures and can throttle; lowering is fine.

## Facts already established (do not re-derive)
- notes/cpu-driver-9820.md F2/F9/F11: CPU clusters are driven by ACPM IPC (mailbox 0x15900000+0x1000, SRAM
  0x2039000+0x30000, framework at SRAM+0x7000), DVFS domain idx 2 LITTLE (A55 x4, 442 MHz..1.95 GHz, 13 OPPs),
  3 MID (A75 x2, 507/650 MHz..2.4 GHz, 17 OPPs; 2.4/2.314 GHz need MIF >= 2.093 GHz), 4 BIG (M4 x2, not running
  under Windows). Message u32[4] {idx | seq<<16, kHz, opcode FREQ_REQ 0 / FREQ_GET 1, 0}, doorbell INTGR0
  (1<<ch)<<16, polling INTSR1, 15 ms timeout; a mis-paired/unanswered request makes the kernel emergency-reset - the
  ACPM is load-bearing. Verified in the shipped kernel binary (F11).
- notes/m15-perf.md runs 31/32: UEFI (Beyond0LteTouchPowerDxe / GpuAcpm.c) already sends FREQ_REQ - today's fixed
  clocks: LITTLE 1456 MHz, MID 1794 MHz (s10eperf measures ~1.45 / ~1.79 GHz), GPU 260 MHz (ACPM ch 0 id 10,
  table 702..156 MHz, notes/thermal-research.md Q5). Windows itself has no ACPM client and no DVFS.
- notes/thermal-research.md: temperatures = ACPM ch 9 READ_TEMP (tz 0..4 BIG/MID/LITTLE/G3D/ISP, reply w1[31:24]
  deg C); the TMU hardware trip (115 C) is probably NOT armed under Windows -> the phone is unprotected; stage plan
  0-3 (UEFI read-only check, Windows logging, ACPI thermal zones, passive cooling). DT trips: CPU control 83 C, GPU
  88 C, alerts 95-110 C, hot 115 C.
- Idle states: PSCI standby per class (F4). Energy data (F5) ranks LITTLE < MID < BIG.

## Design
1. **S10ePower KMDF driver** (one owner of the ACPM mailbox under Windows - serialized, polling mode, never leaves a
   request unanswered): FREQ_REQ/FREQ_GET for idx 2/3 (CPU) and the GPU domain, READ_TEMP ch 9 for tz 1..3. Reads
   the per-SKU OPP ladder from ACPM SRAM fvmap (F9 point 3) instead of hard-coding. IOCTL interface + registry stats.
   Thermal guard inside the driver: any request above a temperature-dependent cap is clamped (CPU 83 C / GPU 88 C
   control temps step clocks down; >= 95 C forces minimum). This makes clock increases allowed under the rule.
2. **Governor** (user-mode service S10eGovernor, SYSTEM): inputs - display state (power setting notification
   GUID_CONSOLE_DISPLAY_STATE + our panel escape state), per-cluster CPU load (NtQuerySystemInformation
   SystemProcessorPerformanceInformation, 20-50 ms), GPU busy (KMD stats escape MW_ESCAPE_STATS), touch activity
   (touch driver interrupt count / an event the touch driver sets). Policy: screen off -> all minimum; idle ->
   minimum; touch -> immediate boost of MID + GPU for N ms (input boost, like Android's touchboost); load-tracking
   in between (schedutil-like: next freq = 1.25 * util * max). Hysteresis to avoid thrash; rate-limit ACPM calls.
3. **UI priority (native Windows mechanism)**: the Windows 11 scheduler places high-QoS (foreground/UI) threads on
   higher-performance cores and EcoQoS/background threads on efficient cores when the firmware describes
   heterogeneity - ACPI MADT GICC EfficiencyClass (LITTLE 0, MID 1) + PPTT. Check what our MADT says today; set the
   classes (firmware/ACPI change, boot path!) so foreground work lands on the A75s and background on the A55s. Then
   the governor can keep LITTLE low and MID fast when the user interacts. (Research MS docs: "heterogeneous
   processors / efficiency class" before changing.)
4. **Max-clock tests** (after 1's thermal guard): step LITTLE to 1.95 GHz, MID to 2.4 GHz (needs MIF), GPU above
   260 MHz (DT ladder 156..702), each under a sustained load (s10eperf cpu, cube/gfxperf GPU) while logging temps;
   stop at the first instability or when the control temperature is reached within N minutes; record sustained vs
   burst limits.

## Tasks (one commit each)
- C0 Stage-0 style read-only check from Windows: ACPM framework + ch 9 READ_TEMP once (S10ePower skeleton, read-only),
  temperatures in the registry. Validates the channel layout under Windows before any write.
- C1 FREQ_GET for idx 2/3/GPU, fvmap ladder read (still read-only).
- C2 FREQ_REQ (down only first: screen-off minimum test), thermal guard.
- C3 governor service (display state, idle, touch boost, load tracking).
- C4 MADT efficiency classes (research first; firmware change; boot hazard -> rollback plan).
- C5 max-clock tests with temperature logging; set the governor's ceilings from the results.

## Progress log
- 2026-10-17 05:15 plan written (phone busy finishing a Windows update restart).
- 05:20 C4 is mostly in place: firmware/Platform/Beyond0LtePkg/AcpiTables/Madt.aslc already sets ProcessorPowerEfficiencyClass LITTLE 0 / MID 1 / BIG 2 (fixed 2026-10-11, notes/m14-smp.md: the 9810 table was inverted). To verify on the device: GetSystemCpuSetInformation EfficiencyClass per CPU; then the governor keeps MID fast during interaction.
- 05:35 C0 code: drivers/S10ePower (legacy NT driver, build.bat), READ-ONLY: maps ACPM SRAM (PAGE_READONLY), records ipc_channels offset, ap_max and the raw channel table (0x48-byte entries) in the service key. Built (s10e-artifacts\s10epower). Not loaded yet (phone still finishing the update restart). Load: sc create S10ePower type= kernel start= demand binPath= C:\S10eTmp\S10ePower.sys; sc start S10ePower.
- 05:55 S10ePower full code (built, not run): C0 dump always; AllowSend=1 (service key) maps the mailbox, finds ch 0 (DVFS) + ch 9 (TMU, only with AllowTmu=1), guard thread 1 s READ_TEMP tz 1/2/3 -> caps (level 0 ladder max, 1 >=83 C boot rates + G3D 325, 2 >=95 C LITTLE 1053/MID 1222/G3D 260, 3 >=105 C minimum; 5 C hysteresis; GPU trips +5 C); no temperature -> cap = UEFI boot rates (1456/1794/260). SET_FREQ IOCTL rounds down to the DT ladders (LITTLE/MID from TWRP sysfs, G3D from exynos9820-mali_common.dtsi: 702/650/572/433/377/325/260/200/156). Device ACL SYSTEM+admins. s10epowerctl state|set. Note: G3D >= 650 and MID 2314 want MIF >= 2.093 GHz (DT tables) - MIF untouched under Windows, its rate UNKNOWN.
- 06:15 C3 prep: touch driver 6.0.5058.7 (sys 408B530D, built, not installed) sets named auto-reset event \BaseNamedObjects\S10eTouchActivity (IoCreateSynchronizationEvent at prepare-hardware, ZwClose at release) from its PASSIVE_LEVEL ISR at most every 20 ms - the governor's touch-boost input.
- 06:35 C3 code: drivers/S10ePower/s10egovernor.c (service S10eGovernor / 'console' test mode): display state (GUID_CONSOLE_DISPLAY_STATE service notification) + MaliG76Wddm PanelDisplayPower trace -> screen off = all minimum; S10eTouchActivity -> boost for BoostMs (300); else per-cluster load tracking every 50 ms (1.25 x busiest core x ceiling, up at once, down after DownSamples=6); GPU keeps its ceiling with the screen on, half-ceiling step after 5 s without touch + idle CPUs. Ceilings default = UEFI boot rates (governor only lowers until C5). Built, not run.
- 05:20 C0 DONE (device): S10ePower loaded read-only (sc create ... start= demand; AllowSend absent): AcpmStatus 0,
  ipc_channels at SRAM+0x5f0c, ap_max 11. Decoded (tools/phone/power/acpmtable.py): id 0 queue q_len 15 elem 16 ap_poll
  1 (= m15 run 30 from UEFI), id 1 q3, id 2 q5, id 3/5/6/7/8 buffer channels, id 4 q3, **id 9 queue q_len 7 elem 16
  ap_poll 1, owner 64** (the TMU plugin - consistent with stock DT acpm_tmu = 9), id 10 q3. Channel 9 layout verified
  live; tzid mapping still from the DT. Next: C1 (AllowSend=1, AllowTmu=1: READ_TEMP every second) after the alpha6
  soak.
- 05:30 C4 check (device, drivers/S10ePower/cpusets.c): GetSystemCpuSetInformation: cpu0-3 EfficiencyClass 0, cpu4-5 EfficiencyClass 1, cpu5 Parked 1 at idle (core parking active), each core its own LLC index. => Windows' heterogeneous scheduling is on (UI/high-QoS threads prefer the A75s). Remaining native levers: power-plan processor settings (heterogeneous thread scheduling policy / short-running policy, core parking min cores / unpark speed) - research MS docs before changing.
- 05:40 C4 conclusion: Balanced plan SCHEDPOLICY = SHORTSCHEDPOLICY = 5 (Automatic: MS Learn configuration-for-hetero-power-scheduling-schedulingpolicy 'the OS determines the policy based on system configuration and QoS type'), CPMINCORES 10 %. With MADT efficiency classes live (cpusets), Windows 11's QoS scheduling already puts high-QoS (foreground/UI) threads on the A75s and EcoQoS/background on the A55s - the native mechanism for 'UI real-time, background lower priority'. No setting changed. The governor's job: keep the A75s fast during interaction (touch boost) and let the A55s idle low.
- 05:40 C1 DONE (device): AllowSend=1 + AllowTmu=1, sc stop/start S10ePower -> DvfsChannelOk 1, TmuChannelOk 1, READ_TEMP replies: MID 47 C, LITTLE 47 C, G3D 46 C at idle (registry 0x33/0x32/0x2e on the first log line), 12 sends, 0 fails, 0 timeouts, guard level 0 (caps = ladder max, nothing requested). First temperatures read by Windows on this phone. Battery 7 % -> parking in TWRP to charge (unattended rule) before C2.
- 05:45 governor: GPU load from Windows' own GPU Engine counters (PDH \GPU Engine(*engtype_3D)\Utilization Percentage, busiest instance, every 200 ms): target 1.25 x busy x ceiling, floor half the ceiling with the screen on; fallback to the old rule if the counter is unavailable. Built.
- 06:30 C2 DONE (device, traces/wddm2-2026-10-16/c2-down.txt): Windows FREQ_REQ via S10ePower: LITTLE 442000 -> s10eperf
  cpu0-3 ~0.43-0.44 GHz, MID 650000 -> cpu4-5 ~0.64-0.65 GHz; restore 1456000/1794000 -> ~1.45/~1.79 GHz. 61 sends,
  0 fails, 0 timeouts, temps 54-61 C. Before any request this boot the cores measured ~1.71-1.74 GHz (LITTLE) /
  ~2.10 GHz (MID) = the 1742000/2106000 steps, NOT the UEFI 1456000/1794000 (Beyond0LteTouchPowerDxe
  BEYOND0LTE_DVFS_*_KHZ) - likely the UEFI request was skipped by its progress-0x66 self-healing guard on this boot
  path (after the TWRP reset); where 1742/2106 come from (sboot / ACPM init level) is UNVERIFIED. Temperature at
  those rates after boot: 58 C, 54 C after lowering. Windows now owns the CPU clocks.
- 06:50 C3 first run (device): touch driver 6.0.5058.7 installed (pnputil, active - Win32_PnPSignedDriver), S10eTouchActivity exists (SSH admin gets access denied = exists; SYSTEM governor opens it: TouchEvent 1). S10eGovernor service (start= demand) running: GpuCounter 1; idle: LITTLE 546-650 MHz, MID 507 MHz, G3D 156 MHz; ~5 changes/s at first -> MinIntervalMs 200 (boost exempt) -> ~3/s, tracking real load bursts. 0 ACPM fails. Touch boost not yet exercised (needs a finger - user).
- 07:00 screen-off test (panelon 0 12 = panel dark for 12 s, process alive): governor ScreenOffs 1, all domains at minimum (LITTLE 442000, MID 507000, G3D 156000); after relight load tracking resumes (LITTLE 1456000, MID 754000). 2164 ACPM sends, 0 fails. Services set to start at boot: S10ePower auto, S10eGovernor auto depend= S10ePower.
- 07:10 reboot test: S10ePower + S10eGovernor auto-start RUNNING after boot, 201 sends 0 fails; temps 63 C right after
  boot. Battery 16 % -> 15-min dark-panel (panelon 0 900) measurement running before C5.
- Review of S10ePower / s10egovernor (harsh): (1) S10ePower has no rundown protection - an IOCTL racing Unload could
  touch unmapped MMIO; SCM refuses to stop a driver with a running dependent (governor depends on it), but a manual
  `sc stop` order mistake remains possible -> follow-up: IoAcquireRemoveLock-style counter. (2) AcpmSend busy-waits with
  KeStallExecutionProcessor up to 100 ms per request while holding a FAST_MUTEX (APC_LEVEL) - acceptable for 3/s, but a
  timeout costs a full CPU for 100 ms; follow-up: KeDelayExecutionThread after the first 1 ms. (3) gState fields
  written by the guard thread and the IOCTL path without a lock - benign counters/caps, but CurKhz could race; follow-up:
  update CurKhz under gAcpmLock. (4) Governor screen-off counter reuses gDom[0].Quiet as a marker - fragile; follow-up:
  own flag. (5) tzid mapping (1 MID, 2 LITTLE, 3 G3D) is still DT-derived - the readings are plausible and track load
  (47 idle -> 63 boot), not independently proven per zone. (6) Ladders duplicated in driver and governor.
- 07:40 C5 DONE (device, 30/25 s steps, battery 17 %, governor stopped during tests):
  MID 1794 -> 2314 MHz: every step granted and measured (s10eperf cpu4-5 1.79/1.89/2.00/2.10/2.20/2.31 GHz), peak
  74 C (2210 step), 67-69 C at 2314 (traces/.../c5-mid.txt). LITTLE 1456 -> 1950 MHz: 1.44/1.56/1.72/1.82/1.92-1.94 GHz,
  peak 60 C (c5-little.txt). G3D 260 -> 702 MHz with d3dtri cube: 0 present failures at every step, G3D <= 54 C, cube
  36.6 -> 38.8 fps (not GPU-bound) (c5-gpu.txt); real GPU clock via MW_ESCAPE_CLOCK (new tool
  drivers/MaliG76Wddm/tools/gpuclock): requested 156/260/433/702 -> measured 156.0/260.0/432.3/702.0 MHz.
  Result: with the 2 big cores off, the full DT ladders are usable on burst: LITTLE 1.95 GHz, MID 2.314 GHz, GPU
  702 MHz (sustained minutes-long thermals not measured yet - the S10ePower guard caps at 83/88 C).
  Governor ceilings set (Parameters): MaxLittle 1950000, MaxMid 2314000, MaxG3d 702000; touch boost BoostLittle
  1742000, BoostMid 2106000, BoostG3d 433000.
- 07:55 governor efficiency points (C5 follow-up): EffLittle 1456000 (LITTLE load tracking stops there unless a core > 90 % or boost), GpuFloor 260000 (was half the ceiling = 377 MHz at idle). Idle after deploy: LITTLE 1456 (background), MID 507, G3D 260 MHz.

## 2026-10-17 evening: "overclock" request, MID 2.4 GHz, stability tools
- User: overclock the 6 usable cores step by step + stability check. Reality: ACPM sets frequency AND voltage from
  fixed tables (fvmap/ECT); only listed rates can be requested. Beyond-table clocks would need direct PLL + PMIC
  programming with unverified values (hardware / data-corruption risk, rule 6) - not done; told the user.
- In-table headroom: MID top step 2400000 (DT, notes/cpu-driver-9820.md:156; needs MIF >= 2093000, mif-perf). Added
  to S10ePower's ladder with an interlock (granted only while this driver set MIF 2093000) and to the governor ladder.
  LITTLE: 1950000 is already the table maximum.
- Device: set mif 2093000 + set mid 2400000 -> ACPM echo 2400000, clocks.exe measured 2399 / 2386 MHz.
- tools: drivers/S10ePower/cpustress.c (one pinned thread per core, deterministic int/FP/memory workload checked
  against a per-thread reference -> silent computation errors), drivers/S10ePower/clocks.c (live measured per-core
  MHz + GPU MHz via MW_ESCAPE_CLOCK + governor/ACPM state; copied to the user's desktop), tools/phone/templog.ps1.
- cpustress results: baseline (governor) 60 s all cores PASS; MID 2400 pinned 2 cores 120 s PASS, 62 C; all 6 at
  LITTLE 1950 / MID 2400 120 s PASS but the S10ePower guard tripped (>= 83 C) mid-run and capped MID to the boot
  1794 (left there afterwards - guard caps only lower); rerun with temperature log (MID actually 1794, LITTLE 1950):
  zones 72-74 C sustained, 82 C peak at 47 s. => 2.4 GHz is computationally stable; sustained all-core max is
  thermally limited (~1 min to the 83 C trip) - bursts and 2-thread loads benefit. Governor MaxMid = 2400000.
- Windows' Task Manager "base speed 1.79 GHz" = Win32_Processor MaxClockSpeed / ~MHz from boot (cpu0 1456, cpu4
  1794), its "speed" = an estimate; it does not see ACPM rates.
- REVIEW 2250edd: governor lowered MIF before the CPUs on screen-off (MID could sit at 2400 with MIF 421 for a moment) -> fixed: MIF up before the CPUs (screen on), down after them (screen off). Deployed.
- clocks.exe is now a small GUI window (user: 'Core #0-5 - x / y, keep it simple'): Core #n - measured / table max MHz + GPU line, 1 s refresh, DPI-aware, measurement on a worker thread; the console variant is clockscli.exe (S10eTmp, for ssh). Deployed to the desktop and running. Governor in normal use (screen on): fast cores measured 2395 / 2389 MHz = the 2.4 GHz step is used.
