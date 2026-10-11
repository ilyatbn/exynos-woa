//
// Exynos9820Gpio.c — GpioClx provider (GPIO controller driver) for the
// Exynos 9820 "ALIVE" pinctrl block at 0x15850000 (ACPI _HID SAMS9821).
//
// Structure follows the official Microsoft SimGpio sample
// (microsoft/Windows-driver-samples, gpio/samples/simgpio — MIT license
// there; this file is an independent implementation against the documented
// GpioClx DDI for the Exynos register set). The register map is VERIFIED
// from the vendor Linux pinctrl driver and the shipped DTB — see
// Exynos9820Gpio.h and notes/d8-audio.md §3. The pseudo-bank model (34
// banks of 1 pin, because GpioClx assigns _CRS interrupts to banks while
// Exynos EINTs are per-pin) is explained in Exynos9820Gpio.h.
//
// Purpose (D8): the ACPI0011 button device in the DSDT (power / volume
// up / volume down / Bixby) is serviced by the INBOX Hidinterrupt.sys,
// which consumes GpioInt resources — those require a GpioClx provider for
// this on-SoC controller. The same provider later serves the CS47L93 codec
// IRQ (gpa2-2) and CS35L40 amp IRQs for the audio tasks.
//
// STATUS: code written against the documented DDI; NOT built (no WDK on
// this machine), NOT loaded, NOT device-validated. The EINT_CON/FLTCON/
// MASK/PEND semantics are VERIFIED from the vendor driver; the GpioClx
// ActiveBoth-emulation contract (level + dynamic polarity) is what the
// DDI docs state but is unexercised here.
//

#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>

#include "Exynos9820Gpio.h"

//
// Forward declarations — the GpioClx callback contract.
//

EVT_WDF_DRIVER_DEVICE_ADD                       ExynosGpioEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD                           ExynosGpioEvtDriverUnload;

GPIO_CLIENT_PREPARE_CONTROLLER                  ExynosGpioPrepareController;
GPIO_CLIENT_RELEASE_CONTROLLER                  ExynosGpioReleaseController;
GPIO_CLIENT_START_CONTROLLER                    ExynosGpioStartController;
GPIO_CLIENT_STOP_CONTROLLER                     ExynosGpioStopController;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION  ExynosGpioQueryBasicInformation;
GPIO_CLIENT_ENABLE_INTERRUPT                    ExynosGpioEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT                   ExynosGpioDisableInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS                     ExynosGpioMaskInterrupts;
GPIO_CLIENT_UNMASK_INTERRUPT                    ExynosGpioUnmaskInterrupt;
GPIO_CLIENT_RECONFIGURE_INTERRUPT               ExynosGpioReconfigureInterrupt;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS             ExynosGpioQueryActiveInterrupts;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS             ExynosGpioClearActiveInterrupts;
GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS            ExynosGpioQueryEnabledInterrupts;
GPIO_CLIENT_CONNECT_IO_PINS                     ExynosGpioConnectIoPins;
GPIO_CLIENT_DISCONNECT_IO_PINS                  ExynosGpioDisconnectIoPins;
GPIO_CLIENT_READ_PINS_MASK                      ExynosGpioReadPins;
GPIO_CLIENT_WRITE_PINS_MASK                     ExynosGpioWritePins;

//
// ---------------------------------------------------------------------------
// Register helpers. Pseudo-bank B == physical pin B:
//   physBank = B / 8, bit = B % 8.
// Callers hold ctx->HwLock for RMWs; plain reads outside RMWs are single
// 32-bit accesses and safe.
// ---------------------------------------------------------------------------
//

static
BOOLEAN
ExynosGpioPinValid (
    _In_ BANK_ID BankId
    )
{
    // With the 1-pin pseudo-bank model, BankId is the global pin number.
    // gpa4 has only 2 pins, so 34 total.
    //
    return (BankId < EXYNOS_GPIO_TOTAL_PINS) ? TRUE : FALSE;
}

static
ULONG
ExynosGpioRead (
    _In_ PEXYNOS_GPIO_CONTEXT Context,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG((PULONG)(Context->Registers + Offset));
}

