/** @file
  SimpleFramebufferDxe — a "dumb framebuffer" Graphics Output Protocol (GOP)
  for Exynos 9820 / Galaxy S10e (`beyond0lte`). Port of the Star2LtePkg driver
  for the Exynos 9810.

  Milestone 3 ("first light"). Instead of bringing up DECON + DSIM + the panel
  from scratch, we reuse the framebuffer sboot already initialized: it lit the
  panel to show its boot logo, so the scanout engine and panel are alive. We
  simply publish that linear framebuffer as a GOP so UEFI (and the Windows boot
  graphics) can draw to the screen.

  Assumptions (all from Silicon/Exynos9820Pkg/Include/Platform/Exynos9820.h):
    - EXYNOS_FB_BASE points at the live scanout buffer: 0xCA000000 on this SoC. **IT IS 0 ON THIS SoC** —
      the carveout has never been read here, and the 9810's 0xCC000000 is
      proven NOT reusable (notes/s10e-live-reads.md §7.3 puts it inside the
      abox-rmem allocation range in the shipping device tree).
    - Geometry EXYNOS_FB_WIDTH x EXYNOS_FB_HEIGHT = 1080 x 2280 (the S10e's own
      s6e3fa7_beyond0 panel, stock DTBO; notes/m8-uefi-image.md B44). The format (BGRX8888)
      and the stride (width*4) are the 9810's and have NOT been re-read here.
  If the base is still 0 (unverified), the driver refuses to load rather than
  scribbling on physical address 0 — that is the honest state, not a bug.

  This does NOT touch clocks, DSI, or the panel — zero hardware re-init. When it
  works you get text/logo on the phone; it never provides GPU acceleration
  (there is no Mali Windows driver — see windows-drivers/README.md).
 **/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DevicePath.h>

#include <Platform/Exynos9820.h>

#include "../../Library/PlatformMemoryMapLib/Beyond0LteCheckpoint.h"

#define FB_PIXEL_SIZE  ((UINTN)EXYNOS_FB_BYTES_PER_PIXEL)

//
// Text-scale factor. The panel is 1080x2280; EDK2's console uses
// a fixed 8x19 px font, so at native resolution text is ~135 columns of small
// text. We publish a virtual GOP mode that is 1/FB_TEXT_SCALE of the native size
// and pixel-double every Blt write to the physical scanout buffer. The console
// then draws into the smaller virtual screen and each virtual pixel becomes an
// NxN block on the panel -> NxN larger, readable text. Tune here:
//   1 = native (tiny)  2 = 90 cols  3 = 60 cols  4 = 45 cols.
//
// At 1080 wide, scale 1 gives 135 columns and 120 rows; scale 2 would give 67
// columns, which is below GraphicsConsole's 80-column floor (see below).
//
// HARD LIMIT (carried over from the 9810 flow, re-derived for this width):
// GraphicsConsoleDxe ASSERTs MaxColumns>=80 && MaxRows>=25 (GraphicsConsole.c:242),
// where MaxColumns = HorizontalResolution / EFI_GLYPH_WIDTH(8). This is a DEBUG
// build (asserts -> CpuDeadLoop), so any virtual width < 640 (scale > 2) HANGS the
// firmware inside GraphicsConsole:
//   (1080 wide) scale 1 -> 135 cols OK ; 2 -> 67 cols HANG.
// So 1 is the only integer scale that keeps >= 80 columns. However, Windows
// Boot Manager writes directly to FrameBufferBase using GOP mode info, bypassing
// Blt(). A scaled virtual mode over the physical 1080-stride framebuffer makes
// Windows draw a garbled/tiled screen. Keep GOP honest/native for OS handoff.
//
#define FB_TEXT_SCALE  1

//
// Vendor device path so BDS can connect this GOP as a console-out device.
//
#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} FB_DEVICE_PATH;
#pragma pack ()

