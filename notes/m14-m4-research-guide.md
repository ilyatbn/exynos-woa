# M14 - Mongoose (M4) research guide for the owner (2026-10-17)

Task: write docs/exynos9820-port/mongoose-research-guide.md - instructions for the repo owner to research Windows
internals HIMSELF (the agent may not, AGENTS.md rule 11). Read-only on the repo; no device access; no commit (the main
agent commits).

## Progress log

- Read: notes/m14-m4-summary.md, m14-m4-brief-other-model.md, m14-m4-dvm-research.md, m14-smp.md (all),
  psci-parking.md (A5.3, B inference, C1-C6), cpu-ram-plan-20261012.md, clocks-dvfs.md (efficiency classes),
  m17-usb-kd.md (KD bring-up), skipped-windows-internals.md, HANDOFF 42/43/44, MILESTONES grep.
- Facts pulled for the guide (all cited in the guide): Windows starts all 8 (KeStartAllProcessors -> ... ->
  HalpGicStartProcessor), dies 30-75 s later only when it RUNS work on an M4; no bugcheck; all cores stay powered
  (EDPRSR); busy cached M4 in the stub is harmless; stub flag 7 (interrupts + WFI on the M4) never run.
- s10e-artifacts/runs/m4ids1/UEFILOG.txt read for the M4 MMFR0 (not in the summary): M4 MMFR0 101122 = A55 (16-bit
  ASID, 40-bit PA); A75 101124. CLIDR decode (mine, from the raw values): A55 82000023 = L1 split + L2 unified,
  LoUIS 0, LoC 2, LoUU 0; M4 0B200123 = L1 split + L2 + L3 unified, LoUIS 1, LoC 3, LoUU 1. UNVERIFIED decode - the
  guide marks it.
- New lead noted from existing notes (inference, not tested): KeRestoreProcessorState writes ACTLR_EL1 = boot CPU's
  saved value (0) on every secondary (m14-smp.md m4noha1/m4noha2); ACTLR_EL1 is IMPLEMENTATION DEFINED on the M4 and
  Samsung's Linux never writes it (m14-m4-dvm-research.md). Its native value on the M4 has never been read.
- ACPI facts checked in source: Madt.aslc PMU GSIV 0 for all GICCs, EFF 0/1/2; Pptt.aslc 3 clusters by efficiency
  class, no cache nodes; no _LPI/_CPC in the DSDT; GTDT GSIVs 29/30/27/26.
- Also recorded: the 47-min freeze of 10-15/10-16 was our UFS driver's watchdog feed ceiling (notes/thermal-clocks.md),
  unrelated to the M4 - the guide warns not to confuse it.
- Guide NOT written: a safety classifier stopped the write of docs/exynos9820-port/mongoose-research-guide.md; the
  file does not exist. No entry added to notes/skipped-windows-internals.md. Decision on how to proceed is the user's.