static
VOID
ExynosGpioWrite (
    _In_ PEXYNOS_GPIO_CONTEXT Context,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG((PULONG)(Context->Registers + Offset), Value);
}

static
ULONG
ExynosGpioWord (
    _In_ ULONG PhysBankBase,   // one of ExynosGpioPhysBankBase[]
    _In_ ULONG BlockBase       // EXYNOS_BANK_* or EXYNOS_EINT_*_BASE
    )
{
    return PhysBankBase + BlockBase;
}

static
VOID
ExynosGpioRmwField (
    _In_ PEXYNOS_GPIO_CONTEXT Context,
    _In_ ULONG Word,
    _In_ ULONG Shift,
    _In_ ULONG Mask,
    _In_ ULONG Value
    )
{
    ULONG v = ExynosGpioRead(Context, Word);

    v = (v & ~(Mask << Shift)) | ((Value & Mask) << Shift);
    ExynosGpioWrite(Context, Word, v);
}

static
ULONG
ExynosGpioRdField (
    _In_ PEXYNOS_GPIO_CONTEXT Context,
    _In_ ULONG Word,
    _In_ ULONG Shift,
    _In_ ULONG Mask
    )
{
    return (ExynosGpioRead(Context, Word) >> Shift) & Mask;
}

//
// Map an ACPI/GpioClx pull request onto the Exynos PUD field.
// VERIFIED encodings: 0 none, 1 down, 2 up (vendor pinctrl bindings);
// the keys have external pull-ups and pud=0 in the DT, so PullNone is
// the faithful default and PullUp is available for consumers that ask.
//
static
ULONG
ExynosGpioPudValue (
    _In_ GPIO_PULL_CONFIGURATION Pull
    )
{
    switch (Pull) {
    case InterruptPullDown:
        return 1;
    case InterruptPullUp:
        return 2;
    case InterruptPullNone:
    default:
        return 0;
    }
}

//
// Map (InterruptMode, Polarity) onto the 4-bit EINT_CON trigger type.
// VERIFIED: EXYNOS_EINT_LEVEL_LOW=0 .. EDGE_BOTH=4 (pinctrl-exynos.h).
// With EmulateActiveBoth = TRUE, GpioClx never presents ActiveBoth here
// (same assertion the SimGpio sample makes).
//
static
NTSTATUS
ExynosGpioTriggerType (
    _In_ GPIO_INTERRUPT_MODE Mode,
    _In_ GPIO_INTERRUPT_POLARITY Polarity,
    _Out_ PULONG Type
    )
{
    if (Mode == LevelSensitive) {
        *Type = (Polarity == InterruptActiveHigh) ? EXYNOS_EINT_TYPE_LEVEL_HIGH
                                                  : EXYNOS_EINT_TYPE_LEVEL_LOW;
        return STATUS_SUCCESS;
    }
    if (Polarity == InterruptActiveBoth) {
        *Type = EXYNOS_EINT_TYPE_EDGE_BOTH;
        return STATUS_SUCCESS;
    }
    *Type = (Polarity == InterruptActiveHigh) ? EXYNOS_EINT_TYPE_EDGE_RISE
                                              : EXYNOS_EINT_TYPE_EDGE_FALL;
    return STATUS_SUCCESS;
}

