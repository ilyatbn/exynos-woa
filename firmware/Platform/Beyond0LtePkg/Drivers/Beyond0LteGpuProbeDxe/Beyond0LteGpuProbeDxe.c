/** @file
  M16 G3: read-only probe of the Mali-G76 ("G3D") block (+ M16 G9 power-on behind a flag, GpuPower.c). Logs to UEFILOG with the "GpuProbe:" prefix.
  Recipe and every constant: notes/gpu-m16-g2g3.md (G2 findings F1-F6, "G3 design").

  Order, each step only if the previous one allows it:
    1. PMU (always on): EMBEDDED_G3D CONFIGURATION/STATUS (0x2100/0x2104), EMBEDDED_G3D_OUT (0x2120),
       G3D CONFIGURATION/STATUS (0x2380/0x2384). 0x2104 bit0 is the vendor kbase's own "GPU powered" test
       (mali DT gpu_pmu_status_reg_offset); the vendor on-sequence also waits for 0x2384 bit0.
    2. CMU_TOP (always on): PLL_G3D LOCKTIME/CON0/CON2 - logged whatever the domain state.
    3. Both PMU status bits set -> CMU_G3D muxes/dividers/gates (inside the domain, lost at power-down).
    4. GPU clock gate not forced off (MANUAL=1 && CG_VAL=0) -> GPU_ID alone first; 0 or ~0 stops (bus
       decode / secure-only canary). Then the read-only ID/status registers panfrost reads at init.
  The probe itself writes nothing to any of these blocks.

  BEYOND0LTE_GPU_POWER_ON (default 0; M16 G9, notes/gpu-m16-g3don.md): after the as-found probe, if the G3D
  domain is OFF, GpuPower.c runs the pmucal embedded_g3d_on sequence (PMU/CMU_TOP writes), the DTZPC restore
  SMC (0x82000410) and an ACPM DVFS FREQ_REQ for 260 MHz, leaves the domain ON for the OS, and probes again.
  If the domain is already ON (EMBEDDED_G3D_STATUS bit0, the vendor's test) it is not touched. With the flag 0 nothing is written anywhere.

  Guard: boot progress word 0x8FE0F000[1] = 0x6A while the probe AND the power-on run; a boot whose previous
  boot ended at 0x6A skips the whole driver (same self-healing shape as AudioProbeDxe's 0x69 / UsbHostDxe's 0x67).

  NOT VALIDATED ON HARDWARE (stage Guided).
**/

#include <Uefi.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Platform/Exynos9820.h>

#include "GpuProbeHw.h"
#include "GpuPower.h"

// 0 = the driver does nothing (one log line).
#ifndef BEYOND0LTE_GPU_PROBE
#define BEYOND0LTE_GPU_PROBE  1
#endif

// M16 G9: 1 = power the G3D on (PMU + SMC + ACPM writes, GpuPower.c) when it is OFF at entry. Committed 0;
// the test image sets it to 1. NOT validated on hardware.
#ifndef BEYOND0LTE_GPU_POWER_ON
#define BEYOND0LTE_GPU_POWER_ON  0
#endif

#define GPU_PROGRESS_TRYING  0x6Au
STATIC volatile UINT32  *CONST  mProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

#define PMU     ((UINTN)EXYNOS_PMU_BASE)
#define CMUTOP  ((UINTN)EXYNOS_CMU_TOP_BASE)
#define CMUG3D  ((UINTN)EXYNOS_CMU_G3D_BASE)
#define GPU     ((UINTN)EXYNOS_G3D_BASE)

typedef struct {
  CONST CHAR8  *Name;
  UINTN        Addr;
} GPU_PROBE_REG;

STATIC CONST GPU_PROBE_REG  mCmuG3dRegs[] = {
  { "MUX_EMBEDDED_G3D_USER", CMUG3D + CMU_G3D_MUX_EMBEDDED_G3D_USER },
  { "MUX_G3D_BUS_USER",      CMUG3D + CMU_G3D_MUX_G3D_BUS_USER      },
  { "MUX_G3D_SWITCH_USER",   CMUG3D + CMU_G3D_MUX_G3D_SWITCH_USER   },
  { "CONTROLLER_OPTION",     CMUG3D + CMU_G3D_CONTROLLER_OPTION     },
  { "EMB_CONTROLLER_OPTION", CMUG3D + CMU_G3D_EMB_CONTROLLER_OPTION },
  { "MUX_CLK_G3D_BUSD",      CMUG3D + CMU_G3D_MUX_CLK_G3D_BUSD      },
  { "DIV_CLK_G3D_BUSD",      CMUG3D + CMU_G3D_DIV_CLK_G3D_BUSD      },
  { "DIV_CLK_G3D_BUSP",      CMUG3D + CMU_G3D_DIV_CLK_G3D_BUSP      },
  { "GATE_CMU_PCLK",         CMUG3D + CMU_G3D_GATE_CMU_PCLK         },
};

