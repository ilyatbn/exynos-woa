/*
 * Miniclass.c - battery class callbacks and the poll that feeds them.
 *
 * Shape follows Microsoft's simbatt sample (MIT): every class callback answers from a cached state under
 * StateLock and never touches the bus. The poll (work item, PASSIVE_LEVEL) takes BusLock, reads the
 * MAX77705, converts to Windows units under StateLock and calls BatteryClassStatusNotify when the reported
 * state changed. Units and the mapping are documented in notes/battery-driver-m1.md ("design decision").
 */

#include "S10eBattery.h"

#define S10E_BATT_DEVICE_NAME   L"Galaxy S10e battery (MAX77705 fuel gauge)"
#define S10E_BATT_MANUFACTURER  L"Samsung"
#define S10E_BATT_SERIAL        L"0"
#define S10E_BATT_UNIQUE_ID     L"S10eBattery0"

#pragma alloc_text(PAGE, S10eBattQueryTag)
#pragma alloc_text(PAGE, S10eBattQueryInformation)
#pragma alloc_text(PAGE, S10eBattSetInformation)
#pragma alloc_text(PAGE, S10eBattQueryStatus)
#pragma alloc_text(PAGE, S10eBattSetStatusNotify)
#pragma alloc_text(PAGE, S10eBattDisableStatusNotify)

static ULONG MahToMwh(_In_ ULONG Mah)
{
    return (ULONG)(((ULONGLONG)Mah * S10E_BATT_NOMINAL_MV_UNVERIFIED) / 1000);
}

VOID S10eBattInitState(_Inout_ PS10E_BATT_FDO Fdo)
{
    Fdo->BatteryTag = BATTERY_TAG_INVALID;
    Fdo->NextTag    = 1;
    RtlZeroMemory(&Fdo->Info, sizeof(Fdo->Info));
    RtlZeroMemory(&Fdo->Status, sizeof(Fdo->Status));
    Fdo->Status.Voltage = BATTERY_UNKNOWN_VOLTAGE;
    Fdo->Status.Rate    = BATTERY_UNKNOWN_RATE;
    Fdo->Status.Capacity = BATTERY_UNKNOWN_CAPACITY;
}

/* Build the static information once per tag. Full/design capacity are frozen for the tag's lifetime so that the
   class driver's cached FullChargedCapacity and our RemainingCapacity stay on the same scale (Windows % =
   Capacity / FullChargedCapacity = SOCREP exactly). */
static VOID BuildInformation(_Inout_ PS10E_BATT_FDO Fdo, _In_ PS10E_BATT_READING R)
{
    ULONG FullMah   = (R->FullCapMah != 0) ? R->FullCapMah : R->DesignCapMah;
    ULONG DesignMah = (R->DesignCapMah != 0) ? R->DesignCapMah : FullMah;

    RtlZeroMemory(&Fdo->Info, sizeof(Fdo->Info));
    Fdo->Info.Capabilities        = BATTERY_SYSTEM_BATTERY;
    Fdo->Info.Technology          = 1;    /* rechargeable */
    Fdo->Info.Chemistry[0]        = 'L';
    Fdo->Info.Chemistry[1]        = 'I';
    Fdo->Info.Chemistry[2]        = 'O';
    Fdo->Info.Chemistry[3]        = 'N';
    Fdo->Info.DesignedCapacity    = MahToMwh(DesignMah);
    Fdo->Info.FullChargedCapacity = MahToMwh(FullMah);
    Fdo->Info.DefaultAlert1       = 0;    /* no vendor value; Windows uses its power-plan levels */
    Fdo->Info.DefaultAlert2       = 0;
    Fdo->Info.CriticalBias        = 0;
    Fdo->Info.CycleCount          = 0;    /* CYCLES_REG is rewritten by the vendor driver - not meaningful */
}

/* Map one reading to BATTERY_STATUS. Charger: CHGIN_DTLS == 3 -> on line; CHG_DTLS 0..2 charging,
   3..4 full, 5..7 not charging, 8/0xA/0xB "discharging" (vendor max77705_get_charger_state). */
