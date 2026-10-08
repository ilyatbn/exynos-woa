#include "internal.h"
#include "controller.h"
#include "device.h"

#include "controller.tmh"

static const PBC_TRANSFER_SETTINGS TransferSettings[] =
{
    {TRUE},
    {TRUE},
    {FALSE},
    {FALSE},
    {TRUE}
};

static
FORCEINLINE
ULONG
ReadRegister32(
    _In_ PUCHAR Base,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG((PULONG)(Base + Offset));
}

static
FORCEINLINE
VOID
WriteRegister32(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value);
}

static
FORCEINLINE
VOID
SetRegisterBits(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Bits
    )
{
    WriteRegister32(Base, Offset, ReadRegister32(Base, Offset) | Bits);
}

static
FORCEINLINE
VOID
UpdateRegisterBits(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value
    )
{
    ULONG currentValue = ReadRegister32(Base, Offset);
    currentValue = (currentValue & ~Mask) | (Value & Mask);
    WriteRegister32(Base, Offset, currentValue);
}

static
VOID
ControllerReadPhaseTelemetry(
    _In_ PPBC_DEVICE Device,
    _In_ PBC_TELEMETRY_EVENT Event,
    _In_ NTSTATUS RequestStatus,
    _Out_ PPBC_PHASE_TELEMETRY Snapshot
    )
{
    Snapshot->Event = (ULONG)Event;
    Snapshot->RequestStatus = (ULONG)RequestStatus;
    Snapshot->InterruptMask = PbcDeviceGetInterruptMask(Device);
    Snapshot->InterruptStatus = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_INTERRUPT_STATUS);
    Snapshot->InterruptEnable = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_INTERRUPT_ENABLE);
    Snapshot->TransferStatus = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TRANSFER_STATUS);
    Snapshot->FifoStatus = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_FIFO_STATUS);
    Snapshot->ErrorStatus = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_ERROR_STATUS);
    Snapshot->AutoConfiguration = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_AUTO_CONFIGURATION);
    Snapshot->Control = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_CONTROL);
    Snapshot->Address = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_ADDRESS);
    Snapshot->Qch = ReadRegister32(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_QCH_USI03);
    Snapshot->GpioData = ReadRegister32(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_DATA);
}

static
VOID
ControllerCapturePhaseTelemetry(
    _In_ PPBC_DEVICE Device,
    _In_ PBC_TELEMETRY_PHASE Phase,
    _In_ PBC_TELEMETRY_EVENT Event,
    _In_ NTSTATUS RequestStatus
    )
{
    PBC_PHASE_TELEMETRY snapshot;
    LONG phaseBit;

    if ((Phase < PbcTelemetryPhaseAfterReset) ||
        (Phase >= PbcTelemetryPhaseCount) ||
        !Device->Powered ||
        (Device->Hsi2c.VirtualAddress == NULL) ||
        (Device->Cmu.VirtualAddress == NULL) ||
        (Device->Gpio.VirtualAddress == NULL))
    {
        return;
    }

    phaseBit = (LONG)(1UL << (ULONG)Phase);
    if ((InterlockedOr(&Device->TelemetryPhaseValidMask, 0) & phaseBit) != 0)
    {
        return;
    }

    ControllerReadPhaseTelemetry(Device, Event, RequestStatus, &snapshot);
    Device->TelemetryPhases[Phase] = snapshot;
    KeMemoryBarrier();
    InterlockedOr(&Device->TelemetryPhaseValidMask, phaseBit);
}

