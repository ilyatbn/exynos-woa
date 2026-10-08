#ifndef _EXYNOS9820_HSI2C_DEVICE_H_
#define _EXYNOS9820_HSI2C_DEVICE_H_

EVT_WDF_DEVICE_PREPARE_HARDWARE OnPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE OnReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY OnD0Entry;
EVT_WDF_DEVICE_D0_EXIT OnD0Exit;

EVT_WDF_INTERRUPT_ISR OnInterruptIsr;
EVT_WDF_INTERRUPT_DPC OnInterruptDpc;
EVT_WDF_REQUEST_CANCEL OnCancel;
EVT_WDF_TIMER OnDelayTimerExpired;
EVT_WDF_TIMER OnRequestTimeout;
EVT_WDF_WORKITEM OnTelemetryWorkItem;

EVT_SPB_TARGET_CONNECT OnTargetConnect;
EVT_SPB_CONTROLLER_LOCK OnControllerLock;
EVT_SPB_CONTROLLER_UNLOCK OnControllerUnlock;
EVT_SPB_CONTROLLER_READ OnRead;
EVT_SPB_CONTROLLER_WRITE OnWrite;
EVT_SPB_CONTROLLER_SEQUENCE OnSequence;

NTSTATUS
PbcTargetGetSettings(
    _In_ PPBC_DEVICE Device,
    _In_ PVOID ConnectionParameters,
    _Out_ PPBC_TARGET_SETTINGS Settings
    );

NTSTATUS
PbcRequestValidate(
    _In_ PPBC_REQUEST Request
    );

VOID
PbcRequestConfigureForNonSequence(
    _In_ WDFDEVICE SpbController,
    _In_ SPBTARGET SpbTarget,
    _In_ SPBREQUEST SpbRequest,
    _In_ size_t Length
    );

NTSTATUS
PbcRequestConfigureForIndex(
    _Inout_ PPBC_REQUEST Request,
    _In_ ULONG Index
    );

VOID
PbcRequestDoTransfer(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request
    );

VOID
PbcRequestComplete(
    _In_ PPBC_REQUEST Request
    );

VOID
PbcQueueTelemetry(
    _In_ PPBC_DEVICE Device
    );

FORCEINLINE
ULONG
PbcDeviceGetInterruptMask(
    _In_ PPBC_DEVICE Device
    )
{
    return (ULONG)InterlockedOr(&Device->InterruptMask, 0);
}

FORCEINLINE
VOID
PbcDeviceSetInterruptMask(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    )
{
    InterlockedExchange(&Device->InterruptMask, (LONG)InterruptMask);
}

FORCEINLINE
VOID
PbcDeviceAndInterruptMask(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    )
{
    InterlockedAnd(&Device->InterruptMask, (LONG)InterruptMask);
}

FORCEINLINE
size_t
PbcRequestGetInfoRemaining(
    _In_ PPBC_REQUEST Request
    )
{
    return Request->Length - Request->Information;
}

NTSTATUS
PbcRequestGetByte(
    _In_ PPBC_REQUEST Request,
    _In_ size_t Index,
    _Out_ UCHAR* Byte
    );

NTSTATUS
PbcRequestSetByte(
    _In_ PPBC_REQUEST Request,
    _In_ size_t Index,
    _In_ UCHAR Byte
    );

#endif
