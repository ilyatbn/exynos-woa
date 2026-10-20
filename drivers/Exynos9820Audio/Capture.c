/*
 * Capture.c - the ACX capture circuit "Microphone0" (the two digital microphones, notes/d8-mic.md) and its RT stream.
 *
 * Shape: Microsoft Windows-driver-samples audio/Acx/Samples/Common/CaptureCircuit.cpp (circuit type Capture; host pin
 * = AcxPinTypeSource / AcxPinCommunicationSink / KSCATEGORY_AUDIO; bridge pin = AcxPinTypeSink / AcxPinCommunicationNone
 * / KSNODETYPE_MICROPHONE with an integrated jack; a volume element) plus a mute element like Render.c.
 *
 * Hardware: Mic.c MicHwStart = codec DMIC1/2 -> AIF1 TX -> ABOX UAIF0 MIC -> NSRC0 -> SPUM -> SIFM0 -> WDMA1 into a
 * non-cached ring of AUD_CAP_SLOTS periods (one packet's frames x 16 bytes: 4 x 32-bit slots, slots 0/1 = DMIC1/2).
 * The packet timer follows the WDMA write position (AboxCapturePosition) and copies each completed period into the
 * client packet: slot 0 -> left, slot 1 -> right, the endpoint level (software, <= 0 dB) and mute applied, 16- or
 * 32-bit containers. The hardware gain (codec digital volume, 0..+31.5 dB) is the "mic boost" set by the speaker trim
 * app over the AudControl property (Control.c) and Parameters\MicGain. Without hardware the stream runs on the QPC
 * and delivers silence.
 */

#include "Exynos9820Audio.h"

/* {B2C5E9A3-3D74-4F0E-9C1B-9820A0D1C35E}: component id of the microphone circuit (project-local, like Render.c's). */
static const GUID AudMicComponentGuid =
    { 0xb2c5e9a3, 0x3d74, 0x4f0e, { 0x9c, 0x1b, 0x98, 0x20, 0xa0, 0xd1, 0xc3, 0x5e } };

/* AUDIO_SIGNALPROCESSINGMODE_DEFAULT (ksmedia.h), as in Render.c. */
static const GUID AudCapModeDefault =
    { 0xc18e2f7e, 0x933d, 0x4965, { 0xb7, 0xd1, 0x1e, 0xef, 0x22, 0x8d, 0x2a, 0xf3 } };

/* Must equal the KSNAME reference string in Exynos9820Audio.inf. */
DECLARE_CONST_UNICODE_STRING(AudMicCircuitName, L"Microphone0");

#define AUD_CAP_ALL_CHANNELS   0xFFFFFFFFu
#define AUD_CAP_VOLUME_MIN     (-96 * 0x10000)   /* KS units (1/65536 dB), as Render.c */
#define AUD_CAP_VOLUME_MAX     0                 /* the boost lives in the codec (app), the endpoint level only cuts */
#define AUD_CAP_VOLUME_STEP    0x8000            /* 0.5 dB */
#define AUD_CAP_SLOTS          8u                /* periods in the WDMA ring */

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE AudCapPcm16 = {
    { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    { { WAVE_FORMAT_EXTENSIBLE, AUD_CHANNELS, AUD_RATE, AUD_RATE * 4, 4, 16, sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) },
      { 16 }, KSAUDIO_SPEAKER_STEREO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM) }
};

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE AudCapPcm32 = {
    { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
      STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    { { WAVE_FORMAT_EXTENSIBLE, AUD_CHANNELS, AUD_RATE, AUD_RATE * 8, 8, 32, sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) },
      { 32 }, KSAUDIO_SPEAKER_STEREO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM) }
};

/* Render.c element callbacks (software level / mute per endpoint; the Capture flag keeps them off the amps). */
EVT_ACX_RAMPED_VOLUME_ASSIGN_LEVEL AudEvtVolumeAssignLevel;
EVT_ACX_VOLUME_RETRIEVE_LEVEL      AudEvtVolumeRetrieveLevel;
EVT_ACX_MUTE_ASSIGN_STATE          AudEvtMuteAssignState;
EVT_ACX_MUTE_RETRIEVE_STATE        AudEvtMuteRetrieveState;

