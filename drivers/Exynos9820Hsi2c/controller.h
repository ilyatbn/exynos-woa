#ifndef _EXYNOS9820_HSI2C_CONTROLLER_H_
#define _EXYNOS9820_HSI2C_CONTROLLER_H_

NTSTATUS
ControllerInitialize(
    _In_ PPBC_DEVICE Device
    );

VOID
ControllerUninitialize(
    _In_ PPBC_DEVICE Device
    );

NTSTATUS
ControllerConfigureForTransfer(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request
    );

NTSTATUS
ControllerTransferData(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request
    );

VOID
ControllerCompleteTransfer(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request,
    _In_ BOOLEAN AbortSequence
    );

VOID
ControllerAbortTransfer(
    _In_ PPBC_DEVICE Device
    );

VOID
ControllerEnableInterrupts(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    );

VOID
ControllerDisableInterrupts(
    _In_ PPBC_DEVICE Device
    );

ULONG
ControllerGetInterruptStatus(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    );

VOID
ControllerAcknowledgeInterrupts(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    );

VOID
ControllerCaptureTelemetry(
    _In_ PPBC_DEVICE Device,
    _In_ PBC_TELEMETRY_EVENT Event,
    _In_ NTSTATUS RequestStatus
    );

VOID
ControllerUpdateTelemetryRequestStatus(
    _In_ PPBC_DEVICE Device,
    _In_ NTSTATUS RequestStatus
    );

VOID
ControllerCaptureFirstEventTelemetry(
    _In_ PPBC_DEVICE Device,
    _In_ PBC_TELEMETRY_EVENT Event,
    _In_ NTSTATUS RequestStatus
    );

VOID
ControllerProcessInterrupts(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request,
    _In_ ULONG InterruptStatus
    );

#endif
