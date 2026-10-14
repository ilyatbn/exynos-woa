/** @file
  Beyond0LteCheckpoint - boot-bring-up progress bands painted straight into the
  sboot scanout buffer (notes/m8-uefi-image.md B39).

  Boots #5-#8 showed the shim's bands, then a SOFT reset that is neither a UEFI
  ResetSystem call (B34) nor a SoC watchdog (B38). These checkpoints place the
  fault: each one paints a 100-row WHITE band at a fixed row, so the lowest band
  on the panel is the last checkpoint reached. The panel is 1080 x 2280
  (notes/m8-uefi-image.md B44); boot #9's rows 2100-3030 were below it.

    row 1300  ArmPlatformPeiBootAction   SEC asm entry, before the SEC stack
    row 1420  ArmPlatformInitialize      CEntryPoint, SEC stack live
    row 1540  ArmPlatformGetVirtualMemoryMap   MemoryPeim, MMU still off
    row 1660  ArmPlatformGetBootMode     MMU on, HOBs built
    row 1780  SimpleFramebufferDxeEntry  DXE core running, drivers dispatching

  (The shim uses rows 150-1199; ResetHold in Beyond0LteResetSystemLib uses
  1950-2149.) The macro is a plain volatile store loop - no call, no memset -
  so it is safe in ArmPlatformPeiBootAction, which runs before the SEC stack is
  set up. White (0xFFFFFFFF) so the unread pixel format does not matter.
  FB base / geometry come from Exynos9820.h (EXYNOS_FB_BASE VERIFIED).

  Set BEYOND0LTE_CHECKPOINTS to 0 to compile every checkpoint out.
**/

#ifndef BEYOND0LTE_CHECKPOINT_H_
#define BEYOND0LTE_CHECKPOINT_H_

#include <Platform/Exynos9820.h>

// 2026-10-14 (user: "5 large white lines from way before ... they appear on the samsung bootloader too"): off.
// The RAM boot log and the progress words carry this information now; -DBEYOND0LTE_CHECKPOINTS=1 brings the bands back.
#ifndef BEYOND0LTE_CHECKPOINTS
#define BEYOND0LTE_CHECKPOINTS  0
#endif

#if BEYOND0LTE_CHECKPOINTS
#define BEYOND0LTE_CHECKPOINT(Row)                                                        \
  do {                                                                                    \
    volatile UINT32  *CpPx_ = (volatile UINT32 *)(UINTN)(EXYNOS_FB_BASE +                 \
                               (UINT64)(Row) * EXYNOS_FB_STRIDE_PIXELS *                  \
                               EXYNOS_FB_BYTES_PER_PIXEL);                                \
    UINTN            CpN_;                                                                \
    for (CpN_ = 0; CpN_ < (UINTN)100 * EXYNOS_FB_WIDTH; CpN_++) {                         \
      CpPx_[CpN_] = 0xFFFFFFFFU;                                                          \
    }                                                                                     \
    __asm__ __volatile__ ("dsb sy" ::: "memory");                                         \
  } while (0)
#else
#define BEYOND0LTE_CHECKPOINT(Row)  do { } while (0)
#endif

#endif // BEYOND0LTE_CHECKPOINT_H_
