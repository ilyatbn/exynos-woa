/** @file
  m14: Windows secondaries through the ACPI Multiprocessor Parking Protocol
  (notes/m14-smp.md).

  Samsung's EL3 is PSCI 0.1 (live DT /psci: cpu_on 0xC4000003, m10 W8). On the
  9810 it did not deliver the PSCI context_id in x0, so Windows' own PSCI CPU_ON
  path started cores that died silently (Star2LtePkg PBML STAR2LTE_PSCI_PARK).
  Here the firmware CPU_ONs each selected secondary into its own parking stub
  (AArch64/Beyond0LteParkStub.S) and publishes that stub's mailbox in the MADT;
  Windows releases the core by writing an entry address into the mailbox.

  Runs once, from the ACPI installer, on the MADT buffer BEFORE it is installed
  (AcpiTableDxe recomputes the checksum on install). A GICC is advertised as
  parked only when CPU_ON returned SUCCESS and the core wrote its own alive mark;
  every other enabled secondary is marked disabled, because with the FADT PSCI
  flag cleared Windows has no other way to start it.

  NOTHING HERE IS VALIDATED ON AN S10e (Guided). Build-verified only.
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <IndustryStandard/Acpi63.h>
#include <Platform/Exynos9820.h>
#include "../../AcpiTables/Beyond0LteAcpiHeader.h"
#include "Beyond0LteParking.h"
#include "../../Library/Beyond0LteFbSerialPortLib/Beyond0LteBootLog.h"

#if BEYOND0LTE_SMP_PARKING

//
// PSCI 0.1 CPU_ON function id: the live S10e DT's /psci cpu_on (m10 W8, VERIFIED
// read). The SMC64 form, so x1 = target MPIDR, x2 = entry PA, x3 = context.
//
#define PARK_PSCI_CPU_ON  0xC4000003u

//
// Mailbox layout: ACPI Multiprocessor Startup for ARM Platforms v1 (u-boot
// arch/arm/include/asm/acpi_table.h ACPI_PP_*, Linux acpi_parking_protocol.c).
// The alive-mark offsets are ours, in the firmware half, and must match the .S.
//
#define PARK_ID_OFF         0x000u
#define PARK_JUMP_OFF       0x008u
#define PARK_STUB_OFF       0x800u
#define PARK_MARK_OFF       0xFE0u
#define PARK_EL_OFF         0xFE8u
#define PARK_MPIDR_OFF      0xFF0u
#define PARK_CNTFRQ_OFF     0xFF8u   // CNTFRQ_EL0 as the parked core sees it (m14)
#define PARK_STAGE_OFF      0xFD8u
#define PARK_GICIF_OFF      0xFB0u   // m14: GICD_ITARGETSR0 byte 0 read by the parked core (its real GIC CPU interface mask)
#define PARK_SLEEP_OFF      0xFB8u   // m14: 1 = the stub sleeps in WFI (DAIF masked) after the alive mark, never polls
#ifndef BEYOND0LTE_PARK_SLEEP_UNOFFERED
#define BEYOND0LTE_PARK_SLEEP_UNOFFERED  0   // m14 isolation test: parked-but-not-offered cores sleep instead of polling
#endif
#define PARK_POLLS_OFF      0xFC0u   // m14 stub self-report (poll count, loop ticks) - must stay above the stub code
#define PARK_LOOPTICKS_OFF  0xFC8u   // m14 DVM test stage (the .S BEYOND0LTE_PARK_DVM_TEST block), 0 if not built
#define PARK_ID_INVALID     0xFFFFFFFFu
#define PARK_ALIVE          0x5A5ABEEFu
#define PARK_VERSION        1u

#define PARK_MAX_GICC       8u      // Madt.aslc BEYOND0LTE_NUM_CORES
#define PARK_WAIT_STEP_US   1000u
#define PARK_WAIT_STEPS     2000u   // up to 2 s for all marks: boot #55 (fast logging, no FB text) saw the cores arrive ~100 ms after CPU_ON - 100 ms was only enough while slow panel text padded the gap (the 9810 waited 30 ms)
#define PARK_MPIDR_AFF_MASK   0xFF00FFFFFFULL   // Aff3 + Aff2..Aff0 (MT/U bits excluded) - ACPI MADT MPIDR field
#ifndef BEYOND0LTE_SMP_REAL_MPIDR
#define BEYOND0LTE_SMP_REAL_MPIDR  0   // m14 run 34: did not fix the M4 hang; untested with the 6-core set - off = the proven boot #63 tables
#endif
#define PARK_PROGRESS_TRYING  0x65u  // boot progress code: CPU_ON of a core outside the safe mask in flight

// Boot progress word page (Beyond0LteBootLog / bootshim: [0] previous boot's last code, [1] this boot's).
STATIC volatile UINT32  *CONST  mParkProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

STATIC
UINT64
ParkBootCntFrq (
  VOID
  )
{
  UINT64  Value;

  __asm__ __volatile__ ("mrs %0, cntfrq_el0" : "=r" (Value));
  return Value;
}

#ifndef BEYOND0LTE_PARK_COHERENCY_MASK
#define BEYOND0LTE_PARK_COHERENCY_MASK  0u   // m14: parked, not-offered cores that run the cached ping-pong test
#endif
#ifndef BEYOND0LTE_PARK_PCSAMPLE_MASK
#define BEYOND0LTE_PARK_PCSAMPLE_MASK   0u   // m14: the parked, not-offered core that samples every core's EDPCSR (stub flag 5)
#endif
#define PARK_PCS_READY      0xC0C00005u
#define PARK_PCS_RING_OFF   0x408u   // sampler parameters, between the vector slots (+0x400 / +0x480 hold branches)
#define PARK_PCS_SLOTS_OFF  0x410u
#define PARK_PCS_PERIOD_OFF 0x418u
#define PARK_PCS_BASES_OFF  0x420u   // 8 x UINT64 debug block bases, MADT GICC order
#define PARK_PCS_GO_OFF     0x460u   // firmware sets 1 after every CPU_ON finished (bases filled) - sampler starts
#define PARK_PCS_FAULT_OFF  0x608u   // ESR, FAR, count, ELR (UINT64 each) written by Beyond0LteParkFault
#define PARK_PCS_PERIOD     6500000u // CNTVCT ticks at 26 MHz = 0.25 s -> 1023 records = 255 s of history
#ifndef BEYOND0LTE_PARK_STRESS_MASK
#define BEYOND0LTE_PARK_STRESS_MASK     0u   // m14: parked, not-offered cores that run the cached copy stress (stub flag 6)
#endif
#define PARK_STRESS_READY   0xC0C00006u
#define PARK_STRESS_BUF_OFF 0x500u   // scratch PA (reserved here, below 4 GiB: the stub maps 0x80000000..0xFFFFFFFF)
#define PARK_STRESS_HALF_OFF 0x508u  // half the scratch size (copy lower half -> upper half)
#define PARK_STRESS_BEAT_OFF 0x340u  // passes done, written by the core
#define PARK_STRESS_PAGES   2048u    // 8 MiB per stress core
#ifndef BEYOND0LTE_PARK_IRQ_MASK
#define BEYOND0LTE_PARK_IRQ_MASK        0u   // m14: parked, not-offered cores that run stress + timer IRQ + SGIs + WFI (stub flag 7)
#endif
#define PARK_IRQ_PEER_OFF   0x510u   // GIC target list for the SGI sent from every timer IRQ (the other flag-7 cores)
#define PARK_IRQ_PERIOD_OFF 0x518u   // CNTP_TVAL reload, 26 MHz ticks
#define PARK_IRQ_PERIOD     26000u   // 1 ms
#define PARK_IRQ_CNT_OFF    0x350u   // timer IRQs, SGIs, other, last IAR, spurious (UINT64 each, written by the core)
#define PARK_IRQ_OPTS_OFF   0x520u   // flag-7 options (bit 0 = PMU probe)
#ifndef BEYOND0LTE_PARK_IRQ_OPTS
#define BEYOND0LTE_PARK_IRQ_OPTS        0u   // m14 flag 7: bit 0 = PMU probe + cycle counter read every pass, bit 1 = shared LSE atomics
#endif
#define PARK_ATOMIC_PA_OFF  0x528u   // option bit 1: PA of the page every flag-7 core runs LSE atomics on
#define PARK_ATOMIC_CNT_OFF 0x348u   // option bit 1: atomic rounds done by this core
#define PARK_PMU_OFF        0x3C0u   // stage, PMCR, PMCEID0, ticks/1000 PMCCNTR reads, PMCCNTR, PMEVCNTR0, last PMCCNTR
#define PARK_COH_PING_OFF   0x100u   // boot CPU writes the token here (own cache line)
#define PARK_COH_ECHO_OFF   0x140u   // the test core copies it here (own cache line)
#define PARK_COH_READY      0xC0C00003u
#define PARK_COH_ROUNDS     100000u
#define PARK_COH_SPIN       2000000u // per round, plain loads (~ms) before counting a miss

#define PARK_IDS_OFF   0xF60u   // m14: 10 UINT64 ID registers written by the stub (order as ParkReadIds)
#define PARK_CTL_OFF   0xF50u   // m14: ACTLR_EL1, SCTLR_EL1 as the core got them (lowest stub-written field)
#define PARK_ID_COUNT  10u

//
// The boot CPU's copy of the registers the stub records, same order: MIDR, CTR, DCZID, ISAR0, ISAR1, MMFR0,
// MMFR1, MMFR2, PFR0, CLIDR.
//
STATIC
VOID
ParkReadIds (
  OUT UINT64  *Id
  )
{
  __asm__ __volatile__ ("mrs %0, midr_el1" : "=r" (Id[0]));
  __asm__ __volatile__ ("mrs %0, ctr_el0" : "=r" (Id[1]));
  __asm__ __volatile__ ("mrs %0, dczid_el0" : "=r" (Id[2]));
  __asm__ __volatile__ ("mrs %0, id_aa64isar0_el1" : "=r" (Id[3]));
  __asm__ __volatile__ ("mrs %0, id_aa64isar1_el1" : "=r" (Id[4]));
  __asm__ __volatile__ ("mrs %0, id_aa64mmfr0_el1" : "=r" (Id[5]));
  __asm__ __volatile__ ("mrs %0, id_aa64mmfr1_el1" : "=r" (Id[6]));
  __asm__ __volatile__ ("mrs %0, id_aa64mmfr2_el1" : "=r" (Id[7]));
  __asm__ __volatile__ ("mrs %0, id_aa64pfr0_el1" : "=r" (Id[8]));
  __asm__ __volatile__ ("mrs %0, clidr_el1" : "=r" (Id[9]));
}

STATIC_ASSERT (EXYNOS_GICD_BASE == 0x10101000ULL, "Beyond0LteParkStub.S hardcodes GICD 0x10101000 (ITARGETSR0 read)");

extern UINT8  Beyond0LteParkStub[];
extern UINT8  Beyond0LteParkStubEnd[];

STATIC
UINT32
ParkRd32 (
  IN UINT64  Address
  )
{
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Address, sizeof (UINT32));
  return *(volatile UINT32 *)(UINTN)Address;
}

STATIC
UINT64
ParkRd64 (
  IN UINT64  Address
  )
{
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Address, sizeof (UINT64));
  return *(volatile UINT64 *)(UINTN)Address;
}

#if BEYOND0LTE_PARK_SLEEP_UNOFFERED
extern UINT8  Beyond0LteParkFault[];
extern UINT8  Beyond0LteParkIrq[];
STATIC EFI_PHYSICAL_ADDRESS  mParkAtomicPage;   // m14 flag-7 option bit 1, shared by every flag-7 core

//
// m14 PC sampler: external debug blocks in MADT GICC order (Exynos9820.h, from the stock DT /coresight node).
//
STATIC CONST UINT64  mParkDbgBase[PARK_MAX_GICC] = {
  EXYNOS_CS_DBG_CL0_CPU0, EXYNOS_CS_DBG_CL0_CPU1, EXYNOS_CS_DBG_CL0_CPU2, EXYNOS_CS_DBG_CL0_CPU3,
  EXYNOS_CS_DBG_CL1_CPU0, EXYNOS_CS_DBG_CL1_CPU1, EXYNOS_CS_DBG_CL2_CPU0, EXYNOS_CS_DBG_CL2_CPU1
};

/**
  m14 PC sampler set-up for one mailbox page: keep the previous boot's ring (a hang's last seconds) in PREV, then
  fill the sampler parameters and a branch to Beyond0LteParkFault in all 16 vector slots (VBAR_EL1 = the page).
**/
STATIC
VOID
ParkPcSampleSetup (
  IN UINT64  Page
  )
{
  volatile UINT32  *Live;
  UINT64           Target;
  UINTN            Slot;
  INT64            Delta;

  Live = (volatile UINT32 *)(UINTN)BEYOND0LTE_PCSAMP_LIVE;   // uncached handoff window (PlatformMemoryMapLib)
  if (Live[0] == BEYOND0LTE_PCSAMP_MAGIC) {
    CopyMem ((VOID *)(UINTN)BEYOND0LTE_PCSAMP_PREV, (VOID *)(UINTN)BEYOND0LTE_PCSAMP_LIVE, (UINTN)BEYOND0LTE_PCSAMP_SIZE);
    DEBUG ((DEBUG_ERROR, "PARK: PCS previous ring kept (%lu samples, last cntvct %lx, was reading core %x)\n",
            *(volatile UINT64 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x10), *(volatile UINT64 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x18),
            *(volatile UINT32 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x28)));
  }

  Live[0] = 0;
  ArmDataSynchronizationBarrier ();

  Target = Page + PARK_STUB_OFF + (UINT64)(Beyond0LteParkFault - Beyond0LteParkStub);
  for (Slot = 0; Slot < 16; Slot++) {
    Delta = (INT64)(Target - (Page + Slot * 0x80)) / 4;
    *(UINT32 *)(UINTN)(Page + Slot * 0x80) = 0x14000000u | ((UINT32)Delta & 0x03FFFFFFu);   // B Beyond0LteParkFault
  }

  *(UINT64 *)(UINTN)(Page + PARK_PCS_RING_OFF)   = BEYOND0LTE_PCSAMP_LIVE;
  *(UINT64 *)(UINTN)(Page + PARK_PCS_SLOTS_OFF)  = BEYOND0LTE_PCSAMP_SIZE / BEYOND0LTE_PCSAMP_SLOT;
  *(UINT64 *)(UINTN)(Page + PARK_PCS_PERIOD_OFF) = PARK_PCS_PERIOD;
  ZeroMem ((VOID *)(UINTN)(Page + PARK_PCS_BASES_OFF), sizeof (mParkDbgBase));   // filled by ParkPcSampleGo
  *(UINT64 *)(UINTN)(Page + PARK_PCS_GO_OFF) = 0;
  *(UINT32 *)(UINTN)(Page + PARK_SLEEP_OFF)  = 5;
}

