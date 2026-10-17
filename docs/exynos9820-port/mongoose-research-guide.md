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

## 3. How to research it yourself

**Tools (all public):** WinDbg (Store version) with the Microsoft public symbol server
(`srv*C:\symbols*https://msdl.microsoft.com/download/symbols`), the project's existing kernel-debug connection
(notes/m17-usb-kd.md has the working KD setup), the ACPI tables, and optionally a disassembler (Ghidra or IDA)
with public symbols.

**Watch it live first (safest, most useful):**
- Boot the 8-core KD image, break in early, and use public debugger commands: `!cpuinfo` (per-CPU ID registers as
  Windows sees them), `!prcb <n>` and `dt nt!_KPRCB` (per-processor state), `~<n>` + `r` (switch to processor n and
  dump its registers, including system registers WinDbg can show), `!running -t` / `!thread` (what each CPU runs).
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
Raw output: attach the WinDbg log
```

With this, the firmware side can (for example) set a different initial state on the M4s, present the cores
differently in ACPI, or add what Samsung's kernel does for the M4 — without the agent itself reading Windows code.

## 6. Tool ideas (optional)

- **A WinDbg script** (`.foreach` over processors) that dumps the same set of per-CPU registers and KPRCB fields to
  a log at every break-in, so runs can be diffed.
- **A tiny kernel driver** (like our S10ePower) that pins one thread per CPU at boot and logs, once a second, a
  heartbeat and a few readable ID/control registers per core to a file or PRAM — the last heartbeat per core says
  which core stopped first.
- **An ACPI dump + diff** of the live tables against our `.aslc` sources.