static EVT_ACX_CIRCUIT_CREATE_STREAM     CapEvtCreateStream;
static EVT_ACX_STREAM_PREPARE_HARDWARE   CapEvtPrepareHardware;
static EVT_ACX_STREAM_RELEASE_HARDWARE   CapEvtReleaseHardware;
static EVT_ACX_STREAM_RUN                CapEvtRun;
static EVT_ACX_STREAM_PAUSE              CapEvtPause;
static EVT_ACX_STREAM_GET_HW_LATENCY     CapEvtGetHwLatency;
static EVT_ACX_STREAM_ALLOCATE_RTPACKETS CapEvtAllocateRtPackets;
static EVT_ACX_STREAM_FREE_RTPACKETS     CapEvtFreeRtPackets;
static EVT_ACX_STREAM_GET_CAPTURE_PACKET CapEvtGetCapturePacket;
static EVT_ACX_STREAM_GET_CURRENT_PACKET CapEvtGetCurrentPacket;
static EVT_ACX_STREAM_GET_PRESENTATION_POSITION CapEvtGetPresentationPosition;
static EVT_WDF_TIMER                     CapEvtTimer;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP    CapEvtStreamCleanup;

/* The mic endpoint exists when the codec, ABOX and the codec-clocked UAIF0 link are enabled (the hardware path). */
BOOLEAN AudMicEndpoint(_In_ PAUD_DEVICE_CONTEXT Dev)
{
    return Dev->Cfg.CodecEnable && Dev->Cfg.AboxEnable && Dev->Cfg.HpMaster;
}

/* ================================================================================================================
 * Circuit.
 * ================================================================================================================ */

static NTSTATUS CapAddFormat(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXDATAFORMATLIST List,
                             _In_ PKSDATAFORMAT_WAVEFORMATEXTENSIBLE Wave, _In_ BOOLEAN Default)
{
    NTSTATUS              Status;
    WDF_OBJECT_ATTRIBUTES Attr;
    ACX_DATAFORMAT_CONFIG Cfg;
    ACXDATAFORMAT         Format;

    ACX_DATAFORMAT_CONFIG_INIT_KS(&Cfg, Wave);
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Circuit;
    Status = AcxDataFormatCreate(Device, &Attr, &Cfg, &Format);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return Default ? AcxDataFormatListAssignDefaultDataFormat(List, Format) : AcxDataFormatListAddDataFormat(List, Format);
}

