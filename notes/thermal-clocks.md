# Thermals, then clocks (2026-10-15)

User: "After bluetooth let's focus on gpu again. I want the desktop look and feel and general ui to offload as much
software rendering to gpu to make it smoother. We also need to ramp up the clock speeds a bit."
Standing rule (memory, user): no GPU/CPU clock increases until Windows reads temperatures and can throttle.
Design source: notes/thermal-research.md (staged plan, constants table).

## Order of work
1. Stage 0 from Windows (no firmware change): S10eDiag peek of TMU_TOP 0x10090000 / TMU_SUB 0x100A0000 CONTROL
   (+0x50: bit0 CORE_EN, bit12 THERM_TRIP_EN), CONTROL1 (+0x54), THD0 (+0xD0, RISE7 = bits[24:16]) - is the TMU
   running, is the 115 C hardware trip armed. (Read-only; an unclocked block could SError -> bugcheck, recoverable.)
2. ACPM ch 9 READ_TEMP prototype as a user-mode tool on S10eDiag peek/poke (port of the UEFI ACPM client), tz 0..4.
3. Driver: S10eThermal (KMDF) polling temps, IOCTL_THERMAL_READ_TEMPERATURE, DSDT ThermalZones, passive cooling via
   GUID_THERMAL_COOLING_INTERFACE -> ACPM ch 0 FREQ_REQ caps (CPU clusters + G3D).
4. Clocks up (only after 3): G3D 260 -> 572 (then 702) MHz, LITTLE/MID toward max, measured with DWM presents/s
   and the cube, temperatures logged.
5. GPU offload of the desktop: what still renders in software (GDI/DirectWrite apps, XAML on WARP?) - inventory.

## Progress log
- 17:10 **Stage 0 answered from Windows** (s10ediag peek over ssh, read-only, no fault):
  - TMU_TOP 0x10090000 and TMU_SUB 0x100A0000 CONTROL (+0x50) = 0x00009503 on both: CORE_EN (bit0) = 1 and
    THERM_TRIP_EN (bit12) = 1 -> the TMU runs and the hardware thermal trip is ARMED on our boot path (someone
    before us - sboot/ACPM - programmed it). CONTROL1 (+0x54) 0x000a0000, +0x58 0x00010016.
  - THD (+0xD0..): 0x017a016d 0x01680163 0x015e0159 0x0154014f (rise), +0xE0 0x0176016c ... (fall).
  - Calibration (two-point, TRIMINFO(p) at p*4: bits[8:0] = 25 C code, bits[17:9] = 85 C code): sensor 0 TOP
    285 / 347 -> RISE7 code 0x17A = 378 -> 25 + (378-285)*60/62 = **115 C** = the DT hot trip. Confirms both the
    register layout and the calibration formula.
  - Current temperatures: +0x84..+0x98 = 11 sensors (low half = sensor 2k, high = 2k+1). TOP codes 0x13a 0x13b 0x139
    0x139 0x13d 0x13d 0x140 0x13e 0x13f 0x13d 0x13d; SUB 0x13c 0x13a 0x139 0x13b 0x139 0x13f 0x13b 0x13b 0x13a 0x139
    0x13b -> ~52-59 C (Windows idle-ish, fixed clocks, BIG cores parked).
  - DT zone masks: TOP BIG 0x7c0 (sensors 6-10), MID 0x20 (5), LITTLE 0x10 (4); SUB G3D 0x21e (1,2,3,4,9), ISP 0x100 (8).
  => No ACPM needed to READ temperatures: plain MMIO + per-sensor trim. (ACPM ch 9 stays the Linux path.)
  Next: s10etemp user tool (zones, max over masks), load test (temperatures must rise), then the driver/ACPI.
