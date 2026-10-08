#include "internal.h"
#include "device.h"
#include "controller.h"

#include "device.tmh"

typedef struct _PBC_TELEMETRY_SNAPSHOT
{
    ULONG Sequence;
    ULONG TransferCount;
    ULONG IsrCount;
    ULONG DpcCount;
    ULONG WatchdogCount;
    ULONG CompletionCount;
    ULONG LastEvent;
    ULONG LastRequestStatus;
    ULONG LastInterruptMask;
    ULONG LastInterruptStatus;
    ULONG LastInterruptEnable;
    ULONG LastTransferStatus;
    ULONG LastFifoStatus;
    ULONG LastErrorStatus;
    ULONG LastAutoConfiguration;
    ULONG LastControl;
    ULONG LastAddress;
    ULONG LastQch;
    ULONG StaticValid;
    ULONG PhaseValidMask;
    PBC_STATIC_TELEMETRY Static;
    PBC_PHASE_TELEMETRY Phases[PbcTelemetryPhaseCount];
} PBC_TELEMETRY_SNAPSHOT, *PPBC_TELEMETRY_SNAPSHOT;

static const PCWSTR TelemetryPhaseRegistryNames[PbcTelemetryPhaseCount] =
{
    L"I2cPhaseAfterReset",
    L"I2cPhaseProgrammed",
    L"I2cPhaseRunEmpty",
    L"I2cPhaseAfterFifo",
    L"I2cPhaseAfter10Us",
    L"I2cPhaseAfter100Us",
    L"I2cPhaseFirstEvent"
};

static const PCWSTR TelemetryPhaseLabels[PbcTelemetryPhaseCount] =
{
    L"after_reset",
    L"programmed",
    L"run_empty",
    L"after_fifo",
    L"after_10us",
    L"after_100us",
    L"first_event"
};

static
ULONG
ReadTelemetryValue(
    _In_ volatile LONG* Value
    )
{
    return (ULONG)InterlockedOr(Value, 0);
}

static
VOID
CaptureTelemetrySnapshot(
    _In_ PPBC_DEVICE Device,
    _Out_ PPBC_TELEMETRY_SNAPSHOT Snapshot
    )
{
    ULONG sequenceBefore;
    ULONG sequenceAfter = 0;

    RtlZeroMemory(Snapshot, sizeof(*Snapshot));

    do
    {
        sequenceBefore = ReadTelemetryValue(&Device->TelemetrySequence);
        if ((sequenceBefore & 1UL) != 0)
        {
            continue;
        }

        KeMemoryBarrier();
        Snapshot->LastEvent =
            ReadTelemetryValue(&Device->TelemetryLastEvent);
        Snapshot->LastRequestStatus =
            ReadTelemetryValue(&Device->TelemetryLastRequestStatus);
        Snapshot->LastInterruptMask =
            ReadTelemetryValue(&Device->TelemetryLastInterruptMask);
        Snapshot->LastInterruptStatus =
            ReadTelemetryValue(&Device->TelemetryLastInterruptStatus);
        Snapshot->LastInterruptEnable =
            ReadTelemetryValue(&Device->TelemetryLastInterruptEnable);
        Snapshot->LastTransferStatus =
            ReadTelemetryValue(&Device->TelemetryLastTransferStatus);
        Snapshot->LastFifoStatus =
            ReadTelemetryValue(&Device->TelemetryLastFifoStatus);
        Snapshot->LastErrorStatus =
            ReadTelemetryValue(&Device->TelemetryLastErrorStatus);
        Snapshot->LastAutoConfiguration =
            ReadTelemetryValue(&Device->TelemetryLastAutoConfiguration);
        Snapshot->LastControl =
            ReadTelemetryValue(&Device->TelemetryLastControl);
        Snapshot->LastAddress =
            ReadTelemetryValue(&Device->TelemetryLastAddress);
        Snapshot->LastQch =
            ReadTelemetryValue(&Device->TelemetryLastQch);
        KeMemoryBarrier();

        sequenceAfter = ReadTelemetryValue(&Device->TelemetrySequence);
    } while ((sequenceBefore != sequenceAfter) ||
             ((sequenceAfter & 1UL) != 0));

    Snapshot->Sequence = sequenceAfter;
    Snapshot->TransferCount =
        ReadTelemetryValue(&Device->TelemetryTransferCount);
    Snapshot->IsrCount =
        ReadTelemetryValue(&Device->TelemetryIsrCount);
    Snapshot->DpcCount =
        ReadTelemetryValue(&Device->TelemetryDpcCount);
    Snapshot->WatchdogCount =
        ReadTelemetryValue(&Device->TelemetryWatchdogCount);
    Snapshot->CompletionCount =
        ReadTelemetryValue(&Device->TelemetryCompletionCount);
    Snapshot->StaticValid =
        ReadTelemetryValue(&Device->TelemetryStaticValid);
    if (Snapshot->StaticValid != 0)
    {
        KeMemoryBarrier();
        Snapshot->Static = Device->TelemetryStatic;
    }

    Snapshot->PhaseValidMask =
        ReadTelemetryValue(&Device->TelemetryPhaseValidMask);
    KeMemoryBarrier();
    for (ULONG phase = 0; phase < PbcTelemetryPhaseCount; phase++)
    {
        if ((Snapshot->PhaseValidMask & (1UL << phase)) != 0)
        {
            Snapshot->Phases[phase] = Device->TelemetryPhases[phase];
        }
    }
}