static
VOID
ControllerCaptureStaticTelemetry(
    _In_ PPBC_DEVICE Device
    )
{
    PBC_STATIC_TELEMETRY snapshot;

    if ((InterlockedOr(&Device->TelemetryStaticValid, 0) != 0) ||
        !Device->Powered ||
        (Device->Hsi2c.VirtualAddress == NULL) ||
        (Device->Cmu.VirtualAddress == NULL) ||
        (Device->Sysreg.VirtualAddress == NULL) ||
        (Device->Gpio.VirtualAddress == NULL))
    {
        return;
    }

    snapshot.TimingFs1 = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_FS1);
    snapshot.TimingFs2 = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_FS2);
    snapshot.TimingFs3 = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_FS3);
    snapshot.TimingSla = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_SLA);
    snapshot.FifoControl = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_FIFO_CONTROL);
    snapshot.Configuration = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_CONFIGURATION);
    snapshot.Timeout = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMEOUT);
    snapshot.UsiControl = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EXYNOS_USI_CONTROL);
    snapshot.UsiOption = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EXYNOS_USI_OPTION);
    snapshot.SysregConfiguration = ReadRegister32(
        Device->Sysreg.VirtualAddress,
        EXYNOS_SYSREG_USI03_CONFIGURATION);
    snapshot.GateSource = ReadRegister32(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_GATE_USI03_SOURCE);
    snapshot.GateResetSync = ReadRegister32(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_GATE_USI03_RESET_SYNC);
    snapshot.GateIpclk = ReadRegister32(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_GATE_USI03_IPCLK);
    snapshot.GatePclk = ReadRegister32(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_GATE_USI03_PCLK);
    snapshot.Qch = ReadRegister32(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_QCH_USI03);
    snapshot.GpioConfiguration = ReadRegister32(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_CONFIGURATION);
    snapshot.GpioPull = ReadRegister32(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_PULL);
    snapshot.GpioDrive = ReadRegister32(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_DRIVE);
    snapshot.GpioData = ReadRegister32(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_DATA);

    Device->TelemetryStatic = snapshot;
    KeMemoryBarrier();
    InterlockedExchange(&Device->TelemetryStaticValid, 1);
}

