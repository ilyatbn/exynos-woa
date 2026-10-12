/** @file
  M16 G9 (UEFI half): power the G3D domain on, restore its DTZPC through EL3, request 260 MHz from ACPM, and
  leave it on for Windows. Only compiled into the flow when BEYOND0LTE_GPU_POWER_ON is 1.
  Every step and constant: notes/gpu-m16-g3don.md H1-H7 and notes/gpu-m16-g2g3.md F3.

  Vendor order (pmucal_local_enable, cal-if/pmucal_local.c:37-60, then gpex set_clock):
    1. CMU_TOP PLL_CON2_PLL_G3D = 0x30000003                [SEQ] :556
    2. CMU_TOP PLL_CON0_PLL_G3D bit4 = 1                    [SEQ] :557
    3. PMU G3D_CONFIGURATION bit0 = 0, wait G3D_STATUS bit0 == 0                    :558-559
    4. PMU EMBEDDED_G3D_OUT atomic-set 0x2 (write PMU+0xE120), delay 280 us          :560-561
    5. PMU G3D_CONFIGURATION bit0 = 1, wait G3D_STATUS bit0 == 1, delay 280 us       :562-564
    6. PMU EMBEDDED_G3D_CONFIGURATION bit0 = 1, wait EMBEDDED_G3D_STATUS bit0 == 1   :565-566
    7. SMC 0x82000410 (1, 0x18410204, 2) = exynos_pd_tz_restore (pmucal_local.c:45-52); non-zero -> power off
    8. (restore list: empty on a first power-on, H5)
    9. ACPM DVFS FREQ_REQ {10, 260000 kHz, 0, 0} on channel 0 (H1-H3)
  Waits: 2000 us (pmucal_rae.c:102-116). Any failure in 1-7 -> PowerOffSafe() and stop.

  Deliberately NOT done (H6): bts_pd_sync, SMC_DRM_G3D_POWER_ON, the restore list.

  NOT VALIDATED ON HARDWARE (stage Guided).
**/

#include <Uefi.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Platform/Exynos9820.h>

#include "GpuProbeHw.h"
#include "GpuAcpm.h"
#include "GpuPower.h"

#define PMU     ((UINTN)EXYNOS_PMU_BASE)
#define CMUTOP  ((UINTN)EXYNOS_CMU_TOP_BASE)

#define PMU_EMB_CFG  (PMU + EXYNOS_PMU_EMBEDDED_G3D_CONFIGURATION_OFFSET)
#define PMU_EMB_ST   (PMU + EXYNOS_PMU_EMBEDDED_G3D_STATUS_OFFSET)
#define PMU_EMB_OUT  (PMU + EXYNOS_PMU_EMBEDDED_G3D_OUT_OFFSET)
#define PMU_G3D_CFG  (PMU + EXYNOS_PMU_G3D_CONFIGURATION_OFFSET)
#define PMU_G3D_ST   (PMU + EXYNOS_PMU_G3D_STATUS_OFFSET)
#define PLL_CON0     (CMUTOP + EXYNOS_CMU_TOP_PLL_G3D_CON0_OFFSET)
#define PLL_CON2     (CMUTOP + EXYNOS_CMU_TOP_PLL_G3D_CON2_OFFSET)

/**
  pmucal WRITE: full-mask write, or read-modify-write of Mask (pmucal_rae.c:130-140). Logs before/after.
**/
STATIC
VOID
SeqWrite (
  IN CONST CHAR8  *Step,
  IN CONST CHAR8  *Name,
  IN UINTN        Addr,
  IN UINT32       Mask,
  IN UINT32       Value
  )
{
  UINT32  Before;

  Before = MmioRead32 (Addr);
  MmioWrite32 (Addr, (Mask == MAX_UINT32) ? Value : ((Before & ~Mask) | (Value & Mask)));
  DEBUG ((DEBUG_WARN, "GpuProbe: on[%a] %a [%lx] %x -> %x\n", Step, Name, (UINT64)Addr, Before, MmioRead32 (Addr)));
}

/**
  pmucal WAIT: poll until (reg & Mask) == Want, 1 us steps, PMU_WAIT_TIMEOUT_US polls (pmucal_rae.c:102-116).
**/
STATIC
BOOLEAN
SeqWait (
  IN CONST CHAR8  *Step,
  IN CONST CHAR8  *Name,
  IN UINTN        Addr,
  IN UINT32       Mask,
  IN UINT32       Want
  )
{
  UINT32  Polls;
  UINT32  V;

  for (Polls = 0; ; Polls++) {
    V = MmioRead32 (Addr);
    if ((V & Mask) == Want) {
      DEBUG ((DEBUG_WARN, "GpuProbe: on[%a] wait %a [%lx] = %x ok after %u polls\n", Step, Name, (UINT64)Addr, V, Polls));
      return TRUE;
    }
    if (Polls >= PMU_WAIT_TIMEOUT_US) {
      DEBUG ((DEBUG_ERROR, "GpuProbe: on[%a] wait %a [%lx] = %x TIMEOUT (want bit %x == %x)\n",
              Step, Name, (UINT64)Addr, V, Mask, Want));
      return FALSE;
    }
    MicroSecondDelay (1);
  }
}

