/*
 * Driver.c - Exynos9820Mfc (D10, notes/d10-mfc-main.md, notes/d10-mfc-decode-flow.md): binds ACPI\SAMS982A (DSDT
 * VIDC), boots the MFC v13 firmware and runs SYS_INIT (V-W1), runs a polled H.264 decode self-test of mfc_test.h264
 * when that file exists (V-W2), proves the R2H interrupt (GSIV 370), then serves user-mode decode sessions through
 * IOCTLs (V3, MfcIoctl.h; engine in Decode.c) with commands completed by the interrupt.
 *
 * Boot = vendor __mfc_init_hw (mfc_run.c) as proven from UEFI (Beyond0LteMfcDxe R3/R4): reset (R2H/H2R/FW_VERSION
 * cleared, 1024 words from 0xF000 cleared, MFC_RESET 0x1FFF then 0), RISC_BASE_ADDRESS = buffer physical address
 * (SYSMMUs left in reset bypass), RISC_ON 1 / MFC_OFF 0, R2H FW_STATUS_RET 12, then CONTEXT_MEM_ADDR/SIZE + H2R
 * SYS_INIT, R2H SYS_INIT_RET 1. The last R2H of the boot sequence is left pending: once WDF connects the interrupt
 * the ISR must fire - that is the interrupt-path proof.
 *
 * Memory: ONE contiguous non-cached allocation below 4 GiB, firmware at its 1 MiB aligned base, common context at
 * +1 MiB, self-test area at +8 MiB, user session region at +16 MiB. Every MFC buffer must lie above the RISC base
 * (a buffer below it got OPEN_INSTANCE ERR_RET 10, vidc3).
 */
#include "Mfc.h"
#include <initguid.h>
#include "MfcIoctl.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MfcEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MfcEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE MfcEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED MfcEvtD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED MfcEvtD0ExitPreInterruptsDisabled;
EVT_WDF_INTERRUPT_ISR MfcEvtIsr;
EVT_WDF_INTERRUPT_DPC MfcEvtDpc;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL MfcEvtIoDeviceControl;
EVT_WDF_FILE_CLEANUP MfcEvtFileCleanup;

static VOID
MfcReset(_In_ PMFC_DEVICE M)
{
    ULONG Index;

    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_HOST2RISC_CMD, 0);
    Wr(M, MFC_REG_FW_VERSION, 0);
    for (Index = 0; Index < MFC_REG_REG_CLEAR_COUNT; Index++)
    {
        Wr(M, MFC_REG_REG_CLEAR_BEGIN + Index * 4, 0);
    }

    Wr(M, MFC_REG_MFC_RESET, 0x1FFF);
    Wr(M, MFC_REG_MFC_RESET, 0);
}

static NTSTATUS
MfcLoadFile(_In_ PCWSTR Path, _Out_writes_bytes_(MaxSize) PUCHAR Dest, _In_ ULONG MaxSize, _Out_ PULONG Size)
{
    UNICODE_STRING            Name;
    OBJECT_ATTRIBUTES         Attr;
    IO_STATUS_BLOCK           Iosb;
    HANDLE                    File;
    FILE_STANDARD_INFORMATION Info;
    NTSTATUS                  Status;

    *Size = 0;
    RtlInitUnicodeString(&Name, Path);
    InitializeObjectAttributes(&Attr, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attr, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        MfcTrace("open %ws 0x%08X\n", Path, Status);
        return Status;
    }

    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && ((Info.EndOfFile.QuadPart <= 0) || (Info.EndOfFile.QuadPart > MaxSize)))
    {
        Status = STATUS_INVALID_IMAGE_FORMAT;
    }

    if (NT_SUCCESS(Status))
    {
        *Size  = (ULONG)Info.EndOfFile.QuadPart;
        Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Dest, *Size, NULL, NULL);
    }

    ZwClose(File);
    return Status;
}