static VOID BuildStatus(_Inout_ PS10E_BATT_FDO Fdo, _In_ PS10E_BATT_READING R)
{
    BOOLEAN OnLine   = FALSE;
    BOOLEAN Charging = FALSE;
    ULONG   ChgDtls  = 0xF;
    LONG    RateMw;

    if (R->ChargerRead) {
        OnLine  = (MAX77705_CHGIN_DTLS(R->ChgDetails00) == MAX77705_CHGIN_DTLS_VALID);
        ChgDtls = MAX77705_CHG_DTLS(R->ChgDetails01);
        Charging = OnLine && (ChgDtls <= 2);
    }
    /* OTG boost on (UEFI host-mode fallback when no source attached, Beyond0LteUsbHostDxe Pmic.c): the phone
       drives VBUS itself, so a valid CHGIN says nothing about an external supply and nothing charges. */
    if (R->Cnfg00Read && ((R->ChgCnfg00 & MAX77705_MODE_MASK) == MAX77705_MODE_OTG_ON ||
                          (R->ChgCnfg00 & MAX77705_MODE_MASK) == MAX77705_MODE_CHG_OTG_ON)) {
        OnLine   = FALSE;
        Charging = FALSE;
    }

    RateMw = (LONG)(((LONGLONG)R->AvgCurrentMa * (LONGLONG)R->VcellMv) / 1000);

    Fdo->Status.PowerState = 0;
    if (OnLine) {
        Fdo->Status.PowerState |= BATTERY_POWER_ON_LINE;
    }

    if (Charging) {
        Fdo->Status.PowerState |= BATTERY_CHARGING;
        Fdo->Status.Rate = (RateMw > 0) ? RateMw : 0;
    } else if (!OnLine || (RateMw < 0 && ChgDtls >= 8)) {
        /* On battery, or plugged in while the charger is off (e.g. the dock: dock-desktop.md section 10). */
        Fdo->Status.PowerState |= BATTERY_DISCHARGING;
        Fdo->Status.Rate = (RateMw < 0) ? RateMw : (LONG)BATTERY_UNKNOWN_RATE;
    } else {
        Fdo->Status.Rate = 0;     /* on line, full / not charging */
    }

    Fdo->Status.Capacity = (ULONG)(((ULONGLONG)Fdo->Info.FullChargedCapacity * R->SocTenths) / 1000);
    Fdo->Status.Voltage  = R->VcellMv;

    /* BatteryTemperature: tenths of a degree Kelvin. */
    Fdo->TemperatureDeciK = (ULONG)((R->TempMilliC + 273150) / 100);
}

/* Diagnostics for the device test (extra-battery.cmd reads them): HKLM\...\Enum\ACPI\SAMS9826\0\Device Parameters */
static VOID SaveDiagnostics(_In_ PS10E_BATT_FDO Fdo, _In_ PS10E_BATT_READING R)
{
    WDFKEY Key;
    struct { PCWSTR Name; ULONG Value; } Values[] = {
        { L"PollOk",        Fdo->PollOk },
        { L"PollFail",      Fdo->PollFail },
        { L"LastStatus",    (ULONG)Fdo->LastPollStatus },
        { L"FgStatus",      R->FgStatus },
        { L"SocTenths",     R->SocTenths },
        { L"VcellMv",       R->VcellMv },
        { L"AvgCurrentMa",  (ULONG)R->AvgCurrentMa },
        { L"TempMilliC",    (ULONG)R->TempMilliC },
        { L"RemCapMah",     R->RemCapMah },
        { L"FullCapMah",    R->FullCapMah },
        { L"DesignCapMah",  R->DesignCapMah },
        { L"ChgDetails",    R->ChargerRead ? ((ULONG)R->ChgDetails00 | ((ULONG)R->ChgDetails01 << 8)) : 0xFFFFFFFF },
        { L"PowerState",    Fdo->Status.PowerState },
        { L"ChgCnfg00",     R->ChgCnfg00 },
        { L"ChgCnfg06",     R->ChgCnfg06 },
        { L"ChgCnfg02",     R->ChgCnfg02 },
        { L"ChgCnfg09",     R->ChgCnfg09 },
        { L"WdtKicked",     R->WdtKicked },
        { L"HandoffUsiMode", Fdo->SavedMode },
        { L"HandoffGpm12",  Fdo->SavedScl },
        { L"HandoffGpm13",  Fdo->SavedSda },
        { L"HandoffUsiCon", Fdo->SavedUsiCon },
        { L"NotifyCount",   Fdo->NotifyCount },
        { L"QueryStatusCount", Fdo->QueryStatusCount },
        { L"QueriedPowerState", Fdo->QueriedPowerState },
    };
    ULONG          i;
    UNICODE_STRING Name;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Fdo->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        return;
    }

    for (i = 0; i < ARRAYSIZE(Values); i++) {
        RtlInitUnicodeString(&Name, Values[i].Name);
        (VOID)WdfRegistryAssignULong(Key, &Name, Values[i].Value);
    }

    WdfRegistryClose(Key);
}

