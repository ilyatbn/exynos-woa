/*
 * Driver.c - WDF/WDM plumbing: DriverEntry, device add, resources, power, battery class + WMI registration.
 *
 * Adapted from Microsoft's Windows-driver-samples simbatt/func/wdf.c (MIT License, Copyright (c) Microsoft
 * Corporation): the IRP preprocess routing to BatteryClassIoctl / BatteryClassSystemControl, the class
 * registration in SelfManagedIoInit and the WMI callbacks are the sample's. Added: the HSI2C45 MMIO resources,
 * the bus bring-up/release in D0Entry/D0Exit, and the periodic poll (timer -> passive work item).
 */

#include "S10eBattery.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD               S10eBattDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE         S10eBattPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE         S10eBattReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY                 S10eBattD0Entry;
EVT_WDF_DEVICE_D0_EXIT                  S10eBattD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT     S10eBattSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP  S10eBattSelfManagedIoCleanup;
EVT_WDF_DEVICE_QUERY_STOP               S10eBattQueryStop;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS        S10eBattIrpDeviceControl;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS        S10eBattIrpSystemControl;
EVT_WDF_TIMER                           S10eBattPollTimer;
EVT_WDF_WORKITEM                        S10eBattPollWork;
WMI_QUERY_REGINFO_CALLBACK              S10eBattQueryWmiRegInfo;
WMI_QUERY_DATABLOCK_CALLBACK            S10eBattQueryWmiDataBlock;

#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, S10eBattDeviceAdd)
#pragma alloc_text(PAGE, S10eBattPrepareHardware)
#pragma alloc_text(PAGE, S10eBattReleaseHardware)
#pragma alloc_text(PAGE, S10eBattD0Exit)
#pragma alloc_text(PAGE, S10eBattSelfManagedIoInit)
#pragma alloc_text(PAGE, S10eBattSelfManagedIoCleanup)
#pragma alloc_text(PAGE, S10eBattQueryStop)
#pragma alloc_text(PAGE, S10eBattIrpDeviceControl)
#pragma alloc_text(PAGE, S10eBattIrpSystemControl)
#pragma alloc_text(PAGE, S10eBattPollWork)
#pragma alloc_text(PAGE, S10eBattQueryWmiRegInfo)
#pragma alloc_text(PAGE, S10eBattQueryWmiDataBlock)

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG     Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PS10E_BATT_GLOBAL     Global;
    NTSTATUS              Status;

    WDF_DRIVER_CONFIG_INIT(&Config, S10eBattDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, S10E_BATT_GLOBAL);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfDriverCreate(DriverObject, RegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) {
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "WdfDriverCreate %08x\n", Status);
        return Status;
    }

    Global = S10eBattGetGlobal(WdfGetDriver());
    Global->RegistryPath.MaximumLength = RegistryPath->Length + sizeof(UNICODE_NULL);
    Global->RegistryPath.Length        = RegistryPath->Length;
    Global->RegistryPath.Buffer        = WdfDriverGetRegistryPath(WdfGetDriver());
    return STATUS_SUCCESS;
}

static NTSTATUS CreateLock(_In_ WDFDEVICE Device, _Out_ WDFWAITLOCK *Lock)
{
    WDF_OBJECT_ATTRIBUTES Attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    return WdfWaitLockCreate(&Attributes, Lock);
}