//
// Chosen here (a namespace identifier, not a device read) and deliberately
// distinct from the 9810's so the two platforms' console handles cannot be
// confused if both ever end up in one FV.
//
#define FB_GOP_VENDOR_GUID \
  { 0x6f2d8b1a, 0x3c4e, 0x4f9a, { 0x9b, 0x22, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f } }

STATIC FB_DEVICE_PATH  mFbDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { sizeof (VENDOR_DEVICE_PATH), 0 } },
    FB_GOP_VENDOR_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

//
// Per-instance state.
//
typedef struct {
  EFI_GRAPHICS_OUTPUT_PROTOCOL          Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  Info;
  UINT32                                *FrameBuffer;   // virtual == physical (identity map)
  UINTN                                 StridePixels;   // PHYSICAL stride (native width)
  UINTN                                 Scale;          // physical pixels per virtual pixel
} FB_GOP_INSTANCE;

STATIC
UINT32 *
PixelPtr (
  IN FB_GOP_INSTANCE  *Ctx,
  IN UINTN             X,
  IN UINTN             Y
  )
{
  return Ctx->FrameBuffer + (Y * Ctx->StridePixels) + X;
}

EFI_STATUS
EFIAPI
FbGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  FB_GOP_INSTANCE  *Ctx;

  if (Info == NULL || SizeOfInfo == NULL || ModeNumber != 0) {
    return EFI_INVALID_PARAMETER;
  }

  Ctx   = BASE_CR (This, FB_GOP_INSTANCE, Gop);
  *Info = AllocateCopyPool (sizeof (Ctx->Info), &Ctx->Info);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  *SizeOfInfo = sizeof (Ctx->Info);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
FbGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  // Single fixed mode; nothing to reprogram (sboot already set scanout).
  return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

//
// ===== DECON command-mode present (DEVICE-PROVEN ON THE 9810 ONLY) =====
// The S9+ panel is COMMAND-MODE: pixels written into the scanout buffer are NOT
// shown until DECON is told to push one fresh frame to the panel GRAM via a
// SW-trigger 0->1 edge. sboot leaves DECON configured; we only (1) order the FB
// writes and (2) issue a shadow-update + a fresh SW-trigger edge. WITHOUT this,
// every GOP Blt succeeds but the panel never updates — which is exactly why
// "first light" needed this.
//
// The BASE moves on this SoC and is taken from the header, not restated: the
// 9810's single DECON at 0x16030000 becomes decon_f@0x19030000 (+ a second
// half, decon_t@0x19050000). The OFFSETS below are the 9810's, read from its
// device-exact register map (dpu_9810 regs-decon.h). *** UNVERIFIED ON 9820 ***
// — the 9820 kernel drives the display through Samsung's dpu20/cal_9820 tree
// (notes/exynos9820-research.md §1a), a different DPU generation, so the
// register map has to be re-derived before this path can be trusted. It is
// unreachable today because the fb base is 0 and the entry refuses; it is kept
// so the day the carveout is measured, the next unknown is visible rather than
// buried.
//
#define DECON_BASE               EXYNOS_DECON_F_BASE   // VERIFIED (decon_f@0x19030000)
#define DECON_SHADOW_UPDATE_REQ  0x0060u               // UNVERIFIED on 9820 (9810 map)
#define DECON_HW_SW_TRIG         0x0070u               // UNVERIFIED on 9820 (9810 map)
#define SHADOW_UPDATE_REQ_GLOBAL (1u << 31)
#define SHADOW_UPDATE_REQ_WINS   0x3Fu                 // all 6 decon windows (9810 count)
#define HW_SW_TRIG_SW_TRIG_EN    (1u << 8)
#define HW_SW_TRIG_HW_TRIG_EN    (1u << 0)             // pulse bit that fires the frame
#define HW_SW_TRIG_HW_TRIG_MASK  (1u << 4)             // 1 = panel TE hardware trigger BLOCKED (sboot default)

