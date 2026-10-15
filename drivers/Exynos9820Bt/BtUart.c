/*
 * BtUart.c - direct-MMIO lower edge of Exynos9820Bt (D4c W2). See BtUart.h.
 *
 * Start-up (PrepareHardware, PASSIVE, interrupts not yet used): UART set-up in polling mode, HCI_RESET,
 * READ_LOCAL_VERSION, Broadcom patchram from the .hcd the package installs (only while the chip still reports the
 * ROM's LMP subversion 0x1111), then the UART switches to interrupt mode. Runtime: TX writes straight into the FIFO;
 * the ISR drains the RX FIFO into a ring, the DPC feeds the sample's H4 reassembler (Io.c), which completes the
 * pending IOCTL_BTHX_READ_HCI requests. Patchram sequence = btbcm.c behaviour (GPL, reference only, no code copied).
 */
#include "Driver.h"

// Io.c (sample) - not declared in its headers.
VOID ReadSegmentStateSet(PUART_READ_CONTEXT _ReadContext, UART_READ_STATE _NewState);
NTSTATUS ReadH4PacketReassemble(PUART_READ_CONTEXT _ReadContext, ULONG _BytesRead, PUCHAR _Buffer);

#define BT_PATCH_PATH   L"\\SystemRoot\\System32\\drivers\\bcm4375B1_semco.hcd"
#define BT_ROM_SUBVER   0x1111u    // LMP subversion the unpatched ROM reports (UEFI R2, notes/bt-driver.md)

static __forceinline ULONG Rd(PBT_UART U, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(U->Base + Off)); }
static __forceinline VOID Wr(PBT_UART U, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(U->Base + Off), V); }

static VOID
SleepMs(ULONG Ms)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

EVT_WDF_INTERRUPT_ISR BtUartIsr;
EVT_WDF_INTERRUPT_DPC BtUartDpc;

NTSTATUS
BtUartFindResources(
    _In_ WDFDEVICE    Device,
    _In_ PBT_UART     Uart,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    ULONG                           Count;
    ULONG                           Index;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    WDF_INTERRUPT_CONFIG            Config;
    NTSTATUS                        Status;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count; Index++)
    {
        Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Desc == NULL)
        {
            continue;
        }

        if ((Desc->Type == CmResourceTypeMemory) && (Uart->Base == NULL))
        {
            if (Desc->u.Memory.Length < BT_UART_MIN_WINDOW)
            {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }

            Uart->Base = (PUCHAR)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                PAGE_READWRITE | PAGE_NOCACHE);
            if (Uart->Base == NULL)
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            Uart->Length = Desc->u.Memory.Length;
            DoTrace(LEVEL_ERROR, TFLAG_PNP, ("UART window 0x%llx + 0x%x mapped", Desc->u.Memory.Start.QuadPart,
                    Desc->u.Memory.Length));
        }
        else if ((Desc->Type == CmResourceTypeInterrupt) && (Uart->Interrupt == NULL))
        {
            WDF_INTERRUPT_CONFIG_INIT(&Config, BtUartIsr, BtUartDpc);
            Config.InterruptRaw        = WdfCmResourceListGetDescriptor(ResourcesRaw, Index);
            Config.InterruptTranslated = Desc;
            Status = WdfInterruptCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &Uart->Interrupt);
            if (!NT_SUCCESS(Status))
            {
                DoTrace(LEVEL_ERROR, TFLAG_PNP, ("WdfInterruptCreate 0x%08X", Status));
                return Status;
            }
        }
    }

    if ((Uart->Base == NULL) || (Uart->Interrupt == NULL))
    {
        DoTrace(LEVEL_ERROR, TFLAG_PNP, ("_CRS needs one memory + one interrupt (mem %p irq %p)", Uart->Base,
                Uart->Interrupt));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    return STATUS_SUCCESS;
}

VOID
BtUartUnmap(
    _In_ PBT_UART Uart
    )
{
    if (Uart->Base != NULL)
    {
        Wr(Uart, BT_UART_UINTM, BT_UINT_ALL);
        MmUnmapIoSpace(Uart->Base, Uart->Length);
        Uart->Base = NULL;
    }
}

