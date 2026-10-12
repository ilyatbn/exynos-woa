- Reused-driver resource contract (W, read 2026-10-12): m1probe.c:231-238 and malikm/device.c:43-48 map the
  **first** CmResourceTypeMemory; maliwddm/adapter.c:106-126 MapGpu the same. Interrupts: malikm/device.c:49-59
  creates up to 3 WDFINTERRUPTs in resource order, one ISR (MkEvtIsr) for all, which reads all three
  INT_STAT blocks (maligpu.c:373-424) -> **any order works**; m1probe connects none (polled, m1probe.c:210-211).
  -> _CRS: GPU block first, then the PMU page, then job 320 / mmu 319 / gpu 321 (= DT order JOB, MMU, GPU,
  gpu-odin-rom.md:55-56,253). A second Memory32Fixed is invisible to maliwddm/malikm (they keep the first).
- Panfrost quirks for G76 (local clone C:\Users\ilya\code\mali\linux, b20a9b5f9): panfrost_gpu.c:117-175
  init_quirks: SHADER_CONFIG gets only SC_TLS_HASH_ENABLE (:141-145; hw_features_g76 has TLS_HASHING,
  panfrost_features.h:85-97; hw_issues_g76 = 0, panfrost_issues.h:259, so no issue-gated bit); TILER_CONFIG
  read back + written (:149-155); JM_CONFIG = JM_DEFAULT_IDVS_GROUP_SIZE 0xF << 16 (:166-170, regs.h:227-228;
  same as maligpu.c:95-98). Called from panfrost_gpu_power_on (:458) after soft reset (:519), before L2_PWRON.

## Task 1 - G9 ACPI half (Device GPU0)

- `Dsdt.asl` Device (GPU0): _HID SAMS9828, _UID 0, _CCA 0; _CRS = Memory32Fixed 0x18500000/0x5000 (FIRST),
  Memory32Fixed 0x15862000/0x1000 (PMU page with 0x2104/0x2384), Interrupt Level/ActiveHigh/Exclusive {320} JOB,
  {319} MMU, {321} GPU. Overlap check: other DSDT windows near it are AUD0 0x15861000 (PMU+0x1000 page), TSP0
  0x15850000, CMU_G3D 0x18400000 is in no device; nothing else claims 0x15862000 or 0x18500000-0x18504FFF
  (rg Memory32Fixed over Dsdt.asl).
- `_STA`: OperationRegion GPST SystemMemory 0x8FE0F010 len 4; 0x0F iff GPUS == 0x30555047 ('GPU0'), else 0 -
  the WLAN pattern.
- `Beyond0LteGpuProbeDxe.c`: GpuProbe() now returns BOOLEAN usable (TRUE only on the path that read GPU_ID != 0/~0
  and ((Id>>16)&0xF00F) == 0x7001, which itself is only reached with both PMU bits 1 and the GPU gate not
  manually off). Entry: word [4] = 0 first (also on the disabled and 0x6A-skip paths), after the last probe
  (the re-probe if GpuPowerOn returned TRUE) word [4] = 'GPU0' or 0, one log line
  "GpuProbe: ACPI GPU0 visible|hidden (status word [4] = ..)".
- `ArmPlatformLib.c` (SEC): word [4] = 0 next to the [2]/[3] clears.
- Cases: flag 0 + sboot leaves G3D off (expected) -> hidden. flag 0 + sboot leaves it on -> visible with sboot's
  clock (no ACPM request by us; the probe logs PLL_G3D M/P/S). flag 1 -> visible after a successful power-on;
  power-on failure path -> domain back off -> hidden; ACPM FREQ_REQ failure -> domain on, clock unknown -> visible
  (logged "on[9] ... FAILED").
- Build (2026-10-12, scratch copy `scratchpad\build-g9b.ps1` of build-g9.ps1: `$ws` = this worktree's firmware,
  edk2 = main checkout's, WORKSPACE = `subst W:` of `scratchpad\g9bbuild`, Conf copied from g9build; flag 0 as
  committed). First try exit 1: my scripted edit put a raw newline into the new DEBUG string (clang
  "missing terminating '\"'"), fixed before commit. Second: **exit 0**, `FVMAIN_COMPACT [99%Full] 774144 (0xbd000)
  total, 772192 used, 1952 (0x7a0) free` (was 2024 free after e951137 -> +72 B), FVMAIN 33 %; FD 7 340 032 B sha256
  `021ab3cdb5dfdb01...`; GpuProbeDxe.efi 14 336 B (contains "ACPI GPU0"). Dsdt.aml disassembled (iasl -d):
  GPU0 _HID SAMS9828, _CRS bytes = Memory32Fixed 0x18500000/0x5000, 0x15862000/0x1000, three Interrupt descriptors
  flags 0x01 (consumer, level, active-high, exclusive) with GSIV 0x140/0x13F/0x141 = 320/319/321, GPST at 0x8FE0F010,
  _STA compares 0x30555047. Not packed, not flashed.
