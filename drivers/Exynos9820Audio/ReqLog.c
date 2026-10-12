/*
 * ReqLog.c - A6/A7 diagnostics: every KS request (property / method / event / create) that reaches the render circuit
 * or one of its streams is logged with its parameters.
 *
 * Why: run audA5 - the endpoint builder probes the host pin (create, RT buffer, HW latency, close) and then every
 * AudioSrv use of the endpoint fails with 0x80070032 (ERROR_NOT_SUPPORTED <- STATUS_NOT_SUPPORTED) without a further
 * pin create. The failing KS request is invisible from the stream callbacks; ACX's request preprocess hook
 * (AcxCircuitInitAssignAcxRequestPreprocessCallback / AcxStreamInitAssignAcxRequestPreprocessCallback, the mechanism
 * of the ACX sample's Common/CircuitHelper.cpp) sees each one before ACX handles it. The driver copies the parameters,
 * hands the request back to ACX (Acx{Circuit,Stream}DispatchAcxRequest, exactly once) and logs what that call
 * returned. NOTE (run audA6u): that is the dispatch result, not the request's completion status - size queries that
 * complete with STATUS_BUFFER_OVERFLOW also show 00000000. Lines are tagged "disp" for that reason.
 *
 * Volume (A7): per request key (set, id, verb, item type, item id) the first AUD_REQKEY_FIRST occurrences are logged,
 * then every AUD_REQKEY_EVERY-th with its running count; the format carried by every PROPOSEDATAFORMAT set on a pin is
 * decoded once per DISTINCT format (A8; up to AUD_FMTLOG_MAX distinct) with its verdict. The file is flushed every AUD_REQLOG_FLUSH logged lines and after error statuses.
 * A7 also answers two host-pin format properties itself (see "driver-completed host-pin format requests" below).
 * notes/d8-audio-win-a5.md.
 */

#include "Exynos9820Audio.h"

#define AUD_REQLOG_MAX    2000u   /* logged request lines per device */
#define AUD_REQLOG_FLUSH  32u
#define AUD_REQKEY_FIRST  6u
#define AUD_REQKEY_EVERY  100u
#define AUD_FMTLOG_MAX    AUD_FMTHASHES   /* distinct proposed formats decoded (Exynos9820Audio.h) */

#define AUD_KSPROPERTY_PIN_PROPOSEDATAFORMAT  14u   /* ks.h KSPROPERTY_PIN_PROPOSEDATAFORMAT */

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

/* The key fields of a request (set Data1, id, verb, item type/id); Create and unknown types use set 0. */
static VOID AudReqKey(_In_ const ACX_REQUEST_PARAMETERS *P, _Out_ PAUD_REQKEY K)
{
    RtlZeroMemory(K, sizeof(*K));
    K->Type = (UCHAR)P->Type;
    switch (P->Type) {
    case AcxRequestTypeProperty:
        K->Data1 = P->Parameters.Property.Set.Data1;  K->Id = P->Parameters.Property.Id;
        K->Verb = (UCHAR)P->Parameters.Property.Verb; K->Item = (UCHAR)P->Parameters.Property.ItemType;
        K->ItemId = P->Parameters.Property.ItemId;
        break;
    case AcxRequestTypeMethod:
        K->Data1 = P->Parameters.Method.Set.Data1;    K->Id = P->Parameters.Method.Id;
        K->Verb = (UCHAR)P->Parameters.Method.Verb;   K->Item = (UCHAR)P->Parameters.Method.ItemType;
        K->ItemId = P->Parameters.Method.ItemId;
        break;
    case AcxRequestTypeEvent:
        K->Data1 = P->Parameters.Event.Set.Data1;     K->Id = P->Parameters.Event.Id;
        K->Verb = (UCHAR)P->Parameters.Event.Verb;    K->Item = (UCHAR)P->Parameters.Event.ItemType;
        K->ItemId = P->Parameters.Event.ItemId;
        break;
    default:
        break;
    }
}

