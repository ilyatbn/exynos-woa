# M4 findings — what the Samsung firmware itself says (2026-10-20 firmware-RE session)

Distillation of the HHXJ6 bootloader RE (uH EL2 hypervisor + sboot BL2 + cm/TZ), for the agent that
tests on the device. Raw session log: `notes/m4-fw-re-20261020.md` (copy: `m4/notes/`). All RE dumps
(disassemblies, string/sysreg indexes, decoded stage-2 maps) are under `m4/artifacts/` — **gitignored,
local only**, and every one regenerates with `tools/dump-tools/{disasm,pagedump,adrpindex}.py` (see the
session note's artifact-policy entry). Nothing on the phone was touched producing this; everything below
is static evidence from the shipped firmware until a test says otherwise.

## 1. Facts (observed in the binaries)

| # | Fact | Evidence |
|---|---|---|
| F1 | `uh.bin` = GREENTEA header (0x1000) + AArch64 ELF, one RWE PT_LOAD at **PA 0x87000000**, sections .text/.rodata/.data/.bss/.stack; the ELF is stripped but small (328 procs). | m4/artifacts/uh*.txt |
| F2 | uH init runs on **every boot**: sboot loads+verifies uh (0x200000 window), enters it at EL2, uH SMCs back (`0xC2000401`), then sboot issues **HVC 0xC300C000 (uh.h APP_INIT)**; only then continues to the host. Our Windows path goes through exactly this. | sboot 0x103540+, 0x15c5b0 |
| F3 | uH boot init: `TTBR0_EL2=0x87021000` (EL2 s1 on, MAIR_EL2=0x4400ff, TCR 0x23f18, M/C/I set), **`VTTBR_EL2=0x87028000`, `VTCR_EL2=0x23f58`, `HCR_EL2 |= 1` (VM)** — stage-2 translation ON from boot. | uh 0x87012d00/0x87013fa8/0x87019190 |
| F4 | **HCR_EL2 has exactly 2 write sites in the whole image: `|= VM` and `&= ~VM` (the latter in a function nothing references).** uH never sets TVM/TTLB/TPU/TPCP/TSW/TACR/TIDCP/TWI/TWE/TSC/FB/AMO/IMO/FMO. Consistent with device m4trap1 (no TVM traps) and with stubs running TLBI/IC ~1 kHz for 15 min unhindered. | uh.sysreg.tsv + disasm 0x870191c4-238 |
| F5 | **Static stage-2 map** (decoded, m4/artifacts/uh-stage2-map.txt): identity PA 0-4GB + `sec_rdx_bootdev` window PA 0x880000000 (matches the DTB reservation). DRAM 2MiB blocks = `0x4fd` = **Normal-WB, RW, SH=Non-shareable**; peripheral window PA 128M-1G = `0x4cd` Normal-NC NS; uH window + **the whole log ring** IPA 0x87000000..0x87200000 = L3 4K pages `0x47f` = WB, **guest RO**, AF pre-set. IPA 4-34GB is UNMAPPED in the static table. | uh.bin .data @0x87028000 |
| F6 | EL1 faults on unmapped IPA are handled **on demand**: EC 0x20/0x24 path replays with AT S12E1R/W, emulates the faulting LDR/STR, inserts s2 entries (helpers 0x87015280/0x870155bc/0x870157fc; attribute composition only partly decoded — the RO bit is confirmed). Android rewrites guest s2 via RKP HVCs; **Windows never does** (no RKP_START). | uh 0x8701a16c |
| F7 | **uH log ring**: `0x87100000 + 0x40000`, zeroed at init, ~20 ASCII lines written during boot init (banner `UH64_23d99e21 neoswbuilder neoswbuilder-db400tda 08/18/2022_17:38 ./build release exynos9820`). Guest-visible RO by static s2 design. Line format: **`[sec.usec][LVL][cpu-slot:seq] <tag> <line> - <msg>`** (CNTPCT/CNTFRQ time; 8 per-CPU 8KiB exception-stack slots at 0x87044000; seq via LDAXR). bigdata page = 0x870ff000 (+0xffc40 rec, 0x3c0). | uh 0x870119ac, 0x8700c4f4 |
| F8 | **uh_panic on 9820** = log at 'D' + PMU software reset **`0x1586080C = arg|0xEE`** (arg 0x8800) + **the faulting core then spins forever at EL2**. A core spinning at EL2 is powered, never answers IPIs, and never returns an EL1 exception — matches the old "silent all-CPU hang, all cores PU=1" death shape IF uH ever panics on an M4. | uh 0x87012e7c, 0x8701a5a0 |
| F9 | sboot.bin = BL2 + PSCI **clients** (it SMCs CPU_ON 0xC4000003 / CPU_OFF 0x84000002 itself around CL2 power/DVFS, wrapper at 0xa5634) + per-CPU **EL1/EL2/EL3 register save/restore thunk tables** (HCR_EL2, VTTBR, VTCR, VBAR_EL2, SCTLR/TTBR/MAIR_EL2, CNTHCTL, CNTFRQ, ACTLR_EL1/2/3, and the **M4 IMP-DEF syndrome reg S3_1_C15_C2_1**). The runtime PSCI monitor's own handler was NOT located in sboot.bin or cm.bin; cm.bin (partition BOOTLOADER2) is S-EL1 TZ content. The monitor blob is not in the HXJ6 BL tar. | sboot.disasm (m4/artifacts) |
| F10 | sboot debug-name table (0x35400): `enter_c2+/-`, `exit_c2+/-`, `cl2emul+/-` (CL2 SFR **emulation** toggle — likely relevant to why off-cluster peeks at 0x1D100104/PMU freeze the SoC), `cl_stat`, `cpu_stat`, `hotpl_fl`. | sboot-strings.txt |
| F11 | The PIT also defines **`uhuh.bin` (UHCFG), `acpm.bin` (ACPM), `ect.bin` (ECT)** partitions — none shipped in the HXJ6 BL tar. ACPM owns M4 rail voltage. | BEYOND0LTE pit strings |
| F12 | **Device reality check (owner dumps, /data2/sams):** the phone's LUN1 runs sboot **G970FXXSGHWC1** (2023, SRPRI28A016), not the HXJ6 build — but every analyzed sboot region (uh-load, APP_INIT, CL2 PSCI/DVFS clients, sysreg thunks) is byte-identical between the two builds. And the **device's uH binary is byte-for-byte the analyzed code** (only the signing trailer differs; Samsung shipped the same uH 2023→2025). All F1-F8 facts describe exactly what Windows runs under. | /data2/sams cmp + m4/artifacts/sboot-ghwc1.* |
| F13 | **`uhuh.bin` (UHCFG, sda11) is entirely zero** — no uH config/policy exists on this device; the binary alone is the whole story. | /data2/sams/uhuh.bin |
| F14 | ACPM firmware is nowhere in LUN0, LUN1(sboot), LUN2, LUN3(cpefs), LUN4(cm container), cm.bin or any tar; the **PIT's ACPM entry is a zero-size stub** (no ACPM/ECT/UL_KEYS partition exists on this device). sboot reads the ACPM initdata table (`[0x2040000+8]` + `0x2039000` walk @0x17810) already at boot — the image source is SoC-internal (ROM/TCM or SecBl1-loaded). **The live RAM read (P7) is therefore the only retrieval path.** | LUN dumps + PIT + sboot disasm |
| F15 | **EL3 monitor is resident at PA `0x80000000` (2 GiB — inside the stage-2 Non-shareable DRAM window).** sboot validates its signature word `{0x66265999, 0x66269633, 0x6626ccca}` and can print `"EL3 Monitor information: Signature %c%c%c%c / Versoin %c%c%c%c"`; its WDT/stuck path writes breadcrumb `0x66269633@[0x80000000]`, `0x77279633|0x77279733@[0x80000010]`, `0x02038000@[0xC8FFE018]` (related rodata: an `SMC_CALL_STUCK` log name exists @0x1a39f0). The monitor binary is in NO flashed partition and cm.bin's 6 TZ blobs (loads `0x8200xxxx`) contain no PSCI/SMC dispatch — static CPU_ON/monitor RE from the package is impossible; P2 stays the way to settle H1a/H1b. | sboot disasm 0xad6bc/0x16438/0x28580 |
| F16 | sboot's DT repack (same flow as uh-load, 0x103620-0x1037f8) reserves **`sboot,rmem @0xC9000000` 16 MB** and **framebuffer @0xCA000000 20 MB** (inside the 2-4 GB window!) and sets **`/cpufreq/domain@2` = "pm_qos-booting" = 2000000 (kHz)** — domain@2 is the M4 cluster (DTB: domain@0=LITTLE). "Update Max freq to %d" comes from an SCM getter via a .bss pointer table (0x479a00). UEFI-side tests must treat `0xC9000000-0xCB400000` as bootloader-occupied. | sboot disasm + base.dts |
| F17 | sboot.bin carries a **dense 32-bit config-table region at file `0x1F0000-0x250000`** (~640 KiB, no magic, entropy 7.4, version-dependent between GHWC1/HXJ6) — prime candidate for the boot DVFS/voltage/DMC tables feeding the 0x17810 consumer. Next host-side decode target. | entropy map + disasm |
| F18 | **CPU/bus FREQUENCY ladders are in the DT package** (not only in ACPM): `/cpufreq-ufc/domain@2` (top cluster; @0 tops 1950000=A55, @1 2314000=A75, @2 2912000=M4) lists a 23-OP ladder 2912000→**520000** with per-point MIF/INT bus floors; dtbo carries the 20-OP `dvfs_mif` ladder 2730000→520000 (x7 cap variants) + per-domain mif-perf caps; `devfreq_mif@17000010` boot_info=(idx 40, cap 2093000) — idx 40 ∉ 20-OP dtbo ladder ⇒ **ACPM's internal MIF table is ~41 OPs**. CL2 bus calibration ECT name: **"dvfs_cpucl2"**. Extracted artifact: `m4/artifacts/dt-dvfs-tables.txt`. **Per-OP core voltages remain ACPM-internal only.** | dt.img/dtbo.img/base.dts decode |
| F19 | **uH's guest stage-2 is effectively static; the fault path cannot extend it.** Runtime inserter helpers (0x87015280/5bc/7fc) build 4-level trees rooted at **TTBR0_EL2 (uH's own EL2 stage-1)**, never the VTTBR root; VTTBR-root walks in code are read-only overlap checks; `MSR VTTBR_EL2` occurs only as "install static 0x87028000" or "zero (s2 off)". Faults replay the instruction (AT s12e1r PAR check) and are gated by an **in-flight-upcounter retry** ([0x870430c0]); all table edits are under a qspinlock, break-before-make, **broadcast TLBI only** (VAE2IS/IPAS2E1IS/VMALLE1IS/VAAE1IS/ALLE2IS/VMALLS12E1IS, `dsb sy`) — stale-s2-by-local-TLBI ruled out. Static s2 covers 0x8000000-0xFFFFFFFF (holes at uH window + 0-0x8000000) and **bank3 0x880000000-0x8C0000000 only**; DRAM bank4 @0x900000000 (DTB: 3x2GB) is unmapped and faults forever. **G30 rule: the UEFI map handed to Windows must stay within the statically-mapped ranges — nothing above 0x8BFFFFFF.** | notes/m4-research-r1r7.md §R2 |
| F20 | **No Mongoose-specific firmware handling found**: uH contains zero IMP-DEF sysreg accesses (no S3_x/actlr/MIDR); sboot's IMP-DEF writes (incl. the M4 syndrome reg S3_1_C15_C2_1, actlr thunks) are **unconditional** — its only MIDR branches test generic parts (0xc0f/0xd03/0xd07). cm/TZ writes one CPUACTLR-class reg. Any true M4 erratum workaround must live in the absent EL3-monitor blob or ACPM itself. | notes/m4-research-r1r7.md §R4 |