static NTSTATUS
MfcAllocate(_In_ PMFC_DEVICE M)
{
    static const ULONG Sessions[] = { MFC_SESSION_SIZE_MAX, MFC_SESSION_SIZE_MIN, 0 };
    PHYSICAL_ADDRESS   Low;
    PHYSICAL_ADDRESS   High;
    PHYSICAL_ADDRESS   Boundary;
    PHYSICAL_ADDRESS   Pa;
    ULONG              Index;
    ULONG              Pad;

    Low.QuadPart      = 0;
    High.QuadPart     = 0xFFFFFFFFLL;   // the MFC takes 32-bit addresses (raw daddr writes in the vendor driver)
    Boundary.QuadPart = 0;
    for (Index = 0; Index < RTL_NUMBER_OF(Sessions); Index++)
    {
        M->BufferAllocSize = (SIZE_T)MFC_FW_BUFFER_SIZE + Sessions[Index] + MFC_BUFFER_ALIGN;
        M->BufferAlloc     = MmAllocateContiguousMemorySpecifyCache(M->BufferAllocSize, Low, High, Boundary, MmNonCached);
        if (M->BufferAlloc != NULL)
        {
            M->SessionSize = Sessions[Index];
            break;
        }
    }

    if (M->BufferAlloc == NULL)
    {
        MfcTrace("no contiguous buffer below 4 GiB\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Pa                   = MmGetPhysicalAddress(M->BufferAlloc);
    Pad                  = (ULONG)(((Pa.QuadPart + MFC_BUFFER_ALIGN - 1) & ~(LONGLONG)(MFC_BUFFER_ALIGN - 1)) - Pa.QuadPart);
    M->Buffer            = (PUCHAR)M->BufferAlloc + Pad;
    M->BufferPa.QuadPart = Pa.QuadPart + Pad;
    RtlZeroMemory(M->Buffer, MFC_FW_BUFFER_SIZE);
    return STATUS_SUCCESS;
}

static NTSTATUS
MfcBoot(_In_ PMFC_DEVICE M)
{
    ULONG    FwSize;
    ULONG    R2h;
    NTSTATUS Status;

    Status = MfcAllocate(M);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = MfcLoadFile(MFC_FW_PATH, M->Buffer, MFC_CTX_OFFSET, &FwSize);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    MfcReset(M);
    Wr(M, MFC_REG_RISC_BASE_ADDRESS, M->BufferPa.LowPart);
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    Wr(M, MFC_REG_RISC_ON, 1);
    Wr(M, MFC_REG_MFC_OFF, 0);
    R2h = MfcPollR2h(M, 2000);
    MfcTrace("fw %u B at PA 0x%llx (session region 0x%x B): R2H %u (FW_STATUS_RET %u), MFC_VERSION 0x%x, FW_VERSION 0x%08x\n",
             FwSize, M->BufferPa.QuadPart, M->SessionSize, R2h, MFC_R2H_FW_STATUS_RET, Rd(M, MFC_REG_MFC_VERSION),
             Rd(M, MFC_REG_FW_VERSION));
    if ((R2h != MFC_R2H_FW_STATUS_RET) || (Rd(M, MFC_REG_MFC_VERSION) != MFC_IP_VER))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    Wr(M, MFC_REG_RISC2HOST_CMD, 0);
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    Wr(M, MFC_REG_CONTEXT_MEM_ADDR, M->BufferPa.LowPart + MFC_CTX_OFFSET);
    Wr(M, MFC_REG_CONTEXT_MEM_SIZE, MFC_DEV_CTX_SIZE);
    Wr(M, MFC_REG_HOST2RISC_CMD, MFC_H2R_CMD_SYS_INIT);
    Wr(M, MFC_REG_HOST2RISC_INT, 1);
    R2h = MfcPollR2h(M, 1000);
    MfcTrace("SYS_INIT: R2H %u (SYS_INIT_RET %u) - interrupt left pending for the ISR test\n", R2h, MFC_R2H_SYS_INIT_RET);
    if (R2h != MFC_R2H_SYS_INIT_RET)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    M->FirmwareUp = TRUE;
    return STATUS_SUCCESS;
}

//
// V-W2 boot self-test: decode mfc_test.h264 (tools/mfc/mkh264pcm.py, bit-exact I_PCM / P_Skip vector) through the
// session engine, polled, and log an FNV-1a 32 hash of each displayed luma / NV12 CbCr plane (expected values in
// notes/d10-mfc-decode-flow.md). The CLOSE_INSTANCE R2H is left pending for the ISR test.
//
static VOID
MfcDecodeTest(_In_ PMFC_DEVICE M)
{
    MFC_SESSION S;
    MFC_RESULT  R;
    PUCHAR      Stream;
    ULONG       Size;
    ULONG       Offset;
    ULONG       Steps;
    ULONG       Window;
    NTSTATUS    Status;

    // the session gets the area minus its last 1 MiB, where the file is staged and windowed into the CPB from
    MfcSessionInit(&S, M->Buffer + MFC_TEST_AREA_OFFSET, M->BufferPa.LowPart + MFC_TEST_AREA_OFFSET,
                   MFC_TEST_AREA_SIZE - (1u << 20), MFC_CODEC_OFFSET_SMALL - MFC_CPB_OFFSET);
    Stream = S.Va + MFC_TEST_AREA_SIZE - (1u << 20);
    if (!NT_SUCCESS(MfcLoadFile(MFC_TEST_PATH, Stream, S.CpbSize - 512u, &Size)))
    {
        return;   // no test stream installed: nothing to do
    }

    Wr(M, MFC_REG_RISC2HOST_CMD, 0);   // the SYS_INIT R2H (polled mode: clear before the first command)
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    MfcTrace("test: %u B stream\n", Size);
    if (!NT_SUCCESS(MfcSessionOpen(M, &S)))
    {
        return;
    }

    for (Offset = 0, Steps = 0; Steps < 16u; Steps++)
    {
        Window = Size - Offset;
        RtlMoveMemory(S.Va + MFC_CPB_OFFSET, Stream + Offset, Window);
        Status = MfcSessionStep(M, &S, Window, &R);
        if (!NT_SUCCESS(Status))
        {
            MfcTrace("test: step 0x%08X\n", Status);
            break;
        }

        if (R.Flags & MFC_DEC_FRAME)
        {
            MfcTrace("test: frame %u %ux%u type %u (%u us): luma fnv 0x%08x chroma fnv 0x%08x\n", S.Frames - 1, S.Width,
                     S.Height, R.FrameType, R.Microseconds, MfcFnv(R.Frame, S.Width, S.Height, S.Stride),
                     MfcFnv(R.Frame + S.ChromaOffset, S.Width, S.Height / 2u, S.Stride));
        }

        if (R.Flags & MFC_DEC_EOS)
        {
            break;
        }

        Offset += R.Consumed;
        if ((R.Flags & MFC_DEC_ERROR) && (R.Consumed == 0))
        {
            break;
        }
    }

    MfcSessionClose(M, &S, TRUE);
}

NTSTATUS
MfcEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Raw, _In_ WDFCMRESLIST Translated)
{
    PMFC_DEVICE                     M = MfcGetDevice(Device);
    ULONG                           Index;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    WDF_INTERRUPT_CONFIG            Config;
    NTSTATUS                        Status;

    for (Index = 0; Index < WdfCmResourceListGetCount(Translated); Index++)
    {
        Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if ((Desc->Type == CmResourceTypeMemory) && (M->Regs == NULL) && (Desc->u.Memory.Length >= MFC_WINDOW_MIN))
        {
            M->Regs = (PUCHAR)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
            M->RegsLength = Desc->u.Memory.Length;
        }
        else if ((Desc->Type == CmResourceTypeInterrupt) && (M->Interrupt == NULL))
        {
            WDF_INTERRUPT_CONFIG_INIT(&Config, MfcEvtIsr, MfcEvtDpc);
            Config.InterruptRaw        = WdfCmResourceListGetDescriptor(Raw, Index);
            Config.InterruptTranslated = Desc;
            Status = WdfInterruptCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &M->Interrupt);
            if (!NT_SUCCESS(Status))
            {
                MfcTrace("WdfInterruptCreate 0x%08X\n", Status);
                return Status;
            }
        }
    }

    if ((M->Regs == NULL) || (M->Interrupt == NULL))
    {
        MfcTrace("_CRS needs the MFC window (>= 0x10000) and one interrupt\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = MfcBoot(M);
    MfcTrace("boot 0x%08X\n", Status);
    if (NT_SUCCESS(Status))
    {
        MfcDecodeTest(M);
    }

    return Status;
}

NTSTATUS
MfcEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PMFC_DEVICE M = MfcGetDevice(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (M->Regs != NULL)
    {
        //
        // Vendor teardown, never a reset of a running core (vidc2: V-W1's RISC_ON 0 + MfcReset here raised an
        // SError, bugcheck 0x122): mfc_risc_off = wait MFC_BUS_STATUS == 0 (MFC_BW_TIMEOUT 500 ms), RISC_ON 0;
        // __mfc_deinit_hw = MFC_OFF 1. The next MfcBoot resets the stopped core.
        //
        ULONG Ms;

        for (Ms = 0; (Ms < 500) && (Rd(M, MFC_REG_MFC_BUS_STATUS) != 0); Ms++)
        {
            KeStallExecutionProcessor(1000);
        }

        MfcTrace("release: bus status 0x%x after %u ms\n", Rd(M, MFC_REG_MFC_BUS_STATUS), Ms);
        Wr(M, MFC_REG_RISC_ON, 0);
        Wr(M, MFC_REG_MFC_OFF, 1);
        MmUnmapIoSpace(M->Regs, M->RegsLength);
        M->Regs = NULL;
        if (Ms == 500)
        {
            M->BufferAlloc = NULL;   // bus never went idle: the core may still DMA into it - leak rather than free
        }
    }

    if (M->BufferAlloc != NULL)
    {
        MmFreeContiguousMemorySpecifyCache(M->BufferAlloc, M->BufferAllocSize, MmNonCached);
        M->BufferAlloc = NULL;
    }

    M->FirmwareUp = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS
MfcEvtD0EntryPostInterruptsEnabled(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PMFC_DEVICE   M = MfcGetDevice(Device);
    LARGE_INTEGER Wait;

    UNREFERENCED_PARAMETER(PreviousState);
    Wait.QuadPart = -10000LL * 100;   // 100 ms for the pending R2H interrupt to be taken
    KeDelayExecutionThread(KernelMode, FALSE, &Wait);
    MfcTrace("interrupt test: ISR ran %d time(s), last R2H cmd %u, R2H int now %u\n", M->IsrCount, M->LastR2hCmd,
             Rd(M, MFC_REG_RISC2HOST_INT));
    M->InterruptsOn = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
MfcEvtD0ExitPreInterruptsDisabled(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    MfcGetDevice(Device)->InterruptsOn = FALSE;
    return STATUS_SUCCESS;
}

BOOLEAN
MfcEvtIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageId)
{
    PMFC_DEVICE M = MfcGetDevice(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageId);
    if (Rd(M, MFC_REG_RISC2HOST_INT) == 0)
    {
        return FALSE;
    }

    M->LastR2hCmd = Rd(M, MFC_REG_RISC2HOST_CMD);
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);   // vendor mfc_clear_int (result registers stay valid until the next command)
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    InterlockedIncrement(&M->IsrCount);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
MfcEvtDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(AssociatedObject);
    KeSetEvent(&MfcGetDevice(WdfInterruptGetDevice(Interrupt))->CmdDone, IO_NO_INCREMENT, FALSE);
}

/** Copy the displayed slot to the caller as tightly packed NV12 (Width x Height luma, Width x Height/2 CbCr). **/
static VOID
MfcCopyFrame(_In_ PMFC_SESSION S, _In_ const UCHAR *Slot, _Out_writes_bytes_(S->Width * S->Height * 3 / 2) PUCHAR Out)
{
    ULONG Row;

    for (Row = 0; Row < S->Height; Row++)
    {
        RtlCopyMemory(Out + Row * S->Width, Slot + Row * S->Stride, S->Width);
    }

    Out += S->Width * S->Height;
    for (Row = 0; Row < S->Height / 2u; Row++)
    {
        RtlCopyMemory(Out + Row * S->Width, Slot + S->ChromaOffset + Row * S->Stride, S->Width);
    }
}

static NTSTATUS
MfcIoOpen(_In_ PMFC_DEVICE M, _In_ WDFREQUEST Request, _In_ WDFFILEOBJECT File)
{
    MFC_OPEN_IN *In;
    NTSTATUS     Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*In), (PVOID *)&In, NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    if ((In->Version != MFC_IOCTL_VERSION) || (In->Codec != MFC_CODEC_H264))
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (!M->FirmwareUp || (M->SessionSize == 0))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    if (M->SessionOwner != NULL)
    {
        return STATUS_DEVICE_BUSY;
    }

    MfcSessionInit(&M->Session, M->Buffer + MFC_SESSION_OFFSET, M->BufferPa.LowPart + MFC_SESSION_OFFSET,
                   M->SessionSize, MFC_CODEC_OFFSET_LARGE - MFC_CPB_OFFSET);
    Status = MfcSessionOpen(M, &M->Session);
    if (NT_SUCCESS(Status))
    {
        M->SessionOwner = File;
    }

    return Status;
}

static NTSTATUS
MfcIoDecode(_In_ PMFC_DEVICE M, _In_ WDFREQUEST Request, _In_ size_t InLength, _Out_ size_t *Information)
{
    PMFC_SESSION    S = &M->Session;
    MFC_DECODE_OUT *Out;
    PVOID           In = NULL;
    size_t          OutLength;
    MFC_RESULT      R;
    ULONG           FrameBytes;
    NTSTATUS        Status;

    *Information = 0;
    if (InLength > MFC_MAX_WINDOW)
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    if (InLength != 0)
    {
        Status = WdfRequestRetrieveInputBuffer(Request, InLength, &In, NULL);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }

    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, &OutLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // a picture cannot be held back once the firmware returned it: demand room for one before decoding
    FrameBytes = S->Ready ? (S->Width * S->Height * 3u / 2u) : 0;
    if (OutLength < sizeof(*Out) + FrameBytes)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (InLength != 0)
    {
        RtlCopyMemory(S->Va + MFC_CPB_OFFSET, In, InLength);
    }

    Status = MfcSessionStep(M, S, (ULONG)InLength, &R);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Flags          = R.Flags;
    Out->Consumed       = R.Consumed;
    Out->Width          = S->Width;
    Out->Height         = S->Height;
    Out->CropLeft       = S->CropLeft;
    Out->CropTop        = S->CropTop;
    Out->CropRight      = S->CropRight;
    Out->CropBottom     = S->CropBottom;
    Out->FrameType      = R.FrameType;
    Out->R2h            = R.R2h;
    Out->Error          = R.Error;
    Out->DisplayStatus  = R.DisplayStatus;
    Out->HwMicroseconds = R.Microseconds;
    *Information        = sizeof(*Out);
    if ((R.Flags & MFC_DEC_FRAME) && (R.Frame != NULL))
    {
        Out->FrameBytes = FrameBytes;
        MfcCopyFrame(S, R.Frame, (PUCHAR)(Out + 1));
        *Information += FrameBytes;
    }

    return STATUS_SUCCESS;
}

VOID
MfcEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                      _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PMFC_DEVICE   M    = MfcGetDevice(WdfIoQueueGetDevice(Queue));
    WDFFILEOBJECT File = WdfRequestGetFileObject(Request);
    size_t        Information = 0;
    NTSTATUS      Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    WdfWaitLockAcquire(M->SessionLock, NULL);
    switch (IoControlCode)
    {
    case IOCTL_MFC_OPEN:
        Status = MfcIoOpen(M, Request, File);
        break;
    case IOCTL_MFC_DECODE:
        Status = (M->SessionOwner == File) ? MfcIoDecode(M, Request, InputBufferLength, &Information) :
                                             STATUS_INVALID_DEVICE_STATE;
        break;
    case IOCTL_MFC_CLOSE:
        Status = STATUS_INVALID_DEVICE_STATE;
        if (M->SessionOwner == File)
        {
            MfcSessionClose(M, &M->Session, FALSE);
            M->SessionOwner = NULL;
            Status = STATUS_SUCCESS;
        }
        break;
    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfWaitLockRelease(M->SessionLock);
    WdfRequestCompleteWithInformation(Request, Status, Information);
}

VOID
MfcEvtFileCleanup(_In_ WDFFILEOBJECT FileObject)
{
    PMFC_DEVICE M = MfcGetDevice(WdfFileObjectGetDevice(FileObject));

    WdfWaitLockAcquire(M->SessionLock, NULL);
    if (M->SessionOwner == FileObject)
    {
        if (M->FirmwareUp)
        {
            MfcSessionClose(M, &M->Session, FALSE);
        }

        M->SessionOwner = NULL;
    }

    WdfWaitLockRelease(M->SessionLock);
}

NTSTATUS
MfcEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_FILEOBJECT_CONFIG        FileConfig;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDF_IO_QUEUE_CONFIG          QueueConfig;
    WDFDEVICE                    Device;
    PMFC_DEVICE                  M;
    NTSTATUS                     Status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware              = MfcEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware              = MfcEvtReleaseHardware;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = MfcEvtD0EntryPostInterruptsEnabled;
    Pnp.EvtDeviceD0ExitPreInterruptsDisabled  = MfcEvtD0ExitPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, MfcEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &FileConfig, WDF_NO_OBJECT_ATTRIBUTES);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, MFC_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    M = MfcGetDevice(Device);
    KeInitializeEvent(&M->CmdDone, NotificationEvent, FALSE);
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &M->SessionLock);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
    QueueConfig.EvtIoDeviceControl = MfcEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ExecutionLevel = WdfExecutionLevelPassive;   // IOCTLs wait for the R2H at PASSIVE_LEVEL
    Status = WdfIoQueueCreate(Device, &QueueConfig, &Attr, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_EXYNOS9820_MFC, NULL);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, MfcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