static
VOID
AssignTelemetryValue(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _In_ ULONG Value,
    _Inout_ NTSTATUS* FirstFailure
    )
{
    UNICODE_STRING valueName;
    NTSTATUS status;

    RtlInitUnicodeString(&valueName, Name);
    status = WdfRegistryAssignULong(Key, &valueName, Value);
    if (!NT_SUCCESS(status) && NT_SUCCESS(*FirstFailure))
    {
        *FirstFailure = status;
    }
}

static
VOID
AssignTelemetryString(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value,
    _Inout_ NTSTATUS* FirstFailure
    )
{
    UNICODE_STRING valueName;
    UNICODE_STRING value;
    NTSTATUS status;

    RtlInitUnicodeString(&valueName, Name);
    RtlInitUnicodeString(&value, Value);
    status = WdfRegistryAssignUnicodeString(Key, &valueName, &value);
    if (!NT_SUCCESS(status) && NT_SUCCESS(*FirstFailure))
    {
        *FirstFailure = status;
    }
}

VOID
PbcQueueTelemetry(
    _In_ PPBC_DEVICE Device
    )
{
    if (Device->TelemetryWorkItem != NULL)
    {
        WdfWorkItemEnqueue(Device->TelemetryWorkItem);
    }
}

VOID
OnTelemetryWorkItem(
    _In_ WDFWORKITEM WorkItem
    )
{
    WDFDEVICE fxDevice =
        (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PPBC_DEVICE device = GetDeviceContext(fxDevice);
    PBC_TELEMETRY_SNAPSHOT snapshot;
    WDFKEY key;
    WCHAR text[384];
    NTSTATUS firstFailure = STATUS_SUCCESS;
    NTSTATUS status;

    CaptureTelemetrySnapshot(device, &snapshot);

    status = WdfDeviceOpenRegistryKey(
        fxDevice,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_WARNING,
            TRACE_FLAG_WDFLOADING,
            "Failed to open telemetry registry key - %!STATUS!",
            status);
        return;
    }

    AssignTelemetryValue(
        key,
        L"I2cTelemetryVersion",
        3,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cSequence",
        snapshot.Sequence,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cTransferCount",
        snapshot.TransferCount,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cIsrCount",
        snapshot.IsrCount,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cDpcCount",
        snapshot.DpcCount,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cWatchdogCount",
        snapshot.WatchdogCount,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cCompletionCount",
        snapshot.CompletionCount,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cLastEvent",
        snapshot.LastEvent,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cLastRequestStatus",
        snapshot.LastRequestStatus,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cInterruptMask",
        snapshot.LastInterruptMask,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cInterruptStatus",
        snapshot.LastInterruptStatus,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cInterruptEnable",
        snapshot.LastInterruptEnable,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cTransferStatus",
        snapshot.LastTransferStatus,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cFifoStatus",
        snapshot.LastFifoStatus,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cErrorStatus",
        snapshot.LastErrorStatus,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cAutoConfiguration",
        snapshot.LastAutoConfiguration,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cControl",
        snapshot.LastControl,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cAddress",
        snapshot.LastAddress,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cQch",
        snapshot.LastQch,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cStaticValid",
        snapshot.StaticValid,
        &firstFailure);
    AssignTelemetryValue(
        key,
        L"I2cPhaseValidMask",
        snapshot.PhaseValidMask,
        &firstFailure);

    if (snapshot.StaticValid != 0)
    {
        status = RtlStringCchPrintfW(
            text,
            ARRAYSIZE(text),
            L"fs=%08lX/%08lX/%08lX sla=%08lX fifo=%08lX "
            L"conf=%08lX timeout=%08lX usi=%08lX/%08lX",
            snapshot.Static.TimingFs1,
            snapshot.Static.TimingFs2,
            snapshot.Static.TimingFs3,
            snapshot.Static.TimingSla,
            snapshot.Static.FifoControl,
            snapshot.Static.Configuration,
            snapshot.Static.Timeout,
            snapshot.Static.UsiControl,
            snapshot.Static.UsiOption);
        if (NT_SUCCESS(status))
        {
            AssignTelemetryString(
                key,
                L"I2cStaticHsi2c",
                text,
                &firstFailure);
        }
        else if (NT_SUCCESS(firstFailure))
        {
            firstFailure = status;
        }

        status = RtlStringCchPrintfW(
            text,
            ARRAYSIZE(text),
            L"sys=%08lX gates=%08lX/%08lX/%08lX/%08lX qch=%08lX "
            L"gpio=%08lX/%08lX/%08lX/%08lX line_scl=%lu line_sda=%lu",
            snapshot.Static.SysregConfiguration,
            snapshot.Static.GateSource,
            snapshot.Static.GateResetSync,
            snapshot.Static.GateIpclk,
            snapshot.Static.GatePclk,
            snapshot.Static.Qch,
            snapshot.Static.GpioConfiguration,
            snapshot.Static.GpioPull,
            snapshot.Static.GpioDrive,
            snapshot.Static.GpioData,
            (snapshot.Static.GpioData >>
                EXYNOS_GPIO_GPP1_SCL_PIN) & 0x1UL,
            (snapshot.Static.GpioData >>
                EXYNOS_GPIO_GPP1_SDA_PIN) & 0x1UL);
        if (NT_SUCCESS(status))
        {
            AssignTelemetryString(
                key,
                L"I2cStaticPlatform",
                text,
                &firstFailure);
        }
        else if (NT_SUCCESS(firstFailure))
        {
            firstFailure = status;
        }
    }

    for (ULONG phase = 0; phase < PbcTelemetryPhaseCount; phase++)
    {
        PPBC_PHASE_TELEMETRY phaseSnapshot;

        if ((snapshot.PhaseValidMask & (1UL << phase)) == 0)
        {
            continue;
        }

        phaseSnapshot = &snapshot.Phases[phase];
        status = RtlStringCchPrintfW(
            text,
            ARRAYSIZE(text),
            L"phase=%ls ev=%lu status=%08lX im=%08lX is=%08lX "
            L"ie=%08lX ts=%08lX fifo=%08lX err=%08lX auto=%08lX "
            L"ctl=%08lX addr=%08lX qch=%08lX gpio=%08lX "
            L"line_scl=%lu line_sda=%lu",
            TelemetryPhaseLabels[phase],
            phaseSnapshot->Event,
            phaseSnapshot->RequestStatus,
            phaseSnapshot->InterruptMask,
            phaseSnapshot->InterruptStatus,
            phaseSnapshot->InterruptEnable,
            phaseSnapshot->TransferStatus,
            phaseSnapshot->FifoStatus,
            phaseSnapshot->ErrorStatus,
            phaseSnapshot->AutoConfiguration,
            phaseSnapshot->Control,
            phaseSnapshot->Address,
            phaseSnapshot->Qch,
            phaseSnapshot->GpioData,
            (phaseSnapshot->GpioData >>
                EXYNOS_GPIO_GPP1_SCL_PIN) & 0x1UL,
            (phaseSnapshot->GpioData >>
                EXYNOS_GPIO_GPP1_SDA_PIN) & 0x1UL);
        if (NT_SUCCESS(status))
        {
            AssignTelemetryString(
                key,
                TelemetryPhaseRegistryNames[phase],
                text,
                &firstFailure);
        }
        else if (NT_SUCCESS(firstFailure))
        {
            firstFailure = status;
        }
    }

    WdfRegistryClose(key);

    if (!NT_SUCCESS(firstFailure))
    {
        Trace(
            TRACE_LEVEL_WARNING,
            TRACE_FLAG_WDFLOADING,
            "Failed to persist complete telemetry snapshot - %!STATUS!",
            firstFailure);
    }
}