_Use_decl_annotations_
NTSTATUS S10eBattDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES        Attributes;
    WDF_TIMER_CONFIG             TimerConfig;
    WDF_WORKITEM_CONFIG          WorkConfig;
    WDFDEVICE                    Device;
    PS10E_BATT_FDO               Fdo;
    NTSTATUS                     Status;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware     = S10eBattPrepareHardware;
    Pnp.EvtDeviceReleaseHardware     = S10eBattReleaseHardware;
    Pnp.EvtDeviceD0Entry             = S10eBattD0Entry;
    Pnp.EvtDeviceD0Exit              = S10eBattD0Exit;
    Pnp.EvtDeviceSelfManagedIoInit    = S10eBattSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = S10eBattSelfManagedIoCleanup;
    Pnp.EvtDeviceQueryStop           = S10eBattQueryStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, S10eBattIrpDeviceControl,
                                                         IRP_MJ_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, S10eBattIrpSystemControl,
                                                         IRP_MJ_SYSTEM_CONTROL, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, S10E_BATT_FDO);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "WdfDeviceCreate %08x\n", Status);
        return Status;
    }

    Fdo = S10eBattGetFdo(Device);
    Fdo->Device      = Device;
    Fdo->ClassHandle = NULL;
    S10eBattInitState(Fdo);

    Status = CreateLock(Device, &Fdo->ClassInitLock);
    if (NT_SUCCESS(Status)) {
        Status = CreateLock(Device, &Fdo->StateLock);
    }
    if (NT_SUCCESS(Status)) {
        Status = CreateLock(Device, &Fdo->BusLock);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* Poll: periodic DISPATCH_LEVEL timer that only queues a PASSIVE_LEVEL work item (the I2C transfers and the
       wait locks need PASSIVE). No automatic serialization: the locks do it. */
    WDF_WORKITEM_CONFIG_INIT(&WorkConfig, S10eBattPollWork);
    WorkConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &Fdo->PollWork);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_TIMER_CONFIG_INIT_PERIODIC(&TimerConfig, S10eBattPollTimer, S10E_BATT_POLL_MS);
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject   = Device;
    Attributes.ExecutionLevel = WdfExecutionLevelDispatch;
    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Fdo->PollTimer);
    return Status;
}

/* The memory resources must be exactly the three VERIFIED windows HSI2C45 / CMGP USI page / CMGP GPIO page, each
   once (matched by base, so a reordered resource list still works). Anything else means the DSDT and this driver
   disagree: refuse rather than poke an unknown block. */
_Use_decl_annotations_
NTSTATUS S10eBattPrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
    static const struct { ULONGLONG Base; ULONG Size; } Expected[3] = {
        { S10E_HSI2C45_BASE,       S10E_HSI2C45_SIZE },
        { S10E_CMGP_USI_PAGE_BASE, S10E_CMGP_USI_PAGE_SIZE },
        { S10E_CMGP_GPIO_BASE,     S10E_CMGP_GPIO_SIZE },
    };
    PS10E_BATT_FDO                  Fdo = S10eBattGetFdo(Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    volatile UCHAR                 *Map[3] = { NULL, NULL, NULL };
    ULONG                           Count;
    ULONG                           i;
    ULONG                           k;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count; i++) {
        Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if ((Desc == NULL) || (Desc->Type != CmResourceTypeMemory)) {
            continue;
        }

        for (k = 0; k < ARRAYSIZE(Expected); k++) {
            if ((ULONGLONG)Desc->u.Memory.Start.QuadPart == Expected[k].Base) {
                break;
            }
        }

        if ((k == ARRAYSIZE(Expected)) || (Map[k] != NULL) || (Desc->u.Memory.Length != Expected[k].Size)) {
            S10eBattPrint(DPFLTR_ERROR_LEVEL, "unexpected memory resource %I64x+%x\n",
                          Desc->u.Memory.Start.QuadPart, Desc->u.Memory.Length);
            goto Fail;
        }

        Map[k] = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                  PAGE_READWRITE | PAGE_NOCACHE);
        if (Map[k] == NULL) {
            goto Fail;
        }
    }

    for (k = 0; k < ARRAYSIZE(Expected); k++) {
        if (Map[k] == NULL) {
            S10eBattPrint(DPFLTR_ERROR_LEVEL, "memory resource %I64x missing\n", Expected[k].Base);
            goto Fail;
        }
    }

    Fdo->I2c      = Map[0];
    Fdo->UsiPage  = Map[1];
    Fdo->GpioPage = Map[2];
    return STATUS_SUCCESS;