- 17:15 tools/phone/s10etemp (ARM64, S10eDiag READ_PHYS, read-only): zone maxima over the DT masks, per-sensor -v.
  Idle: BIG 57-60, MID 54-59, LITTLE 55-57, G3D 54, ISP 53-54 C. **Load test** (ffmpeg software h264 decode,
  all 6 cores, ~40 s): BIG 59.8 -> 63.7, MID up to 60.8, LITTLE up to 58.8, G3D 54 -> 55.9; after the load
  BIG back to 57.4-60.8 within ~15 s. => the MMIO readout tracks real temperature. (Phone clock in the log is
  not local time.)
- 17:20 tools/phone/s10eacpm (ARM64; port of the UEFI ACPM client over S10eDiag READ_PHYS/WRITE32): channel 0 =
  type 1 queue, ap_poll 1, q_len 15, elem 16 (same as UEFI run 30). No-op FREQ_REQs from Windows: G3D (10)
  260000 -> reply 00010000 0003f7a0 (= 260000), LITTLE (2) 1456000 -> 00010000 00163780 (= 1456000) - the same
  echo pattern as the UEFI runs. => **Windows can request clocks through ACPM** (user mode, one process at a time).
  Domain ladders: LITTLE 442k..1950k (13 steps), MID 507k..2400k (2314k/2400k need MIF >= 2093000 - avoid until MIF
  is known), G3D 156k..702k (156/200/260/325/377/433/572/650/702).
- 17:30 baseline (ssh, before any clock change): s10eperf cpu0-3 ~1.40-1.45 GHz, cpu4-5 ~1.79 GHz; d3dtri cube
  (runuser2, blt/DISCARD, FL 10_0 on "Arm Mali-G76 MP12") 260-384 fps over the first 8 s (run cut short by the
  runuser2 60 s wait); temps BIG 56.9 MID 55 LITTLE 54 G3D 53 C. gfxperf: DWM 1.0 fps composed while idle, CPU
  25-47 % with **WerFault up to ~100 % of a core** and MsMpEng 18-32 %.
- WerFault cause (Application log): **WindowsTerminal.exe access violation 0xc0000005 in d3d11.dll** (MoAppCrash,
  repeated) - Terminal's D3D11 renderer on our Mali UMD crashes; also LogonUI.exe APPCRASH in combase.dll (x2).
  -> GPU-offload compatibility item: D3D11 apps must not crash on the Mali UMD (task for the UI-smoothness work).
- 17:35 **governor live** (scheduled task S10eGovernor, onstart, SYSTEM; started now): log "CPU level 0 (LITTLE
  1742000, MID 2106000)", "GPU level 0 (572000)". Measured: cpu0-3 ~1.67-1.74 GHz (was 1.40-1.45), cpu4-5 ~2.10
  GHz (was 1.79). Temps under the cube + idle desktop: BIG peak 66.6, MID 62.7, LITTLE 60.8, G3D 57.9 C (governor
  thresholds 78/80 C not reached). d3dtri cube first 3 s 394-416 fps (baseline 262-384).
- d3dtri cube STALLS after ~7-8 s in BOTH runs (before and after the clock change): fps lines stop, the process
  never exits (runuser2 done marker missing after 100 s). Not caused by the clocks. -> UI/GPU task: find the stall
  (present path / GPU job / fence) together with the WindowsTerminal d3d11 crash.
- 17:45 crash survey (Application log, last 80 EventID 1000): TextInputHost 21 x ucrtbase.dll, WindowsTerminal
  19 x d3d11.dll, ShellExperienceHost 19 x ucrtbase.dll, SearchHost 13 x ucrtbase.dll, StartMenuExperienceHost 3 x
  ucrtbase.dll -> the XAML shell hosts crash-loop (fail-fast in ucrtbase); a big part of "the desktop is not smooth".
  Terminal dump (WER LocalDumps): d3d11!NDXGI::CDevice::WaitTrackedFenceImpl+0x1c ldr x8,[x21] with x21 = 8 (a field
  of a NULL fence object) <- d3d11!CDevice::WaitForFence <- dxgi!CDXGISwapChain::AcquireBuffer <- d3d11!CContext::
  AcquireResource <- OMSetRenderTargets <- d2d1 EndDraw <- Terminal Atlas BackendD2D: the swap chain's buffer
  fence is missing (composition/flip swap chain on the WDDM 1.3 Mali adapter). Global WER LocalDumps enabled
  (C:\S10eTmp\dumps, minidump, 20) to catch the shell hosts.