static
NTSTATUS
ControllerConfigurePlatform(
    _In_ PPBC_DEVICE Device
    )
{
    const ULONG gateEnable = EXYNOS_CMU_GATE_MANUAL | EXYNOS_CMU_GATE_VALUE;
    const ULONG pinMask =
        (EXYNOS_GPIO_PIN_FIELD_MASK << EXYNOS_GPIO_GPP1_SCL_SHIFT) |
        (EXYNOS_GPIO_PIN_FIELD_MASK << EXYNOS_GPIO_GPP1_SDA_SHIFT);
    const ULONG pinFunction =
        (EXYNOS_GPIO_PIN_FUNCTION_I2C << EXYNOS_GPIO_GPP1_SCL_SHIFT) |
        (EXYNOS_GPIO_PIN_FUNCTION_I2C << EXYNOS_GPIO_GPP1_SDA_SHIFT);

    SetRegisterBits(Device->Cmu.VirtualAddress, EXYNOS_CMU_GATE_USI03_SOURCE, gateEnable);
    SetRegisterBits(Device->Cmu.VirtualAddress, EXYNOS_CMU_GATE_USI03_RESET_SYNC, gateEnable);
    SetRegisterBits(Device->Cmu.VirtualAddress, EXYNOS_CMU_GATE_USI03_IPCLK, gateEnable);
    SetRegisterBits(Device->Cmu.VirtualAddress, EXYNOS_CMU_GATE_USI03_PCLK, gateEnable);
    SetRegisterBits(
        Device->Cmu.VirtualAddress,
        EXYNOS_CMU_QCH_USI03,
        EXYNOS_CMU_QCH_REQUIRED);

    WriteRegister32(
        Device->Sysreg.VirtualAddress,
        EXYNOS_SYSREG_USI03_CONFIGURATION,
        EXYNOS_SYSREG_USI_MODE_I2C);

    UpdateRegisterBits(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_CONFIGURATION,
        pinMask,
        pinFunction);
    UpdateRegisterBits(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_PULL,
        pinMask,
        0);
    UpdateRegisterBits(
        Device->Gpio.VirtualAddress,
        EXYNOS_GPIO_GPP1_DRIVE,
        pinMask,
        0);

    KeMemoryBarrier();

    if ((ReadRegister32(
            Device->Sysreg.VirtualAddress,
            EXYNOS_SYSREG_USI03_CONFIGURATION) &
         EXYNOS_SYSREG_USI_MODE_I2C) != EXYNOS_SYSREG_USI_MODE_I2C)
    {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    if ((ReadRegister32(
            Device->Gpio.VirtualAddress,
            EXYNOS_GPIO_GPP1_CONFIGURATION) &
         pinMask) != pinFunction)
    {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    if ((ReadRegister32(
            Device->Cmu.VirtualAddress,
            EXYNOS_CMU_QCH_USI03) &
         EXYNOS_CMU_QCH_REQUIRED) != EXYNOS_CMU_QCH_REQUIRED)
    {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
ControllerSetBusTiming(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG BusSpeed
    )
{
    ULONGLONG denominator;
    ULONGLONG timingCoefficient;
    ULONG divider;
    ULONG highShift;
    ULONG startShift;
    ULONG highMask;
    ULONG startMask;

    if ((BusSpeed != 100000UL) && (BusSpeed != 400000UL))
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (BusSpeed == 100000UL)
    {
        denominator = (ULONGLONG)BusSpeed * 16ULL;
        timingCoefficient = 25ULL;
    }
    else
    {
        denominator = (ULONGLONG)BusSpeed * 15ULL;
        timingCoefficient = 9ULL;
    }

    divider = (ULONG)(EXYNOS_HSI2C_SOURCE_CLOCK_HZ / denominator);
    if (divider > 0xFFUL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    highShift = (ULONG)(
        (timingCoefficient *
         (EXYNOS_HSI2C_SOURCE_CLOCK_HZ / 1000000UL)) /
        (((ULONGLONG)divider + 1ULL) * 10ULL));
    if (highShift > 7UL)
    {
        highShift = 7UL;
    }

    startShift = (highShift == 0UL) ? 0UL : highShift - 1UL;
    highMask = (0xFFFFFFFFUL >> highShift) << highShift;
    startMask = (0xFFFFFFFFUL >> startShift) << startShift;

    UpdateRegisterBits(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_FS1,
        0x00FF0000UL,
        (startMask & 0xFFUL) << 16);
    UpdateRegisterBits(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_FS2,
        0x000000FFUL,
        highMask & 0xFFUL);
    UpdateRegisterBits(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TIMING_FS3,
        0x00FF0000UL,
        (divider & 0xFFUL) << 16);

    Device->ActiveBusSpeed = BusSpeed;
    return STATUS_SUCCESS;
}

static
NTSTATUS
ControllerReset(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG BusSpeed
    )
{
    ULONG control;
    NTSTATUS status;

    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_INTERRUPT_ENABLE, 0);

    control = ReadRegister32(Device->Hsi2c.VirtualAddress, EHI2C_CONTROL);
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_CONTROL,
        control | EHI2C_CONTROL_SOFTWARE_RESET);
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_CONTROL,
        control & ~EHI2C_CONTROL_SOFTWARE_RESET);

    WriteRegister32(Device->Hsi2c.VirtualAddress, EXYNOS_USI_CONTROL, 0);

    status = ControllerSetBusTiming(Device, BusSpeed);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_CONTROL,
        EHI2C_CONTROL_MASTER);
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_TRAILING_CONTROL,
        EHI2C_TRAILING_COUNT_MAX);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_TIMEOUT, 0);
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_CONFIGURATION,
        EHI2C_CONFIGURATION_AUTO_MODE);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_FIFO_CONTROL, 0);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_AUTO_CONFIGURATION, 0);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_ADDRESS, 0);

    ControllerAcknowledgeInterrupts(Device, EHI2C_INTERRUPT_ALL);

    if (ExynosTestAnyBits(
            ReadRegister32(Device->Hsi2c.VirtualAddress, EHI2C_CONTROL),
            EHI2C_CONTROL_SOFTWARE_RESET) ||
        !ExynosTestAnyBits(
            ReadRegister32(Device->Hsi2c.VirtualAddress, EHI2C_CONFIGURATION),
            EHI2C_CONFIGURATION_AUTO_MODE))
    {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
ControllerInitialize(
    _In_ PPBC_DEVICE Device
    )
{
    NTSTATUS status;

    status = ControllerConfigurePlatform(Device);
    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_FLAG_PBCLOADING,
            "Failed to configure Exynos9820 USI03 clocks and pins - %!STATUS!",
            status);
        return status;
    }

    status = ControllerReset(Device, EXYNOS_HSI2C_DEFAULT_BUS_SPEED_HZ);
    if (!NT_SUCCESS(status))
    {
        Trace(
            TRACE_LEVEL_ERROR,
            TRACE_FLAG_PBCLOADING,
            "Failed to reset HSI2C10 - %!STATUS!",
            status);
        return status;
    }

    Device->CurrentTarget = NULL;
    Device->InterruptStatus = 0;
    PbcDeviceSetInterruptMask(Device, 0);
    Device->Powered = TRUE;

    Trace(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_PBCLOADING,
        "Exynos9820 HSI2C10 initialized at %I64x",
        Device->Hsi2c.PhysicalAddress.QuadPart);

    return STATUS_SUCCESS;
}

