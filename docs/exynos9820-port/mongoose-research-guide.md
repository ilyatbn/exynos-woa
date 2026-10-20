# Mongoose (M4) cores — a research guide for you

Written 2026-10-17 at your request ("information on how I can do it and what to look for in windows"). The agent
does not inspect Windows binaries itself (AGENTS.md rule 11); this guide is for **you** to do that research on your
own device, and lists what to hand back so the firmware/ACPI side can act on it. Project facts below cite the notes
they come from; anything marked *inference* is unverified.

## 1. What already happened (short)

- **Bring-up works.** Secondaries start through the ACPI parking protocol (not PSCI). Windows starts all 8 cores
  (`KeStartAllProcessors` → `HalpInterruptStartProcessor` → `HalpGicStartProcessor` for n = 1..7), reports
  "MP (8 procs)", and both M4s answer the debugger's freeze IPI (notes/m14-m4-summary.md §1, notes/m14-smp.md ~250-272).
- **It dies once Windows runs work on an M4.** The system hangs ~30-75 s after the processor start, with one M4 or
  two. No bugcheck, no SError/FIQ reaches Windows, all cores still powered at death (m14-m4-summary.md §1).
- **What is NOT the cause.** M4s that are powered but not offered to Windows are fine, even when they run a busy,
  cached loop with broadcast cache/TLB maintenance (the "m4stress" stub); the GPU is not the trigger; a long list of
  other causes is already excluded (m14-m4-summary.md §2, m14-smp.md ~311-354).
- **Never run:** "stub flag 7" (stress + timer interrupt via the M4's GIC CPU interface + WFI with IRQs unmasked,
  m14-m4-summary.md:108) and the `m4eff1` image (both M4s as efficiency class 1, m14-smp.md:357-365).
- Don't confuse it with the 47-minute freezes of 10-15/10-16: that was our UFS driver's watchdog feed
  (notes/thermal-clocks.md), unrelated.

## 2. The open questions (most valuable first)

1. **What per-CPU state Windows writes on a secondary.** Windows restores the boot CPU's saved control registers
   (SCTLR/ACTLR/CPACR/TCR/MAIR) on every secondary (m14-smp.md "m4noha1/m4noha2", m14-m4-summary.md ~94-96). On the
   M4, ACTLR_EL1 is implementation-defined and Samsung's Linux never writes it (m14-m4-dvm-research.md). *Inference:*
   writing the A55's value (0) into the M4's ACTLR_EL1 may switch something off the M4 needs. Nobody has read the
   M4's native ACTLR_EL1 yet.
   - *Useful answer:* the full list of system registers Windows writes on a secondary and their values, in order;
     and the M4's own values before Windows touches it.
2. **Which local (non-broadcast) TLB / cache maintenance Windows does on a running core.** Samsung's 9820 kernel
   adds a "dummy" broadcast `TLBI VAALE1IS` after every local `TLBI VMALLE1` / `IC IALLU`, at 6 sites
   (m14-m4-dvm-research.md). The stub stress only used broadcast operations. *Inference (decode unverified):* the
   M4's CLIDR shows an L3 with LoC 3 / LoUU 1, where the A55 has LoC 2.
   - *Useful answer:* does Windows ever issue local-only TLBI/IC on context switch, ASID rollover or idle exit, and
     where (function name)?
3. **Per-core feature / errata / Spectre decisions.** A KPRCB byte (prcb-0x93A) has an extra bit 0x20 on the M4
   whose meaning is unknown; the M4 lacks LRCPC and HPDS2 compared with the boot CPU (m14-smp.md "m4spec1/m4ids1",
   m14-m4-summary.md §1).
   - *Useful answer:* which per-processor feature flags Windows computes from the ID registers, and which code paths
     they select (mitigation sequences, atomics, page-table features).
4. **Idle.** What the scheduler does on an idle M4 (WFI only? any other state?). Our ACPI has no `_LPI`/`_CPC`
   (DSDT), so Windows should only use WFI — worth confirming, because flag 7 (WFI with interrupts) was never tested.
5. **Heterogeneous policy.** What changes when an M4 is efficiency class 1 instead of 2 (MADT EFF field;
   `Madt.aslc`, `Pptt.aslc` - 3 clusters by class, no cache nodes). Hangs start after boot, when Windows' hetero/PPM
   policy kicks in (m14-smp.md ~357).