static NTSTATUS CapCreateElements(_In_ ACXCIRCUIT Circuit, _In_ PAUD_DEVICE_CONTEXT Dev, _In_ PAUD_ENDPOINT Ep)
{
    NTSTATUS              Status;
    WDF_OBJECT_ATTRIBUTES Attr;
    ACX_VOLUME_CALLBACKS  VolCb;
    ACX_VOLUME_CONFIG     VolCfg;
    ACX_MUTE_CALLBACKS    MuteCb;
    ACX_MUTE_CONFIG       MuteCfg;
    ACXELEMENT            Elements[2];

    ACX_VOLUME_CALLBACKS_INIT(&VolCb);
    VolCb.EvtAcxRampedVolumeAssignLevel = AudEvtVolumeAssignLevel;
    VolCb.EvtAcxVolumeRetrieveLevel     = AudEvtVolumeRetrieveLevel;
    ACX_VOLUME_CONFIG_INIT(&VolCfg);
    VolCfg.ChannelsCount = AUD_CHANNELS;
    VolCfg.Minimum       = AUD_CAP_VOLUME_MIN;
    VolCfg.Maximum       = AUD_CAP_VOLUME_MAX;
    VolCfg.SteppingDelta = AUD_CAP_VOLUME_STEP;
    VolCfg.Name          = &KSAUDFNAME_VOLUME_CONTROL;
    VolCfg.Callbacks     = &VolCb;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_ELEMENT_CONTEXT);
    Attr.ParentObject = Circuit;
    Status = AcxVolumeCreate(Circuit, &Attr, &VolCfg, (ACXVOLUME *)&Elements[0]);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    AudGetElementContext(Elements[0])->Dev = Dev;
    AudGetElementContext(Elements[0])->Ep  = Ep;

    ACX_MUTE_CALLBACKS_INIT(&MuteCb);
    MuteCb.EvtAcxMuteAssignState   = AudEvtMuteAssignState;
    MuteCb.EvtAcxMuteRetrieveState = AudEvtMuteRetrieveState;
    ACX_MUTE_CONFIG_INIT(&MuteCfg);
    MuteCfg.ChannelsCount = AUD_CHANNELS;
    MuteCfg.Name          = &KSAUDFNAME_WAVE_MUTE;
    MuteCfg.Callbacks     = &MuteCb;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_ELEMENT_CONTEXT);
    Attr.ParentObject = Circuit;
    Status = AcxMuteCreate(Circuit, &Attr, &MuteCfg, (ACXMUTE *)&Elements[1]);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    AudGetElementContext(Elements[1])->Dev = Dev;
    AudGetElementContext(Elements[1])->Ep  = Ep;

    return AcxCircuitAddElements(Circuit, Elements, ARRAYSIZE(Elements));
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AudCaptureCircuitCreate(_In_ WDFDEVICE Device, _In_ PAUD_DEVICE_CONTEXT Dev)
{
    PAUD_ENDPOINT         Ep = &Dev->Ep[AUD_EP_MIC];
    NTSTATUS              Status;
    PACXCIRCUIT_INIT      Init;
    ACXCIRCUIT            Circuit;
    WDF_OBJECT_ATTRIBUTES Attr;
    ACX_PIN_CONFIG        PinCfg;
    ACXPIN                Pins[2];
    ACX_JACK_CONFIG       JackCfg;
    ACXJACK               Jack;
    ACXDATAFORMATLIST     List;

    PAGED_CODE();

    Init = AcxCircuitInitAllocate(Device);
    if (Init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    AcxCircuitInitSetComponentId(Init, &AudMicComponentGuid);
    Status = AcxCircuitInitAssignName(Init, &AudMicCircuitName);
    if (NT_SUCCESS(Status)) {
        AcxCircuitInitSetCircuitType(Init, AcxCircuitTypeCapture);
        Status = AcxCircuitInitAssignAcxCreateStreamCallback(Init, CapEvtCreateStream);
    }
    if (!NT_SUCCESS(Status)) {
        AcxCircuitInitFree(Init);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_CIRCUIT_CONTEXT);
    Status = AcxCircuitCreate(Device, &Attr, &Init, &Circuit);
    if (!NT_SUCCESS(Status)) {
        AcxCircuitInitFree(Init);
        return Status;
    }
    AudGetCircuitContext(Circuit)->Dev = Dev;
    AudGetCircuitContext(Circuit)->Ep  = Ep;

    Status = CapCreateElements(Circuit, Dev, Ep);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* host (streaming) pin */
    ACX_PIN_CONFIG_INIT(&PinCfg);
    PinCfg.Type          = AcxPinTypeSource;
    PinCfg.Communication = AcxPinCommunicationSink;
    PinCfg.Category      = &KSCATEGORY_AUDIO;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Circuit;
    Status = AcxPinCreate(Circuit, &Attr, &PinCfg, &Pins[0]);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* bridge pin: the built-in microphones */
    ACX_PIN_CONFIG_INIT(&PinCfg);
    PinCfg.Type          = AcxPinTypeSink;
    PinCfg.Communication = AcxPinCommunicationNone;
    PinCfg.Category      = &KSNODETYPE_MICROPHONE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Circuit;
    Status = AcxPinCreate(Circuit, &Attr, &PinCfg, &Pins[1]);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ACX_JACK_CONFIG_INIT(&JackCfg);
    JackCfg.Description.ChannelMapping = KSAUDIO_SPEAKER_STEREO;
    JackCfg.Description.Color          = 0;
    JackCfg.Description.ConnectionType = AcxConnTypeAtapiInternal;
    JackCfg.Description.GeoLocation    = AcxGeoLocFront;
    JackCfg.Description.GenLocation    = AcxGenLocPrimaryBox;
    JackCfg.Description.PortConnection = AcxPortConnIntegratedDevice;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_ELEMENT_CONTEXT);
    Attr.ParentObject = Pins[1];
    Status = AcxJackCreate(Pins[1], &Attr, &JackCfg, &Jack);
    if (NT_SUCCESS(Status)) {
        AudGetElementContext(Jack)->Dev = Dev;
        AudGetElementContext(Jack)->Ep  = Ep;
        Status = AcxPinAddJacks(Pins[1], &Jack, 1);   /* no presence detection: Ep->Jack stays NULL (AudJackNotify) */
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    List = AcxPinGetRawDataFormatList(Pins[0]);
    if (List == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = CapAddFormat(Device, Circuit, List, &AudCapPcm16, TRUE);   /* = INF PKEY_AudioEngine_OEMFormat */
    if (NT_SUCCESS(Status)) {
        Status = CapAddFormat(Device, Circuit, List, &AudCapPcm32, FALSE);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (Dev->Cfg.DefaultMode) {   /* RAW + DEFAULT like the render host pins (Render.c A10) */
        ACX_DATAFORMAT_LIST_CONFIG ListCfg;
        ACXDATAFORMATLIST          ModeList = NULL;
        NTSTATUS                   ModeStatus;

        ACX_DATAFORMAT_LIST_CONFIG_INIT(&ListCfg);
        WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
        Attr.ParentObject = Pins[0];
        ModeStatus = AcxDataFormatListCreate(Device, &Attr, &ListCfg, &ModeList);
        if (NT_SUCCESS(ModeStatus)) {
            ModeStatus = CapAddFormat(Device, Circuit, ModeList, &AudCapPcm16, TRUE);
        }
        if (NT_SUCCESS(ModeStatus)) {
            ModeStatus = CapAddFormat(Device, Circuit, ModeList, &AudCapPcm32, FALSE);
        }
        if (NT_SUCCESS(ModeStatus)) {
            ModeStatus = AcxPinAssignModeDataFormatList(Pins[0], &AudCapModeDefault, ModeList);
        }
        AudLog(Dev->Log, "mic circuit: DEFAULT-mode format list: %08lX\r\n", ModeStatus);
    }

    Status = AcxCircuitAddPins(Circuit, Pins, ARRAYSIZE(Pins));
    if (NT_SUCCESS(Status)) {
        Ep->Circuit = Circuit;
        AudLog(Dev->Log, "circuit Microphone0: host pin id %lu, bridge pin id %lu\r\n", AcxPinGetId(Pins[0]),
               AcxPinGetId(Pins[1]));
    }
    return Status;
}

/* ================================================================================================================
 * Stream.
 * ================================================================================================================ */

static PUCHAR CapPacketVa(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONG Packet)
{
    ULONG Index = Packet % Ctx->PacketCount;

    return Ctx->PacketBuf[Index] + ((Index == 0) ? Ctx->FirstPacketOffset : 0);
}

/* Period Slot of the WDMA ring -> client packet Dst: slot 0 -> left, slot 1 -> right, level / mute, container. The ring
   is non-cached: explicit 32-bit volatile loads (as Render.c's stores). */
static VOID CapCopyPeriod(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONG Slot, _Out_ PUCHAR Dst)
{
    const volatile LONG *Src = (const volatile LONG *)(Ctx->DmaVa + (SIZE_T)(Slot % AUD_CAP_SLOTS) * Ctx->DmaSlotBytes);
    PAUD_ENDPOINT        Ep = Ctx->Ep;
    LONG                 GainL = Ep->SwGainQ30[0];
    LONG                 GainR = Ep->SwGainQ30[1];
    BOOLEAN              Muted = Ep->SwMuted != 0 || Src == NULL;
    ULONG                F;

    for (F = 0; F < Ctx->FramesPerPacket; F++) {
        LONG L = 0, R = 0;

        if (!Muted) {
            L = (LONG)(((LONGLONG)Src[4 * F + 0] * GainL) >> 30);   /* Q30 gain <= 1.0: no overflow */
            R = (LONG)(((LONGLONG)Src[4 * F + 1] * GainR) >> 30);
        }
        if (Ctx->BitsPerSample == 16) {
            ((PSHORT)Dst)[2 * F]     = (SHORT)(L >> 16);
            ((PSHORT)Dst)[2 * F + 1] = (SHORT)(R >> 16);
        } else {
            ((PLONG)Dst)[2 * F]     = L;
            ((PLONG)Dst)[2 * F + 1] = R;
        }
    }
}

static ULONGLONG CapNowHns(_In_ PAUD_STREAM_CONTEXT Ctx)
{
    return KSCONVERT_PERFORMANCE_TIME(Ctx->QpcFrequency.QuadPart, KeQueryPerformanceCounter(NULL));
}

/* Frames written since Run: WDMA ring position accumulated modulo the ring (sampled every period), else QPC. */
static ULONGLONG CapElapsedFrames(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONG     Count;
    ULONG     Raw;
    ULONGLONG Frames;
    KIRQL     OldIrql;

    if (!Ctx->HwStarted) {
        return (CapNowHns(Ctx) - Ctx->StartTime) * AUD_RATE / AUD_HNS_PER_SEC;
    }
    KeAcquireSpinLock(&Ctx->HwLock, &OldIrql);
    if (NT_SUCCESS(AboxCapturePosition(Ctx->Dev, &Count, &Raw))) {
        ULONG RingFrames = Ctx->DmaBytes / AUD_HW_FRAME_BYTES;

        Ctx->HwLastRaw = Raw;
        Count %= RingFrames;
        Ctx->HwFrames += (Count + RingFrames - Ctx->HwLastPos) % RingFrames;
        Ctx->HwLastPos = Count;
    }
    Frames = Ctx->HwFrames;
    KeReleaseSpinLock(&Ctx->HwLock, OldIrql);
    return Frames;
}

static LONGLONG CapNextDue(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONGLONG Frames)
{
    ULONGLONG Next = ((ULONGLONG)Ctx->Periods + 1) * Ctx->FramesPerPacket;
    ULONGLONG Hns  = (Next > Frames) ? (Next - Frames) * AUD_HNS_PER_SEC / AUD_RATE : 0;

    Hns += 5000;   /* 0.5 ms past the boundary */
    return -(LONGLONG)((Hns < 10000) ? 10000 : Hns);
}

_Use_decl_annotations_
static VOID CapEvtTimer(WDFTIMER Timer)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetTimerContext(Timer)->Stream;
    ULONGLONG           Frames = CapElapsedFrames(Ctx);
    ULONG               Done = 0;

    while (Frames >= ((ULONGLONG)Ctx->Periods + 1) * Ctx->FramesPerPacket) {
        ULONG     Packet = Ctx->Periods;
        ULONGLONG Behind = Frames - (ULONGLONG)Packet * Ctx->FramesPerPacket;   /* frames since this packet began */
        LONGLONG  Qpc    = KeQueryPerformanceCounter(NULL).QuadPart -
                           (LONGLONG)(Behind * (ULONGLONG)Ctx->QpcFrequency.QuadPart / AUD_RATE);

        /* review F4 of a808ccf: the WDMA reuses this packet's ring period once it reaches period Packet + AUD_CAP_SLOTS;
           with the WDMA already in period Packet + AUD_CAP_SLOTS - 1 or later (one period of margin for the copy) the
           data is overwritten or about to be - deliver silence rather than torn audio */
        if (Ctx->HwStarted && Frames < ((ULONGLONG)Packet + AUD_CAP_SLOTS - 1) * Ctx->FramesPerPacket) {
            CapCopyPeriod(Ctx, Packet, CapPacketVa(Ctx, Packet));
        } else {
            RtlZeroMemory(CapPacketVa(Ctx, Packet), Ctx->PacketSize);
            if (Ctx->HwStarted) {
                Ctx->CapLost++;
            }
        }
        Ctx->LastCaptureQpc = (ULONGLONG)Qpc;
        InterlockedExchange(&Ctx->LastCapturePacket, (LONG)Packet);
        Ctx->Periods++;
        InterlockedExchange(&Ctx->CurrentPacket, (LONG)Ctx->Periods);
        (VOID)AcxRtStreamNotifyPacketComplete(Ctx->Stream, Packet, (ULONGLONG)Qpc);
        Done++;
        if (Ctx->HwStarted && (Packet % 500) == 0) {
            AudLog(Ctx->Dev->Log, "mic stream: packet %lu, WDMA frames %I64u (pos %lu, STATUS %08lX)\r\n", Packet, Frames,
                   Ctx->HwLastPos, Ctx->HwLastRaw);
        }
    }
    if (Done > 1) {
        Ctx->Glitches++;
    }
    (VOID)WdfTimerStart(Timer, CapNextDue(Ctx, Frames));
}

static VOID CapRingFree(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONG I;

    for (I = 0; I < AUD_MAX_PACKETS; I++) {
        if (Ctx->PacketMdl[I] != NULL) {
            IoFreeMdl(Ctx->PacketMdl[I]);
            Ctx->PacketMdl[I] = NULL;
        }
        if (Ctx->PacketBuf[I] != NULL) {
            ExFreePoolWithTag(Ctx->PacketBuf[I], AUD_POOL_TAG);
            Ctx->PacketBuf[I] = NULL;
        }
    }
    if (Ctx->DmaVa != NULL) {
        if (Ctx->Dev->AboxCapDmaStuck) {
            AudLog(Ctx->Dev->Log, "mic stream: WDMA still busy - ring 0x%I64X leaked\r\n", Ctx->DmaPhys.QuadPart);
        } else {
            MmFreeContiguousMemory(Ctx->DmaVa);
        }
        Ctx->DmaVa = NULL;
    }
    Ctx->PacketCount = 0;
}

static VOID CapHwStop(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    if (Ctx->HwStarted) {
        MicHwStop(Ctx->Dev);
        Ctx->HwStarted = FALSE;
    }
}

_Use_decl_annotations_
static NTSTATUS CapEvtAllocateRtPackets(ACXSTREAM Stream, ULONG PacketCount, ULONG PacketSize, PACX_RTPACKET *Packets)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    PHYSICAL_ADDRESS    Low, High, Skip;
    PACX_RTPACKET       P;
    ULONG               I;

    PAGED_CODE();

    *Packets = NULL;
    if (PacketCount == 0 || PacketCount > AUD_MAX_PACKETS || PacketSize == 0 || PacketSize > (1u << 20) ||
        (PacketSize % Ctx->BlockAlign) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->PacketBuf[0] != NULL || Ctx->DmaVa != NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    Ctx->AllocPerPacket = (ULONG)ROUND_TO_PAGES(PacketSize);
    for (I = 0; I < PacketCount; I++) {
        Ctx->PacketBuf[I] = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Ctx->AllocPerPacket, AUD_POOL_TAG);
        if (Ctx->PacketBuf[I] == NULL) {
            CapRingFree(Ctx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* WDMA ring: AUD_CAP_SLOTS periods of 4 x 32-bit frames, non-cached, below 4 GiB (32-bit BUF_STR / BUF_END). */
    Low.QuadPart  = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Skip.QuadPart = 0;
    Ctx->FramesPerPacket = PacketSize / Ctx->BlockAlign;
    Ctx->DmaSlotBytes    = Ctx->FramesPerPacket * AUD_HW_FRAME_BYTES;
    Ctx->DmaBytes        = Ctx->DmaSlotBytes * AUD_CAP_SLOTS;
    Ctx->DmaVa = (PUCHAR)MmAllocateContiguousNodeMemory(Ctx->DmaBytes, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE,
                                                        MM_ANY_NODE_OK);
    Ctx->DmaPhys.QuadPart = (Ctx->DmaVa != NULL) ? MmGetPhysicalAddress(Ctx->DmaVa).QuadPart : 0;

    P = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * PacketCount, AUD_POOL_TAG);
    if (P == NULL) {
        CapRingFree(Ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Ctx->PacketCount       = PacketCount;
    Ctx->PacketSize        = PacketSize;
    Ctx->FirstPacketOffset = Ctx->AllocPerPacket - PacketSize;
    for (I = 0; I < PacketCount; I++) {
        Ctx->PacketMdl[I] = IoAllocateMdl(Ctx->PacketBuf[I], Ctx->AllocPerPacket, FALSE, TRUE, NULL);
        if (Ctx->PacketMdl[I] == NULL) {
            ExFreePoolWithTag(P, AUD_POOL_TAG);
            CapRingFree(Ctx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(Ctx->PacketMdl[I]);
        ACX_RTPACKET_INIT(&P[I]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&P[I].RtPacketBuffer, Ctx->PacketMdl[I], Ctx->AllocPerPacket);
        P[I].RtPacketOffset = (I == 0) ? Ctx->FirstPacketOffset : 0;
        P[I].RtPacketSize   = PacketSize;
    }
    AudLog(Ctx->Dev->Log, "mic stream: %lu packets x %lu B (%lu frames, %lu bit); WDMA ring %s 0x%I64X + 0x%lX\r\n",
           PacketCount, PacketSize, Ctx->FramesPerPacket, Ctx->BitsPerSample, (Ctx->DmaVa != NULL) ? "at" : "NOT allocated,",
           Ctx->DmaPhys.QuadPart, Ctx->DmaBytes);
    *Packets = P;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static VOID CapEvtFreeRtPackets(ACXSTREAM Stream, PACX_RTPACKET Packets, ULONG PacketCount)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    UNREFERENCED_PARAMETER(PacketCount);
    PAGED_CODE();

    CapHwStop(Ctx);   /* never free a ring the WDMA may still write */
    CapRingFree(Ctx);
    if (Packets != NULL) {
        ExFreePoolWithTag(Packets, AUD_POOL_TAG);
    }
}

_Use_decl_annotations_
static NTSTATUS CapEvtPrepareHardware(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT   Ctx = AudGetStreamContext(Stream);
    WDF_TIMER_CONFIG      TimerCfg;
    WDF_OBJECT_ATTRIBUTES Attr;
    NTSTATUS              Status;

    PAGED_CODE();

    if (Ctx->State == AcxStreamStatePause) {
        return STATUS_SUCCESS;
    }
    if (Ctx->State != AcxStreamStateStop) {
        return STATUS_INVALID_STATE_TRANSITION;
    }
    WDF_TIMER_CONFIG_INIT(&TimerCfg, CapEvtTimer);
    TimerCfg.AutomaticSerialization = TRUE;
    TimerCfg.UseHighResolutionTimer = WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_TIMER_CONTEXT);
    Attr.ParentObject = Stream;
    Status = WdfTimerCreate(&TimerCfg, &Attr, &Ctx->Timer);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    AudGetTimerContext(Ctx->Timer)->Stream = Ctx;
    Ctx->State = AcxStreamStatePause;
    AudLog(Ctx->Dev->Log, "mic stream: prepared (%lu bit)\r\n", Ctx->BitsPerSample);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtReleaseHardware(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    if (Ctx->State == AcxStreamStateStop) {
        return STATUS_SUCCESS;
    }
    if (Ctx->Timer != NULL) {
        WdfTimerStop(Ctx->Timer, TRUE);
        WdfObjectDelete(Ctx->Timer);
        Ctx->Timer = NULL;
    }
    KeFlushQueuedDpcs();
    CapHwStop(Ctx);
    AudLog(Ctx->Dev->Log, "mic stream: released after %lu packets, %lu late timer passes\r\n", Ctx->Periods, Ctx->Glitches);
    Ctx->CurrentPacket = 0;
    Ctx->State         = AcxStreamStateStop;
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtRun(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    PAUD_DEVICE_CONTEXT Dev = Ctx->Dev;
    NTSTATUS            Status = STATUS_DEVICE_NOT_READY;
    ULONG               Count = 0;

    PAGED_CODE();

    if (Ctx->State == AcxStreamStateRun) {
        return STATUS_SUCCESS;
    }
    if (Ctx->State != AcxStreamStatePause || Ctx->PacketCount == 0) {
        return STATUS_INVALID_STATE_TRANSITION;
    }
    Ctx->Periods = 0;
    InterlockedExchange(&Ctx->LastCapturePacket, -1);
    InterlockedExchange(&Ctx->CurrentPacket, 0);
    if (Ctx->DmaVa != NULL && !Dev->AboxCapRunning) {
        Status = MicHwStart(Dev, Ctx->DmaPhys, Ctx->DmaBytes, Ctx->DmaSlotBytes);
        if (NT_SUCCESS(Status)) {
            (VOID)AboxCapturePosition(Dev, &Count, &Ctx->HwLastRaw);
            /* the WDMA starts at the ring start (STR_POINT): frames since Run = its position, so period j stays slot j */
            Ctx->HwLastPos = Count % (Ctx->DmaBytes / AUD_HW_FRAME_BYTES);
            Ctx->HwFrames  = Ctx->HwLastPos;
            Ctx->HwStarted = TRUE;
        }
    }
    Ctx->StartTime = CapNowHns(Ctx);
    Ctx->State     = AcxStreamStateRun;
    (VOID)WdfTimerStart(Ctx->Timer, CapNextDue(Ctx, 0));
    AudLog(Dev->Log, "mic stream: run, %s (%08lX)\r\n", Ctx->HwStarted ? "WDMA1 running (hardware clock)" : "no hardware (silence)",
           Status);
    AudLogFlush(Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtPause(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    if (Ctx->State == AcxStreamStatePause) {
        return STATUS_SUCCESS;
    }
    if (Ctx->State != AcxStreamStateRun) {
        return STATUS_INVALID_STATE_TRANSITION;
    }
    WdfTimerStop(Ctx->Timer, TRUE);
    Ctx->State = AcxStreamStatePause;
    CapHwStop(Ctx);
    AudLog(Ctx->Dev->Log, "mic stream: pause after %lu packets, %lu late timer passes, %lu overrun packets (silence)\r\n",
           Ctx->Periods, Ctx->Glitches, Ctx->CapLost);
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtGetHwLatency(ACXSTREAM Stream, ULONG *FifoSize, ULONG *Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    PAGED_CODE();

    *FifoSize = 128;   /* the ACX sample's values (Render.c A11) */
    *Delay    = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtGetCapturePacket(ACXSTREAM Stream, PULONG LastCapturePacket, PULONGLONG QPCPacketStart,
                                       PBOOLEAN MoreData)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    LONG                Last;

    PAGED_CODE();

    Last = InterlockedCompareExchange(&Ctx->LastCapturePacket, -1, -1);
    if (Last < 0) {
        return STATUS_DEVICE_NOT_READY;   /* no packet completed since Run yet */
    }
    *LastCapturePacket = (ULONG)Last;
    *QPCPacketStart    = Ctx->LastCaptureQpc;
    *MoreData          = FALSE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtGetCurrentPacket(ACXSTREAM Stream, PULONG CurrentPacket)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    *CurrentPacket = (ULONG)InterlockedCompareExchange(&Ctx->CurrentPacket, -1, -1);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static NTSTATUS CapEvtGetPresentationPosition(ACXSTREAM Stream, PULONGLONG PositionInBlocks, PULONGLONG QPCPosition)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    *PositionInBlocks = (Ctx->State == AcxStreamStateRun) ? CapElapsedFrames(Ctx) : (ULONGLONG)Ctx->Periods * Ctx->FramesPerPacket;
    *QPCPosition      = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static VOID CapEvtStreamCleanup(WDFOBJECT Object)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext((ACXSTREAM)Object);

    if (Ctx->Dev != NULL) {
        CapHwStop(Ctx);
        CapRingFree(Ctx);
    }
}

_Use_decl_annotations_
static NTSTATUS NTAPI CapEvtCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN Pin, PACXSTREAM_INIT StreamInit,
                                         ACXDATAFORMAT StreamFormat, const GUID *SignalProcessingMode,
                                         ACXOBJECTBAG VarArguments)
{
    PAUD_DEVICE_CONTEXT     Dev = AudGetCircuitContext(Circuit)->Dev;
    NTSTATUS                Status;
    ACX_STREAM_CALLBACKS    Cb;
    ACX_RT_STREAM_CALLBACKS Rt;
    WDF_OBJECT_ATTRIBUTES   Attr;
    ACXSTREAM               Stream;
    PAUD_STREAM_CONTEXT     Ctx;
    PWAVEFORMATEXTENSIBLE   Wfx;

    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);
    PAGED_CODE();

    ACX_STREAM_CALLBACKS_INIT(&Cb);
    Cb.EvtAcxStreamPrepareHardware = CapEvtPrepareHardware;
    Cb.EvtAcxStreamReleaseHardware = CapEvtReleaseHardware;
    Cb.EvtAcxStreamRun             = CapEvtRun;
    Cb.EvtAcxStreamPause           = CapEvtPause;
    Status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &Cb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    ACX_RT_STREAM_CALLBACKS_INIT(&Rt);
    Rt.EvtAcxStreamGetHwLatency            = CapEvtGetHwLatency;
    Rt.EvtAcxStreamAllocateRtPackets       = CapEvtAllocateRtPackets;
    Rt.EvtAcxStreamFreeRtPackets           = CapEvtFreeRtPackets;
    Rt.EvtAcxStreamGetCapturePacket        = CapEvtGetCapturePacket;
    Rt.EvtAcxStreamGetCurrentPacket        = CapEvtGetCurrentPacket;
    Rt.EvtAcxStreamGetPresentationPosition = CapEvtGetPresentationPosition;
    Status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &Rt);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_STREAM_CONTEXT);
    Attr.EvtCleanupCallback = CapEvtStreamCleanup;
    Status = AcxRtStreamCreate(Device, Circuit, &Attr, &StreamInit, &Stream);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Ctx = AudGetStreamContext(Stream);
    Ctx->Dev         = Dev;
    Ctx->Ep          = AudGetCircuitContext(Circuit)->Ep;
    Ctx->Stream      = Stream;
    Ctx->Format      = StreamFormat;
    Ctx->Capture     = TRUE;
    Ctx->BlockAlign  = AcxDataFormatGetBlockAlign(StreamFormat);
    Ctx->BytesPerSec = AcxDataFormatGetAverageBytesPerSec(StreamFormat);
    Wfx = (PWAVEFORMATEXTENSIBLE)AcxDataFormatGetWaveFormatExtensible(StreamFormat);
    Ctx->BitsPerSample = (Wfx != NULL) ? Wfx->Format.wBitsPerSample : 0;
    Ctx->State = AcxStreamStateStop;
    Ctx->LastCapturePacket = -1;
    KeInitializeSpinLock(&Ctx->HwLock);
    KeQueryPerformanceCounter(&Ctx->QpcFrequency);
    if (Ctx->BlockAlign != AUD_CHANNELS * (Ctx->BitsPerSample / 8) || (Ctx->BitsPerSample != 16 && Ctx->BitsPerSample != 32) ||
        Ctx->BytesPerSec != AUD_RATE * Ctx->BlockAlign) {
        AudLog(Dev->Log, "mic stream: format %lu B/s block %lu %lu bit not supported\r\n", Ctx->BytesPerSec, Ctx->BlockAlign,
               Ctx->BitsPerSample);
        return STATUS_NOT_SUPPORTED;
    }
    AudLog(Dev->Log, "mic stream: created, %lu bit\r\n", Ctx->BitsPerSample);
    AudLogFlush(Dev->Log);
    return STATUS_SUCCESS;
}