VOID
ControllerUninitialize(
    _In_ PPBC_DEVICE Device
    )
{
    ControllerDisableInterrupts(Device);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_AUTO_CONFIGURATION, 0);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_FIFO_CONTROL, 0);
    PbcDeviceSetInterruptMask(Device, 0);
    Device->InterruptStatus = 0;
    Device->Powered = FALSE;
}

NTSTATUS
ControllerConfigureForTransfer(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request
    )
{
    BOOLEAN firstTransfer;
    LONG transferCount;
    PPBC_TARGET target;
    ULONG autoConfiguration;
    ULONG control;
    ULONG fifoControl;
    ULONG interruptMask;
    ULONG triggerLevel;
    NTSTATUS status;

    target = Device->CurrentTarget;
    if ((target == NULL) || (target->CurrentRequest != Request))
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if ((Request->Length == 0) ||
        (Request->Length > EXYNOS_HSI2C_MAX_TRANSFER_LENGTH) ||
        (Request->Length > EHI2C_AUTO_LENGTH_MASK))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    Request->Settings = TransferSettings[Request->SequencePosition];
    Request->Status = STATUS_SUCCESS;
    Request->Information = 0;

    if ((Request->SequencePosition == SpbRequestSequencePositionSingle) ||
        (Request->SequencePosition == SpbRequestSequencePositionFirst))
    {
        status = ControllerReset(Device, target->Settings.ConnectionSpeed);
    }
    else if (Device->ActiveBusSpeed != target->Settings.ConnectionSpeed)
    {
        status = ControllerSetBusTiming(Device, target->Settings.ConnectionSpeed);
    }
    else
    {
        status = STATUS_SUCCESS;
    }

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    transferCount = InterlockedIncrement(&Device->TelemetryTransferCount);
    firstTransfer = transferCount == 1;
    if (firstTransfer)
    {
        ControllerCaptureStaticTelemetry(Device);
        ControllerCapturePhaseTelemetry(
            Device,
            PbcTelemetryPhaseAfterReset,
            PbcTelemetryTransferStarted,
            STATUS_PENDING);
    }

    ControllerDisableInterrupts(Device);
    ControllerAcknowledgeInterrupts(Device, EHI2C_INTERRUPT_ALL);

    triggerLevel = (ULONG)min(Request->Length, (size_t)EXYNOS_HSI2C_FIFO_TRIGGER);
    fifoControl =
        EHI2C_FIFO_RX_ENABLE |
        EHI2C_FIFO_TX_ENABLE |
        EHI2C_FIFO_RX_TRIGGER(triggerLevel) |
        EHI2C_FIFO_TX_TRIGGER(triggerLevel);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_FIFO_CONTROL, fifoControl);

    control = EHI2C_CONTROL_MASTER;
    autoConfiguration = (ULONG)Request->Length;
    interruptMask = EHI2C_INTERRUPT_COMPLETION_MASK;

    if (Request->Direction == SpbTransferDirectionFromDevice)
    {
        control |= EHI2C_CONTROL_RX_CHANNEL;
        autoConfiguration |= EHI2C_AUTO_READ;
        Request->DataInterrupt =
            EHI2C_INTERRUPT_RX_ALMOST_FULL | EHI2C_INTERRUPT_TRAILING;
    }
    else if (Request->Direction == SpbTransferDirectionToDevice)
    {
        control |= EHI2C_CONTROL_TX_CHANNEL;
        Request->DataInterrupt = EHI2C_INTERRUPT_TX_ALMOST_EMPTY;
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Request->Settings.IssueStop)
    {
        autoConfiguration |= EHI2C_AUTO_STOP;
    }

    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_CONTROL, control);
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_ADDRESS,
        EHI2C_ADDRESS_MASTER(target->Settings.Address));
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_AUTO_CONFIGURATION,
        autoConfiguration);

    if (firstTransfer)
    {
        ControllerCapturePhaseTelemetry(
            Device,
            PbcTelemetryPhaseProgrammed,
            PbcTelemetryTransferStarted,
            STATUS_PENDING);
    }

    if ((Request->Direction == SpbTransferDirectionFromDevice) &&
        (PbcRequestGetInfoRemaining(Request) > 0))
    {
        interruptMask |= Request->DataInterrupt;
    }

    WdfInterruptAcquireLock(Device->InterruptObject);
    Device->InterruptStatus = 0;
    PbcDeviceSetInterruptMask(Device, interruptMask);
    ControllerEnableInterrupts(Device, interruptMask);
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_AUTO_CONFIGURATION,
        autoConfiguration | EHI2C_AUTO_RUN);

    if (firstTransfer)
    {
        ControllerCapturePhaseTelemetry(
            Device,
            PbcTelemetryPhaseRunEmpty,
            PbcTelemetryTransferStarted,
            STATUS_PENDING);
    }

    if (Request->Direction == SpbTransferDirectionToDevice)
    {
        status = ControllerTransferData(Device, Request);
        if (NT_SUCCESS(status))
        {
            if (PbcRequestGetInfoRemaining(Request) > 0)
            {
                interruptMask |= Request->DataInterrupt;
            }
            else
            {
                interruptMask &= ~Request->DataInterrupt;
            }
            PbcDeviceSetInterruptMask(Device, interruptMask);
            ControllerEnableInterrupts(Device, interruptMask);
        }
    }

    if (firstTransfer)
    {
        ControllerCapturePhaseTelemetry(
            Device,
            PbcTelemetryPhaseAfterFifo,
            PbcTelemetryTransferStarted,
            status);
        ControllerCaptureTelemetry(
            Device,
            PbcTelemetryTransferStarted,
            status);
    }
    WdfInterruptReleaseLock(Device->InterruptObject);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (firstTransfer)
    {
        KeStallExecutionProcessor(10);
        WdfInterruptAcquireLock(Device->InterruptObject);
        ControllerCapturePhaseTelemetry(
            Device,
            PbcTelemetryPhaseAfter10Us,
            PbcTelemetryTransferStarted,
            STATUS_PENDING);
        WdfInterruptReleaseLock(Device->InterruptObject);

        KeStallExecutionProcessor(45);
        KeStallExecutionProcessor(45);
        WdfInterruptAcquireLock(Device->InterruptObject);
        ControllerCapturePhaseTelemetry(
            Device,
            PbcTelemetryPhaseAfter100Us,
            PbcTelemetryTransferStarted,
            STATUS_PENDING);
        WdfInterruptReleaseLock(Device->InterruptObject);

        PbcQueueTelemetry(Device);
    }

    Trace(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_TRANSFER,
        "Started %s of %Iu byte(s) to address 0x%hx, stop=%u",
        Request->Direction == SpbTransferDirectionFromDevice ? "read" : "write",
        Request->Length,
        target->Settings.Address,
        Request->Settings.IssueStop);

    return STATUS_SUCCESS;
}

