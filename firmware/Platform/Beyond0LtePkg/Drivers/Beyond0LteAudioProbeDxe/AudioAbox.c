/** @file
  D8a audio milestone 3, stage 0: READ-ONLY audit of the ABOX audio block that feeds the loudspeaker amps
  (ABOX UAIF1 -> CS35L40 x2). Nothing is written. Every offset and its source is in
  notes/d8-audio-m3-abox.md (F2-F7, "Stage 0").

  The AUD power domain is read first (PMU AUD_STATUS bit0). With pd-aud off, nothing inside the AUD block
  (CMU_AUD 0x18C00000 .. SysMMU 0x18E00000) is touched: an access to a powered-down block is treated as a
  hang. A hang anyway (e.g. a TZPC-protected SFR faulting) is covered by the 0x69 guard of the caller.

  NOT VALIDATED ON HARDWARE (stage Guided).
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>

#include <Platform/Exynos9820.h>

#include "AudioCodec.h"

#define PMU  ((UINTN)EXYNOS_PMU_BASE)
#define IFR  ((UINTN)EXYNOS_ABOX_IFR_BASE)
#define CMU  ((UINTN)EXYNOS_CMU_AUD_BASE)

#define ABOX_IP_INDEX_ABOX  0x41424F58u   // "ABOX", abox_soc.c reg_defaults_9820 row 1 (notes F2)

typedef struct {
  CONST CHAR8  *Name;
  UINTN        Addr;
} ABOX_REG;

STATIC CONST ABOX_REG  mIfrRegs[] = {
  { "VERSION",          IFR + 0x0004 },
  { "SYSPOWER_STATUS",  IFR + 0x0014 },
  { "QCHANNEL_DISABLE", IFR + 0x0038 },
  { "ROUTE_CTRL0",      IFR + 0x0040 },
  { "SPUS_CTRL0",       IFR + 0x0200 },
  { "SPUS_CTRL1",       IFR + 0x0204 },
  { "SPUS_SBANK_RDMA0", IFR + 0x0220 },
  { "UAIF1_CTRL0",      IFR + 0x0510 },
  { "UAIF1_CTRL1",      IFR + 0x0514 },
  { "UAIF1_STATUS",     IFR + 0x051C },
  { "RDMA0_CTRL0",      IFR + 0x1000 },
  { "RDMA0_+04",        IFR + 0x1004 },
  { "RDMA0_+08",        IFR + 0x1008 },
  { "RDMA0_+0C",        IFR + 0x100C },
  { "RDMA0_+10",        IFR + 0x1010 },
  { "RDMA0_+14",        IFR + 0x1014 },
  { "RDMA0_+18",        IFR + 0x1018 },
  { "RDMA0_+1C",        IFR + 0x101C },
  { "RDMA0_+20",        IFR + 0x1020 },
  { "RDMA0_+24",        IFR + 0x1024 },
  { "RDMA0_+28",        IFR + 0x1028 },
  { "RDMA0_+2C",        IFR + 0x102C },
  { "RDMA0_+30",        IFR + 0x1030 },
};

STATIC CONST ABOX_REG  mOtherRegs[] = {
  { "CMU_AUD PLL_AUD0_CON0",   CMU + 0x0120 },
  { "CMU_AUD PLL_AUD0_CON3",   CMU + 0x012C },
  { "CMU_AUD MUX_UAIF1",       CMU + 0x1010 },
  { "CMU_AUD DIV_AUDIF",       CMU + 0x1800 },
  { "CMU_AUD DIV_BUS",         CMU + 0x1804 },
  { "CMU_AUD DIV_UAIF1",       CMU + 0x1830 },
  { "CMU_AUD GATE_ACLK",       CMU + 0x2010 },
  { "CMU_AUD GATE_BCLK_UAIF1", CMU + 0x2024 },
  { "SYSREG_AUD MISC_CON",     (UINTN)EXYNOS_ABOX_SYSREG_BASE + 0x0504 },
  { "SYSMMU_AUD CTRL",         (UINTN)EXYNOS_ABOX_SYSMMU_BASE + 0x0000 },
  { "SYSMMU_AUD CFG",          (UINTN)EXYNOS_ABOX_SYSMMU_BASE + 0x0004 },
  { "SYSMMU_AUD VERSION",      (UINTN)EXYNOS_ABOX_SYSMMU_BASE + 0x0034 },
  { "GPIO_AUD gpb1 CON",       (UINTN)EXYNOS_PINCTRL_AUD_BASE + 0x0020 },
  { "GPIO_AUD gpb1 PUD",       (UINTN)EXYNOS_PINCTRL_AUD_BASE + 0x0028 },
};

STATIC
VOID
DumpRegs (
  IN CONST ABOX_REG  *Regs,
  IN UINTN           Count
  )
{
  UINTN  I;

  for (I = 0; I < Count; I++) {
    DEBUG ((DEBUG_WARN, "AudioAbox:   %a [%lx] = %x\n", Regs[I].Name, (UINT64)Regs[I].Addr, MmioRead32 (Regs[I].Addr)));
  }
}

BOOLEAN
AboxAudit (
  VOID
  )
{
  UINT32  Config;
  UINT32  Status;
  UINT32  Pad;
  UINT32  Ip;

  Config = MmioRead32 (PMU + EXYNOS_PMU_AUD_CONFIGURATION_OFFSET);
  Status = MmioRead32 (PMU + EXYNOS_PMU_AUD_STATUS_OFFSET);
  Pad    = MmioRead32 (PMU + EXYNOS_PMU_AUD_PAD_OFFSET);
  DEBUG ((DEBUG_WARN, "AudioAbox: stage 0 (read-only): pd-aud CONFIGURATION %x STATUS %x (bit0 = on), pad %x (bit11 = normal)\n",
          Config, Status, Pad));
  if ((Status & 1u) == 0) {
    DEBUG ((DEBUG_WARN, "AudioAbox: pd-aud is OFF at this point - nothing in the AUD block is read\n"));
    return FALSE;
  }

  // One read first: a TZPC-protected (secure-only) block would fault or read garbage here.
  Ip = MmioRead32 (IFR);
  DEBUG ((DEBUG_WARN, "AudioAbox: ABOX IP_INDEX [%lx] = %x (%a)\n", (UINT64)IFR, Ip,
          Ip == ABOX_IP_INDEX_ABOX ? "\"ABOX\" - non-secure access works" : "NOT \"ABOX\" - stopping"));
  if (Ip != ABOX_IP_INDEX_ABOX) {
    return FALSE;
  }

  DumpRegs (mIfrRegs, ARRAY_SIZE (mIfrRegs));
  DumpRegs (mOtherRegs, ARRAY_SIZE (mOtherRegs));
  DEBUG ((DEBUG_WARN, "AudioAbox: stage 0 done\n"));
  return TRUE;
}