Fail:
    for (i = 0; i < ARRAYSIZE(Map); i++) {
        if (Map[i] != NULL) {
            MmUnmapIoSpace((PVOID)Map[i], Expected[i].Size);
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

_Use_decl_annotations_
NTSTATUS S10eBattReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    if (Fdo->I2c != NULL)      { MmUnmapIoSpace((PVOID)Fdo->I2c, S10E_HSI2C45_SIZE); }
    if (Fdo->UsiPage != NULL)  { MmUnmapIoSpace((PVOID)Fdo->UsiPage, S10E_CMGP_USI_PAGE_SIZE); }
    if (Fdo->GpioPage != NULL) { MmUnmapIoSpace((PVOID)Fdo->GpioPage, S10E_CMGP_GPIO_SIZE); }
    Fdo->I2c = Fdo->UsiPage = Fdo->GpioPage = NULL;
    return STATUS_SUCCESS;
}

/* D0Entry runs at PASSIVE_LEVEL here (the driver object is created with WdfExecutionLevelPassive and the device
   is not in the paging path). Bus up, one reading now, then the periodic poll. */
_Use_decl_annotations_
NTSTATUS S10eBattD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    WdfWaitLockAcquire(Fdo->BusLock, NULL);
    S10eHsi2cBusInit(Fdo);
    WdfWaitLockRelease(Fdo->BusLock);

    S10eBattPoll(Device);
    WdfTimerStart(Fdo->PollTimer, WDF_REL_TIMEOUT_IN_MS(S10E_BATT_POLL_MS));
    return STATUS_SUCCESS;      /* a dead gauge is not a device failure: QueryTag reports no battery */
}

_Use_decl_annotations_
NTSTATUS S10eBattD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(Device);

    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();

    WdfTimerStop(Fdo->PollTimer, TRUE);
    WdfWorkItemFlush(Fdo->PollWork);

    WdfWaitLockAcquire(Fdo->BusLock, NULL);
    S10eHsi2cBusRelease(Fdo);
    WdfWaitLockRelease(Fdo->BusLock);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID S10eBattPollTimer(WDFTIMER Timer)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(WdfTimerGetParentObject(Timer));

    WdfWorkItemEnqueue(Fdo->PollWork);     /* no-op if already queued */
}