STATIC CONST GPU_PROBE_REG  mGpuRegs[] = {
  { "L2_FEATURES",        GPU + GPU_L2_FEATURES        },
  { "CORE_FEATURES",      GPU + GPU_CORE_FEATURES      },
  { "TILER_FEATURES",     GPU + GPU_TILER_FEATURES     },
  { "MEM_FEATURES",       GPU + GPU_MEM_FEATURES       },
  { "MMU_FEATURES",       GPU + GPU_MMU_FEATURES       },
  { "AS_PRESENT",         GPU + GPU_AS_PRESENT         },
  { "JS_PRESENT",         GPU + GPU_JS_PRESENT         },
  { "GPU_INT_RAWSTAT",    GPU + GPU_INT_RAWSTAT        },
  { "GPU_STATUS",         GPU + GPU_STATUS             },
  { "GPU_FAULT_STATUS",   GPU + GPU_FAULT_STATUS       },
  { "THREAD_MAX_THREADS", GPU + GPU_THREAD_MAX_THREADS },
  { "THREAD_FEATURES",    GPU + GPU_THREAD_FEATURES    },
  { "SHADER_PRESENT_LO",  GPU + GPU_SHADER_PRESENT_LO  },
  { "SHADER_PRESENT_HI",  GPU + GPU_SHADER_PRESENT_HI  },
  { "TILER_PRESENT_LO",   GPU + GPU_TILER_PRESENT_LO   },
  { "L2_PRESENT_LO",      GPU + GPU_L2_PRESENT_LO      },
  { "SHADER_READY_LO",    GPU + GPU_SHADER_READY_LO    },
  { "TILER_READY_LO",     GPU + GPU_TILER_READY_LO     },
  { "L2_READY_LO",        GPU + GPU_L2_READY_LO        },
  { "COHERENCY_FEATURES", GPU + GPU_COHERENCY_FEATURES },
  { "STACK_PRESENT_LO",   GPU + GPU_STACK_PRESENT_LO   },
  { "JOB_INT_RAWSTAT",    GPU + GPU_JOB_INT_RAWSTAT    },
  { "MMU_INT_RAWSTAT",    GPU + GPU_MMU_INT_RAWSTAT    },
  { "AS0_STATUS",         GPU + GPU_AS0_STATUS         },
};

STATIC
VOID
DumpRegs (
  IN CONST CHAR8          *Block,
  IN CONST GPU_PROBE_REG  *Regs,
  IN UINTN                Count
  )
{
  UINTN  I;

  for (I = 0; I < Count; I++) {
    DEBUG ((DEBUG_WARN, "GpuProbe:   %a %a [%lx] = %x\n", Block, Regs[I].Name, (UINT64)Regs[I].Addr,
            MmioRead32 (Regs[I].Addr)));
  }
}

STATIC
UINT32
PopCount32 (
  IN UINT32  V
  )
{
  UINT32  N;   // printed with %u: must stay 32-bit

  for (N = 0; V != 0; V &= V - 1) {
    N++;
  }

  return N;
}