static
VOID
UnmapRegion(
    _Inout_ PPBC_MMIO_REGION Region
    )
{
    if (Region->VirtualAddress != NULL)
    {
        MmUnmapIoSpace(Region->VirtualAddress, Region->Length);
        Region->VirtualAddress = NULL;
        Region->Length = 0;
        Region->PhysicalAddress.QuadPart = 0;
    }
}

static
VOID
UnmapControllerResources(
    _Inout_ PPBC_DEVICE Device
    )
{
    UnmapRegion(&Device->Gpio);
    UnmapRegion(&Device->Sysreg);
    UnmapRegion(&Device->Cmu);
    UnmapRegion(&Device->Hsi2c);
}

static
NTSTATUS
MapMemoryResource(
    _Inout_ PPBC_DEVICE Device,
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource
    )
{
    PPBC_MMIO_REGION region;
    ULONG expectedLength;
    ULONGLONG physicalAddress = (ULONGLONG)Resource->u.Memory.Start.QuadPart;

    switch (physicalAddress)
    {
    case EXYNOS_HSI2C10_PHYSICAL_BASE:
        region = &Device->Hsi2c;
        expectedLength = EXYNOS_HSI2C_REGISTER_LENGTH;
        break;
    case EXYNOS_PERIC0_CMU_PHYSICAL_BASE:
        region = &Device->Cmu;
        expectedLength = EXYNOS_PERIC0_CMU_REGISTER_LENGTH;
        break;
    case EXYNOS_PERIC0_SYSREG_PHYSICAL_BASE:
        region = &Device->Sysreg;
        expectedLength = EXYNOS_PERIC0_SYSREG_REGISTER_LENGTH;
        break;
    case EXYNOS_PERIC0_GPIO_PHYSICAL_BASE:
        region = &Device->Gpio;
        expectedLength = EXYNOS_PERIC0_GPIO_REGISTER_LENGTH;
        break;
    default:
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if ((Resource->u.Memory.Length != expectedLength) ||
        (region->VirtualAddress != NULL))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    region->VirtualAddress = (PUCHAR)MmMapIoSpaceEx(
        Resource->u.Memory.Start,
        Resource->u.Memory.Length,
        PAGE_NOCACHE | PAGE_READWRITE);
    if (region->VirtualAddress == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    region->Length = Resource->u.Memory.Length;
    region->PhysicalAddress = Resource->u.Memory.Start;

    Trace(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_WDFLOADING,
        "Mapped resource %I64x length 0x%lx at %p",
        physicalAddress,
        region->Length,
        region->VirtualAddress);

    return STATUS_SUCCESS;
}

NTSTATUS
OnPrepareHardware(
    _In_ WDFDEVICE FxDevice,
    _In_ WDFCMRESLIST FxResourcesRaw,
    _In_ WDFCMRESLIST FxResourcesTranslated
    )
{
    PPBC_DEVICE device = GetDeviceContext(FxDevice);
    ULONG interruptCount = 0;
    ULONG memoryCount = 0;
    ULONG resourceCount;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(FxResourcesRaw);

    resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);
    for (ULONG index = 0; index < resourceCount; index++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR resource =
            WdfCmResourceListGetDescriptor(FxResourcesTranslated, index);

        if (resource == NULL)
        {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
            break;
        }

        if (resource->Type == CmResourceTypeMemory)
        {
            status = MapMemoryResource(device, resource);
            if (!NT_SUCCESS(status))
            {
                break;
            }
            memoryCount++;
        }
        else if (resource->Type == CmResourceTypeInterrupt)
        {
            interruptCount++;
        }
    }

    if (NT_SUCCESS(status) &&
        ((memoryCount != 4) ||
         (interruptCount != 1) ||
         (device->Hsi2c.VirtualAddress == NULL) ||
         (device->Cmu.VirtualAddress == NULL) ||
         (device->Sysreg.VirtualAddress == NULL) ||
         (device->Gpio.VirtualAddress == NULL)))
    {
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_FLAG_WDFLOADING,
            "Invalid HSI2C10 ACPI resources: memory=%lu interrupt=%lu - %!STATUS!",
            memoryCount,
            interruptCount,
            status);
        UnmapControllerResources(device);
    }

    return status;
}