//
// ---------------------------------------------------------------------------
// WDF entry points
// ---------------------------------------------------------------------------
//

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    GPIO_CLIENT_REGISTRATION_PACKET packet;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, ExynosGpioEvtDeviceAdd);
    config.EvtDriverUnload = ExynosGpioEvtDriverUnload;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&packet, sizeof(packet));
    packet.Version = GPIO_CLIENT_VERSION;
    packet.Size = sizeof(packet);
    packet.ControllerContextSize = sizeof(EXYNOS_GPIO_CONTEXT);

    packet.CLIENT_PrepareController = ExynosGpioPrepareController;
    packet.CLIENT_ReleaseController = ExynosGpioReleaseController;
    packet.CLIENT_StartController = ExynosGpioStartController;
    packet.CLIENT_StopController = ExynosGpioStopController;
    packet.CLIENT_QueryControllerBasicInformation = ExynosGpioQueryBasicInformation;

    packet.CLIENT_EnableInterrupt = ExynosGpioEnableInterrupt;
    packet.CLIENT_DisableInterrupt = ExynosGpioDisableInterrupt;
    packet.CLIENT_MaskInterrupts = ExynosGpioMaskInterrupts;
    packet.CLIENT_UnmaskInterrupt = ExynosGpioUnmaskInterrupt;
    packet.CLIENT_ReconfigureInterrupt = ExynosGpioReconfigureInterrupt;

    packet.CLIENT_QueryActiveInterrupts = ExynosGpioQueryActiveInterrupts;
    packet.CLIENT_ClearActiveInterrupts = ExynosGpioClearActiveInterrupts;
    packet.CLIENT_QueryEnabledInterrupts = ExynosGpioQueryEnabledInterrupts;

    packet.CLIENT_ConnectIoPins = ExynosGpioConnectIoPins;
    packet.CLIENT_DisconnectIoPins = ExynosGpioDisconnectIoPins;
    packet.CLIENT_ReadGpioPinsUsingMask = ExynosGpioReadPins;
    packet.CLIENT_WriteGpioPinsUsingMask = ExynosGpioWritePins;

    return GPIO_CLX_RegisterClient(WdfGetDriver(), &packet, RegistryPath);
}

VOID
ExynosGpioEvtDriverUnload (
    _In_ WDFDRIVER Driver
    )
{
    (VOID) GPIO_CLX_UnregisterClient(Driver);
}

NTSTATUS
ExynosGpioEvtDeviceAdd (
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver,
                                                      DeviceInit,
                                                      &attributes);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, device);
}