/**
  m14 PC sampler: after every CPU_ON, publish the debug bases of the cores that are up (the boot core and every
  core whose alive mark is set) and release the sampler. A powered-off core's debug block is never read
  (m4pcs1: sampling while later cores were still off hung UEFI).
**/
STATIC
VOID
ParkPcSampleGo (
  IN UINT64                      SamplerPage,
  IN EFI_ACPI_6_3_GIC_STRUCTURE  **Gicc,
  IN UINT64                      *Page,
  IN UINTN                       GiccCount,
  IN UINT64                      BootMpidr
  )
{
  UINTN  Index;

  for (Index = 0; (Index < GiccCount) && (Index < PARK_MAX_GICC); Index++) {
    if ((GET_MPIDR_AFFINITY_BITS (Gicc[Index]->MPIDR) == BootMpidr) ||
        ((Page[Index] != 0) && (ParkRd32 (Page[Index] + PARK_MARK_OFF) == PARK_ALIVE)))
    {
      *(volatile UINT64 *)(UINTN)(SamplerPage + PARK_PCS_BASES_OFF + Index * 8) = mParkDbgBase[Index];
    }
  }

  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)SamplerPage, EFI_PAGE_SIZE);
  *(volatile UINT64 *)(UINTN)(SamplerPage + PARK_PCS_GO_OFF) = 1;
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)(SamplerPage + PARK_PCS_GO_OFF), sizeof (UINT64));
}
#endif // BEYOND0LTE_PARK_SLEEP_UNOFFERED