/**
  embedded_g3d_off ([SEQ] :586-594) WITHOUT its two CMU_G3D writes (QCH_CON_BUSIF_HPMG3D bit2, and
  MUX_CLKCMU_EMBEDDED_G3D_USER bit4): this runs after a failed step or a refused DTZPC restore, when an
  NS access to CMU_G3D may be secure-only and abort (g2g3 F6). Also WITHOUT the exynos_pd_tz_save SMC that
  pmucal_local_disable issues first (H4). G3D_CONFIGURATION (0x2380) is left as is - the vendor off
  sequence never clears it either.
**/
STATIC
VOID
PowerOffSafe (
  VOID
  )
{
  DEBUG ((DEBUG_WARN, "GpuProbe: powering the G3D domain back off (PMU + PLL only; CMU_G3D writes and the "
                      "tz_save SMC skipped)\n"));
  SeqWrite ("off", "EMBEDDED_G3D_CONFIGURATION", PMU_EMB_CFG, PMU_G3D_ON_BIT, 0);
  (VOID)SeqWait ("off", "EMBEDDED_G3D_STATUS", PMU_EMB_ST, PMU_G3D_ON_BIT, 0);
  MicroSecondDelay (PMU_G3D_OFF_DELAY_US);
  SeqWrite ("off", "PLL_CON0_PLL_G3D", PLL_CON0, PLL_G3D_CON0_BIT4, 0);
  SeqWrite ("off", "PLL_CON2_PLL_G3D", PLL_CON2, MAX_UINT32, PLL_G3D_CON2_OFF_VALUE);
}

