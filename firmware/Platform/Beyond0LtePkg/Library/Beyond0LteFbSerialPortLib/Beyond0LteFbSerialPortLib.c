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

  Exception mode (m10 W14): the CPU exception dump (banner, "PC ... <image>"
  backtrace, registers, stack) is longer than the screen, so the wrap used to
  overwrite the line naming the faulting module. A write containing
  "Exception at 0x" now clears the panel, restarts at the top and stops
  wrapping: output past the last line is dropped, keeping the dump's head.
  The mode flags live in two pixels of the strip below the last text row
  (rows ROWS*CELL_H.. are never drawn), not in the state record: the record
  survives a reset, while sboot repaints the whole scanout buffer on every boot,
  so a dead-looped exception cannot leave the next boot's console frozen.

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

  BEYOND0LTE_FB_CONSOLE (user, 2026-10-11: "disable it and just write it to logs and keep it clean (behind a
  feature flag), instead put like a new loading bar looping on the bottom of the screen"): 0 (default) = no
  text on the panel - the sboot splash stays - and every write moves a block along a progress track near the
  bottom instead; the RAM boot log (UEFILOG.TXT) still gets every byte, and an exception banner still switches
  the panel to the full text dump. 1 = the text console as before.
**/

#include <Base.h>
#include <Library/SerialPortLib.h>
#include <Platform/Exynos9820.h>

#include "Beyond0LteFont8x16.h"
#include "Beyond0LteBootLog.h"

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

#ifndef BEYOND0LTE_FB_CONSOLE
#define BEYOND0LTE_FB_CONSOLE  0
#endif

// Progress track (console off): above the undrawn flag strip, clear of the splash's centre.
#define BAR_H       12u
#define BAR_Y0      (EXYNOS_FB_HEIGHT - 120u)
#define BAR_X0      140u
#define BAR_W       (EXYNOS_FB_WIDTH - 2u * BAR_X0)
#define BAR_BLK     (BAR_W / 5u)
#define BAR_STEP    6u
#define BAR_TRACK   0xFF303030U
#define BAR_FG      0xFFFFFFFFU

typedef struct {
  UINT32    Magic;
  UINT32    Col;
  UINT32    Row;
  UINT32    Tick;                       // progress block position (console off)
} FB_CON_STATE;

STATIC_ASSERT (EXYNOS_FB_BYTES_PER_PIXEL == 4, "renderer stores UINT32 pixels");

#define FB_PIXELS  ((volatile UINT32 *)(UINTN)EXYNOS_FB_BASE)
#define FB_STATE   ((volatile FB_CON_STATE *)(UINTN)(EXYNOS_FB_BASE + EXYNOS_FB_SIZE))

STATIC_ASSERT (ROWS * CELL_H < EXYNOS_FB_HEIGHT, "exception-mode flags need an undrawn strip below the text");

// Exception-mode flags: pixels 0 and 1 of the last scanline (below the text area).
#define FB_FROZEN  (FB_PIXELS[(UINTN)(EXYNOS_FB_HEIGHT - 1) * EXYNOS_FB_STRIDE_PIXELS])
#define FB_FULL    (FB_PIXELS[(UINTN)(EXYNOS_FB_HEIGHT - 1) * EXYNOS_FB_STRIDE_PIXELS + 1])

//
// RAM boot log (m10 W19): every byte written here also goes to a 1 MiB ring in reserved RAM
// (Beyond0LteBootLog.h), which Beyond0LteBootLogDxe copies to \UEFILOG.TXT on the ESP, so a boot
// can be read from TWRP instead of photographed. The ring is reset at the first write of a new
// boot: SEC runs with the MMU off, every later module with it on, so "MMU off now, last writer
// had it on" can only be the next boot. Byte stores only (the MMU-off view is Device memory).
//
#define BOOTLOG_HDR   ((volatile BEYOND0LTE_BOOTLOG_HEADER *)(UINTN)BEYOND0LTE_BOOTLOG_BASE)
#define BOOTLOG_DATA  ((volatile UINT8 *)(UINTN)(BEYOND0LTE_BOOTLOG_BASE + sizeof (BEYOND0LTE_BOOTLOG_HEADER)))

STATIC
BOOLEAN
MmuIsOn (
  VOID
  )
{
  UINT64  Sctlr;

  __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Sctlr));
  return (BOOLEAN)((Sctlr & 1u) != 0);
}