NTSTATUS
ControllerTransferData(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Request->Direction == SpbTransferDirectionToDevice)
    {
        while ((PbcRequestGetInfoRemaining(Request) > 0) &&
               !ExynosTestAnyBits(
                   ReadRegister32(Device->Hsi2c.VirtualAddress, EHI2C_FIFO_STATUS),
                   EHI2C_FIFO_TX_FULL))
        {
            UCHAR byte;

            status = PbcRequestGetByte(Request, Request->Information, &byte);
            if (!NT_SUCCESS(status))
            {
                break;
            }

            WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_TX_DATA, byte);
            Request->Information++;
        }
    }
    else
    {
        while ((PbcRequestGetInfoRemaining(Request) > 0) &&
               !ExynosTestAnyBits(
                   ReadRegister32(Device->Hsi2c.VirtualAddress, EHI2C_FIFO_STATUS),
                   EHI2C_FIFO_RX_EMPTY))
        {
            UCHAR byte = (UCHAR)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_RX_DATA);

            status = PbcRequestSetByte(Request, Request->Information, byte);
            if (!NT_SUCCESS(status))
            {
                break;
            }

            Request->Information++;
        }
    }

    return status;
}

VOID
ControllerCaptureTelemetry(
    _In_ PPBC_DEVICE Device,
    _In_ PBC_TELEMETRY_EVENT Event,
    _In_ NTSTATUS RequestStatus
    )
{
    InterlockedIncrement(&Device->TelemetrySequence);
    KeMemoryBarrier();

    InterlockedExchange(
        &Device->TelemetryLastRequestStatus,
        (LONG)RequestStatus);

    if (Device->Powered && (Device->Hsi2c.VirtualAddress != NULL))
    {
        InterlockedExchange(
            &Device->TelemetryLastInterruptMask,
            (LONG)PbcDeviceGetInterruptMask(Device));
        InterlockedExchange(
            &Device->TelemetryLastInterruptStatus,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_INTERRUPT_STATUS));
        InterlockedExchange(
            &Device->TelemetryLastInterruptEnable,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_INTERRUPT_ENABLE));
        InterlockedExchange(
            &Device->TelemetryLastTransferStatus,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_TRANSFER_STATUS));
        InterlockedExchange(
            &Device->TelemetryLastFifoStatus,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_FIFO_STATUS));
        InterlockedExchange(
            &Device->TelemetryLastErrorStatus,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_ERROR_STATUS));
        InterlockedExchange(
            &Device->TelemetryLastAutoConfiguration,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_AUTO_CONFIGURATION));
        InterlockedExchange(
            &Device->TelemetryLastControl,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_CONTROL));
        InterlockedExchange(
            &Device->TelemetryLastAddress,
            (LONG)ReadRegister32(
                Device->Hsi2c.VirtualAddress,
                EHI2C_ADDRESS));
    }

    if (Device->Powered && (Device->Cmu.VirtualAddress != NULL))
    {
        InterlockedExchange(
            &Device->TelemetryLastQch,
            (LONG)ReadRegister32(
                Device->Cmu.VirtualAddress,
                EXYNOS_CMU_QCH_USI03));
    }

    KeMemoryBarrier();
    InterlockedExchange(&Device->TelemetryLastEvent, (LONG)Event);
    InterlockedIncrement(&Device->TelemetrySequence);
}

