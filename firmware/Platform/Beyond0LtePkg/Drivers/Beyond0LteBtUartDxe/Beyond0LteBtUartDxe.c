/** @file
  D4c Bluetooth UEFI half: BT UART role latch + rungs R1/R2 (docs/exynos9820-port/D4c-BT-DESIGN.md,
  notes/bt-driver.md B6). Every constant comes from Exynos9820.h; register semantics from Samsung's
  serial_s3c.h / samsung.c (lineage-22.2), cited per line.

  Off unless BEYOND0LTE_BT_UART_PROBE is 1. Guard: progress word [1] = 0x6B while the probe runs; a boot whose
  previous boot stopped at 0x6B skips the probe. NOTHING HERE IS VALIDATED ON AN S10e YET.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Platform/Exynos9820.h>

#ifndef BEYOND0LTE_BT_UART_ROLE
#define BEYOND0LTE_BT_UART_ROLE  0         // role latch for Windows: clocks, pins, USI/UART set-up, bt_en power-on
#endif
#ifndef BEYOND0LTE_BT_UART_PROBE
#define BEYOND0LTE_BT_UART_PROBE  0        // R1 loopback + R2 HCI version into UEFILOG (implies the role latch)
#endif
#ifndef BEYOND0LTE_BT_UEFI_PATCHRAM
#define BEYOND0LTE_BT_UEFI_PATCHRAM  0     // R3 at ReadyToBoot: the first signal comes before UFS is up and later
#endif                                     // output lands after BootLog's flush - Windows patches instead (bt4)

#define BT_PROGRESS_TRYING  0x6Bu
#define BT_DSDT_WORD        11u           // progress word [11] = 'BTH0' -> DSDT BTH0 present
#define BT_DSDT_PRESENT     0x30485442u   // 'BTH0'
STATIC BOOLEAN  mBtR2Ok;   // the ROM answered READ_LOCAL_VERSION: R3 may run at ReadyToBoot
STATIC volatile UINT32  *CONST  mProgress = (volatile UINT32 *)(UINTN)0x8FE0F000ULL;

//
// UART registers (serial_s3c.h) and bits.
//
#define UART_ULCON      0x00u
#define UART_UCON       0x04u
#define UART_UFCON      0x08u
#define UART_UMCON      0x0Cu
#define UART_UTRSTAT    0x10u
#define UART_UERSTAT    0x14u
#define UART_UFSTAT     0x18u
#define UART_UMSTAT     0x1Cu
#define UART_UTXH       0x20u
#define UART_URXH       0x24u
#define UART_UBRDIV     0x28u
#define UART_UFRACVAL   0x2Cu   // S3C2443_DIVSLOT; Exynos has_fracval = 1
#define UART_UINTM      0x38u

#define ULCON_8N1           0x3u          // 8 data bits, no parity, 1 stop
#define UCON_TXRX_POLL      0x5u          // S3C2410_UCON_TXIRQMODE (1<<2) | RXIRQMODE (1<<0)
#define UCON_LOOPBACK       (1u << 5)     // S3C2443_UCON_LOOPBACK
#define UFCON_FIFO_ON       (1u << 0)     // S3C2410_UFCON_FIFOMODE
#define UFCON_RESET_RX      (1u << 1)
#define UFCON_RESET_TX      (1u << 2)
#define UMCON_RTS_ASSERT    (1u << 0)     // S3C2410_UMCOM_RTS_LOW: nRTS driven active
#define UFSTAT_RX_COUNT     0xFFu         // S5PV210_UFSTAT_RXMASK
#define UFSTAT_RX_FULL      (1u << 8)     // S5PV210_UFSTAT_RXFULL
#define UFSTAT_TX_FULL      (1u << 24)    // S5PV210_UFSTAT_TXFULL
#define UFSTAT_TX_COUNT     (0xFFu << 16) // S5PV210_UFSTAT_TXMASK
#define USI_SET_RESET       (1u << 0)     // serial_s3c.h USI_SET_RESET
#define USI_CLKREQ_ON       (1u << 1)     // serial_s3c.h USI_HWACG_CLKREQ_ON

//
// 115200 8N1 from the 26 MHz oscillator: 26e6 / 16 / 115200 = 14.106 -> UBRDIV 13, UFRACVAL round(0.106*16) = 2.
//
#define BT_UBRDIV_115200    13u
#define BT_UFRACVAL_115200  2u

//
// CMU gate/QCH convention proven on CMU_FSYS1 (Beyond0LtePcieDxe PcieLink.c): gate MANUAL (bit20) + CG_VAL (bit21);
// QCH ENABLE (bit0) + CLOCK_REQ (bit1).
//
#define CMU_GATE_ON         ((1u << 21) | (1u << 20))
#define CMU_QCH_ON          0x3u
#define CMU_MUX_SEL         (1u << 4)
#define CMU_MUX_BUSY        (1u << 7)
#define CMU_DIV_RATIO       0xFu
#define CMU_DIV_BUSY        (1u << 16)

#define GPIO_CON    0x0u
#define GPIO_DAT    0x4u

STATIC
VOID
WaitClear (
  IN UINTN   Address,
  IN UINT32  Mask
  )
{
  UINTN  Spin;

  for (Spin = 0; (Spin < 1000) && ((MmioRead32 (Address) & Mask) != 0); Spin++) {
    MicroSecondDelay (10);
  }
}

/** UART_BT clock: user mux -> OSCCLK (26 MHz), divider /1, gates + QCH on. **/
STATIC
VOID
BtClocksOn (
  VOID
  )
{
  UINTN  Cmu;

  Cmu = (UINTN)EXYNOS_CMU_PERIC1_BASE;
  DEBUG ((DEBUG_ERROR, "[BT] CMU before: mux %08x div %08x ipclk %08x pclk %08x rstn %08x qch %08x\n",
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_MUX), MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_DIV),
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_IPCLK), MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_PCLK),
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_RSTN), MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_QCH)));

  MmioAnd32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_MUX, ~CMU_MUX_SEL);
  WaitClear (Cmu + EXYNOS_CMU_PERIC1_UART_BT_MUX, CMU_MUX_BUSY);
  MmioAnd32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_DIV, ~CMU_DIV_RATIO);
  WaitClear (Cmu + EXYNOS_CMU_PERIC1_UART_BT_DIV, CMU_DIV_BUSY);
  MmioOr32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_IPCLK, CMU_GATE_ON);
  MmioOr32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_PCLK, CMU_GATE_ON);
  MmioOr32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_RSTN, CMU_GATE_ON);
  MmioOr32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_QCH, CMU_QCH_ON);

  DEBUG ((DEBUG_ERROR, "[BT] CMU after:  mux %08x div %08x ipclk %08x pclk %08x rstn %08x qch %08x\n",
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_MUX), MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_DIV),
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_IPCLK), MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_PCLK),
          MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_RSTN), MmioRead32 (Cmu + EXYNOS_CMU_PERIC1_UART_BT_QCH)));
}