STATIC
VOID
BootLogAppend (
  IN CONST UINT8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  volatile BEYOND0LTE_BOOTLOG_HEADER  *Hdr;
  BOOLEAN                             MmuOn;
  UINT32                              Head;
  UINTN                               Index;

  Hdr   = BOOTLOG_HDR;
  MmuOn = MmuIsOn ();
  if ((Hdr->Magic != BEYOND0LTE_BOOTLOG_MAGIC) || (Hdr->Head >= BEYOND0LTE_BOOTLOG_DATA_SIZE) ||
      (!MmuOn && (Hdr->LastMmuOn != 0)))
  {
    Hdr->BootCount = (Hdr->Magic == BEYOND0LTE_BOOTLOG_MAGIC) ? Hdr->BootCount + 1 : 1;
    Hdr->Head      = 0;
    Hdr->Wrapped   = 0;
    Hdr->Magic     = BEYOND0LTE_BOOTLOG_MAGIC;
  }

  Hdr->LastMmuOn = MmuOn ? 1u : 0u;
  Head           = Hdr->Head;
  for (Index = 0; Index < NumberOfBytes; Index++) {
    BOOTLOG_DATA[Head] = Buffer[Index];
    if (++Head >= BEYOND0LTE_BOOTLOG_DATA_SIZE) {
      Head         = 0;
      Hdr->Wrapped = 1;
    }
  }

  Hdr->Head = Head;
}

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
  if (FB_FROZEN == CON_MAGIC) {
    if (St->Row + 1 >= ROWS) {
      FB_FULL = CON_MAGIC;
      return;
    }

    St->Row++;
    return;
  }

  St->Row = (St->Row + 1 < ROWS) ? St->Row + 1 : 0;
  FbClearLine (St->Row);
  FbClearLine ((St->Row + 1 < ROWS) ? St->Row + 1 : 0);
}

#if !BEYOND0LTE_FB_CONSOLE
/** Fill columns [X0, X1) of the progress track with Color. **/
STATIC
VOID
FbBarColumns (
  IN UINT32  X0,
  IN UINT32  X1,
  IN UINT32  Color
  )
{
  UINT32  Y;
  UINT32  X;

  for (Y = 0; Y < BAR_H; Y++) {
    volatile UINT32  *Line;

    Line = FB_PIXELS + (UINTN)(BAR_Y0 + Y) * EXYNOS_FB_STRIDE_PIXELS + BAR_X0;
    for (X = X0; X < X1; X++) {
      Line[X] = Color;
    }
  }
}

/** Advance the progress block by BAR_STEP, drawing only the columns that change. **/
STATIC
VOID
FbBarTick (
  IN volatile FB_CON_STATE  *St
  )
{
  UINT32  Old;
  UINT32  New;

  Old = St->Tick;
  New = Old + BAR_STEP;
  if (New + BAR_BLK > BAR_W) {
    FbBarColumns (Old, Old + BAR_BLK, BAR_TRACK);
    FbBarColumns (0, BAR_BLK, BAR_FG);
    New = 0;
  } else {
    FbBarColumns (Old, New, BAR_TRACK);
    FbBarColumns (Old + BAR_BLK, New + BAR_BLK, BAR_FG);
  }

  St->Tick = New;
}
#endif

STATIC
volatile FB_CON_STATE *
FbState (
  VOID
  )
{
  volatile FB_CON_STATE  *St;

  St = FB_STATE;
  // The record survives resets; Tick is validated too (older firmware never wrote it - an out-of-range value
  // would put the progress block outside the track, past the framebuffer).
  if ((St->Magic != CON_MAGIC) || (St->Col >= COLS) || (St->Row >= ROWS) || (St->Tick + BAR_BLK > BAR_W)) {
    St->Magic = CON_MAGIC;
    St->Col   = 0;
    St->Row   = 0;
    St->Tick  = 0;
#if BEYOND0LTE_FB_CONSOLE
    FbClearLine (0);
    FbClearLine (1);
#else
    FbBarColumns (0, BAR_W, BAR_TRACK);
    FbBarColumns (0, BAR_BLK, BAR_FG);
#endif
  }

  return St;
}

/** TRUE if Buffer contains "Exception at 0x" (CpuExceptionHandlerLib's banner). **/
STATIC
BOOLEAN
HasExceptionBanner (
  IN CONST UINT8  *Buffer,
  IN UINTN        NumberOfBytes
  )
{
  STATIC CONST CHAR8  Banner[] = "Exception at 0x";
  UINTN               Index;
  UINTN               Match;

  for (Index = 0; Index + sizeof (Banner) - 1 <= NumberOfBytes; Index++) {
    for (Match = 0; Match < sizeof (Banner) - 1; Match++) {
      if (Buffer[Index + Match] != (UINT8)Banner[Match]) {
        break;
      }
    }

    if (Match == sizeof (Banner) - 1) {
      return TRUE;
    }
  }

  return FALSE;
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

  BootLogAppend (Buffer, NumberOfBytes);   // before the panel: exception mode drops panel output
  St = FbState ();
  if ((FB_FROZEN != CON_MAGIC) && HasExceptionBanner (Buffer, NumberOfBytes)) {
    for (Index = 0; Index < ROWS; Index++) {
      FbClearLine ((UINT32)Index);
    }

    St->Col   = 0;
    St->Row   = 0;
    FB_FULL   = 0;
    FB_FROZEN = CON_MAGIC;
  }

#if !BEYOND0LTE_FB_CONSOLE
  if (FB_FROZEN != CON_MAGIC) {
    FbBarTick (St);   // normal boot: no text on the panel, the log is in the RAM ring / UEFILOG.TXT
    __asm__ __volatile__ ("dsb sy" ::: "memory");
    return NumberOfBytes;
  }
#endif

  for (Index = 0; Index < NumberOfBytes; Index++) {
    if ((FB_FROZEN == CON_MAGIC) && (FB_FULL == CON_MAGIC)) {
      break;
    }

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