VOID
Beyond0LteParkSecondaries (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *Madt
  )
{
  EFI_ACPI_6_3_GIC_STRUCTURE  *Gicc[PARK_MAX_GICC];
  UINT64                      Page[PARK_MAX_GICC];
  UINTN                       OnStatus[PARK_MAX_GICC];
  UINT32                      Arrive[PARK_MAX_GICC];
  UINTN                       GiccCount;
  UINTN                       ParkCount;
  UINTN                       Index;
  UINTN                       Slot;
  UINTN                       Offset;
  UINTN                       StubSize;
  UINTN                       Step;
  UINTN                       Parked;
  UINT64                      BootMpidr;
  EFI_PHYSICAL_ADDRESS        Pages;
  EFI_STATUS                  Status;
  ARM_SMC_ARGS                Smc;
  UINT8                       *Bytes;
  UINT32                      Mask;
  UINT32                      SavedProgress;

  StubSize  = (UINTN)(Beyond0LteParkStubEnd - Beyond0LteParkStub);
  BootMpidr = GET_MPIDR_AFFINITY_BITS (ArmReadMpidr ());
  DEBUG ((DEBUG_ERROR, "PARK: mask=%x boot-mpidr=%lx stub=%u B\n",
          (UINT32)BEYOND0LTE_SMP_PARK_MASK, BootMpidr, (UINT32)StubSize));
  if ((StubSize == 0) || (PARK_STUB_OFF + StubSize > PARK_CTL_OFF)) {
    DEBUG ((DEBUG_ERROR, "PARK: stub size %u does not fit - nothing parked\n", (UINT32)StubSize));
    return;
  }

  //
  // Collect the GICC entries in table order; bit n of the mask = n-th GICC.
  //
  //
  // m14 self-healing guard: a CPU_ON that hangs the SoC happens before ReadyToBoot, so no \WINRUN.FLG exists
  // and the SEC dead man's switch (82 s) would reboot into the same hang forever. Before starting any core
  // outside BEYOND0LTE_SMP_SAFE_MASK (proven on device) this boot stores progress 0x65; the shim copies it to
  // [0] on the next boot, which then parks only the safe cores.
  //
  Mask = BEYOND0LTE_SMP_PARK_MASK;
  if (mParkProgress[0] == PARK_PROGRESS_TRYING) {
    Mask &= BEYOND0LTE_SMP_SAFE_MASK;
    DEBUG ((DEBUG_ERROR, "PARK: previous boot hung starting a core (progress 0x65) - safe mask %x\n", Mask));
  }

  Bytes     = (UINT8 *)Madt;
  GiccCount = 0;
  ParkCount = 0;
  Offset    = sizeof (EFI_ACPI_6_3_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER);
  while ((Offset + 2 <= Madt->Length) && (GiccCount < PARK_MAX_GICC)) {
    UINT8  Type;
    UINT8  Len;

    Type = Bytes[Offset];
    Len  = Bytes[Offset + 1];
    if ((Len < 2) || (Offset + Len > Madt->Length)) {
      break;
    }

    if ((Type == EFI_ACPI_6_3_GIC) && (Len >= sizeof (EFI_ACPI_6_3_GIC_STRUCTURE))) {
      Gicc[GiccCount]     = (EFI_ACPI_6_3_GIC_STRUCTURE *)(Bytes + Offset);
      Page[GiccCount]     = 0;
      OnStatus[GiccCount] = MAX_UINTN;
      if (((Mask >> GiccCount) & 1u) &&
          ((Gicc[GiccCount]->Flags & EFI_ACPI_6_3_GIC_ENABLED) != 0) &&
          (GET_MPIDR_AFFINITY_BITS (Gicc[GiccCount]->MPIDR) != BootMpidr))
      {
        ParkCount++;
      }

      GiccCount++;
    }

    Offset += Len;
  }

  //
  // One page per parked core. Below the top of bank0 (EXYNOS_DRAM_BASE +
  // EXYNOS_DRAM_BANK0_SIZE, both VERIFIED in Exynos9820.h) and Reserved, so it
  // survives into Windows untouched and never needs a 64-bit mapping. The
  // allocator cannot hand out a monitor carve-out, the boot log, WINPRAM or the
  // UFS DMA pool: all of them are memory-allocation HOBs (PlatformMemoryMapLib).
  //
  Pages = 0;
  if (ParkCount != 0) {
    Pages  = EXYNOS_DRAM_BASE + EXYNOS_DRAM_BANK0_SIZE - 1;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, ParkCount, &Pages);
    DEBUG ((DEBUG_ERROR, "PARK: %u page(s) at %lx: %r\n", (UINT32)ParkCount, Pages, Status));
    if (EFI_ERROR (Status)) {
      Pages     = 0;
      ParkCount = 0;
    }
  }

  //
  // Build every mailbox, publish them to the point of coherency (the cores fetch
  // and load with the MMU off), then start the cores.
  //
  Slot = 0;
  for (Index = 0; (Index < GiccCount) && (Pages != 0); Index++) {
    if (!(((Mask >> Index) & 1u) &&
          ((Gicc[Index]->Flags & EFI_ACPI_6_3_GIC_ENABLED) != 0) &&
          (GET_MPIDR_AFFINITY_BITS (Gicc[Index]->MPIDR) != BootMpidr)))
    {
      continue;
    }

    Page[Index] = Pages + (UINT64)Slot * EFI_PAGE_SIZE;
    Slot++;
    ZeroMem ((VOID *)(UINTN)Page[Index], EFI_PAGE_SIZE);
    *(UINT32 *)(UINTN)(Page[Index] + PARK_ID_OFF)   = PARK_ID_INVALID;
    *(UINT64 *)(UINTN)(Page[Index] + PARK_JUMP_OFF) = 0;
    CopyMem ((VOID *)(UINTN)(Page[Index] + PARK_STUB_OFF), Beyond0LteParkStub, StubSize);
#if BEYOND0LTE_PARK_SLEEP_UNOFFERED
    if (((BEYOND0LTE_PARK_PCSAMPLE_MASK >> Index) & 1u) != 0) {
      ParkPcSampleSetup (Page[Index]);   // m14 PC sampler core (never offered)
    } else if ((((BEYOND0LTE_PARK_STRESS_MASK | BEYOND0LTE_PARK_IRQ_MASK) >> Index) & 1u) != 0) {
      EFI_PHYSICAL_ADDRESS  Scratch;

      Scratch = EXYNOS_DRAM_BASE + EXYNOS_DRAM_BANK0_SIZE - 1;
      if (!EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, PARK_STRESS_PAGES, &Scratch))) {
        *(UINT64 *)(UINTN)(Page[Index] + PARK_STRESS_BUF_OFF)  = Scratch;   // m14 cached stress core (never offered)
        *(UINT64 *)(UINTN)(Page[Index] + PARK_STRESS_HALF_OFF) = EFI_PAGES_TO_SIZE (PARK_STRESS_PAGES) / 2;
        *(UINT32 *)(UINTN)(Page[Index] + PARK_SLEEP_OFF)       = 6;
        if (((BEYOND0LTE_PARK_IRQ_MASK >> Index) & 1u) != 0) {
          UINT64  Target;
          UINTN   VSlot;
          INT64   Delta;

          //
          // m14 flag 7: VBAR_EL1 = the page; current-EL IRQ slots (+0x080 SP0, +0x280 SPx) -> Beyond0LteParkIrq,
          // the other current-EL slots -> Beyond0LteParkFault. Lower-EL slots are not filled (+0x500.. holds the
          // parameters; the core never runs below EL1). GIC CPU interface number = GICC index (m4gic1, VERIFIED).
          //
          for (VSlot = 0; VSlot < 8; VSlot++) {
            Target = Page[Index] + PARK_STUB_OFF + (UINT64)(((VSlot == 1) || (VSlot == 5) ? Beyond0LteParkIrq : Beyond0LteParkFault) -
                                                            Beyond0LteParkStub);
            Delta  = (INT64)(Target - (Page[Index] + VSlot * 0x80)) / 4;
            *(UINT32 *)(UINTN)(Page[Index] + VSlot * 0x80) = 0x14000000u | ((UINT32)Delta & 0x03FFFFFFu);
          }

          *(UINT32 *)(UINTN)(Page[Index] + PARK_IRQ_PEER_OFF)   = BEYOND0LTE_PARK_IRQ_MASK & ~(1u << Index) & 0xFFu;
          *(UINT64 *)(UINTN)(Page[Index] + PARK_IRQ_PERIOD_OFF) = PARK_IRQ_PERIOD;
          *(UINT32 *)(UINTN)(Page[Index] + PARK_IRQ_OPTS_OFF)   = BEYOND0LTE_PARK_IRQ_OPTS;
          if ((BEYOND0LTE_PARK_IRQ_OPTS & 2u) != 0) {
            if (mParkAtomicPage == 0) {
              mParkAtomicPage = EXYNOS_DRAM_BASE + EXYNOS_DRAM_BANK0_SIZE - 1;   // the stub maps only < 4 GiB
              if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 1, &mParkAtomicPage))) {
                mParkAtomicPage = 0;
              } else {
                ZeroMem ((VOID *)(UINTN)mParkAtomicPage, EFI_PAGE_SIZE);
                WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)mParkAtomicPage, EFI_PAGE_SIZE);
                DEBUG ((DEBUG_ERROR, "PARK: ATOMIC shared page %lx\n", mParkAtomicPage));
              }
            }

            if (mParkAtomicPage == 0) {
              *(UINT32 *)(UINTN)(Page[Index] + PARK_IRQ_OPTS_OFF) = BEYOND0LTE_PARK_IRQ_OPTS & ~2u;   // no page: no atomics
            }

            *(UINT64 *)(UINTN)(Page[Index] + PARK_ATOMIC_PA_OFF) = mParkAtomicPage;
          }
          *(UINT32 *)(UINTN)(Page[Index] + PARK_SLEEP_OFF)      = 7;
        }
        WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Scratch, EFI_PAGES_TO_SIZE (PARK_STRESS_PAGES));
        DEBUG ((DEBUG_ERROR, "PARK: STRESS gicc%u scratch %lx + %x\n", (UINT32)Index, Scratch,
                (UINT32)EFI_PAGES_TO_SIZE (PARK_STRESS_PAGES)));
      }
    } else if (((BEYOND0LTE_PARK_COHERENCY_MASK >> Index) & 1u) != 0) {
      *(UINT32 *)(UINTN)(Page[Index] + PARK_SLEEP_OFF) = 3;   // m14 coherency test core (never offered)
    } else if (((BEYOND0LTE_SMP_OFFER_MASK >> Index) & 1u) == 0) {
      // m14: 1 = this core sleeps in WFI after its alive mark, 2 = it calls PSCI CPU_OFF instead
      *(UINT32 *)(UINTN)(Page[Index] + PARK_SLEEP_OFF) = BEYOND0LTE_PARK_SLEEP_UNOFFERED;
    }
