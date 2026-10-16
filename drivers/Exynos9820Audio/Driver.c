/*
 * Driver.c - Exynos9820Audio entry, PnP/power and the _CRS contract.
 *
 * Binds ACPI\SAMS9827 (Dsdt.asl Device (AUD0)). ACX 1.1 like the sample (A6; 1.0 is the pre-release DDI version,
 * notes/d8-audio-win-a5.md); the 22621 runtime is Acx01000.sys (major 1). Entry/AddDevice shape:
 * Windows-driver-samples audio/Acx/Samples/AudioCodec/Driver/{Driver,Device}.cpp.
 *
 * PrepareHardware: check the eight memory descriptors against the Exynos9820.h windows (refuse to map anything on a
 * mismatch), map them uncached, run the read-only ABOX audit (guarded by PMU AUD_STATUS), bring the amp bus up and
 * identify the amps (Parameters\AmpInit), add the render circuit. The circuit is added even when the hardware part
 * failed: the endpoint then streams on its timer clock only, which still proves the Windows side.
 *
 * NOT VALIDATED ON HARDWARE (stage Guided).
 */

#include "Exynos9820Audio.h"

DRIVER_INITIALIZE                 DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD         AudEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE   AudEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE   AudEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY           AudEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT            AudEvtD0Exit;
EVT_WDF_OBJECT_CONTEXT_CLEANUP    AudEvtDeviceCleanup;

#define AUD_DRIVER_VERSION "1.0.1.33"

static AUD_CONFIG g_Cfg;

static const struct {
    ULONGLONG Base;
    ULONG     Size;
    PCSTR     Name;
} AudResContract[AUD_RES_COUNT] = {
    { AUD_ABOX_IFR_BASE,     AUD_ABOX_IFR_SIZE,     "ABOX IFR" },
    { AUD_ABOX_SYSREG_BASE,  AUD_ABOX_SYSREG_SIZE,  "SYSREG_AUD" },
    { AUD_CMU_AUD_BASE,      AUD_CMU_AUD_SIZE,      "CMU_AUD" },
    { AUD_GPIO_AUD_BASE,     AUD_GPIO_AUD_SIZE,     "GPIO_AUD" },
    { AUD_HSI2C_BASE,        AUD_HSI2C_SIZE,        "HSI2C38" },
    { AUD_ABOX_SRAM_BASE,    AUD_ABOX_SRAM_SIZE,    "ABOX SRAM" },
    { AUD_PMU_AUD_PAGE_BASE, AUD_PMU_AUD_PAGE_SIZE, "PMU AUD page" },
    { AUD_PERIC1_GPIO_BASE,  AUD_PERIC1_GPIO_SIZE,  "PERIC1 GPIO" },
};