/** Polling-mode set-up (the UEFI half's proven sequence): USI reset release, 115200 8N1, FIFOs reset, RTS on. **/
VOID
BtUartInitHw(
    _In_ PBT_UART Uart
    )
{
    Wr(Uart, BT_UART_UINTM, BT_UINT_ALL);
    Wr(Uart, BT_UART_USI_CON, 1);
    KeStallExecutionProcessor(10);
    Wr(Uart, BT_UART_USI_CON, 0);
    Wr(Uart, BT_UART_USI_OPTION, 0x2);                 // CLKREQ_ON
    Wr(Uart, BT_UART_ULCON, BT_ULCON_8N1);
    Wr(Uart, BT_UART_UBRDIV, BT_UBRDIV_115200);
    Wr(Uart, BT_UART_UFRACVAL, BT_UFRACVAL_115200);
    Wr(Uart, BT_UART_UFCON, BT_UFCON_FIFO_ON | BT_UFCON_RESET);
    KeStallExecutionProcessor(100);
    Wr(Uart, BT_UART_UFCON, BT_UFCON_FIFO_ON);
    Wr(Uart, BT_UART_UMCON, BT_UMCON_RTS);
    Wr(Uart, BT_UART_UCON, BT_UCON_POLL);
    Wr(Uart, BT_UART_UINTP, BT_UINT_ALL);
    DoTrace(LEVEL_ERROR, TFLAG_UART, ("UART init: ucon %x ufcon %x ubrdiv %x frac %x umstat %x",
            Rd(Uart, BT_UART_UCON), Rd(Uart, BT_UART_UFCON), Rd(Uart, BT_UART_UBRDIV), Rd(Uart, BT_UART_UFRACVAL),
            Rd(Uart, BT_UART_UMSTAT)));
}

VOID
BtUartTx(
    _In_ PBT_UART Uart,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Index;
    ULONG Spin;

    for (Index = 0; Index < Length; Index++)
    {
        for (Spin = 0; (Spin < 200000) && ((Rd(Uart, BT_UART_UFSTAT) & BT_UFSTAT_TX_FULL) != 0); Spin++)
        {
            KeStallExecutionProcessor(1);
        }

        Wr(Uart, BT_UART_UTXH, Data[Index]);
    }
}

/** Polling receive used only during start-up (RX interrupt still masked). **/
static ULONG
BtUartRxPoll(
    _In_ PBT_UART Uart,
    _Out_writes_bytes_(Max) PUCHAR Buffer,
    _In_ ULONG Max,
    _In_ ULONG TimeoutMs
    )
{
    ULONG     Got;
    ULONGLONG End;
    ULONG     Stat;

    Got = 0;
    End = KeQueryInterruptTime() + (ULONGLONG)TimeoutMs * 10000ULL;
    while ((Got < Max) && (KeQueryInterruptTime() < End))
    {
        Stat = Rd(Uart, BT_UART_UFSTAT);
        if (((Stat & BT_UFSTAT_RX_COUNT) != 0) || ((Stat & BT_UFSTAT_RX_FULL) != 0))
        {
            Buffer[Got++] = (UCHAR)Rd(Uart, BT_UART_URXH);
        }
        else
        {
            KeStallExecutionProcessor(5);
        }
    }

    return Got;
}