#endif
  }

  if (Pages != 0) {
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Pages, ParkCount * EFI_PAGE_SIZE);
    InvalidateInstructionCacheRange ((VOID *)(UINTN)Pages, ParkCount * EFI_PAGE_SIZE);
  }

  //
  // m14 boots #55/#56: with the panel text console gone, the CPU_ON calls went out back to back and only the
  // LAST core started reached the stub (gicc1-4 never wrote their mark, even after 2 s). The text console used
  // to space the calls out by its rendering time. So: one core at a time - CPU_ON, then wait for that core's
  // own mark before the next CPU_ON.
  //
  SavedProgress = mParkProgress[1];
  for (Index = 0; Index < GiccCount; Index++) {
    if (Page[Index] == 0) {
      continue;
    }

    if (((BEYOND0LTE_SMP_SAFE_MASK >> Index) & 1u) == 0) {
      mParkProgress[1] = PARK_PROGRESS_TRYING;   // uncached handoff window (PlatformMemoryMapLib)
      ArmDataSynchronizationBarrier ();
    }

    ZeroMem (&Smc, sizeof (Smc));
    Smc.Arg0 = PARK_PSCI_CPU_ON;
    Smc.Arg1 = (UINTN)Gicc[Index]->MPIDR;
    Smc.Arg2 = (UINTN)(Page[Index] + PARK_STUB_OFF);
    Smc.Arg3 = (UINTN)Page[Index];   // context_id: unused by the stub (see the .S)
    ArmCallSmc (&Smc);
    OnStatus[Index] = Smc.Arg0;

    Arrive[Index] = MAX_UINT32;
    if (OnStatus[Index] == 0) {
      for (Step = 0; Step < PARK_WAIT_STEPS; Step++) {
        if (ParkRd32 (Page[Index] + PARK_MARK_OFF) == PARK_ALIVE) {
          Arrive[Index] = (UINT32)Step;
          break;
        }

        //
        // TimerLib, not gBS->Stall: this driver's depex does not include the
        // Metronome arch protocol, and CoreStall returns EFI_NOT_AVAILABLE_YET (no delay)
        // until it is installed.
        //
        MicroSecondDelay (PARK_WAIT_STEP_US);
      }
    }

    DEBUG ((DEBUG_ERROR, "PARK: CPU_ON gicc%u mpidr=%lx entry=%lx -> %lx, mark after %d ms, dvm stage %u\n",
            (UINT32)Index, Gicc[Index]->MPIDR, Page[Index] + PARK_STUB_OFF, (UINT64)Smc.Arg0,
            (INT32)Arrive[Index], ParkRd32 (Page[Index] + PARK_STAGE_OFF)));
  }

  mParkProgress[1] = SavedProgress;   // every started core reported in or timed out - no SoC hang here
  ArmDataSynchronizationBarrier ();