/* One history line per notify, ring of S10E_BATT_HIST_SLOTS values (HistNN, slot = NotifyCount % slots):
   n = notify number, t = seconds since boot, ps = PowerState sent, soc = tenths, chg = DETAILS_01:00,
   i = AVG_I mA, q = QueryStatus calls so far, qps = PowerState the class driver last read. */
static VOID SaveHistory(_In_ PS10E_BATT_FDO Fdo, _In_ PS10E_BATT_READING R)
{
    WDFKEY         Key;
    WCHAR          NameBuf[8];
    WCHAR          Line[128];
    UNICODE_STRING Name;
    UNICODE_STRING Value;

    if (!NT_SUCCESS(RtlStringCbPrintfW(NameBuf, sizeof(NameBuf), L"Hist%02u", Fdo->NotifyCount % S10E_BATT_HIST_SLOTS)) ||
        !NT_SUCCESS(RtlStringCbPrintfW(Line, sizeof(Line), L"n=%u t=%I64us ps=%u soc=%u chg=%04X i=%d q=%u qps=%u",
                                       Fdo->NotifyCount, KeQueryInterruptTime() / 10000000ULL, Fdo->Status.PowerState,
                                       R->SocTenths,
                                       R->ChargerRead ? ((ULONG)R->ChgDetails00 | ((ULONG)R->ChgDetails01 << 8)) : 0xFFFF,
                                       R->AvgCurrentMa, Fdo->QueryStatusCount, Fdo->QueriedPowerState))) {
        return;
    }
    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Fdo->Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        return;
    }
    RtlInitUnicodeString(&Name, NameBuf);
    RtlInitUnicodeString(&Value, Line);
    (VOID)WdfRegistryAssignUnicodeString(Key, &Name, &Value);
    WdfRegistryClose(Key);
}

