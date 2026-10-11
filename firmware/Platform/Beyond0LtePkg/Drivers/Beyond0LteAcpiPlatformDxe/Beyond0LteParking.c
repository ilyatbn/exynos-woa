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
#define PARK_ID_INVALID     0xFFFFFFFFu
#define PARK_ALIVE          0x5A5ABEEFu
#define PARK_VERSION        1u

#define PARK_MAX_GICC       8u      // Madt.aslc BEYOND0LTE_NUM_CORES
#define PARK_WAIT_STEP_US   1000u
#define PARK_WAIT_STEPS     100u    // up to 100 ms for all marks (the 9810 waited 30 ms)

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

VOID
Beyond0LteParkSecondaries (
  IN OUT EFI_ACPI_DESCRIPTION_HEADER  *Madt
  )
{
  EFI_ACPI_6_3_GIC_STRUCTURE  *Gicc[PARK_MAX_GICC];
  UINT64                      Page[PARK_MAX_GICC];
  UINTN                       OnStatus[PARK_MAX_GICC];
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

  StubSize  = (UINTN)(Beyond0LteParkStubEnd - Beyond0LteParkStub);
  BootMpidr = GET_MPIDR_AFFINITY_BITS (ArmReadMpidr ());
  DEBUG ((DEBUG_ERROR, "PARK: mask=%x boot-mpidr=%lx stub=%u B\n",
          (UINT32)BEYOND0LTE_SMP_PARK_MASK, BootMpidr, (UINT32)StubSize));
  if ((StubSize == 0) || (PARK_STUB_OFF + StubSize > PARK_MARK_OFF)) {
    DEBUG ((DEBUG_ERROR, "PARK: stub size %u does not fit - nothing parked\n", (UINT32)StubSize));
    return;
  }

  //
  // Collect the GICC entries in table order; bit n of the mask = n-th GICC.
  //
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
      if (((BEYOND0LTE_SMP_PARK_MASK >> GiccCount) & 1u) &&
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
    if (!(((BEYOND0LTE_SMP_PARK_MASK >> Index) & 1u) &&
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
  }

  if (Pages != 0) {
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Pages, ParkCount * EFI_PAGE_SIZE);
    InvalidateInstructionCacheRange ((VOID *)(UINTN)Pages, ParkCount * EFI_PAGE_SIZE);
  }

  for (Index = 0; Index < GiccCount; Index++) {
    if (Page[Index] == 0) {
      continue;
    }

    ZeroMem (&Smc, sizeof (Smc));
    Smc.Arg0 = PARK_PSCI_CPU_ON;
    Smc.Arg1 = (UINTN)Gicc[Index]->MPIDR;
    Smc.Arg2 = (UINTN)(Page[Index] + PARK_STUB_OFF);
    Smc.Arg3 = (UINTN)Page[Index];   // context_id: unused by the stub (see the .S)
    ArmCallSmc (&Smc);
    OnStatus[Index] = Smc.Arg0;
    DEBUG ((DEBUG_ERROR, "PARK: CPU_ON gicc%u mpidr=%lx entry=%lx -> %lx\n",
            (UINT32)Index, Gicc[Index]->MPIDR, Page[Index] + PARK_STUB_OFF, (UINT64)Smc.Arg0));
  }

  //
  // Wait for each started core's own alive mark.
  //
  for (Step = 0; Step < PARK_WAIT_STEPS; Step++) {
    UINTN  Missing;

    Missing = 0;
    for (Index = 0; Index < GiccCount; Index++) {
      if ((Page[Index] != 0) && (OnStatus[Index] == 0) &&
          (ParkRd32 (Page[Index] + PARK_MARK_OFF) != PARK_ALIVE))
      {
        Missing++;
      }
    }

    if (Missing == 0) {
      break;
    }

    //
    // TimerLib, not gBS->Stall: this driver's depex does not include the
    // Metronome arch protocol, and CoreStall returns EFI_NOT_AVAILABLE_YET (no delay)
    // until it is installed.
    //
    MicroSecondDelay (PARK_WAIT_STEP_US);
  }

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
    if ((Page[Index] != 0) && (OnStatus[Index] == 0) && (Mark == PARK_ALIVE)) {
      Gicc[Index]->ParkingProtocolVersion = PARK_VERSION;
      Gicc[Index]->ParkedAddress          = Page[Index];
      Parked++;
      DEBUG ((DEBUG_ERROR, "PARK: gicc%u parked mb=%lx el=%lx mpidr_el1=%lx (%u ms)\n",
              (UINT32)Index, Page[Index],
              ParkRd64 (Page[Index] + PARK_EL_OFF), ParkRd64 (Page[Index] + PARK_MPIDR_OFF),
              (UINT32)Step));
    } else if ((Gicc[Index]->Flags & EFI_ACPI_6_3_GIC_ENABLED) != 0) {
      Gicc[Index]->Flags &= ~(UINT32)EFI_ACPI_6_3_GIC_ENABLED;
      DEBUG ((DEBUG_ERROR, "PARK: gicc%u mpidr=%lx disabled (page=%lx on=%lx mark=%x)\n",
              (UINT32)Index, Gicc[Index]->MPIDR, Page[Index], (UINT64)OnStatus[Index], Mark));
    }
  }

  DEBUG ((DEBUG_ERROR, "PARK: %u core(s) parked; FADT PSCI=%u\n",
          (UINT32)Parked, (UINT32)BEYOND0LTE_FADT_PSCI));
}

#endif // BEYOND0LTE_SMP_PARKING