/** Set one pin's 4-bit CON field. **/
STATIC
VOID
PinFunc (
  IN UINTN   Bank,
  IN UINT32  Pin,
  IN UINT32  Func
  )
{
  MmioAndThenOr32 (Bank + GPIO_CON, ~(0xFu << (Pin * 4)), Func << (Pin * 4));
}

STATIC
VOID
PinOut (
  IN UINTN    Bank,
  IN UINT32   Pin,
  IN BOOLEAN  High
  )
{
  if (High) {
    MmioOr32 (Bank + GPIO_DAT, 1u << Pin);
  } else {
    MmioAnd32 (Bank + GPIO_DAT, ~(1u << Pin));
  }

  PinFunc (Bank, Pin, EXYNOS_GPIO_FUNC_OUTPUT);
}

/** gpq0-0..3 = UART (fn 2), bt_wake gpg2-0 = out high, bt_hostwake gpa2-3 left as is (input). **/
STATIC
VOID
BtPinsOn (
  VOID
  )
{
  UINTN   Gpq0;
  UINT32  Pin;

  Gpq0 = (UINTN)EXYNOS_ALIVE_GPIO_BASE + EXYNOS_ALIVE_GPQ0_OFFSET;
  DEBUG ((DEBUG_ERROR, "[BT] pins before: gpq0 con %08x pud %08x, gpm8 con %08x dat %x, gpg2 con %08x\n",
          MmioRead32 (Gpq0 + GPIO_CON), MmioRead32 (Gpq0 + 0x8),
          MmioRead32 ((UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM8_OFFSET + GPIO_CON),
          MmioRead32 ((UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM8_OFFSET + GPIO_DAT),
          MmioRead32 ((UINTN)EXYNOS_PERIC0_GPIO_BASE + EXYNOS_PERIC0_GPG2_OFFSET + GPIO_CON)));
  for (Pin = 0; Pin < EXYNOS_BT_UART_PINS; Pin++) {
    PinFunc (Gpq0, Pin, EXYNOS_BT_UART_FN);
  }

  PinOut ((UINTN)EXYNOS_PERIC0_GPIO_BASE + EXYNOS_PERIC0_GPG2_OFFSET, EXYNOS_BT_WAKE_PIN, TRUE);
}

/** USI mode = UART, USI reset release, then 115200 8N1, FIFOs on, polling, RTS asserted, no auto flow control. **/
STATIC
VOID
BtUartInit (
  IN BOOLEAN  Loopback
  )
{
  UINTN  Uart;

  Uart = (UINTN)EXYNOS_UART_BT_BASE;
  MmioWrite32 ((UINTN)EXYNOS_USI_BT_MODE_BASE, EXYNOS_USI_MODE_UART);
  MmioWrite32 (Uart + EXYNOS_UART_USI_CON_OFF, USI_SET_RESET);
  MicroSecondDelay (10);
  MmioWrite32 (Uart + EXYNOS_UART_USI_CON_OFF, 0);
  MmioWrite32 (Uart + EXYNOS_UART_USI_OPT_OFF, USI_CLKREQ_ON);

  MmioWrite32 (Uart + UART_UINTM, 0xF);   // mask all UART interrupts: polling only
  MmioWrite32 (Uart + UART_ULCON, ULCON_8N1);
  MmioWrite32 (Uart + UART_UBRDIV, BT_UBRDIV_115200);
  MmioWrite32 (Uart + UART_UFRACVAL, BT_UFRACVAL_115200);
  MmioWrite32 (Uart + UART_UFCON, UFCON_FIFO_ON | UFCON_RESET_RX | UFCON_RESET_TX);
  MicroSecondDelay (100);
  MmioWrite32 (Uart + UART_UFCON, UFCON_FIFO_ON);
  MmioWrite32 (Uart + UART_UMCON, UMCON_RTS_ASSERT);
  MmioWrite32 (Uart + UART_UCON, UCON_TXRX_POLL | (Loopback ? UCON_LOOPBACK : 0));
  DEBUG ((DEBUG_ERROR, "[BT] UART: usi-mode %x usi-con %x opt %x ulcon %x ucon %x ufcon %x umcon %x ubrdiv %x frac %x ufstat %x\n",
          MmioRead32 ((UINTN)EXYNOS_USI_BT_MODE_BASE), MmioRead32 (Uart + EXYNOS_UART_USI_CON_OFF),
          MmioRead32 (Uart + EXYNOS_UART_USI_OPT_OFF), MmioRead32 (Uart + UART_ULCON), MmioRead32 (Uart + UART_UCON),
          MmioRead32 (Uart + UART_UFCON), MmioRead32 (Uart + UART_UMCON), MmioRead32 (Uart + UART_UBRDIV),
          MmioRead32 (Uart + UART_UFRACVAL), MmioRead32 (Uart + UART_UFSTAT)));
}

STATIC
UINT64
Ticks (
  VOID
  )
{
  UINT64  V;

  __asm__ __volatile__ ("isb; mrs %0, cntvct_el0" : "=r" (V));
  return V;
}

STATIC
VOID
BtTx (
  IN CONST UINT8  *Data,
  IN UINTN        Len
  )
{
  UINTN  Uart;
  UINTN  Index;
  UINTN  Spin;

  Uart = (UINTN)EXYNOS_UART_BT_BASE;
  for (Index = 0; Index < Len; Index++) {
    for (Spin = 0; (Spin < 100000) && ((MmioRead32 (Uart + UART_UFSTAT) & UFSTAT_TX_FULL) != 0); Spin++) {
    }

    MmioWrite32 (Uart + UART_UTXH, Data[Index]);
  }
}

/** Collect bytes for up to TimeoutUs (or until Max bytes); returns the count. **/
STATIC
UINTN
BtRx (
  OUT UINT8  *Buf,
  IN  UINTN  Max,
  IN  UINTN  TimeoutUs
  )
{
  UINTN   Uart;
  UINTN   Got;
  UINT64  End;
  UINT32  Stat;

  Uart = (UINTN)EXYNOS_UART_BT_BASE;
  Got  = 0;
  End  = Ticks () + (UINT64)TimeoutUs * (EXYNOS_OSCCLK_HZ / 1000000u);
  while ((Got < Max) && (Ticks () < End)) {
    Stat = MmioRead32 (Uart + UART_UFSTAT);
    if (((Stat & UFSTAT_RX_COUNT) != 0) || ((Stat & UFSTAT_RX_FULL) != 0)) {
      Buf[Got++] = (UINT8)MmioRead32 (Uart + UART_URXH);
    }
  }

  return Got;
}

STATIC
VOID
LogHex (
  IN CONST CHAR8  *Tag,
  IN CONST UINT8  *Buf,
  IN UINTN        Len
  )
{
  CHAR8  Line[3 * 48 + 1];
  UINTN  Index;
  UINTN  Pos;

  Pos = 0;
  for (Index = 0; (Index < Len) && (Index < 48); Index++) {
    Line[Pos++] = "0123456789ABCDEF"[Buf[Index] >> 4];
    Line[Pos++] = "0123456789ABCDEF"[Buf[Index] & 0xF];
    Line[Pos++] = ' ';
  }

  Line[Pos] = 0;
  DEBUG ((DEBUG_ERROR, "[BT] %a (%u B): %a\n", Tag, (UINT32)Len, Line));
}

/** R1: internal loopback at 115200; the transfer time also checks the 26 MHz clock assumption (32 B = 2.78 ms). **/
STATIC
VOID
BtR1Loopback (
  VOID
  )
{
  UINT8   Out[32];
  UINT8   In[32];
  UINTN   Index;
  UINTN   Got;
  UINTN   Match;
  UINT64  T0;
  UINT64  T1;

  BtUartInit (TRUE);
  for (Index = 0; Index < sizeof (Out); Index++) {
    Out[Index] = (UINT8)(Index ^ 0x5A);
  }

  T0  = Ticks ();
  BtTx (Out, sizeof (Out));
  Got = BtRx (In, sizeof (In), 50000);
  T1  = Ticks ();
  for (Index = 0, Match = 0; Index < Got; Index++) {
    Match += (In[Index] == Out[Index]) ? 1 : 0;
  }

  DEBUG ((DEBUG_ERROR, "[BT] R1 loopback: %u/32 bytes back, %u match, %lu us (2778 us expected at 115200)\n",
          (UINT32)Got, (UINT32)Match, (T1 - T0) / (EXYNOS_OSCCLK_HZ / 1000000u)));
}

/** Role latch: power-cycle the chip through bt_en gpm8-0 (low 20 ms, high, 150 ms for the ROM to come up). **/
STATIC
VOID
BtPowerOn (
  VOID
  )
{
  UINTN  BtEn;

  BtEn = (UINTN)EXYNOS_CMGP_GPIO_BASE + EXYNOS_CMGP_GPM8_OFFSET;
  PinOut (BtEn, EXYNOS_BT_EN_PIN, FALSE);
  MicroSecondDelay (20000);
  PinOut (BtEn, EXYNOS_BT_EN_PIN, TRUE);
  MicroSecondDelay (150000);
  DEBUG ((DEBUG_ERROR, "[BT] bt_en gpm8-0 high: con %08x dat %x; umstat %x uerstat %x\n",
          MmioRead32 (BtEn + GPIO_CON), MmioRead32 (BtEn + GPIO_DAT),
          MmioRead32 ((UINTN)EXYNOS_UART_BT_BASE + UART_UMSTAT), MmioRead32 ((UINTN)EXYNOS_UART_BT_BASE + UART_UERSTAT)));
}

/** R2: HCI_RESET and HCI_READ_LOCAL_VERSION to the freshly powered chip, raw replies logged. **/
STATIC
VOID
BtR2Hci (
  VOID
  )
{
  STATIC CONST UINT8  HciReset[]   = { 0x01, 0x03, 0x0C, 0x00 };
  STATIC CONST UINT8  HciVersion[] = { 0x01, 0x01, 0x10, 0x00 };
  UINT8               In[64];
  UINTN               Got;

  BtUartInit (FALSE);

  Got = BtRx (In, sizeof (In), 20000);
  LogHex ("R2 spontaneous", In, Got);

  BtTx (HciReset, sizeof (HciReset));
  Got = BtRx (In, sizeof (In), 500000);
  LogHex ("R2 HCI_RESET reply", In, Got);

  BtTx (HciVersion, sizeof (HciVersion));
  Got = BtRx (In, sizeof (In), 500000);
  LogHex ("R2 READ_LOCAL_VERSION reply", In, Got);
  if ((Got >= 15) && (In[0] == 0x04) && (In[1] == 0x0E) && (In[4] == 0x01) && (In[5] == 0x10) && (In[6] == 0x00)) {
    mBtR2Ok = TRUE;
    DEBUG ((DEBUG_ERROR, "[BT] R2: HCI ver %u rev %04x LMP ver %u manuf %04x LMP subver %04x\n",
            In[7], In[8] | (In[9] << 8), In[10], In[11] | (In[12] << 8), In[13] | (In[14] << 8)));
  }

  DEBUG ((DEBUG_ERROR, "[BT] after R2: umstat %x uerstat %x ufstat %x\n",
          MmioRead32 ((UINTN)EXYNOS_UART_BT_BASE + UART_UMSTAT), MmioRead32 ((UINTN)EXYNOS_UART_BT_BASE + UART_UERSTAT),
          MmioRead32 ((UINTN)EXYNOS_UART_BT_BASE + UART_UFSTAT)));
}

/**
  Send one HCI command (H4 type 01) and wait for its Command Complete (0x0E) or Command Status (0x0F) event.
  Returns the status byte (0x00 = success), or 0xFF on timeout. Other events in between are skipped.
**/
STATIC
UINT8
BtHciCmd (
  IN  UINT16       Opcode,
  IN  CONST UINT8  *Params,
  IN  UINT8        Len,
  IN  UINTN        TimeoutUs,
  OUT UINT8        *Reply      OPTIONAL,   // event parameters (>= 258 B) of the matching event
  OUT UINTN        *ReplyLen   OPTIONAL
  )
{
  UINT8   Head[4];
  UINT8   Ev[3];
  UINT8   Body[258];
  UINT64  End;

  Head[0] = 0x01;
  Head[1] = (UINT8)Opcode;
  Head[2] = (UINT8)(Opcode >> 8);
  Head[3] = Len;
  BtTx (Head, 4);
  if (Len != 0) {
    BtTx (Params, Len);
  }

  End = Ticks () + (UINT64)TimeoutUs * (EXYNOS_OSCCLK_HZ / 1000000u);
  while (Ticks () < End) {
    if (BtRx (Ev, 1, TimeoutUs) != 1) {
      break;
    }

    if (Ev[0] != 0x04) {
      continue;                                   // not an event packet: resync on the next 0x04
    }

    if ((BtRx (Ev + 1, 2, 100000) != 2) || (BtRx (Body, Ev[2], 100000) != Ev[2])) {
      break;
    }

    if ((Ev[1] == 0x0E) && (Ev[2] >= 4) && ((Body[1] | (Body[2] << 8)) == Opcode)) {
      if (Reply != NULL) {
        CopyMem (Reply, Body, Ev[2]);
        *ReplyLen = Ev[2];
      }

      return Body[3];
    }

    if ((Ev[1] == 0x0F) && (Ev[2] >= 4) && ((Body[2] | (Body[3] << 8)) == Opcode)) {
      return Body[0];
    }
  }

  return 0xFF;
}

/** Reads <volume>\S10EDIAG\bt\bcm4375B1_semco.hcd from the first file system that has it (never committed). **/
STATIC
UINT8 *
BtLoadPatch (
  OUT UINTN  *Size
  )
{
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *Info;
  UINTN                            InfoSize;
  UINT8                            *Data;
  EFI_STATUS                       Status;

  Data = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    return NULL;
  }

  for (Index = 0; (Index < Count) && (Data == NULL); Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
        EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
    {
      continue;
    }

    Status = Root->Open (Root, &File, L"\\S10EDIAG\\bt\\bcm4375B1_semco.hcd", EFI_FILE_MODE_READ, 0);
    DEBUG ((DEBUG_ERROR, "[BT] R3: volume %u/%u open %r\n", (UINT32)Index, (UINT32)Count, Status));
    if (!EFI_ERROR (Status)) {
      InfoSize = SIZE_OF_EFI_FILE_INFO + 256;
      Info     = AllocatePool (InfoSize);
      Status   = (Info != NULL) ? File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info) : EFI_OUT_OF_RESOURCES;
      DEBUG ((DEBUG_ERROR, "[BT] R3: GetInfo %r size %lu\n", Status, EFI_ERROR (Status) ? 0 : Info->FileSize));
      if (!EFI_ERROR (Status)) {
        *Size  = (UINTN)Info->FileSize;
        Data   = AllocatePool (*Size);
        Status = (Data != NULL) ? File->Read (File, Size, Data) : EFI_OUT_OF_RESOURCES;
        DEBUG ((DEBUG_ERROR, "[BT] R3: Read %r (%u B)\n", Status, (UINT32)*Size));
        if (EFI_ERROR (Status) && (Data != NULL)) {
          FreePool (Data);
          Data = NULL;
        }
      }

      if (Info != NULL) {
        FreePool (Info);
      }

      File->Close (File);
    }

    Root->Close (Root);
  }

  FreePool (Handles);
  return Data;
}

