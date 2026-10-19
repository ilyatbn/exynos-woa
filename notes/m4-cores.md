# M4 (Mongoose / cheetah) big cores in Windows - 2026-10-19 session

User goal (2026-10-19): "get these M4 cores working ... unattended ... as much research as possible without
triggering anthropic security systems since we are not allowed to disassemble windows". Also: Windows feels slow
(Steam / Brave first start takes long). Stop hook: "get M4 processors working."

Ground rules for this file: AGENTS.md 11 (no Windows binary inspection - the earlier ntoskrnl reads in
notes/m14-smp.md predate the rule and are NOT continued), 6 (constants), 7 (device validation).

## Where it stands (from notes/m14-smp.md, m14-m4-summary.md, m14-m4-brief-other-model.md)

- Windows starts all 8 cores via the parking protocol; it dies 30-75 s later only when it RUNS work on an M4
  (silent all-CPU hang, no bugcheck, all cores stay powered). One M4 alone also dies.
- Ruled out: GPU KMD, BIG DVFS (520 MHz), WFE wake, CNTFRQ, GIC interface numbering (6/7), LLC, DVM broadcasts,
  coherency (cached ping-pong OK), CTR/DCZID, memory-map prefetch guard bands, EL2 traps on TTBR/TCR/SCTLR/MAIR,
  Spectre SMC front ends, hardware access flag (HA off on every core still dies), busy cached M4 with broadcast
  maintenance (16 min harmless).