STATIC
VOID
GpuProbe (
  VOID
  )
{
  UINT32  EmbCfg, EmbSt, EmbOut, G3dCfg, G3dSt;
  UINT32  PllLock, Pll0, Pll2;
  UINT32  Gate;
  UINT32  Id;
  UINT32  Shader;

  // 1. PMU - always-on block, mapped since M8.
  EmbCfg = MmioRead32 (PMU + EXYNOS_PMU_EMBEDDED_G3D_CONFIGURATION_OFFSET);
  EmbSt  = MmioRead32 (PMU + EXYNOS_PMU_EMBEDDED_G3D_STATUS_OFFSET);
  EmbOut = MmioRead32 (PMU + EXYNOS_PMU_EMBEDDED_G3D_OUT_OFFSET);
  G3dCfg = MmioRead32 (PMU + EXYNOS_PMU_G3D_CONFIGURATION_OFFSET);
  G3dSt  = MmioRead32 (PMU + EXYNOS_PMU_G3D_STATUS_OFFSET);
  DEBUG ((DEBUG_WARN, "GpuProbe: PMU EMBEDDED_G3D CONFIG %x STATUS %x OUT %x | G3D CONFIG %x STATUS %x (bit0 = on)\n",
          EmbCfg, EmbSt, EmbOut, G3dCfg, G3dSt));

  // 2. PLL_G3D lives in CMU_TOP (CMU_CMU), outside the G3D domain.
  PllLock = MmioRead32 (CMUTOP + EXYNOS_CMU_TOP_PLL_G3D_LOCKTIME_OFFSET);
  Pll0    = MmioRead32 (CMUTOP + EXYNOS_CMU_TOP_PLL_G3D_CON0_OFFSET);
  Pll2    = MmioRead32 (CMUTOP + EXYNOS_CMU_TOP_PLL_G3D_CON2_OFFSET);
  DEBUG ((DEBUG_WARN, "GpuProbe: PLL_G3D CON0 %x (ENABLE %u STABLE %u M %u P %u S %u) CON2 %x LOCKTIME %x\n",
          Pll0, (Pll0 & PLL_CON0_ENABLE) ? 1 : 0, (Pll0 & PLL_CON0_STABLE) ? 1 : 0,
          PLL_CON0_M (Pll0), PLL_CON0_P (Pll0), PLL_CON0_S (Pll0), Pll2, PllLock));

  if (((EmbSt & 1u) == 0) || ((G3dSt & 1u) == 0)) {
    DEBUG ((DEBUG_WARN, "GpuProbe: G3D domain is OFF (EMBEDDED_G3D_STATUS bit0 %u, G3D_STATUS bit0 %u) - "
                        "nothing in CMU_G3D / GPU is read\n", EmbSt & 1u, G3dSt & 1u));
    return;
  }

  // 3. CMU_G3D - inside the domain, read only now.
  DEBUG ((DEBUG_WARN, "GpuProbe: G3D domain is ON - reading CMU_G3D\n"));
  DumpRegs ("CMU_G3D", mCmuG3dRegs, ARRAY_SIZE (mCmuG3dRegs));
  Gate = MmioRead32 (CMUG3D + CMU_G3D_GATE_GPU_CLK);
  DEBUG ((DEBUG_WARN, "GpuProbe: CMU_G3D GATE_GPU_CLK [%lx] = %x (MANUAL %u CG_VAL %u)\n",
          (UINT64)(CMUG3D + CMU_G3D_GATE_GPU_CLK), Gate, (Gate & CMU_GATE_MANUAL) ? 1 : 0,
          (Gate & CMU_GATE_CG_VAL) ? 1 : 0));
  if (((Gate & CMU_GATE_MANUAL) != 0) && ((Gate & CMU_GATE_CG_VAL) == 0)) {
    DEBUG ((DEBUG_WARN, "GpuProbe: GPU clock is manually gated off - no GPU register is read\n"));
    return;
  }

  // 4. One GPU read first: a secure-only (D_TZPC) or undecoded window would fault or read 0 / ~0 here.
  Id = MmioRead32 (GPU + GPU_ID);
  DEBUG ((DEBUG_WARN, "GpuProbe: GPU_ID [%lx] = %x (arch %u.%u r%u, product_major %u, ver r%up%u status %u; "
                      "G76 0x7001/0xF00F match: %a)\n",
          (UINT64)(GPU + GPU_ID), Id, GPU_ID_ARCH_MAJOR (Id), GPU_ID_ARCH_MINOR (Id), GPU_ID_ARCH_REV (Id),
          GPU_ID_PRODUCT_MAJOR (Id), GPU_ID_VER_MAJOR (Id), GPU_ID_VER_MINOR (Id), GPU_ID_VER_STATUS (Id),
          (((Id >> 16) & GPU_ID_G76_MASK) == GPU_ID_G76_MODEL) ? "yes" : "NO"));
  if ((Id == 0) || (Id == MAX_UINT32)) {
    DEBUG ((DEBUG_WARN, "GpuProbe: GPU_ID is %x - stopping (no further GPU reads)\n", Id));
    return;
  }

  DumpRegs ("GPU", mGpuRegs, ARRAY_SIZE (mGpuRegs));
  Shader = MmioRead32 (GPU + GPU_SHADER_PRESENT_LO);
  DEBUG ((DEBUG_WARN, "GpuProbe: shader cores present (popcount SHADER_PRESENT_LO) = %u, L2 slices = %u, "
                      "AS = %u, JS = %u\n",
          PopCount32 (Shader), PopCount32 (MmioRead32 (GPU + GPU_L2_PRESENT_LO)),
          PopCount32 (MmioRead32 (GPU + GPU_AS_PRESENT)), PopCount32 (MmioRead32 (GPU + GPU_JS_PRESENT))));
}

EFI_STATUS
EFIAPI
Beyond0LteGpuProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Saved;

  if (!BEYOND0LTE_GPU_PROBE) {
    DEBUG ((DEBUG_WARN, "GpuProbe: disabled (BEYOND0LTE_GPU_PROBE 0)\n"));
    return EFI_SUCCESS;
  }

  if (mProgress[0] == GPU_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "GpuProbe: previous boot hung in the GPU probe (progress 0x6A) - skipping it\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_WARN, "GpuProbe: start (BEYOND0LTE_GPU_POWER_ON %u)\n", BEYOND0LTE_GPU_POWER_ON));
  Saved        = mProgress[1];
  mProgress[1] = GPU_PROGRESS_TRYING;
  __asm__ __volatile__ ("dsb sy" ::: "memory");   // guard word out before the first risky access

  GpuProbe ();

  if (BEYOND0LTE_GPU_POWER_ON) {
    if (GpuPowerOn ()) {
      GpuProbe ();     // after power-on: PLL_G3D (STABLE = lock), CMU_G3D gate, GPU_ID ...
    }
  }

  mProgress[1] = Saved;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  DEBUG ((DEBUG_WARN, "GpuProbe: done\n"));
  return EFI_SUCCESS;
}