NTSTATUS
OnReleaseHardware(
    _In_ WDFDEVICE FxDevice,
    _In_ WDFCMRESLIST FxResourcesTranslated
    )
{
    PPBC_DEVICE device = GetDeviceContext(FxDevice);

    UNREFERENCED_PARAMETER(FxResourcesTranslated);

    UnmapControllerResources(device);
    return STATUS_SUCCESS;
}

NTSTATUS
OnD0Entry(
    _In_ WDFDEVICE FxDevice,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PPBC_DEVICE device = GetDeviceContext(FxDevice);

    UNREFERENCED_PARAMETER(PreviousState);

    return ControllerInitialize(device);
}

NTSTATUS
OnD0Exit(
    _In_ WDFDEVICE FxDevice,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PPBC_DEVICE device = GetDeviceContext(FxDevice);

    UNREFERENCED_PARAMETER(TargetState);

    ControllerUninitialize(device);
    device->CurrentTarget = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
OnTargetConnect(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget
    )
{
    PPBC_DEVICE device = GetDeviceContext(SpbController);
    PPBC_TARGET target = GetTargetContext(SpbTarget);
    SPB_CONNECTION_PARAMETERS parameters;
    NTSTATUS status;

    SPB_CONNECTION_PARAMETERS_INIT(&parameters);
    SpbTargetGetConnectionParameters(SpbTarget, &parameters);

    status = PbcTargetGetSettings(
        device,
        parameters.ConnectionParameters,
        &target->Settings);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    target->SpbTarget = SpbTarget;
    target->CurrentRequest = NULL;

    Trace(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_SPBDDI,
        "Connected target address 0x%hx at %lu Hz",
        target->Settings.Address,
        target->Settings.ConnectionSpeed);

    return STATUS_SUCCESS;
}

VOID
OnControllerLock(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest
    )
{
    PPBC_DEVICE device = GetDeviceContext(SpbController);
    PPBC_TARGET target = GetTargetContext(SpbTarget);
    NTSTATUS status = STATUS_SUCCESS;

    WdfSpinLockAcquire(device->Lock);
    if ((device->CurrentTarget != NULL) || (target->CurrentRequest != NULL))
    {
        status = STATUS_DEVICE_BUSY;
    }
    else
    {
        device->CurrentTarget = target;
    }
    WdfSpinLockRelease(device->Lock);

    SpbRequestComplete(SpbRequest, status);
}

VOID
OnControllerUnlock(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest
    )
{
    PPBC_DEVICE device = GetDeviceContext(SpbController);
    PPBC_TARGET target = GetTargetContext(SpbTarget);
    NTSTATUS status = STATUS_SUCCESS;

    WdfSpinLockAcquire(device->Lock);
    if ((device->CurrentTarget != target) || (target->CurrentRequest != NULL))
    {
        status = STATUS_INVALID_DEVICE_STATE;
    }
    else
    {
        device->CurrentTarget = NULL;
    }
    WdfSpinLockRelease(device->Lock);

    SpbRequestComplete(SpbRequest, status);
}

VOID
OnRead(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest,
    _In_ size_t Length
    )
{
    PbcRequestConfigureForNonSequence(
        SpbController,
        SpbTarget,
        SpbRequest,
        Length);
}

VOID
OnWrite(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest,
    _In_ size_t Length
    )
{
    PbcRequestConfigureForNonSequence(
        SpbController,
        SpbTarget,
        SpbRequest,
        Length);
}

static
VOID
StartConfiguredRequest(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_TARGET Target,
    _In_ PPBC_REQUEST Request
    )
{
    BOOLEAN cancelOwnsRequest = FALSE;
    BOOLEAN completeRequest = FALSE;
    BOOLEAN targetClaimed = FALSE;
    NTSTATUS status;

    WdfSpinLockAcquire(Device->Lock);

    status = WdfRequestMarkCancelableEx(Request->SpbRequest, OnCancel);
    if (!NT_SUCCESS(status))
    {
        WdfSpinLockRelease(Device->Lock);
        SpbRequestComplete(Request->SpbRequest, status);
        return;
    }

    if (Request->Type == SpbRequestTypeSequence)
    {
        if (Device->CurrentTarget != NULL)
        {
            status = STATUS_DEVICE_BUSY;
        }
        else
        {
            Device->CurrentTarget = Target;
            targetClaimed = TRUE;
        }
    }
    else if (Request->SequencePosition == SpbRequestSequencePositionSingle)
    {
        if (Device->CurrentTarget != NULL)
        {
            status = STATUS_DEVICE_BUSY;
        }
        else
        {
            Device->CurrentTarget = Target;
            targetClaimed = TRUE;
        }
    }
    else if (Device->CurrentTarget != Target)
    {
        status = STATUS_INVALID_DEVICE_STATE;
    }

    if (NT_SUCCESS(status) && (Target->CurrentRequest != NULL))
    {
        status = STATUS_DEVICE_BUSY;
    }

    if (NT_SUCCESS(status))
    {
        Target->CurrentRequest = Request;
        PbcRequestDoTransfer(Device, Request);
        completeRequest = Request->IoComplete;
    }
    else
    {
        if (targetClaimed)
        {
            Device->CurrentTarget = NULL;
        }

        NTSTATUS cancelStatus =
            WdfRequestUnmarkCancelable(Request->SpbRequest);
        if (cancelStatus == STATUS_CANCELLED)
        {
            cancelOwnsRequest = TRUE;
        }
        else
        {
            NT_ASSERT(NT_SUCCESS(cancelStatus));
        }
    }

    WdfSpinLockRelease(Device->Lock);

    if (!NT_SUCCESS(status) && !cancelOwnsRequest)
    {
        SpbRequestComplete(Request->SpbRequest, status);
    }
    else if (completeRequest)
    {
        PbcRequestComplete(Request);
    }
}

VOID
OnSequence(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest,
    _In_ ULONG TransferCount
    )
{
    PPBC_DEVICE device = GetDeviceContext(SpbController);
    PPBC_TARGET target = GetTargetContext(SpbTarget);
    PPBC_REQUEST request = GetRequestContext(SpbRequest);
    SPB_REQUEST_PARAMETERS parameters;
    NTSTATUS status;

    SPB_REQUEST_PARAMETERS_INIT(&parameters);
    SpbRequestGetParameters(SpbRequest, &parameters);

    request->SpbRequest = SpbRequest;
    request->Type = parameters.Type;
    request->TransferCount = TransferCount;
    request->TransferIndex = 0;
    request->TotalInformation = 0;
    request->Status = STATUS_SUCCESS;
    request->IoComplete = FALSE;

    status = PbcRequestValidate(request);
    if (NT_SUCCESS(status))
    {
        status = PbcRequestConfigureForIndex(request, 0);
    }

    if (!NT_SUCCESS(status))
    {
        SpbRequestComplete(SpbRequest, status);
        return;
    }

    StartConfiguredRequest(device, target, request);
}

VOID
OnCancel(
    _In_ WDFREQUEST FxRequest
    )
{
    SPBREQUEST spbRequest = (SPBREQUEST)FxRequest;
    PPBC_DEVICE device = GetDeviceContext(SpbRequestGetController(spbRequest));
    PPBC_TARGET target = GetTargetContext(SpbRequestGetTarget(spbRequest));
    PPBC_REQUEST request = GetRequestContext(spbRequest);

    WdfSpinLockAcquire(device->Lock);

    WdfTimerStop(device->DelayTimer, FALSE);

    if ((device->CurrentTarget == target) &&
        (target->CurrentRequest == request))
    {
        WdfInterruptAcquireLock(device->InterruptObject);
        ControllerAbortTransfer(device);
        device->InterruptStatus = 0;
        WdfInterruptReleaseLock(device->InterruptObject);

        request->Status = STATUS_CANCELLED;
        request->Information = 0;
        ControllerCompleteTransfer(device, request, TRUE);
    }
    else
    {
        request->Status = STATUS_CANCELLED;
        request->TotalInformation = 0;
        request->IoComplete = TRUE;
    }

    WdfSpinLockRelease(device->Lock);
    PbcRequestComplete(request);
}

BOOLEAN
OnInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageId
    )
{
    PPBC_DEVICE device = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
    LONG isrCount;
    ULONG interruptMask;
    ULONG interruptStatus;

    UNREFERENCED_PARAMETER(MessageId);

    interruptMask = PbcDeviceGetInterruptMask(device);
    if (interruptMask == 0)
    {
        return FALSE;
    }

    interruptStatus = ControllerGetInterruptStatus(device, interruptMask);
    if (interruptStatus == 0)
    {
        return FALSE;
    }

    isrCount = InterlockedIncrement(&device->TelemetryIsrCount);
    if (isrCount == 1)
    {
        ControllerCaptureFirstEventTelemetry(
            device,
            PbcTelemetryInterrupt,
            STATUS_PENDING);
        ControllerCaptureTelemetry(
            device,
            PbcTelemetryInterrupt,
            STATUS_PENDING);
    }
    ControllerDisableInterrupts(device);
    ControllerAcknowledgeInterrupts(device, interruptStatus);
    InterlockedOr(&device->InterruptStatus, (LONG)interruptStatus);
    (VOID)WdfInterruptQueueDpcForIsr(Interrupt);

    return TRUE;
}