//
// ---------------------------------------------------------------------------
// GpioClx callbacks
// ---------------------------------------------------------------------------
//

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioPrepareController (
    _In_ WDFDEVICE Device,
    _In_ PVOID Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    WDF_OBJECT_ATTRIBUTES attributes;
    ULONG count;
    ULONG i;
    ULONG interrupts = 0;
    ULONG memory = 0;
    PHYSICAL_ADDRESS phys = { 0 };
    ULONG length = 0;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    RtlZeroMemory(ctx, sizeof(*ctx));

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i += 1) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        switch (desc->Type) {
        case CmResourceTypeMemory:
            if (memory == 0) {
                phys = desc->u.Memory.Start;
                length = desc->u.Memory.Length;
            }
            memory += 1;
            break;

        case CmResourceTypeInterrupt:
            interrupts += 1;
            break;

        default:
            break;
        }
    }

    // The DSDT GPIO device declares exactly one window (0x15850000+0x1000)
    // and 34 EINT lines in bank order (notes/d8-audio.md §3). Anything
    // else is a firmware/driver mismatch — refuse rather than guess.
    //
    if (memory != 1 || length < 0x1000 || interrupts != EXYNOS_GPIO_TOTAL_PINS) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    ctx->Registers = (PUCHAR) MmMapIoSpaceEx(phys, length,
                                            PAGE_READWRITE | MmNonCached);
    if (ctx->Registers == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ctx->RegisterCount = length;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;

    return WdfSpinLockCreate(&attributes, &ctx->HwLock);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
ExynosGpioReleaseController (
    _In_ WDFDEVICE Device,
    _In_ PVOID Context
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Device);

    if (ctx->Registers != NULL) {
        MmUnmapIoSpace(ctx->Registers, ctx->RegisterCount);
        ctx->Registers = NULL;
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioStartController (
    _In_ PVOID Context
    )
{
    ((PEXYNOS_GPIO_CONTEXT)Context)->Started = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioStopController (
    _In_ PVOID Context
    )
{
    ((PEXYNOS_GPIO_CONTEXT)Context)->Started = FALSE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioQueryBasicInformation (
    _In_ PVOID Context,
    _Out_ PCLIENT_CONTROLLER_BASIC_INFORMATION Information
    )
{
    UNREFERENCED_PARAMETER(Context);

    Information->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    Information->Size = sizeof(*Information);

    // 34 banks of 1 pin — see the pseudo-bank note in the header. This is
    // what makes the 34-entry _CRS interrupt list map 1:1 to EINT lines.
    //
    Information->TotalPins = EXYNOS_GPIO_TOTAL_PINS;
    Information->NumberOfPinsPerBank = EXYNOS_GPIO_PINS_PER_BANK;

    // Memory-mapped: query/clear run at DIRQL. The IO registers (CON/DAT/
    // PUD) share the CON word with the EINT function setting, so IO and
    // interrupt paths are NOT independent — do not claim the optimization.
    //
    Information->Flags.MemoryMappedController = TRUE;
    Information->Flags.IndependentIoHwSupported = FALSE;

    // PENDING is read-only with write-1-clears: never auto-clears.
    //
    Information->Flags.ActiveInterruptsAutoClearOnRead = FALSE;

    Information->Flags.FormatIoRequestsAsMasks = TRUE;

    // No D/F-state power management of the block (registers are always-on
    // ALIVE domain; sboot owns pad retention).
    //
    Information->Flags.DeviceIdlePowerMgmtSupported = FALSE;
    Information->Flags.BankIdlePowerMgmtSupported = FALSE;

    // The DSDT declares the keys Edge/ActiveBoth; GpioClx emulates that
    // with level interrupts + dynamic polarity (documented requirement
    // for WoA buttons, notes/d8-audio-windows.md Q3.2). Exynos EINT
    // supports level-low/level-high natively (VERIFIED encodings), which
    // is exactly what the emulation drives.
    //
    Information->Flags.EmulateActiveBoth = TRUE;

    // The hardware digital filter (FLTCON EN|SEL, width 0) is enabled per
    // pin, but it is far shorter than the 10 ms the Android DT debounces
    // these keys with — let GpioClx software-debounce as well.
    //
    Information->Flags.EmulateDebouncing = TRUE;

    return STATUS_SUCCESS;
}

//
// Interrupts. In every callback: BankId == global pin (0..33); PinNumber
// is 0 by construction (1-pin banks).
//

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioEnableInterrupt (
    _In_ PVOID Context,
    _In_ PGPIO_ENABLE_INTERRUPT_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;
    ULONG conWord;
    ULONG type;
    NTSTATUS status;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ExynosGpioTriggerType(Params->InterruptMode,
                                   Params->Polarity,
                                   &type);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfSpinLockAcquire(ctx->HwLock);

    conWord = ExynosGpioWord(ExynosGpioPhysBankBase[phys], EXYNOS_BANK_CON);

    // Refuse to steal a pin already owned by another peripheral function
    // (CON field 1..0xE). Input (0) and EINT (0xF) are claimable.
    //
    {
        ULONG fn = ExynosGpioRdField(ctx, conWord, bit * 4, 0xF);
        if (fn != EXYNOS_PINFUNC_INPUT && fn != EXYNOS_PINFUNC_EINT) {
            WdfSpinLockRelease(ctx->HwLock);
            return STATUS_SHARING_VIOLATION;
        }
    }

    // Function F = EINT (VERIFIED: DT samsung,pin-function = <0xf> for
    // every key; the validated 9810 touch driver writes the same).
    //
    ExynosGpioRmwField(ctx, conWord, bit * 4, 0xF, EXYNOS_PINFUNC_EINT);

    if (Params->PullConfiguration != InterruptPullUnchanged) {
        ExynosGpioRmwField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_PUD),
                           bit * 4, 0xF,
                           ExynosGpioPudValue(Params->PullConfiguration));
    }

    // FLTCON: 8-bit field; FLTCON0 (pins 0-3) at +0, FLTCON1 (pins 4-7)
    // at +4 of the bank's FLTCON pair.
    //
    ExynosGpioRmwField(ctx,
                       EXYNOS_EINT_FLTCON_BASE + 8u * phys + (bit >= 4 ? 4u : 0u),
                       (bit % 4) * 8, 0xFF, EXYNOS_EINT_FLTCON_VALUE);

    ExynosGpioRmwField(ctx, EXYNOS_EINT_CON_BASE + 4u * phys,
                       bit * 4, 0xF, type);

    // Ack any stale pending edge, then unmask.
    //
    ExynosGpioWrite(ctx, EXYNOS_EINT_PEND_BASE + 4u * phys, 1u << bit);
    ExynosGpioRmwField(ctx, EXYNOS_EINT_MASK_BASE + 4u * phys,
                       bit, 1, 0);

    WdfSpinLockRelease(ctx->HwLock);

    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioDisableInterrupt (
    _In_ PVOID Context,
    _In_ PGPIO_DISABLE_INTERRUPT_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->HwLock);
    ExynosGpioRmwField(ctx, EXYNOS_EINT_MASK_BASE + 4u * phys, bit, 1, 1);

    // Leave CON/PUD/EINT_CON as they are: the pin stays an EINT source
    // for the next enable, and touch-style consumers reprogram anyway.
    //
    WdfSpinLockRelease(ctx->HwLock);

    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioMaskInterrupts (
    _In_ PVOID Context,
    _In_ PGPIO_MASK_INTERRUPT_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->HwLock);
    if (Params->PinMask & 1u) {
        ExynosGpioRmwField(ctx, EXYNOS_EINT_MASK_BASE + 4u * phys, bit, 1, 1);
    }
    WdfSpinLockRelease(ctx->HwLock);

    Params->FailedMask = 0;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioUnmaskInterrupt (
    _In_ PVOID Context,
    _In_ PGPIO_ENABLE_INTERRUPT_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->HwLock);
    ExynosGpioWrite(ctx, EXYNOS_EINT_PEND_BASE + 4u * phys, 1u << bit);
    ExynosGpioRmwField(ctx, EXYNOS_EINT_MASK_BASE + 4u * phys, bit, 1, 0);
    WdfSpinLockRelease(ctx->HwLock);

    return STATUS_SUCCESS;
}

//
// Called by GpioClx at DIRQL for ActiveBoth emulation (polarity flip
// between press and release).
//
_IRQL_requires_same_
NTSTATUS
ExynosGpioReconfigureInterrupt (
    _In_ PVOID Context,
    _In_ PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;
    ULONG type;
    NTSTATUS status;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    status = ExynosGpioTriggerType(Params->InterruptMode,
                                   Params->Polarity,
                                   &type);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Single 32-bit RMW; DIRQL, GpioClx holds its bank lock, and no other
    // CPU can be in these callbacks for the same physical word without our
    // HwLock — take it (WdfSpinLock is DIRQL-safe).
    //
    WdfSpinLockAcquire(ctx->HwLock);
    ExynosGpioRmwField(ctx, EXYNOS_EINT_CON_BASE + 4u * phys, bit * 4, 0xF,
                       type);
    WdfSpinLockRelease(ctx->HwLock);

    return STATUS_SUCCESS;
}

_IRQL_requires_same_
NTSTATUS
ExynosGpioQueryActiveInterrupts (
    _In_ PVOID Context,
    _In_ PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;
    ULONG pending;
    ULONG mask;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    pending = ExynosGpioRead(ctx, EXYNOS_EINT_PEND_BASE + 4u * phys);
    mask = ExynosGpioRead(ctx, EXYNOS_EINT_MASK_BASE + 4u * phys);

    Params->ActiveMask = ((pending & ~(ULONG)mask) & (1u << bit)) ? 1u : 0u;
    return STATUS_SUCCESS;
}

_IRQL_requires_same_
NTSTATUS
ExynosGpioClearActiveInterrupts (
    _In_ PVOID Context,
    _In_ PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    // Write-1-clears (VERIFIED: exynos_irq_ack writes the pend register
    // with the bit). Only bit 0 of the caller's mask exists (1-pin bank).
    //
    if (Params->ClearActiveMask & 1u) {
        ExynosGpioWrite(ctx, EXYNOS_EINT_PEND_BASE + 4u * phys, 1u << bit);
    }

    Params->FailedClearMask = 0;
    return STATUS_SUCCESS;
}

_IRQL_requires_same_
NTSTATUS
ExynosGpioQueryEnabledInterrupts (
    _In_ PVOID Context,
    _In_ PGPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;
    ULONG fn;
    ULONG mask;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    fn = ExynosGpioRdField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_CON),
                           bit * 4, 0xF);
    mask = ExynosGpioRead(ctx, EXYNOS_EINT_MASK_BASE + 4u * phys);

    Params->EnabledMask =
        (fn == EXYNOS_PINFUNC_EINT && (mask & (1u << bit)) == 0) ? 1u : 0u;
    return STATUS_SUCCESS;
}

