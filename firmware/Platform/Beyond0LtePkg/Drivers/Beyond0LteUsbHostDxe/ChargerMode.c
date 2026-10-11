// ChargerMode.c - "off-mode charging" for the S10e (user, 2026-10-12: plugging a charger into the powered-off phone
// booted our image on top of sboot's charging screen and went on into TWRP/Windows).
//
// Samsung's sboot powers the phone on when a charger is attached and boots the boot image in low-power-charging mode,
// flagging it in the FDT it hands over (x0, arm64 Linux boot protocol): /chosen/bootargs carries "lpcharge=1" /
// "androidboot.mode=charger" (Android then shows its charging animation). The shim keeps x0 at progress page +0x20
// with the 'FDT0' magic at +0x28 (bootshim.S). When the flag is there this driver never lets the boot continue:
//   - stops the SEC/BootLog dead man's watchdog (an 82 s reset would land in TWRP);
//   - draws a battery bar from the MAX77705 fuel gauge SOC, blanks the panel after 10 s, a power tap shows it again;
//   - power held 1 s -> warm reset = a normal boot (sboot enters charger mode only on a power-on);
//   - charger unplugged (MAX77705 BC_STATUS VBUSDet 0 for 2 s) -> power off (ResetSystem shutdown = PS_HOLD).
// Everything is logged to UEFILOG (Beyond0LteBootLog ring) before the loop starts.

#include <Library/BaseLib.h>
#include <Library/FdtLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include "UsbHostInternal.h"

#define CHG_PAGE              0x8FE0F000ULL
#define CHG_FDT_PTR_OFFSET    0x20
#define CHG_FDT_MAGIC_OFFSET  0x28
#define CHG_FDT_MAGIC         0x30544446u        // 'FDT0', written by bootshim.S
#define CHG_POWER_KEY_DAT     (EXYNOS_ALIVE_GPIO_BASE + 0x044)   // gpa2 DAT (D8 constants: GPA2_OFF 0x040, DAT +4)
#define CHG_POWER_KEY_BIT     BIT4                                // gpa2-4 power key, active-low
#define CHG_SCREEN_ON_MS      10000
#define CHG_HOLD_MS           1000
#define CHG_UNPLUG_MS         2000

STATIC
BOOLEAN
ChargerFlagInBootargs (
  OUT CHAR8  *Shown,
  IN  UINTN  ShownSize
  )
{
  volatile UINT8  *Page;
  UINT64          FdtAddress;
  CONST VOID      *Fdt;
  INT32           Node;
  INT32           Length;
  CONST CHAR8     *Args;
  UINTN           Index;

  Page = (volatile UINT8 *)(UINTN)CHG_PAGE;
  AsciiStrCpyS (Shown, ShownSize, "(none)");
  if (*(volatile UINT32 *)(Page + CHG_FDT_MAGIC_OFFSET) != CHG_FDT_MAGIC) {
    DEBUG ((DEBUG_INFO, "ChargerMode: no FDT pointer from the shim\n"));
    return FALSE;
  }

  FdtAddress = *(volatile UINT64 *)(Page + CHG_FDT_PTR_OFFSET);
  *(volatile UINT32 *)(Page + CHG_FDT_MAGIC_OFFSET) = 0;   // consumed: a warm reset must not reuse a stale pointer
  if ((FdtAddress < 0x80000000ULL) || (FdtAddress >= 0x100000000ULL) || ((FdtAddress & 7) != 0)) {
    DEBUG ((DEBUG_INFO, "ChargerMode: FDT pointer %lx outside bank0 - ignored\n", FdtAddress));
    return FALSE;
  }

  Fdt = (CONST VOID *)(UINTN)FdtAddress;
  if (FdtCheckHeader (Fdt) != 0) {
    DEBUG ((DEBUG_INFO, "ChargerMode: no valid FDT at %lx (overwritten?)\n", FdtAddress));
    return FALSE;
  }

  Node = FdtPathOffset (Fdt, "/chosen");
  Args = (Node >= 0) ? (CONST CHAR8 *)FdtGetProp (Fdt, Node, "bootargs", &Length) : NULL;
  if ((Args == NULL) || (Length <= 0)) {
    DEBUG ((DEBUG_INFO, "ChargerMode: FDT at %lx has no /chosen/bootargs\n", FdtAddress));
    return FALSE;
  }

  for (Index = 0; (Index + 1 < ShownSize) && (Index < (UINTN)Length) && (Args[Index] != '\0'); Index++) {
    Shown[Index] = Args[Index];
  }

  Shown[Index] = '\0';
  DEBUG ((DEBUG_INFO, "ChargerMode: sboot FDT at %lx, bootargs (%d B): %a\n", FdtAddress, Length, Shown));
  return (AsciiStrStr (Args, "lpcharge=1") != NULL) || (AsciiStrStr (Args, "androidboot.mode=charger") != NULL);
}