BOOLEAN
GpuPowerOn (
  VOID
  )
{
  UINT32        EmbSt;
  UINT32        G3dSt;
  ARM_SMC_ARGS  Smc;
  UINT32        SmcRet;
  UINT32        Reply[4];
  EFI_STATUS    Status;

  EmbSt = MmioRead32 (PMU_EMB_ST);
  G3dSt = MmioRead32 (PMU_G3D_ST);
  if (((EmbSt & PMU_G3D_ON_BIT) != 0) && ((G3dSt & PMU_G3D_ON_BIT) != 0)) {
    DEBUG ((DEBUG_WARN, "GpuProbe: power-on: G3D domain already ON at entry - not touching it (no SMC, no ACPM)\n"));
    return FALSE;
  }
  if (((EmbSt & PMU_G3D_ON_BIT) != 0) || ((G3dSt & PMU_G3D_ON_BIT) != 0)) {
    DEBUG ((DEBUG_ERROR, "GpuProbe: power-on: inconsistent entry state (EMBEDDED_G3D_STATUS %x G3D_STATUS %x) - "
                         "not touching it\n", EmbSt, G3dSt));
    return FALSE;
  }

  DEBUG ((DEBUG_WARN, "GpuProbe: power-on: G3D domain OFF at entry - running pmucal embedded_g3d_on\n"));

  // 1-2. PLL_G3D (CMU_TOP, outside the domain)
  SeqWrite ("1", "PLL_CON2_PLL_G3D", PLL_CON2, MAX_UINT32, PLL_G3D_CON2_ON_VALUE);
  SeqWrite ("2", "PLL_CON0_PLL_G3D", PLL_CON0, PLL_G3D_CON0_BIT4, PLL_G3D_CON0_BIT4);

  // 3. G3D (outer) off first, as the vendor sequence does
  SeqWrite ("3", "G3D_CONFIGURATION", PMU_G3D_CFG, PMU_G3D_ON_BIT, 0);
  if (!SeqWait ("3", "G3D_STATUS", PMU_G3D_ST, PMU_G3D_ON_BIT, 0)) {
    goto Fail;
  }

  // 4. EMBEDDED_G3D_OUT atomic set (pmucal_set_bit_atomic: value written to offset | 0xC000)
  {
    UINTN   Alias  = PMU + (EXYNOS_PMU_EMBEDDED_G3D_OUT_OFFSET | EXYNOS_PMU_ATOMIC_SET_ALIAS);
    UINT32  Before = MmioRead32 (PMU_EMB_OUT);

    MmioWrite32 (Alias, PMU_EMBEDDED_G3D_OUT_VALUE);
    DEBUG ((DEBUG_WARN, "GpuProbe: on[4] EMBEDDED_G3D_OUT atomic-set %x via [%lx]: [%lx] %x -> %x\n",
            PMU_EMBEDDED_G3D_OUT_VALUE, (UINT64)Alias, (UINT64)PMU_EMB_OUT, Before, MmioRead32 (PMU_EMB_OUT)));
    MicroSecondDelay (PMU_G3D_ON_DELAY_US);
  }

  // 5. G3D (outer) on
  SeqWrite ("5", "G3D_CONFIGURATION", PMU_G3D_CFG, PMU_G3D_ON_BIT, PMU_G3D_ON_BIT);
  if (!SeqWait ("5", "G3D_STATUS", PMU_G3D_ST, PMU_G3D_ON_BIT, PMU_G3D_ON_BIT)) {
    goto Fail;
  }
  MicroSecondDelay (PMU_G3D_ON_DELAY_US);

  // 6. EMBEDDED_G3D on
  SeqWrite ("6", "EMBEDDED_G3D_CONFIGURATION", PMU_EMB_CFG, PMU_G3D_ON_BIT, PMU_G3D_ON_BIT);
  if (!SeqWait ("6", "EMBEDDED_G3D_STATUS", PMU_EMB_ST, PMU_G3D_ON_BIT, PMU_G3D_ON_BIT)) {
    goto Fail;
  }

  // 7. DTZPC restore through EL3 (exynos_pd_tz_restore). exynos_smc keeps the result as int32_t (H4).
  ZeroMem (&Smc, sizeof (Smc));
  Smc.Arg0 = EXYNOS_SMC_CMD_PREPARE_PD_ONOFF;
  Smc.Arg1 = EXYNOS_SMC_WAKEUP_PD_DOWN;
  Smc.Arg2 = EXYNOS_G3D_DTZPC_NEED_SMC;
  Smc.Arg3 = EXYNOS_SMC_RUNTIME_PM_TZPC_GROUP;
  DEBUG ((DEBUG_WARN, "GpuProbe: on[7] SMC %x (%x, %x, %x) DTZPC restore ...\n", EXYNOS_SMC_CMD_PREPARE_PD_ONOFF,
          EXYNOS_SMC_WAKEUP_PD_DOWN, EXYNOS_G3D_DTZPC_NEED_SMC, EXYNOS_SMC_RUNTIME_PM_TZPC_GROUP));
  ArmCallSmc (&Smc);
  SmcRet = (UINT32)Smc.Arg0;
  DEBUG ((DEBUG_WARN, "GpuProbe: on[7] SMC returned x0 %lx (low32 %x) -> %a\n", (UINT64)Smc.Arg0, SmcRet,
          (SmcRet == 0) ? "ok" : "REFUSED"));
  if (SmcRet != 0) {
    goto Fail;
  }

  DEBUG ((DEBUG_WARN, "GpuProbe: power-on: PMU EMBEDDED_G3D_STATUS %x G3D_STATUS %x after DTZPC restore\n",
          MmioRead32 (PMU_EMB_ST), MmioRead32 (PMU_G3D_ST)));

  // 9. Clock + voltage through ACPM (gpex set_clock_using_calapi -> cal_dfs_set_rate(0x0B04000A, 260000))
  Status = GpuAcpmDvfsFreqReq (ACPM_DVFS_G3D_IDX, ACPM_DVFS_G3D_START_KHZ, Reply);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GpuProbe: on[9] ACPM FREQ_REQ G3D (idx %u) %u kHz FAILED (%r) - domain stays ON, clock "
                         "not requested (PLL_G3D state in the re-probe)\n", ACPM_DVFS_G3D_IDX, ACPM_DVFS_G3D_START_KHZ, Status));
  } else {
    DEBUG ((DEBUG_WARN, "GpuProbe: on[9] ACPM FREQ_REQ G3D (idx %u) %u kHz -> reply %08x %08x %08x %08x (echo %a)\n",
            ACPM_DVFS_G3D_IDX, ACPM_DVFS_G3D_START_KHZ, Reply[0], Reply[1], Reply[2], Reply[3],
            (Reply[1] == ACPM_DVFS_G3D_START_KHZ) ? "yes" : "NO"));
  }

  MicroSecondDelay (GPU_DVFS_SETTLE_US);
  DEBUG ((DEBUG_WARN, "GpuProbe: power-on: done, domain left ON for the OS - re-probing\n"));
  return TRUE;

Fail:
  PowerOffSafe ();
  DEBUG ((DEBUG_ERROR, "GpuProbe: power-on FAILED - domain powered back off, no ACPM request sent\n"));
  return FALSE;
}