static VOID AudReadConfig(_In_ WDFDRIVER Driver)
{
    static const struct {
        PCWSTR Name;
        SIZE_T Offset;
    } Values[] = {
        { L"AmpInit",    FIELD_OFFSET(AUD_CONFIG, AmpInit) },
        { L"AmpEnable",  FIELD_OFFSET(AUD_CONFIG, AmpEnable) },
        { L"AboxEnable", FIELD_OFFSET(AUD_CONFIG, AboxEnable) },
        { L"HwVolume",   FIELD_OFFSET(AUD_CONFIG, HwVolume) },
        { L"Stereo",     FIELD_OFFSET(AUD_CONFIG, Stereo) },
        { L"KsOverride", FIELD_OFFSET(AUD_CONFIG, KsOverride) },
        { L"DefaultMode", FIELD_OFFSET(AUD_CONFIG, DefaultMode) },
        { L"RcvSlotReg", FIELD_OFFSET(AUD_CONFIG, RcvSlotReg) },
        { L"RcvAtten",   FIELD_OFFSET(AUD_CONFIG, RcvAtten) },
        { L"SpkAtten",   FIELD_OFFSET(AUD_CONFIG, SpkAtten) },
        { L"RcvGain",    FIELD_OFFSET(AUD_CONFIG, RcvGain) },
        { L"SpkGain",    FIELD_OFFSET(AUD_CONFIG, SpkGain) },
        { L"CodecEnable", FIELD_OFFSET(AUD_CONFIG, CodecEnable) },
        { L"JackTone",   FIELD_OFFSET(AUD_CONFIG, JackTone) },
        { L"HpRoute",    FIELD_OFFSET(AUD_CONFIG, HpRoute) },
        { L"HpMaster",   FIELD_OFFSET(AUD_CONFIG, HpMaster) },
    };
    WDFKEY         Key;
    UNICODE_STRING Name;
    ULONG          V;
    ULONG          I;

    g_Cfg.AmpInit    = 0;   /* A8: hardware stages off unless the tester sets them (Exynos9820Audio.h AUD_CONFIG) */
    g_Cfg.AmpEnable  = 0;
    g_Cfg.AboxEnable = 0;
    g_Cfg.HwVolume   = 0;
    g_Cfg.Stereo     = 0;
    g_Cfg.KsOverride = 1;
    g_Cfg.DefaultMode = 1;
    g_Cfg.RcvSlotReg  = 0xFFFFFFFF;
    /* Balance baseline by ear (user, 2026-10-16, S10eSpeakerTrim): earpiece analog gain 12 (+2 dB over the boot #99 0x0A),
       loudspeaker 0x0A with 5 dB digital attenuation. Max gain AmpI2c.c AMP_GAIN_MAX. Same values = 0 dB in the app. */
    g_Cfg.RcvAtten    = 0;
    g_Cfg.SpkAtten    = 40;
    g_Cfg.RcvGain     = 12;
    g_Cfg.SpkGain     = 0x0A;
    g_Cfg.CodecEnable = 0;
    g_Cfg.JackTone    = 0;
    g_Cfg.HpRoute     = 1;
    g_Cfg.HpMaster    = 1;   /* ABOX-master BCLK never reached the codec (FLL2 no lock, 1.0.1.25-31) */

    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        return;
    }

    for (I = 0; I < ARRAYSIZE(Values); I++) {
        RtlInitUnicodeString(&Name, Values[I].Name);
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &Name, &V))) {
            *(PULONG)((PUCHAR)&g_Cfg + Values[I].Offset) = V;
        }
    }

    WdfRegistryClose(Key);
}

_Use_decl_annotations_
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG WdfCfg;
    ACX_DRIVER_CONFIG AcxCfg;
    WDFDRIVER         Driver;
    NTSTATUS          Status;

    WDF_DRIVER_CONFIG_INIT(&WdfCfg, AudEvtDeviceAdd);
    WdfCfg.DriverPoolTag = AUD_POOL_TAG;
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &WdfCfg, &Driver);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ACX_DRIVER_CONFIG_INIT(&AcxCfg);
    Status = AcxDriverInitialize(Driver, &AcxCfg);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    AudReadConfig(Driver);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS                     Status;
    ACX_DEVICEINIT_CONFIG        DevInitCfg;
    ACX_DEVICE_CONFIG            DevCfg;
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES        Attr;
    WDFDEVICE                    Device;
    PAUD_DEVICE_CONTEXT          Dev;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    ACX_DEVICEINIT_CONFIG_INIT(&DevInitCfg);
    Status = AcxDeviceInitInitialize(DeviceInit, &DevInitCfg);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = AudEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = AudEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry         = AudEvtD0Entry;
    Pnp.EvtDeviceD0Exit          = AudEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_DEVICE_CONTEXT);
    Attr.EvtCleanupCallback = AudEvtDeviceCleanup;
    Status = WdfDeviceCreate(&DeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Dev = AudGetDeviceContext(Device);
    Dev->Device = Device;
    Dev->Cfg    = g_Cfg;
    Dev->Log    = AudLogCreate();
    KeInitializeSpinLock(&Dev->ReqLock);
    Dev->SpeakerConfig = KSAUDIO_SPEAKER_STEREO;
    Dev->Amp[0].Addr = AUD_AMP_R_I2C_ADDR;
    Dev->Amp[0].ResetPin = AUD_AMP_R_RESET_PIN;
    Dev->Amp[0].Name = "R/bot";
    Dev->Amp[1].Addr = AUD_AMP_L_I2C_ADDR;
    Dev->Amp[1].ResetPin = AUD_AMP_L_RESET_PIN;
    Dev->Amp[1].Name = "L/rcv";
    /* HwVolume = 1: until Windows assigns its persisted level, sit at the amp notes' first-test -20 dB, not at the
       0 dB a zeroed context would mean (KS units, 1/65536 dB). */
    Dev->VolumeLevel = -20 * 0x10000;
    AudSwVolumeInit(Dev, -20 * 0x10000);   /* HwVolume = 0: same starting level, applied in software */

    AudLog(Dev->Log, "Exynos9820Audio %s (ACX 1.%d, ACPI\\SAMS9827): AmpInit %lu AmpEnable %lu AboxEnable %lu HwVolume %lu "
           "Stereo %lu\r\n", AUD_DRIVER_VERSION, (int)ACX_VERSION_MINOR, Dev->Cfg.AmpInit, Dev->Cfg.AmpEnable, Dev->Cfg.AboxEnable,
           Dev->Cfg.HwVolume, Dev->Cfg.Stereo);

    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->BusLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->CodecLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ACX_DEVICE_CONFIG_INIT(&DevCfg);
    Status = AcxDeviceInitialize(Device, &DevCfg);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "AcxDeviceInitialize %08lX\r\n", Status);
        return Status;
    }

    Status = AudRenderCircuitCreate(Device, Dev);
    AudLog(Dev->Log, "render circuit \"Speaker0\": %08lX%s\r\n", Status,
           Dev->Cfg.HwVolume ? " (volume + mute elements on the amps)" : " (volume + mute elements in software)");
    return Status;
}