VOID S10eBattPoll(_In_ WDFDEVICE Device)
{
    PS10E_BATT_FDO    Fdo = S10eBattGetFdo(Device);
    S10E_BATT_READING R;
    NTSTATUS          Status;
    BOOLEAN           Notify = FALSE;
    BOOLEAN           Save   = FALSE;
    ULONG             OldPowerState;
    ULONG             OldPercent;
    ULONG             OldTag;

    WdfWaitLockAcquire(Fdo->BusLock, NULL);
    Status = S10eMax77705Read(Fdo, &R);
    WdfWaitLockRelease(Fdo->BusLock);

    WdfWaitLockAcquire(Fdo->StateLock, NULL);
    OldTag        = Fdo->BatteryTag;
    OldPowerState = Fdo->Status.PowerState;
    OldPercent    = Fdo->Last.SocTenths / 10;
    Fdo->LastPollStatus = Status;

    if (NT_SUCCESS(Status)) {
        Fdo->PollOk++;
        if (Fdo->BatteryTag == BATTERY_TAG_INVALID) {
            Fdo->BatteryTag = Fdo->NextTag++;
            if (Fdo->NextTag == BATTERY_TAG_INVALID) {
                Fdo->NextTag = 1;
            }
            BuildInformation(Fdo, &R);
        }
        Fdo->Last = R;
        BuildStatus(Fdo, &R);
        Notify = (OldTag != Fdo->BatteryTag) || (OldPowerState != Fdo->Status.PowerState) ||
                 (OldPercent != R.SocTenths / 10);
        Save = (Fdo->PollOk == 1) || Notify || ((Fdo->PollOk % 15) == 0);   /* every 5 min: QueryStatusCount */
    } else {
        Fdo->PollFail++;
        if (Status == STATUS_NO_MEDIA_IN_DEVICE) {
            /* Gauge says the battery is absent: drop the tag so the class driver re-queries. */
            Fdo->BatteryTag = BATTERY_TAG_INVALID;
            Notify = (OldTag != BATTERY_TAG_INVALID);
        }
        /* Any other failure: keep reporting the last good reading (stale but sane). The diagnostics get the
           attempted (partial / rejected) reading R, which is what a device test needs to see. */
        Save = (Fdo->PollFail <= 3) || ((Fdo->PollFail % 32) == 0);
        S10eBattPrint(DPFLTR_ERROR_LEVEL, "poll failed %08x (ok %u fail %u)\n", Status, Fdo->PollOk, Fdo->PollFail);
    }

    if (Notify) {
        Fdo->NotifyCount++;
        SaveHistory(Fdo, &R);
    }
    if (Save) {
        SaveDiagnostics(Fdo, &R);
    }
    WdfWaitLockRelease(Fdo->StateLock);

    if (Notify) {
        WdfWaitLockAcquire(Fdo->ClassInitLock, NULL);
        if (Fdo->ClassHandle != NULL) {
            BatteryClassStatusNotify(Fdo->ClassHandle);
        }
        WdfWaitLockRelease(Fdo->ClassInitLock);
    }
}

_Use_decl_annotations_
NTSTATUS S10eBattQueryTag(PVOID Context, PULONG BatteryTag)
{
    PS10E_BATT_FDO Fdo = (PS10E_BATT_FDO)Context;

    PAGED_CODE();
    WdfWaitLockAcquire(Fdo->StateLock, NULL);
    *BatteryTag = Fdo->BatteryTag;
    WdfWaitLockRelease(Fdo->StateLock);
    return (*BatteryTag == BATTERY_TAG_INVALID) ? STATUS_NO_SUCH_DEVICE : STATUS_SUCCESS;
}

static NTSTATUS CopyString(_In_ PCWSTR Src, _Out_ PVOID *Ret, _Out_ size_t *RetLen)
{
    NTSTATUS Status;

    *Ret    = (PVOID)Src;
    Status  = RtlStringCbLengthW(Src, MAX_BATTERY_STRING_SIZE * sizeof(WCHAR), RetLen);
    *RetLen += sizeof(WCHAR);
    return Status;
}