6. **Does Samsung's EL2 hypervisor (uH) catch the M4?** Windows runs at **EL1 under Samsung uH (EL2)**, which
   sboot loads from the `uh` partition before it jumps to our image (el2_code `0x87000000 + 0x200000`,
   notes/K10-rkp.md T16). See §2b. Two things are still unknown: which EL1 operations uH traps, and what uH does
   with an exception it does not handle.
   - *Useful answer:* uH's HCR_EL2 value(s), and what its EL2 vector table does in the default case (spin, reset
     or inject into EL1).

## 2b. Samsung's EL2 layer: what the public write-ups say (read 2026-10-20)

Sources: dayzerosec "Reversing Samsung's H-Arx hypervisor part 1", Longterm/Impalabs "A Samsung RKP Compendium",
Project Zero "Lifting the (Hyper) Visor", Phrack 70:10 "Hypervisor Necromancy", and Quarkslab "Samsung TrustZone
part 1". Full notes and URLs are in notes/m4-uh-public-research-20261020.md. **None of them mentions Mongoose,
PSCI/CPU_ON or ACTLR, so none answers the M4 question directly.** They do describe the layer between Windows and EL3:

- **Generation.** The S10 uses **uH**. H-Arx (dayzerosec) is the S20+ framework, so its addresses and API do not
  apply to us. The 9610 uH in the compendium has the same `0x87000000 + 0x200000` region and the same log address as
  our 9820. That makes it the closest public model of our uH (*inference*).
- **How uH handles an exception.** The EL2 sync handler dispatches HVC (EC 0x16), trapped MSR/MRS (EC 0x18, checked
  against an allow-list) and stage-2 aborts. Anything it rejects goes to `uh_panic`. The 9610 build then logs,
  copies the state to "bigdata" and calls `exynos_reset(0x8800)`. The older S8 vmm instead **spins at EL2**, and its
  unhandled vector slots are `b .` loops.
  - *Inference:* a core spinning at EL2 stays powered and no exception reaches Windows. It also stops answering
    IPIs, so the next TLB-flush or freeze IPI hangs every other CPU. That is exactly our passive two-M4 death
    (plain hang, never resets, every core PU=1). A 9610-style panic would reset the SoC, and we see no reset
    without KD. So if uH is involved, it is a spin or deadlock, not `uh_panic`.
- **Which bits are set (example).** The only HCR_EL2 value published is from the S8: `0x84000003` = RW | TVM |
  SWIO | VM. That build traps no ACTLR (TACR), IMPDEF registers (TIDCP), TLBI (TTLB), cache maintenance (TPU/TPCP/
  TSW), WFI/WFE or SMC. It does not route IRQ/FIQ/SError to EL2 (IMO/FMO/AMO). Our m4trap1 run agrees on TVM: it
  found no trap on TTBR0/TCR/SCTLR/MAIR/CONTEXTIDR writes. **m4trap1 never timed TLBI, IC/DC maintenance, ACTLR or
  WFI.** The 9810 port did see uH trap TLBI (notes/m10-windows-deploy-plan.md:70,136), and one of the last M4 PCs
  before a death was MiCopyPage right after `IC IALLUIS`. So on the 9820 this is open (experiment E1 below).
- **uH's log ring is at a fixed address: `0x87100000 + 0x40000`.** Samsung's 9820 kernel header hardcodes it as
  `UH_LOG_START` (notes/K10-rkp.md:271). On 9610 the region is read-only to EL1 after RKP starts. RKP never starts on
  our path, so whether EL1 can read it here is **UNVERIFIED**: a read could fault into uH. uH zeroes the ring at
  init, so it must be copied **before** the reset. Reading it after the reboot finds nothing (experiment E2).
- **Not relevant to the M4.** The ROPP key reload on secondary cores (`ropp_secondary_init`) and the RKP/KDP
  commands are only needed by Samsung's own kernel. LineageOS runs all 8 cores with `CONFIG_UH` off
  (notes/m4-linux-research-20261019.md:255).