- 17:50 correction: the shell-host crashes are OLD (latest ShellExperienceHost/SearchHost/StartMenu 2026-10-14
  13:19Z, TextInputHost 10-14 08:26Z; this boot 10-15 11:47Z) and all four hosts run now. Only WindowsTerminal
  crash-loops on this boot (latest 12:27Z).
- 17:48 second silent reset today (16:22 was before any clock change). debug_history (TWRP): unrequested resets show
  as "SOFT, INFORM3(12345670) > NORMAL" (INFORM3 = the normal-boot value BootLogDxe sets at ExitBootServices),
  OS restarts as REBOOTMODE0, recovery as REBOOTMODE4. With BEYOND0LTE_WINDOWS_WATCHDOG 1 the 82 s cluster-0 WDT
  (0x10050000) stays armed in Windows and Exynos9820Ufs pets it (RWD1 record 0xFED13D80; NoPetReason stops the pet
  on fatal conditions) -> prime suspect: petting stopped. Temps at the reset 62-67 C (not thermal); the governor log
  shows the KD break gap (50 s) right before it. Instrumentation: s10egov now logs WTCON/WTCNT + the 8 RWD1 words
  every 30 s and every second while WTCNT < half.
- The S10eGovernor "onstart" task never fired at boot (default battery conditions fixed; still no boot run) ->
  collect.cmd (S10eAgent, every boot, SYSTEM) now starts C:\S10eTmp\s10egov.exe.
- 18:06 boot after TWRP: collect.cmd started s10egov at boot (log "CPU level 0 ... GPU level 0" without manual
  start) - the boot path works. New build swapped in (taskkill + sftp + schtasks /run): "wdt con 0000ff39 cnt
  64870/65525" = cluster-0 WDT armed and petted (counter near full); RWD1 record = all zeros (that PRAM handshake is
  not in use on this build; UFS_LOW_WDT_PRAM_ENABLE 0) -> WTCNT is the evidence to watch at the next silent reset.
  Post-boot temps LITTLE up to 68.5 C (boot load).