UCHAR
BtUartHciCmd(
    _In_ PBT_UART Uart,
    _In_ USHORT Opcode,
    _In_reads_bytes_opt_(Length) const UCHAR *Params,
    _In_ UCHAR Length,
    _In_ ULONG TimeoutMs,
    _Out_writes_bytes_opt_(258) PUCHAR Reply,
    _Out_opt_ PULONG ReplyLength
    )
{
    UCHAR     Head[4];
    UCHAR     Ev[3];
    UCHAR     Body[258];
    ULONGLONG End;

    Head[0] = 0x01;
    Head[1] = (UCHAR)Opcode;
    Head[2] = (UCHAR)(Opcode >> 8);
    Head[3] = Length;
    BtUartTx(Uart, Head, sizeof(Head));
    if ((Length != 0) && (Params != NULL))
    {
        BtUartTx(Uart, Params, Length);
    }

    End = KeQueryInterruptTime() + (ULONGLONG)TimeoutMs * 10000ULL;
    while (KeQueryInterruptTime() < End)
    {
        if (BtUartRxPoll(Uart, Ev, 1, TimeoutMs) != 1)
        {
            break;
        }

        if (Ev[0] != 0x04)
        {
            continue;
        }

        if ((BtUartRxPoll(Uart, Ev + 1, 2, 100) != 2) || (BtUartRxPoll(Uart, Body, Ev[2], 100) != Ev[2]))
        {
            break;
        }

        if ((Ev[1] == 0x0E) && (Ev[2] >= 4) && ((USHORT)(Body[1] | (Body[2] << 8)) == Opcode))
        {
            if ((Reply != NULL) && (ReplyLength != NULL))
            {
                RtlCopyMemory(Reply, Body, Ev[2]);
                *ReplyLength = Ev[2];
            }

            return Body[3];
        }

        if ((Ev[1] == 0x0F) && (Ev[2] >= 4) && ((USHORT)(Body[2] | (Body[3] << 8)) == Opcode))
        {
            return Body[0];
        }
    }

    return 0xFF;
}

/** LMP subversion from READ_LOCAL_VERSION, 0 on failure. **/
static USHORT
BtUartLmpSubver(
    _In_ PBT_UART Uart
    )
{
    UCHAR Reply[258];
    ULONG Len;

    Len = 0;
    if ((BtUartHciCmd(Uart, 0x1001, NULL, 0, 500, Reply, &Len) == 0) && (Len >= 12))
    {
        DoTrace(LEVEL_ERROR, TFLAG_HCI, ("READ_LOCAL_VERSION: HCI %u LMP %u manuf %04x subver %04x", Reply[4], Reply[7],
                Reply[8] | (Reply[9] << 8), Reply[10] | (Reply[11] << 8)));
        return (USHORT)(Reply[10] | (Reply[11] << 8));
    }

    return 0;
}

/** Diagnostics: HCI Read Local Name (0x0C14) and Broadcom verbose config (0xFC79: chip id + firmware build). **/
static VOID
BtUartLogIdentity(
    _In_ PBT_UART Uart,
    _In_z_ const char *Tag
    )
{
    UCHAR Reply[258];
    ULONG Len;
    UCHAR St;
    CHAR  Name[64];
    ULONG Index;

    Len = 0;
    St  = BtUartHciCmd(Uart, 0x0C14, NULL, 0, 500, Reply, &Len);
    RtlZeroMemory(Name, sizeof(Name));
    for (Index = 0; (Index + 4 < Len) && (Index < sizeof(Name) - 1) && (Reply[4 + Index] != 0); Index++)
    {
        Name[Index] = ((Reply[4 + Index] >= 0x20) && (Reply[4 + Index] < 0x7F)) ? (CHAR)Reply[4 + Index] : '.';
    }

    DoTrace(LEVEL_ERROR, TFLAG_HCI, ("%s: READ_LOCAL_NAME %02x \"%s\"", Tag, St, Name));
    Len = 0;
    St  = BtUartHciCmd(Uart, 0xFC79, NULL, 0, 500, Reply, &Len);
    DoTrace(LEVEL_ERROR, TFLAG_HCI, ("%s: BCM verbose config %02x len %u: %02x %02x %02x %02x %02x %02x %02x %02x", Tag, St, Len,
            Len > 4 ? Reply[4] : 0, Len > 5 ? Reply[5] : 0, Len > 6 ? Reply[6] : 0, Len > 7 ? Reply[7] : 0,
            Len > 8 ? Reply[8] : 0, Len > 9 ? Reply[9] : 0, Len > 10 ? Reply[10] : 0, Len > 11 ? Reply[11] : 0));
}