#if BEYOND0LTE_PARK_SLEEP_UNOFFERED
  for (Index = 0; Index < GiccCount; Index++) {
    if ((((BEYOND0LTE_PARK_PCSAMPLE_MASK >> Index) & 1u) != 0) && (Page[Index] != 0) &&
        (ParkRd32 (Page[Index] + PARK_MARK_OFF) == PARK_ALIVE))
    {
      ParkPcSampleGo (Page[Index], Gicc, Page, GiccCount, BootMpidr);
    }
  }
#endif

  //
  // Patch the MADT: parked -> version 1 + mailbox; any other enabled secondary
  // -> disabled. The boot core is never touched.
  //
  Parked = 0;
  for (Index = 0; Index < GiccCount; Index++) {
    UINT32  Mark;

    if (GET_MPIDR_AFFINITY_BITS (Gicc[Index]->MPIDR) == BootMpidr) {
      continue;
    }

    Mark = (Page[Index] != 0) ? ParkRd32 (Page[Index] + PARK_MARK_OFF) : 0;
    if ((Page[Index] != 0) && (OnStatus[Index] == 0) && (Mark == PARK_ALIVE) &&
        (((BEYOND0LTE_SMP_OFFER_MASK >> Index) & 1u) == 0))
    {
      //
      // m14 M4 test: started and spinning in the stub, but NOT offered to Windows (MADT-disabled) - does the SoC
      // still freeze ~1 min in when the M4 cluster merely runs?
      //
      Gicc[Index]->Flags &= ~(UINT32)EFI_ACPI_6_3_GIC_ENABLED;
      DEBUG ((DEBUG_ERROR, "PARK: gicc%u running in the stub but NOT offered (offer mask %x) mpidr_el1=%lx gic-if %02x sleep %x\n",
              (UINT32)Index, (UINT32)BEYOND0LTE_SMP_OFFER_MASK, ParkRd64 (Page[Index] + PARK_MPIDR_OFF),
              ParkRd32 (Page[Index] + PARK_GICIF_OFF), ParkRd32 (Page[Index] + PARK_SLEEP_OFF)));
    } else if ((Page[Index] != 0) && (OnStatus[Index] == 0) && (Mark == PARK_ALIVE)) {
      Gicc[Index]->ParkingProtocolVersion = PARK_VERSION;
      Gicc[Index]->ParkedAddress          = Page[Index];
      Parked++;
      DEBUG ((DEBUG_ERROR, "PARK: gicc%u parked mb=%lx el=%lx mpidr_el1=%lx cntfrq=%lu (boot cpu %lu) gic-if mask %02x (MADT GicId %u, boot cpu mask %02x) (%u ms)\n",
              (UINT32)Index, Page[Index],
              ParkRd64 (Page[Index] + PARK_EL_OFF), ParkRd64 (Page[Index] + PARK_MPIDR_OFF),
              ParkRd64 (Page[Index] + PARK_CNTFRQ_OFF), ParkBootCntFrq (), ParkRd32 (Page[Index] + PARK_GICIF_OFF),
              Gicc[Index]->CPUInterfaceNumber, *(volatile UINT8 *)(UINTN)(EXYNOS_GICD_BASE + 0x800), Arrive[Index]));
    } else if ((Gicc[Index]->Flags & EFI_ACPI_6_3_GIC_ENABLED) != 0) {
      Gicc[Index]->Flags &= ~(UINT32)EFI_ACPI_6_3_GIC_ENABLED;
      DEBUG ((DEBUG_ERROR, "PARK: gicc%u mpidr=%lx disabled (page=%lx on=%lx mark=%x)\n",
              (UINT32)Index, Gicc[Index]->MPIDR, Page[Index], (UINT64)OnStatus[Index], Mark));
    }
  }

  //
  // m14: ID / cache-type registers per core (stub +0xF60..+0xFA8) next to the boot CPU's. Windows samples some
  // of these on the boot CPU only; a mismatch on the M4s is a candidate for the "Windows on the M4s dies" bug.
  //
  {
    UINT64  Id[PARK_ID_COUNT];

    ParkReadIds (Id);
    {
      UINT64  Ctl[2];

      __asm__ __volatile__ ("mrs %0, actlr_el1" : "=r" (Ctl[0]));
      __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Ctl[1]));
      DEBUG ((DEBUG_ERROR, "PARK: boot  ACTLR_EL1 %lx SCTLR_EL1 %lx (UEFI's, not entry)\n", Ctl[0], Ctl[1]));
    }
    DEBUG ((DEBUG_ERROR, "PARK: boot  ids MIDR %lx CTR %lx DCZID %lx ISAR0 %lx ISAR1 %lx MMFR0 %lx MMFR1 %lx MMFR2 %lx PFR0 %lx CLIDR %lx\n",
            Id[0], Id[1], Id[2], Id[3], Id[4], Id[5], Id[6], Id[7], Id[8], Id[9]));
    for (Index = 0; Index < GiccCount; Index++) {
      if ((Page[Index] == 0) || (ParkRd32 (Page[Index] + PARK_MARK_OFF) != PARK_ALIVE)) {
        continue;
      }

      DEBUG ((DEBUG_ERROR, "PARK: gicc%u ids MIDR %lx CTR %lx DCZID %lx ISAR0 %lx ISAR1 %lx MMFR0 %lx MMFR1 %lx MMFR2 %lx PFR0 %lx CLIDR %lx\n",
              (UINT32)Index,
              ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x00), ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x08),
              ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x10), ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x18),
              ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x20), ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x28),
              ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x30), ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x38),
              ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x40), ParkRd64 (Page[Index] + PARK_IDS_OFF + 0x48)));
      DEBUG ((DEBUG_ERROR, "PARK: gicc%u entry ACTLR_EL1 %lx SCTLR_EL1 %lx\n", (UINT32)Index,
              ParkRd64 (Page[Index] + PARK_CTL_OFF), ParkRd64 (Page[Index] + PARK_CTL_OFF + 8)));
    }
  }

  //
  // m14 PC sampler (stub sleep flag 5): show what it sees while the other parked cores sit in the stub poll loop
  // (their PCs must fall inside their own mailbox page + 0x800..) and the boot CPU runs this driver.
  //
  for (Index = 0; Index < GiccCount; Index++) {
    volatile UINT8  *Rec;
    UINT64          Next;
    UINTN           Core;
    UINT32          Wait;

    if ((((BEYOND0LTE_PARK_PCSAMPLE_MASK >> Index) & 1u) == 0) || (Page[Index] == 0)) {
      continue;
    }

    for (Wait = 0; (Wait < 1000) && (*(volatile UINT64 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x10) < 3); Wait++) {
      MicroSecondDelay (1000);
    }

    Next = *(volatile UINT64 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x8);
    Rec  = (volatile UINT8 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + ((Next > 1) ? Next - 1 : 1) * BEYOND0LTE_PCSAMP_SLOT);
    DEBUG ((DEBUG_ERROR, "PARK: PCS gicc%u reading core %x (0xFF = between samples)\n", (UINT32)Index,
            *(volatile UINT32 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x28)));
    DEBUG ((DEBUG_ERROR, "PARK: PCS gicc%u flag %x magic %x samples %lu faults %lu (esr %lx far %lx elr %lx)\n",
            (UINT32)Index, ParkRd32 (Page[Index] + PARK_SLEEP_OFF), *(volatile UINT32 *)(UINTN)BEYOND0LTE_PCSAMP_LIVE,
            *(volatile UINT64 *)(UINTN)(BEYOND0LTE_PCSAMP_LIVE + 0x10), ParkRd64 (Page[Index] + PARK_PCS_FAULT_OFF + 0x10),
            ParkRd64 (Page[Index] + PARK_PCS_FAULT_OFF), ParkRd64 (Page[Index] + PARK_PCS_FAULT_OFF + 0x8),
            ParkRd64 (Page[Index] + PARK_PCS_FAULT_OFF + 0x18)));
    for (Core = 0; Core < PARK_MAX_GICC; Core++) {
      volatile UINT32  *W;

      W = (volatile UINT32 *)(Rec + 0x10 + Core * 0x18);
      DEBUG ((DEBUG_ERROR, "PARK: PCS   cpu%u pc %08x%08x edscr %x vid %x prsr %x authstatus %x (mailbox %lx)\n",
              (UINT32)Core, W[3], W[0], W[1], W[2], W[4], W[5], Page[Core]));
    }
  }

  //
  // m14 cached stress (stub sleep flag 6): the core must report the stress mode and its pass counter must move.
  //
  for (Index = 0; Index < GiccCount; Index++) {
    UINT64  Beat0;

    if (((((BEYOND0LTE_PARK_STRESS_MASK | BEYOND0LTE_PARK_IRQ_MASK) >> Index) & 1u) == 0) || (Page[Index] == 0)) {
      continue;
    }

    Beat0 = ParkRd64 (Page[Index] + PARK_STRESS_BEAT_OFF);
    MicroSecondDelay (200000);
    DEBUG ((DEBUG_ERROR, "PARK: STRESS gicc%u flag %x passes %lu -> %lu in 200 ms; irq timer %lu sgi %lu other %lu lastiar %x spurious %lu\n",
            (UINT32)Index, ParkRd32 (Page[Index] + PARK_SLEEP_OFF), Beat0, ParkRd64 (Page[Index] + PARK_STRESS_BEAT_OFF),
            ParkRd64 (Page[Index] + PARK_IRQ_CNT_OFF), ParkRd64 (Page[Index] + PARK_IRQ_CNT_OFF + 8),
            ParkRd64 (Page[Index] + PARK_IRQ_CNT_OFF + 0x10), ParkRd32 (Page[Index] + PARK_IRQ_CNT_OFF + 0x18),
            ParkRd64 (Page[Index] + PARK_IRQ_CNT_OFF + 0x20)));
    if (((BEYOND0LTE_PARK_IRQ_MASK >> Index) & 1u) != 0) {
      DEBUG ((DEBUG_ERROR, "PARK: ATOMIC gicc%u opts %x rounds %lu (shared page %lx)\n", (UINT32)Index,
              ParkRd32 (Page[Index] + PARK_IRQ_OPTS_OFF), ParkRd64 (Page[Index] + PARK_ATOMIC_CNT_OFF), mParkAtomicPage));
    }
    if ((BEYOND0LTE_PARK_IRQ_OPTS & 1u) != 0) {
      DEBUG ((DEBUG_ERROR, "PARK: PMU gicc%u stage %u PMCR %lx PMCEID0 %lx ticks/1000 PMCCNTR reads %lu PMCCNTR %lu INST_RETIRED %lu last PMCCNTR %lu\n",
              (UINT32)Index, ParkRd32 (Page[Index] + PARK_PMU_OFF), ParkRd64 (Page[Index] + PARK_PMU_OFF + 0x08),
              ParkRd64 (Page[Index] + PARK_PMU_OFF + 0x10), ParkRd64 (Page[Index] + PARK_PMU_OFF + 0x18),
              ParkRd64 (Page[Index] + PARK_PMU_OFF + 0x20), ParkRd64 (Page[Index] + PARK_PMU_OFF + 0x28),
              ParkRd64 (Page[Index] + PARK_PMU_OFF + 0x30)));
    }
  }

  //
  // m14 coherency test: the test core runs cached (stub sleep flag 3) and echoes +0x100 into +0x140. Plain
  // volatile accesses here, NO cache maintenance (ParkRd32 would clean/invalidate and hide the bug).
  //
  for (Index = 0; Index < GiccCount; Index++) {
    volatile UINT32  *Ping;
    volatile UINT32  *Echo;
    UINT32           Round;
    UINT32           Spin;
    UINT32           Misses;
    UINT32           Step;
    UINT64           T0;
    UINT64           T1;

    if ((((BEYOND0LTE_PARK_COHERENCY_MASK >> Index) & 1u) == 0) || (Page[Index] == 0)) {
      continue;
    }

    for (Step = 0; (Step < 200) && (ParkRd32 (Page[Index] + PARK_SLEEP_OFF) != PARK_COH_READY); Step++) {
      MicroSecondDelay (1000);
    }

    if (ParkRd32 (Page[Index] + PARK_SLEEP_OFF) != PARK_COH_READY) {
      DEBUG ((DEBUG_ERROR, "PARK: COH gicc%u never reported cached mode (flag %x)\n",
              (UINT32)Index, ParkRd32 (Page[Index] + PARK_SLEEP_OFF)));
      continue;
    }

    Ping   = (volatile UINT32 *)(UINTN)(Page[Index] + PARK_COH_PING_OFF);
    Echo   = (volatile UINT32 *)(UINTN)(Page[Index] + PARK_COH_ECHO_OFF);
    Misses = 0;
    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (T0));
    for (Round = 1; (Round <= PARK_COH_ROUNDS) && (Misses < 20); Round++) {
      *Ping = Round;
      for (Spin = 0; (Spin < PARK_COH_SPIN) && (*Echo != Round); Spin++) {
      }

      if (*Echo != Round) {
        Misses++;
      }
    }

    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (T1));
    *Ping = 0xFFFFFFFFu;
    DEBUG ((DEBUG_ERROR, "PARK: COH gicc%u %u rounds, %u missed (echo %x), %lu ticks @26MHz\n",
            (UINT32)Index, Round - 1, Misses, *Echo, T1 - T0));
  }