/**
  R3 at ReadyToBoot (the ESP is readable then): Broadcom patchram (btbcm.c behaviour, no code copied) -
  HCI_RESET, 0xFC2E download minidriver, 50 ms, every .hcd record as an HCI command waiting for its completion,
  250 ms after the last (0xFC4E launch), HCI_RESET, READ_LOCAL_VERSION (the LMP subversion must change from the
  ROM's 0x1111). ~20 s at 115200, so the 82 s watchdog is reloaded as it goes.
**/
STATIC
VOID
EFIAPI
BtR3OnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8   *Patch;
  UINTN   Size;
  UINTN   Pos;
  UINTN   Records;
  UINTN   Failed;
  UINT8   Status;
  UINT8   Reply[258];
  UINTN   ReplyLen;
  UINT64  T0;
  UINT32  Saved;

  STATIC UINTN  Tries;

  //
  // bt2/bt3: the FIRST ReadyToBoot signal came before any file system was connected (no volume at all), so keep
  // the event and try again on the next signals (BootLogDxe also expects more than one); give up after 4.
  //
  Tries++;
  Patch = BtLoadPatch (&Size);
  if (Patch == NULL) {
    DEBUG ((DEBUG_ERROR, "[BT] R3: try %u: \\S10EDIAG\\bt\\bcm4375B1_semco.hcd not loaded\n", (UINT32)Tries));
    if (Tries >= 4) {
      gBS->CloseEvent (Event);
    }

    return;
  }

  gBS->CloseEvent (Event);

  Saved        = mProgress[1];
  mProgress[1] = BT_PROGRESS_TRYING;
  T0           = Ticks ();
  Status       = BtHciCmd (0x0C03, NULL, 0, 500000, NULL, NULL);
  DEBUG ((DEBUG_ERROR, "[BT] R3: %u B patch; HCI_RESET %02x\n", (UINT32)Size, Status));
  Status = BtHciCmd (BCM_HCI_OP_START_DOWNLOAD, NULL, 0, 500000, NULL, NULL);
  DEBUG ((DEBUG_ERROR, "[BT] R3: download minidriver (0xFC2E) %02x\n", Status));
  MicroSecondDelay (50000);

  Records = 0;
  Failed  = 0;
  for (Pos = 0; Pos + 3 <= Size; ) {
    UINT16  Op;
    UINT8   Len;

    Op  = (UINT16)(Patch[Pos] | (Patch[Pos + 1] << 8));
    Len = Patch[Pos + 2];
    if (Pos + 3 + Len > Size) {
      break;
    }

    Status = BtHciCmd (Op, Patch + Pos + 3, Len, 500000, NULL, NULL);
    if (Status != 0) {
      Failed++;
      if (Failed <= 3) {
        DEBUG ((DEBUG_ERROR, "[BT] R3: record %u op %04x len %u -> %02x\n", (UINT32)Records, Op, Len, Status));
      }
    }

    Records++;
    Pos += 3 + Len;
    if ((Records % 100) == 0) {
      MmioWrite32 ((UINTN)EXYNOS_WDT_BASE + EXYNOS_WDT_WTCNT_OFFSET, EXYNOS_WDT_COUNT_82S);
    }
  }

  MicroSecondDelay (250000);
  DEBUG ((DEBUG_ERROR, "[BT] R3: %u records, %u failed, %lu ms\n", (UINT32)Records, (UINT32)Failed,
          (Ticks () - T0) / (EXYNOS_OSCCLK_HZ / 1000u)));
  Status = BtHciCmd (0x0C03, NULL, 0, 1000000, NULL, NULL);
  DEBUG ((DEBUG_ERROR, "[BT] R3: HCI_RESET after launch %02x\n", Status));
  ReplyLen = 0;
  Status   = BtHciCmd (0x1001, NULL, 0, 500000, Reply, &ReplyLen);
  if ((Status == 0) && (ReplyLen >= 12)) {
    DEBUG ((DEBUG_ERROR, "[BT] R3: patched: HCI ver %u rev %04x LMP ver %u manuf %04x LMP subver %04x\n",
            Reply[4], Reply[5] | (Reply[6] << 8), Reply[7], Reply[8] | (Reply[9] << 8), Reply[10] | (Reply[11] << 8)));
  } else {
    DEBUG ((DEBUG_ERROR, "[BT] R3: READ_LOCAL_VERSION after launch %02x (%u B)\n", Status, (UINT32)ReplyLen));
  }

  FreePool (Patch);
  mProgress[1] = Saved;
}