//
// IO.
//

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioConnectIoPins (
    _In_ PVOID Context,
    _In_ PGPIO_CONNECT_IO_PINS_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;
    ULONG conWord;
    ULONG fn;
    ULONG i;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    conWord = ExynosGpioWord(ExynosGpioPhysBankBase[phys], EXYNOS_BANK_CON);

    WdfSpinLockAcquire(ctx->HwLock);

    fn = ExynosGpioRdField(ctx, conWord, bit * 4, 0xF);
    if (fn == EXYNOS_PINFUNC_EINT) {
        // Refuse to steal an EINT-claimed pin.
        //
        WdfSpinLockRelease(ctx->HwLock);
        return STATUS_SHARING_VIOLATION;
    }

    for (i = 0; i < Params->PinCount; i += 1) {

        // PinNumberTable entries are bank-relative; with 1-pin banks only
        // pin 0 is valid.
        //
        if (Params->PinNumberTable[i] != 0) {
            WdfSpinLockRelease(ctx->HwLock);
            return STATUS_INVALID_PARAMETER;
        }

        ExynosGpioRmwField(ctx, conWord, bit * 4, 0xF,
                           (Params->ConnectMode == ConnectModeInput)
                               ? EXYNOS_PINFUNC_INPUT
                               : EXYNOS_PINFUNC_OUTPUT);

        ExynosGpioRmwField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_PUD),
                           bit * 4, 0xF,
                           ExynosGpioPudValue(Params->PullConfiguration));
    }

    WdfSpinLockRelease(ctx->HwLock);
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioDisconnectIoPins (
    _In_ PVOID Context,
    _In_ PGPIO_DISCONNECT_IO_PINS_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;
    ULONG i;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    // Return to reset-ish state: input, no pull. (The vendor PDN registers
    // are left alone — pad retention is sboot/PMIC policy, not ours.)
    //
    WdfSpinLockAcquire(ctx->HwLock);
    for (i = 0; i < Params->PinCount; i += 1) {
        if (Params->PinNumberTable[i] != 0) {
            WdfSpinLockRelease(ctx->HwLock);
            return STATUS_INVALID_PARAMETER;
        }
        ExynosGpioRmwField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_CON),
                           bit * 4, 0xF, EXYNOS_PINFUNC_INPUT);
        ExynosGpioRmwField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_PUD),
                           bit * 4, 0xF, 0);
    }
    WdfSpinLockRelease(ctx->HwLock);

    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioReadPins (
    _In_ PVOID Context,
    _In_ PGPIO_READ_PINS_MASK_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    *Params->PinValues =
        (ExynosGpioRead(ctx,
                        ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                       EXYNOS_BANK_DAT)) >> bit) & 1u;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ExynosGpioWritePins (
    _In_ PVOID Context,
    _In_ PGPIO_WRITE_PINS_MASK_PARAMETERS Params
    )
{
    PEXYNOS_GPIO_CONTEXT ctx = (PEXYNOS_GPIO_CONTEXT)Context;
    BANK_ID pin = Params->BankId;
    ULONG phys = pin / EXYNOS_GPIO_BANK_STRIDE;
    ULONG bit = pin % EXYNOS_GPIO_BANK_STRIDE;

    if (!ExynosGpioPinValid(pin)) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfSpinLockAcquire(ctx->HwLock);
    if (Params->SetMask & 1u) {
        ExynosGpioRmwField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_DAT),
                           bit, 1, 1);
    }
    if (Params->ClearMask & 1u) {
        ExynosGpioRmwField(ctx,
                           ExynosGpioWord(ExynosGpioPhysBankBase[phys],
                                          EXYNOS_BANK_DAT),
                           bit, 1, 0);
    }
    WdfSpinLockRelease(ctx->HwLock);

    return STATUS_SUCCESS;
}
