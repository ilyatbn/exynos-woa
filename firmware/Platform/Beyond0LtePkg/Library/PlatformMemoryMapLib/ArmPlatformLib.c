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

#include "Beyond0LteCheckpoint.h"
#include <Platform/Exynos9820.h>

#define BEYOND0LTE_EARLY_WATCHDOG  1   // W25: unattended bring-up - see ArmPlatformInitialize

//
// Boot-bring-up exception catcher (AArch64/Beyond0LteDiagVectors.S,
// notes/m8-uefi-image.md B51): any EL1 exception before DXE's CpuDxe replaces
// VBAR_EL1 paints ESR/ELR/FAR/vector strips and parks instead of resetting.
//
#define BEYOND0LTE_DIAG_VECTORS  1

VOID
Beyond0LteInstallDiagVectors (
  VOID
  );

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
// CLOSED by two independent reads (notes/dock-desktop.md §16): the live S10e's
// own /proc/device-tree/cpus/ and the vendor DT agree on eight CPU nodes named
// by MPIDR —
//
//   cpu@0000 cpu@0001 cpu@0002 cpu@0003 cpu@0004 cpu@0005     Aff1 = 0
//   cpu@0100 cpu@0101                                        Aff1 = 1
//
// with identities ananke (MPIDR 0x0-0x3, Cortex-A55 class), prometheus (0x4-0x5)
// and cheetah (0x100-0x101, Samsung's own, implementer 0x53). So there are
// **TWO MPIDR clusters, sized 6 + 2**, and three efficiency classes inside them.
//
// The 9810 had two clusters of four, so its
//
//   CoresPerCluster = PcdCoreCount / PcdClusterCount;
//   position        = cluster * CoresPerCluster + core;
//
// was exact. Here 8 / 2 = 4, which would place the two BIG cores at linear
// positions 4 and 5 — but those slots belong to the MID cores. The error is not
// a wrong number in a table: that index selects per-core stack and structure
// slots inside the SEC entry path, so a wrong one is a memory bug during
// bootstrapping.
//
// The table below is therefore explicit. Cluster 0 holds six cores (ananke +
// prometheus), cluster 1 holds two (cheetah).
// ---------------------------------------------------------------------------
//
STATIC CONST UINTN  mCoresPerCluster[] = { 6, 2 };   // {ananke+prometheus}, {cheetah}

/**
   Per-core early init, called very early from PrePi for each booting CPU.
   sboot leaves DRAM and the debug UART configured, so nothing is mandatory yet.
**/
RETURN_STATUS
ArmPlatformInitialize (
  IN UINTN  MpId
  )
{
  BEYOND0LTE_CHECKPOINT (1420);   // CEntryPoint, SEC stack live (B39)
#if BEYOND0LTE_DIAG_VECTORS
  Beyond0LteInstallDiagVectors ();
#endif
  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1] = 0x60;   // W28 boot progress: SEC entered (shim 0x5x before)
  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[2] = 0;      // M16 USB status word: Beyond0LteUsbHostDxe sets it, DSDT XHC0._STA reads it
  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[3] = 0;      // M12 WLAN status word: Beyond0LtePcieDxe sets 'WLAN', DSDT WLAN._STA reads it
#if BEYOND0LTE_EARLY_WATCHDOG
  //
  // W25 dead man's switch (unattended bring-up). Boot #36 stalled on the logo after a watchdog
  // reset, before the panel console printed anything. From here on any UEFI hang ends in TWRP:
  // reboot reason = recovery, watchdog cl0 = 82 s (the stock s3c2410wdt start sequence, constants
  // in Exynos9820.h). Beyond0LteBootLogDxe sets the reason back to "normal" and re-arms at
  // ReadyToBoot / ExitBootServices, so only a firmware that never reaches the OS hand-off is
  // diverted. MMU is off here: plain device stores.
  //
  {
    volatile UINT32  *Pmu;
    volatile UINT32  *Wdt;

    Pmu = (volatile UINT32 *)(UINTN)EXYNOS_PMU_BASE;
    Wdt = (volatile UINT32 *)(UINTN)EXYNOS_WDT_BASE;
    Pmu[EXYNOS_PMU_INFORM3_OFFSET / 4] = EXYNOS_REBOOT_REASON_BASE | EXYNOS_REBOOT_MODE_RECOVERY;
    Wdt[EXYNOS_WDT_WTCON_OFFSET / 4]   = 0;
    Wdt[EXYNOS_WDT_WTCLRINT_OFFSET / 4] = 1;
    Pmu[(EXYNOS_PMU_WDT_NONCPU_INT_EN_OFFSET | EXYNOS_PMU_ATOMIC_SET_ALIAS) / 4] = EXYNOS_PMU_WDT_NONCPU_INT_EN_BIT;
    Wdt[EXYNOS_WDT_WTDAT_OFFSET / 4]   = EXYNOS_WDT_COUNT_82S;
    Wdt[EXYNOS_WDT_WTCNT_OFFSET / 4]   = EXYNOS_WDT_COUNT_82S;
    Wdt[EXYNOS_WDT_WTCON_OFFSET / 4]   = EXYNOS_WDT_WTCON_82S;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }
#endif
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
  BEYOND0LTE_CHECKPOINT (1300);   // SEC asm entry, before the SEC stack (B39)
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
  BEYOND0LTE_CHECKPOINT (1660);   // MMU on, HOBs built (B39)
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
  ((volatile UINT32 *)(UINTN)0x8FE0F000ULL)[1] = 0x63;   // W31 progress: MMU is on (MemoryPeim returned)
  *PpiListSize = sizeof (mBeyond0LtePlatformPpiList);
  *PpiList     = mBeyond0LtePlatformPpiList;
}