- FVMAIN_COMPACT headroom is now 1952 B: the next firmware change may need the FV auto-grow seen with flag 1.

## Review of 889f781 (G9 ACPI half, adversarial)
- Checked mechanically: 9810 tree untouched (`git diff --stat e951137..HEAD -- firmware/Platform/Star2LtePkg
  firmware/Silicon/Exynos9810Pkg` empty); Dsdt.aml disassembled = intended _CRS bytes (above); no other ACPI table
  in Beyond0LtePkg/AcpiTables names GSIV 319/320/321 or the 0x15862000 page (rg); GpuPowerOn's return values
  (GpuPower.c:87-233): FALSE for "already ON" (first probe's verdict stands) and for the failure path (domain
  back off -> first probe said OFF -> hidden), TRUE after success or after an ACPM failure (domain on, re-probed).
  The status write precedes the existing `dsb sy` after the guard restore, and the page is uncached.
- Accepted, not defects (recorded): (a) ACPM FREQ_REQ failure -> GPU0 visible at sboot's/reset PLL_G3D rate (the
  re-probe already read GPU_ID through that clock, so the block answers); (b) EMBEDDED on + outer off at entry ->
  hidden and not powered on (conservative); (c) 0x6A-skip boot -> hidden (word cleared before the skip).
- Nothing to fix in a follow-up commit.

## Task 2 - G9b drivers/MaliG76Probe (design, 2026-10-12)

Source: W `drivers/m1probe/m1probe.c` (280 lines, GPL-2.0, gahingwoo, commit 16f7e7c6), INF m1probe.inf.
Retarget per R-list:
- R3: binds ACPI\SAMS9828. R1: GPU = first memory resource; the driver ALSO checks it is 0x18500000/0x5000 and
  the second is 0x15862000/0x1000 (refuses otherwise, BAT0/AUD0 style) - no MmMapIoSpace of a hard-coded PA
  (m1probe mapped PMU_PA itself; we take the PMU page from _CRS).
- R4: powered = PMU+0x2104 bit0 == 1 AND PMU+0x2384 bit0 == 1 (polarity inverted vs RK). Checked before the first
  GPU access in PrepareHardware and again right before the run. Not powered -> no GPU mapping, no interrupt
  objects, run log "not powered", device starts doing nothing. CMU_G3D gate (R4 3rd condition) is not in _CRS:
  UEFI already required it for the 'GPU0' word that makes the device present (_STA) - trusted, recorded.
- R5: ((GPU_ID >> 16) & 0xFF0F) == 0x7201, else stop.
- R6: after soft reset, panfrost_gpu_init_quirks for G76: SHADER_CONFIG 0xF04 = SC_TLS_HASH_ENABLE BIT(17)
  (panfrost_gpu.c:141-145, regs.h:180,211), TILER_CONFIG read+write back (:149-155), JM_CONFIG = 0xF << 16
  (:166-170, regs.h:227-228). Read back and logged. L2_MMU_CONFIG (0xF0C) NOT written (panfrost does not; R6).
- R7/R8: L2_PRESENT must have exactly one bit (panfrost_get_core_mask, panfrost_gpu.c:429-450, uses all cores
  then); more than one -> stop and log (core-group masking left to G11, no guess). Power order as panfrost
  (:461-471): L2 = L2_PRESENT, poll L2_READY == L2_PRESENT; then SHADER = SHADER_PRESENT and TILER = TILER_PRESENT
  (m1probe.c:145-151), poll both; 20 ms per poll (panfrost readl_relaxed_poll_timeout 20000 us).
