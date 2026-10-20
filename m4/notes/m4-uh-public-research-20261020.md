# M4 hang vs. Samsung uH/H-Arx — public write-ups (2026-10-20)

Task: read two public articles (and their references) on Samsung's EL2 hypervisor and decide whether
anything helps the M4 (Mongoose) hang; update docs/exynos9820-port/mongoose-research-guide.md.
Sources are public blog posts; no Samsung or Windows binaries were inspected by the agent.

## Progress log

### 1. dayzerosec "Reversing Samsung's H-Arx hypervisor, part 1" (2025-03-08)
URL: https://dayzerosec.com/blog/2025/03/08/reversing-samsungs-h-arx-hypervisor-part-1.html
- Device: Galaxy S23 (Exynos 2200 era). H-Arx is the S20+ framework; **the S10 (9820) uses the older uH**
  (Longterm article states "S20 and later use H-Arx"). So H-Arx specifics are not directly ours.
- H-Arx core + uH plugin both at EL2, non-secure. sboot loads HARX to 0xC0000000 and inits via SMC 0x82000480;
  uH plugin at 0xC1400000 via HVC 0xC6000010. (S23 addresses, not 9820.)
- Core->plugin API includes `set_hcr_el2`, `register_pm_handler`, `register_sync_exception_handler`,
  `sw_reset`, `invoke_smc`. Shows the hypervisor has a **power-management hook** (CPU on/off/suspend) and can
  reset the SoC itself. No register values given.
- HVC ranges: uH 0x83000000-0x8300FFFF (S23). Per-CPU save area in RW data.
- NOT covered: Mongoose, HCR_EL2 bits, ACTLR, stage-2 details, PSCI, TLB/cache maintenance.
- References: Impalabs/Longterm RKP compendium, Quarkslab S6 SBOOT part 1, Quarkslab Samsung TrustZone part 1,
  S23FE kernel uh.h/rkp.h/kdp.h, Knox HDM blog, dayzerosec HARX loader.

### 2. Longterm/Impalabs "A Samsung RKP Compendium" (A51 / exynos9610 uH) — first 100k chars
URL: https://www.longterm.io/samsung_rkp.html (same text: https://blog.impalabs.com/2101_samsung-rkp-compendium.html)
- Kernel at EL1, uH at EL2, ATF-based monitor at EL3; sboot loads `uh` partition (GREENTEA magic) to
  0x86FFF000, entry 0x87000000, max 0x200000. **9820 live DTB has the same el2_code 0x87000000+0x200000**
  (notes/K10-rkp.md T16) -> 9820 uH is the same generation as the 9610 sample. (Inference, strong.)
- uH config: SCTLR_EL2 C/I/WXN/M; HCR_EL2.TVM set (EL1 VM-register writes trap), HCR_EL2.VM set in s2_enable;
  VTCR T0SZ=24, SL0=1, 4K, 40-bit PA.
  - Our m4trap1 run measured **no trap** on TTBR0/TCR/SCTLR/MAIR/CONTEXTIDR writes (m14-smp.md:528-533) ->
    on our boot path TVM is evidently not set (no RKP_START is ever issued). Consistent.
- EL2 sync handler: EC 0x16 HVC, **EC 0x18 trapped MSR/MRS checked against an allow-list**
  (`other_msr_mrs_system`), stage-2 aborts. **Any disallowed / unhandled case goes to `uh_panic`: logs state,
  copies it to "bigdata", then `exynos_reset(0x8800)`** — i.e. a SoC reset with no EL1 bugcheck.
- **uH log ring: 0x87100000, 0x40000 bytes; RW in EL2 stage 1, read-only (ROX) to EL1 in stage 2.**
  9820 value verified in Samsung's header (K10-rkp.md:271). Bigdata page 0x870FF000 on 9610 (9820 UNVERIFIED).
- Per-CPU arrays sized for 8 CPUs, indexed by get_current_cpu().
- NOT covered: Mongoose, PSCI/hotplug, ACTLR.

### 3. Remaining Longterm text (offset 100k+)
- `uh_log(level,...)`: level 'D' -> `uh_panic()`. Fixed bug: `check_kernel_input` logged at 'L' and kept going.
- Secondary cores: Samsung's kernel `__secondary_switched` -> `ropp_secondary_init` -> uH call (RKP 0x10) to
  reload the ROPP key into DBGBVR5_EL1. Only with CONFIG_RKP/ROPP; LineageOS runs 8 cores without it
  (m4-linux-research-20261019.md:255) -> not needed for the M4.
- Refs: P0 2017 "Lifting the (Hyper) Visor", P0 issues 970-984 / 1041, Phrack 70:10 "Hypervisor Necromancy",
  BH USA 2017 KNOX, POC 2016/2019 x82.

### 4. Project Zero "Lifting the (Hyper) Visor" (S7 Edge SM-G935F, 2017)
URL: https://projectzero.google/2017/02/lifting-hyper-visor-bypassing-samsungs.html
- HCR_EL2.TVM set (TRVM not); TTBR writes trapped and re-verified; TCR/SCTLR not intercepted then (locked later).
- Unmapped stage-2 access -> translation fault -> crash + reboot. No PSCI/SMP/Mongoose content.