- **The most direct answer would come from Samsung's uH binary**, not Windows. It is the `uh` entry in the BL tar,
  or the `uh` partition (sda10, read-only `dd` from TWRP). Format: GREENTEA magic and a 0x1000-byte header, per the
  compendium. What to look for:
  - every `msr hcr_el2` and the value written
  - the VBAR_EL2 table: which slots are `b .`, and what the sync handler's default case does
  - any MIDR check for implementer 0x53 ('S', Mongoose)
  - CPU power / PSCI hooks
  - the panic path: spin or reset

  This is Samsung firmware, so AGENTS.md rule 11 (Windows internals) does not cover it. Whether to analyse it is the
  owner's call.

## 3. How to research it yourself

**Tools (all public):** WinDbg (Store version) with the Microsoft public symbol server
(`srv*C:\symbols*https://msdl.microsoft.com/download/symbols`), the project's existing kernel-debug connection
(notes/m17-usb-kd.md has the working KD setup), the ACPI tables, and optionally a disassembler (Ghidra or IDA)
with public symbols.

**Watch it live first (safest, most useful):**
- Boot the 8-core KD image, break in early, and use public debugger commands: `!cpuinfo` (per-CPU ID registers as
  Windows sees them), `!prcb <n>` and `dt nt!_KPRCB` (per-processor state), `~<n>` + `r` (switch to processor n and
  dump its general registers), `!running -t` / `!thread` (what each CPU runs).
- **System registers:** KD's `rdmsr` does not read ARM64 system registers (m14-m4-summary.md §4). Use the values
  Windows saved in each processor's KPRCB (an earlier session dumped MIDR/SCTLR/ACTLR/CPACR/TCR from every KPRCB,
  m14-smp.md ~601). Or ask for a park-stub build that writes the core's own registers into its mailbox page before
  Windows starts; the mailbox map is in `Beyond0LteParkStub.S` (+0xF60.. ID registers). A mailbox page can be read
  from KD with `!dq [uc]`; for gicc n the pages have been at 0xBA240000 + (n-1)*0x1000 in every run so far.
- Repeat every few seconds until the hang. Record what the M4s run just before the last good break-in.
- Compare an A75 and an M4 side by side: anything set differently (feature bits, control registers) is a lead.

**Study the start path (only if the live view is not enough):**
- With public symbols loaded, the names already in our notes are the entry points to read: `KeStartAllProcessors`,
  `HalpInterruptStartProcessor`, `HalpGicStartProcessor`, `KeRestoreProcessorState`, and whatever processor-feature
  initialisation they call. The goal is to **write down the order of operations and which system registers are
  written with what**, not to change anything.
- Search the symbols for functions about processor features, errata, TLB flush and idle (names containing
  `Feature`, `Errata`, `Flush`, `Tb`, `Idle`), and note which ones branch on per-CPU flags.

**Tracing without a debugger:** Event Tracing for Windows with Windows Performance Recorder / Analyzer (processor
power, idle states, scheduling per CPU) shows *which* CPU was doing what in the last seconds before a hang, if you
can capture a trace to disk before it dies (a short circular buffer helps).

**ACPI side:** dump the live tables (`!acpicache` / `!acpitable` in WinDbg, or a table dump tool) and compare with
our sources (`Madt.aslc`, `Pptt.aslc`, DSDT). Confirm Windows parsed the M4 GICC entries as intended (MPIDR,
efficiency class, parking address).

## 4. Experiments you can run safely

Only the `boot` partition is ever written; recovery stays reachable (AGENTS.md rules 8-10). Each line: what to run,
what each outcome means.