/** Broadcom patchram: 0xFC2E, 50 ms, each .hcd record as a command, 250 ms after the last (0xFC4E launch). **/
static NTSTATUS
BtUartPatchram(
    _In_ PBT_UART Uart
    )
{
    UNICODE_STRING            Name;
    OBJECT_ATTRIBUTES         Attr;
    IO_STATUS_BLOCK           Iosb;
    HANDLE                    File;
    FILE_STANDARD_INFORMATION Info;
    PUCHAR                    Patch;
    ULONG                     Size;
    ULONG                     Pos;
    ULONG                     Records;
    ULONG                     Failed;
    NTSTATUS                  Status;

    RtlInitUnicodeString(&Name, BT_PATCH_PATH);
    InitializeObjectAttributes(&Attr, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attr, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        DoTrace(LEVEL_ERROR, TFLAG_HCI, ("patch file open 0x%08X", Status));
        return Status;
    }

    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (!NT_SUCCESS(Status) || (Info.EndOfFile.QuadPart <= 0) || (Info.EndOfFile.QuadPart > (1024 * 1024)))
    {
        ZwClose(File);
        return NT_SUCCESS(Status) ? STATUS_INVALID_IMAGE_FORMAT : Status;
    }

    Size  = (ULONG)Info.EndOfFile.QuadPart;
    Patch = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, Size, 'tBxE');
    if (Patch == NULL)
    {
        ZwClose(File);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Patch, Size, NULL, NULL);
    ZwClose(File);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Patch, 'tBxE');
        return Status;
    }

    DoTrace(LEVEL_ERROR, TFLAG_HCI, ("patchram: %u B, download minidriver %02x", Size,
            BtUartHciCmd(Uart, 0xFC2E, NULL, 0, 500, NULL, NULL)));
    SleepMs(50);
    Records = 0;
    Failed  = 0;
    for (Pos = 0; Pos + 3 <= Size; )
    {
        USHORT Op  = (USHORT)(Patch[Pos] | (Patch[Pos + 1] << 8));
        UCHAR  Len = Patch[Pos + 2];

        if (Pos + 3 + Len > Size)
        {
            break;
        }

        if (BtUartHciCmd(Uart, Op, Patch + Pos + 3, Len, 500, NULL, NULL) != 0)
        {
            Failed++;
        }

        Records++;
        Pos += 3 + Len;
    }

    ExFreePoolWithTag(Patch, 'tBxE');
    SleepMs(250);
    DoTrace(LEVEL_ERROR, TFLAG_HCI, ("patchram: %u records, %u failed", Records, Failed));
    return (Failed == 0) ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
}

BOOLEAN
BtUartBringUp(
    _In_ PBT_UART Uart
    )
{
    USHORT Subver;
    UCHAR  Status;

    BtUartInitHw(Uart);
    Status = BtUartHciCmd(Uart, 0x0C03, NULL, 0, 1000, NULL, NULL);
    DoTrace(LEVEL_ERROR, TFLAG_HCI, ("HCI_RESET %02x", Status));
    if (Status != 0)
    {
        return FALSE;
    }

    Subver = BtUartLmpSubver(Uart);
    BtUartLogIdentity(Uart, "before patch");
    if (Subver == BT_ROM_SUBVER)
    {
        BtUartPatchram(Uart);
        DoTrace(LEVEL_ERROR, TFLAG_HCI, ("HCI_RESET after patch %02x", BtUartHciCmd(Uart, 0x0C03, NULL, 0, 1000, NULL, NULL)));
        Subver = BtUartLmpSubver(Uart);
        BtUartLogIdentity(Uart, "after patch");
    }

    // Interrupt mode for the runtime path (RX still masked until BtUartRxStart).
    Wr(Uart, BT_UART_UFCON, BT_UFCON_IRQ);
    Wr(Uart, BT_UART_UCON, BT_UCON_IRQ);
    return (Subver != 0);
}