_Use_decl_annotations_
NTSTATUS S10eBattQueryInformation(PVOID Context, ULONG BatteryTag, BATTERY_QUERY_INFORMATION_LEVEL Level,
                                  LONG AtRate, PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    PS10E_BATT_FDO Fdo = (PS10E_BATT_FDO)Context;
    ULONG          Value;
    PVOID          Ret    = NULL;
    size_t         RetLen = 0;
    NTSTATUS       Status;

    PAGED_CODE();
    *ReturnedLength = 0;
    WdfWaitLockAcquire(Fdo->StateLock, NULL);
    if ((BatteryTag != Fdo->BatteryTag) || (BatteryTag == BATTERY_TAG_INVALID)) {
        Status = STATUS_NO_SUCH_DEVICE;
        goto End;
    }

    switch (Level) {
    case BatteryInformation:
        Ret    = &Fdo->Info;
        RetLen = sizeof(Fdo->Info);
        Status = STATUS_SUCCESS;
        break;

    case BatteryEstimatedTime:
        /* Seconds of runtime at AtRate (or the current rate) from the remaining energy; unknown unless
           discharging. */
        if (AtRate == 0) {
            AtRate = ((Fdo->Status.PowerState & BATTERY_DISCHARGING) != 0) ? Fdo->Status.Rate : 0;
        }
        if ((AtRate < 0) && (AtRate != (LONG)BATTERY_UNKNOWN_RATE)) {
            Value = (ULONG)(((ULONGLONG)Fdo->Status.Capacity * 3600) / (ULONG)(-AtRate));
        } else {
            Value = BATTERY_UNKNOWN_TIME;
        }
        Ret    = &Value;
        RetLen = sizeof(Value);
        Status = STATUS_SUCCESS;
        break;

    case BatteryTemperature:
        Value  = Fdo->TemperatureDeciK;
        Ret    = &Value;
        RetLen = sizeof(Value);
        Status = STATUS_SUCCESS;
        break;

    case BatteryDeviceName:
        Status = CopyString(S10E_BATT_DEVICE_NAME, &Ret, &RetLen);
        break;

    case BatteryManufactureName:
        Status = CopyString(S10E_BATT_MANUFACTURER, &Ret, &RetLen);
        break;

    case BatterySerialNumber:
        Status = CopyString(S10E_BATT_SERIAL, &Ret, &RetLen);
        break;

    case BatteryUniqueID:
        Status = CopyString(S10E_BATT_UNIQUE_ID, &Ret, &RetLen);
        break;

    case BatteryGranularityInformation:
    case BatteryManufactureDate:
    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (NT_SUCCESS(Status)) {
        *ReturnedLength = (ULONG)RetLen;
        if ((Buffer == NULL) || (BufferLength < RetLen)) {
            Status = STATUS_BUFFER_TOO_SMALL;
        } else {
            RtlCopyMemory(Buffer, Ret, RetLen);
        }
    }

End:
    WdfWaitLockRelease(Fdo->StateLock);
    return Status;
}

_Use_decl_annotations_
NTSTATUS S10eBattQueryStatus(PVOID Context, ULONG BatteryTag, PBATTERY_STATUS BatteryStatus)
{
    PS10E_BATT_FDO Fdo = (PS10E_BATT_FDO)Context;
    NTSTATUS       Status;

    PAGED_CODE();
    WdfWaitLockAcquire(Fdo->StateLock, NULL);
    if ((BatteryTag != Fdo->BatteryTag) || (BatteryTag == BATTERY_TAG_INVALID)) {
        Status = STATUS_NO_SUCH_DEVICE;
    } else {
        RtlCopyMemory(BatteryStatus, &Fdo->Status, sizeof(BATTERY_STATUS));
        Fdo->QueryStatusCount++;
        Fdo->QueriedPowerState = Fdo->Status.PowerState;
        Status = STATUS_SUCCESS;
    }
    WdfWaitLockRelease(Fdo->StateLock);
    return Status;
}

/* Read-only driver: no charge control, no critical bias. */
_Use_decl_annotations_
NTSTATUS S10eBattSetInformation(PVOID Context, ULONG BatteryTag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buffer);
    PAGED_CODE();
    return STATUS_NOT_SUPPORTED;
}

/* No hardware alarm: the class driver polls QueryStatus (as with simbatt); we also notify on every change the
   poll sees. */
_Use_decl_annotations_
NTSTATUS S10eBattSetStatusNotify(PVOID Context, ULONG BatteryTag, PBATTERY_NOTIFY BatteryNotify)
{
    PS10E_BATT_FDO Fdo = (PS10E_BATT_FDO)Context;
    NTSTATUS       Status;

    UNREFERENCED_PARAMETER(BatteryNotify);
    PAGED_CODE();
    WdfWaitLockAcquire(Fdo->StateLock, NULL);
    Status = ((BatteryTag != Fdo->BatteryTag) || (BatteryTag == BATTERY_TAG_INVALID)) ? STATUS_NO_SUCH_DEVICE
                                                                                         : STATUS_NOT_SUPPORTED;
    WdfWaitLockRelease(Fdo->StateLock);
    return Status;
}

_Use_decl_annotations_
NTSTATUS S10eBattDisableStatusNotify(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    PAGED_CODE();
    return STATUS_NOT_SUPPORTED;
}