/* Returns TRUE when all eight descriptors are present, in order, and equal the contract. */
static BOOLEAN AudCheckResources(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ WDFCMRESLIST Translated,
                                 _Out_writes_(AUD_RES_COUNT) PHYSICAL_ADDRESS *Phys)
{
    ULONG   Count = WdfCmResourceListGetCount(Translated);
    ULONG   Mem = 0;
    ULONG   I;
    BOOLEAN Ok = TRUE;

    for (I = 0; I < Count; I++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR D = WdfCmResourceListGetDescriptor(Translated, I);

        if (D == NULL || D->Type != CmResourceTypeMemory) {
            continue;
        }
        if (Mem < AUD_RES_COUNT) {
            Phys[Mem] = D->u.Memory.Start;
            Dev->Len[Mem] = D->u.Memory.Length;
            AudLog(Dev->Log, "  mem%lu: 0x%I64X + 0x%lX (%s, expected 0x%I64X + 0x%lX)\r\n", Mem, D->u.Memory.Start.QuadPart,
                   D->u.Memory.Length, AudResContract[Mem].Name, AudResContract[Mem].Base, AudResContract[Mem].Size);
            if ((ULONGLONG)D->u.Memory.Start.QuadPart != AudResContract[Mem].Base ||
                D->u.Memory.Length != AudResContract[Mem].Size) {
                Ok = FALSE;
            }
        }
        Mem++;
    }

    if (Mem != AUD_RES_COUNT) {
        AudLog(Dev->Log, "_CRS has %lu memory descriptors, the contract has %u\r\n", Mem, AUD_RES_COUNT);
        Ok = FALSE;
    }

    return Ok;
}

static VOID AudUnmap(_Inout_ PAUD_DEVICE_CONTEXT Dev)
{
    ULONG I;

    for (I = 0; I < AUD_RES_COUNT; I++) {
        if (Dev->Va[I] != NULL) {
            MmUnmapIoSpace(Dev->Va[I], Dev->Len[I]);
            Dev->Va[I] = NULL;
        }
    }
    Dev->Mapped = FALSE;
}

