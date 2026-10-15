/** @file
  D10 video (MFC) UEFI probe (notes/d10-mfc-main.md). Constants from Exynos9820.h.

  R1 (BEYOND0LTE_MFC_PROBE): read PMU MFC_STATUS; only if pd-mfc is on, read CMU_MFC (mux / ACLK gate / QCH) and the
  MFC version register (0xF028, expect ip_ver 0x1300). Zero writes.
  R2 (BEYOND0LTE_MFC_POWER): if pd-mfc is off, power it on the way the vendor PMU table does (MFC_CONFIGURATION bit0,
  wait MFC_STATUS bit0), then the R1 reads. The vendor also issues an SMC (DTZPC restore, need_smc 0x18820204) after
  power-on that this does NOT do - if the version reads 0 / faults, that is the reason to look at first.

  Guard: progress word [1] = 0x6C while it runs; a boot whose previous boot stopped at 0x6C skips it.
  NOTHING HERE IS VALIDATED ON AN S10e YET.
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Platform/Exynos9820.h>

#ifndef BEYOND0LTE_MFC_PROBE
#define BEYOND0LTE_MFC_PROBE  0
#endif
#ifndef BEYOND0LTE_MFC_POWER
#define BEYOND0LTE_MFC_POWER  0
#endif

#define MFC_PROGRESS_TRYING  0x6Cu
STATIC volatile UINT32  *CONST  mProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

STATIC
BOOLEAN
MfcPowered (
  VOID
  )
{
  return (MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_STATUS) & 0x1) != 0;
}

STATIC
VOID
MfcReadIds (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "[MFC] CMU_MFC: mfc user mux %08x, mfc aclk gate %08x, mfc qch %08x\n",
          MmioRead32 ((UINTN)EXYNOS_CMU_MFC_BASE + EXYNOS_CMU_MFC_MFC_USER_MUX),
          MmioRead32 ((UINTN)EXYNOS_CMU_MFC_BASE + EXYNOS_CMU_MFC_MFC_ACLK),
          MmioRead32 ((UINTN)EXYNOS_CMU_MFC_BASE + EXYNOS_CMU_MFC_MFC_QCH)));
  DEBUG ((DEBUG_ERROR, "[MFC] R1: version=0x%x (ip_ver 0x%x expected), risc_on %x\n",
          MmioRead32 ((UINTN)EXYNOS_MFC_BASE + MFC_REG_MFC_VERSION), EXYNOS_MFC_IP_VER,
          MmioRead32 ((UINTN)EXYNOS_MFC_BASE + MFC_REG_RISC_ON)));
}

EFI_STATUS
EFIAPI
Beyond0LteMfcEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Saved;
  UINTN   Wait;

  if (!BEYOND0LTE_MFC_PROBE && !BEYOND0LTE_MFC_POWER) {
    return EFI_SUCCESS;
  }

  if (mProgress[0] == MFC_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "[MFC] previous boot hung in the MFC probe (progress 0x6C) - skipped this boot\n"));
    return EFI_SUCCESS;
  }

  Saved        = mProgress[1];
  mProgress[1] = MFC_PROGRESS_TRYING;
  DEBUG ((DEBUG_ERROR, "[MFC] pd-mfc: configuration %08x status %08x\n",
          MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_CONFIGURATION),
          MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_STATUS)));

  if (!MfcPowered () && BEYOND0LTE_MFC_POWER) {
    MmioOr32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_CONFIGURATION, 0x1);
    for (Wait = 0; (Wait < 1000) && !MfcPowered (); Wait++) {
      MicroSecondDelay (100);
    }

    DEBUG ((DEBUG_ERROR, "[MFC] R2: power-on -> status %08x after %u x 100 us\n",
            MmioRead32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_MFC_STATUS), (UINT32)Wait));
  }

  if (MfcPowered ()) {
    MfcReadIds ();
  } else {
    DEBUG ((DEBUG_ERROR, "[MFC] pd-mfc is off - no CMU_MFC / MFC reads (R2 powers it on)\n"));
  }

  mProgress[1] = Saved;
  return EFI_SUCCESS;
}
