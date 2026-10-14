//
// Beyond0LteParkStub.S - ACPI Multiprocessor Parking Protocol stub (m14, notes/m14-smp.md).
//
// NOT executed in place. Beyond0LteParking.c copies the bytes between
// Beyond0LteParkStub and Beyond0LteParkStubEnd to offset 0x800 (the firmware half)
// of each secondary's 4 KiB mailbox page, and PSCI CPU_ONs the core at that copy.
// The code is therefore position-independent: no literals, no absolute addresses.
//
// Entry state: whatever Samsung's EL3 (through uH at EL2) hands a CPU_ON'd core -
// MMU off, so every data access is Device memory and goes straight to DRAM. x0 is
// NOT used: on the 9810 the EL3 delivered 0xBFFF01C instead of the context_id
// (Star2LtePkg PBML STAR2LTE_PSCI_PARK block), so the stub finds its mailbox from
// its own PC instead.
//
// Mailbox (one page per CPU):
//   +0x000 UINT32 ProcessorId   OS writes (firmware initialises 0xFFFFFFFF)
//   +0x008 UINT64 JumpAddress   OS writes the entry PA; 0 = not yet
//   +0x800        this stub
//   +0xF60..0xFA8 UINT64 x10    m14: MIDR, CTR, DCZID, ISAR0, ISAR1, MMFR0, MMFR1, MMFR2, PFR0, CLIDR of this core
//   +0xFB0 UINT32 GIC if mask  m14: GICD_ITARGETSR0 byte 0 as read by this core (its real CPU interface)
//   +0xFB8 UINT32 sleep        m14: firmware sets 1 -> after the alive mark the core sleeps in WFI forever
//   +0xFC0 UINT64 polls        m14 self-report: poll iterations so far (alive while it rises)
//   +0xFC8 UINT64 loop ticks    m14: CNTVCT ticks for the last 0x2000-iteration delay loop (core clock)
//   +0xFD8 UINT32 stage        m14 DVM test: 1 before IC IALLUIS, 2 before TLBI VMALLE1IS, 3 after both
//   +0xFE0 UINT32 0x5A5ABEEF    alive mark, written by the core itself
//   +0xFE8 UINT64 CurrentEL     as seen by the stub (EL1 expected: 0x4)
//   +0xFF0 UINT64 MPIDR_EL1     as seen by the stub
//   +0xFF8 UINT64 CNTFRQ_EL0    as seen by the stub (m14: is it set on every cluster?)
//
// Wait: a polling loop, not WFI/WFE. Windows wakes a parked core with an SGI, but
// nothing has enabled this core's GIC CPU interface, so a WFI wake is not
// guaranteed; polling is what the 9810 port proved under Windows. Each poll does
// DC CIVAC on the mailbox line before the load, so an OS write that is still
// dirty in another core's cache (if the OS mapped the page cacheable) is pushed
// to DRAM where this MMU-off load can see it. The delay keeps three idle cores
// from hammering the interconnect while Windows boots.
//
// Release: JumpAddress != 0 -> clear it (the spec's acknowledgement; Linux WARNs
// if it is left set), x0 = mailbox PA, branch. ProcessorId is not checked: the
// mailbox is private to this core, and the 9810 port worked without the check.
//

#include <AsmMacroLib.h>

#define PARK_MARK_OFF   0xFE0
#define PARK_EL_OFF     0xFE8
#define PARK_MPIDR_OFF  0xFF0
#define PARK_STAGE_OFF  0xFD8
#define PARK_CNTFRQ_OFF 0xFF8
#define PARK_POLLS_OFF      0xFC0
#define PARK_SLEEP_OFF      0xFB8
#define PARK_GICIF_OFF      0xFB0
#define PARK_LOOPTICKS_OFF  0xFC8

#ifndef BEYOND0LTE_PARK_DVM_TEST
#define BEYOND0LTE_PARK_DVM_TEST  1   // m14: does a broadcast (DVM) maintenance op + DSB complete on this core?
#endif

ASM_FUNC(Beyond0LteParkStub)
  adr   x1, .                       // PA of this copy (MMU off: VA == PA)
  and   x1, x1, #0xFFFFFFFFFFFFF000 // -> mailbox page base

