/*
 * BtUart.h - direct-MMIO lower edge of Exynos9820Bt (D4c W2): the Samsung USI v2 UART (uart@10840000) that carries
 * the BCM4375 Bluetooth HCI (H4). Replaces the serialhcibus sample's UART-controller I/O target.
 *
 * Register semantics: Samsung serial_s3c.h / samsung.c (LineageOS android_kernel_samsung_exynos9820, lineage-22.2),
 * behaviour reference only. The same sequence is proven on the device by the UEFI half (Beyond0LteBtUartDxe,
 * notes/bt-driver.md B6: R1 loopback, R2 HCI version from the ROM).
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>

#define BT_UART_ULCON       0x00u
#define BT_UART_UCON        0x04u
#define BT_UART_UFCON       0x08u
#define BT_UART_UMCON       0x0Cu
#define BT_UART_UTRSTAT     0x10u
#define BT_UART_UERSTAT     0x14u
#define BT_UART_UFSTAT      0x18u
#define BT_UART_UMSTAT      0x1Cu
#define BT_UART_UTXH        0x20u
#define BT_UART_URXH        0x24u
#define BT_UART_UBRDIV      0x28u
#define BT_UART_UFRACVAL    0x2Cu
#define BT_UART_UINTP       0x30u      // pending, write 1 to clear
#define BT_UART_UINTM       0x38u      // mask, 1 = masked
#define BT_UART_USI_CON     0xC4u
#define BT_UART_USI_OPTION  0xC8u
#define BT_UART_MIN_WINDOW  0x100u     // DSDT BTH0 Memory32Fixed length

#define BT_ULCON_8N1        0x3u
#define BT_UCON_POLL        0x5u       // TX + RX interrupt/polling mode, no RX timeout interrupt
#define BT_UCON_IRQ         0x3C5u     // S5PV210_UCON_DEFAULT: TX/RX level irq, RX FIFO timeout, RX error irq, TX/RX mode
#define BT_UFCON_FIFO_ON    0x1u
#define BT_UFCON_RESET      0x6u       // reset RX (bit1) + TX (bit2)
#define BT_UFCON_IRQ        0x111u     // S5PV210_UFCON_DEFAULT: FIFO on, TX trigger 4, RX trigger 4
#define BT_UMCON_RTS        0x1u       // nRTS asserted (manual; ignored while AFC is on)
#define BT_UMCON_AFC        0x10u      // S3C2410_UMCOM_AFC (serial_s3c.h:143): hardware RTS/CTS; bits [7:5] = 0 = AFC_63:
                                       // RTS drops at the top RX FIFO level (RFCOMM echo lost RX bytes without it)
#define BT_UFSTAT_RX_COUNT  0xFFu
#define BT_UFSTAT_RX_FULL   (1u << 8)
#define BT_UFSTAT_TX_FULL   (1u << 24)
#define BT_UTRSTAT_TX_EMPTY (1u << 2)  // transmitter (shift register + FIFO) empty
#define BT_UINT_RXD         (1u << 0)
#define BT_UINT_ERROR       (1u << 1)
#define BT_UINT_TXD         (1u << 2)
#define BT_UINT_MODEM       (1u << 3)
#define BT_UINT_ALL         0xFu

// 115200 8N1 from the 26 MHz oscillator the UEFI half selects (UEFI R1 timing confirmed 26 MHz).
#define BT_UBRDIV_115200    13u
#define BT_UFRACVAL_115200  2u
// Operating speed (notes/goal-bbb-bt-wifi.md T3): A2DP SBC needs ~41 KB/s, 115200 carries 10.5 KiB/s.
// Measured on the device (1.0.0.7, loopback + QPC): 13/2 -> 114985 (the fraction works there), 0/10 -> 1584761: with
// UBRDIV 0 the fraction is ignored, the UART runs at 26 MHz / 16 = 1625000. 1 Mbaud is out of reach from 26 MHz, so
// the operating speed is that exact 1625000 (UBRDIV 0, UFRACVAL 0). The controller follows Broadcom's
// Update_UART_Baud_Rate (0xFC18, {le16 0, le32 baud}, Linux btbcm.h:12-15 / hci_bcm.c:178-224; 0xFC45 only > 3 Mbaud).
#define BT_BAUD_OPER        1625000u
#define BT_UBRDIV_OPER      0u
#define BT_UFRACVAL_OPER    0u

#define BT_RX_RING_SIZE     16384u     // power of two (4096 overflowed 8611 times in the RFCOMM echo test)
#define BT_TX_RING_SIZE     16384u     // power of two; the TX FIFO is refilled from the ISR (TXD level interrupt)

typedef struct _BT_UART
{
    PUCHAR          Base;
    SIZE_T          Length;
    WDFINTERRUPT    Interrupt;
    UCHAR           Ring[BT_RX_RING_SIZE];
    volatile ULONG  Head;              // written by the ISR
    volatile ULONG  Tail;              // written by the DPC
    volatile LONG   RxEnabled;
    volatile LONG   RxThrottled;       // ring full: RX interrupt masked, bytes wait in the FIFO, AFC drops RTS
    ULONG           RingOverflows;
    ULONG           LineErrors;
    ULONG           LoggedLineErrors;  // DPC logs the counters when they change
    ULONG           LoggedOverflows;
    ULONG           LastErrStat;
    UCHAR           TxRing[BT_TX_RING_SIZE];
    volatile ULONG  TxHead;            // written by BtUartTxQueue (under the interrupt lock)
    volatile ULONG  TxTail;            // written by TxFill (ISR or BtUartTxQueue, under the interrupt lock)
    volatile LONG   TxIrq;             // runtime: BtUartTxQueue uses the ring (set by BtUartRxStart)
    ULONG           TxRingWaits;       // BtUartTxQueue found the ring full and had to wait
    ULONG           TxDropped;         // bytes dropped after the bounded wait (should stay 0)
} BT_UART, *PBT_UART;

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
BtUartFindResources(
    _In_ WDFDEVICE    Device,
    _In_ PBT_UART     Uart,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    );

VOID
BtUartUnmap(
    _In_ PBT_UART Uart
    );

VOID
BtUartInitHw(
    _In_ PBT_UART Uart
    );

VOID
BtUartTx(
    _In_ PBT_UART Uart,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length
    );

// Runtime TX (any IRQL <= DISPATCH): copies into the TX ring and lets the ISR feed the FIFO; polled BtUartTx
// before BtUartRxStart.
VOID
BtUartTxQueue(
    _In_ PBT_UART Uart,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
UCHAR
BtUartHciCmd(
    _In_ PBT_UART Uart,
    _In_ USHORT Opcode,
    _In_reads_bytes_opt_(Length) const UCHAR *Params,
    _In_ UCHAR Length,
    _In_ ULONG TimeoutMs,
    _Out_writes_bytes_opt_(258) PUCHAR Reply,
    _Out_opt_ PULONG ReplyLength
    );

_IRQL_requires_max_(PASSIVE_LEVEL)
BOOLEAN
BtUartBringUp(
    _In_ PBT_UART Uart
    );

VOID
BtUartRxStart(
    _In_ PBT_UART Uart
    );

VOID
BtUartRxStop(
    _In_ PBT_UART Uart
    );
