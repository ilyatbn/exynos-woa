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