/* Running count of this key (1 = first time); 0 when the table is full (then every request is logged). */
static ULONG AudReqKeyCount(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ const AUD_REQKEY *K)
{
    KIRQL OldIrql;
    ULONG I;
    ULONG Count = 0;

    KeAcquireSpinLock(&Dev->ReqLock, &OldIrql);
    for (I = 0; I < Dev->ReqKeyCount; I++) {
        PAUD_REQKEY E = &Dev->ReqKeys[I];

        if (E->Data1 == K->Data1 && E->Id == K->Id && E->Type == K->Type && E->Verb == K->Verb && E->Item == K->Item &&
            E->ItemId == K->ItemId) {
            Count = ++E->Count;
            break;
        }
    }
    if (I == Dev->ReqKeyCount && I < AUD_REQKEYS) {
        Dev->ReqKeys[I] = *K;
        Dev->ReqKeys[I].Count = 1;
        Dev->ReqKeyCount++;
        Count = 1;
    }
    KeReleaseSpinLock(&Dev->ReqLock, OldIrql);
    return Count;
}

/* TRUE the first time this exact format (size + bytes, FNV-1a) is seen; FALSE afterwards or when the table is full. */
static BOOLEAN AudFmtIsNew(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_reads_bytes_(Cb) const UCHAR *Data, _In_ ULONG Cb)
{
    ULONG   Hash = 2166136261u ^ Cb;
    ULONG   I;
    KIRQL   OldIrql;
    BOOLEAN New = FALSE;

    for (I = 0; I < Cb && I < 256u; I++) {
        Hash = (Hash ^ Data[I]) * 16777619u;
    }

    KeAcquireSpinLock(&Dev->ReqLock, &OldIrql);
    for (I = 0; I < Dev->FmtHashCount; I++) {
        if (Dev->FmtHash[I] == Hash) {
            break;
        }
    }
    if (I == Dev->FmtHashCount && I < AUD_FMTHASHES) {
        Dev->FmtHash[I] = Hash;
        Dev->FmtHashCount++;
        New = TRUE;
    }
    KeReleaseSpinLock(&Dev->ReqLock, OldIrql);
    return New;
}

/* Decode the KSDATAFORMAT (+ WAVEFORMATEX[TENSIBLE]) of a PROPOSEDATAFORMAT set, once per distinct format, while the
   buffer is still valid (before ACX or the driver completes the request). Verdict = who answers / what. */
static VOID AudLogProposedFormat(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ CHAR Who, _In_ const ACX_REQUEST_PARAMETERS *P,
                                 _In_ PCSTR Verdict)
{
    const KSDATAFORMAT *F = (const KSDATAFORMAT *)P->Parameters.Property.Value;
    ULONG              Cb = P->Parameters.Property.ValueCb;
    ULONG              N;

    if (F == NULL || Cb < sizeof(KSDATAFORMAT) || !AudFmtIsNew(Dev, (const UCHAR *)F, Cb)) {
        return;
    }
    N = (ULONG)InterlockedIncrement(&Dev->FmtLogged);
    AudLog(Dev->Log, "fmt %c #%lu verdict: %s\r\n", Who, N, Verdict);

    if (Cb >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEX *W = (const WAVEFORMATEX *)(F + 1);

        if (W->wFormatTag == WAVE_FORMAT_EXTENSIBLE && Cb >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) {
            const WAVEFORMATEXTENSIBLE *X = (const WAVEFORMATEXTENSIBLE *)W;

            AudLog(Dev->Log, "fmt %c #%lu %s pin#%lu: size %lu flags %lX sub {%08lX} spec {%08lX} EXT %u ch %lu Hz %u/%u bit "
                   "align %u avg %lu mask %lX sub {%08lX}\r\n", Who, N, AudVerbName(P->Type, (ULONG)P->Parameters.Property.Verb),
                   P->Parameters.Property.ItemId, F->FormatSize, F->Flags, F->SubFormat.Data1, F->Specifier.Data1,
                   W->nChannels, W->nSamplesPerSec, W->wBitsPerSample, X->Samples.wValidBitsPerSample, W->nBlockAlign,
                   W->nAvgBytesPerSec, X->dwChannelMask, X->SubFormat.Data1);
        } else {
            AudLog(Dev->Log, "fmt %c #%lu %s pin#%lu: size %lu flags %lX sub {%08lX} spec {%08lX} tag %X %u ch %lu Hz %u bit "
                   "align %u avg %lu cb %u\r\n", Who, N, AudVerbName(P->Type, (ULONG)P->Parameters.Property.Verb),
                   P->Parameters.Property.ItemId, F->FormatSize, F->Flags, F->SubFormat.Data1, F->Specifier.Data1,
                   W->wFormatTag, W->nChannels, W->nSamplesPerSec, W->wBitsPerSample, W->nBlockAlign, W->nAvgBytesPerSec,
                   W->cbSize);
        }
    } else {
        AudLog(Dev->Log, "fmt %c #%lu %s pin#%lu: size %lu flags %lX major {%08lX} sub {%08lX} spec {%08lX} (no wave format)\r\n",
               Who, N, AudVerbName(P->Type, (ULONG)P->Parameters.Property.Verb), P->Parameters.Property.ItemId,
               F->FormatSize, F->Flags, F->MajorFormat.Data1, F->SubFormat.Data1, F->Specifier.Data1);
    }
}

