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
| F14 | ACPM firmware is nowhere in LUN0, LUN1(sboot), LUN2, cm.bin or the tars; sboot carries the ACPM load/dump framework (`acpm_dump: base %x size %x`, `acpm_vclk_list`). Candidates: UFS LUN3/LUN4 (sdd 8 MiB incl. cpefs, sde 8 MiB) - not yet dumped. | strings + recon |

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

**P7 — ACPM RAM dump (the voltage data; no extra files needed).** M4 DVFS/OPP+voltage tables are NOT in
the DTB and NOT in vendor.img - Samsung keeps them inside the ACPM coprocessor image, which runs live in
chip RAM: DTB `acpm_ipc` = reg `0x15900000/0x1000` (IPC SFRs) + **`0x02039000/0x30000` (ACPM RAM)**,
initdata-base 0x7000, dump-base 0x203c000 size 0x2d000 (dumps/hhxj6/base.dts:6116-6141).
From running Windows: `s10ediagx dumpmmio 0x2039000 0x80` as a liveness probe first (m4mb.sh pattern,
watchdog armed), then `0x2039000 0x30000`. On-chip RAM, not a control SFR - expected safe - but treat it
like the 0x1D100104 lesson: bisect, never blind-loop. Result = ground truth for which voltage ACPM picks
for the 520 MHz CL2 step (closes the m4clk1 UNVERIFIED gap) + a base for FREQ_REQ/ACPM mailbox RE.

## 4. Data the owner can provide that would sharpen all of this

1. **DONE (2026-10-21):** recon + LUN1/LUN2 + uh + uhcfg dumps received (see /data2/sams, F12-F14).
   Remaining ask: **full dumps of `sdd` and `sde`** (8 MiB each, TWRP:
   `dd if=/dev/block/sdd of=/sdd.bin && dd if=/dev/block/sde of=/sde.bin && adb pull /sdd.bin /sde.bin`)
   plus the LUN map (`for l in sdb sdc sdd sde; do echo $l $(cat /sys/block/$l/device/lun); done`).
   ACPM is the only thing they can still hide; nothing else pending on the device side.
   (uhuh.bin answered: all-zero. Sibling-package `uh.bin` diffing is moot: device vs HXJ6 uH are the
   same code; a sibling 9820 package would only help find a packaged `acpm.bin`.)
2. **`dqmdbg.img` is already inside the AP tar** (local) — worth cracking next session: debug images
   sometimes carry the log-tag dictionaries that map the `RKP_xxxxxxxx` hashed names to readable strings,
   which would make P3 ring lines human-readable.
3. From the Windows side (our own KD/s10ediagx instrumentation only, no Windows binaries): the **final
   physical address ranges** Windows actually uses (kernel .text/.data/pool from our KPRCB dumps + the
   E820 map we hand it) — needed to size P5 and to know how much of the kernel heap sits in the NS window.

## 5. Still-open host-side RE (no device needed)

- uH insert helpers 0x87015280 / 0x870155bc / 0x870157fc: exact MemAttr/SH/XN of fault-inserted s2 entries
  (decides whether H1's "fault-path pages are fine" premise holds).
- uH app-dispatch FUN_8700b220 + per-app tables (DAT_87043298..0432d0): what apps 3..7 initialized in
  sboot's loop are, and whether anything arms per-CPU state.
- sboot `cl2emul` implementation (command strings @0x355e4): Samsung's own emulation shim for CL2 SFR
  access — may name the exact hazard behind the SoC-freezing peeks (0x1D100104 / 0x15861200-0x1650).
- The PSCI monitor itself: not in these tars; needs the device dumps (item 4.1) or another package.
