/** @file
  ArmPlatformLib entry points for Beyond0LtePkg (Exynos 9820 / Galaxy S10e).

  Complements PlatformMemoryMapLib.c (which provides ArmPlatformGetVirtualMemoryMap)
  to form a complete ArmPlatformLib used by the ArmPlatformPkg PrePi boot flow.

  Mirrors Star2LtePkg's library for the Exynos 9810. NOTHING HERE IS VALIDATED
  on an S10e: this is the port's floor, and the boot it produces stays Guided
  until it has been observed on the device.

  These are minimal, "sboot already did the heavy lifting" implementations:
  DRAM and the debug UART clock/pinmux are assumed live on entry. Fill in real
  init only where a milestone proves it necessary.
**/

#include <Library/ArmPlatformLib.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Ppi/ArmMpCoreInfo.h>

//
// The 9810 flow appended single-byte breadcrumbs to 0xFED14000 so TWRP could
// read how far bring-up got. That address is above the S9+'s last DRAM bank.
// On the 9820 bank0 runs to 0x100000000, so 0xFED14000 is INSIDE conventional
// DRAM — writing there scribbles into memory the loader owns. The channel is
// compiled out until a reserved carveout is chosen from the vendor DT's
// reserved-memory node (el2_code, el2_earlymem, debug_snapshot, seclog_mem,
// cp_ram_logging, kaslr, abox-rmem). See PlatformMemoryMapLib.c.
//
#define BEYOND0LTE_PRAM_DIAG      0

#if BEYOND0LTE_PRAM_DIAG
#error The 9820 breadcrumb address has not been re-picked; see notes/dock-desktop.md §15
#endif

//
// ---------------------------------------------------------------------------
// Core positioning — this is the one place the 9810's math does NOT carry over.
//
// Verified on the S10e from /proc/cpuinfo: 8 logical CPUs in THREE clusters,
// sized 4 + 2 + 2, with three distinct core identities
//
//   processors 0-3   implementer 0x41 (ARM)      variant 0x1  part 0xd05
//   processors 4-5   implementer 0x41 (ARM)      variant 0x2  part 0xd0a
//   processors 6-7   implementer 0x53 (Samsung)  variant 0x1  part 0x003
//
// and three power domains in the live platform tree: 10090000.LITTLE,
// 10090000.MID, 10090000.BIG. The 9810 had two clusters of four, so its
//
//   CoresPerCluster = PcdCoreCount / PcdClusterCount;
//   position        = cluster * CoresPerCluster + core;
//
// was exact. Here 8 / 3 is not integral, so that formula would place the MID
// and BIG cores at wrong linear positions — and that index selects per-core
// stack and structure slots inside the SEC entry path. A wrong index is a
// memory bug during bootstrapping, not a cosmetic one.
//
// The table below is therefore explicit. The cluster ORDER (0 = LITTLE, 1 = MID,
// 2 = BIG) is an inference from the power-domain naming and the fact that
// GET_CLUSTER_ID comes from MPIDR affinity 1; it must be confirmed by reading
// MPIDR_EL1 on each core, which is the first on-device task here.
// ---------------------------------------------------------------------------
//
STATIC CONST UINTN  mCoresPerCluster[] = { 4, 2, 2 };   // LITTLE, MID, BIG

/**
   Per-core early init, called very early from PrePi for each booting CPU.
   sboot leaves DRAM and the debug UART configured, so nothing is mandatory yet.
**/
RETURN_STATUS
ArmPlatformInitialize (
  IN UINTN  MpId
  )
{
  // TODO: any clock/GPIO setup that MUST precede DRAM/UART use (likely none,
  // because sboot configured them before chainloading us).
  return RETURN_SUCCESS;
}

/**
   DRAM is already trained and online (sboot used it). Nothing to do.
**/
VOID
ArmPlatformInitializeSystemMemory (
  VOID
  )
{
}

/**
   Action taken at the very start of the PEI phase. No-op for now.
**/
VOID
ArmPlatformPeiBootAction (
  VOID
  )
{
  // The 9810 dropped a breadcrumb 'P' here — the first C function the SEC asm
  // entry calls, before the EDK II stack exists — because it was the cheapest
  // way to tell a bad branch target from a faulting prologue. There is no
  // equivalent channel on the 9820 yet, so this stays empty and the bring-up
  // question is answered by the debug UART instead.
}

/**
   Boot mode. We always do a full configuration boot (no S3/resume path yet).
**/
EFI_BOOT_MODE
ArmPlatformGetBootMode (
  VOID
  )
{
  return BOOT_WITH_FULL_CONFIGURATION;
}

