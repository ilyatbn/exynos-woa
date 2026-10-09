/** @file
  Minimal Samsung/Exynos UART SerialPortLib for Exynos 9820 (Galaxy S10e,
  `beyond0lte`). Ported from Silicon/Exynos9810Pkg/Library/Exynos9810SerialPortLib.

  GOAL: the Milestone 1 equivalent on this SoC — print a character so you know
  the UEFI payload runs. On the 9820 this port is the ONLY sign-of-life channel:
  the 9810's PRAM breadcrumb had to be compiled out here because 0xFED14000 is
  INSIDE DRAM bank0 on this SoC (bank0 is 0x80000000 + 0x80000000, so it runs to
  0x100000000). See BEYOND0LTE_PRAM_DIAG in ArmPlatformLib.c and PlatformMemoryMapLib.c,
  and notes/s10e-live-reads.md §7.3.

  This assumes sboot already configured the UART clock and pinmux (it printed
  its own boot log), so we do NOT touch clocks/baud here — we only poll status
  and push bytes. Once you have sign-of-life, add proper Initialize() that
  programs ULCON/UCON/UFCON/UBRDIV for a known baud.

  Register offsets below are the standard Samsung S3C/Exynos UART layout — they
  are NOT addresses and are the same on both SoCs. The BASE address is the only
  thing supplied from outside this file, and it is never restated here: it comes
  from the Silicon package's PcdSerialRegisterBase, whose default and the header's
  EXYNOS_UART_BASE are the same read (uart@10440000). GetUartBase() and
  SerialPortInitialize() check the two against each other so a half-edited
  constant set cannot silently pick one of them.
 **/

#include <Base.h>
#include <Library/SerialPortLib.h>
#include <Library/BaseLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>

#include <Platform/Exynos9820.h>

//
// Samsung/Exynos UART register offsets, relative to the UART node's base.
//
#define EXYNOS_UART_ULCON     0x00  // Line control
#define EXYNOS_UART_UCON      0x04  // Control
#define EXYNOS_UART_UFCON     0x08  // FIFO control
#define EXYNOS_UART_UTRSTAT   0x10  // TX/RX status
#define EXYNOS_UART_UFSTAT    0x18  // FIFO status
#define EXYNOS_UART_UTXH      0x20  // Transmit buffer
#define EXYNOS_UART_URXH      0x24  // Receive buffer

#define UTRSTAT_TX_EMPTY      (1u << 1)  // Transmit buffer register empty
#define UTRSTAT_RX_READY      (1u << 0)  // Receive buffer data ready

#define ULCON_8N1             0x3   // 8 data bits, no parity, 1 stop bit
#define UCON_TXRX_POLL        0x5   // TX + RX in interrupt/polling mode

//
// The 9820's declared window is 0x100 (Exynos9820.h, vendor DT reg), where the
// 9810's was 0x1000. Every offset this library touches is inside the smaller
// window, so the shrink costs nothing here — asserted rather than eyeballed.
//
// URXH (+0x24) is the highest offset this library touches.
STATIC_ASSERT (
  EXYNOS_UART_URXH < EXYNOS_UART_SIZE,
  "A UART register this library touches falls outside the 9820's declared window"
  );

STATIC
UINTN
GetUartBase (
  VOID
  )
{
  //
  // Provided by Silicon/Exynos9820Pkg (PcdSerialRegisterBase). VERIFIED on the
  // live S10e and in the shipping vendor DT: uart@10440000, the same base the
  // 9810 uses. The header's EXYNOS_UART_BASE is the repo's single source of
  // MMIO addresses, so a zero PCD falls back to it rather than to a literal.
  //
  UINTN  Base;

  Base = (UINTN)FixedPcdGet64 (PcdSerialRegisterBase);
  if (Base == 0) {
    Base = (UINTN)EXYNOS_UART_BASE;
  }
  return Base;
}