VOID
BtUartRxStart(
    _In_ PBT_UART Uart
    )
{
    PFDO_EXTENSION Fdo;

    Fdo = CONTAINING_RECORD(Uart, FDO_EXTENSION, Uart);
    WdfInterruptAcquireLock(Uart->Interrupt);
    ReadSegmentStateSet(&Fdo->ReadContext, GET_PKT_TYPE);
    Uart->Head = 0;
    Uart->Tail = 0;
    InterlockedExchange(&Uart->RxEnabled, 1);
    Wr(Uart, BT_UART_UINTP, BT_UINT_ALL);
    Wr(Uart, BT_UART_UINTM, BT_UINT_TXD | BT_UINT_MODEM);   // RX + error unmasked
    WdfInterruptReleaseLock(Uart->Interrupt);
}

VOID
BtUartRxStop(
    _In_ PBT_UART Uart
    )
{
    if (Uart->Base == NULL)
    {
        return;
    }

    WdfInterruptAcquireLock(Uart->Interrupt);
    InterlockedExchange(&Uart->RxEnabled, 0);
    Wr(Uart, BT_UART_UINTM, BT_UINT_ALL);
    WdfInterruptReleaseLock(Uart->Interrupt);
}

BOOLEAN
BtUartIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageId
    )
{
    PBT_UART Uart;
    ULONG    Pending;
    ULONG    Stat;

    UNREFERENCED_PARAMETER(MessageId);
    Uart    = &FdoGetExtension(WdfInterruptGetDevice(Interrupt))->Uart;
    Pending = Rd(Uart, BT_UART_UINTP) & BT_UINT_ALL;
    if (Pending == 0)
    {
        return FALSE;
    }

    if ((Pending & BT_UINT_ERROR) != 0)
    {
        Uart->LastErrStat = Rd(Uart, BT_UART_UERSTAT);
        Uart->LineErrors++;
    }

    for (;;)
    {
        Stat = Rd(Uart, BT_UART_UFSTAT);
        if (((Stat & BT_UFSTAT_RX_COUNT) == 0) && ((Stat & BT_UFSTAT_RX_FULL) == 0))
        {
            break;
        }

        if ((Uart->Head - Uart->Tail) >= BT_RX_RING_SIZE)
        {
            (VOID)Rd(Uart, BT_UART_URXH);
            Uart->RingOverflows++;
        }
        else
        {
            Uart->Ring[Uart->Head & (BT_RX_RING_SIZE - 1)] = (UCHAR)Rd(Uart, BT_UART_URXH);
            Uart->Head++;
        }
    }

    Wr(Uart, BT_UART_UINTP, Pending);
    if (Uart->RxEnabled)
    {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }

    return TRUE;
}

VOID
BtUartDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PFDO_EXTENSION Fdo;
    PBT_UART       Uart;
    ULONG          Head;
    ULONG          Chunk;
    ULONG          Offset;
    NTSTATUS       Status;

    UNREFERENCED_PARAMETER(AssociatedObject);
    Fdo  = FdoGetExtension(WdfInterruptGetDevice(Interrupt));
    Uart = &Fdo->Uart;
    Head = Uart->Head;
    while (Uart->Tail != Head)
    {
        Offset = Uart->Tail & (BT_RX_RING_SIZE - 1);
        Chunk  = min(Head - Uart->Tail, BT_RX_RING_SIZE - Offset);
        Status = ReadH4PacketReassemble(&Fdo->ReadContext, Chunk, &Uart->Ring[Offset]);
        if (!NT_SUCCESS(Status))
        {
            Fdo->OutOfSyncErrorCount++;
            DoTrace(LEVEL_ERROR, TFLAG_IO, ("H4 out of sync (%u), byte 0x%x", Fdo->OutOfSyncErrorCount, Uart->Ring[Offset]));
            ReadSegmentStateSet(&Fdo->ReadContext, GET_PKT_TYPE);
        }

        Uart->Tail += Chunk;
        if (Uart->Tail == Head)
        {
            Head = Uart->Head;
        }
    }
}