STATIC
VOID
ChargerFill (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINTN   Width,
  IN UINTN   Height,
  IN UINT32  Color
  )
{
  volatile UINT32  *Fb;
  UINTN            Row;
  UINTN            Col;

  Fb = (volatile UINT32 *)(UINTN)EXYNOS_FB_BASE;
  for (Row = Y; (Row < Y + Height) && (Row < EXYNOS_FB_HEIGHT); Row++) {
    for (Col = X; (Col < X + Width) && (Col < EXYNOS_FB_WIDTH); Col++) {
      Fb[Row * EXYNOS_FB_STRIDE_PIXELS + Col] = Color;
    }
  }
}

STATIC
VOID
ChargerDraw (
  IN UINTN    Tenths,      // SOC in 0.1 %, or MAX_UINTN if unknown
  IN BOOLEAN  Visible
  )
{
  CONST UINTN  W  = 360;
  CONST UINTN  H  = 720;
  CONST UINTN  X  = (EXYNOS_FB_WIDTH - W) / 2;
  CONST UINTN  Y  = (EXYNOS_FB_HEIGHT - H) / 2;
  CONST UINTN  T  = 16;    // outline thickness
  UINTN        Inner;
  UINTN        Level;
  UINT32       Color;

  ChargerFill (0, 0, EXYNOS_FB_WIDTH, EXYNOS_FB_HEIGHT, 0);
  if (!Visible) {
    return;
  }

  ChargerFill (X + W / 3, Y - 40, W / 3, 40, 0x00FFFFFF);                     // terminal
  ChargerFill (X, Y, W, T, 0x00FFFFFF);                                       // outline
  ChargerFill (X, Y + H - T, W, T, 0x00FFFFFF);
  ChargerFill (X, Y, T, H, 0x00FFFFFF);
  ChargerFill (X + W - T, Y, T, H, 0x00FFFFFF);
  if (Tenths == MAX_UINTN) {
    ChargerFill (X + 2 * T, Y + H / 2 - T, W - 4 * T, 2 * T, 0x00808080);     // unknown level: a grey dash
    return;
  }

  Inner = H - 4 * T;
  Level = (Inner * Tenths) / 1000;
  Color = (Tenths < 150) ? 0x00FF4040 : 0x00FFFFFF;   // pixel format unread: red vs white is the only hint used
  ChargerFill (X + 2 * T, Y + 2 * T + (Inner - Level), W - 4 * T, Level, Color);
}

STATIC
BOOLEAN
ChargerVbusPresent (VOID)
{
  UINT8  Bc;

  Bc = 0;
  if (EFI_ERROR (Max77705RegRead (MAX77705_CCIC_ADDR, MAX77705_BC_STATUS, &Bc))) {
    return TRUE;   // unreadable: never power off on a read error
  }

  return (Bc & BIT7) != 0;
}

