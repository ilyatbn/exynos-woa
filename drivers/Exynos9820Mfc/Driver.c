/*
 * Driver.c - Exynos9820Mfc V-W1 (D10, notes/d10-mfc-main.md): binds ACPI\SAMS982A (DSDT VIDC), boots the MFC v13
 * firmware and runs SYS_INIT, then proves the R2H interrupt (GSIV 370). No decode path yet (V-W2).
 *
 * Sequence = vendor __mfc_init_hw (mfc_run.c) as proven from UEFI (Beyond0LteMfcDxe R3/R4): reset (R2H/H2R/FW_VERSION
 * cleared, 1024 words from 0xF000 cleared, MFC_RESET 0x1FFF then 0), RISC_BASE_ADDRESS = buffer physical address
 * (SYSMMUs left in reset bypass), RISC_ON 1 / MFC_OFF 0, R2H FW_STATUS_RET 12, then CONTEXT_MEM_ADDR/SIZE + H2R
 * SYS_INIT, R2H SYS_INIT_RET 1. The last R2H interrupt is deliberately left pending: once WDF connects the interrupt
 * (after D0Entry) the ISR must fire - that is the interrupt-path proof.
 */
#include "Mfc.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MfcEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MfcEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE MfcEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED MfcEvtD0EntryPostInterruptsEnabled;
EVT_WDF_INTERRUPT_ISR MfcEvtIsr;
EVT_WDF_INTERRUPT_DPC MfcEvtDpc;

static __forceinline ULONG Rd(PMFC_DEVICE M, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(M->Regs + Off)); }
static __forceinline VOID Wr(PMFC_DEVICE M, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(M->Regs + Off), V); }

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

/** Poll R2H (interrupts not connected yet) for up to TimeoutMs; returns the R2H command or 0. **/
static ULONG
MfcPollR2h(_In_ PMFC_DEVICE M, _In_ ULONG TimeoutMs)
{
    ULONG Ms;

    for (Ms = 0; Ms < TimeoutMs; Ms++)
    {
        if (Rd(M, MFC_REG_RISC2HOST_INT) != 0)
        {
            return Rd(M, MFC_REG_RISC2HOST_CMD);
        }

        KeStallExecutionProcessor(1000);
    }

    return 0;
}

static NTSTATUS
MfcLoadFirmware(_In_ PMFC_DEVICE M, _Out_ PULONG Size)
{
    UNICODE_STRING            Name;
    OBJECT_ATTRIBUTES         Attr;
    IO_STATUS_BLOCK           Iosb;
    HANDLE                    File;
    FILE_STANDARD_INFORMATION Info;
    NTSTATUS                  Status;

    *Size = 0;
    RtlInitUnicodeString(&Name, MFC_FW_PATH);
    InitializeObjectAttributes(&Attr, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attr, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status))
    {
        MfcTrace("firmware open 0x%08X\n", Status);
        return Status;
    }

    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && ((Info.EndOfFile.QuadPart <= 0) || (Info.EndOfFile.QuadPart > MFC_CTX_OFFSET)))
    {
        Status = STATUS_INVALID_IMAGE_FORMAT;
    }

    if (NT_SUCCESS(Status))
    {
        *Size  = (ULONG)Info.EndOfFile.QuadPart;
        Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, M->Buffer, *Size, NULL, NULL);
    }

    ZwClose(File);
    return Status;
}

static NTSTATUS
MfcBoot(_In_ PMFC_DEVICE M)
{
    PHYSICAL_ADDRESS Low;
    PHYSICAL_ADDRESS High;
    PHYSICAL_ADDRESS Boundary;
    ULONG            FwSize;
    ULONG            R2h;
    NTSTATUS         Status;

    Low.QuadPart      = 0;
    High.QuadPart     = 0xFFFFFFFFLL;   // the MFC takes 32-bit addresses (raw daddr writes in the vendor driver)
    Boundary.QuadPart = 0;
    M->BufferAlloc = MmAllocateContiguousMemorySpecifyCache(MFC_FW_BUFFER_SIZE + MFC_BUFFER_ALIGN, Low, High, Boundary,
                                                            MmNonCached);
    if (M->BufferAlloc == NULL)
    {
        MfcTrace("no contiguous buffer below 4 GiB\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    {
        PHYSICAL_ADDRESS Pa = MmGetPhysicalAddress(M->BufferAlloc);
        ULONG Pad = (ULONG)(((Pa.QuadPart + MFC_BUFFER_ALIGN - 1) & ~(LONGLONG)(MFC_BUFFER_ALIGN - 1)) - Pa.QuadPart);

        M->Buffer            = (PUCHAR)M->BufferAlloc + Pad;
        M->BufferPa.QuadPart = Pa.QuadPart + Pad;
    }

    RtlZeroMemory(M->Buffer, MFC_FW_BUFFER_SIZE);
    Status = MfcLoadFirmware(M, &FwSize);
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
    MfcTrace("fw %u B at PA 0x%llx: R2H %u (FW_STATUS_RET %u), MFC_VERSION 0x%x, FW_VERSION 0x%08x\n", FwSize,
             M->BufferPa.QuadPart, R2h, MFC_R2H_FW_STATUS_RET, Rd(M, MFC_REG_MFC_VERSION), Rd(M, MFC_REG_FW_VERSION));
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
    return Status;
}

NTSTATUS
MfcEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST Translated)
{
    PMFC_DEVICE M = MfcGetDevice(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (M->Regs != NULL)
    {
        Wr(M, MFC_REG_RISC_ON, 0);
        MfcReset(M);
        MmUnmapIoSpace(M->Regs, M->RegsLength);
        M->Regs = NULL;
    }

    if (M->BufferAlloc != NULL)
    {
        MmFreeContiguousMemorySpecifyCache(M->BufferAlloc, MFC_FW_BUFFER_SIZE + MFC_BUFFER_ALIGN, MmNonCached);
        M->BufferAlloc = NULL;
    }

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
    Wr(M, MFC_REG_RISC2HOST_CMD, 0);   // vendor mfc_clear_int
    Wr(M, MFC_REG_RISC2HOST_INT, 0);
    InterlockedIncrement(&M->IsrCount);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
MfcEvtDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(AssociatedObject);
}

NTSTATUS
MfcEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDFDEVICE                    Device;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware              = MfcEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware              = MfcEvtReleaseHardware;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = MfcEvtD0EntryPostInterruptsEnabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, MFC_DEVICE);
    return WdfDeviceCreate(&DeviceInit, &Attr, &Device);
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, MfcEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