### 5. Phrack 70:10 "Hypervisor Necromancy" (S8+ G955F vmm.elf, QEMU emulation), first 200k chars
URLs: https://phrack.org/issues/70/10 , https://www.exploit-db.com/papers/48073
- Logged **hcr_el2 = 0x84000003** = RW(31) | TVM(26) | SWIO(1) | VM(0). **No** TACR, TIDCP, TSC, TWI/TWE, TTLB,
  TPU/TPC, FB, AMO/IMO/FMO in that build. (S8 / 2017 vmm, not the 9820 uH - shape only.)
- sctlr_el2 0x30CD1835, vtcr_el2 0x80023F58. MSR handler refuses MMU-off / endianness / granule changes.
- `vmm_panic()` / `rkp_policy_violation()` = **hang at EL2** (old vmm); unhandled vectors are `b .` loops.
  Contrast 9610 uH (Longterm): uh_panic -> exynos_reset(0x8800).
- No SMP / PSCI content (single-core emulation).

### 6. Quarkslab "Deep dive into Samsung's TrustZone part 1"
- EL3 = ATF-based, in sboot; Samsung runtime services by OEN; encrypted monitor from S8 on. S10 uses TEEGRIS.
  No PSCI / SCR_EL3 / CPU-power content. Nothing for the M4.

## Assessment (what helps the M4 work, and what does not)

Nothing in these sources mentions Mongoose, PSCI/CPU_ON, ACTLR/CPUACTLR or per-core-type handling. No direct
answer. What they DO give is the shape of the layer sitting between Windows (EL1) and EL3 on this phone:

1. **Silent EL2 death is a real mechanism.** Unhandled EL2 exceptions are `b .` (S8) and panics spin (S8) or reset
   (9610). A core spinning at EL2 = powered (EDPRSR PU=1), no exception reaches Windows, stops answering IPIs ->
   every other CPU hangs in the next TB-flush/freeze IPI. That matches the passive two-M4 death exactly
   (m14-m4-summary.md §1: plain hang, never resets, all cores PU=1). INFERENCE, untested. The 9610-style
   reset-on-panic would NOT match (we see no reset without KD) -> if it is uH, it is a spin/deadlock, not uh_panic.
2. **Which EL1 ops go to EL2 decides what can trigger it.** m4trap1 timed only TTBR0/TCR/SCTLR/MAIR/CONTEXTIDR
   writes. It never timed **TLBI (HCR_EL2.TTLB), IC/DC maintenance (TPU/TPCP/TSW), ACTLR (TACR), IMPDEF regs
   (TIDCP), WFI/WFE (TWI/TWE), SMC (TSC)**. The 9810 project claimed "uH traps / cannot emulate an MMU-off tlbi"
   (m10-windows-deploy-plan.md:70,136) - i.e. TTLB on that uH build - but the 9820 MMU-off-tlbi theory was later
   refuted (m10:502). So whether the 9820 uH traps TLBI / IC is OPEN, and the last M4 PCs include
   MiCopyPage right after IC IALLUIS + DSB. -> new experiment E1 (trap timing for maintenance ops, per core class).
3. **uH keeps a log ring the kernel can read: 0x87100000 + 0x40000** (9820 header UH_LOG_START, K10-rkp.md:271).
   9610: RW at EL2, RO to EL1 in stage 2 after RKP start; on our path RKP never starts, so EL1 readability is
   UNVERIFIED (a read could take a stage-2 fault to uH). uH zeroes the region at init -> it must be copied while
   the system is alive/hung (non-offered A55 sampler -> WINPRAM, the m4pcs1 pattern), not read after the reset.
   -> experiment E2.
4. HCR_EL2 shape of the S8 build (no TACR/TIDCP/AMO/IMO/FMO) suggests ACTLR / IMPDEF writes and SError/IRQ/FIQ are
   NOT routed to uH. If the 9820 uH is the same shape, the ACTLR question in the guide stays a core question, not
   a uH one. Weak (different build).
5. Static analysis of the 9820 **uH binary itself** (Samsung firmware, `uh` partition sda10 / BL tar `uh` entry,
   GREENTEA header, 0x1000 header, maybe LZ4) would answer 2 and 1 directly: HCR_EL2 value(s), VBAR_EL2 vectors
   (which ECs it handles, what the default case does), any MIDR implementer 0x53 ('S') check, PM/CPU_ON hooks.
   This is NOT Windows internals (rule 11 does not apply) - but it is the owner's call; offered, not done.

## Review of 6d539cbf (harsh pass)
- Checked: m4trap1 ops + 26-tick threshold (m14-smp.md:528-533) OK; sda10 = uh (dock-desktop.md:530) OK;
  UH_LOG_START (K10-rkp.md:271) OK; MiCopyPage after IC IALLUIS (m14-m4-summary.md §1) OK.
- FOUND 1: guide cited commit b66429a8 (copied from m4-cores.md:68) - `git cat-file` says not a valid object in
  this repo. Fixed: cite notes/m4-cores.md:68 instead.
- FOUND 2: E1 listed "WFI with a pending timer" - its wake latency swamps a trap cost; not measurable. Removed.
- FOUND 3: "9810 port did see uH trap TLBI" overstated. Source (Star2LtePkg PBML.c:2337-2343) says uH emulates
  MMU-on TLBI and HANGS on an MMU-off one; but on the 9820 the m4dvm1 stub ran MMU-off TLBI VMALLE1IS on all cores
  without a hang (m14-m4-summary.md §1). Reworded; the hang-on-unhandled-case is noted as a second example of a
  silent uH stop.
