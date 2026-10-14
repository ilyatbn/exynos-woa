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
//   +0xFB8 UINT32 sleep         m14: firmware sets 1 -> after the alive mark the core sleeps in WFI forever
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
  movz  w2, #0xBEEF
  movk  w2, #0x5A5A, lsl #16
  str   w2, [x1, #PARK_MARK_OFF]
  dsb   sy

  ldr   w3, [x1, #PARK_SLEEP_OFF]   // m14 isolation test: firmware asked this core to sleep, not poll
  cbz   w3, 0f
  msr   daifset, #0xf
3:
  wfi
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

