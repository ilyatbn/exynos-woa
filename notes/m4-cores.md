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
