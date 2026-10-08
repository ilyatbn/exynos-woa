#ifndef _EXYNOS9820_HSI2C_INTERNAL_H_
#define _EXYNOS9820_HSI2C_INTERNAL_H_

#pragma warning(push)
#pragma warning(disable:4512)
#pragma warning(disable:4480)

#define EHI2C_POOL_TAG ((ULONG)'IHEE')

#include <initguid.h>
#include <ntddk.h>
#include <wdm.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "SPBCx.h"
#include "i2ctrace.h"
#include "reshub.h"
#include "exynos9820hsi2c.h"

#include "pshpack1.h"
typedef struct _PNP_I2C_SERIAL_BUS_DESCRIPTOR {
    PNP_SERIAL_BUS_DESCRIPTOR SerialBusDescriptor;
    ULONG ConnectionSpeed;
    USHORT SlaveAddress;
} PNP_I2C_SERIAL_BUS_DESCRIPTOR, *PPNP_I2C_SERIAL_BUS_DESCRIPTOR;
#include "poppack.h"

#define I2C_SERIAL_BUS_TYPE                         0x01
#define I2C_SERIAL_BUS_SPECIFIC_FLAG_10BIT_ADDRESS  0x0001

typedef enum _ADDRESS_MODE {
    AddressMode7Bit,
    AddressMode10Bit
} ADDRESS_MODE;

typedef struct _PBC_TARGET_SETTINGS {
    ADDRESS_MODE AddressMode;
    USHORT Address;
    ULONG ConnectionSpeed;
} PBC_TARGET_SETTINGS, *PPBC_TARGET_SETTINGS;

typedef struct _PBC_TRANSFER_SETTINGS {
    BOOLEAN IssueStop;
} PBC_TRANSFER_SETTINGS, *PPBC_TRANSFER_SETTINGS;

typedef struct _PBC_DEVICE PBC_DEVICE, *PPBC_DEVICE;
typedef struct _PBC_TARGET PBC_TARGET, *PPBC_TARGET;
typedef struct _PBC_REQUEST PBC_REQUEST, *PPBC_REQUEST;

typedef struct _PBC_MMIO_REGION {
    PUCHAR VirtualAddress;
    ULONG Length;
    PHYSICAL_ADDRESS PhysicalAddress;
} PBC_MMIO_REGION, *PPBC_MMIO_REGION;

typedef enum _PBC_TELEMETRY_EVENT {
    PbcTelemetryNone = 0,
    PbcTelemetryTransferStarted = 1,
    PbcTelemetryInterrupt = 2,
    PbcTelemetryWatchdog = 3
} PBC_TELEMETRY_EVENT;

typedef enum _PBC_TELEMETRY_PHASE {
    PbcTelemetryPhaseAfterReset = 0,
    PbcTelemetryPhaseProgrammed = 1,
    PbcTelemetryPhaseRunEmpty = 2,
    PbcTelemetryPhaseAfterFifo = 3,
    PbcTelemetryPhaseAfter10Us = 4,
    PbcTelemetryPhaseAfter100Us = 5,
    PbcTelemetryPhaseFirstEvent = 6,
    PbcTelemetryPhaseCount = 7
} PBC_TELEMETRY_PHASE;

typedef struct _PBC_PHASE_TELEMETRY {
    ULONG Event;
    ULONG RequestStatus;
    ULONG InterruptMask;
    ULONG InterruptStatus;
    ULONG InterruptEnable;
    ULONG TransferStatus;
    ULONG FifoStatus;
    ULONG ErrorStatus;
    ULONG AutoConfiguration;
    ULONG Control;
    ULONG Address;
    ULONG Qch;
    ULONG GpioData;
} PBC_PHASE_TELEMETRY, *PPBC_PHASE_TELEMETRY;

typedef struct _PBC_STATIC_TELEMETRY {
    ULONG TimingFs1;
    ULONG TimingFs2;
    ULONG TimingFs3;
    ULONG TimingSla;
    ULONG FifoControl;
    ULONG Configuration;
    ULONG Timeout;
    ULONG UsiControl;
    ULONG UsiOption;
    ULONG SysregConfiguration;
    ULONG GateSource;
    ULONG GateResetSync;
    ULONG GateIpclk;
    ULONG GatePclk;
    ULONG Qch;
    ULONG GpioConfiguration;
    ULONG GpioPull;
    ULONG GpioDrive;
    ULONG GpioData;
} PBC_STATIC_TELEMETRY, *PPBC_STATIC_TELEMETRY;

struct _PBC_DEVICE {
    WDFDEVICE FxDevice;

    PBC_MMIO_REGION Hsi2c;
    PBC_MMIO_REGION Cmu;
    PBC_MMIO_REGION Sysreg;
    PBC_MMIO_REGION Gpio;

    PPBC_TARGET CurrentTarget;
    WDFINTERRUPT InterruptObject;
    volatile LONG InterruptMask;
    volatile LONG InterruptStatus;
    WDFSPINLOCK Lock;
    WDFTIMER DelayTimer;
    WDFTIMER RequestTimer;
    WDFWORKITEM TelemetryWorkItem;

    volatile LONG TelemetrySequence;
    volatile LONG TelemetryTransferCount;
    volatile LONG TelemetryIsrCount;
    volatile LONG TelemetryDpcCount;
    volatile LONG TelemetryWatchdogCount;
    volatile LONG TelemetryCompletionCount;
    volatile LONG TelemetryLastEvent;
    volatile LONG TelemetryLastRequestStatus;
    volatile LONG TelemetryLastInterruptMask;
    volatile LONG TelemetryLastInterruptStatus;
    volatile LONG TelemetryLastInterruptEnable;
    volatile LONG TelemetryLastTransferStatus;
    volatile LONG TelemetryLastFifoStatus;
    volatile LONG TelemetryLastErrorStatus;
    volatile LONG TelemetryLastAutoConfiguration;
    volatile LONG TelemetryLastControl;
    volatile LONG TelemetryLastAddress;
    volatile LONG TelemetryLastQch;
    volatile LONG TelemetryStaticValid;
    volatile LONG TelemetryPhaseValidMask;
    volatile LONG TelemetryFirstEventClaimed;
    PBC_STATIC_TELEMETRY TelemetryStatic;
    PBC_PHASE_TELEMETRY TelemetryPhases[PbcTelemetryPhaseCount];

    ULONG ActiveBusSpeed;
    BOOLEAN Powered;
};

struct _PBC_TARGET {
    SPBTARGET SpbTarget;
    PBC_TARGET_SETTINGS Settings;
    PPBC_REQUEST CurrentRequest;
};

struct _PBC_REQUEST {
    SPBREQUEST SpbRequest;
    SPB_REQUEST_TYPE Type;
    ULONG TransferCount;
    ULONG TransferIndex;
    size_t TotalInformation;
    NTSTATUS Status;
    BOOLEAN IoComplete;

    size_t Length;
    PMDL MdlChain;
    SPB_REQUEST_SEQUENCE_POSITION SequencePosition;
    PBC_TRANSFER_SETTINGS Settings;
    SPB_TRANSFER_DIRECTION Direction;
    ULONG DelayInUs;
    ULONG DataInterrupt;
    size_t Information;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PBC_DEVICE, GetDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PBC_TARGET, GetTargetContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PBC_REQUEST, GetRequestContext);

#pragma warning(pop)

#endif
