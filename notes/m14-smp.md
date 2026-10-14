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