/* Log one request after ACX (or the driver, Who in lower case) handled it. P is the caller's copy of the parameters.
   Tag: "disp" = Acx*DispatchAcxRequest's return (not the completion status), "done" = completed by the driver. */
static VOID AudReqLog(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ CHAR Who, _In_ const ACX_REQUEST_PARAMETERS *P, _In_ PCSTR Tag,
               _In_ NTSTATUS Status, _In_ ULONG_PTR Info)
{
    AUD_REQKEY K;
    ULONG      Count;
    ULONG      N;

    if (Dev == NULL) {
        return;
    }

    AudReqKey(P, &K);
    Count = AudReqKeyCount(Dev, &K);
    if (Count > AUD_REQKEY_FIRST && (Count % AUD_REQKEY_EVERY) != 0 && !NT_ERROR(Status)) {
        return;   /* repetitive request: counted, not logged */
    }

    N = (ULONG)InterlockedIncrement(&Dev->ReqLogged);
    if (N > AUD_REQLOG_MAX) {
        if (N == AUD_REQLOG_MAX + 1) {
            AudLog(Dev->Log, "req: cap of %u lines reached - further requests not logged\r\n", AUD_REQLOG_MAX);
            AudLogFlush(Dev->Log);
        }
        return;
    }

    switch (P->Type) {
    case AcxRequestTypeProperty:
        AudLog(Dev->Log, "req %c x%lu prop %s.%lu %s %s#%ld ctl %lu val %lu {%08lX-%04X-%04X} %s %08lX info %Iu\r\n", Who, Count,
               AudSetName(&P->Parameters.Property.Set), P->Parameters.Property.Id,
               AudVerbName(P->Type, (ULONG)P->Parameters.Property.Verb), AudItemName(P->Parameters.Property.ItemType),
               (LONG)P->Parameters.Property.ItemId, P->Parameters.Property.ControlCb, P->Parameters.Property.ValueCb,
               P->Parameters.Property.Set.Data1, P->Parameters.Property.Set.Data2, P->Parameters.Property.Set.Data3,
               Tag, Status, Info);
        break;
    case AcxRequestTypeMethod:
        AudLog(Dev->Log, "req %c x%lu method %s.%lu %s %s#%ld args %lu res %lu {%08lX} %s %08lX\r\n", Who, Count,
               AudSetName(&P->Parameters.Method.Set), P->Parameters.Method.Id,
               AudVerbName(P->Type, (ULONG)P->Parameters.Method.Verb), AudItemName(P->Parameters.Method.ItemType),
               (LONG)P->Parameters.Method.ItemId, P->Parameters.Method.ArgsCb, P->Parameters.Method.ResultCb,
               P->Parameters.Method.Set.Data1, Tag, Status);
        break;
    case AcxRequestTypeEvent:
        AudLog(Dev->Log, "req %c x%lu event %s.%lu %s %s#%ld data %lu {%08lX} %s %08lX\r\n", Who, Count,
               AudSetName(&P->Parameters.Event.Set), P->Parameters.Event.Id,
               AudVerbName(P->Type, (ULONG)P->Parameters.Event.Verb), AudItemName(P->Parameters.Event.ItemType),
               (LONG)P->Parameters.Event.ItemId, P->Parameters.Event.DataCb, P->Parameters.Event.Set.Data1, Tag, Status);
        break;
    case AcxRequestTypeCreate:
        AudLog(Dev->Log, "req %c x%lu create ctl %lu %s %08lX\r\n", Who, Count, P->Parameters.Create.ControlCb, Tag, Status);
        break;
    default:
        AudLog(Dev->Log, "req %c x%lu type %d major %u minor %u %s %08lX\r\n", Who, Count, (int)P->Type,
               (unsigned)P->MajorFunction, (unsigned)P->MinorFunction, Tag, Status);
        break;
    }

    if (NT_ERROR(Status) || (N % AUD_REQLOG_FLUSH) == 0) {
        AudLogFlush(Dev->Log);
    }
}