VOID
OnInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PPBC_DEVICE device = GetDeviceContext((WDFDEVICE)AssociatedObject);
    PPBC_TARGET target;
    PPBC_REQUEST request = NULL;
    BOOLEAN completeRequest = FALSE;
    ULONG interruptStatus;
    ULONG interruptMask;

    UNREFERENCED_PARAMETER(Interrupt);

    WdfSpinLockAcquire(device->Lock);

    interruptStatus = (ULONG)InterlockedExchange(&device->InterruptStatus, 0);
    if (interruptStatus != 0)
    {
        InterlockedIncrement(&device->TelemetryDpcCount);
    }
    target = device->CurrentTarget;
    if ((target == NULL) || (target->CurrentRequest == NULL))
    {
        PbcDeviceSetInterruptMask(device, 0);
        WdfSpinLockRelease(device->Lock);
        if (ReadTelemetryValue(&device->TelemetryIsrCount) == 1)
        {
            PbcQueueTelemetry(device);
        }
        return;
    }

    request = target->CurrentRequest;
    if (interruptStatus != 0)
    {
        ControllerProcessInterrupts(device, request, interruptStatus);
        completeRequest = request->IoComplete;
    }

    interruptMask = PbcDeviceGetInterruptMask(device);
    if (interruptMask != 0)
    {
        WdfInterruptAcquireLock(device->InterruptObject);
        ControllerEnableInterrupts(device, interruptMask);
        WdfInterruptReleaseLock(device->InterruptObject);
    }

    WdfSpinLockRelease(device->Lock);
    if ((interruptStatus != 0) &&
        (ReadTelemetryValue(&device->TelemetryIsrCount) == 1))
    {
        PbcQueueTelemetry(device);
    }

    if (completeRequest)
    {
        PbcRequestComplete(request);
    }
}