## 2. Hypotheses these facts produce (ranked, testable)

- **H1 — stage-2 attribute/shareability mismatch.** Every EL1 access to PA 2-4GB (where UEFI puts
  Windows kernel image/heap/pool) carries stage-2 SH=**Non-shareable** (F5). Within one DSU cluster that is
  invisible (internal snoop filter coheres A55+A75 regardless); **across the M4 cluster boundary it is
  exactly the "rare, random, structural corruption only when an M4 participates" signature** (m4kd8
  heap-metadata corruption, silent hangs, atomics-on-one-word passing when both sides were no-s2 stubs?).
  Two sub-variants — the discriminating probe is P2:
  - H1a: CPU_ONed secondaries inherit uH's EL2 state (uniform s2). Samsung's per-CPU EL2 save/restore
    thunks (F9) say yes; m4atom11 (A55+M4 shared-word atomics, 1.1M updates, no loss) argues no,
    or says DSU/interconnect snoop cacheable traffic regardless of SH.
  - H1b: only the boot CPU actually runs under s2; secondaries boot with cold EL2 registers
    (HCR=0, VBAR_EL2=0). Then M4-vs-boot-CPU is the mismatched pair, DSU-vs-DSU is hidden again.
- **H2 — uH's on-demand fault-mapping races.** Windows will fault-map many PA>=4GB pages; if uH's
  table insert isn't safe against another core simultaneously faulting the same 2MiB range, its own
  s2 tables could get corrupted → matches "shared kernel memory comes out wrong" without any M4-core
  hardware fault. The ring sampler (P3) catches uH's own lines if this ever takes a stage-2 fault itself.