VOID
ControllerUpdateTelemetryRequestStatus(
    _In_ PPBC_DEVICE Device,
    _In_ NTSTATUS RequestStatus
    )
{
    InterlockedIncrement(&Device->TelemetrySequence);
    KeMemoryBarrier();
    InterlockedExchange(
        &Device->TelemetryLastRequestStatus,
        (LONG)RequestStatus);
    KeMemoryBarrier();
    InterlockedIncrement(&Device->TelemetrySequence);
}

VOID
ControllerCaptureFirstEventTelemetry(
    _In_ PPBC_DEVICE Device,
    _In_ PBC_TELEMETRY_EVENT Event,
    _In_ NTSTATUS RequestStatus
    )
{
    if ((InterlockedOr(&Device->TelemetryTransferCount, 0) != 1) ||
        (InterlockedCompareExchange(
            &Device->TelemetryFirstEventClaimed,
            1,
            0) != 0))
    {
        return;
    }

    ControllerCapturePhaseTelemetry(
        Device,
        PbcTelemetryPhaseFirstEvent,
        Event,
        RequestStatus);
}

VOID
ControllerProcessInterrupts(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request,
    _In_ ULONG InterruptStatus
    )
{
    NTSTATUS status;

    if (ExynosTestAnyBits(InterruptStatus, EHI2C_INTERRUPT_ERROR_MASK))
    {
        if (ExynosTestAnyBits(
                InterruptStatus,
                EHI2C_INTERRUPT_NO_DEVICE | EHI2C_INTERRUPT_NO_ACK))
        {
            Request->Status = STATUS_NO_SUCH_DEVICE;
        }
        else if (ExynosTestAnyBits(
                    InterruptStatus,
                    EHI2C_INTERRUPT_TIMEOUT))
        {
            Request->Status = STATUS_IO_TIMEOUT;
        }
        else
        {
            Request->Status = STATUS_IO_DEVICE_ERROR;
        }

        Request->Information = 0;
        ControllerAbortTransfer(Device);
        ControllerCompleteTransfer(Device, Request, TRUE);
        return;
    }

    if (ExynosTestAnyBits(
            InterruptStatus,
            Request->DataInterrupt | EHI2C_INTERRUPT_TRANSFER_DONE))
    {
        status = ControllerTransferData(Device, Request);
        if (!NT_SUCCESS(status))
        {
            Request->Status = status;
            Request->Information = 0;
            ControllerAbortTransfer(Device);
            ControllerCompleteTransfer(Device, Request, TRUE);
            return;
        }
    }

    if (PbcRequestGetInfoRemaining(Request) == 0)
    {
        PbcDeviceAndInterruptMask(Device, ~Request->DataInterrupt);
    }

    if (ExynosTestAnyBits(
            InterruptStatus,
            EHI2C_INTERRUPT_TRANSFER_DONE))
    {
        if (PbcRequestGetInfoRemaining(Request) != 0)
        {
            Request->Status = STATUS_DEVICE_DATA_ERROR;
            Request->Information = 0;
            ControllerAbortTransfer(Device);
            ControllerCompleteTransfer(Device, Request, TRUE);
            return;
        }

        ControllerCompleteTransfer(Device, Request, FALSE);
    }
}