EFI_STATUS
EFIAPI
Beyond0LteBtUartEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Saved;

  //
  // DSDT BTH0._STA reads progress word [11] (0x8FE0F02C): 'BTH0' only when THIS boot latched the BT role, so a
  // value surviving a warm reset can never expose the device with its clocks and pins unset.
  //
  mProgress[BT_DSDT_WORD] = 0;
  if (!BEYOND0LTE_BT_UART_ROLE && !BEYOND0LTE_BT_UART_PROBE) {
    return EFI_SUCCESS;
  }

  if (mProgress[0] == BT_PROGRESS_TRYING) {
    DEBUG ((DEBUG_ERROR, "[BT] previous boot hung in the BT probe (progress 0x6B) - skipped this boot\n"));
    return EFI_SUCCESS;
  }

  Saved        = mProgress[1];
  mProgress[1] = BT_PROGRESS_TRYING;
  BtClocksOn ();
  BtPinsOn ();
  if (BEYOND0LTE_BT_UART_PROBE) {
    BtR1Loopback ();
  }

  BtUartInit (FALSE);
  BtPowerOn ();
  if (BEYOND0LTE_BT_UART_PROBE) {
    BtR2Hci ();
  }

  mProgress[1]            = Saved;
  mProgress[BT_DSDT_WORD] = BT_DSDT_PRESENT;
  if (mBtR2Ok && BEYOND0LTE_BT_UEFI_PATCHRAM) {
    EFI_EVENT  Event;

    gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, BtR3OnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
  }

  return EFI_SUCCESS;
}