/**
   Program the UART for a known 8N1 line format in polling mode.

   Deliberately conservative: sboot already configured the UART clock and baud
   divisor (it printed its own boot log over this port), so we do NOT touch
   UFCON/UBRDIV/UFRACVAL — reprogramming the baud wrong is the fastest way to turn
   readable output into garbage. We only force ULCON (8N1) and UCON (TX/RX on).
   Matches the standalone m1-hello payload so both agree.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  UINTN  Base;

  Base = GetUartBase ();
  if (Base == 0) {
    // Base not verified yet — skip init rather than poke physical address 0.
    return RETURN_SUCCESS;
  }

  //
  // The PCD and the silicon header are two copies of the same read. If they
  // disagree, one of them was edited and the log below is the only clue the
  // boot will produce — a wrong UART base means no output at all, which is
  // otherwise indistinguishable from a dead phone.
  //
  if (Base != (UINTN)EXYNOS_UART_BASE) {
    DEBUG ((DEBUG_ERROR,
      "Exynos9820SerialPortLib: PcdSerialRegisterBase 0x%lx != Exynos9820.h EXYNOS_UART_BASE 0x%lx\n",
      (UINT64)Base, (UINT64)EXYNOS_UART_BASE));
  }

  MmioWrite32 (Base + EXYNOS_UART_ULCON, ULCON_8N1);
  MmioWrite32 (Base + EXYNOS_UART_UCON,  UCON_TXRX_POLL);
  // UFCON / UBRDIV / UFRACVAL left as sboot configured them.
  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN   NumberOfBytes
  )
{
  UINTN  Base;
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  Base = GetUartBase ();
  if (Base == 0) {
    return 0;  // not configured yet
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    // Wait until the transmit buffer is empty — BOUNDED so a UART that is not
    // TX-ready in this boot context can never hang the whole firmware (a real
    // risk during bring-up: PeilessSec writes its banner here before MMU).
    // Bound kept SMALL (~8k) so that when the UART never drains (no cable in this
    // chainloaded context), each dropped byte costs microseconds, not ~2 ms — a
    // verbose DEBUG/ASSERT burst can never stall the boot for seconds.
    UINT32  Spin = 0;
    while ((MmioRead32 (Base + EXYNOS_UART_UTRSTAT) & UTRSTAT_TX_EMPTY) == 0) {
      if (++Spin > 8000u) {
        return Index;  // give up; drop the rest rather than spin forever
      }
      CpuPause ();
    }
    MmioWrite32 (Base + EXYNOS_UART_UTXH, Buffer[Index]);
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN   NumberOfBytes
  )
{
  UINTN  Base;
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  Base = GetUartBase ();
  if (Base == 0) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    UINT32  Spin = 0;
    while ((MmioRead32 (Base + EXYNOS_UART_UTRSTAT) & UTRSTAT_RX_READY) == 0) {
      if (++Spin > 2000000u) {
        return Index;  // bounded: never hang waiting for input that never comes
      }
      CpuPause ();
    }
    Buffer[Index] = (UINT8)MmioRead32 (Base + EXYNOS_UART_URXH);
  }

  return NumberOfBytes;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  UINTN  Base;

  Base = GetUartBase ();
  if (Base == 0) {
    return FALSE;
  }

  return (BOOLEAN)((MmioRead32 (Base + EXYNOS_UART_UTRSTAT) & UTRSTAT_RX_READY) != 0);
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  if (Control == NULL) {
    return RETURN_INVALID_PARAMETER;
  }
  *Control = 0;
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
  // TODO: program ULCON/UCON/UFCON/UBRDIV for a real baud once needed.
  // The 9820's UART node declares fifo-size = 0x100 (Exynos9820.h), which is
  // what a real UFCON/UBRDIV programming pass would have to respect; the baud
  // sboot left in place has NOT been read on this SoC, so it stays UNVERIFIED.
  return RETURN_SUCCESS;
}
