/*
 * ReqLog.c - A6 diagnostics: every KS request (property / method / event / create) that reaches the render circuit or
 * one of its streams is logged with its parameters and the status ACX returned for it.
 *
 * Why: run audA5 - the endpoint builder probes the host pin (create, RT buffer, HW latency, close) and then every
 * AudioSrv use of the endpoint fails with 0x80070032 (ERROR_NOT_SUPPORTED <- STATUS_NOT_SUPPORTED) without a further
 * pin create. The failing KS request is invisible from the stream callbacks; ACX's request preprocess hook
 * (AcxCircuitInitAssignAcxRequestPreprocessCallback / AcxStreamInitAssignAcxRequestPreprocessCallback, the mechanism
 * of the ACX sample's Common/CircuitHelper.cpp) sees each one before ACX handles it. The driver only observes: it
 * copies the parameters, hands the request back to ACX (Acx{Circuit,Stream}DispatchAcxRequest, exactly once) and
 * logs the returned status (STATUS_PENDING would mean ACX completes it later). notes/d8-audio-win-a5.md.
 *
 * Volume: capped at AUD_REQLOG_MAX lines per device; the file is flushed every AUD_REQLOG_FLUSH lines and after every
 * error status, so the last lines before a failure (or a crash) are on disk.
 */

#include "Exynos9820Audio.h"

#define AUD_REQLOG_MAX    2000u
#define AUD_REQLOG_FLUSH  32u

typedef struct _AUD_SET_NAME {
    ULONG Data1;
    PCSTR Name;
} AUD_SET_NAME;

/* Data1 of the KS sets (WDK 10.0.26100 ks.h / ksmedia.h DEFINE_GUIDSTRUCT); the full GUID is logged as well. */
static const AUD_SET_NAME AudSetNames[] = {
    { 0x8C134960, "Pin" },            { 0x720D4AC0, "Topology" },       { 0x1D58C920, "Connection" },
    { 0x1464EDA5, "General" },        { 0x65AABA60, "Stream" },         { 0x45FFAAA0, "Audio" },
    { 0xA855A48C, "RtAudio" },        { 0x4509F757, "Jack" },           { 0x3A2F82DC, "AudioEngine" },
    { 0x4F67B528, "AudioSignalProc" },{ 0xC034FDB0, "AudioModule" },    { 0xA3FB7B0D, "AudioPosture" },
    { 0xD0B305E1, "AudioResMgmt" },   { 0xE9EBE550, "InterleavedAudio" },{ 0x113C425E, "SoundDetector" },
    { 0x2F2C8DDD, "DrmAudioStream" }, { 0x4E73C07F, "AudioBufDuration" },{ 0xB3648BC8, "AudioLoopback" },
    { 0xE85E9698, "EvAudioCtlChange" },{ 0xDD4F192E, "EvPinCapsChange" },{ 0x7F4BCBE0, "EvConnection" },
    { 0x162AC456, "EvDynFormatChange" },{ 0x45FFAAA1, "TopologyNode" }, { 0x924E54B0, "Wave" },
    { 0xDF12A4C0, "Clock" },          { 0x7FA06C40, "BtAudio" },        { 0xB6DF7EB1, "TelephonyControl" },
};

static PCSTR AudSetName(_In_ const GUID *Set)
{
    ULONG I;

    for (I = 0; I < ARRAYSIZE(AudSetNames); I++) {
        if (AudSetNames[I].Data1 == Set->Data1) {
            return AudSetNames[I].Name;
        }
    }
    return "?";
}

static PCSTR AudVerbName(_In_ ACX_REQUEST_TYPE Type, _In_ ULONG Verb)
{
    static const PCSTR Prop[] = { "none", "get", "set", "basic", "setsup" };
    static const PCSTR Meth[] = { "none", "send", "basic", "setsup" };
    static const PCSTR Evt[]  = { "none", "enable", "disable", "basic", "setsup" };

    switch (Type) {
    case AcxRequestTypeProperty: return (Verb < ARRAYSIZE(Prop)) ? Prop[Verb] : "?";
    case AcxRequestTypeMethod:   return (Verb < ARRAYSIZE(Meth)) ? Meth[Verb] : "?";
    case AcxRequestTypeEvent:    return (Verb < ARRAYSIZE(Evt)) ? Evt[Verb] : "?";
    default:                     return "-";
    }
}

static PCSTR AudItemName(_In_ ACX_ITEM_TYPE Item)
{
    switch (Item) {
    case AcxItemTypeDefault: return "obj";
    case AcxItemTypePin:     return "pin";
    case AcxItemTypeElement: return "elem";
    default:                 return "?";
    }
}