| Experiment | If it survives | If it still hangs |
|---|---|---|
| `m4eff1` image (M4s as class 1) | the problem is in class-2 (big-core) policy, not the core | core-level problem |
| Offer one M4, and keep a KD break-in loop at 5 s | narrows the time window and shows the last code the M4 ran | — |
| Stub flag 7 (interrupts + WFI on the M4, Windows not involved) | WFI/IRQ on the M4 is fine; look at Windows-specific state | the firmware/GIC side is wrong for the M4, not Windows |
| Read the M4's native ACTLR_EL1 / CPUACTLR in the stub before Windows starts, and compare with what Windows leaves there | — | if they differ, test with the native value restored (by us, in firmware) |
| Pin a single test thread to an M4 (process affinity) after boot, with everything else idle | shows whether "any work" or specific work kills it | — |
| **E1 (new, §2b):** trap timing for maintenance ops. Bring back the m4trap1 method (stub flag 4, removed in b66429a8): 1000 × each of `TLBI VMALLE1`, `TLBI VMALLE1IS`, `TLBI VAE1IS`, `TLBI ASIDE1IS`, `IC IALLU`, `IC IALLUIS`, `IC IVAU`, `DC CIVAC`, `DC CISW`, `ACTLR_EL1` write-back of its own value, and `WFI` with a pending timer, against the nop baseline. Run on an A55, an A75 and an M4, all parked and not offered. | no op adds ≥ ~26 ticks on any core → uH does not intercept maintenance ops, and the uH hypothesis narrows to stage-2 faults | an op traps (any core) → uH handles Windows' maintenance. If it traps only on the M4, or the M4 never returns, that is the lead |
| **E2 (new, §2b):** copy uH's log ring `0x87100000 + 0x40000` from the not-offered A55 sampler (the m4pcs1 pattern, gicc3) into WINPRAM every 0.25 s during a two-M4 run; dump after the watchdog reset. First try a single read from UEFI and watch for a stage-2 abort. | ring readable and unchanged at the death → uH saw nothing (points to EL3 / hardware) | new uH lines at the death → they name the cause. A fault on the first read → the region is stage-2 unmapped and this path is closed |

## 5. What to hand back (template)

```
Experiment / image: ...
Time from M4 start to hang: ... s
Last good break-in: CPU n, thread/process ..., function ...
Per-CPU (A75 vs M4) differences seen: register / flag name = value (A75) vs value (M4)
System registers Windows writes on a secondary, in order: REG = value (where: function name)
Local (non-broadcast) TLBI / IC sites found: function names, when they run
Feature/errata flags that differ per CPU and the code they select: ...
ACPI as parsed by Windows (MADT/PPTT for the M4s): ...
Samsung uH (if analysed): HCR_EL2 value(s); EL2 vector default case (spin / reset / inject); Mongoose MIDR checks
Raw output: attach the WinDbg log
```

With this, the firmware side can (for example) set a different initial state on the M4s, present the cores
differently in ACPI, or add what Samsung's kernel does for the M4 — without the agent itself reading Windows code.

## 5b. Project tooling and pitfalls (read before any KD run)

- **Read first:** notes/m14-m4-summary.md (one page: facts, the ruled-out table, tools, pitfalls), then
  notes/m14-m4-brief-other-model.md (spec of the untested stub flag 7 and the build / flash / KD recipe), then
  notes/m14-smp.md from "## PC sampler" on.
- **KD runner:** `firmware/tools/kdrun/kdrun.exe <log> <total_s> <break_s> [reboot|twrp|go|every] [init-cmds]
  [per-break-cmds]`. Only one debugger at a time; the per-break command file is re-read at every break-in.
- **Test switches** are build-only defines (park / offer masks, stress / coherency / PC-sampler masks, real MPIDR,
  no-HW-AF). Each defaults to the old behaviour; m14-m4-summary.md §3 lists them with their files.
- **Pitfalls that cost hours before:**
  - Timed KD break-ins during the failing window turn the hang into a reset loop; use a passive run to see the real
    failure.
  - Without `stay.flg` the harness restarts a healthy Windows, which looks like a freeze. "Shutdown occurred" in the
    KD log means a clean restart.
  - A leftover `WINRUN.FLG` changes the next boot path.
  - Reading SCI registers over KD can hang the SoC.
- **Images already built:** the safe 6-core KD image `boot-kd6.img`; `m4eff1` (f161ec88, never run); `m4one1`
  (77225b39, one M4). All are in s10e-artifacts (m14-smp.md).

## 5c. Other data that would help, beyond the M4 hang

The owner's researcher can also answer the questions in [research-requests.md](research-requests.md) (graphics,
display wake, D3D9, x64 apps, Task Manager clocks). Use the same answer format as there.

## 6. Tool ideas (optional)

- **A WinDbg script** (`.foreach` over processors) that dumps the same set of per-CPU registers and KPRCB fields to
  a log at every break-in, so runs can be diffed.
- **A tiny kernel driver** (like our S10ePower) that pins one thread per CPU at boot and logs, once a second, a
  heartbeat and a few readable ID/control registers per core to a file or PRAM — the last heartbeat per core says
  which core stopped first.
- **An ACPI dump + diff** of the live tables against our `.aslc` sources.
