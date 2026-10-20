# M4 (Mongoose) cores — task list for the next agent

Goal (owner, 2026-10-19): Windows runs on all 8 cores, including the two M4 big cores. Milestone line:
`docs/exynos9820-port/MILESTONES.md` **G30** (stays `doing` until 8 cores run on the device).

## 0. Before you start

- Follow `AGENTS.md`. These rules matter most here:
  - **rule 2:** notes file + progress log, written as you go
  - **rule 3:** one commit per task, G30 line in the same commit
  - **rule 4:** harsh review of every commit; fixes go in their own commits
  - **rule 6:** no guessed constants
  - **rule 8:** nothing destructive on the phone
  - **rule 11:** no Windows binary inspection. The owner's researcher answers the Windows-side questions; see
    `docs/mongoose-research-guide.md` §2 and §5.
- This folder is a **copy** of the M4 research. The originals stay in `notes/` and `docs/exynos9820-port/`, and other
  files link to them. Write new findings to the **original** files, or to a new `notes/m4-*.md`.
- Read in this order:
  1. `notes/m4-cores.md`, the latest session. Read "State at end of 2026-10-19" first.
  2. `notes/m14-m4-summary.md`: what was established, what was ruled out, tools, pitfalls.
  3. `notes/m14-m4-brief-other-model.md`: the build / flash / KD recipe.
  4. `notes/m4-uh-public-research-20261020.md`: Samsung's EL2 hypervisor, from public write-ups.
  5. `notes/m14-smp.md`, only for run details.

## 1. Where it stands (2026-10-20)

**A newer distillation with firmware-RE findings and a device-test protocol lives in `findings.md`
(uH stage-2 = non-shareable DRAM 2-4GB lead; ring/hvc/split-map tests) — read it before T2.**

- Windows starts all 8 cores through the parking protocol. With **any** M4 offered it dies. This holds for 8 cores,
  for one M4, for M4s in the A75 efficiency class, and for A55 + M4 without the A75s.
- **The newest failure is kernel heap corruption, not only silent hangs.** In m4kd8 an A75 bugchecks in
  `RtlpHpLfhSubsegmentFreeBlock` about every 40 s. Shared kernel memory written with an M4 present comes out wrong.
  **The leading suspect is memory consistency between the M4 cluster and the A55/A75 (DSU) cluster.**
- **Ruled out on a bare-metal M4** (park stub next to a running 6-core Windows):
  - ACTLR_EL1 (0 natively on every core)
  - interrupts, timers, SGIs, WFI with IRQs on (m4irq1, 15+ min)
  - CNTVOFF, PMU, DVM broadcasts
  - cached ping-pong coherency
  - LSE + LL/SC atomics on one shared word, no lost updates (m4atom11)
  - hardware Access flag, prefetch into carve-outs, efficiency class, DVFS at 520 MHz
- **Not yet tested:**
  - ordering between *different* addresses (message passing, lock-protected data)
  - the stub running with **Windows' own** page-table attributes
  - which maintenance ops Samsung's EL2 hypervisor (uH) traps
  - uH's log ring
- **Pitfalls:**
  - Never peek CPUCL2 SYSREG 0x1D100104 or PMU 0x15861200-0x1650 from Windows; both froze the SoC.
  - Reading SCI registers over KD can hang the SoC.
  - Remove `WINRUN.FLG` before a test reboot.
  - Timed KD break-ins turn a hang into a reset loop.

## 2. Tasks (in order; each is one commit + its review)

### T1 — Bring the copied docs up to date
`docs/exynos9820-port/mongoose-research-guide.md` §1 and §4 are stale, and so is `notes/m14-m4-summary.md` §5:
- They say stub flag 7 must never run. It was run (m4irq1) and passed.
- ACTLR is still listed as open; m4-cores.md settled it (ACTLR_EL1 = 0 natively on every core).
- `m4eff1` is still listed as untested; it is covered by m4eff2 (dead).
- They say "no bugcheck"; m4kd8 bugchecks.

Fix the originals and re-copy them into `m4/`. Verify every claim with `rg` against `notes/m4-cores.md`.

### T2 — Verified cross-cluster memory-ordering tests (top lead)
In the park stub, between a not-offered A55 or A75 and both M4s, next to a 6-core Windows (the m4atom11 setup).
Each pattern runs a fixed number of rounds and then **checks the result**, with error counters in the mailbox and a
`PARK:` line in UEFILOG:
1. Message passing. The writer does data stores + `DMB ISHST` + flag `STLR`; the reader does flag `LDAR` + data
   loads. Count stale reads.
2. Lock-protected data: an `LDAXR/STLXR` spinlock around a multi-word non-atomic update (a linked list or a pair of
   counters that must stay equal). Check the invariant.