- **H3 — the old uH-trap theory is DEAD** for the 9820 (F4): no EL1 op is trapped; T4/m4trap1 revival is
  not needed (a single cheap confirmation run on an M4 is enough if at all).

### 2026-10-21 research-pass update (details: notes/m4-research-r1r7.md)

- **H2 is DOWNGRADED to "avoid by construction"**: uH never extends the guest stage-2 at runtime (F19);
  its static map covers exactly PA 0-4GB (holes at 0-0x8000000 and the uH window) + bank3
  0x880000000-0x8BFFFFFF. Windows RAM outside those ranges faults forever rather than racing - and all
  uH table surgery is lock+BBM+broadcast-TLBI, so the race form of H2 is statically ruled out.
  **Immediate static action for G30: audit the UEFI/PUBLISH_HIGH_BANKS range generator so no RAM is
  offered above 0x8BFFFFFF (bank4 @0x900000000 must be excluded).**
- H1 (R1 answer, cited in notes §R1 Q1/Q2): ARM ARM combine tables (C.a D4.5.4 Tables D4-41..43;
  G.a D5.5.7 D5-43/44/45; K.a D8.6.5/D8.6.7.1 D8-91/92/97) - **shareability = the MORE shareable of the
  two wins** (explicit row "Inner Shareable | Non-shareable | Inner Shareable"), and any Device result is
  forced **Outer Shareable**; FWB does not affect the combine ("unaffected by the value of HCR_EL2.FWB").
  uH's s2 SH=Non therefore **cannot** downgrade Windows' IS mappings architecturally. H1 survives only if
  (a) Windows maps Normal WB Non-shareable (it does not), or (b) M4 deviates from the architecture -
  and **no public M4 erratum/TRM/optimization-guide exists at all** (checked ARM erratics group, Samsung
  9820 kernel cpu_errata.c/silicon-errata, LLVM/golang only ever document the M3/9810 ISA+HWCAP bug;
  M4 = MIDR (0x53,1,0x003), dts "arm,cheetah", MPIDR 0x100/0x101 - a separate cluster from A55/A75).
  ISCA'20 paper: zero coherency-programing content, "coherent interconnect contains a snoop filter
  directory". **Re-ranking: with R5's evidence (below) H1 drops; the strongest remaining leads are H1's
  non-architectural form (device probe P2/P4/P5 discriminate) and whatever the absent EL3 monitor does
  to secondary EL2 state.**