- 18:20 **the "cube stall" was not a GPU stall.** cubelife.ps1 (process sampler): d3dtri ran its full time, but the
  wrapper cmd.exe died with it (no "exit" line, no runuser2 done marker, stdout tail + summary lost). Cause:
  Windows 11 default-terminal delegation - every console started in the user session (runuser2's cmd.exe) is hosted
  by Windows Terminal, which crash-loops (d3d11 WaitTrackedFenceImpl, above) and takes the console session (cmd +
  child) down. Fix for the harness: user S10e HKCU\Console\%%Startup DelegationConsole = DelegationTerminal =
  {B23D10C0-E52E-411E-9D5B-C09FDF709C7D} (Console Host). Rerun: d3dtri cube 10 s complete, "d3dtri: done, exit 0",
  marker written; fps 391/448/396/296/313/296/290/270/275 (G3D 572 MHz, CPU 1.74/2.10 GHz).
  (A first attempt wrote the key as "%%%%Startup" - deleted.) Remaining real GPU bug: Terminal's d3d11 crash.
- 18:45 **Terminal crash root cause (static RE of the phone's dxgi.dll/d3d11.dll with public symbols; reference only,
  nothing committed from it):** dxgi!CDXGISwapChain::BindToDwm+0x150 calls
  CDXGISwapChain::CreateTrackedFenceExtended (-> device internal interface slot 46 -> d3d11!NDXGI::CDevice::
  CreateFence, a runtime sync call, opcode 0x23, that builds a D3D11_TRACKED_FENCE) which returns SUCCESS with a NULL
  fence on our adapter; BindToDwm stores it at swapchain+0x550. The first AcquireBuffer that has to wait
  (presents >= buffers) calls d3d11!CDevice::WaitForFence(NULL) -> WaitTrackedFenceImpl(&NULL->x = 8) -> AV.
  Every DWM-bound flip/composition swap chain on the Mali adapter has this (Terminal's SwapChainPanel; XAML
  SwapChainPanel apps; DirectComposition presentation). (inference) The tracked fence = a WDDM 2.0 monitored fence;
  our KMD is WDDM 1.3 (Windows 11 expects WDDM 2.x). Fix options: (a) KMD -> WDDM 2.0 (monitored fences, GPU VA
  model) - large; (b) per-app software rendering (Terminal "rendering.software") - workaround; (c) investigate whether
  a WDDM 1.3 flag makes DXGI skip the tracked fence (UNVERIFIED).
  Current effect: with Console Host as default terminal Terminal no longer auto-launches -> crash loop stopped.
- 18:50 third unrequested reset, this time to TWRP. s10egov log (read-only from TWRP): WTCNT 65053..65062/65525
  every 30 s (petted), last line "12:32:54 ... cnt 49626/65525" (~20 s without a pet), then NOTHING - the governor
  stopped too => the whole system froze (UFS pet DPC + user-mode governor both dead); the 82 s WDT then reset it.
  So the WDT is the messenger; the cause is a SYSTEM HANG (at idle, at the raised clocks: CPU 1.74/2.10 GHz,
  G3D 572 MHz). The 16:22 reset was at base clocks (cause not captured). Open: is the hang clock-related (CPU
  2.1 GHz voltage / G3D 572 MHz) or older (pre-existing silent resets)? Next evidence: KD session during idle
  ("every" break mode brackets a freeze) or run a day at base clocks for comparison.
- 18:56 bisect config live (boot after the TWRP reset): CPU level 0 (1742/2106 MHz), G3D held at 260 MHz; WDT petted (65062/65525). If idle hangs continue -> not the G3D clock.
- 19:05 G15 S0: phone = Windows 11 23H2 10.0.22631.6199 (UBR 0x1837); MaliG76Wddm oem29.inf Started, Parameters GpuEnable 1, FullAdapter 1, ScanoutStride 0x440, OfferRotation 1. Design: notes/gpu-wddm2-design.md.
- 2026-10-15 ~20:29 local: second silent system freeze + reset (user: "it just restarted"). Evidence: System 41 +
  6008 (unexpected shutdown, no bugcheck, no Minidump dir), new boot at 20:32. s10egov.log: last line 12:13:40
  (phone clock) with WTCNT 62869/65525 = still being petted 1 s earlier; NO per-second "cnt below half" lines
  follow -> the governor froze together with the whole system (same signature as 18:50), and the 82 s cluster-0
  watchdog reset it (ssh lost ~20:29:41, ping lost 20:30:39 ~ +60-80 s). Conditions: GPU held at 260 MHz (bisect),
  CPU boost level 0, temps 52-57 C (not thermal), DWM NOT running (crashed 19:52 in maliumd.dll, see
  notes/gpu-wddm2-design.md) -> GPU essentially idle. => the GPU clock is unlikely to be the trigger; KD was
  detached (time limit 19:51), so no kernel capture. Phone wall clock is not synced across boots (event times jump).
- 2026-10-15 ~22:08 local: third silent freeze + watchdog reset (notes/goal-bbb-bt-wifi.md): governor's last line
  WTCNT 64268/65525, temps 55-61 C, no bugcheck/minidump. Context: Wi-Fi reconnecting between 2.4 and 5 GHz networks,
  BT A2DP streaming. All three freezes so far had the Wi-Fi driver associated; GPU idle in two of them.
- 2026-10-16 ~02:09 local: fourth silent freeze (user away, phone idle on 2.4 GHz Wi-Fi, BT 1.0.0.16 / WlanCx 1.0.10.7):
  ssh closed while ping half-answered, then reset; BootLog "Windows died 2 times in a row - ignoring stay.flg,
  going to TWRP" (so a death was already counted from an earlier boot). No bugcheck.
- 2026-10-16 ~03:20 local: fifth silent freeze during Wi-Fi diagnostics (WlanCx 1.0.10.12, 2.4 GHz, BT device
  disabled at the time, repeated driver reloads + UDP/TCP load tests in the minutes before): same signature (ssh
  closed, ping half-answering ~1 min, reboot; uptime 1.2 min at 03:22, stay.flg brought Windows back). All five
  freezes had the Wi-Fi driver associated; BT was NOT running in this one -> BT driver excluded as the sole trigger.
- 2026-10-16 ~04:07 local: sixth silent freeze, phone IDLE (no tests running for ~15 min; WlanCx 1.0.10.20 on 2.4 GHz,
  BT 1.0.0.16 on). Same signature (ssh closed, ping answering ~1 min, then reset); being the second death in a row
  (after 03:20) BootLog parked it in TWRP. Battery 44 %. Rebooted to Windows with `adb reboot`.
- 2026-10-16 ~05:01 local: seventh silent freeze, idle, ~46 min after the 04:15 boot (WlanCx 1.0.10.20 2.4 GHz, BT
  on); stay.flg -> straight back to Windows (uptime 1 min at 05:03). Spacing tonight 03:20 / 04:07 / 05:01 = ~50 min
  between freezes (each ~45-50 min after a boot) - a timer/counter-like period worth checking (log rotation? alive
  counters? a Windows idle task?).
- 2026-10-16 ~05:48 local: eighth silent freeze, idle; second in a row -> TWRP. **Period:** freeze-to-freeze for
  back-to-back Windows boots 03:20 -> 04:07 (47 min), 05:01 -> 05:48 (47 min); 2026-10-15: 18:50 -> 20:29 -> 22:08
  (99 min = 2 x ~49.5). Per boot: freeze + 82 s cluster watchdog + ~60-70 s boot to Windows, so the freeze lands
  ~44-45 min after power-on. **Hypothesis: 2^36 ticks of the 26 MHz system counter = 2643 s = 44.05 min** (counter
  reset by the SoC reset). Test: read QPC (frequency + value) right after the next boot and predict the moment
  CNTVCT crosses 2^36; compare with the next freeze. UNVERIFIED until that comparison.
- 05:52 test armed (boot at 05:50:10 by `adb reboot`): Stopwatch.Frequency 26000000; KUSER_SHARED_DATA QpcBias
  (0x7FFE03B8) -1212029012 -> raw CNTVCT = QPC - bias = 116.3 s at 05:52:06, i.e. the counter starts at the reset.
  PC-side logger (one ssh session, scratchpad cntlog.ps1, raw counter every 2 s with PC timestamps) predicts
  CNTVCT = 2^36 at **06:34:17**. Pre-registered: a freeze within ~1 min of that = hypothesis supported; a freeze
  elsewhere or none by ~06:45 = rejected.
- 06:37 **result: 2^36 hypothesis REJECTED.** CNTVCT passed 2^36 at 06:34:17 with the phone alive; the logger's last
  line was 06:37:25 at raw counter 73621194128 = 2831.6 s (47.2 min after the reset), then ssh died (freeze #9,
  idle). The time-to-freeze per boot is still ~47 min (consistent with #6-#8) -> next hypothesis: a resource that
  runs out at a constant rate (pool / commit leak, handle leak). Next boot: sample memory + pool counters each minute.
- 07:25 **freeze #10 exactly as predicted**: memory sampler (scratchpad memlog.ps1, every 50 s) alive at raw 2821 s,
  ssh dead ~2835 s (#9: alive 2831.6 s). Memory FLAT the whole boot: avail ~2.1-2.3 GB, commit ~4.0 GB, nonpaged
  ~90-109 MB, paged ~180-237 MB, handles 54-60k, no growing pool tag (top NP: ConT 133 MB constant) -> NOT a leak.
  => deterministic time-triggered hang at ~2832 s (47.2 min) after the SoC reset, independent of load (logger
  traffic in #9, idle in #6-#8). Next: KD host attached from boot (kdrun.py), CTRL_BREAK ~15 s after the predicted
  freeze to see every CPU (!running -ti) before the watchdog resets it.
- 07:29 KD capture armed: `adb reboot` 07:26:35, raw counter 157.7 s at ~07:29:14 -> reset ~07:26:36; predicted
  freeze ~08:13:48 (reset + 2832 s). tools/kd-runner/kdrun.py gained --break-at EPOCH (absolute-time CTRL_BREAK; the
  first attempt's --break-after 2800 would have fired before the freeze because KD connects only ~27 s after the
  reset). kdrun started with --break-at 08:14:03 and commands .reload; !running -ti; !cpuinfo; ~*k; r; !analyze -v; g.
  Killing the first kd host while connected did not disturb the target (ssh up at 07:29:02, KD reconnected).
- 08:14 KD capture at the freeze FAILED: break-in at 08:14:03 -> "Failed to write breakin packet ... HOST cannot
  communicate with the TARGET" -> the hang also takes down the KDNET/USB path (whole-system hang, not one thread).
  Last KD output before it: routine WlanCx alive ticks (phone on HomeX-5 / 5 GHz this boot, 216 Mbit/s HE) and BT
  stats lines (rx bursts of ~70 KB) - nothing unusual. Freeze #11 at ~08:13:48 as predicted; also on 5 GHz.
- 08:16 experiment armed (reset 08:13:53): SYSTEM task wifiexp.cmd disables ACPI\SAMS9825\0 (WlanCx) at raw ~375 s,
  re-enables at raw ~3000 s; KD stays attached over USB; kdrun --break-at raw 2900 s (09:02:13). A successful
  break-in at 2900 s = the system survived past 2832 s without Wi-Fi -> WlanCx / BCM4375 implicated.
- 08:25 correction: `pnputil /disable-device ACPI\SAMS9825\0` -> "Cannot disable critical system device"; used
  `pnputil /remove-device` instead (KD log: "[cx] worker stopping ... D0 exit to state 5 ... released" at raw ~680 s);
  `pnputil /scan-devices` at raw ~3000 s brings it back. KD break still at raw 2900 s (09:02:13).
- 09:01 **freeze #12 WITHOUT Wi-Fi**: WlanCx removed at raw ~680 s (D0 exit + released in the KD log), system
  alive over KD (BT stats lines) until the hang at the predicted raw ~2832 s (09:01:05); KD break at 2900 s failed
  (target unreachable); BootLog -> TWRP. => **WlanCx / BCM4375 driver is NOT the trigger.** BT driver also excluded
  (freeze #5 hit with the BT device disabled). Freeze tracks the RAW counter (time since SoC reset), not Windows
  boot time: boots with QpcBias 23.9 s and 46.6 s both hung at raw ~2832 s. 2832 s is not a power of two of 26 MHz
  (2^36 = 2643 s) nor of the obvious clocks (24.0/24.576/32.768 kHz). Suspects now live below Windows / independent
  of our Windows drivers' activity: a firmware-side timer counting from the reset (our UEFI, Samsung EL2 uH / EL3 /
  TEEGRIS secure timer, ACPM) or a remaining always-loaded driver (GPU KMD, audio, S10eBattery, UFS, governor
  agent). Tools for the next step: the m14 EL3 PC sampler (notes/m14-smp.md "PC sampler") or the DSS/scan2dram
  dump path (BEYOND0LTE_DSS_DUMP, S2D.BIN / ARRAYDUMP.BIN) to read the hung cores' PCs - both need a new boot image.
- 13:46 **freeze #14/15 with a per-second recorder** (scratchpad frzlog.ps1 via a reconnecting ssh loop: per-core CPU,
  DPC/interrupt, top processes, raw counter): the phone was IDLE to the end - last sample raw 2833.2 s (13:46:54):
  total CPU 1-3 %, DPC 0 %, interrupt 0 %, top MsMpEng 0.1-0.2 CPU-s per sample, nothing ramping; then silence and
  "Connection reset". => no workload / CPU / DPC storm precedes the hang; it is instantaneous at ~2833-2835 s after
  the SoC reset (13:46:35 predicted). Points further at a firmware/hardware timer event (EL3/uH/ACPM/our UEFI) rather
  than a Windows component. Second death in a row -> TWRP.
- 2026-10-16 16:14 **freeze #16** (boot reset ~15:26:39, predicted 16:13:51): recorder's last sample 16:13:54 at raw
  **2832.0 s**, CPU 1 %, DPC 0 (user had been playing mpv at ~95 % CPU until ~16:08, then idle); ssh reset at 16:14:18.
  Spread of the hang moment across #9/#10/#15/#16: raw 2831.6-2835 s.
- Freeze #16 timing detail: next boot's raw counter 186 s at 16:17:08 -> SoC reset ~16:14:02, i.e. ~8 s after the last
  recorder sample (16:13:54), not freeze + 82 s WDT. Either the counter does not start at the reset (bootloader time
  not counted? then reset is earlier still) or something other than the 82 s cluster WDT resets the SoC. UNVERIFIED -
  compare with debug_history / reset_reason next time the phone is in TWRP.
- 2026-10-16 17:00 **hypothesis: the KDNET debug transport.** User asked whether the USB debugger is running: no host
  on the PC, but the phone's BCD has had `debug Yes` (KDNET, busparams 0, hostip 169.254.255.255, port 50005) since
  2026-10-13 23:17 (notes/m12-wifi-wificx.md:429-433, commit 599d032); the first recorded silent resets are 2026-10-15
  (no Windows watchdog before 10-12 boot #97 -> earlier hangs could have gone unseen, so the timing is suggestive, not
  proof). Freeze #12 with KD *attached* and freezes with the host detached both had the transport enabled in the
  target. Test armed: `bcdedit /debug {current} off` at 17:00:32 (bootdebug already No) -> takes effect on the boot
  after freeze #17 (due ~17:01:15). Pre-registered: no freeze by reset + ~2900 s on that boot = KD transport is the
  trigger; a freeze at ~2832 s = KD excluded. Undo: `bcdedit /debug {current} on` (dbgsettings unchanged).
- 2026-10-16 17:01 **freeze #17** as predicted (17:01:15): last samples raw 2832.2 / 2832.6 s, CPU 2-5 %, DPC 0; ssh lost 17:01:41. KDNET still on for this boot (BCD change applies to the next).
- 2026-10-16 18:08:44 **freeze #18 with KDNET OFF** (BCD debug No, boot reset ~17:22:13 after the 1.0.1.22 hang):
  last sample raw **2790.9 s**, CPU 2-6 %, DPC 0, no newly started process in the last samples (recorder prints
  "NEW ..." since 17:17); ssh reset 18:09:07. => the KD transport is NOT the trigger (pre-registered test: freeze ->
  excluded). New detail: ~41 s earlier than the 2831.6-2835 s of #9-#17 - the first boot whose previous reset was the
  quick hang-reset of 17:22 rather than a watchdog/TWRP cycle; whether the raw counter really restarts at 0 on that
  reset path is UNVERIFIED (would shift the apparent hang time). Scheduled tasks checked (notes/phone-debloat.md): no
  boot/logon task near 47 min. BCD debug left off (user: leave it off for now).