/* Log one request after ACX handled it. P is the caller's copy of the parameters (the request may be gone). */
static VOID AudReqLog(_In_ PAUD_DEVICE_CONTEXT Dev, _In_ CHAR Who, _In_ const ACX_REQUEST_PARAMETERS *P, _In_ NTSTATUS Status)
{
    ULONG N;

    if (Dev == NULL) {
        return;
    }

    N = (ULONG)InterlockedIncrement((volatile LONG *)&Dev->ReqLogged);
    if (N > AUD_REQLOG_MAX) {
        if (N == AUD_REQLOG_MAX + 1) {
            AudLog(Dev->Log, "req: cap of %u lines reached - further requests not logged\r\n", AUD_REQLOG_MAX);
            AudLogFlush(Dev->Log);
        }
        return;
    }

    switch (P->Type) {
    case AcxRequestTypeProperty:
        AudLog(Dev->Log, "req %c #%lu prop %s.%lu %s %s#%ld ctl %lu val %lu {%08lX-%04X-%04X} -> %08lX\r\n", Who, N,
               AudSetName(&P->Parameters.Property.Set), P->Parameters.Property.Id,
               AudVerbName(P->Type, (ULONG)P->Parameters.Property.Verb), AudItemName(P->Parameters.Property.ItemType),
               (LONG)P->Parameters.Property.ItemId, P->Parameters.Property.ControlCb, P->Parameters.Property.ValueCb,
               P->Parameters.Property.Set.Data1, P->Parameters.Property.Set.Data2, P->Parameters.Property.Set.Data3,
               Status);
        break;
    case AcxRequestTypeMethod:
        AudLog(Dev->Log, "req %c #%lu method %s.%lu %s %s#%ld args %lu res %lu {%08lX} -> %08lX\r\n", Who, N,
               AudSetName(&P->Parameters.Method.Set), P->Parameters.Method.Id,
               AudVerbName(P->Type, (ULONG)P->Parameters.Method.Verb), AudItemName(P->Parameters.Method.ItemType),
               (LONG)P->Parameters.Method.ItemId, P->Parameters.Method.ArgsCb, P->Parameters.Method.ResultCb,
               P->Parameters.Method.Set.Data1, Status);
        break;
    case AcxRequestTypeEvent:
        AudLog(Dev->Log, "req %c #%lu event %s.%lu %s %s#%ld data %lu {%08lX} -> %08lX\r\n", Who, N,
               AudSetName(&P->Parameters.Event.Set), P->Parameters.Event.Id,
               AudVerbName(P->Type, (ULONG)P->Parameters.Event.Verb), AudItemName(P->Parameters.Event.ItemType),
               (LONG)P->Parameters.Event.ItemId, P->Parameters.Event.DataCb, P->Parameters.Event.Set.Data1, Status);
        break;
    case AcxRequestTypeCreate:
        AudLog(Dev->Log, "req %c #%lu create ctl %lu -> %08lX\r\n", Who, N, P->Parameters.Create.ControlCb, Status);
        break;
    default:
        AudLog(Dev->Log, "req %c #%lu type %d major %u minor %u -> %08lX\r\n", Who, N, (int)P->Type,
               (unsigned)P->MajorFunction, (unsigned)P->MinorFunction, Status);
        break;
    }

    if (NT_ERROR(Status) || (N % AUD_REQLOG_FLUSH) == 0) {
        AudLogFlush(Dev->Log);
    }
}

_Use_decl_annotations_
VOID NTAPI AudEvtCircuitRequestPreprocess(ACXOBJECT Object, ACXCONTEXT DriverContext, WDFREQUEST Request)
{
    ACX_REQUEST_PARAMETERS P;
    NTSTATUS               Status;

    PAGED_CODE();

    ACX_REQUEST_PARAMETERS_INIT(&P);
    AcxRequestGetParameters(Request, &P);
    Status = AcxCircuitDispatchAcxRequest((ACXCIRCUIT)Object, Request);   /* the request belongs to ACX from here on */
    AudReqLog((PAUD_DEVICE_CONTEXT)DriverContext, 'C', &P, Status);
}

_Use_decl_annotations_
VOID NTAPI AudEvtStreamRequestPreprocess(ACXOBJECT Object, ACXCONTEXT DriverContext, WDFREQUEST Request)
{
    ACX_REQUEST_PARAMETERS P;
    NTSTATUS               Status;

    PAGED_CODE();

    ACX_REQUEST_PARAMETERS_INIT(&P);
    AcxRequestGetParameters(Request, &P);
    Status = AcxStreamDispatchAcxRequest((ACXSTREAM)Object, Request);
    AudReqLog((PAUD_DEVICE_CONTEXT)DriverContext, 'S', &P, Status);
}
