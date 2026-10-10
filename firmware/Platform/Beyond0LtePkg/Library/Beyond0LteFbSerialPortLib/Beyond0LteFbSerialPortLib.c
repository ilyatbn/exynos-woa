/** @file
  Beyond0LteFbSerialPortLib - SerialPortLib that renders the DEBUG log onto the
  panel (notes/m8-uefi-image.md B54).

  Boot #13 showed the FD dying between ArmPlatformGetBootMode and the first DXE
  driver with a reset that no EL1 vector sees (B53). Nobody can watch the UART
  on this phone, so this library puts every SerialPortWrite on the sboot scanout
  buffer instead: 8x16 glyphs at 2x (16x32 px), 67 columns x 71 lines on the
  1080 x 2280 panel, white on black. When the bottom is reached it wraps to the
  top, and the line after the cursor is always blanked, so the newest text sits
  just above a black line.

  Every module links its own copy of this library (SEC, DXE core, each driver),
  so the cursor cannot live in a global: it lives in a small record just past
  the end of the framebuffer (EXYNOS_FB_BASE + EXYNOS_FB_SIZE), inside the page-
  rounded FB mapping PlatformMemoryMapLib publishes as device memory. The idea
  (not the code) is edk2-exynos' InitializeSharedUartBuffers.

  Safe with the MMU off (SEC) and on (DXE, FB mapped as device). No boot
  services, no DebugLib (this IS DebugLib's back end), no stack-heavy calls.
  Not for runtime after SetVirtualAddressMap (the FB is not runtime-mapped).

  Geometry and base come from Exynos9820.h (EXYNOS_FB_BASE / WIDTH / HEIGHT,
  verified 1080 x 2280, B44); 4 B per pixel per EXYNOS_FB_BYTES_PER_PIXEL.
**/

#include <Base.h>
#include <Library/SerialPortLib.h>
#include <Platform/Exynos9820.h>

#include "Beyond0LteFont8x16.h"

#define GLYPH_W     8u
#define GLYPH_H     16u
#define SCALE       2u
#define CELL_W      (GLYPH_W * SCALE)
#define CELL_H      (GLYPH_H * SCALE)
#define COLS        (EXYNOS_FB_WIDTH / CELL_W)
#define ROWS        (EXYNOS_FB_HEIGHT / CELL_H)
#define FG          0xFFFFFFFFU
#define BG          0xFF000000U
#define CON_MAGIC   0x46424331U          // "FBC1"

typedef struct {
  UINT32    Magic;
  UINT32    Col;
  UINT32    Row;
} FB_CON_STATE;

STATIC_ASSERT (EXYNOS_FB_BYTES_PER_PIXEL == 4, "renderer stores UINT32 pixels");

#define FB_PIXELS  ((volatile UINT32 *)(UINTN)EXYNOS_FB_BASE)
#define FB_STATE   ((volatile FB_CON_STATE *)(UINTN)(EXYNOS_FB_BASE + EXYNOS_FB_SIZE))

STATIC
VOID
FbClearLine (
  IN UINT32  Row
  )
{
  volatile UINT32  *Px;
  UINTN            Count;

  Px = FB_PIXELS + (UINTN)Row * CELL_H * EXYNOS_FB_STRIDE_PIXELS;
  for (Count = 0; Count < (UINTN)CELL_H * EXYNOS_FB_STRIDE_PIXELS; Count++) {
    Px[Count] = BG;
  }
}

STATIC
VOID
FbDrawChar (
  IN UINT32  Col,
  IN UINT32  Row,
  IN UINT8   Ch
  )
{
  CONST UINT8      *Glyph;
  volatile UINT32  *Line;
  UINTN            Y;
  UINTN            X;
  UINT32           Color;

  if ((Ch < 0x20) || (Ch > 0x7E)) {
    Ch = '?';
  }

  Glyph = mFont[Ch - 0x20];
  for (Y = 0; Y < CELL_H; Y++) {
    Line = FB_PIXELS + ((UINTN)Row * CELL_H + Y) * EXYNOS_FB_STRIDE_PIXELS + (UINTN)Col * CELL_W;
    for (X = 0; X < CELL_W; X++) {
      Color   = ((Glyph[Y / SCALE] << (X / SCALE)) & 0x80) ? FG : BG;
      Line[X] = Color;
    }
  }
}

STATIC
VOID
FbNewLine (
  IN volatile FB_CON_STATE  *St
  )
{
  St->Col = 0;
  St->Row = (St->Row + 1 < ROWS) ? St->Row + 1 : 0;
  FbClearLine (St->Row);
  FbClearLine ((St->Row + 1 < ROWS) ? St->Row + 1 : 0);
}

STATIC
volatile FB_CON_STATE *
FbState (
  VOID
  )
{
  volatile FB_CON_STATE  *St;

  St = FB_STATE;
  if ((St->Magic != CON_MAGIC) || (St->Col >= COLS) || (St->Row >= ROWS)) {
    St->Magic = CON_MAGIC;
    St->Col   = 0;
    St->Row   = 0;
    FbClearLine (0);
    FbClearLine (1);
  }

  return St;
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  (VOID)FbState ();
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  volatile FB_CON_STATE  *St;
  UINTN                  Index;
  UINT8                  Ch;

  if (Buffer == NULL) {
    return 0;
  }

  St = FbState ();
  for (Index = 0; Index < NumberOfBytes; Index++) {
    Ch = Buffer[Index];
    if (Ch == '\n') {
      FbNewLine (St);
    } else if (Ch == '\r') {
      St->Col = 0;
    } else {
      FbDrawChar (St->Col, St->Row, Ch);
      if (St->Col + 1 >= COLS) {
        FbNewLine (St);
      } else {
        St->Col++;
      }
    }
  }

  __asm__ __volatile__ ("dsb sy" ::: "memory");
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  (VOID)Buffer;
  (VOID)NumberOfBytes;
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  (VOID)Control;
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  if (Control != NULL) {
    *Control = 0;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}