- **R5 answer**: postmarketOS/mainline-fork on Exynos 9820 runs **all 8 CPUs including both M4s** under
  the stock sboot chain (U-Boot fork second stage; psci node identical to Samsung's: method "smc",
  CPU_ON 0xC4000003 served by stock firmware). No maxcpus/isolcpus anywhere, no public 9820 report of
  memory corruption/hangs that vanish with M4 offline; suspend lockups are cpuidle-C2-related for all
  clusters. Community-documented quirks: **PSCI AFFINITY_INFO broken, ACPM FREQ_GET broken**. A generic
  OS on all 8 cores under the same uH => sharpens suspicion on Windows-side (EL2 state on CPU_ON,
  memory-map composition incl. bank4 carve-out) rather than M4 hardware.

## 3. Device test protocol (order = value/risk; all EL1-side, boot partition only)

**P1 — UEFI-time ring peek (no risk).** Early in the host image, read 8 bytes @0x87100000 and log to
UEFILOG, plus the following 256 bytes as text. Expected if F2/F5/F7 hold on our boot path: the
`UH64_23d99e21 ... exynos9820` banner and region lines `[s.us][L][c:n] RKP_... - `. A zero read means
s2/uH is NOT on our path (or the load was optimized away) — big negative result, cheap. Implementation:
a 16-qword read loop + AsciiSPrint into UEFILOG early in BDS, image built with
`build-host-image.ps1 -Set` as usual; the UEFILOG line surviving at all is itself the proof that no
stage-2 abort fired.