VOID
ChargerModeCheckAndRun (VOID)
{
  CHAR8      Args[200];
  BOOLEAN    BusOk;
  UINTN      Tenths;
  UINTN      Elapsed;
  UINTN      ShownSince;
  UINTN      PressedSince;
  UINTN      UnpluggedSince;
  BOOLEAN    Visible;
  BOOLEAN    WasPressed;

  if (!ChargerFlagInBootargs (Args, sizeof (Args))) {
    return;
  }

  DEBUG ((DEBUG_WARN, "ChargerMode: charger power-on - staying in off-mode charging (power 1 s = boot)\n"));
  // Stop the dead man's watchdog (BootLogDxe only cancels it at ReadyToBoot/EBS, which never come here).
  MmioWrite32 ((UINTN)EXYNOS_PMU_BASE + EXYNOS_PMU_INFORM3_OFFSET, EXYNOS_REBOOT_REASON_BASE);
  MmioWrite32 ((UINTN)EXYNOS_WDT_BASE + EXYNOS_WDT_WTCON_OFFSET, 0);
  MmioWrite32 ((UINTN)EXYNOS_WDT_BASE + EXYNOS_WDT_WTCLRINT_OFFSET, 1);

  BusOk          = !EFI_ERROR (PmicBusInit ());
  Tenths         = MAX_UINTN;
  if (BusOk && EFI_ERROR (FuelGaugeReadSoc (&Tenths))) {
    Tenths = MAX_UINTN;
  }

  DEBUG ((DEBUG_INFO, "ChargerMode: bus %a, SOC %u (0.1%%), VBUS %u\n", BusOk ? "ok" : "FAILED", Tenths,
          BusOk ? ChargerVbusPresent () : 2));
  Visible        = TRUE;
  ShownSince     = 0;
  PressedSince   = 0;
  UnpluggedSince = 0;
  WasPressed     = FALSE;
  ChargerDraw (Tenths, Visible);

  for (Elapsed = 0; ; Elapsed += 50) {
    BOOLEAN  Pressed = (MmioRead32 ((UINTN)CHG_POWER_KEY_DAT) & CHG_POWER_KEY_BIT) == 0;

    if (Pressed && !WasPressed) {
      PressedSince = Elapsed;
      if (!Visible) {
        Visible    = TRUE;
        ShownSince = Elapsed;
        ChargerDraw (Tenths, Visible);
      }
    }

    if (Pressed && (Elapsed - PressedSince >= CHG_HOLD_MS)) {
      DEBUG ((DEBUG_WARN, "ChargerMode: power held - rebooting into a normal boot\n"));
      ChargerFill (0, 0, EXYNOS_FB_WIDTH, EXYNOS_FB_HEIGHT, 0);
      if (BusOk) {
        PmicBusRelease ();
      }

      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    }

    WasPressed = Pressed;

    if (BusOk && ((Elapsed % 10000) == 0)) {
      UINT8  Cnfg06;

      // Charger watchdog kick (kernel max77705_chg_set_wdtmr_kick: CNFG_06 0xBD WDTCLR[1:0] = 01). Without it the
      // MAX77705 stops charging when its watchdog expires (CHG_DTLS 0x0B, seen under Windows, run batt1).
      if (!EFI_ERROR (Max77705RegRead (MAX77705_CHG_ADDR, 0xBD, &Cnfg06))) {
        (VOID)Max77705RegWrite (MAX77705_CHG_ADDR, 0xBD, (UINT8)((Cnfg06 & ~0x03) | 0x01));
      }
    }

    if ((Elapsed % 1000) == 0) {
      if (BusOk && !ChargerVbusPresent ()) {
        if (UnpluggedSince == 0) {
          UnpluggedSince = Elapsed + 1;
        } else if (Elapsed + 1 - UnpluggedSince >= CHG_UNPLUG_MS) {
          DEBUG ((DEBUG_WARN, "ChargerMode: charger removed - powering off\n"));
          ChargerFill (0, 0, EXYNOS_FB_WIDTH, EXYNOS_FB_HEIGHT, 0);
          PmicBusRelease ();
          gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        }
      } else {
        UnpluggedSince = 0;
      }
    }

    if ((Elapsed % 30000) == 0) {
      UINTN  Now = Tenths;

      if (BusOk && !EFI_ERROR (FuelGaugeReadSoc (&Now)) && (Now != Tenths)) {
        Tenths = Now;
        if (Visible) {
          ChargerDraw (Tenths, Visible);
        }
      }
    }

    if (Visible && (Elapsed - ShownSince >= CHG_SCREEN_ON_MS)) {
      Visible = FALSE;
      ChargerDraw (Tenths, Visible);
    }

    gBS->Stall (50000);
  }
}