3. A page copy followed by `DC CVAU` / `IC IVAU` / `DSB ISH` / `ISB` on one core while another core reads the page
   (the MiCopyPage pattern, a last PC seen before an old death).
4. Each pattern in both directions (M4 as writer, M4 as reader), A55↔M4 and A75↔M4.

Any error → the cause is found, and next is the smallest pattern that fails. All clean → go on to T3.

### T3 — Same tests with Windows' memory attributes
The values come from the earlier KPRCB dump (m14-smp.md:596); read no Windows binary:
- TCR_EL1 = `0x695B5513511` (HA=1, HPD).
- MAIR_EL1 = `0x444400FF444400FF`: Attr0 = 0xFF (normal WB), Attr1 = 0x00 (Device-nGnRnE), Attr2/Attr3 = 0x44
  (**normal non-cacheable**).

Re-run T2 with the stub's identity map using the same MAIR, once with inner-shareable WB pages and once with
**normal-NC pages shared between the clusters**. Windows uses NC memory, and cross-cluster NC ordering has never been
tested. If T2 passed only with the stub's own attributes, this is where it breaks.
- The shareability bits Windows uses per mapping are not in the dump; they stay `UNVERIFIED` (rule 6). Test both
  inner and outer shareable.

### T4 — E1: does uH trap cache/TLB maintenance?
Bring back the m4trap1 timing method (stub flag 4, removed earlier; m4-cores.md:68, m14-smp.md:528-533). Time 1000×
each of `TLBI VMALLE1`, `TLBI VMALLE1IS`, `TLBI VAE1IS`, `TLBI ASIDE1IS`, `IC IALLU`, `IC IALLUIS`, `IC IVAU`,
`DC CIVAC`, `DC CISW` and an `ACTLR_EL1` self-write against the nop baseline. Run it on an A55, an A75 and an M4.
- A trap adds ≥ ~26 CNTVCT ticks per op.
- Result → `docs/exynos9820-port/mongoose-research-guide.md` §2b.

### T5 — E2: read uH's log ring
`0x87100000 + 0x40000` (UH_LOG_START in Samsung's 9820 header, notes/K10-rkp.md:271).
1. First a **single 8-byte read from UEFI**, logged before and after. A stage-2 fault is possible. The phone
   recovers (only `boot` is written), but say so in the notes before running.
2. If it is readable: dump the first 4 KiB as text to UEFILOG.
3. Then extend the not-offered A55 sampler (m4pcs1 pattern, gicc3, WINPRAM ring) to copy the ring every 0.25 s
   during a one-M4 Windows run.
4. Look for uH lines at the death.

### T6 — Restart point
Update `docs/exynos9820-port/HANDOFF.md` (new numbered section) and the G30 line with what T1-T5 found and the next
order of work, before the last commit of the session.

## 3. Not for the agent
- Everything in `docs/mongoose-research-guide.md` §2 questions 1-5 needs Windows internals. That is the owner's
  researcher's job (rule 11); log any urge to look in `notes/skipped-windows-internals.md` instead.
- Do not run the old `m4eff1` image or anything that writes partitions other than `boot`.

## 4. Files in this folder
- `findings.md`: firmware-RE distillation (uH/sboot, 2026-10-20) + ranked hypotheses + device test protocol P1-P6.
- `artifacts/`: RE dumps (disassemblies, indexes, decoded s2 maps) - **gitignored, local, regenerable** (see findings.md §0 + notes copy).
- `docs/mongoose-research-guide.md`: questions for the owner's researcher; §2b covers Samsung uH, §2c the firmware RE answers.
- `docs/research-requests.md`: other open questions for the researcher.
- `notes/m4-cores.md`: 2026-10-19 session (latest).
- `notes/m14-m4-summary.md`, `notes/m14-smp.md`, `notes/m14-m4-brief-other-model.md`, `notes/m14-m4-dvm-research.md`,
  `notes/m14-m4-research-guide.md`: 2026-10-14..16 sessions.
- `notes/m4-linux-research-20261019.md`: what Samsung's / LineageOS's kernel does for the M4.
- `notes/m4-uh-public-research-20261020.md`: Samsung EL2 hypervisor, from public write-ups.
- `notes/K10-rkp.md`: uH/RKP addresses on the 9820 (el2_code, UH_LOG_START).
- `notes/psci-parking.md`: the parking protocol background.
- `notes/cpu-ram-plan-20261012.md`: core + RAM plan, reserved memory.
- `notes/skipped-windows-internals.md`: the rule-11 log.
- `notes/s10e-debug-history-*.txt`: Samsung reset-history dumps.