_Use_decl_annotations_
VOID S10eBattPollWork(WDFWORKITEM WorkItem)
{
    PAGED_CODE();
    S10eBattPoll((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
}

_Use_decl_annotations_
NTSTATUS S10eBattSelfManagedIoInit(WDFDEVICE Device)
{
    BATTERY_MINIPORT_INFO_V1_1 Init;
    PS10E_BATT_FDO             Fdo = S10eBattGetFdo(Device);
    NTSTATUS                   Status;

    PAGED_CODE();

    RtlZeroMemory(&Init, sizeof(Init));
    Init.MajorVersion        = BATTERY_CLASS_MAJOR_VERSION;
    Init.MinorVersion        = BATTERY_CLASS_MINOR_VERSION_1;
    Init.Context             = Fdo;
    Init.QueryTag            = S10eBattQueryTag;
    Init.QueryInformation    = S10eBattQueryInformation;
    Init.SetInformation      = S10eBattSetInformation;
    Init.QueryStatus         = S10eBattQueryStatus;
    Init.SetStatusNotify     = S10eBattSetStatusNotify;
    Init.DisableStatusNotify = S10eBattDisableStatusNotify;
    Init.Pdo                 = WdfDeviceWdmGetPhysicalDevice(Device);
    Init.DeviceName          = NULL;
    Init.Fdo                 = WdfDeviceWdmGetDeviceObject(Device);

    WdfWaitLockAcquire(Fdo->ClassInitLock, NULL);
    Status = BatteryClassInitializeDevice((PBATTERY_MINIPORT_INFO)&Init, &Fdo->ClassHandle);
    WdfWaitLockRelease(Fdo->ClassInitLock);
    if (!NT_SUCCESS(Status)) {
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "BatteryClassInitializeDevice %08x\n", Status);
        return Status;
    }

    Fdo->WmiLibContext.GuidCount         = 0;
    Fdo->WmiLibContext.GuidList          = NULL;
    Fdo->WmiLibContext.QueryWmiRegInfo   = S10eBattQueryWmiRegInfo;
    Fdo->WmiLibContext.QueryWmiDataBlock = S10eBattQueryWmiDataBlock;
    Fdo->WmiLibContext.SetWmiDataBlock   = NULL;
    Fdo->WmiLibContext.SetWmiDataItem    = NULL;
    Fdo->WmiLibContext.ExecuteWmiMethod  = NULL;
    Fdo->WmiLibContext.WmiFunctionControl = NULL;
    Status = IoWMIRegistrationControl(WdfDeviceWdmGetDeviceObject(Device), WMIREG_ACTION_REGISTER);
    if (!NT_SUCCESS(Status)) {
        S10eBattPrint(DPFLTR_WARNING_LEVEL, "IoWMIRegistrationControl %08x (non-fatal)\n", Status);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID S10eBattSelfManagedIoCleanup(WDFDEVICE Device)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(Device);

    PAGED_CODE();

    (VOID)IoWMIRegistrationControl(WdfDeviceWdmGetDeviceObject(Device), WMIREG_ACTION_DEREGISTER);

    WdfWaitLockAcquire(Fdo->ClassInitLock, NULL);
    if (Fdo->ClassHandle != NULL) {
        (VOID)BatteryClassUnload(Fdo->ClassHandle);
        Fdo->ClassHandle = NULL;
    }
    WdfWaitLockRelease(Fdo->ClassInitLock);
}

/* Fail resource rebalance, as simbatt does: our three windows are fixed MMIO (nothing to rebalance), and a stop
   would leave class IOCTLs pended while the device is out of D0. */
_Use_decl_annotations_
NTSTATUS S10eBattQueryStop(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    PAGED_CODE();
    return STATUS_UNSUCCESSFUL;
}

/* simbatt: hand the IRP to the battery class first; whatever it does not own goes back to WDF (no queue here,
   so WDF completes it with STATUS_INVALID_DEVICE_REQUEST). */
/* G18 H1b: IOCTL_S10E_USBC_READ - Length (1..128) bytes of the max77705 USB-C block from Reg, in FIFO-sized chunks,
   under BusLock; STATUS_DEVICE_NOT_READY outside D0 (bus released). */
static NTSTATUS S10eUsbcReadIoctl(_Inout_ PS10E_BATT_FDO Fdo, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);
    S10E_USBC_READ_IN  In;
    UCHAR              Data[S10E_USBC_READ_MAX];
    ULONG              Done;
    ULONG              Chunk;
    NTSTATUS           Status = STATUS_SUCCESS;

    PAGED_CODE();
    Irp->IoStatus.Information = 0;
    if (Sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(In) || Irp->AssociatedIrp.SystemBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    In = *(S10E_USBC_READ_IN *)Irp->AssociatedIrp.SystemBuffer;
    if (In.Length == 0 || In.Length > S10E_USBC_READ_MAX || (ULONG)In.Reg + In.Length > 0x100 ||
        Sp->Parameters.DeviceIoControl.OutputBufferLength < In.Length) {
        return STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockAcquire(Fdo->BusLock, NULL);
    if (!Fdo->BusReady) {
        Status = STATUS_DEVICE_NOT_READY;
    }
    for (Done = 0; NT_SUCCESS(Status) && Done < In.Length; Done += Chunk) {
        Chunk  = min(In.Length - Done, HSI2C_FIFO_DEPTH);
        Status = S10eMax77705ReadN(Fdo, MAX77705_USBC_ADDR, (UCHAR)(In.Reg + Done), &Data[Done], Chunk);
    }
    WdfWaitLockRelease(Fdo->BusLock);
    if (NT_SUCCESS(Status)) {
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Data, In.Length);
        Irp->IoStatus.Information = In.Length;
    }
    return Status;
}

_Use_decl_annotations_
NTSTATUS S10eBattIrpDeviceControl(WDFDEVICE Device, PIRP Irp)
{
    PS10E_BATT_FDO Fdo = S10eBattGetFdo(Device);
    NTSTATUS       Status = STATUS_NOT_SUPPORTED;

    PAGED_CODE();

    if (IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode == IOCTL_S10E_USBC_READ) {
        Status = S10eUsbcReadIoctl(Fdo, Irp);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    #pragma warning(suppress: 28118)
    WdfWaitLockAcquire(Fdo->ClassInitLock, NULL);
    if (Fdo->ClassHandle != NULL) {
        #pragma warning(suppress: 28118)
        Status = BatteryClassIoctl(Fdo->ClassHandle, Irp);
    }
    WdfWaitLockRelease(Fdo->ClassInitLock);

    if (Status == STATUS_NOT_SUPPORTED) {
        IoSkipCurrentIrpStackLocation(Irp);
        Status = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS S10eBattIrpSystemControl(WDFDEVICE Device, PIRP Irp)
{
    PS10E_BATT_FDO          Fdo = S10eBattGetFdo(Device);
    SYSCTL_IRP_DISPOSITION  Disposition = IrpForward;
    NTSTATUS                Status = STATUS_NOT_IMPLEMENTED;

    PAGED_CODE();

    #pragma warning(suppress: 28118)
    WdfWaitLockAcquire(Fdo->ClassInitLock, NULL);
    if (Fdo->ClassHandle != NULL) {
        Status = BatteryClassSystemControl(Fdo->ClassHandle, &Fdo->WmiLibContext,
                                           WdfDeviceWdmGetDeviceObject(Device), Irp, &Disposition);
    }
    WdfWaitLockRelease(Fdo->ClassInitLock);

    switch (Disposition) {
    case IrpProcessed:
        break;
    case IrpNotCompleted:
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    case IrpForward:
    case IrpNotWmi:
    default:
        IoSkipCurrentIrpStackLocation(Irp);
        Status = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
        break;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS S10eBattQueryWmiRegInfo(PDEVICE_OBJECT DeviceObject, PULONG RegFlags, PUNICODE_STRING InstanceName,
                                 PUNICODE_STRING *RegistryPath, PUNICODE_STRING MofResourceName,
                                 PDEVICE_OBJECT *Pdo)
{
    WDFDEVICE Device = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);

    UNREFERENCED_PARAMETER(InstanceName);
    UNREFERENCED_PARAMETER(MofResourceName);
    PAGED_CODE();

    *RegFlags     = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &S10eBattGetGlobal(WdfGetDriver())->RegistryPath;
    *Pdo          = WdfDeviceWdmGetPhysicalDevice(Device);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS S10eBattQueryWmiDataBlock(PDEVICE_OBJECT DeviceObject, PIRP Irp, ULONG GuidIndex, ULONG InstanceIndex,
                                   ULONG InstanceCount, PULONG InstanceLengthArray, ULONG BufferAvail,
                                   PUCHAR Buffer)
{
    PS10E_BATT_FDO Fdo;
    NTSTATUS       Status;

    UNREFERENCED_PARAMETER(InstanceIndex);
    UNREFERENCED_PARAMETER(InstanceCount);
    PAGED_CODE();

    if (InstanceLengthArray == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    Fdo = S10eBattGetFdo(WdfWdmDeviceGetWdfDeviceHandle(DeviceObject));
    Status = BatteryClassQueryWmiDataBlock(Fdo->ClassHandle, DeviceObject, Irp, GuidIndex, InstanceLengthArray,
                                           BufferAvail, Buffer);
    if (Status == STATUS_WMI_GUID_NOT_FOUND) {
        Status = WmiCompleteRequest(DeviceObject, Irp, STATUS_WMI_GUID_NOT_FOUND, 0, IO_NO_INCREMENT);
    }

    return Status;
}
