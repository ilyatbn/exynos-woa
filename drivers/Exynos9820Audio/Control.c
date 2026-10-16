/*
 * Control.c - speaker trim property (AudControl.h) for the S10e speaker app.
 */
#include "Exynos9820Audio.h"
#include "AudControl.h"

static const GUID AudPropSetSpeakerTrim = AUDPROPSETID_SPEAKERTRIM_INIT;

#define AUD_TRIM_ATTEN_MAX  816u   /* CS35L40 lowest non-mute step (-102 dB) */
#define AUD_TRIM_GAIN_MAX   17u    /* AmpI2c.c AMP_GAIN_MAX, the stock music value */

static VOID AudTrimPersist(_In_ PAUD_DEVICE_CONTEXT Dev)
{
    static const PCWSTR Names[4] = { L"RcvAtten", L"SpkAtten", L"RcvGain", L"SpkGain" };
    const ULONG         Values[4] = { Dev->Cfg.RcvAtten, Dev->Cfg.SpkAtten, Dev->Cfg.RcvGain, Dev->Cfg.SpkGain };
    WDFKEY              Key;
    UNICODE_STRING      Name;
    ULONG               I;
    NTSTATUS            Status;

    Status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &Key);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "trim: Parameters key not writable %08lX\r\n", (ULONG)Status);
        return;
    }
    for (I = 0; I < ARRAYSIZE(Names); I++) {
        RtlInitUnicodeString(&Name, Names[I]);
        (VOID)WdfRegistryAssignULong(Key, &Name, Values[I]);
    }
    WdfRegistryClose(Key);
}

/* TRUE = the request was ours and is completed. PASSIVE_LEVEL (circuit preprocess). */
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN AudControlRequest(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ WDFREQUEST Request, _In_ const ACX_REQUEST_PARAMETERS *P)
{
    NTSTATUS  Status;
    ULONG_PTR Info = 0;
    PAUD_TRIM T = (PAUD_TRIM)P->Parameters.Property.Value;
    ULONG     Cb = P->Parameters.Property.ValueCb;

    PAGED_CODE();

    if (Dev == NULL || P->Type != AcxRequestTypeProperty ||
        RtlCompareMemory(&P->Parameters.Property.Set, &AudPropSetSpeakerTrim, sizeof(GUID)) != sizeof(GUID)) {
        return FALSE;
    }

    if (P->Parameters.Property.Id != AUD_PROPERTY_TRIM) {
        Status = STATUS_NOT_FOUND;
    } else if (P->Parameters.Property.Verb == AcxPropertyVerbBasicSupport) {
        if (T != NULL && Cb >= sizeof(ULONG)) {
            *(PULONG)T = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
            Info = sizeof(ULONG);
            Status = STATUS_SUCCESS;
        } else {
            Info = sizeof(ULONG);
            Status = (Cb == 0) ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
        }
    } else if (T == NULL || Cb < sizeof(AUD_TRIM)) {
        Info = sizeof(AUD_TRIM);
        Status = (Cb == 0) ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
    } else if (P->Parameters.Property.Verb == AcxPropertyVerbGet) {
        T->RcvAtten = Dev->Cfg.RcvAtten;
        T->SpkAtten = Dev->Cfg.SpkAtten;
        T->RcvGain  = Dev->Cfg.RcvGain;
        T->SpkGain  = Dev->Cfg.SpkGain;
        Info = sizeof(AUD_TRIM);
        Status = STATUS_SUCCESS;
    } else if (P->Parameters.Property.Verb == AcxPropertyVerbSet) {
        WdfWaitLockAcquire(Dev->BusLock, NULL);   /* the amp sequences read these under BusLock */
        Dev->Cfg.RcvAtten = min(T->RcvAtten, AUD_TRIM_ATTEN_MAX);
        Dev->Cfg.SpkAtten = min(T->SpkAtten, AUD_TRIM_ATTEN_MAX);
        Dev->Cfg.RcvGain  = min(T->RcvGain, AUD_TRIM_GAIN_MAX);
        Dev->Cfg.SpkGain  = min(T->SpkGain, AUD_TRIM_GAIN_MAX);
        WdfWaitLockRelease(Dev->BusLock);
        AudTrimPersist(Dev);
        Status = AmpApplyTrim(Dev);
    } else {
        Status = STATUS_NOT_SUPPORTED;
    }

    WdfRequestCompleteWithInformation(Request, Status, Info);
    return TRUE;
}
