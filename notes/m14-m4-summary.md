# M4 big cores in Windows - state of the investigation (read this before touching the M4s again)

Goal: all 8 cores in Windows. Status (2026-10-15 09:00): **NOT working**. Default builds park 6 cores
(BEYOND0LTE_SMP_PARK_MASK 0x3E). Full run-by-run log: notes/m14-smp.md (sections from "M4 big cores under KD",
2026-10-14 21:05 on). Research: notes/m14-m4-dvm-research.md. Restart point: HANDOFF.md section 42.

## 1. What is established (with evidence)

- Windows STARTS all 8 cores: KeStartAllProcessors -> HalStartNextProcessor -> HalpInterruptStartProcessor ->
  HalpGicStartProcessor for n = 1..7, returns 1; "MP (8 procs)"; all 8 answer the KD freeze IPI (m4kd2, m4kd3).
- As soon as Windows RUNS WORK on an M4, the system HANGS within ~30-75 s of the processor start (one M4 = PARK 0x7E,
  or two). Last M4 PCs seen before deaths: MiCopyPage (after IC IALLUIS + DSB), rdyboost memory-compression store,
  CI!memcmp, KiQuantumEnd - ordinary work, no common instruction.
- The hang is NOT a Windows fault: stopping bps on KiAsyncSystemErrorException, KiFIQException,
  KiFatalExceptionHandler/Filter, HalpRebootNow, HalpReboot, HalReturnToFirmware, KiBugCheckDebugBreak, KeBugCheck2,
  WheaReportFatalHwErrorDeviceDriverEx never hit (m4one2). No S10eDiag bugcheck mark. => external (EL3 / uH / HW).
- Without KD break-ins a death is a plain HANG that never resets (m4dss1, sat dark 5 h until the user forced it).
  The one-M4 "fast reset loop" seen in m4one1/m4one2 was provoked by the timed break-ins - do not read it as a
  separate failure mode.
- M4s POWERED but NOT offered to Windows are fine (OFFER mask). The m4off1 / m4self1 / m4wfi1 / m4soak2 "freezes"
  were the HARNESS restarting a healthy Windows (KD log says "Shutdown occurred") - I misread them for an hour.
- M4 hardware checks that PASS in UEFI (MMU off unless noted): CPU_ON/park, CPU_OFF, IC IALLUIS / TLBI VMALLE1IS + DSB,
  CACHED coherency ping-pong with the boot A55 (100000 round trips, 0 misses, ~13 us), GIC CPU interface = 6/7 (banked
  ITARGETSR0), CNTFRQ 26 MHz, no EL2 trap on TTBR0/TCR/SCTLR/MAIR/CONTEXTIDR writes, 4 min soak in UEFI.
- ID registers: M4 MIDR 531F0030; CTR_EL0 84448004 and DCZID 4 IDENTICAL to A55/A75; M4 lacks LRCPC (ISAR1 1 vs 100001)
  and HPDS2 (MMFR1 10211102 vs 10212122; VMIDBits 0). ntoskrnl executes no LDAPR (only a decoder constant).

## 2. Ruled out (do not retry without a new reason)

| Hypothesis | Run / evidence |
|---|---|
| GPU KMD (Mali) | m4nogpu1/2 (nogpu.flg): still dies |
| Audio driver | m4noaud1 (noaudio.flg): died early, inconclusive but no sign |
| BIG clock/voltage | m4dvfs1: ACPM FREQ_REQ idx 4 = 520 MHz accepted (echo 0x7EF40), still dies |
| WFE wake across clusters | ntoskrnl spins with YIELD (714), 1 WFE in KeStallWhileFrozen, CNTKCTL_EL1 = 3 |
| CNTFRQ unset on M4 | m4llc1 log: 26 MHz on every core |
| GIC numbering | m4gic1: M4 masks 0x40/0x80 = MADT GicId 6/7 |
| LLC (SCI) | ACPM default region 0 / en 0 (LLC OFF); enabling like Linux hangs Windows at KD init (m4llc1) |
| Cache geometry | CTR/DCZID identical (m4ids1) |
| Coherency / snoop | m4coh1 ping-pong 0 misses |
| DVM broadcasts | m4dvm1 stub stage 3 on all cores |
| "Powered cluster is enough" / external timer | retracted - harness restarts (see above); m4wfi2 ran 5 min |
| Prefetch into protected carve-outs | m4guard1: 17 x 2 MiB guards at every carve-out / bank edge, still dies |
| Spectre SMC exception front ends | m4spec1: KeArm64VectorBase = plain KiArm64ExceptionVectors; KiSpeculationPsciState 1 |
| uH trapping EL1 sysreg writes | m4trap1: no trap overhead on any core |
| MPIDR collision A55#1 vs M4#0 | run 34 (REAL_MPIDR): still dies |

## 3. Tools and switches (all committed; every switch defaults to the old behaviour)

