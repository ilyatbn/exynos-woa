# M14 - Windows SMP via the ACPI Multiprocessor Parking Protocol (S10e) (2026-10-11)

Task: Windows boots numproc=1. Samsung EL3 is PSCI 0.1 (m10 W8). Bring up the 4 A55s (MPIDR 0x000-0x003)
through the ACPI parking protocol, stop Windows using PSCI (restart -> FADT ResetReg = PMU SWRESET, W32 VERIFIED).
No device access in this task; build-verified only (Guided, not Ready).

## Progress log
- S9 implementation read (Star2LtePkg PlatformBootManagerLib.c:8210-8562, called at :9807-9812 in BDS):
  * allocates STAR2LTE_PARK_COUNT (3) pages AllocateMaxAddress 0xFFFFFFFF, EfiReservedMemoryType; zeroes them.
  * per page: +0x00 UINT32 = target MPIDR (1,2,3); +0x800 = stub (17 insns, emitted as words at run time with
    the mailbox PA baked as MOVZ/MOVK immediates); +0x18 = liveness mark 0x5A5ABEEF written by the core itself.
  * stub: x1=mailbox; store mark; DSB; loop { delay 0x2000; DC CIVAC x1; DSB SY; ldr x2,[x1,#8]; cbz -> loop };
    x0 = x1; br x2. No WFI/WFE, no ProcessorId check, JumpAddress NOT cleared.
  * WriteBackInvalidateDataCacheRange + InvalidateInstructionCacheRange over the pages, then for each core
    ArmCallSmc3(0xC4000003, mpidr, page+0x800, 0); Stall 30 ms; read marks.
  * patches the INSTALLED MADT (found via RSDP->XSDT) GICC +16 ParkingProtocolVersion=1, +24 ParkedAddress=page,
    only for cores whose CPU_ON returned 0; recomputes checksum.
  * S9 FADT kept ArmBootArch = PSCI_COMPLIANT (Star2LtePkg/AcpiTables/Fadt.aslc:75).
  * Reason (S9 comment): EL3 delivers x0=0xBFFF01C instead of context_id; CPU_ON of big core 0x100 hung the SoC.
  * Result on S9: "boot-v22 brought the four Cortex-A55s up" (S9 comment at the PMU probe block).
- Spec / reference implementations (fetched 2026-10-11, copies in the session scratchpad):
  * Mailbox (ACPI "Multiprocessor Startup for ARM Platforms" v1, as implemented by u-boot
    arch/arm/include/asm/acpi_table.h + arch/arm/cpu/armv8/acpi_park_v8.S and Linux
    arch/arm64/kernel/acpi_parking_protocol.c): one 4 KiB, 4 KiB-aligned page per CPU; +0x000 UINT32 ProcessorId
    (firmware initialises it to 0xFFFFFFFF = invalid), +0x004 reserved, +0x008 UINT64 JumpAddress (0 = invalid);
    0x000-0x7FF OS area, 0x800-0xFFF firmware area (u-boot ACPI_PP_CPU_CODE_OFFSET 2048, Minoca RPi2
    ARM_PARKING_PROTOCOL_FIRMWARE_OFFSET 0x800 - stub code lives there).
  * OS side (Linux): writes JumpAddress, THEN ProcessorId (= MADT GICC CPU interface number), then a wakeup SGI.
    Linux refuses to start a CPU whose mailbox ProcessorId != 0xFFFFFFFF, and WARNs if firmware did not clear
    JumpAddress after taking it. Linux maps the mailbox Device (spec says nGnRnE).
  * Firmware side (u-boot): WFE; wait ProcessorId != invalid AND JumpAddress != 0; clear JumpAddress; dsb; br
    with x0 = mailbox page. Minoca (Windows-on-RPi2 UEFI): WFI loop until ProcessorId == own id, then read
    JumpAddress, store 0, dsb, bx.
  * ACPI 6.3 5.2.12.14 GICC: ParkingProtocolVersion (+16, 0 = PSCI only), ParkedAddress (+24, 64-bit PA of the
    mailbox). FADT ARM_BOOT_ARCH (ACPI 6.3 table 5-36): bit0 PSCI_COMPLIANT, bit1 PSCI_USE_HVC.
  * Decision: our stub POLLS (no WFI/WFE): a parked core's GIC CPU interface is never enabled by firmware, so
    a WFI wake on Windows' SGI is not guaranteed; the S9 polling loop is the variant proven under Windows.
    It ignores ProcessorId (per-CPU mailbox, S9-proven), waits only for JumpAddress != 0, clears it (spec ack),
    x0 = mailbox. ProcessorId initialised to 0xFFFFFFFF (spec/Linux), not the S9's MPIDR.
- Repo facts: bank0 = PcdSystemMemoryBase/Size (0x80000000..0xBAB00000, live DTB + /proc/iomem); reservations
  (monitor carve-outs, boot log, WINPRAM, UFS DMA pool) are memory-allocation HOBs, so AllocatePages can never
  return them. ArmSmcLib = MdePkg ArmSmcLib (DSC:171); CacheMaintenanceLib = ArmCacheMaintenanceLib.
  AcpiTableDxe recomputes checksums on InstallAcpiTable (AcpiTableProtocol.c:806-814).
  FVMAIN_COMPACT is auto-sized (FDF NumBlocks 0): "free" is padding to the next 4 KiB block.
- Precedent check for FADT PSCI_COMPLIANT=0 + parking under Windows: edk2-platforms RaspberryPi keeps
  PSCI_COMPLIANT (TF-A) - not a precedent. Lumia950XLPkg (MSM8994, MADT.MpPark.asl exists) sources not
  retrievable (404). => "Windows boots and resets via FADT ResetReg with PSCI_COMPLIANT=0" is UNVERIFIED;
  the S9 (proven) combination was PSCI_COMPLIANT=1 + parked GICCs (Windows picks parking per GICC).
  Design: two independent switches, BEYOND0LTE_SMP_PARKING and BEYOND0LTE_FADT_PSCI (default !SMP_PARKING), so
  the S9 combination is one flag away if Windows rejects a non-PSCI FADT.
- DSDT CPU0-7 _MAT carry ParkingProtocolVersion 0 / ParkedAddress 0 (static). S9 DSDT did the same and parking
  worked there, so Windows reads the parking fields from the MADT. Left unchanged.
- Implementation written (uncommitted): AcpiTables/Beyond0LteAcpiHeader.h switches (BEYOND0LTE_SMP_PARKING 1,
  BEYOND0LTE_SMP_PARK_MASK 0x0E, BEYOND0LTE_FADT_PSCI = !PARKING); Fadt.aslc ArmBootArch per switch;
  Beyond0LteAcpiPlatformDxe: Beyond0LteParking.c/.h + AArch64/Beyond0LteParkStub.S, called on the MADT buffer
  before InstallAcpiTable (DXE driver entry). Pages: AllocateMaxAddress(EXYNOS_DRAM_BASE+EXYNOS_DRAM_BANK0_SIZE-1),
  EfiReservedMemoryType, one per parked core. Alive marks at +0xFE0 (firmware half), not the S9's +0x18 (OS half).
  Advertise only if CPU_ON==0 AND alive mark seen within 100 ms; other enabled secondaries -> MADT disabled.
- Build 1 (12:55): exit 0. FVMAIN_COMPACT 720896 total, 718944 used, 1952 free (99%); FVMAIN 28%.
  Stub disassembly (llvm-objdump of Beyond0LteParkStub.obj) = 21 insns, 0x54 B, matches the source.
  REVIEW finding: linking ArmLib (for ArmReadMpidr) pulled ~70 ASM_FUNC sections, each a 4 KiB-aligned PE
  section, into the driver. Fix: own 2-insn Beyond0LteParkReadMpidr in the .S, ArmLib dropped from the INF.
- Build 2 (12:57): exit 0, FVMAIN_COMPACT 719000 used / 1896 free. CORRECTION of the build-1 review: the ~70
  .text.Ar* sections are still there without ArmLib in our INF - they come from ArmCacheMaintenanceLib's own
  ArmLib dependency, already in the driver before m14. Own MPIDR helper reverted; ArmReadMpidr used (no new code).
- REVIEW finding: gBS->Stall in the mark wait. This driver's depex (AcpiTable + FV2) does not order it after
  Metronome; CoreStall returns EFI_NOT_AVAILABLE_YET without delaying when gMetronome == NULL
  (MdeModulePkg/Core/Dxe/Misc/Stall.c), which would have collapsed the 100 ms wait to ~0 and disabled every
  core. Fixed: TimerLib MicroSecondDelay (DSC TimerLib = ArmArchTimerLib, also used by Beyond0LteUfsHcDxe).
- Build 3 (12:59): exit 0. FVMAIN_COMPACT 720896 total / 719192 used / 1704 free (99%); FVMAIN 28%.
  FD 7 340 032 B sha256 8ADCA173...0B1A0417. Compiled Fadt.acpi checked byte-wise: ArmBootArch(@129)=0x0,
  Flags=0x300400 (HW_REDUCED|LOW_POWER_S0|RESET_REG_SUP), ResetReg 0x15863A00, ResetValue 2.
  Linked driver: smc #0 path present (ArmCallSmc), stub section .text.Beyond0LteParkStub 0x58 B.
- Final self-review: (a) alloc failure / empty mask => every enabled secondary MADT-disabled = uniprocessor,
  logged - intended fail-safe. (b) DSDT CPU4-7 _MAT keep Flags=ENABLED while the MADT disables them (same
  documented mismatch as BEYOND0LTE_UNIPROCESSOR_BOOT) - risk, not fixed. (c) a core whose CPU_ON succeeded
  but never marked is left running (unknown state) and MADT-disabled - logged.
- Constants: 0xC4000003 CPU_ON = live DT /psci (VERIFIED read, m10 W8). Mailbox offsets 0/8/0x800 = spec
  (cited above). 0xFFFFFFFF ProcessorId-invalid = spec/u-boot/Linux. 0xFE0/0xFE8/0xFF0 mark offsets, 0x5A5ABEEF,
  delay 0x2000, 100 ms wait = our choices (not hardware constants). Allocation ceiling = EXYNOS_DRAM_BASE +
  EXYNOS_DRAM_BANK0_SIZE - 1 (both VERIFIED, Exynos9820.h:110-111). MPIDRs = Madt.aslc (live DT reads).
  UNVERIFIED: Samsung EL3 CPU_ON of 9820 A55s from UEFI under uH; entry EL; Windows accepting PSCI_COMPLIANT=0;
  Windows restart via ResetReg with PSCI off.
- Out of scope / next: remove BCD numproc=1 on the device (bcdedit /store <ESP>\EFI\Microsoft\Boot\BCD
  /deletevalue {default} numproc) once UEFILOG shows "3 core(s) parked". MILESTONES/HANDOFF not updated (no commit).

## First on-device test (recommended)
1. Flash this image, keep BCD numproc=1. Expect Windows to boot exactly as before (one CPU). In \UEFILOG.TXT:
   "PARK: mask=e boot-mpidr=0 stub=84 B", "PARK: 3 page(s) at ba....000: Success", three
   "PARK: CPU_ON gicc1..3 ... -> 0", three "PARK: giccN parked mb=... el=4 mpidr_el1=8100000N|N", then
   "gicc4..7 ... disabled" and "PARK: 3 core(s) parked; FADT PSCI=0".
   Restart from Windows must now reboot via the PMU SWRESET (no 0xA0 / no B3=0x84000009 in S10eDiag).
   A hang right after "CPU_ON gicc1" = the 9810 big-core failure mode on a little core: force-restart,
   set BEYOND0LTE_SMP_PARK_MASK 0 (keeps PSCI off) and report.
2. Then delete numproc: Windows should show 4 logical processors (Task Manager / msinfo32), CPUs 1-3 no longer
   code 10, CPUs 4-7 absent (MADT-disabled). If Windows fails to boot with PSCI off, rebuild with
   BEYOND0LTE_FADT_PSCI 1 (S9 combination).
- Run 14 (boot #49, 13:05, numproc=1 still set, back 467 s): UEFILOG "PARK: CPU_ON gicc1..3 -> 0", all three
  "parked ... el=4 mpidr_el1=8100010x (0 ms)" (EL1, MPIDR 0x100/0x200/0x300 = Aff1 1..3 ... NOTE the MPIDR_EL1
  reads 0x81000100 etc - Aff1 holds the core number, the MADT/PSCI value 1/2/3 still worked for CPU_ON), gicc4..7
  disabled, "3 core(s) parked; FADT PSCI=0". Windows booted with FADT ArmBootArch=0 (desktop 205 s), collect ran,
  restart -> TWRP. => parking + PSCI-less FADT boot is OK on device.
- Run 15 prep: BCD numproc removed on the device (PC bcdedit /store on the pulled ESP BCD, '{default}' quoted);
  backup %TEMP%\c60\bcd\BCD.orig. extra-cpu.cmd: CPU counts + System events 41/6008/1074/1001.
- REVIEW FLAG (run 14 log): the parked cores report MPIDR_EL1 0x81000100 / 0x81000200 / 0x81000300 (MT bit 24 set,
  core number in Aff1), while the MADT GICC MPIDRs are 0x1/0x2/0x3 (Madt.aslc). Samsung EL3 accepted 1/2/3 for
  CPU_ON, but Windows matches a running core's MPIDR_EL1 to its GICC entry -> a mismatch may break the secondary
  start. If run 15 fails there: MADT (and DSDT _MAT, and the park mask MPIDR compare) -> 0x100/0x200/0x300, after
  reading the live DT /cpus/cpu@*/reg in TWRP (and the boot core's own MPIDR_EL1 = 0x81000000).
- Run 15 (boot #49, numproc removed, 13:14, back in 228 s): Windows NumberOfLogicalProcessors = 4 (wmic cpu + cs),
  TotalPhysicalMemory 5 631 082 496. Desktop "explorer up 30 s" at 98 s (was ~200 s on 1 core), collect.cmd 9 s
  (was ~100 s). => 4 A55s WORK via the parking protocol; the MPIDR 1/2/3 vs MPIDR_EL1 0x100/0x200/0x300 difference
  did not matter (Windows wakes each core through its GICC's ParkedAddress mailbox).
  Restart events: each previous boot still shows Kernel-Power 41 + EventLog 6008 (unclean) next to the 1074 restart
  request - whether the PSCI-less restart itself is clean (no 0xA0) is not yet read from the PRAM ring. TODO.
  Next: mid cores (A75, GICC 4/5) with a self-healing guard (a hang in CPU_ON before ReadyToBoot would loop every
  82 s with no WINRUN.FLG), then the big cores (S9: CPU_ON of a big core hung the SoC).
- Run 16 (boot #50, mask 0x3E, 13:20, back 218 s): all 5 secondaries parked (gicc4/5 = A75, MPIDR_EL1
  0x81000400/0x81000500), Windows NumberOfLogicalProcessors = 6. Mid cores WORK.
  UEFILOG progress prev=0x81 = run 15 ended through S10eDiag's recovery reset (bugcheck or fallback), so the
  PSCI-less restart is still not clean - read the mark ring next.
- Run 16 restart decoded (firmware bounce, S10eDiag ring): shutdown notification -> BUGCHECK 0x1E
  KMODE_EXCEPTION_NOT_HANDLED P1 C0000005 P2 FFFFF803E17838F8 P4 0x8FF00000 = the PHYSICAL boot-log ring. With
  PSCI hidden, Windows restarts via EFI ResetSystem; a DEBUG print in the runtime path wrote through
  Beyond0LteFbSerialPortLib to 0x8FF00000 after SetVirtualAddressMap. Fix (boot #51): DXE_RUNTIME_DRIVER DebugLib ->
  BaseDebugLibNull. Also Beyond0LteResetSystemLib now keeps INFORM3 if it is already recovery (0x12345674), else
  "Reboot to TWRP" would restart into Windows through this path.
- Boot #51 also: SAFE mask 0x3E (GICC 1-5 proven), PARK mask 0xFE (+ M4 big cores GICC 6-7) behind the 0x65 guard.
- Run 17 (boot #51, mask 0xFE, 13:28, back 233 s): UEFI parked ALL 7 secondaries incl. the M4s (gicc6/7
  MPIDR_EL1 0x80000100 / 0x80000101 - MT bit clear on the M4, Aff1=1 cluster, Aff0 0/1). => CPU_ON of a 9820 big
  core does NOT hang (unlike the 9810). But Windows never reached the agent: S10eDiag loaded at 73 s, then no
  more marks, no bugcheck (no =BUGCHECK, no S10eDiag BC lines); next UEFI boot saw progress prev=0x72 (Windows
  running, no 0x80/0x81) and took the boot pair. => offering the big cores hangs Windows' boot, and something
  reset the SoC without a crash record. Open questions: Windows' processor-start of the M4 (different MPIDR
  format MT=0 vs the A55/A75 MT=1; MADT efficiency class 0; GICR/PMU/ID differences), EL3/uH reaction.
  Default back to mask 0x3E (6 cores). Boot #52 = 6 cores + the run-16 restart fix (not yet exercised).
- Run 18 (boot #52, 6 cores + restart fix, 13:34): RESTARTS ARE CLEAN - user: "it keeps loading windows then safely
  rebooting (i see a you are about to be logged out screen). nothing crashes"; System log: four 1074 planned
  restarts in a row, no Kernel-Power 41 between them (the one 41 = the user's forced reset). 6 logical CPUs each pass.
  But the recovery reboot reason did not survive: every pass UEFI saw progress 0x80 ("Windows asked for this
  restart - continuing") -> endless agent loop until the user forced TWRP by keys.
  Fix (boot #53 + S10eDiag): on a recovery reboot also clear PMU RSTSTAT (+0x404, VERIFIED) like Samsung's recovery
  reboot / BootLogDxe RebootToRecovery / S10eDiag's hard reset (all of which do reach TWRP): ResetSystemLib when
  INFORM3 is already recovery; S10eDiag at RECOVERY_ON_RESET and in the last-chance shutdown notification.
  UNVERIFIED that RSTSTAT is what sboot keys on - run 19 decides.
- Run 19 (boot #53, RSTSTAT also cleared): still loops (user: "looks the same already restarted twice") - INFORM3 +
  RSTSTAT on the clean ResetSystem path do not make sboot boot recovery. Boot #54: ResetSystemLib stores progress
  0x81 (not 0x80) when INFORM3 is recovery -> the next UEFI boot (WINRUN.FLG present) takes the boot pair's
  dump + RebootToRecovery, which is proven. Why sboot ignores INFORM3 after this path: open (sboot disassembly).
- Boot #55 (FB console off, 14:25, stay.flg): user saw "hal initialization failed" (0x5C). UEFILOG: CPU_ON gicc1-5
  all -> 0, but only gicc5's mark seen, at 100 ms (the wait limit); gicc1-4 "disabled (on=0 mark=0)" while
  actually running into the stub => MADT said disabled, cores alive -> HAL_INITIALIZATION_FAILED. Cause: a core
  needs ~100 ms from CPU_ON to the stub (EL3/uH bring-up); the old text console's slow DEBUG rendering between
  CPU_ON and the wait padded that. Fix: PARK_WAIT_STEPS 100 -> 2000 (2 s max). Boot #56.
- Boot #56 (2 s wait): HAL_INITIALIZATION_FAILED again (user). UEFILOG: gicc1-4 still never marked, gicc5 (the LAST
  CPU_ON) seen only at the end => not a timing problem. Hypothesis: back-to-back CPU_ON calls (no text-console
  rendering between them any more) - only the last one reaches the stub (EL3/uH keep a single pending entry?).
  UNVERIFIED. Fix (boot #58 = + clean shim): one core at a time - CPU_ON, wait for its own mark (<= 2 s), then
  the next; per-core arrival ms logged on the CPU_ON line.
- Boot #58 (serialized CPU_ON + clean shim, 14:32, stay.flg): USER CONFIRMED "its booted ... i see 6 processors",
  most of the RAM; feels slow ("maybe because of no gpu"). => serialization fixed the HAL failure.
- drivers-branch notes/psci-parking.md (user's research) confirms the DT/PSCI numbering (A55 0x0-0x3, A75 0x4-0x5,
  M4 0x100-0x101) and leaves "does Windows implement parking v1 with PSCI cleared" open - runs 15/16 answered it
  (yes). Combined with our stub log: real MPIDR_EL1 A55/A75 0x81000100..0x81000500 (MT=1, core in Aff1), M4
  0x80000100/0x80000101 => A55#1's real affinity 0x100 == the M4#0 MADT entry. Theory for the run-17 hang: Windows
  matched A55#1 to the M4's GICC. Boot #64: BEYOND0LTE_SMP_REAL_MPIDR 1 - every parked core's MADT MPIDR = its
  reported MPIDR_EL1 & 0xFF00FFFFFF (boot core from ArmReadMpidr), later duplicates disabled (expect M4#0 out ->
  7 cores); park mask 0xFE again. CPU_ON still uses the DT numbering. RISK: DSDT _MAT still carries the DT
  numbers (MADT/_MAT mismatch) - flag off restores boot #63 behaviour.
- Run 34 (boot #64, REAL_MPIDR + mask 0xFE, 16:23, back 126 s): MADT patched as designed (gicc0 0, gicc1-5
  0x100..0x500, gicc6 disabled as colliding with gicc1, gicc7 0x101 enabled). Windows died BEFORE S10eDiag loaded
  (PRAM ring held only run 33's lines, no bugcheck record), UEFI saw progress 0x72 -> boot pair. => not the MPIDR
  collision: offering even one M4 (real, unique MPIDR) kills Windows early - the M4 running Windows' processor
  start/init path (CPU feature / system-register difference on Samsung's custom core, or EL2/EL3 reaction).
  Needs kernel-level debugging (KDNET/USB) - parked. Defaults restored: mask 0x3E, REAL_MPIDR 0 (= boot #63).

## MADT efficiency classes were inverted (2026-10-11, user question on big.LITTLE scheduling)
- ACPI 6.0+ GICC ProcessorPowerEfficiencyClass: "lower efficiency class numbers are more efficient than higher
  ones" (Linux arch/arm64/include/asm/acpi.h comment; Windows uses the field to pick cores for foreground/high-QoS
  work). Madt.aslc (from the 9810) had EFF_BIG 0 / MID 1 / LITTLE 2 with the comment "0 = highest performance" ->
  Windows saw the four A55s as its FASTEST class and the A75 pair as middle.
- Fixed: EFF_LITTLE 0, EFF_MID 1, EFF_BIG 2 (Pptt.aslc comment updated; no code reads the macros elsewhere).
  Built; compiled MADT decoded: uid 0-3 eff 0, uid 4-5 eff 1, uid 6-7 eff 2. Device effect UNVERIFIED (expect
  foreground threads on CPU 4/5; check Task Manager per-core load under a single-threaded load).
- No DVFS from Windows (no PEP/_CPC): clocks are fixed by UEFI (LITTLE 1.456 GHz, MID 1.794 GHz, notes/m15-perf.md).

## M4 big cores under KD (2026-10-14 ~21:05, user: "get the 2 last cpu cores running ... debug what you need")
- Known (notes/cpu-ram-plan-20261012.md): CPU_ON of both M4s works (they reach the park stub); offering either to
  Windows kills it before drivers load (runs 17/34). Linux: M4 MIDR 0x531f0030 lacks LRCPC (ISAR1[23:20] 0 vs 1) and
  16-bit VMID (MMFR1 VMIDBits 0 vs 2, HPDS 1 vs 2) relative to the A55 boot CPU. Never looked at under KD.
- Host tool firmware/tools/kdrun (dbgeng, x64): attaches net:port=50005,key=1.2.3.4, logs, diagnoses every break
  (!analyze -v on bugchecks, !running -it, !prcb per CPU, k, lm), timed break-in for hangs, "twrp" mode resets the
  phone into TWRP afterwards (S10eDiag hard-reset writes via !ed [uc]).
- Phone BCD: bcdedit /dbgsettings net hostip:169.254.255.255 port:50005 key:1.2.3.4, busparams 0, dhcp No, /debug on
  (set from Windows over SSH; was debug No, debugtype Local). Battery 35 %.
- Image s10e-artifacts/boot-m4kd1.img b4fe9ac8... = HEAD + build-only WINDOWS_WATCHDOG 0, GPU_POWER_ON 1,
  USB_KD_DEVICE 1 (DWC3 device role -> KDNET over USB EEM, no USB host), SMP_PARK_MASK 0xFE (SAFE mask 0x3E: the M4
  CPU_ONs run behind the 0x65 guard).
- Run m4kd1 (21:07, boot-m4kd1): user saw a boot loop "before reaching Windows". Next normal boot's UEFILOG: progress
  prev=0x72 (= ExitBootServices reached: Windows started) -> Windows died early and restarted itself. WINPRAM.BIN marks
  ring empty (S10eDiag never loaded). kdrun never saw a target (its first "connected" was a false positive: AttachKernel
  succeeds without one - fixed, 6ccf6a5). Either Windows died before KD init (~4 s) or KDNET-over-USB does not attach
  with today's firmware. Separate them: boot-kd6.img (7810a0bd...) = same KD image, park mask 0x3E (6 cores).
- Restored boot116-gpuon-nowdt-clean3 in between (TWRP, readback d85b7391 ok).
- Run kd6 (21:25, boot-kd6 = KD image, park mask 0x3E): UEFILOG gicc1-5 parked, gicc6/7 disabled; Windows booted and
  KD ATTACHED ("Connected to target 169.254.164.102 on port 50005", s10e-artifacts/runs/kd6/kd-raw.txt). => KDNET over
  USB works with today's firmware; m4kd1's failure is the M4s killing Windows before KD attached, not KD.
  (kdrun.exe's own dbgeng session in kd.log never saw the target: a second debugger - a kd.exe - held port 50005.)
- HOST FREEZE (21:35, user had to reboot the PC): that kd.exe wrote "kd: Could not write to pipe, 1450"
  (ERROR_NO_SYSTEM_RESOURCES) 163 771 times into kd-raw.txt (5.8 MB in ~7 min) - its stdout pipe reader was gone
  while Exynos9820Wlan DbgPrint traffic kept streaming. Likely freeze cause (INFERRED). Rule: one debugger on 50005
  at a time, never kd.exe with a piped stdout left orphaned; kdrun.exe (dbgeng, file log) only, killed at the end.
- 2026-10-14 21:45 session restart: phone in Windows (kd6, 6 cores, KD on; Wi-Fi not up per user), host shows the
  "Windows KDNET USB-EEM Network Adapter" Up (Ethernet 6).
- kdrun.exe validation (21:42, live kd6 Windows): 150 s, never saw the target. Its banner reads "Debugger Version
  10.0.26100.1" = the inbox C:\Windows\System32\dbgeng.dll (exe dir, then System32, before the cwd in the DLL search
  order), not the Kits 10.0.26100.6584 one that kd.exe uses and that connected in kd6. => m4kd1's "KD never attached"
  is UNPROVEN either way: kdrun may simply never have connected. Fix: load the Kits dbgeng.dll by full path.
  Kits kd.exe (60 s, file log only) also did not reconnect to the already-running target ("Waiting to reconnect...")
  - the target may be stuck after the orphaned kd.exe (user: Wi-Fi not working in this Windows session, possibly the
  WLAN driver's DbgPrint stuck on the dead debugger - INFERRED). A target reboot is needed to re-test.
- Commit 2b0ec94 (kdrun loads the Kits dbgeng by full path). REVIEW: verified mechanically - kdrun.exe imports only
  KERNEL32.dll (byte scan), a 15 s run prints "Debugger Version 10.0.26100.6584"; build-kdrun.cmd no longer links
  dbgeng.lib. Overclaim in the message: "never connected" is not proven to be the inbox engine's fault - the Kits
  kd.exe also failed to reconnect to that same (possibly stuck) target. Closes only on the next target boot.
- Commit f9caaf3 (kdrun initial-break commands file). REVIEW: the code comment "KDNET connects in phase-0 kernel init,
  before the secondary processors are started" is INFERRED (m17: connection at uptime 0:00:04 with 1 processor),
  not verified - run kd6b checks it (is the initial break reported with 1 processor?). The commit also carried the
  2b0ec94 review note (notes only).
- "No Wi-Fi in the KD boot" (user, 21:50): not KD - the phone's WLAN driver scans for S10eTest (kd6 kd-raw), the PC's
  Mobile Hotspot, which was Off after the PC reboot. User moved the phone to home Wi-Fi (phonestate needs WINIP
  set to its home-network address). kdrun kd6b (init break + twrp after 240 s) waiting for the next boot.
- Run kd6b (21:50-22:10, Kits engine 26100.6584, initial-break file, phone restarted over SSH 21:59, Windows back
  22:02): kdrun NEVER connected, no "Waiting to reconnect..." line (kd.exe prints it right after "Opened WinSock").
  => the engine's transport loop is not running under kdrun; suspect WaitForEvent failing (its HRESULT was ignored).
- ROOT CAUSE of kdrun never connecting: WaitForEvent(timeout 1000) returns 0x80004001 E_NOTIMPL with execution status
  7 (NO_DEBUGGEE) before a kernel target is connected - the KDNET transport never started ("Waiting to reconnect..."
  never printed). Fix: WaitForEvent(INFINITE) on the main thread + a timer thread doing SetInterrupt(ACTIVE) for the
  timed break-in and SetInterrupt(EXIT) for the time limit. Test 22:11 against the still-running kd6b Windows:
  "Waiting to reconnect..." then "Connected to target ... on port 50005" within 2 s - first kdrun connection ever.
  (KDNET then logged "target machine restarted without notifying the debugger" + reconnect: a reattach quirk.)
  => m4kd1's "KD never attached" says NOTHING about the M4s; the M4 run must be repeated with this kdrun.
- REVIEW 00332b5: (1) BUG - the timer could interrupt while a break was being handled (initial-break symbol load
  > break_after_s), leaving gBreakInPending set so the NEXT break (a breakpoint) counted as the timed one -> premature
  TWRP reset. Fixed in the follow-up (gInBreak). A narrow race remains (timer reads gInBreak 0, the target breaks
  before SetInterrupt) - accepted. (2) the 12 s limit exited after 29 s: SetInterrupt(EXIT) waited out a KDNET
  reconnect - harmless, noted. (3) diagnostics text unchanged.
- Run kd6c (22:13, live kd6 Windows, fixed kdrun): connected, initial break taken, init file ran, symbols
  (.reload) in ~1 s. Target: 22621.1 arm64fre ni_release, "MP (6 procs)". Start-path symbols (HAL is inside nt):
  nt!KeStartAllProcessors, nt!HalpInterruptStartProcessor, nt!HalpGicStartProcessor,
  nt!HalpInterruptStartProcessorPsci, nt!HalpInterruptGetParkingPageInformation (parking protocol),
  HalpHvStartProcessor, HalpBcm2836StartProcessor. A second break (80000003) came 3 s after the first (the engine's
  reconnect break) and got the plain diagnostics + go.
- kd6c end: timed break-in at +60 s, diagnostics, "!ed [uc]" hard-reset -> TWRP 22:15:14 (bat 34 %). The whole
  unattended chain (connect, initial break, symbols, timed break, TWRP reset) WORKS.
- M4 run prep (22:16): ESP \S10EDIAG\stay.flg -> stay.flg.m4off so a silent Windows death lands in TWRP (boot pair)
  instead of looping into Windows. RESTORE stay.flg after the M4 runs (user's interactive setup).
- Run m4kd2 prep (22:18): boot-m4kd1.img (b4fe9ac8, park mask 0xFE) flashed, readback b4fe9ac8 = image.
  kdrun 1500 s / break 300 s / twrp, init file s10e-artifacts/runs/m4kd2/init.txt: auto-continue logging bps
  ("M4BP ...") on KeStartAllProcessors (+ its return), HalpInterruptStartProcessor, HalpGicStartProcessor,
  HalpInterruptGetParkingPageInformation; stopping bp on KeBugCheckEx.
- m4kd2 attempt 1 (22:17) TESTED NOTHING: UEFILOG line 1267 "previous boot ran Windows - saving its PRAM, then
  recovery" - kdrun's twrp hard reset (kd6c) goes straight to recovery, so \WINRUN.FLG stayed on the ESP and the next
  UEFI boot took the boot pair (dump + recovery) without starting Windows. PARK lines in that boot: all 7
  secondaries parked incl. gicc6/7 M4 (mpidr_el1 80000100/80000101). Rule: after a kdrun twrp reset, the next boot
  is consumed by the boot pair (or delete \WINRUN.FLG from TWRP first). Also: kdrun hung after its twrp reset (held
  port 50005, had to be killed) - follow-up fix.
- m4kd2 attempt 2: WINRUN.FLG gone (cleared by that boot), adb reboot ~22:38, kdrun still listening (to ~22:42 +
  restarted below if needed).
- m4kd2 attempt 2 (boot-m4kd1, mask 0xFE, 22:36:52 reboot): KD CONNECTED 22:37:16 (24 s), initial break, bps set.
  KeStartAllProcessors (Phase1InitializationDiscard) -> HalStartNextProcessor -> HalpInterruptStartProcessor(x0=x1=
  n) -> HalpGicStartProcessor(x1=n) for n = 1..7 (all 7 secondaries incl. both M4s); HalpInterruptGetParking-
  PageInformation never hit (not on this path). KeStartAllProcessors RETURNED x0=1 at ~22:37:20. => the M4s do NOT
  kill the processor start; the death (runs 17/34/m4kd1) is later. m4kd1 "KD never attached" = the broken kdrun.
- m4kd2 attempt 2 result: after KeStartAllProcessors returned the only target output was "KDTARGET: Refreshing KD
  connection"; then the KD link was dead (timed break-in 22:42:16: "Failed to write breakin packet ... 0x2741, HOST
  cannot communicate with the TARGET"). KeBugCheckEx bp never hit. debug_history: SOFT reset INFORM3(12345670)
  ~5 min 34 s after the reboot (same spacing as the cycle before: fixed timer), then the boot pair (UEFILOG prev=0x72,
  "previous boot ran Windows - saving its PRAM, then recovery") -> TWRP.
  WINPRAM ring: storport UFS init + ~40 verified writes, "S10eDiag loaded" (t=30142), INFORM3 12345670, then NOTHING -
  no "keys: polling" (good 6-core boots log it ~14 s after "S10eDiag loaded", then "S10eAgent start"), no BUGCHECK
  callback mark. => all-system freeze (not a bugcheck) within ~14 s of S10eDiag loading, after all 8 CPUs started.
  Hypotheses: an M4 stops answering IPIs (TB flush / freeze IPIs then hang every CPU); an M4 takes an exception it
  cannot report. Next: periodic break-ins with !running -it to catch the 8-CPU state before/at the freeze.
- REVIEW 56638a8 (kdrun exit after TWRP reset): committed before its build ran (link failed: the m4kd2 kdrun held the exe); rebuilt after killing it - see next line.
- Run m4kd3 (22:45, boot-m4kd1, kdrun 'every' 10 s): KeStartAllProcessors returned (uptime 0:00:04.5), vertarget
  "MP (8 procs)"; !running -it: CPU0 in Phase1InitializationDiscard, CPUs 1-7 (incl. M4 6/7) idle, all in
  KiFreezeTargetExecution via KiIpiServiceRoutine => the freeze IPI reaches and is answered by both M4s.
  "M4 does not take IPIs" hypothesis OUT (at least at t+4 s).
- m4kd3 freeze bracket: break-ins at +16 s, +30 s, +43 s (22:46:08/22:46:22/22:46:35) all fine (8 CPUs idle or in
  DPCs; at 22:46:35 CPU0 in kdnic!RxReceiveIndicateDpc). Resume 22:46:38 -> next break-in 22:46:48 FAILED (link
  dead). The target printed NOTHING after the 22:46:38 resume. In flight around then: MaliG76Wddm start (GpuEnable 1
  FullAdapter 1, QueryInterface failures), Exynos9820Wlan firmware download + WDI init, Exynos9820Audio amp init.
  Silent all-system freeze while drivers load = load on the M4s. Suspects: M4 cluster DVFS (clock/voltage left by
  sboot, UEFI only sets LITTLE/MID) and M4-cluster coherency/snoop. kdrun 'every' BUG: a failed break-in never
  resumes, so gLastResume never advances and the timer fires every 0.5 s - fix next.
- BIG DVFS (22:55): TWRP cpufreq: cpu6/7 policy cur 2080000, ladder 2730000..520000 (21 steps), related 6-7; live DT
  cpufreq/domain@2 cal-id 0x0b040004 (idx 4), sibling-cpus "6-7", min-freq 520000, max-freq 2080000 (VERIFIED, TWRP
  root hexdump). No boot-freq property (sboot's BIG rate unknown; FREQ_GET returns 0 on this firmware, m15).
  UEFI never requested a BIG rate. Change: TouchPowerDxe FREQ_REQ list += idx 4 = 520000 kHz (lowest step = can only
  lower the M4 clock; ACPM sets the matching voltage). Test: if the 8-core freeze goes, the M4 V/F state was it.
- kdrun 'every' re-fire fix: the timer sets gLastResume at each fire, so a break-in to a dead target repeats every break_after_s, not every 500 ms.
- Run m4dvfs1 (22:51): boot-m4dvfs1.img ac667cf0 (= 2dba65a + KD flags as m4kd1: WATCHDOG 0, GPU_POWER_ON 1, USB_KD_DEVICE 1, PARK_MASK 0xFE), readback ok; kdrun every 15 s.
- Run m4dvfs1 (BIG FREQ_REQ 520 MHz): 8 CPUs started, break-ins +22 s, +41 s fine (later than m4kd3's freeze), then
  frozen by +59 s (22:53:20 "HOST cannot communicate"). Last target output: MaliG76Wddm bring-up (panel/restride
  values, RecommendMonitorModes, BrightnessQuery, repeated Fail_QueryInterface c00000bb). => BIG at its lowest V/F
  point does NOT fix it (DVFS hypothesis weakened; UEFILOG reply for idx 4 still to read). Both freezes coincide
  with the Mali KMD start (GPU_POWER_ON 1, ESP gpu\full.flg). New suspect: GPU + M4 (coherency / GPU work on M4).
- m4dvfs1 UEFILOG: FREQ_REQ idx 4 = 520000 -> reply 00080000 0007EF40 (echo = accepted). Back in TWRP 22:54:05. Next m4nogpu1: same image, ESP \S10EDIAG\nogpu.flg set (DSDT GPU0 hidden, no Mali KMD).
- Run m4nogpu1 (nogpu.flg, same 8-core image): NO Mali KMD at all, still frozen: break-ins +25 s, +49 s ok, dead by
  +75 s (22:56:04); last output Exynos9820Wlan WDI init. => GPU is NOT the trigger. Freeze ~45-75 s after KD connect
  in all three runs, during general driver load. Hypothesis (INFERRED, to verify): cross-cluster WFE wake-up - Windows
  ARM64 spin-waits in WFE expecting the exclusive-monitor clear (lock release) as wake event; if that event does not
  reach the separate M4 cluster, an M4 waiting at DISPATCH (IRQs masked by PMR) never wakes and everyone deadlocks on
  it. Linux masks such a defect with the arch-timer event stream (CNTKCTL_EL1.EVNTEN, on by default). Check first
  whether ntoskrnl writes CNTKCTL_EL1.
- ntoskrnl.exe (phone, 10 423 200 B, pulled read-only from TWRP) disassembled (llvm-objdump): the only CNTKCTL_EL1
  write is "mov x8,#3; msr CNTKCTL_EL1,x8" (EL0 counter access, EVNTEN 0; skipped only when [x14+0x18]==3) - Windows
  never runs the event stream. But the kernel has 1 WFE (KeStallWhileFrozen), 0 SEVL, 714 YIELD: spinlocks spin with
  YIELD, not WFE => WFE-wake hypothesis RULED OUT.
  Remaining picture: an M4 wedges (hardware-level: bus/power/attribute), then the other CPUs deadlock on IPIs to it
  and KD dies with them. Next (m4nogpu2): break-ins every 4 s to catch the window where KD cannot freeze CPU 6/7.
- Run m4nogpu2 (break-ins every 4 s): 8 good breaks, last at 22:59:45 (~61 s after connect), dead by 22:59:52.
  Last good !running -it: CPUs 0-5 idle (0 in kdnic DPC); CPU 6 (M4) in nt!MiCopyPage <- MiCopyOnWrite <-
  MiUserFault (page copy, user COW fault); CPU 7 (M4) in rdyboost!ST_STORE::StDmPageRemove <- SmStWorker (memory
  compression store). => the freeze comes when the M4s do page-sized memory work. Candidates: cache-geometry
  mismatch (CTR_EL0 line sizes / DCZID_EL0 block size read on the boot CPU and assumed everywhere), cache
  maintenance by VA on the M4, coherency. Next: TWRP dmesg "SANITY CHECK: Unexpected variation" (Linux cpufeature).
- MiCopyPage+0x324 (CPU 6's PC in the last good break; dbh: MiCopyPage RVA 0x44A090) disassembled: memmove(page) ->
  KeSystemFullyCacheCoherent+0x18 helper (cache clean by range) -> dsb sy -> `sys #0,c7,c1,#0` = IC IALLUIS (I-cache
  invalidate all, inner-shareable BROADCAST) -> dsb sy -> isb (PC). => the M4 was doing DVM broadcast maintenance.
  Hypothesis (INFERRED): DVM/snoop between the M4 cluster (CPUCL2) and the DSU cluster is not fully enabled; a DSB
  waiting for DVM-sync completion wedges a core in hardware -> IPI deadlock -> all-system freeze. Linux works, so
  either EL3 or a kernel PMUCAL "cluster on" sequence enables it there - find which.
- DVM test (park stub): BEYOND0LTE_PARK_DVM_TEST 1 - every parked core writes stage 1 -> IC IALLUIS; DSB SY; ISB ->
  stage 2 -> TLBI VMALLE1IS; DSB SY; ISB -> stage 3 -> alive mark; "PARK: CPU_ON ... dvm stage N" logs it. Stub
  0x90 B (llvm-objdump of the obj matches the source). Size guard now stops at the stage word (0xFD8).
  Image boot-m4dvm1.img (KD flags + mask 0xFE as m4kd1). A wedged core = stage 1/2, no mark, MADT-disabled.
- Run m4dvm1 (boot-m4dvm1 4c22575e, nogpu.flg): 8 procs after start => both M4s passed the stub DVM self-test
  (IC IALLUIS + TLBI VMALLE1IS + DSB complete on an M4 with MMU off; UEFILOG stages to confirm). Windows froze
  between +59 s and +72 s after KD connect (23:05:12 ok, 23:05:25 dead). Last good break: CPU6 (M4) CI!memcmp <-
  qsort <- CI!I_MapFileHashes (catalog parsing), CPU7 (M4) InterlockedExchange_acq in KiAcquirePrcbLocksForIsolation
  Unit <- KiQuantumEnd, CPU0 in PpmCheckStart/PpmUpdatePerformanceFeedback, rest idle. Plain work, no common op.
  Freeze windows after connect, all runs: m4kd3 43-56 s, m4dvfs1 41-59, m4nogpu1 49-75, m4nogpu2 61-68, m4dvm1 59-72.
  => looks TIME-triggered (~1 min), not workload/instruction-triggered. New suspect: something outside Windows
  (ACPM / EL3 CPU-PM) acting on the M4 cluster after ~60 s (e.g. cluster considered idle/unowned -> power or clock
  change while running). Research agent asked about CPU_INFORM / cpupm / flexpmu.
- BEYOND0LTE_SMP_OFFER_MASK (default = PARK_MASK, no change for normal builds): parked cores outside it keep
  spinning in the stub but are MADT-disabled. Test image boot-m4off1.img 43530959 = KD flags + PARK 0xFE + OFFER 0x3E:
  M4s CPU_ON'd and running (stub poll loop), Windows on 6 cores. Freeze ~1 min in => something outside Windows hits
  the running M4 cluster; no freeze => it is Windows running on the M4s.
- m4dvm1 UEFILOG: all 7 secondaries 'dvm stage 3' (M4 gicc6/7 too) - VERIFIED the broadcasts complete on the M4s in UEFI.
- Run m4off1 (boot-m4off1 43530959: M4s CPU_ON'd, running the stub poll loop, NOT offered; Windows 6 procs): alive
  past +92 s (every 8-core run was frozen by +75 s), Wi-Fi associates. => the M4 cluster merely running does NOT
  freeze the SoC; the freeze needs Windows running ON the M4s (time ~1 min).
  Next suspect (Windows side): 3 MADT efficiency classes with the M4s (A55 0 / A75 1 / M4 2) vs 2 in every working
  boot; Windows' hetero/PPM policy starts after boot (CPU0 was in PpmCheckStart in m4dvm1). Test m4eff1: EFF_BIG = 1.
- Research agent done (notes/m14-m4-dvm-research.md, LineageOS exynos9820 kernel a412cfaf). Leads: (1) TIMER - stock
  DT arch timer "use-clocksource-only; use-physical-timer", MCT does clockevents: Samsung Linux never uses generic
  timer compare/IRQs on any core, Windows ticks off it; Samsung bootloaders may not set CNTFRQ_EL0 (Exynos 990
  review). Our ntoskrnl timer init compares CNTFRQ_EL0 with a stored value (0x1404af078). (2) 9820-only dummy
  `tlbi vaale1is, xzr` after local TLBI VMALLE1 / IC IALLU (6 sites, CONFIG_SOC_EXYNOS9820). (3) LLC: Linux sends
  ACPM ch10 LLC_REGION_ALLOC / LLC_EN at boot. Ruled out/low: boot QoS, CPU_INFORM (CPU_SUSPEND only), EMA, WDT cl2
  (bootshim stops it), thermal.
- m4eff1 image f161ec88: 8-core KD image with EFF_BIG 1 (MADT decoded: uid 0-3 eff 0, uid 4-7 eff 1). Not yet run.
- CORRECTION m4off1: it ALSO froze, later - last good break-in 23:10:59 (+144 s after connect), dead by 23:11:15
  (break-ins failing from then until the kdrun limit), then the boot pair took it to TWRP. Windows used only 6 CPUs;
  the M4s only ran the park-stub poll loop (DC CIVAC + DSB SY on their mailbox line every ~0x2000 loops). 6-core
  boots without the M4s powered run for hours. => trigger = M4 cluster ON + its memory/maintenance traffic; more
  traffic (Windows on M4) -> ~1 min, stub polling -> ~2.5 min. Fits research lead 3: Samsung Linux keeps CPUCL2 out
  of LLC allocation (ACPM ch10 LLC_REGION_ALLOC LIT_MID_ALL) before LLC_EN. Next: send that from UEFI.
- Live DT exynos-sci (TWRP hexdump, VERIFIED): acpm-ipc-channel 0xa, use_init_llc_region 1, initial_llc_region 1,
  llc_enable 1. TouchPower (BEYOND0LTE_SCI_LLC_INIT 1, progress guard 0x67) at ReadyToBoot on ACPM ch 10: GET region,
  GET en, SET region LIT_MID_ALL (0x1042), SET LLC_EN 1 (0x1043), GET both again - logged. Image boot-m4llc1.img
  (KD flags, PARK 0xFE, EFF_BIG 2 = the real config, CNTFRQ logging).
- Run m4llc1 (boot-m4llc1 4c7d52c7, LLC region LIT_MID_ALL + LLC_EN at ReadyToBoot): KD connected 23:29:44 at uptime
  0:00:03.9 ("Initial Breakpoint requested") but the initial break NEVER arrived and no later break-in landed (no
  "HOST cannot" either - the link stayed up, the kernel never answered). Phone dark 20 min, no self-reset (KD image
  = watchdog off). => the LLC sequence as sent hangs (or deadlocks) the kernel in phase 0. UEFILOG replies needed
  (default region/enable values, SET errors). Needs a force restart (user).
- m4llc1 UEFILOG: CNTFRQ_EL0 = 26000000 on ALL cores incl. M4 (= boot CPU) -> CNTFRQ lead OUT. ACPM ch10 defaults:
  LLC_REGION 0 (DISABLE), LLC_EN 0 -> the LLC was OFF in every earlier run, so the freeze is not M4 LLC allocation.
  SETs accepted (GET after: region 1, en 1, err 0) and enabling the LLC hung Windows in phase 0 (LLC enable needs
  more than Linux's probe shows, or Windows' cache maintenance does not cover the LLC). LLC lead closed;
  BEYOND0LTE_SCI_LLC_INIT -> 0 (code kept).
- Run m4sci1 (boot-m4dvm1, kdrun every-break `!dd [uc] 1a000108 L5` + `!dd [uc] 1a000140 L1`; SCI offsets from
  LineageOS exynos-sci.c: ErrCnt 0x108, ErrAddrHi/Lo 0x10C/0x110, ErrStatHi/Lo 0x114/0x118): one read at the
  KeStartAllProcessors-return break: ErrCnt ffff3ce7, ErrAddr/Stat 0, PM_SCI_CTL 3fec8000. Froze within 10 s of
  that resume (earliest of all runs) - the KD [uc] mapping of SCI registers may itself be unsafe => run contaminated,
  instrument dropped.
- Next instrument: the park stub keeps a poll counter (+0xFC0) and CNTVCT ticks per 0x2000-iteration delay loop
  (+0xFC8) in its mailbox (DRAM). With M4s parked-not-offered, KD reads them per break: M4 clock (did FREQ_REQ
  520 MHz take? throttling?) and whether the M4 stops before the system freezes.
- Stub self-report built: +0xFC0 poll count, +0xFC8 CNTVCT ticks per 0x2000-iteration delay loop (objdump verified, stub 0xB4 B, guard now stops at 0xFC0). Image boot-m4self1.img = KD flags + PARK 0xFE + OFFER 0x3E (M4s parked, not offered) + BIG 520 MHz FREQ_REQ at ReadyToBoot.
- Run m4self1 (boot-m4self1 6eb76df9, M4s parked NOT offered, kdrun every 15 s with !dq [uc] ba245fc0/ba246fc0):
  loop ticks ~0xBB00-0xC040 (~48k ticks / 0x2000 iterations = ~230 ns per iteration: MMU-off stub fetches every
  instruction uncached from DRAM -> measures DRAM latency, NOT the core clock; the parked M4s generate continuous
  uncached DRAM traffic). Poll counters kept rising at every break (last good 23:59:20, +101 s); froze before
  23:59:35 (+116 s): break-ins stopped landing (no "HOST cannot" this time), phone back in TWRP by 00:08.
  => no sign that an M4 stops before the system does (15 s resolution).
- Next: not-offered cores sleep in WFI right after the alive mark (mailbox +0xFB8 flag set by the firmware) -
  M4 cluster powered with ZERO traffic. Freeze anyway => powering the M4 cluster is enough (EL3 / uH / ACPM);
  no freeze => the M4s' traffic.
- boot-m4wfi1.img: KD flags + PARK 0xFE + OFFER 0x3E + BEYOND0LTE_PARK_SLEEP_UNOFFERED 1 (M4s: alive mark, then DAIF masked + WFI loop; objdump verified).
- Run m4wfi1 (boot-m4wfi1 7295c459: M4s CPU_ON'd, alive mark, then DAIF masked + WFI forever; not offered; Windows
  6 procs): break-ins fine through 00:12:34 (+95 s after connect), then none landed (no "HOST cannot"); phone back in
  TWRP. => the M4 cluster merely POWERED, with ZERO M4 traffic, still freezes the SoC (~1.5-2.5 min); Windows load on
  the M4s only shortens it. Root cause is outside Windows and outside the M4s' own activity: something
  (EL3 / uH at EL2 / ACPM / hardware power-thermal) acting once CPUCL2 is up. (Monitor note: a failed break-in can
  show no error - watch for the ABSENCE of successful break-ins.)
- boot-m4soak1.img da5a41f9: KD flags + PARK 0xFE + OFFER 0x3E (M4s polling, not offered) + BEYOND0LTE_M4_UEFI_SOAK_S 240 (progress 0x68 at ReadyToBoot).
- m4soak1 INVALID: the early-UEFI dead-man watchdog (ArmPlatformLib W25: WDT cl0 82 s, INFORM3 = recovery) is only
  re-armed by BootLog's ReadyToBoot handler, which runs AFTER TouchPower's; the 240 s soak inside TouchPower's
  handler tripped it at ~82 s -> straight to TWRP (dark 00:28:13 -> TWRP 00:30:36). The soak boot never wrote a
  UEFILOG (flush comes after). Fix: the soak reloads WTCNT every second. Re-run as m4soak2.
- boot-m4soak2.img 60cbd050 (soak 240 s with WTCNT reloads).
- Run m4soak2 (boot-m4soak2 60cbd050: M4s parked+polling, not offered, 240 s UEFI soak with WDT reloads): UEFI
  SURVIVED the soak (KD connected 00:41:12 = 4 min 24 s after the reboot). Windows (6 procs) then froze between
  00:42:42 (+90 s after KD connect) and ~00:43:05. => the M4s were powered > 6 min, 4 of them in UEFI without harm;
  the freeze clock starts with WINDOWS (or our ExitBootServices handlers), not with M4 power-on. Something global
  that the Windows side does (driver, EBS handler, Windows platform setup) breaks once the M4 cluster is powered.
  Next: bisect - hide devices from Windows (ESP flags) / disable EBS handlers.
- Run m4noaud1 (00:48): boot-m4dvm1 (8 cores offered) + ESP nogpu.flg + noaudio.flg (GPU0 and audio hidden from Windows).
- Run m4noaud1 (boot-m4dvm1 8 cores, nogpu + noaudio, kdrun WITHOUT init file): KD connected at uptime 3.9 s,
  "Refreshing KD connection", link dead by the first break-in (+20 s); phone reset to TWRP within ~1 min.
  debug_history: new entry type "PP" SOFT INFORM3(88ee). Too early to say anything about audio. Note: run without
  the initial break dies EARLIER (no KD pauses).
- HYPOTHESIS (strong, testable): the GIC is GICv2 (GIC-400, Exynos9820.h VERIFIED); Windows takes each core's GIC
  CPU INTERFACE NUMBER from the MADT GICC GicId (ours: logical order 0-7, M4 = 6/7, never verified). Linux reads the
  real one from the banked GICD_ITARGETSR0 per CPU. If the M4s are wired to other interface numbers, Windows'
  TARGETED IPIs / SPI routing hit the wrong core or none -> requester waits forever = silent all-CPU freeze at a
  random time; BROADCAST IPIs (KD freeze, "all others") still work - matches every run. Test: each parked core
  reads its own GICD_ITARGETSR0 byte (GICD 0x10101000 + 0x800) in the stub, logged.
- boot-m4gic1.img: stub logs each parked core's GICD_ITARGETSR0 byte (its real CPU interface mask) vs MADT GicId (objdump: movz/movk 0x10101800, ldrb).
- m4gic1 result: GIC CPU interface masks (banked GICD_ITARGETSR0 read by each core): boot 01, gicc1-5 02/04/08/10/20
  (UEFILOG), M4 gicc6 0x40, gicc7 0x80 (mailbox +0xFB0, read via KD !dd [uc] ba245fb0/ba246fb0 from the running
  6-core Windows). == MADT GicId 6/7. GIC numbering hypothesis OUT.
- Live DT cpufreq/domain@2/dm-constraints (TWRP): mif-perf (dm-type 3 = MIF, const-type 0) table pairs (BIG kHz ->
  MIF min kHz): 2912000..2210000 -> 2093000, 2080000 and below -> 0. mif-skew: ect-name "dvfs_cpucl2" (table in the
  ECT blob, not in the DT), const-type 0. devfreq_mif cur 2093000 (max), ladder 421000..2093000. => no MIF minimum
  applies at BIG <= 2.08 GHz; skew content unknown (ECT). Not pursued further for now.
- Next structural test (m4off2): M4s CPU_ON'd, alive mark, then PSCI CPU_OFF (0x84000002) from the stub; UEFI
  checks AFFINITY_INFO; Windows 6 cores. Freeze => CPU_ON leaves something armed; none => the powered cluster.
- boot-m4cpuoff1.img: KD flags + PARK 0xFE + OFFER 0x3E + SLEEP_UNOFFERED 2 (M4s: alive mark, then PSCI CPU_OFF 0x84000002 from the stub; mailbox pointer saved in x19 across the SMC; objdump verified).
- Run m4cpuoff1 (boot-m4cpuoff1 8d46d4e5: M4s CPU_ON'd, alive mark, then PSCI CPU_OFF; UEFILOG "sleep 2" = CPU_OFF
  taken, no return): Windows 6 procs, break-ins fine to +90 s, then a CLEAN harness restart ("Shutdown occurred"
  01:00:27 = +95 s; next UEFI prev=0x80 -> torecovery). NEW READING of the not-offered runs: their "freeze" times
  (+95..160 s) coincide with the harness's restart after collect (no stay.flg) => maybe the RESTART/RESET path hangs
  while the M4 cluster is powered (KD drops at shutdown; SoC soft-reset minutes later by something else).
  The full 8-core runs died earlier (driver load) - possibly a second issue. Test: stay.flg back, M4s powered not
  offered, 10 min.
- Run m4wfi2 (boot-m4wfi1 7295c459, M4s powered asleep in WFI, not offered; stay.flg RESTORED = no harness restart):
  break-ins fine through 01:06:58 (+268 s) - far past every earlier not-offered "freeze" (+95..160 s). => those
  freezes were the harness RESTART hanging while the M4 cluster is powered, not a runtime freeze. Confirming next
  with a manual restart over SSH.
- !!! CORRECTION (01:09): the "not offered" runs m4off1 / m4self1 / m4wfi1 / m4soak2 did NOT freeze - their KD logs
  all contain "Shutdown occurred" (23:11:04 / 23:59:25 / 00:12:38 / 00:42:52) and no "HOST cannot": the harness
  (collect.cmd without stay.flg) restarted a HEALTHY Windows into TWRP. A manual `shutdown /r` in m4wfi2 also
  restarted cleanly. => RETRACTED: "M4 cluster merely powered freezes the SoC", "freeze clock starts with Windows",
  "external agent (EL3/uH/ACPM)". Powered-but-unused M4s are FINE.
  STANDING FACT: only runs where Windows RUNS ON the M4s die (m4kd2/kd3/dvfs1/nogpu1/nogpu2/dvm1/noaud1: no
  shutdown, "HOST cannot communicate"). Ruled out so far for that: GPU KMD, BIG 520 MHz DVFS, WFE wake (YIELD
  spinlocks), CNTFRQ (26 MHz everywhere), GIC CPU interface numbering (6/7), LLC (off; enabling it hangs earlier),
  DVM broadcast completing on the M4 (in UEFI). Last M4 PC before a death: MiCopyPage after IC IALLUIS + DSB.
- New lead: CTR_EL0 (cache type: IDC/DIC, IminLine/DminLine) and DCZID_EL0 differ between A55 and M4? Windows takes
  the boot CPU's values; Linux sanitizes system-wide. A boot CPU with IDC/DIC=1 or a larger line/ZVA size than the
  M4 would make Windows skip / under-do cache maintenance on the M4 (stale I-cache after code-page copies).
  Next: stub records CTR_EL0 + DCZID_EL0 per core.
- boot-m4ids1.img c7a72990: stub records MIDR/CTR/DCZID/ISAR0/ISAR1/MMFR0/1/2/PFR0/CLIDR per parked core (+0xF60), UEFI logs them next to the boot CPU's (KD rdmsr is not supported for ARM64 sysregs: 'no such msr').
- m4ids1 ID registers (UEFILOG via SSH, runs/m4ids1/UEFILOG.txt):
  boot/A55: MIDR 411FD050 CTR 84448004 DCZID 4 ISAR0 100010211120 ISAR1 100001 MMFR0 101122 MMFR1 10212122 MMFR2 1011
            PFR0 10112222 CLIDR 82000023
  A75:      MIDR 412FD0A1 ... MMFR0 101124 (PARange 44-bit) CLIDR C3000123, rest = A55
  M4:       MIDR 531F0030 CTR 84448004 DCZID 4 ISAR0 = A55, ISAR1 1 (LRCPC 0 vs 1), MMFR1 10211102 (VMIDBits 0 vs 2,
            HPDS 1 vs 2), MMFR2 1011, PFR0 10112222, CLIDR B200123
  => CTR_EL0 / DCZID identical: cache-geometry lead OUT. ISA gap: LRCPC (LDAPR) missing on the M4; HPDS2 missing.