#if BEYOND0LTE_PARK_DVM_TEST
  //
  // m14 M4 freeze (notes/m14-smp.md): Windows froze right after an M4 ran IC IALLUIS + DSB. Issue the same
  // inner-shareable broadcasts here, one stage mark before each, so a core that wedges in the DSB is visible in
  // the firmware log (stage stuck at 1 or 2, no alive mark -> the core is MADT-disabled, Windows boots without it).
  //
  mov   w3, #1
  str   w3, [x1, #PARK_STAGE_OFF]
  dsb   sy
  ic    ialluis
  dsb   sy
  isb
  mov   w3, #2
  str   w3, [x1, #PARK_STAGE_OFF]
  dsb   sy
  tlbi  vmalle1is
  dsb   sy
  isb
  mov   w3, #3
  str   w3, [x1, #PARK_STAGE_OFF]
  dsb   sy
#endif

  mrs   x2, CurrentEL
  str   x2, [x1, #PARK_EL_OFF]
  mrs   x2, mpidr_el1
  str   x2, [x1, #PARK_MPIDR_OFF]
  mrs   x2, cntfrq_el0              // m14: Samsung Linux never relies on it (DT clock); Windows' timer init does
  str   x2, [x1, #PARK_CNTFRQ_OFF]
  //
  // m14: this core's GIC CPU interface mask = GICD_ITARGETSR0 byte 0 (banked per CPU, GICv2 architecture).
  // GICD = EXYNOS_GICD_BASE 0x10101000 (Exynos9820.h, VERIFIED; Beyond0LteParking.c STATIC_ASSERTs the value),
  // ITARGETSR0 at +0x800. Built with movz/movk: the stub must stay position-independent (no literal pool).
  //
  movz  x3, #0x1010, lsl #16
  movk  x3, #0x1800
  ldrb  w2, [x3]
  str   w2, [x1, #PARK_GICIF_OFF]
  //
  // m14: ID / cache-type registers of this core (Windows samples several on the boot CPU only; Linux sanitizes
  // them system-wide) - logged next to the boot CPU's own values by Beyond0LteParking.c.
  //
  mrs   x2, midr_el1
  str   x2, [x1, #0xF60]
  mrs   x2, ctr_el0
  str   x2, [x1, #0xF68]
  mrs   x2, dczid_el0
  str   x2, [x1, #0xF70]
  mrs   x2, id_aa64isar0_el1
  str   x2, [x1, #0xF78]
  mrs   x2, id_aa64isar1_el1
  str   x2, [x1, #0xF80]
  mrs   x2, id_aa64mmfr0_el1
  str   x2, [x1, #0xF88]
  mrs   x2, id_aa64mmfr1_el1
  str   x2, [x1, #0xF90]
  mrs   x2, id_aa64mmfr2_el1
  str   x2, [x1, #0xF98]
  mrs   x2, id_aa64pfr0_el1
  str   x2, [x1, #0xFA0]
  mrs   x2, clidr_el1
  str   x2, [x1, #0xFA8]
  movz  w2, #0xBEEF
  movk  w2, #0x5A5A, lsl #16
  str   w2, [x1, #PARK_MARK_OFF]
  dsb   sy

  ldr   w3, [x1, #PARK_SLEEP_OFF]   // m14 isolation test: firmware asked this core to sleep, not poll
  cbz   w3, 0f
  msr   daifset, #0xf
  cmp   w3, #3
  b.eq  5f
  cmp   w3, #4
  b.eq  7f
  cmp   w3, #2
  b.ne  3f
  //
  // sleep flag 2 (m14): power this core back off through Samsung's PSCI 0.1 CPU_OFF (live DT /psci cpu_off =
  // 0x84000002, VERIFIED). Must not return; if it does, mark +0xFB8 = 0xDEAD0002 and fall into the WFI loop.
  //
  mov   x19, x1                     // SMCCC: x0-x17 may be clobbered, x19 is preserved
  movz  x0, #0x8400, lsl #16
  movk  x0, #0x0002
  smc   #0
  mov   x1, x19
  movz  w3, #0x0002
  movk  w3, #0xDEAD, lsl #16
  str   w3, [x1, #PARK_SLEEP_OFF]
  dsb   sy
3:
  wfi
  b     3b

  //
  // sleep flag 3 (m14 coherency test): run CACHED. Identity map in this mailbox page at +0x200 (level-1 table,
  // T0SZ 32, 4 KiB granule): entries 2 and 3 = 1 GiB blocks 0x80000000 / 0xC0000000, Normal WB (MAIR attr0 0xFF),
  // inner shareable, AF, EL1 RW, executable. Then echo: whenever the word at +0x100 (written by the boot CPU)
  // changes, copy it to +0x140 (own cache line); 0xFFFFFFFF ends the test -> WFI. No cache maintenance anywhere:
  // only hardware coherency can make the two cores see each other.
  //
5:
  add   x5, x1, #0x200              // table PA (64-byte aligned inside the mailbox page)
  str   xzr, [x5, #0x00]            // [0] 0x00000000 - invalid
  str   xzr, [x5, #0x08]            // [1] 0x40000000 - invalid
  movz  x6, #0x8000, lsl #16
  movk  x6, #0x0701                 // block | SH inner (3 << 8) | AF (1 << 10), AttrIndx 0
  str   x6, [x5, #0x10]             // [2] 0x80000000
  movz  x6, #0xC000, lsl #16
  movk  x6, #0x0701
  str   x6, [x5, #0x18]             // [3] 0xC0000000
  dsb   sy
  mov   x6, #0xFF                   // MAIR attr0 = Normal WB RW-allocate
  msr   mair_el1, x6
  movz  x6, #0x0080, lsl #16        // EPD1 (bit 23)
  movk  x6, #0x3520                 // SH0 3 | ORGN0 1 | IRGN0 1 | T0SZ 32, TG0 4K, IPS 32-bit
  msr   tcr_el1, x6
  msr   ttbr0_el1, x5
  isb
  tlbi  vmalle1
  dsb   nsh
  isb
  mrs   x6, sctlr_el1
  movz  x7, #0x1005                 // I (bit 12) | C (bit 2) | M (bit 0)
  orr   x6, x6, x7
  msr   sctlr_el1, x6
  isb
  movz  w3, #0x0003
  movk  w3, #0xC0C0, lsl #16
  str   w3, [x1, #PARK_SLEEP_OFF]   // 0xC0C00003: running cached, echo loop entered
  mov   w8, #0                      // last value echoed
6:
  ldr   w4, [x1, #0x100]
  cmp   w4, w8
  b.eq  6b
  cmn   w4, #1
  b.eq  3b                          // 0xFFFFFFFF: test over -> WFI (still cached; this core is never offered)
  str   w4, [x1, #0x140]
  mov   w8, w4
  b     6b

  //
  // sleep flag 4 (m14 trap timing): write each EL1 register back with its own value 1000 times and store the
  // CNTVCT ticks at +0x300 (baseline nop loop), +0x308 TTBR0, +0x310 TCR, +0x318 SCTLR, +0x320 MAIR,
  // +0x328 CONTEXTIDR. A write trapped to EL2 (uH) costs microseconds, an untrapped one a few ns. Then WFI.
  //
7:
  mov   x9, #1000
  mrs   x4, cntvct_el0
71:
  nop
  subs  x9, x9, #1
  b.ne  71b
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #0x300]

  mrs   x6, ttbr0_el1
  mov   x9, #1000
  mrs   x4, cntvct_el0
72:
  msr   ttbr0_el1, x6
  subs  x9, x9, #1
  b.ne  72b
  isb
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #0x308]

  mrs   x6, tcr_el1
  mov   x9, #1000
  mrs   x4, cntvct_el0
73:
  msr   tcr_el1, x6
  subs  x9, x9, #1
  b.ne  73b
  isb
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #0x310]

  mrs   x6, sctlr_el1
  mov   x9, #1000
  mrs   x4, cntvct_el0
74:
  msr   sctlr_el1, x6
  subs  x9, x9, #1
  b.ne  74b
  isb
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #0x318]

  mrs   x6, mair_el1
  mov   x9, #1000
  mrs   x4, cntvct_el0
75:
  msr   mair_el1, x6
  subs  x9, x9, #1
  b.ne  75b
  isb
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #0x320]

  mrs   x6, contextidr_el1
  mov   x9, #1000
  mrs   x4, cntvct_el0
76:
  msr   contextidr_el1, x6
  subs  x9, x9, #1
  b.ne  76b
  isb
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #0x328]
  movz  w3, #0x0004
  movk  w3, #0xC0C0, lsl #16
  str   w3, [x1, #PARK_SLEEP_OFF]   // 0xC0C00004: trap timing done
  dsb   sy
  b     3b
0:
  mov   x6, #0                      // m14 self-report: poll count
1:
  mrs   x4, cntvct_el0
  movz  w3, #0x2000
2:
  subs  w3, w3, #1
  b.ne  2b
  mrs   x5, cntvct_el0
  sub   x5, x5, x4
  str   x5, [x1, #PARK_LOOPTICKS_OFF] // 26 MHz ticks per 0x2000 delay iterations -> this core's clock
  add   x6, x6, #1
  str   x6, [x1, #PARK_POLLS_OFF]   // keeps rising while the core is alive in the stub
  dc    civac, x1
  dsb   sy
  ldr   x2, [x1, #8]                // JumpAddress
  cbz   x2, 1b

  str   xzr, [x1, #8]               // acknowledge: JumpAddress = 0
  dsb   sy
  mov   x0, x1                      // x0 = mailbox PA
  br    x2

  .global ASM_PFX(Beyond0LteParkStubEnd)
ASM_PFX(Beyond0LteParkStubEnd):
  nop