**P2 — `hvc #0` presence probe (stub; escalate carefully).** uH's HVC handler returns quietly for any
imm16 != 0xC300cXXX (F2/F6 code path: EC 0x16, mask check fails, break, eret). On a parked, NOT-offered
core: mailbox mark `HVC0 PRE`, `hvc #0`, mark `HVC0 POST`, UEFILOG both.
- POST printed → uH vectors/HCR ARE installed on that core class → H1a.
- SoC dies/resets (core jumping to cold VBAR_EL2=0) → secondaries have no uH state → H1b, and
  the boot-CPU/secondary s2 asymmetry is real.
  Order: boot CPU at UEFI (guaranteed warm, proves the harness), then A55 secondary, A75 secondary,
  then each M4. Recovery as usual (Vol-Down+Power / TWRP); only `boot` is ever written.

**P3 — ring sampler during an M4 death (E2 final).** Extend the m4pcs1 not-offered-A55 sampler: every
0.25 s copy 4 KiB of 0x87100000 (+ optionally 0x870ff000..0x870fff00) into the WINPRAM ring. After the
next one-M4 death, dump via s10ediagx and parse with the F7 format. uH lines appearing around the death
name the fault; uH panicking = SoC reset instead (watch debug_history for a PMU-RST entry, INFORM3
value ~0x88EE — unverified signature, record whatever appears).

**P4 — T2/T3 with the s2 axis (now the sharpest ordering test).** Run the planned T2 patterns
(message passing, LDAXR/STLXR spinlock + invariant, MiCopyPage copy+DC CVAU/IC IVAU pattern) twice with
identical Windows-side config except the shared pages' PA:
- set A: pages in PA 0x80000000-0xBFFFFFFF (static s2 **NS** window),
- set B: pages in PA >= 0x100000000 (fault-path entries).
Different outcomes between sets ⇒ H1/H2 confirmed directly at stub level (no Windows needed); same
outcomes ⇒ s2 attrs are not the operational difference, return to m4/tasks.md T2→T3 as written.
(PA choice: the stub picks `PARK_ATOMIC_PA_OFF`-style UEFI-allocated pages — add a
`BEYOND0LTE_PARK_PAGE_PA` build knob ≥4GB. Mind the m4atom8/10 lesson: SGIs to powered-off M4s.)