/* Called before a request goes to ACX: decode what is worth decoding while the buffers are still ours to read. */
static VOID AudReqInspect(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ CHAR Who, _In_ const ACX_REQUEST_PARAMETERS *P)
{
    if (Dev != NULL && P->Type == AcxRequestTypeProperty && P->Parameters.Property.Set.Data1 == 0x8C134960 &&
        P->Parameters.Property.Id == AUD_KSPROPERTY_PIN_PROPOSEDATAFORMAT &&
        P->Parameters.Property.Verb == AcxPropertyVerbSet && P->Parameters.Property.ItemType == AcxItemTypePin) {
        AudLogProposedFormat(Dev, Who, P, "to ACX (answer not visible)");
    }
}

/* ================================================================================================================
 * A7: driver-completed host-pin format requests (Parameters\KsOverride = 1, default).
 *
 * Run audA6u: every endpoint-builder round ends with KSPROPERTY_PIN_DATAINTERSECTION on the host pin (input: our own
 * range list, 184 B; output size query) and never a sized follow-up - the intersection fails inside ACX, the engine
 * gets no device format, AudioSrv rejects the endpoint (0x80070032). The KS contract of the two pin properties is
 * small and fully specified, so the driver answers them for the host pin itself:
 *  - DATAINTERSECTION get: walk the KSMULTIPLE_ITEM of KSDATARANGEs (8-byte aligned, KSDATARANGE_ATTRIBUTES ranges
 *    followed by their attribute list); the first range admitting 48 kHz / 2 ch / 16 bit (else 32 bit) PCM yields
 *    the matching format of the pin's list (Render.c AudFormatPcm16/32). Output length 0 = size query:
 *    STATUS_BUFFER_OVERFLOW + Information = size (the convention of the ACX sample's CircuitHelper.cpp:540-545);
 *    too small = STATUS_BUFFER_TOO_SMALL; no range = STATUS_NO_MATCH.
 *  - PROPOSEDATAFORMAT set with a wave format: STATUS_SUCCESS for 48 kHz stereo 16/32-bit integer PCM
 *    (WAVEFORMATEX PCM or EXTENSIBLE/PCM with valid bits = container, mask 0 or stereo), else STATUS_NO_MATCH.
 * Everything else (other pins, other verbs, formats without a wave header) still goes to ACX.
 * ================================================================================================================ */

#define AUD_KSPROPERTY_PIN_DATAINTERSECTION   4u    /* ks.h KSPROPERTY_PIN_DATAINTERSECTION */

static const GUID AudGuidNull = { 0 };

static BOOLEAN AudGuidIs(_In_ const GUID *A, _In_ const GUID *B)
{
    return (RtlCompareMemory(A, B, sizeof(GUID)) == sizeof(GUID)) ? TRUE : FALSE;
}

static BOOLEAN AudGuidIsOrWild(_In_ const GUID *A, _In_ const GUID *B)
{
    return AudGuidIs(A, B) || AudGuidIs(A, &AudGuidNull);
}