STATIC
VOID
FbDeconPresent (
  VOID
  )
{
  volatile UINT32  *Trig = (volatile UINT32 *)(UINTN)(DECON_BASE + DECON_HW_SW_TRIG);
  volatile UINT32  *Shad = (volatile UINT32 *)(UINTN)(DECON_BASE + DECON_SHADOW_UPDATE_REQ);
  UINT32           Base;

  // Order the framebuffer writes (Device-mapped, already in DRAM) before trigger.
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  // Force HW_TRIG_EN low in our working copy so each present is a fresh edge.
  Base = *Trig & ~HW_SW_TRIG_HW_TRIG_EN;

  // Request a shadow-register update (global + all windows).
  *Shad = SHADOW_UPDATE_REQ_GLOBAL | SHADOW_UPDATE_REQ_WINS;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  gBS->Stall (1000);

  // Two-write SW trigger: the enable->assert 0->1 edge that pushes one frame.
  *Trig = Base | HW_SW_TRIG_SW_TRIG_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
  gBS->Stall (1000);
  *Trig = Base | HW_SW_TRIG_SW_TRIG_EN | HW_SW_TRIG_HW_TRIG_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");

  // Let one frame latch. Then leave the command-mode panel in TE-driven HARDWARE
  // auto-refresh (unmask the panel TE trigger + keep HW_TRIG_EN, clear
  // SW_TRIG_EN) so DECON keeps re-scanning the framebuffer ~60 Hz with no CPU
  // involvement. This keeps the display live after firmware stops running, so the
  // Windows kernel's own post-handoff output (logo / BSOD) that it writes to the
  // inherited GOP framebuffer becomes visible. Harmless if the panel TE is not
  // driving DECON (panel just holds the last SW-triggered frame).
  gBS->Stall (33000);
  *Trig = (Base & ~(HW_SW_TRIG_HW_TRIG_MASK | HW_SW_TRIG_SW_TRIG_EN)) | HW_SW_TRIG_HW_TRIG_EN;
  __asm__ __volatile__ ("dsb sy" ::: "memory");
}