NTSTATUS
PbcTargetGetSettings(
    _In_ PPBC_DEVICE Device,
    _In_ PVOID ConnectionParameters,
    _Out_ PPBC_TARGET_SETTINGS Settings
    )
{
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER connection;
    PPNP_I2C_SERIAL_BUS_DESCRIPTOR descriptor;
    USHORT flags;

    UNREFERENCED_PARAMETER(Device);

    if ((ConnectionParameters == NULL) || (Settings == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    connection = (PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER)ConnectionParameters;
    if (connection->PropertiesLength < sizeof(PNP_I2C_SERIAL_BUS_DESCRIPTOR))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    descriptor =
        (PPNP_I2C_SERIAL_BUS_DESCRIPTOR)connection->ConnectionProperties;
    if (descriptor->SerialBusDescriptor.SerialBusType != I2C_SERIAL_BUS_TYPE)
    {
        return STATUS_NOT_SUPPORTED;
    }

    flags = descriptor->SerialBusDescriptor.TypeSpecificFlags;
    if (ExynosTestAnyBits(
            flags,
            I2C_SERIAL_BUS_SPECIFIC_FLAG_10BIT_ADDRESS) ||
        (descriptor->SlaveAddress == 0) ||
        (descriptor->SlaveAddress > 0x7F) ||
        ((descriptor->ConnectionSpeed != 100000UL) &&
         (descriptor->ConnectionSpeed != 400000UL)))
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Settings->AddressMode = AddressMode7Bit;
    Settings->Address = descriptor->SlaveAddress;
    Settings->ConnectionSpeed = descriptor->ConnectionSpeed;
    return STATUS_SUCCESS;
}

NTSTATUS
PbcRequestValidate(
    _In_ PPBC_REQUEST Request
    )
{
    SPB_TRANSFER_DESCRIPTOR descriptor;

    if ((Request->TransferCount == 0) ||
        (Request->TransferCount > 64))
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (ULONG index = 0; index < Request->TransferCount; index++)
    {
        SPB_TRANSFER_DESCRIPTOR_INIT(&descriptor);
        SpbRequestGetTransferParameters(
            Request->SpbRequest,
            index,
            &descriptor,
            NULL);

        if ((descriptor.TransferLength == 0) ||
            (descriptor.TransferLength > EXYNOS_HSI2C_MAX_TRANSFER_LENGTH) ||
            ((descriptor.Direction != SpbTransferDirectionToDevice) &&
             (descriptor.Direction != SpbTransferDirectionFromDevice)))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    return STATUS_SUCCESS;
}

VOID
PbcRequestConfigureForNonSequence(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest,
    _In_ size_t Length
    )
{
    PPBC_DEVICE device = GetDeviceContext(SpbController);
    PPBC_TARGET target = GetTargetContext(SpbTarget);
    PPBC_REQUEST request = GetRequestContext(SpbRequest);
    SPB_REQUEST_PARAMETERS parameters;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Length);

    SPB_REQUEST_PARAMETERS_INIT(&parameters);
    SpbRequestGetParameters(SpbRequest, &parameters);

    request->SpbRequest = SpbRequest;
    request->Type = parameters.Type;
    request->TransferCount = 1;
    request->TransferIndex = 0;
    request->SequencePosition = parameters.Position;
    request->TotalInformation = 0;
    request->Status = STATUS_SUCCESS;
    request->IoComplete = FALSE;

    status = PbcRequestValidate(request);
    if (NT_SUCCESS(status))
    {
        status = PbcRequestConfigureForIndex(request, 0);
    }

    if (!NT_SUCCESS(status))
    {
        SpbRequestComplete(SpbRequest, status);
        return;
    }

    StartConfiguredRequest(device, target, request);
}

NTSTATUS
PbcRequestConfigureForIndex(
    _Inout_ PPBC_REQUEST Request,
    _In_ ULONG Index
    )
{
    SPB_TRANSFER_DESCRIPTOR descriptor;
    PMDL mdl = NULL;

    if (Index >= Request->TransferCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SPB_TRANSFER_DESCRIPTOR_INIT(&descriptor);
    SpbRequestGetTransferParameters(
        Request->SpbRequest,
        Index,
        &descriptor,
        &mdl);
    if (mdl == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Request->MdlChain = mdl;
    Request->Length = descriptor.TransferLength;
    Request->Information = 0;
    Request->Direction = descriptor.Direction;
    Request->DelayInUs = descriptor.DelayInUs;

    if (Request->Type == SpbRequestTypeSequence)
    {
        if (Request->TransferCount == 1)
        {
            Request->SequencePosition = SpbRequestSequencePositionSingle;
        }
        else if (Index == 0)
        {
            Request->SequencePosition = SpbRequestSequencePositionFirst;
        }
        else if (Index == Request->TransferCount - 1)
        {
            Request->SequencePosition = SpbRequestSequencePositionLast;
        }
        else
        {
            Request->SequencePosition = SpbRequestSequencePositionContinue;
        }
    }

    if ((Request->SequencePosition < SpbRequestSequencePositionSingle) ||
        (Request->SequencePosition > SpbRequestSequencePositionLast))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

VOID
PbcRequestDoTransfer(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request
    )
{
    NTSTATUS status;

    if (Request->TransferIndex == 0)
    {
        (VOID)WdfTimerStart(
            Device->RequestTimer,
            WDF_REL_TIMEOUT_IN_MS(EXYNOS_HSI2C_REQUEST_TIMEOUT_MS));
    }

    if (Request->DelayInUs != 0)
    {
        (VOID)WdfTimerStart(
            Device->DelayTimer,
            WDF_REL_TIMEOUT_IN_US(Request->DelayInUs));
        return;
    }

    status = ControllerConfigureForTransfer(Device, Request);
    if (!NT_SUCCESS(status))
    {
        Request->Status = status;
        Request->Information = 0;
        ControllerAbortTransfer(Device);
        ControllerCompleteTransfer(Device, Request, TRUE);
    }
}

VOID
OnDelayTimerExpired(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE fxDevice = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PPBC_DEVICE device = GetDeviceContext(fxDevice);
    PPBC_REQUEST request = NULL;
    BOOLEAN completeRequest = FALSE;
    NTSTATUS status;

    WdfSpinLockAcquire(device->Lock);

    if ((device->CurrentTarget != NULL) &&
        (device->CurrentTarget->CurrentRequest != NULL))
    {
        request = device->CurrentTarget->CurrentRequest;
        status = ControllerConfigureForTransfer(device, request);
        if (!NT_SUCCESS(status))
        {
            request->Status = status;
            request->Information = 0;
            ControllerAbortTransfer(device);
            ControllerCompleteTransfer(device, request, TRUE);
        }
        completeRequest = request->IoComplete;
    }

    WdfSpinLockRelease(device->Lock);

    if (completeRequest)
    {
        PbcRequestComplete(request);
    }
}

VOID
OnRequestTimeout(
    _In_ WDFTIMER Timer
    )
{
    WDFDEVICE fxDevice = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PPBC_DEVICE device = GetDeviceContext(fxDevice);
    PPBC_REQUEST request = NULL;
    BOOLEAN completeRequest = FALSE;

    WdfSpinLockAcquire(device->Lock);

    if ((device->CurrentTarget != NULL) &&
        (device->CurrentTarget->CurrentRequest != NULL))
    {
        request = device->CurrentTarget->CurrentRequest;

        WdfInterruptAcquireLock(device->InterruptObject);
        InterlockedIncrement(&device->TelemetryWatchdogCount);
        ControllerCaptureFirstEventTelemetry(
            device,
            PbcTelemetryWatchdog,
            STATUS_IO_TIMEOUT);
        ControllerCaptureTelemetry(
            device,
            PbcTelemetryWatchdog,
            STATUS_IO_TIMEOUT);
        ControllerAbortTransfer(device);
        device->InterruptStatus = 0;
        WdfInterruptReleaseLock(device->InterruptObject);

        request->Status = STATUS_IO_TIMEOUT;
        request->Information = 0;
        ControllerCompleteTransfer(device, request, TRUE);
        completeRequest = request->IoComplete;
    }

    WdfSpinLockRelease(device->Lock);
    if (request != NULL)
    {
        PbcQueueTelemetry(device);
    }

    if (completeRequest)
    {
        PbcRequestComplete(request);
    }
}

VOID
PbcRequestComplete(
    _In_ PPBC_REQUEST Request
    )
{
    PPBC_DEVICE device =
        GetDeviceContext(SpbRequestGetController(Request->SpbRequest));
    LONG completionCount;

    completionCount = InterlockedIncrement(
        &device->TelemetryCompletionCount);

    if ((completionCount == 1) || !NT_SUCCESS(Request->Status))
    {
        WdfInterruptAcquireLock(device->InterruptObject);
        ControllerUpdateTelemetryRequestStatus(device, Request->Status);
        WdfInterruptReleaseLock(device->InterruptObject);
        PbcQueueTelemetry(device);
    }

    WdfRequestSetInformation(
        Request->SpbRequest,
        Request->TotalInformation);
    SpbRequestComplete(
        Request->SpbRequest,
        Request->Status);
}

static
NTSTATUS
GetMdlByteAddress(
    _In_ PPBC_REQUEST Request,
    _In_ size_t Index,
    _Out_ PUCHAR* Address
    )
{
    PMDL mdl;
    size_t currentOffset;

    if ((Index >= Request->Length) || (Address == NULL))
    {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    mdl = Request->MdlChain;
    currentOffset = Index;
    while (mdl != NULL)
    {
        size_t mdlLength = MmGetMdlByteCount(mdl);
        if (currentOffset < mdlLength)
        {
            PUCHAR buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(
                mdl,
                NormalPagePriority | MdlMappingNoExecute);
            if (buffer == NULL)
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            *Address = buffer + currentOffset;
            return STATUS_SUCCESS;
        }

        currentOffset -= mdlLength;
        mdl = mdl->Next;
    }

    return STATUS_INFO_LENGTH_MISMATCH;
}

NTSTATUS
PbcRequestGetByte(
    _In_ PPBC_REQUEST Request,
    _In_ size_t Index,
    _Out_ UCHAR* Byte
    )
{
    PUCHAR address;
    NTSTATUS status;

    if (Byte == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    status = GetMdlByteAddress(Request, Index, &address);
    if (NT_SUCCESS(status))
    {
        *Byte = *address;
    }
    return status;
}

NTSTATUS
PbcRequestSetByte(
    _In_ PPBC_REQUEST Request,
    _In_ size_t Index,
    _In_ UCHAR Byte
    )
{
    PUCHAR address;
    NTSTATUS status = GetMdlByteAddress(Request, Index, &address);

    if (NT_SUCCESS(status))
    {
        *address = Byte;
    }
    return status;
}
