/*  BtTransport.c — S10e BCM4375 Bluetooth-over-UART transport (skeleton).

    Architecture: modeled on the WDK sample bluetooth/serialhcibus (MS-PL) —
    a function driver that will expose one BthxDDI HCI function to bthport.sys
    over an H4-framed UART. The lower edge is DIRECT MMIO to the Samsung UART
    (no Windows serial stack exists for it — design section 3).

    HONEST STAGING (rung ladder: D4c-BT-DESIGN.md section 1):
      * implemented now: UART programming, H4 TX framing, the .hcd record
        walker (pure logic, matches the byte-exact parse in notes B2).
      * NOT-IMPLEMENTED markers name the rung and the reference file each
        later stage copies from. The FDO enumerates; nothing pretends a
        device observation happened.
*/

#include "Exynos9820Bt.h"

/* -------------------------------------------------------------------- */
/* UART register access                                                  */
/* -------------------------------------------------------------------- */

ULONG
BtUartRead (_In_ PBT_DEVICE D, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG ((PULONG)(D->UartWindow + Offset));
}

VOID
BtUartWrite (_In_ PBT_DEVICE D, _In_ ULONG Offset, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG ((PULONG)(D->UartWindow + Offset), Value);
}

NTSTATUS
BtUartInit (_In_ PBT_DEVICE D)
/* 8N1, interrupt mode, FIFOs on, 115200. USI v2 quirks per vendor
 * samsung.c exynos_usi_init: release pad reset (USI_CON 1->0), then
 * USI_OPTION CLKREQ_ON or the clock never reaches the block.          */
{
    ULONG div;

    BtUartWrite (D, BT_UART_USI_CON, BT_USI_SET_RESET);
    BtUartWrite (D, BT_UART_USI_CON, 0);
    BtUartWrite (D, BT_UART_USI_OPT, BT_USI_CLKREQ_ON);

    BtUartWrite (D, BT_UART_ULCON, BT_ULCON_8N1);
    BtUartWrite (D, BT_UART_UCON, BT_UCON_INT_MODE);
    BtUartWrite (D, BT_UART_UFCON, BT_UFCON_FIFO_EN);

    /* UNVERIFIED: BT_UART_SRC_CLK_HZ and the fractional encoding —
     * rung R1 (loopback) closes both.                                 */
    div = (BT_UART_SRC_CLK_HZ / (16u * BT_UART_INIT_BAUD)) - 1u;
    BtUartWrite (D, BT_UART_UBRDIV, div);
    BtUartWrite (D, BT_UART_UFRACVAL, 0);

    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------- */
/* H4 framing                                                            */
/* -------------------------------------------------------------------- */

#define BT_TX_SPIN_LIMIT  1000000u   /* FIFO-drain wait; R1 replaces with
                                        ISR-driven TX once IRQs are wired */

static
BOOLEAN
BtUartPutByte (_In_ PBT_DEVICE D, _In_ UCHAR B)
{
    for (UINT32 i = 0; i < BT_TX_SPIN_LIMIT; i++) {
        if (BtUartRead (D, BT_UART_UTRSTAT) & BT_UTRSTAT_TX_EMPTY) {
            BtUartWrite (D, BT_UART_UTXH, B);
            return TRUE;
        }
    }
    return FALSE;   /* caller counts failures; rung R1 surfaces them */
}

NTSTATUS
BtH4SendCommand (_In_ PBT_DEVICE D, _In_ USHORT Opcode,
                 _In_ UCHAR ParamLen, _In_reads_(ParamLen) PUCHAR Params)
{
    if (!BtUartPutByte (D, H4_COMMAND))    return STATUS_DEVICE_NOT_READY;
    if (!BtUartPutByte (D, (UCHAR)(Opcode & 0xFF)))   return STATUS_DEVICE_NOT_READY;
    if (!BtUartPutByte (D, (UCHAR)(Opcode >> 8)))     return STATUS_DEVICE_NOT_READY;
    if (!BtUartPutByte (D, ParamLen))      return STATUS_DEVICE_NOT_READY;
    for (UCHAR i = 0; i < ParamLen; i++)
        if (!BtUartPutByte (D, Params[i])) return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

NTSTATUS
BtH4SendAcl (_In_ PBT_DEVICE D, _In_ PVOID Buffer, _In_ size_t Length)
{
    PUCHAR p = (PUCHAR)Buffer;
    if (!BtUartPutByte (D, H4_ACL)) return STATUS_DEVICE_NOT_READY;
    for (size_t i = 0; i < Length; i++)
        if (!BtUartPutByte (D, p[i])) return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------- */
/* .hcd record walker — the format proven byte-exact in notes B2:
 *   repeated: opcode u16 LE | plen u8 | params ; end of buffer = end.  */
/* -------------------------------------------------------------------- */

NTSTATUS
BtHcdNextRecord (_Inout_ PUCHAR *Cursor, _In_ PUCHAR End, _Out_ BT_HCD_RECORD *Rec)
{
    PUCHAR p = *Cursor;

    if (p + 3 > End) return STATUS_NO_MORE_ENTRIES;
    Rec->Opcode   = (USHORT)(p[0] | (p[1] << 8));
    Rec->ParamLen = p[2];
    if (p + 3 + Rec->ParamLen > End) return STATUS_DATA_ERROR;
    Rec->Params = p + 3;
    *Cursor = p + 3 + Rec->ParamLen;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------------------- */
/* BthxDDI dispatch. NOTE: bthport.sys sends BTHX IOCTLs to the HCI
 *  FUNCTION (PDO) once R4 splits FDO/PDO — this FDO queue is the
 *  placeholder landing point (reference: serialhcibus Fdo.c + pdo.c).  */
/* -------------------------------------------------------------------- */

static
VOID
EvtIoDeviceControl (_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                    _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength,
                    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER (Queue);
    UNREFERENCED_PARAMETER (OutputBufferLength);
    UNREFERENCED_PARAMETER (InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_BTHX_SET_VERSION:
    case IOCTL_BTHX_GET_VERSION:
    case IOCTL_BTHX_QUERY_CAPABILITIES:
        /* NOT-IMPLEMENTED (R4): copy shape from serialhcibus Fdo.c
         * (MS-PL); capabilities = HCI transport, LE+ACL, SCO None
         * (slice 1, design section 4.5).                              */
        WdfRequestComplete (Request, STATUS_NOT_IMPLEMENTED);
        break;
    case IOCTL_BTHX_READ_HCI:
    case IOCTL_BTHX_WRITE_HCI:
        /* NOT-IMPLEMENTED (R4/R5): wire to the H4 engine + RX ring;
         * reference serialhcibus io.c read pump + cywbtserialbus.     */
        WdfRequestComplete (Request, STATUS_NOT_IMPLEMENTED);
        break;
    default:
        WdfRequestComplete (Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}

/* -------------------------------------------------------------------- */
/* Bring-up state machine                                                */
/* -------------------------------------------------------------------- */

static
VOID
BtBringup (_In_ PBT_DEVICE D)
{
    UNREFERENCED_PARAMETER (D);
    /* NOT-IMPLEMENTED (R2): HCI_RESET + HCI_OP_READ_LOCAL_VERSION via
     * BtH4SendCommand + RX match; print version. Reference: btbcm.c
     * btbcm_read_info (GPL — semantics only).                         */
    /* NOT-IMPLEMENTED (R3): if RAM state: 0xFC2E, 50 ms, walk the .hcd
     * with BtHcdNextRecord sending each record via BtH4SendCommand,
     * then 0xFC4E FFFFFFFF, 250 ms. Reference: btbcm_patchram (GPL —
     * semantics only) / cywbtserialbus device.c (MS-PL).              */
    /* NOT-IMPLEMENTED (Q2, registry-gated): 0xFC45 + 0xFC18 baud switch,
     * then re-program UBRDIV. Reference: hci_bcm.c bcm_set_baudrate.  */
    D->State = BtStateInit;
}

/* -------------------------------------------------------------------- */
/* Device setup                                                          */
/* -------------------------------------------------------------------- */

static
NTSTATUS
EvtDeviceAdd (_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    PBT_DEVICE D;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_IO_QUEUE_CONFIG qcfg;
    NTSTATUS status;

    UNREFERENCED_PARAMETER (Driver);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&attrs, BT_DEVICE);

    status = WdfDeviceCreate (&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS (status)) return status;

    D = BtGetDeviceContext (device);
    D->WdfDevice = device;
    D->State = BtStateInit;

    /* NOT-IMPLEMENTED (R4): parse _CRS per the binding contract in
     * Exynos9820Bt.h (desc 0 UART MEM -> map into D->UartWindow;
     * Interrupt GSIV 440 -> WdfInterruptCreate; desc 1 GpioInt via the
     * SAMS9821 GpioClx controller -> D->HostwakeInterrupt). Until then
     * the FDO enumerates with no mapped window and BtUartInit is NOT
     * called — calling it on an unmapped window would fault.          */
    if (D->UartWindow != NULL) {
        status = BtUartInit (D);
        if (!NT_SUCCESS (status)) return status;
        BtBringup (D);
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE (&qcfg, WDF_IO_QUEUE_DISPATCH_PARALLEL);
    qcfg.EvtIoDeviceControl = EvtIoDeviceControl;
    status = WdfIoQueueCreate (device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    return status;
}

NTSTATUS
DriverEntry (_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT (&config, EvtDeviceAdd);
    return WdfDriverCreate (DriverObject, RegistryPath,
                            WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