EFI_STATUS
EFIAPI
FbGopBlt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer  OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN     UINTN                              SourceX,
  IN     UINTN                              SourceY,
  IN     UINTN                              DestinationX,
  IN     UINTN                              DestinationY,
  IN     UINTN                              Width,
  IN     UINTN                              Height,
  IN     UINTN                              Delta        OPTIONAL
  )
{
  FB_GOP_INSTANCE  *Ctx;
  UINTN            BufStridePixels;
  UINTN            Row;
  UINTN            Col;
  UINT32           *Dst;
  UINT32           *Src;
  UINT32           *BufRow;
  BOOLEAN          DidWrite;
  UINTN            S;
  UINTN            Sx;
  UINTN            Sy;

  Ctx = BASE_CR (This, FB_GOP_INSTANCE, Gop);
  S   = Ctx->Scale;

  if (Width == 0 || Height == 0) {
    return EFI_SUCCESS;
  }

  // Track whether this Blt modified the framebuffer; if so, present (push the
  // frame to the command-mode panel via DECON) before returning. Reads
  // (EfiBltVideoToBltBuffer) clear this so they do not trigger a needless frame.
  DidWrite = TRUE;

  // Delta of 0 means the BltBuffer is exactly Width pixels wide.
  if (Delta == 0) {
    BufStridePixels = Width;
  } else {
    BufStridePixels = Delta / sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  switch (BltOperation) {
  case EfiBltVideoFill:
    if (BltBuffer == NULL) {
      return EFI_INVALID_PARAMETER;
    }
    if ((DestinationX + Width) > Ctx->Info.HorizontalResolution ||
        (DestinationY + Height) > Ctx->Info.VerticalResolution) {
      return EFI_INVALID_PARAMETER;
    }
    {
      UINT32  Pixel = *(UINT32 *)BltBuffer;   // BGRX layout matches framebuffer
      // Expand the whole rectangle by S in both axes (physical coordinates).
      for (Row = 0; Row < Height * S; Row++) {
        Dst = PixelPtr (Ctx, DestinationX * S, DestinationY * S + Row);
        for (Col = 0; Col < Width * S; Col++) {
          Dst[Col] = Pixel;
        }
      }
    }
    break;

  case EfiBltBufferToVideo:
    if (BltBuffer == NULL) {
      return EFI_INVALID_PARAMETER;
    }
    if ((DestinationX + Width) > Ctx->Info.HorizontalResolution ||
        (DestinationY + Height) > Ctx->Info.VerticalResolution) {
      return EFI_INVALID_PARAMETER;
    }
    for (Row = 0; Row < Height; Row++) {
      BufRow = (UINT32 *)BltBuffer + ((SourceY + Row) * BufStridePixels) + SourceX;
      // Each source pixel becomes an SxS block on the physical panel.
      for (Sy = 0; Sy < S; Sy++) {
        Dst = PixelPtr (Ctx, DestinationX * S, (DestinationY + Row) * S + Sy);
        for (Col = 0; Col < Width; Col++) {
          for (Sx = 0; Sx < S; Sx++) {
            Dst[Col * S + Sx] = BufRow[Col];
          }
        }
      }
    }
    break;

  case EfiBltVideoToBltBuffer:
    if (BltBuffer == NULL) {
      return EFI_INVALID_PARAMETER;
    }
    if ((SourceX + Width) > Ctx->Info.HorizontalResolution ||
        (SourceY + Height) > Ctx->Info.VerticalResolution) {
      return EFI_INVALID_PARAMETER;
    }
    DidWrite = FALSE;   // read-back only; no panel update needed
    for (Row = 0; Row < Height; Row++) {
      // Sample the top-left pixel of each SxS physical block.
      Src    = PixelPtr (Ctx, SourceX * S, (SourceY + Row) * S);
      BufRow = (UINT32 *)BltBuffer + ((DestinationY + Row) * BufStridePixels) + DestinationX;
      for (Col = 0; Col < Width; Col++) {
        BufRow[Col] = Src[Col * S];
      }
    }
    break;

  case EfiBltVideoToVideo:
    if ((SourceX + Width) > Ctx->Info.HorizontalResolution ||
        (SourceY + Height) > Ctx->Info.VerticalResolution ||
        (DestinationX + Width) > Ctx->Info.HorizontalResolution ||
        (DestinationY + Height) > Ctx->Info.VerticalResolution) {
      return EFI_INVALID_PARAMETER;
    }
    // Copy row-by-row in PHYSICAL space (scaled); handle vertical overlap by
    // choosing copy direction.
    if (DestinationY <= SourceY) {
      for (Row = 0; Row < Height * S; Row++) {
        Src = PixelPtr (Ctx, SourceX * S, SourceY * S + Row);
        Dst = PixelPtr (Ctx, DestinationX * S, DestinationY * S + Row);
        CopyMem (Dst, Src, Width * S * sizeof (UINT32));
      }
    } else {
      for (Row = Height * S; Row-- > 0; ) {
        Src = PixelPtr (Ctx, SourceX * S, SourceY * S + Row);
        Dst = PixelPtr (Ctx, DestinationX * S, DestinationY * S + Row);
        CopyMem (Dst, Src, Width * S * sizeof (UINT32));
      }
    }
    break;

  default:
    return EFI_INVALID_PARAMETER;
  }

  //
  // Command-mode panel: pixels are now in the scanout buffer but invisible until
  // DECON pushes a frame. Trigger one present for any operation that drew.
  //
  if (DidWrite) {
    FbDeconPresent ();
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SimpleFramebufferDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS        Status;
  FB_GOP_INSTANCE   *Ctx;
  EFI_HANDLE        Handle;

  BEYOND0LTE_CHECKPOINT (1780);   // DXE core running, drivers dispatching (B39)

  //
  // Refuse to run until the framebuffer base is verified (placeholder is 0).
  //
  // On THIS SoC that is the current, deliberate state: the sboot-lit scanout
  // carveout has to be MEASURED on the phone before this driver can do anything.
  // The 9810's 0xCC000000 is not a usable stand-in — notes/s10e-live-reads.md §7.3
  // shows it falls inside the vendor DT's abox-rmem allocation range on the S10e
  // (and inside edk2-exynos' HLOS 1 region), so publishing it would map a region
  // the modem/CP stack owns. A wrong fb base is a blank or faulting screen, not a
  // recoverable mistake, which is why the driver stops instead of guessing.
  //
  if (EXYNOS_FB_BASE == 0) {   // constant-false while the header carries the read
    DEBUG ((DEBUG_ERROR,
      "SimpleFramebufferDxe: EXYNOS_FB_BASE not set — measure the sboot-lit framebuffer "
      "carveout on the S10e and record it in Exynos9820.h before enabling this driver.\n"
      "  (0xCC000000, the 9810's, is NOT reusable on this SoC.)\n"));
    return EFI_NOT_READY;
  }

  Ctx = AllocateZeroPool (sizeof (FB_GOP_INSTANCE));
  if (Ctx == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Ctx->FrameBuffer  = (UINT32 *)(UINTN)EXYNOS_FB_BASE;
  Ctx->StridePixels = EXYNOS_FB_STRIDE_PIXELS;   // PHYSICAL stride (native width)
  Ctx->Scale        = FB_TEXT_SCALE;

  //
  // Publish a VIRTUAL mode that is 1/Scale of the native panel. GraphicsConsole
  // draws into this smaller logical screen; FbGopBlt pixel-doubles every write
  // by Scale to the physical panel so the 8x19 font renders Scale x larger.
  // (GraphicsConsole uses Blt exclusively, so the scaled-down resolution vs. the
  // native FrameBufferBase below is consistent for all console drawing.)
  //
  Ctx->Info.Version              = 0;
  Ctx->Info.HorizontalResolution = EXYNOS_FB_WIDTH  / FB_TEXT_SCALE;
  Ctx->Info.VerticalResolution   = EXYNOS_FB_HEIGHT / FB_TEXT_SCALE;
  Ctx->Info.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  Ctx->Info.PixelsPerScanLine    = EXYNOS_FB_WIDTH  / FB_TEXT_SCALE;

  Ctx->Mode.MaxMode         = 1;
  Ctx->Mode.Mode            = 0;
  Ctx->Mode.Info            = &Ctx->Info;
  Ctx->Mode.SizeOfInfo      = sizeof (Ctx->Info);
  Ctx->Mode.FrameBufferBase = (EFI_PHYSICAL_ADDRESS)EXYNOS_FB_BASE;
  Ctx->Mode.FrameBufferSize = (UINTN)EXYNOS_FB_SIZE;

  Ctx->Gop.QueryMode = FbGopQueryMode;
  Ctx->Gop.SetMode   = FbGopSetMode;
  Ctx->Gop.Blt       = FbGopBlt;
  Ctx->Gop.Mode      = &Ctx->Mode;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                   &Handle,
                   &gEfiGraphicsOutputProtocolGuid, &Ctx->Gop,
                   &gEfiDevicePathProtocolGuid,      &mFbDevicePath,
                   NULL
                   );
  if (EFI_ERROR (Status)) {
    FreePool (Ctx);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "SimpleFramebufferDxe: GOP up at 0x%lx, %ux%u.\n",
    (UINT64)EXYNOS_FB_BASE, (UINT32)EXYNOS_FB_WIDTH, (UINT32)EXYNOS_FB_HEIGHT));
  return EFI_SUCCESS;
}
