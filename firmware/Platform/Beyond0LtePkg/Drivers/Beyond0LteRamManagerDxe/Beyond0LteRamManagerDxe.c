/** @file
  Publishes the device-tree-verified Exynos 9820 DRAM banks as DXE GCD
  system-memory ranges for the Galaxy S10e (beyond0lte).

  Ported from Star2LtePkg/Drivers/Star2LteRamManagerDxe (Exynos 9810 / Galaxy
  S9+). The 9810's two hardcoded bank-1 ranges are NOT carried: they are 9810
  DRAM-shape facts (bank-1 at 0xC0000000 with the seclog and the framebuffer
  subtracted). The 9820 ranges come from the macros in
  Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h instead, and a range with
  an unread extent is skipped rather than guessed.

  Why this module is still needed at all: PlatformMemoryMapLib's
  ArmPlatformGetVirtualMemoryMap() feeds the ARM PAGE TABLES (which addresses
  are cached vs Device), while AddMemorySpace(EfiGcdMemoryTypeSystemMemory,…)
  feeds the DXE GLOBAL SYSTEM CONFIGURATION map, which is what the DXE core
  allocates from and what the memory map Windows finally sees. Publishing a
  bank in one does not publish it in the other.

  Follows the approach of Mu-Silicium's RamManagerDxe (Project Silicium,
  https://github.com/Project-Silicium/Mu-Silicium, SiliciumPkg): map extra
  DRAM from DXE with AddMemorySpace and the matching memory attributes.

  NOTHING HERE IS VALIDATED ON AN S10e.
**/

#include <Uefi.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Platform/Exynos9820.h>

//
// The 9810 manager stamped a status word at 0xFED17F60 so a post-mortem read
// could tell which range failed. That address is inside bank0 DRAM on this SoC
// (bank0 is 0x80000000 + 0x80000000) and inside the vendor's debug_snapshot
// region, so the channel has no legal 9820 address yet. Compiled out, not
// retargeted - the same hazard already recorded in
// PlatformMemoryMapLib.c, Beyond0LteAcpiPlatformDxe.c and
// Beyond0LteUfsHcDxe.c (notes/s10e-live-reads.md §7.3).
//
#define BEYOND0LTE_RAM_STATUS_DIAG  0

typedef struct {
  EFI_PHYSICAL_ADDRESS  Base;
  UINT64                Length;
} BEYOND0LTE_RAM_RANGE;

STATIC CONST BEYOND0LTE_RAM_RANGE  mRamRanges[] = {
  //
  // Two further 2 GiB banks above the 32-bit window. Both base AND size are
  // vendor DT reads (memory@880000000 reg = <0x8 0x80000000 0x80000000>,
  // memory@900000000 reg = <0x9 0x00000000 0x80000000>) - Exynos9820.h:87-90,
  // notes/s10e-live-reads.md §7.2.
  //
  { EXYNOS_DRAM_BANK3_BASE, EXYNOS_DRAM_BANK3_SIZE },
  { EXYNOS_DRAM_BANK4_BASE, EXYNOS_DRAM_BANK4_SIZE },
  //
  // The 9820 also declares banks at 0xC0000000 and 0xE1900000 (Exynos9820.h:83,
  // 85). Their BASES are reads but their SIZES are not - Exynos9820.h:84 and :86
  // carry 0 with the candidate extents in a comment - so they are left out of
  // this table entirely rather than published at a guessed length.
  //
};

#define BEYOND0LTE_RAM_RANGE_COUNT  (sizeof (mRamRanges) / sizeof (mRamRanges[0]))

STATIC
EFI_STATUS
Beyond0LteAddRamRange (
  IN CONST BEYOND0LTE_RAM_RANGE  *Range
  )
{
  EFI_STATUS  Status;

  //
  // Map the range before publishing it as conventional memory. AddMemorySpace
  // may grow DXE's memory-map bookkeeping and allocate from the newly added
  // highest range immediately.
  //
  Status = ArmSetMemoryAttributes (
             Range->Base,
             Range->Length,
             EFI_MEMORY_WB,
             0
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gDS->AddMemorySpace (
               EfiGcdMemoryTypeSystemMemory,
               Range->Base,
               Range->Length,
               EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB
               );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gDS->SetMemorySpaceAttributes (
               Range->Base,
               Range->Length,
               EFI_MEMORY_WB
               );
  if (EFI_ERROR (Status)) {
    gDS->RemoveMemorySpace (Range->Base, Range->Length);
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Beyond0LteRamManagerEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS       Status;
  EFI_STATUS       RollbackStatus;
  UINTN            Index;
  UINTN            MappedCount;
  UINTN            MappedIdx[BEYOND0LTE_RAM_RANGE_COUNT];
#if BEYOND0LTE_RAM_STATUS_DIAG
  volatile UINT32  *Result;

  //
  // 9810 address, kept for reference only - NOT usable on this SoC, which is
  // why BEYOND0LTE_RAM_STATUS_DIAG is 0.
  //
  Result    = (volatile UINT32 *)(UINTN)0xFED17F60ULL;
  Result[0] = 0x52414D32u;  // 'RAM2'
  Result[1] = MAX_UINT32;
  Result[2] = MAX_UINT32;
#endif

  MappedCount = 0;

  for (Index = 0; Index < BEYOND0LTE_RAM_RANGE_COUNT; Index++) {
    //
    // A zero length means the extent was never read (Exynos9820.h marks those
    // UNVERIFIED); skip it instead of publishing a zero- or guessed-size region.
    //
    if (mRamRanges[Index].Length == 0) {
      DEBUG ((
        DEBUG_WARN,
        "Beyond0LteRamManager: skipping bank with unread extent at 0x%Lx\n",
        mRamRanges[Index].Base
        ));
      continue;
    }

    Status = Beyond0LteAddRamRange (&mRamRanges[Index]);
#if BEYOND0LTE_RAM_STATUS_DIAG
    Result[Index + 1] = (UINT32)Status;
#endif
    ArmDataSynchronizationBarrier ();
    if (EFI_ERROR (Status)) {
      while (MappedCount > 0) {
        MappedCount--;
        RollbackStatus = gDS->RemoveMemorySpace (
                                 mRamRanges[MappedIdx[MappedCount]].Base,
                                 mRamRanges[MappedIdx[MappedCount]].Length
                                 );
        if (EFI_ERROR (RollbackStatus)) {
          DEBUG ((
            DEBUG_ERROR,
            "Beyond0LteRamManager: rollback failed for 0x%Lx: %r\n",
            mRamRanges[MappedIdx[MappedCount]].Base,
            RollbackStatus
            ));
        }
      }

      DEBUG ((
        DEBUG_ERROR,
        "Beyond0LteRamManager: failed to add 0x%Lx..0x%Lx: %r\n",
        mRamRanges[Index].Base,
        mRamRanges[Index].Base + mRamRanges[Index].Length,
        Status
        ));
      return Status;
    }

    MappedIdx[MappedCount++] = Index;
  }

  DEBUG ((
    DEBUG_INFO,
    "Beyond0LteRamManager: published %u verified high banks\n",
    (UINT32)BEYOND0LTE_RAM_RANGE_COUNT
    ));

  (VOID)ImageHandle;
  (VOID)SystemTable;
  return EFI_SUCCESS;
}