/* Bits (16 or 32) one range admits for 48 kHz stereo PCM, 0 if none. */
static ULONG AudRangeBits(_In_ const KSDATARANGE *R)
{
    const KSDATARANGE_AUDIO *A = (const KSDATARANGE_AUDIO *)R;

    if (!AudGuidIsOrWild(&R->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) ||
        !AudGuidIsOrWild(&R->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
        return 0;
    }
    if (AudGuidIs(&R->Specifier, &AudGuidNull)) {
        return 16;                                   /* full wildcard */
    }
    if (!AudGuidIs(&R->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) || R->FormatSize < sizeof(KSDATARANGE_AUDIO)) {
        return 0;
    }
    if (A->MaximumChannels != (ULONG)-1 && A->MaximumChannels < AUD_CHANNELS) {
        return 0;
    }
    if (A->MinimumSampleFrequency > AUD_RATE || A->MaximumSampleFrequency < AUD_RATE) {
        return 0;
    }
    if (A->MinimumBitsPerSample <= 16 && A->MaximumBitsPerSample >= 16) {
        return 16;
    }
    if (A->MinimumBitsPerSample <= 32 && A->MaximumBitsPerSample >= 32) {
        return 32;
    }
    return 0;
}

static NTSTATUS AudIntersect(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ const ACX_REQUEST_PARAMETERS *P, _Out_ PULONG_PTR Info)
{
    const KSMULTIPLE_ITEM *Item = (const KSMULTIPLE_ITEM *)P->Parameters.Property.Control;
    const UCHAR           *Cur;
    const UCHAR           *End;
    const KSDATARANGE     *R;
    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *Out = NULL;
    ULONG                  I;
    ULONG                  Bits = 0;

    *Info = 0;
    if (Item == NULL || P->Parameters.Property.ControlCb < sizeof(KSMULTIPLE_ITEM) ||
        Item->Size < sizeof(KSMULTIPLE_ITEM) || Item->Size > P->Parameters.Property.ControlCb) {
        AudLog(Dev->Log, "ks: intersection control %lu B not a range list (size %lu)\r\n", P->Parameters.Property.ControlCb,
               (Item != NULL && P->Parameters.Property.ControlCb >= sizeof(ULONG)) ? Item->Size : 0);
        return STATUS_INVALID_PARAMETER;
    }

    Cur = (const UCHAR *)(Item + 1);
    End = (const UCHAR *)Item + Item->Size;
    for (I = 0; I < Item->Count && Bits == 0; I++) {
        /* Cur may step past End by the 8-byte alignment of the last item: test before the subtraction (review R4). */
        if (Cur >= End || (SIZE_T)(End - Cur) < sizeof(KSDATARANGE)) {
            break;
        }
        R = (const KSDATARANGE *)Cur;
        if (R->FormatSize < sizeof(KSDATARANGE) || R->FormatSize > (ULONG)(End - Cur)) {
            break;
        }
        Bits = AudRangeBits(R);
        AudLog(Dev->Log, "ks: intersection range %lu: size %lu flags %lX sub {%08lX} spec {%08lX} -> %lu bit\r\n", I,
               R->FormatSize, R->Flags, R->SubFormat.Data1, R->Specifier.Data1, Bits);
        Cur += (R->FormatSize + 7u) & ~7u;
        if ((R->Flags & KSDATARANGE_ATTRIBUTES) != 0 && Cur < End && (SIZE_T)(End - Cur) >= sizeof(KSMULTIPLE_ITEM)) {
            const KSMULTIPLE_ITEM *Attr = (const KSMULTIPLE_ITEM *)Cur;   /* the range's attribute list */

            if (Attr->Size < sizeof(KSMULTIPLE_ITEM) || Attr->Size > (ULONG)(End - Cur)) {
                break;
            }
            Cur += (Attr->Size + 7u) & ~7u;
        }
    }

    if (Bits == 0) {
        return STATUS_NO_MATCH;
    }
    Out = AudHostFormat(Bits);

    if (P->Parameters.Property.ValueCb == 0) {
        *Info = sizeof(*Out);
        return STATUS_BUFFER_OVERFLOW;
    }
    if (P->Parameters.Property.ValueCb < sizeof(*Out) || P->Parameters.Property.Value == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(P->Parameters.Property.Value, Out, sizeof(*Out));
    *Info = sizeof(*Out);
    return STATUS_SUCCESS;
}

/* PROPOSEDATAFORMAT set: TRUE = handled (Status set), FALSE = not a wave format, leave it to ACX. */
static BOOLEAN AudPropose(_In_ const ACX_REQUEST_PARAMETERS *P, _Out_ NTSTATUS *Status)
{
    const KSDATAFORMAT *F = (const KSDATAFORMAT *)P->Parameters.Property.Value;
    ULONG              Cb = P->Parameters.Property.ValueCb;
    const WAVEFORMATEX *W;
    ULONG              Bits;

    *Status = STATUS_NO_MATCH;
    if (F == NULL || Cb < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX) ||
        !AudGuidIs(&F->Specifier, &KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return FALSE;
    }
    W = (const WAVEFORMATEX *)(F + 1);
    Bits = W->wBitsPerSample;

    if (!AudGuidIs(&F->MajorFormat, &KSDATAFORMAT_TYPE_AUDIO) || !AudGuidIs(&F->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
        W->nChannels != AUD_CHANNELS || W->nSamplesPerSec != AUD_RATE || (Bits != 16 && Bits != 32) ||
        W->nBlockAlign != AUD_CHANNELS * (Bits / 8) || W->nAvgBytesPerSec != AUD_RATE * W->nBlockAlign) {
        return TRUE;
    }
    if (W->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *X = (const WAVEFORMATEXTENSIBLE *)W;

        if (Cb < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) || !AudGuidIs(&X->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM) ||
            X->Samples.wValidBitsPerSample != Bits || (X->dwChannelMask != 0 && X->dwChannelMask != KSAUDIO_SPEAKER_STEREO)) {
            return TRUE;
        }
    } else if (W->wFormatTag != WAVE_FORMAT_PCM) {
        return TRUE;
    }

    *Status = STATUS_SUCCESS;
    return TRUE;
}

/* TRUE = the driver completed the request. */
static BOOLEAN AudKsOverride(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ WDFREQUEST Request, _In_ const ACX_REQUEST_PARAMETERS *P)
{
    NTSTATUS  Status;
    ULONG_PTR Info = 0;

    if (Dev == NULL || !Dev->Cfg.KsOverride || P->Type != AcxRequestTypeProperty ||
        P->Parameters.Property.Set.Data1 != 0x8C134960 || P->Parameters.Property.ItemType != AcxItemTypePin ||
        P->Parameters.Property.ItemId != Dev->HostPinId ||
        !AudGuidIs(&P->Parameters.Property.Set, &KSPROPSETID_Pin)) {
        return FALSE;
    }

    if (P->Parameters.Property.Id == AUD_KSPROPERTY_PIN_DATAINTERSECTION && P->Parameters.Property.Verb == AcxPropertyVerbGet) {
        Status = AudIntersect(Dev, P, &Info);
    } else if (P->Parameters.Property.Id == AUD_KSPROPERTY_PIN_PROPOSEDATAFORMAT &&
               P->Parameters.Property.Verb == AcxPropertyVerbSet) {
        if (!AudPropose(P, &Status)) {
            return FALSE;
        }
        AudLogProposedFormat(Dev, 'c', P, NT_SUCCESS(Status) ? "driver ACCEPTS" : "driver rejects (NO_MATCH)");
    } else {
        return FALSE;
    }

    WdfRequestCompleteWithInformation(Request, Status, Info);   /* P's buffers are not touched after this */
    AudReqLog(Dev, 'c', P, "done", Status, Info);
    return TRUE;
}

_Use_decl_annotations_
VOID NTAPI AudEvtCircuitRequestPreprocess(ACXOBJECT Object, ACXCONTEXT DriverContext, WDFREQUEST Request)
{
    PAUD_DEVICE_CONTEXT    Dev = (PAUD_DEVICE_CONTEXT)DriverContext;
    ACX_REQUEST_PARAMETERS P;
    NTSTATUS               Status;

    PAGED_CODE();

    ACX_REQUEST_PARAMETERS_INIT(&P);
    AcxRequestGetParameters(Request, &P);
    if (AudKsOverride(Dev, Request, &P)) {
        return;                                                            /* completed by the driver */
    }
    AudReqInspect(Dev, 'C', &P);                                           /* still ours to read: ACX has not seen it */
    Status = AcxCircuitDispatchAcxRequest((ACXCIRCUIT)Object, Request);   /* the request belongs to ACX from here on */
    AudReqLog(Dev, 'C', &P, "disp", Status, 0);
}

_Use_decl_annotations_
VOID NTAPI AudEvtStreamRequestPreprocess(ACXOBJECT Object, ACXCONTEXT DriverContext, WDFREQUEST Request)
{
    PAUD_DEVICE_CONTEXT    Dev = (PAUD_DEVICE_CONTEXT)DriverContext;
    ACX_REQUEST_PARAMETERS P;
    NTSTATUS               Status;

    PAGED_CODE();

    ACX_REQUEST_PARAMETERS_INIT(&P);
    AcxRequestGetParameters(Request, &P);
    Status = AcxStreamDispatchAcxRequest((ACXSTREAM)Object, Request);
    AudReqLog(Dev, 'S', &P, "disp", Status, 0);
}
