/** @file
  Beyond0LteFbText — direct-framebuffer progress text for the SEC/PEI bring-up
  phase (Exynos 9820 / Galaxy S10e, `beyond0lte`). Port of Star2LteFbText for the
  Exynos 9810. See Beyond0LteFbText.c.
**/

#ifndef BEYOND0LTE_FB_TEXT_H_
#define BEYOND0LTE_FB_TEXT_H_

#include <Uefi.h>

/**
  Draw one line of ASCII text directly to the panel scanout buffer at the given
  pixel row (white-on-black, 2x scale). MMU-off safe; no boot services used.

  This is a NO-OP on the S10e as the tree stands: EXYNOS_FB_BASE is 0 in
  Exynos9820.h because the framebuffer carveout has never been read on this SoC
  (the 9810's 0xCC000000 is proven unusable here — notes/s10e-live-reads.md §7.3).
  The call returns without touching memory rather than writing to physical
  address 0, so wiring it into the boot flow is harmless but also silent until
  the carveout is measured.

  @param[in] PixelRow  Top pixel row (Y) of the text line on the 1440x3040 panel.
  @param[in] Str       NUL-terminated ASCII string (0x20..0x7E; others -> space).
**/
VOID
Beyond0LteFbText (
  IN UINT32       PixelRow,
  IN CONST CHAR8  *Str
  );

#endif // BEYOND0LTE_FB_TEXT_H_