#if BEYOND0LTE_SMP_REAL_MPIDR
  //
  // m14 run 17 + drivers-branch notes/psci-parking.md: the MADT carries Samsung's DT/PSCI numbering (A55 0x0-0x3,
  // A75 0x4-0x5, M4 0x100-0x101), which is what EL3's CPU_ON takes - but the cores REPORT different MPIDR_EL1
  // values (stub +0xFF0): A55/A75 0x81000100..0x81000500 (MT=1, core in Aff1), M4 0x80000100/0x80000101. So A55
  // core 1's real affinity 0x100 equals the M4 core 0 MADT entry: with the big cores offered, Windows can match
  // A55#1 to the M4's processor (run 17 hang). Publish the REAL affinity (MPIDR_EL1 & 0xFF00FFFFFF) for every
  // parked core and the boot core, then disable any later entry whose affinity duplicates an earlier one.
  // CPU_ON above already used the DT numbering; nothing after this point issues PSCI with these values.
  //
  for (Index = 0; Index < GiccCount; Index++) {
    if (GET_MPIDR_AFFINITY_BITS (Gicc[Index]->MPIDR) == BootMpidr) {
      Gicc[Index]->MPIDR = ArmReadMpidr () & PARK_MPIDR_AFF_MASK;
    } else if (Gicc[Index]->ParkingProtocolVersion == PARK_VERSION) {
      Gicc[Index]->MPIDR = ParkRd64 (Page[Index] + PARK_MPIDR_OFF) & PARK_MPIDR_AFF_MASK;
    }
  }

  for (Index = 0; Index < GiccCount; Index++) {
    UINTN  Earlier;

    if ((Gicc[Index]->Flags & EFI_ACPI_6_3_GIC_ENABLED) == 0) {
      continue;
    }

    for (Earlier = 0; Earlier < Index; Earlier++) {
      if (((Gicc[Earlier]->Flags & EFI_ACPI_6_3_GIC_ENABLED) != 0) && (Gicc[Earlier]->MPIDR == Gicc[Index]->MPIDR)) {
        Gicc[Index]->Flags                 &= ~(UINT32)EFI_ACPI_6_3_GIC_ENABLED;
        Gicc[Index]->ParkingProtocolVersion = 0;
        Gicc[Index]->ParkedAddress          = 0;
        Parked--;
        DEBUG ((DEBUG_ERROR, "PARK: gicc%u real mpidr %lx collides with gicc%u - disabled\n",
                (UINT32)Index, Gicc[Index]->MPIDR, (UINT32)Earlier));
        break;
      }
    }
  }

  for (Index = 0; Index < GiccCount; Index++) {
    DEBUG ((DEBUG_ERROR, "PARK: MADT gicc%u mpidr=%lx flags=%x ppv=%u\n", (UINT32)Index, Gicc[Index]->MPIDR,
            Gicc[Index]->Flags, Gicc[Index]->ParkingProtocolVersion));
  }
#endif

  DEBUG ((DEBUG_ERROR, "PARK: %u core(s) parked; FADT PSCI=%u\n",
          (UINT32)Parked, (UINT32)BEYOND0LTE_FADT_PSCI));
}

#endif // BEYOND0LTE_SMP_PARKING