- R10 decision (page-table format): **Mali-LPAE ("legacy") exactly as m1probe**, TRANSTAB = L0 | 0x7
  (ADRMODE_TABLE 3 | READ_INNER, regs.h:359-361), MEMATTR 0x00888d88, TRANSCFG 0 (= AS_TRANSCFG_ADRMODE_LEGACY,
  regs.h:331), 4 levels L0-L3, leaf 0x2C5, tables | 3. Reason: the KMD we will reuse (maligpu.c:115-130) uses it and
  it ran on a Bifrost v7 G52; mainline panfrost uses ARM_MALI_LPAE on every SoC except MT8188/8192/8370
  (panfrost_mmu.c:786-795) although G76 also has HW_FEATURE_AARCH64_MMU (panfrost_features.h:94). Whether the
  G76 accepts legacy mode is exactly what this probe proves (AS0_FAULTSTATUS / MMU_INT_RAWSTAT logged).
  MILESTONES G5's AArch64-4K plan stays open (decide at G5; R10).
- R11: contiguous memory limited to min(40, MMU_FEATURES PA bits) (m1probe used 40; G76 PA bits UNVERIFIED -> read).
- R12: memory MmNonCached (no CPU cache maintenance), JS_CONFIG flush clean+invalidate at start and end (m1probe).
- R17: WRITE_VALUE job header/payload byte-identical to m1probe.c:182-186 on **JS0** (the slot m1probe proved on
  G52; panfrost's JS1 choice is for G6/G11). JS0_FEATURES logged.
- Completion: polled (authoritative, as m1probe) PLUS interrupts connected for the three _CRS lines: one ISR
  reads JOB/MMU/GPU_INT_STAT, clears what it saw, accumulates the bits per line. JOB_INT_MASK = JS0 done|fail
  (0x00010001), MMU_INT_MASK = AS0 fault (0x00010001) only during the job, GPU_INT_MASK stays 0; all masks 0
  again afterwards and in D0Exit. The poll ORs RAWSTAT with the ISR-seen bits (the ISR clears RAWSTAT). Result:
  whether each line fired = the first evidence for G11 that GSIV 319/320 + Level/High work (R2 UNVERIFIED).
- Timeouts: soft reset 100 ms (m1probe Poll 10000 x 10 us), AS update 100 ms, job 500 ms (m1probe 50000 x 10 us).
  Job not finished -> soft reset again before freeing the memory; if that reset also times out the 5 pages are
  leaked on purpose (the GPU may still write them).
- Output: \SystemRoot\System32\LogFiles\MaliG76Probe-run.txt rewritten at each checkpoint (Exynos9820Audio
  RunLog.c pattern), plus DbgPrintEx. Runs once per driver load, in EvtDeviceD0EntryPostInterruptsEnabled.

## Task 2 - implementation + build (2026-10-12)
- Files: drivers/MaliG76Probe/{MaliG76Probe.c, .inf, .vcxproj, build-package.ps1, LICENSE (= upstream LICENSE,
  sha256 498d3290..., GPL-2.0), README.md (origin)}; NOTICE gets a wddm-mali-g52 entry. KMDF 1.15 (repo
  convention; m1probe used 1.33 - nothing used needs > 1.11: WdfInterruptCreate in PrepareHardware is 1.11+).
- Flow: PrepareHardware: run log; resources logged (raw GSIV + level/edge per interrupt); contract check (2 memory =
  0x18500000/0x5000 then 0x15862000/0x1000, 3 interrupts) else STATUS_DEVICE_CONFIGURATION_ERROR; map PMU page;
  powered? no -> "RESULT: SKIPPED (not powered)", GPU never mapped; map GPU; GPU_ID gate; all INT_MASKs 0; 3
  WDFINTERRUPTs (one ISR). D0EntryPostInterruptsEnabled (once): MpRun. D0ExitPreInterruptsDisabled: masks 0 if
  still powered. ISR: re-checks PMU power, reads JOB/MMU/GPU_INT_STAT, clears + accumulates, counts per line.
- Build: `build-package.ps1 -Out C:\Users\ilya\code\s10e-artifacts\mali-g9b` **exit 0** first try (VS18 MSBuild,
  WDK 10.0.26100, /W4 /WX), Inf2Cat ok, InfVerif /w exit 0, sys + cat signed by "WDKTestCert ilya,134359747320405644"
  (status UnknownError on the PC = test root not trusted here, as for every package). MaliG76Probe.sys 30 616 B.
  sha256:
  - MaliG76Probe.sys `F4DC8B0176ADE0AF96EF73F41272F5E27146E544BE45C3200D44735181477B8D`
  - MaliG76Probe.inf `4B606010D4A9906DBD20CBE99426C1A59FC27C64953DCAFDD4236BF649A8245D`
  - malig76probe.cat `9170BB98F0A853F9AF3740A25327723DA5649107ACDBAF83D7C5C5FEBA292436`

## Expected run log (C:\Windows\System32\LogFiles\MaliG76Probe-run.txt), PASS case
```
[0.000] MaliG76Probe (M16 G9b) start - 5 resources
[0.000] res 0: memory 0000000018500000 len 5000
[0.000] res 1: memory 0000000015862000 len 1000
[0.000] res 2: interrupt raw vector 320 (expect the _CRS GSIV) level, translated vector .. level ..   (+ 319, 321)
[0.000] G3D powered (EMBEDDED_G3D_STATUS ...1 G3D_STATUS ...1)
[0.000] GPU_ID 72?1???? -> ((id >> 16) & ff0f) = 7201, G76 7201: yes
[0.000] ready: 3 interrupts created; the run starts once they are connected
[0.0xx] run: start
[0.0xx] GPU_ID .. L2_FEATURES .. CORE_FEATURES .. TILER_FEATURES .. MEM_FEATURES ..
[0.0xx] MMU_FEATURES .. AS_PRESENT .. JS_PRESENT .. JS0_FEATURES .. GPU_STATUS .. THREAD_MAX_THREADS .. COHERENCY_FEATURES ..
[0.0xx] SHADER_PRESENT 00000000:.... (12? cores) TILER_PRESENT 00000001 L2_PRESENT 00000001
[0.0xx] soft reset: GPU_INT_RAWSTAT 00000100 -> RESET_COMPLETED      (bit8; other bits may be set too)
[0.0xx] quirks: SHADER_CONFIG 00020000 TILER_CONFIG .. JM_CONFIG 000f0000
[0.0xx] power: L2_READY 00000001 TILER_READY 00000001 SHADER_READY ....
[0.0xx] memory: 5 pages at PA 00000000........ (limit 2^NN)
[0.0xx] AS0: TRANSTAB ..:.......7 MEMATTR 00888d88 TRANSCFG 00000000 (Mali-LPAE) - updated
[0.0xx] job: WRITE_VALUE c0ffee42 -> GPU VA 2000100 submitted on JS0 (affinity ....)
[0.0xx] job: JS0 DONE (JOB_INT bit 0); JOB_INT_RAWSTAT 00000000 JS0_STATUS .. MMU_INT_RAWSTAT 00000000 AS0_FAULTSTATUS 00000000 ..
[0.0xx] irq: ISR saw JOB 00000001 MMU 00000000 GPU 00000000; hits per line 1/0/0 (GSIV 320/319/321), empty 0
[0.0xx] memory: job header word0 (exception status) 00000001, target c0ffee42 (want c0ffee42)
[0.0xx] RESULT: PASS (the G76 wrote the value)
```
- Other shapes: "G3D NOT powered ... RESULT: SKIPPED" (GPU0 should then not exist at all - _STA); "GPU_ID .. NO";
  "core layout not handled" (L2_PRESENT != 1 bit or cores in SHADER_PRESENT_HI: needs the panfrost core-group
  mask, G11); "power: .. after 20 ms"; "AS0 .. still active"; "job: MMU FAULT" (LPAE rejected or a PTE bit wrong
  -> AS0_FAULTSTATUS/ADDRESS); "job: TIMEOUT" + "irq: ... 0/0/0" (no completion, no line). If JOB bits appear in
  "ISR saw" with hits on line 0, GSIV 320 + Level/High is proven for G11. "empty N" > 0 = the line fired without
  a GPU status bit (misrouted / wrong trigger).
- Exception status word: Bifrost job header exception_status low byte 0x01 = DONE (panfrost/kbase naming);
  the exact expected value is not asserted by the driver (PASS = completion + target value).

## Hazards (for the boot)
- Needs G9 flag 1 (or sboot leaving G3D on) for GPU0 to exist; with the committed flag 0 the package installs
  nothing that runs (no device).
- First GPU job ever on this phone: the G76 issuing DMA at a Windows PA through Mali-LPAE. A wrong PTE/attribute
  shows as an MMU fault (logged), not a hang, if the MMU behaves like the G52's. The pages are freed only after a
  completed job or a successful soft reset; otherwise leaked on purpose.
- Interrupts: GSIV 319/320/321 and Level/High UNVERIFIED (DT flags 0). A misrouted level line that is asserted
  without a GPU status bit would make the ISR return FALSE forever (interrupt storm -> CPU stuck). Counted as
  "empty", but a storm would prevent the log flush. Mitigation if it happens: uninstall / disable the service
  offline (R19 path) - a storm would show as a hang right after the driver loads.
- Clock: whatever UEFI's ACPM FREQ_REQ (260 MHz) left; no DVFS, no thermal under Windows. One short job only.
- A bus fault on the first GPU read (DTZPC not restored in UEFI) would be an SError -> bugcheck at driver load.
  UEFI's own re-probe read GPU_ID before setting 'GPU0', so this should already have been caught there.

## Review of 9caabb0 (G9b, adversarial)
- Checked mechanically: 9810 trees untouched (diff --stat e951137..HEAD on Star2LtePkg, Exynos9810Pkg,
  drivers/Exynos9810*: empty); page tables, job words, TRANSTAB |0x7, MEMATTR, JS_CONFIG byte-identical to
  m1probe.c (grep of both files side by side); every register offset used vs panfrost_regs.h (grep: GPU_* 0x004-0x0A0,
  PRESENT/READY/PWRON 0x100-0x1A0, JS 0x1800 + 0x24/0x40-0x60, MMU_BASE 0x2400 + AS 0x00-0x34) - all match;
  LICENSE sha256 = upstream; InfVerif /w exit 0.
- Found (fixed in the follow-up commit, not in 9caabb0):
  1. "job: completed" took JOB_INT ERR (bit 16) as completion: a FAILED job would have been logged "completed"
     (RESULT stayed FAIL only because the target would not match). Now the line says "JS0 DONE (JOB_INT bit 0)"
     or "JS0 FAILED (JOB_INT ERR bit 16)", and PASS needs JobBits == 0x1 exactly.
  2. PrepareHardware on a stop/restart re-used stale counters (IrqCount kept growing, ISR hits/bits not reset).
     Now reset at entry.
  3. The resource log line asserted "(GSIV)" for the raw interrupt vector; that the raw descriptor carries the GSIV
     on ARM64/ACPI is not verified here -> "(expect the _CRS GSIV)".
  4. This note's expected-log block carried an annotation "(ISR cleared it)" that the driver never prints - removed.
- Accepted (recorded): JS0 instead of panfrost's JS1 (R17, m1probe-proven); exception-status value not asserted.
- Rebuild after the fix: **exit 0**, sys 30 616 B. sha256 (FINAL package in s10e-artifacts\mali-g9b):
  - MaliG76Probe.sys `244550B8AB7751B6D96023A749C27889AF1B4709C1142FB3272A2851DD7EA4C6`
  - MaliG76Probe.inf `4B606010D4A9906DBD20CBE99426C1A59FC27C64953DCAFDD4236BF649A8245D`
  - malig76probe.cat `73D881487956DC831CA6B9D2F9C03D649D6939D6F44B8B6DE47CA3286F1BE123`
  (the sha256 list under "Task 2 - implementation + build" is the superseded pre-fix package)

## Review of 316a8e8 (review fix)
- `git show 316a8e8 -- drivers`: JobBits is masked to JOB_INT_JS0 before use, so PASS == "only DONE(0) of JS0";
  the counters reset happens before the log allocation and before any interrupt object exists (no ISR race);
  the log text change is wording only. After a JS0 ERR the slot has stopped, so freeing without a soft reset is
  unchanged behaviour (as m1probe). Rebuild exit 0 (above). No further defect.

## Next (main session)
1. Merge branch `worktree-agent-a6e14c0a6296725e6` (fast-forward on main e951137).
2. Test image: `BEYOND0LTE_GPU_POWER_ON 1`, boot, check UEFILOG "GpuProbe: ACPI GPU0 visible (status word [4] =
   30555047)", then in Windows `s10ediag peek 0x8FE0F010` and Device Manager ACPI\SAMS9828 with 2 memory + 3 IRQ.
3. Install s10e-artifacts\mali-g9b (pnputil /add-driver MaliG76Probe.inf /install, test signing on), read
   C:\Windows\System32\LogFiles\MaliG76Probe-run.txt. Have the R19 offline-disable path ready (IRQ storm hazard).

## Superseded (2026-10-12, M16 G10 rescue path, notes/gpu-m16-g10.md Task A)
- The case "flag 0 + sboot leaves it on -> visible" no longer holds: a `BEYOND0LTE_GPU_POWER_ON 0` image now always
  hides GPU0 (it is the rescue image), and `\S10EDIAG\nogpu.flg` on the ESP hides it on any image (word [4] = 'NOGP').
  For the G9b test use a flag-1 image without that file.