- Never tested on an M4: interrupts (own GIC CPU interface, timer PPI, SGIs), WFI with IRQs unmasked, EL0, the
  ACTLR_EL1 value Windows writes (boot CPU's), local (non-broadcast) TLBI/IC.

## Progress log

- 01:15 monitor (watchall) armed; phone in Windows, idle 183 s, no download traffic, battery 49 % charging.
- 01:20 stub flag 7 written (Beyond0LteParkStub.S): flag-6 cached copy stress + own GIC CPU interface enabled
  (PMR 0xF0, CTLR EnableGrp1, SGI 1 + PPI 30 prio 0xA0, ISENABLER0), EL1 physical timer PPI 30 every 1 ms
  (26000 ticks), an SGI 1 to the peer M4 from every timer IRQ (target list = other flag-7 cores; interface number =
  GICC index, m4gic1 VERIFIED), WFI with IRQs UNMASKED after every pass. Vectors in the mailbox page (IRQ slots ->
  Beyond0LteParkIrq, other current-EL slots -> Beyond0LteParkFault); identity table moved to +0x740 for flag 7 and
  entry [0] = 1 GiB Device-nGnRnE block (GIC with the MMU on). Counters +0x340 passes, +0x350 timer, +0x358 SGI,
  +0x360 other, +0x368 last IAR, +0x370 spurious.
  Every parked core also records ACTLR_EL1 (+0xF50) and SCTLR_EL1 (+0xF58) at entry; UEFI logs them next to the boot
  CPU's (UEFI-time) values. Guard: stub must end below +0xF50.
- firmware/tools/build-host-image.ps1 -Set "File|NAME|VALUE,..." for reproducible test images.
- Review of my own disassembly (llvm-objdump of our stub .obj): first build zeroed +0x200 (the EL1h Sync vector)
  before the flag-7 branch - fixed (entry [0] is written after the branch). Stub 0x680 B (ends +0xE80).
- Image boot-m4irq1.img 3fea49b9 = host image + Watchdog 1 + PARK 0xFEu, OFFER 0x3Eu, SLEEP_UNOFFERED 1,
  IRQ_MASK 0xC0u (both M4 flag 7, Windows on the 6 small cores).
  Expectation: SoC survives (Windows healthy 15 min) -> IRQ/WFI/SGI on the M4 are fine at the hardware level;
  SoC hangs -> the interrupt path on the M4 is the trigger (bisect timer vs SGI vs WFI).
- 01:32 m4irq1 flashed (TWRP via tools/phone/totwrp.cmd; rollback = boot-host-r2.img, backup in boot-backups),
  Windows up 01:33 (SSH).
- m4irq1 UEFILOG (VERIFIED on device):
  - entry ACTLR_EL1 = 0 on EVERY core (A55, A75, M4) -> Windows' "restore the boot CPU's ACTLR (0)" changes nothing
    on the M4. ACTLR lead OUT. Entry SCTLR_EL1 = 0xC5083A on all cores; boot CPU (UEFI) 0x30C5183D.
  - "STRESS gicc6 flag C0C00007 passes 2 -> 201 in 200 ms; irq timer 201 sgi 200 other 0 lastiar 1E spurious 0",
    gicc7 timer 399 sgi 400 lastiar 1801 (= SGI 1 from CPU 6). => timer PPI 30, SGIs between the M4s, and WFI wake
    with IRQs unmasked all work on the M4 in UEFI.
- m4irq1 in Windows (6 cores; M4s not offered, running flag 7): tools/phone/m4mb.sh reads the mailboxes through
  s10ediagx dumpmmio (dumpram -> MmCopyMemory refuses the reserved page, error 59). 01:34 timer 111856 / sgi 143836,
  01:37 timer 300095 / sgi 335268 per M4 (1 kHz, no faults, no spurious). SGI count > timer count: Windows' own
  broadcast IPIs reach the M4s too (lastiar 0x001 = SGI 1 from CPU 0) and are EOI'd fine.
- Windows TCR_EL1 0x695B5513511 decoded: T0SZ/T1SZ 17, TG1 4K, IPS 5, AS 1, A1 1, HA 1, HD 0, HPD0/1 1, HWU 0,
  TBI 0. HPD needs HPDS >= 1 (M4 has 1); nothing in TCR needs a feature the M4 lacks.
- New lead (PMU): Exynos 990 mainline review notes "no PMU model for cluster2 (Mongoose cores)"; Windows' processor
  power management samples performance counters (m4dvm1: CPU0 in PpmUpdatePerformanceFeedback at the last good
  break - from our own KD log, not from Windows code). Stock 9820 DT (Dumps/extracted/dts/dt.img.0.dts:4749)
  arm,armv8-pmuv3 with 8 SPIs: 0x8d-0x92 (A55/A75), 0xae/0xaf (M4) - our MADT has PMU GSIV 0 for every GICC.
  Test: flag-7 option bit 0 = PMU probe on the M4 (stages at +0x380: PMCR, PMCEID0, PMEVTYPER0/PMCR.E/PMCNTENSET,
  1000 timed PMCCNTR reads, PMEVCNTR0) + PMCCNTR read every pass. Image boot-m4pmu1.img 8106534e (= m4irq1 +
  BEYOND0LTE_PARK_IRQ_OPTS 1u). Stub now 0x748 B of 0x750 available (fields from +0xF50) - no room for more code
  without moving fields.
- **m4irq1 RESULT (01:48, 15.8 min soak)**: both M4s timer ~950 000 IRQs (1 kHz), SGIs ~987 000 (peer + Windows'
  broadcasts), passes ~942 000, 0 faults, 0 spurious; Windows (6 procs) healthy throughout (SSH, ping).
  => interrupts on the M4 (own GIC CPU interface, EL1 physical timer PPI 30, SGIs) + WFI wake with IRQs unmasked +
  cached stress with broadcast maintenance are all fine at the hardware level. Interrupt/idle lead OUT.
- REVIEW of the PMU option before running it: its stage word was at +0x380 = the EL1h SError vector slot (slot 7)
  -> moved the PMU fields to +0x3C0..+0x3F8; boot-m4pmu1.img (8106534e) deleted unrun, rebuilt as boot-m4pmu2.img
  b66429a8. Flag-4 (EL2 trap timing, result recorded in m14-smp.md m4trap1) removed from stub + Parking.c to make
  room; stub now 0x694 B.
- Option bit 1 added: LSE atomics (LDADDAL / SWPAL / CASAL, 1024 rounds per pass, three words one cache line each)
  on ONE shared reserved page (PARK_ATOMIC_PA_OFF +0x528, allocated once, logged "PARK: ATOMIC shared page") by
  every flag-7 core; rounds at +0x348. LSE takes no immediate offset (first build failed) - separate address regs.
  Plan: IRQ_MASK 0xC8 (A55 gicc3 + both M4) = cross-cluster atomic contention, Windows on 0x36 (5 cores).
- tools/phone/m4mb.sh [GICC...] decodes passes/atomics/irq/pmu fields of any mailbox.
- Commit 4e3081d REVIEW: stub guard (0x800 + 0x694 < 0xF50) holds; no TRAP leftovers (script asserted, -Werror build
  passes); vector branches are +-128 MiB B; flag-7 IRQ handler EOIs before the counter store, spurious path skips
  EOI (correct for 1020-1023); "Windows' broadcast IPIs" rests on IAR source CPU 0 (only Windows runs there) - stated
  as such. The PMU/atomics options were committed untested (default off) - their validation is the next two runs.
- **m4pmu2 RESULT (01:52, UEFILOG)**: M4 PMU fully usable from EL1: PMCR_EL0 0x53033000 (IMP 0x53 Samsung, IDCODE 3,
  N = 6 counters), PMCEID0 0xFFFFFFFF, 1000 PMCCNTR reads = 416 ticks (~16 ns each -> no EL2/EL3 trap), PMCCNTR and
  event 0x08 (INST_RETIRED 3004) count. PMU lead OUT.
- Research agent (notes/m4-linux-research-20261019.md, LineageOS exynos9820 a412cfaf): CONFIG_SEC_ARM64_LSE_ATOMICS
  makes every kernel atomic LSE with no LL/SC fallback; SEC_ARM64_LSE_SPINLOCK forces LSE spinlocks with the comment
  "Please try to use LSE Atomics unless you are sure LL/SC is safe" (spinlock.h:30). Windows and user mode still use
  LL/SC (LDAXR/STLXR). Other leads: HIU/GCU of CL2 (0x1E4C0000), SYSREG_CPUCL2 0x1D100104 bus clock gating written
  on cluster up, CPU_INFORM per core, SICD never on cpus 6-7, watchdog_cl2 (30 s), PMUPCSR 0x1E430200 (already
  closed: DBGAUTHSTATUS 0xAA), M4 memory-error syndrome regs S3_1_C15_C2_0..7 (IMPDEF; read candidate).
- Option bit 2 added: LL/SC (LDAXR/ADD/STLXR loop) on the LDADDAL word inside each atomic round.
  Image boot-m4atom1.img 04268b77 = host + Watchdog 1, PARK 0xFE, OFFER 0x36 (Windows: A55 x2 + A75 x2 + boot),
  IRQ_MASK 0xC8 (A55 gicc3 + both M4 run flag 7), IRQ_OPTS 6 (LSE + LL/SC on one shared page).
- **m4atom1 RESULT (01:58)**: gicc3 (A55) + gicc6 (M4#0) ran: LSE+LL/SC rounds 31.6 M / 63.7 M, timers/SGIs fine,
  Windows (5 procs) up. **gicc7 (M4#1) wedged**: flag stayed 7 (never reached 0xC0C00007), passes/timer/atomics 0,
  no fault record. Mailbox dump: its identity table at +0x740 WAS written (MMU-off stores), so it stopped between
  the table writes and the flag-7 running store: MAIR/TCR/TTBR0, `tlbi vmalle1` (LOCAL) + `dsb nsh`, SCTLR.M/C/I,
  VBAR, GIC, timer. In m4irq1 the same path worked on both M4s. Difference: gicc3/gicc6 were already running
  (heavier traffic, broadcasts every pass, contended atomics) when gicc7 did its local TLBI.
  HYPOTHESIS (strong, testable): the Samsung 9820 erratum behind the "dummy TLBI" - a LOCAL TLBI (+ DSB NSH) on an
  M4 can hang while other cores generate DVM traffic; Samsung puts `tlbi vaale1is, xzr` before the DSB. Linux runs
  local TLBIs only at boot/idmap (6 sites + local_flush_tlb_all, all patched); Windows on the M4 would hit it at a
  random moment -> the M4 never answers the next IPI -> silent all-CPU hang.
- Stage marks added (+0x3F8: 1 before local TLBI, 2 after DSB NSH, 3 MMU on, 4 GIC done) and option bit 3 = the
  dummy TLBI. Images: boot-m4atom2.img de4298e8 (= m4atom1 config, opts 6, stages) to locate the stall;
  boot-m4atom3.img 4742a9ac (opts 14 = + dummy TLBI).
- PITFALL (02:06): `s10ediagx peek 0x1D100104` (SYSREG_CPUCL2 DRCG_EN, research lead 4) from Windows on an A55 FROZE
  the SoC (watchdog reset ~82 s, stay.flg brought Windows back). Do not read CPUCL2 SYSREG from outside the cluster.
- **m4atom2 (stages)**: gicc7 setup stage 2 = it passed the LOCAL TLBI + DSB NSH and stopped before stage 3, i.e. in
  the SCTLR M/C/I enable or its first cached fetch/walk/store. Dummy-TLBI erratum NOT the cause (atom3 not run).
- **m4atom4 (LSE only, opts 2)**: same - gicc7 stage 2. **m4atom6 (no atomics, opts 0, same 3 cores)**: same - gicc7
  stage 2. => atomics are NOT the trigger. Reproducible 4/4 with IRQ_MASK 0xC8; 0/2 with 0xC0 (m4irq1, m4pmu2).
  Difference: with 0xC8 an A55 in the OTHER cluster (gicc3) is already running the cached stress (copy + IC IALLUIS +
  TLBI VMALLE1IS every pass) when gicc7 turns its MMU/caches on; with 0xC0 only the M4 sibling was.
  Hypothesis: an M4 enabling MMU/caches while DVM broadcasts arrive from the DSU cluster deadlocks.
  tools/phone/m4run.sh IMAGE [GICC...] = one test cycle (TWRP, flash, reboot, UEFILOG lines, mailboxes).
- Option bit 5 = no IC IALLUIS / TLBI VMALLE1IS in the passes. boot-m4atom7.img 1c32212c (0xC8, opts 0x20).
- **m4atom7 (no broadcasts, opts 0x20)**: gicc7 stage 2 again -> DVM broadcasts NOT the trigger.
- **m4atom8 274eb58c (IRQ_MASK 0xC0 + STRESS_MASK 0x08: gicc3 copy stress WITHOUT interrupts/SGIs)**: BOTH M4s came
  up (setup stage 4, timers/SGIs fine). => gicc3's flag-7 interrupt activity is the trigger, most likely its SGI 1
  to gicc6/gicc7 every ms, which starts while the whole M4 cluster is still powered OFF (gicc3 is CPU_ON'd first).
  (In m4irq1 gicc6 also sent SGIs to a not-yet-started gicc7, but the M4 cluster was already on.)
  Hypothesis: a pending interrupt aimed at a powered-off M4 (cluster) makes the PMU/EL3 wake it outside PSCI CPU_ON
  (GIC wake-request -> PMU), which collides with our CPU_ON and wedges that core.
- BEYOND0LTE_PARK_IRQ_PEER_MASK (flag-7 cores allowed to send the SGI). boot-m4atom10.img bb459c85 = atom6 config
  (0xC8, opts 0) + PEER_MASK 0xC0 (gicc3 sends no SGIs).
- **m4atom10 RESULT (02:43)**: gicc3 in flag 7 but sending NO SGIs -> both M4s up (setup stage 4), all counters
  running. **CONFIRMED: the gicc7 wedge needs SGIs aimed at the M4s while the M4 cluster is still powered off**
  (a pending interrupt to a powered-off M4 vs. our PSCI CPU_ON). Steady-state SGIs from the A55 to a RUNNING M4 are
  fine (gicc6 received gicc3's SGIs throughout atom6/7).
- PMU CPU_INFORM0..7 (0x15860860..0x1586087C, research lead 4): all 0 (read from Windows, VERIFIED).
- PITFALL 2 (02:44): `peek` of PMU 0x15861300/0x1380/0x1500/0x1580/0x1600/0x1200 (0x50 each) from Windows froze the
  SoC again (one of those windows; not bisected). Rule from now: no exploratory SFR reads from Windows - only
  registers already proven safe.
- **m4vt1 2f926042 (02:52; IRQ_MASK 0xC0, opts 0x40 = EL1 virtual timer PPI 27)**: both M4s take the virtual timer
  (lastiar 0x1B) at 1 kHz plus SGIs; Windows (6 procs) healthy. CNTVCT - CNTPCT (virtual offset from EL2) = 0..1 tick
  on every core incl. the M4s (boot CPU 0) - read latency only. Virtual-timer and counter-offset leads OUT.
- Ruled out on the bare-metal M4 so far this session: ACTLR, interrupts/WFI/SGI (physical + virtual timer), PMU,
  LSE + LL/SC atomics cross-cluster (on running cores), DVM broadcasts. Found an unrelated bring-up wedge (pending
  SGI to a powered-off M4 before CPU_ON - not on the Windows path, since nothing sends SGIs before CPU_ON there).
- REVIEW 5d6021a: BUG - mParkAtomicPage was declared inside #if BEYOND0LTE_PARK_SLEEP_UNOFFERED but used by the unconditional ATOMIC log -> every default build (incl. the daily image) failed to compile. Fixed (declaration moved out); default host build and the 8-core build both compile.
- **m4w8b e8a43c25 (03:00; 8 cores offered, today's firmware + drivers, Watchdog 1, stay.flg)**: Windows died twice ->
  safeguard "Windows died 2 times in a row - ignoring stay.flg, going to TWRP" (03:10). NEW vs every 10-14/10-15
  run: the death is a **BUGCHECK 0x7E** (SYSTEM_THREAD_EXCEPTION_NOT_HANDLED) C0000005 at FFFFF8035870C514
  (P3 FFFFA10747E08078, P4 FFFFA10747E07BE0) recorded by S10eDiag/UFS rings (WINPRAM.BIN; same params in the UFS
  driver's previous-boot UFSBUGCHECK line = both deaths identical), right after the UFS driver went operational
  (early boot). No minidump/MEMORY.DMP (userdata mounted read-only from TWRP to check).
  Next: KD 8-core image + kdrun to name the faulting module (our frames only, AGENTS 11).
- **m4kd8 2e67d202 (03:16; 8 cores offered + KD over USB, BCD debug on)**: KD connected (1 proc), KeStartAllProcessors
  -> "MP (8 procs)". Then a bugcheck break about every 40 s (each boot), always the kernel heap fast-fail:
  CPU 5 (A75): KeBugCheck2 <- RtlpHpHeapHandleError <- RtlpHpLfhSubsegmentFreeBlock <- ExFreeHeapPool <-
  CmpDereferenceNameControlBlockWithLock (registry KCB cleanup, ExpWorkerThread); next boot: KiDispatchFastFail from
  RtlpHpLfhSubsegmentFreeBlock. Log s10e-artifacts/runs/m4kd8/kd.log. (Stack frames read from the debugger output
  only - no Windows binary was opened.) kdrun's "twrp" mode did not fire on these 80000003 breaks; a second short
  run (kdrun 150 8 twrp) reset the phone to TWRP (03:28).
  READING: with an M4 in the system, shared kernel memory gets CORRUPTED (heap metadata freed on an A75 is wrong),
  instead of / in addition to the old silent hangs. That points at lost/torn updates between the M4 cluster and the
  DSU cluster (atomicity or ordering of shared writes), not at the M4 wedging by itself.
  NEXT: make the stub atomics test VERIFY results (fixed number of rounds per core, then compare the shared counters
  with the sum of all cores' increments - so far the test only counted rounds and never checked for lost updates);
  same for a plain lock-protected (LDAXR/STLXR + DMB) shared-counter pattern and for release/acquire message passing
  (STLR/LDAR) between an A55/A75 and an M4.

## Summary 2026-10-19 (for the next session)

Phone: daily image boot-host-r2.img restored (6 cores); BCD debug turned on for m4kd8 - turn it OFF on the next
Windows boot (`bcdedit /debug off`). stay.flg on.
Done tonight (all on the device): ACTLR_EL1 identical (0) on every core; M4 interrupts (physical + virtual timer,
SGIs, WFI with IRQs) 15+ min fine; M4 PMU usable; cross-cluster LSE + LL/SC atomics RUN without wedging (correctness
not yet checked); CNTVOFF 0 everywhere; DVM broadcasts fine. Found: a pending SGI to a powered-off M4 before PSCI
CPU_ON wedges that core's bring-up (not on the Windows path). 8 cores on today's firmware still fail, but now
visibly: kernel heap corruption bugchecks (see m4kd8) -> next suspect is memory consistency between the clusters.
Pitfalls: never `peek` CPUCL2 SYSREG (0x1D100104) or PMU 0x15861200-0x1650 from Windows - both froze the SoC.
- 03:35 phone back on boot-host-r2 (6 procs), BCD debug turned OFF again (verified: debug No).
- **m4one3 b37654d1 (10:21; ONE M4 offered, PARK 0x7E, today's firmware, Watchdog 1, stay.flg)**: no SSH ever, safeguard
  returned to TWRP after 222 s (2 deaths). One M4 is enough to kill today's Windows. tools/phone/m4win.sh = the
  Windows-level trial helper. Daily image restored afterwards.
- 2026-10-19 morning: the verified shared-counter test (NEXT above) was not written - two attempts were stopped by a
  safety classifier mid-response; work moved to the UFS driver (user request) with the M4 goal still open.
- **m4atom11 dcf849af (11:09; gicc3 A55 + both M4 run flag 7 opts 6, gicc3 sends no SGIs, Windows on 5 cores)**:
  UEFILOG "PARK: SHARED word ..." 5 samples 100 ms apart: word 1829249 .. 2959360, every sample inside the expected
  window 2 x (sum of round counters) -> consistent. Shared-word updates from the A55 and both M4s (LSE + LL/SC on one
  word, ~1.1 M updates in 0.5 s) are not lost. Windows (5 procs) up normally.