**P5 — split-memory-map experiment (the potential native fix).** If P1/P2/P4 point at H1: rebuild the
host image's platform memory map (firmware/Platform/Beyond0LtePkg/Library/PlatformMemoryMapLib) so
Windows is offered only PA >= 0x100000000 for RAM (keep device windows/MMIO carve-outs where they are),
offer all 8 cores, soak. Heap-corruption signature gone/moved ⇒ confirms H1 at system level and gives a
shipping direction (Windows never touches the NS-window pages; uH's fault-path attrs become the norm).

**P6 — (optional, owner-side) RKP-lite s2 upgrade.** If H1 is confirmed and the split-map fix is too
invasive: the RKP HVC surface (0xC300C200 APP_RKP + RKP_WRITE_PGT1..3) rewrites guest s2 with proper
attrs — but it expects Linux structures (rkp_init); treat as research, not a near-term step.

**P7 — ACPM RAM dump (the voltage data; no extra files needed).** The frequency ladders are already in
the DT (F18) - what is NOT anywhere on flash is the per-OP **voltage** table and ACPM's full ~41-OP MIF
table. They live inside the ACPM coprocessor image, resident in
chip RAM: DTB `acpm_ipc` = reg `0x15900000/0x1000` (IPC SFRs) + **`0x02039000/0x30000` (ACPM RAM)**,
initdata-base 0x7000, dump-base 0x203c000 size 0x2d000 (dumps/hhxj6/base.dts:6116-6141).
From running Windows: `s10ediagx dumpmmio 0x2039000 0x80` as a liveness probe first (m4mb.sh pattern,
watchdog armed), then `0x2039000 0x30000`. On-chip RAM, not a control SFR - expected safe - but treat it
like the 0x1D100104 lesson: bisect, never blind-loop. Result = ground truth for which voltage ACPM picks
for the 520 MHz CL2 step (closes the m4clk1 UNVERIFIED gap) + a base for FREQ_REQ/ACPM mailbox RE.
Partition hunt is closed (F14): this dump is the only way to ever see the ACPM image.

**P8 — EL3 monitor identification read (new, trivially safe).** 256-byte RAM read at **PA `0x80000000`**
via the same UEFI/s10ediagx path. Expect: signature word (one of `0x6626xxxx` per F15) + a 4-char ASCII
version string ("Versoin: %c%c%c%c") + the monitor's info block. Compare against sboot GHWC1's expected
constants; also read `0x80000010` (`0x77279633/9733` = WDT breadcrumb flags — nonzero here means a past
stuck-monitor event was recorded). Plain DRAM inside the s2-identity-mapped 0-4 GB region — no SFR risk.

## 4. Data the owner can provide that would sharpen all of this

1. **DONE (2026-10-21, both rounds):** recon + LUN1-LUN4 + uh + uhcfg dumps all received and parsed
   (/data2/sams, F12-F15). The storage hunt is **closed**: LUN3=cpefs, LUN4=cm TZ blobs, and no
   ACPM/ECT/UL_KEYS partition exists (PIT stub). Nothing further to dd — the ACPM image can only be
   retrieved live (P7). Sibling-package `uh.bin` diffing is moot (device vs HXJ6 uH = same code).
2. **`dqmdbg.img` is already inside the AP tar** (local) — worth cracking next session: debug images
   sometimes carry the log-tag dictionaries that map the `RKP_xxxxxxxx` hashed names to readable strings,
   which would make P3 ring lines human-readable.
3. From the Windows side (our own KD/s10ediagx instrumentation only, no Windows binaries): the **final
   physical address ranges** Windows actually uses (kernel .text/.data/pool from our KPRCB dumps + the
   E820 map we hand it) — needed to size P5 and to know how much of the kernel heap sits in the NS window.

## 5. Still-open host-side RE (no device needed)

Answered 2026-10-21 (see notes/m4-research-r1r7.md): uH fault-path helpers (F19), DT frequency ladders
(F18), IMP-DEF/MIDR sweep (F20). Still open:
- sboot `cl2emul` implementation (command strings @0x355e4/0x355f0): not traced this pass - may name the
  exact hazard behind the SoC-freezing peeks (0x1D100104 / 0x15861200-0x1650).
- sboot `0x1F0000-0x250000` config-table region (F17) x the 0x17810 initdata consumer: decode the boot
  DVFS/voltage tables statically - pairs with P7's live ACPM dump.
- The PSCI monitor: confirmed NOT in the package (F15) - resident at PA 0x80000000 at runtime. P8+
  (256 B probe first) could extend to a live code dump via our own UEFI tool.
- The fault-path instruction decoder 0x870190fc + 0x87016590/0x870166d4/0x87016818 trio: the exact
  covered classes for the ISV=0 emulated path (LDP/STP, LSE atomics, exclusives, DC ZVA, FP) - unmapped
  this pass (time-boxed); matters only if F19's map rule is violated and a fault actually fires.
- sboot SCM client layer: .bss pointer table at image 0x479a00 (thunks 0xbbb0c/0xbbb38/0xbbb64...); find
  the registration site to name every SCM command sboot issues (frequency get/set among them).