- firmware/tools/kdrun/kdrun.exe `kdrun <log> <total_s> <break_s> [reboot|twrp|go|every] [init-cmds|""] [per-break-cmds]`
  (loads the Kits dbgeng by path; INFINITE wait + timer thread; shared log; init file runs at the initial break;
  'every' repeats break-ins; per-break file is re-read each break, so it can be edited live; twrp = S10eDiag
  hard-reset writes then exit). Run it from any dir; only ONE debugger on port 50005 at a time.
- tools/phone/watchall.sh [kd.log]: one line per change (adb / Windows ping (20 s debounce) / SSH / KD break-ins and
  their ABSENCE). Address in gitignored local/phone.env (WINIP=...). Keep it running as a Monitor (user rule).
- Firmware test switches (build-only: sed them in, build, pack, `git checkout -- firmware/Platform`):
  - AcpiTables/Beyond0LteAcpiHeader.h: BEYOND0LTE_SMP_PARK_MASK (0x3E default; 0xFE = 8 cores, 0x7E = one M4),
    BEYOND0LTE_SMP_OFFER_MASK (park but MADT-disable).
  - Beyond0LteParking.c: BEYOND0LTE_PARK_SLEEP_UNOFFERED (1 WFI, 2 PSCI CPU_OFF), BEYOND0LTE_PARK_COHERENCY_MASK,
    BEYOND0LTE_PARK_TRAPTIME_MASK, BEYOND0LTE_SMP_REAL_MPIDR. The stub always logs DVM stage, CNTFRQ, GIC mask, ID regs,
    poll counter (+0xFC0) and loop ticks (+0xFC8) - see the mailbox map in Beyond0LteParkStub.S.
  - Beyond0LteTouchPowerDxe.c: BIG FREQ_REQ 520 MHz (ON by default since 2dba65a), BEYOND0LTE_SCI_LLC_INIT (0),
    BEYOND0LTE_M4_UEFI_SOAK_S (0).
  - PlatformMemoryMapLib.c: BEYOND0LTE_MEM_GUARD (0). BootLogDxe: BEYOND0LTE_DSS_DUMP (0; dumps log_s2d/arraydump
    heads after a death on the stay.flg path - never got a chance to run, the hang does not reboot).
- KD image flags used for every run: BEYOND0LTE_WINDOWS_WATCHDOG 0, GPU_POWER_ON 1, USB_KD_DEVICE 1. BCD debug ON
  (net hostip 169.254.255.255 port 50005 key 1.2.3.4, busparams 0). Safe image: s10e-artifacts/boot-kd6.img (6 cores).

## 4. Pitfalls that cost time (avoid)

- \WINRUN.FLG left by a kdrun/S10eDiag hard reset or a key-combo TWRP entry makes the next boot take the boot pair
  (dump + recovery) without starting Windows - `rm /cache/WINRUN.FLG` before every test reboot.
- Without \S10EDIAG\stay.flg the harness restarts a healthy Windows after collect (~95-160 s) - it LOOKS like a freeze.
  Check the KD log: "Shutdown occurred" = clean restart; "HOST cannot communicate" / silence = real death.
- Timed KD break-ins during the failing window turn the hang into a reset loop - use a passive kdrun (go, long
  break_s) when the failure mode itself matters.
- KD reads of SCI registers with `!dd [uc]` may themselves hang the SoC (m4sci1) - avoid.
- KD `rdmsr` does not support ARM64 system registers; read sysregs in the park stub instead.
- ParkRd32/ParkRd64 clean+invalidate the line before reading - never use them in a coherency test.
- The early-UEFI dead-man WDT (82 s, recovery) is only re-armed by BootLog's ReadyToBoot handler: any long work in an
  earlier ReadyToBoot handler must reload WTCNT.
- Read UEFILOG from a running Windows over SSH (`type S:\UEFILOG.TXT`) instead of waiting for TWRP. The not-offered
  branch of the park log does not print every field - read the mailbox via KD `!dd [uc] ba24Nfxx` if needed
  (pages: gicc n at 0xBA240000 + (n-1)*0x1000 in every run so far).
- Python heredocs through Git Bash mangle `\\n` / `\\` - use the Edit tool for C string literals.
- Never record MAC/IP addresses in notes or commits (user rule).

## 5. Next ideas (untried)

1. Get EL3's view of the hang: map the debug-snapshot region (0xFD900000.., carved out, not mapped by UEFI) and dump it
   on the boot after a FORCE restart; or make the hang reset (PMIC/sboot watchdog left armed) so BEYOND0LTE_DSS_DUMP runs.
2. Windows EL1 settings derived from the boot CPU's ID registers that the M4 lacks: TCR_EL1 HPD/HWU (HPDS2), hardware
   AF/DBM (both report HAFDBS 2), ASID/VMID width. Find where ntoskrnl sets TCR_EL1/SCTLR_EL1 and what it writes.
3. Run the M4 with a reduced Windows workload to find the trigger (e.g. a pinned test thread doing page copies on CPU 6
   while everything else is affinitized away).