VOID
ControllerCompleteTransfer(
    _In_ PPBC_DEVICE Device,
    _In_ PPBC_REQUEST Request,
    _In_ BOOLEAN AbortSequence
    )
{
    PPBC_TARGET target = Device->CurrentTarget;

    if (NT_SUCCESS(Request->Status))
    {
        Request->TotalInformation += Request->Information;
    }
    Request->Information = 0;

    if (!AbortSequence)
    {
        Request->TransferIndex++;
        if (Request->TransferIndex < Request->TransferCount)
        {
            Request->Status = PbcRequestConfigureForIndex(
                Request,
                Request->TransferIndex);
            if (NT_SUCCESS(Request->Status))
            {
                PbcRequestDoTransfer(Device, Request);
                return;
            }
        }
    }

    ControllerDisableInterrupts(Device);
    PbcDeviceSetInterruptMask(Device, 0);
    WdfTimerStop(Device->RequestTimer, FALSE);

    if (Request->Status != STATUS_CANCELLED)
    {
        NTSTATUS cancelStatus = WdfRequestUnmarkCancelable(Request->SpbRequest);
        if (!NT_SUCCESS(cancelStatus))
        {
            NT_ASSERT(cancelStatus == STATUS_CANCELLED);
            return;
        }
    }

    if (target != NULL)
    {
        target->CurrentRequest = NULL;
    }

    if ((Request->Type == SpbRequestTypeSequence) ||
        (Request->SequencePosition == SpbRequestSequencePositionSingle))
    {
        Device->CurrentTarget = NULL;
    }

    Request->IoComplete = TRUE;
}

VOID
ControllerAbortTransfer(
    _In_ PPBC_DEVICE Device
    )
{
    ULONG busSpeed;

    ControllerDisableInterrupts(Device);
    PbcDeviceSetInterruptMask(Device, 0);
    WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_AUTO_CONFIGURATION, 0);

    busSpeed = Device->ActiveBusSpeed;
    if (busSpeed == 0)
    {
        busSpeed = EXYNOS_HSI2C_DEFAULT_BUS_SPEED_HZ;
    }

    (VOID)ControllerReset(Device, busSpeed);
}

VOID
ControllerEnableInterrupts(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    )
{
    WriteRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_INTERRUPT_ENABLE,
        InterruptMask);
}

VOID
ControllerDisableInterrupts(
    _In_ PPBC_DEVICE Device
    )
{
    if (Device->Hsi2c.VirtualAddress != NULL)
    {
        WriteRegister32(Device->Hsi2c.VirtualAddress, EHI2C_INTERRUPT_ENABLE, 0);
    }
}

ULONG
ControllerGetInterruptStatus(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    )
{
    return ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_INTERRUPT_STATUS) & InterruptMask;
}

VOID
ControllerAcknowledgeInterrupts(
    _In_ PPBC_DEVICE Device,
    _In_ ULONG InterruptMask
    )
{
    ULONG pending = ReadRegister32(
        Device->Hsi2c.VirtualAddress,
        EHI2C_INTERRUPT_STATUS);
    pending &= InterruptMask;
    if (pending != 0)
    {
        WriteRegister32(
            Device->Hsi2c.VirtualAddress,
            EHI2C_INTERRUPT_STATUS,
            pending);
    }
}