_Use_decl_annotations_
NTSTATUS AudEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Resources, WDFCMRESLIST Translated)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetDeviceContext(Device);
    PHYSICAL_ADDRESS    Phys[AUD_RES_COUNT];
    NTSTATUS            Status;
    ULONG               I;

    UNREFERENCED_PARAMETER(Resources);
    PAGED_CODE();

    RtlZeroMemory(Phys, sizeof(Phys));
    AudLog(Dev->Log, "PrepareHardware: %lu resources\r\n", WdfCmResourceListGetCount(Translated));
    if (!AudCheckResources(Dev, Translated, Phys)) {
        AudLog(Dev->Log, "RESULT: _CRS does not match the Dsdt.asl AUD0 contract - hardware untouched, timer-only endpoint\r\n");
    } else {
        Dev->Mapped = TRUE;
        for (I = 0; I < AUD_RES_COUNT; I++) {
            Dev->Va[I] = (PUCHAR)MmMapIoSpaceEx(Phys[I], Dev->Len[I], PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Va[I] == NULL) {
                AudLog(Dev->Log, "RESULT: MmMapIoSpaceEx(%s) failed - hardware untouched\r\n", AudResContract[I].Name);
                AudUnmap(Dev);
                break;
            }
        }
    }

    if (Dev->Mapped) {
        (VOID)AboxAudit(Dev);
        if (Dev->Cfg.AmpInit) {
            Status = AmpInit(Dev);
            AudLog(Dev->Log, "AmpInit %08lX\r\n", Status);
        } else {
            AudLog(Dev->Log, "AmpInit skipped (Parameters\\AmpInit = 0)\r\n");
        }
    }
    if (Dev->Cfg.CodecEnable) {
        Status = CodecInit(Dev);
        AudLog(Dev->Log, "CodecInit %08lX\r\n", Status);
        AudLogFlush(Dev->Log);
        if (NT_SUCCESS(Status)) {
            Status = CodecSetup(Dev);
            AudLog(Dev->Log, "CodecSetup %08lX\r\n", Status);
            AudLogFlush(Dev->Log);
        }
        if (NT_SUCCESS(Status)) {
            Status = CodecJackStart(Dev, Device);
            AudLog(Dev->Log, "CodecJackStart %08lX\r\n", Status);
        }
        if (NT_SUCCESS(Status) && Dev->Cfg.JackTone != 0) {
            Status = CodecTone(Dev, min(Dev->Cfg.JackTone, 5000u));
            AudLog(Dev->Log, "CodecTone %08lX\r\n", Status);
        }
    }

    Status = STATUS_SUCCESS;
    if (Dev->Render == NULL) {
        /* run audB2 (bugcheck D1 on pnputil /restart-device): ReleaseHardware's AcxDeviceRemoveCircuit deletes the
           circuit, so a restart must create a new one (ACX AudioCodec sample: create + add in PrepareHardware) */
        Status = AudRenderCircuitCreate(Device, Dev);
        AudLog(Dev->Log, "render circuit re-created: %08lX\r\n", Status);
    }
    if (NT_SUCCESS(Status) && !Dev->CircuitAdded && Dev->Render != NULL) {
        Status = AcxDeviceAddCircuit(Device, Dev->Render);
        Dev->CircuitAdded = NT_SUCCESS(Status);
    }

    AudLog(Dev->Log, "PrepareHardware done: hardware %s, ABOX %s, circuit %08lX\r\n", Dev->Mapped ? "mapped" : "NOT mapped",
           Dev->AboxAlive ? "alive" : "off", Status);
    AudLogFlush(Dev->Log);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    PAGED_CODE();

    if (Dev->CircuitAdded) {
        (VOID)AcxDeviceRemoveCircuit(Device, Dev->Render);
        Dev->CircuitAdded = FALSE;
        Dev->Render       = NULL;   /* deleted by the remove; PrepareHardware creates a new one */
    }

    if (Dev->Mapped) {
        AmpRelease(Dev);    /* amps off before their clocks go */
        AboxRelease(Dev);   /* stop + restore every stage-1 register */
    }
    CodecJackStop(Dev);
    CodecRelease(Dev);
    AudUnmap(Dev);
    Dev->AboxReady = FALSE;
    Dev->AboxAlive = FALSE;

    AudLog(Dev->Log, "ReleaseHardware: hardware released\r\n");
    AudLogFlush(Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetDeviceContext(Device);

    AudLog(Dev->Log, "D0Entry from D%d\r\n", (int)PreviousState - (int)WdfPowerDeviceD0);
    AudLogFlush(Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetDeviceContext(Device);

    PAGED_CODE();

    /* Streams are stopped by ACX before the device leaves D0; this only catches a hardware path left on. */
    AmpPowerDown(Dev);
    AboxStop(Dev);
    AudLog(Dev->Log, "D0Exit to D%d\r\n", (int)TargetState - (int)WdfPowerDeviceD0);
    AudLogFlush(Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID AudEvtDeviceCleanup(WDFOBJECT Object)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetDeviceContext((WDFDEVICE)Object);

    AudLogFree(Dev->Log);
    Dev->Log = NULL;
}