/**
   Linear core position used to index per-core stacks/structures.

   Explicit, not a division: the clusters are 4 + 2 + 2.
**/
UINTN
ArmPlatformGetCorePosition (
  IN UINTN  MpId
  )
{
  UINTN  Cluster;
  UINTN  Core;
  UINTN  Position;
  UINTN  Index;

  Cluster = GET_CLUSTER_ID (MpId);
  Core    = GET_CORE_ID (MpId);

  if (Cluster >= ARRAY_SIZE (mCoresPerCluster)) {
    // An affinity outside the table is a fact we got wrong, not a state to
    // recover from. Say so and stay in range rather than indexing past it.
    DEBUG ((DEBUG_ERROR, "ArmPlatformGetCorePosition: cluster %d outside the 4+2+2 table\n",
      (UINT32)Cluster));
    return 0;
  }

  if (Core >= mCoresPerCluster[Cluster]) {
    DEBUG ((DEBUG_ERROR, "ArmPlatformGetCorePosition: core %d outside cluster %d\n",
      (UINT32)Core, (UINT32)Cluster));
    return 0;
  }

  Position = 0;
  for (Index = 0; Index < Cluster; Index++) {
    Position += mCoresPerCluster[Index];
  }
  Position += Core;

  return Position;
}

/**
   MpId of the core sboot boots us on. Almost certainly LITTLE cluster 0 core 0
   (MpId 0x0), as on the 9810, but that has not been observed on this SoC — the
   9810 confirmed it with a UART log in its first milestone.
**/
UINTN
ArmPlatformGetPrimaryCoreMpId (
  VOID
  )
{
  return (UINTN)PcdGet32 (PcdArmPrimaryCore);
}

/**
   TRUE if MpId is the primary (boot) core. Compare cluster+core affinity only.
**/
BOOLEAN
ArmPlatformIsPrimaryCore (
  IN UINTN  MpId
  )
{
  return (BOOLEAN)((MpId & (ARM_CLUSTER_MASK | ARM_CORE_MASK)) ==
                   (PcdGet32 (PcdArmPrimaryCore) & (ARM_CLUSTER_MASK | ARM_CORE_MASK)));
}

/**
   MP Core Info for beyond0lte / Exynos 9820.

   This SoC is an 8-core SMP part in three clusters, so MPIDR_EL1.U == 0 and
   ArmIsMpCore() returns TRUE. PeilessSec/SecMain therefore *requires* the
   ARM_MP_CORE_INFO_PPI — an empty PPI list makes SecMain CpuDeadLoop right
   after MemoryPeim (that was device-confirmed on the 9810, where the SEC
   breadcrumb trail stopped at 'F' with no 'G').

   We report the boot core only, exactly as the 9810 does. The mailbox fields
   are zeroed because secondary cores are started via PSCI (SMC), not a
   hardware mailbox. Expanding this table to all eight cores is what the MADT
   needs for Windows to report the right processor count, and the affinities
   have to be read from MPIDR_EL1 on the device rather than assumed — the
   cluster ORDER here is an inference (see the block above).
**/
STATIC ARM_CORE_INFO  mBeyond0LteCoreInfoTable[] = {
  {
    // Cluster 0, Core 0 — the primary/boot core (MPIDR affinity 0.0.0).
    0x0000000000000000ULL,   // Mpidr
    0, 0, 0, 0               // Mailbox Set/Get/Clear addresses + clear value (unused; PSCI)
  }
};

STATIC
EFI_STATUS
EFIAPI
Beyond0LteGetMpCoreInfo (
  OUT UINTN          *CoreCount,
  OUT ARM_CORE_INFO  **CoreInfoTable
  )
{
  if ((CoreCount == NULL) || (CoreInfoTable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *CoreCount     = ARRAY_SIZE (mBeyond0LteCoreInfoTable);
  *CoreInfoTable = mBeyond0LteCoreInfoTable;
  return EFI_SUCCESS;
}

STATIC ARM_MP_CORE_INFO_PPI  mBeyond0LteMpCoreInfoPpi = {
  Beyond0LteGetMpCoreInfo
};

STATIC EFI_PEI_PPI_DESCRIPTOR  mBeyond0LtePlatformPpiList[] = {
  {
    EFI_PEI_PPI_DESCRIPTOR_PPI | EFI_PEI_PPI_DESCRIPTOR_TERMINATE_LIST,
    &gArmMpCoreInfoPpiGuid,
    &mBeyond0LteMpCoreInfoPpi
  }
};

/**
   Provide the platform PEI PPI list. beyond0lte must publish the ARM MP Core
   Info PPI so PeilessSec/SecMain does not assert on an MP-capable part.
**/
VOID
ArmPlatformGetPlatformPpiList (
  OUT UINTN                   *PpiListSize,
  OUT EFI_PEI_PPI_DESCRIPTOR  **PpiList
  )
{
  *PpiListSize = sizeof (mBeyond0LtePlatformPpiList);
  *PpiList     = mBeyond0LtePlatformPpiList;
}
