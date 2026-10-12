/*
 * Render.c - the ACX render circuit "Speaker0" and its WaveRT (ACX RT) stream.
 *
 * Shape: Microsoft Windows-driver-samples audio/Acx/Samples (tree 2dc3fd3a): Common/RenderCircuit.cpp
 * (circuit, host + bridge pin, jack, formats) and Common/StreamEngine.cpp (RT packets, one-shot high-resolution
 * WDF timer per packet, QPC position). Differences, each for a reason in notes/d8-audio-win-m1.md "Design":
 *   - the packets are carved from ONE physically contiguous allocation below 4 GiB, so the ring is a valid ABOX
 *     RDMA buffer (32-bit BUF_STR/END); packet 0 ends exactly where packet 1 starts;
 *   - cache maintenance (clean) on SetRenderPacket, Run and every packet tick: the ABOX DMA does not snoop
 *     (the m12-wifi-m5 lesson);
 *   - no volume/mute element unless Parameters\HwVolume = 1: without one, audiodg applies endpoint volume in
 *     software, which is what makes the volume keys and the slider work before the hardware path is proven;
 *   - the timer clock stays the stream clock; when the hardware runs, the RDMA offset is only logged next to it;
 *   - UpdatePosition subtracts the glitch time in time units (the sample subtracts HNS from a byte count).
 *
 * NOT VALIDATED ON HARDWARE (stage Guided).
 */

#include "Exynos9820Audio.h"

/* {B2C5E9A1-3D74-4F0E-9C1B-9820A0D1C35E}: component id of this circuit (vendor-specific, project-local). */
static const GUID AudRenderComponentGuid =
    { 0xb2c5e9a1, 0x3d74, 0x4f0e, { 0x9c, 0x1b, 0x98, 0x20, 0xa0, 0xd1, 0xc3, 0x5e } };

/* Must equal the KSNAME reference string in Exynos9820Audio.inf. */
DECLARE_CONST_UNICODE_STRING(AudRenderCircuitName, L"Speaker0");

#define AUD_ALL_CHANNELS       0xFFFFFFFFu
#define AUD_VOLUME_MIN         (-96 * 0x10000)   /* KS units, 1/65536 dB (sample VOLUME_LEVEL_MINIMUM) */
#define AUD_VOLUME_MAX         0                 /* never above 0 dB at the amp (amp notes Safety) */
#define AUD_VOLUME_STEP        0x8000            /* 0.5 dB */
#define AUD_HW_LOG_PACKETS     100u              /* hardware-vs-timer position line every 100 packets (~1 s at 10 ms) */

/* 48 kHz stereo PCM: 16-bit and 32-bit containers (Inc/AudioFormats.h Pcm48000c2 shape). */
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE AudFormatPcm16 = {
    {
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    {
        { WAVE_FORMAT_EXTENSIBLE, AUD_CHANNELS, AUD_RATE, AUD_RATE * 4, 4, 16,
          sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) },
        { 16 },
        KSAUDIO_SPEAKER_STEREO,
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
    }
};

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE AudFormatPcm32 = {
    {
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)
    },
    {
        { WAVE_FORMAT_EXTENSIBLE, AUD_CHANNELS, AUD_RATE, AUD_RATE * 8, 8, 32,
          sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) },
        { 32 },
        KSAUDIO_SPEAKER_STEREO,
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)
    }
};

EVT_ACX_CIRCUIT_CREATE_STREAM        AudEvtCircuitCreateStream;
EVT_ACX_STREAM_PREPARE_HARDWARE      AudEvtStreamPrepareHardware;
EVT_ACX_STREAM_RELEASE_HARDWARE      AudEvtStreamReleaseHardware;
EVT_ACX_STREAM_RUN                   AudEvtStreamRun;
EVT_ACX_STREAM_PAUSE                 AudEvtStreamPause;
EVT_ACX_STREAM_GET_HW_LATENCY        AudEvtStreamGetHwLatency;
EVT_ACX_STREAM_ALLOCATE_RTPACKETS    AudEvtStreamAllocateRtPackets;
EVT_ACX_STREAM_FREE_RTPACKETS        AudEvtStreamFreeRtPackets;
EVT_ACX_STREAM_SET_RENDER_PACKET     AudEvtStreamSetRenderPacket;
EVT_ACX_STREAM_GET_CURRENT_PACKET    AudEvtStreamGetCurrentPacket;
EVT_ACX_STREAM_GET_PRESENTATION_POSITION AudEvtStreamGetPresentationPosition;
EVT_ACX_RAMPED_VOLUME_ASSIGN_LEVEL   AudEvtVolumeAssignLevel;
EVT_ACX_VOLUME_RETRIEVE_LEVEL        AudEvtVolumeRetrieveLevel;
EVT_ACX_MUTE_ASSIGN_STATE            AudEvtMuteAssignState;
EVT_ACX_MUTE_RETRIEVE_STATE          AudEvtMuteRetrieveState;
EVT_WDF_TIMER                        AudEvtStreamTimer;
EVT_WDF_OBJECT_CONTEXT_CLEANUP       AudEvtStreamCleanup;

/* ================================================================================================================
 * Volume / mute elements (Parameters\HwVolume = 1 only).
 * ================================================================================================================ */

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtVolumeAssignLevel(ACXVOLUME Volume, ULONG Channel, LONG VolumeLevel, ACX_VOLUME_CURVE_TYPE CurveType,
                                       ULONGLONG CurveDuration)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Volume)->Dev;

    UNREFERENCED_PARAMETER(CurveType);
    UNREFERENCED_PARAMETER(CurveDuration);
    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    /* One level for both amps (each amp plays one slot of the stereo frame); the last assignment wins. */
    AudLog(Dev->Log, "volume ch %ld <- %ld/65536 dB\r\n", (LONG)Channel, VolumeLevel);
    return AmpSetVolume(Dev, VolumeLevel);
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtVolumeRetrieveLevel(ACXVOLUME Volume, ULONG Channel, LONG *VolumeLevel)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Volume)->Dev;

    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    *VolumeLevel = Dev->VolumeLevel;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtMuteAssignState(ACXMUTE Mute, ULONG Channel, ULONG State)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Mute)->Dev;

    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    Dev->Muted = (State != 0) ? 1 : 0;
    AudLog(Dev->Log, "mute ch %ld <- %lu\r\n", (LONG)Channel, State);
    return AmpMute(Dev, State != 0);
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtMuteRetrieveState(ACXMUTE Mute, ULONG Channel, ULONG *State)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Mute)->Dev;

    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    *State = Dev->Muted;
    return STATUS_SUCCESS;
}

static NTSTATUS AudCreateElements(_In_ ACXCIRCUIT Circuit, _In_ PAUD_DEVICE_CONTEXT Dev)
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
    VolCfg.Minimum       = AUD_VOLUME_MIN;
    VolCfg.Maximum       = AUD_VOLUME_MAX;
    VolCfg.SteppingDelta = AUD_VOLUME_STEP;
    VolCfg.Name          = &KSAUDFNAME_VOLUME_CONTROL;
    VolCfg.Callbacks     = &VolCb;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_ELEMENT_CONTEXT);
    Attr.ParentObject = Circuit;
    Status = AcxVolumeCreate(Circuit, &Attr, &VolCfg, (ACXVOLUME *)&Elements[0]);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    AudGetElementContext(Elements[0])->Dev = Dev;

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

    return AcxCircuitAddElements(Circuit, Elements, ARRAYSIZE(Elements));
}

/* ================================================================================================================
 * Circuit.
 * ================================================================================================================ */

static NTSTATUS AudAddFormat(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXDATAFORMATLIST List,
                             _In_ PKSDATAFORMAT_WAVEFORMATEXTENSIBLE Wave)
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

    return AcxDataFormatListAddDataFormat(List, Format);
}

/*
 * Host pin (sink, streaming) -> [volume -> mute, HwVolume = 1 only] -> bridge pin (source, KSNODETYPE_SPEAKER,
 * integrated jack). Created in EvtDeviceAdd, added to the device in PrepareHardware (the sample's split).
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AudRenderCircuitCreate(_In_ WDFDEVICE Device, _In_ PAUD_DEVICE_CONTEXT Dev)
{
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

    AcxCircuitInitSetComponentId(Init, &AudRenderComponentGuid);
    Status = AcxCircuitInitAssignName(Init, &AudRenderCircuitName);
    if (NT_SUCCESS(Status)) {
        AcxCircuitInitSetCircuitType(Init, AcxCircuitTypeRender);
        Status = AcxCircuitInitAssignAcxCreateStreamCallback(Init, AudEvtCircuitCreateStream);
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

    if (Dev->Cfg.HwVolume) {
        Status = AudCreateElements(Circuit, Dev);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    ACX_PIN_CONFIG_INIT(&PinCfg);
    PinCfg.Type          = AcxPinTypeSink;
    PinCfg.Communication = AcxPinCommunicationSink;
    PinCfg.Category      = &KSCATEGORY_AUDIO;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Circuit;
    Status = AcxPinCreate(Circuit, &Attr, &PinCfg, &Pins[0]);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ACX_PIN_CONFIG_INIT(&PinCfg);
    PinCfg.Type          = AcxPinTypeSource;
    PinCfg.Communication = AcxPinCommunicationNone;
    PinCfg.Category      = &KSNODETYPE_SPEAKER;
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
    WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
    Attr.ParentObject = Pins[1];
    Status = AcxJackCreate(Pins[1], &Attr, &JackCfg, &Jack);
    if (NT_SUCCESS(Status)) {
        Status = AcxPinAddJacks(Pins[1], &Jack, 1);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    List = AcxPinGetRawDataFormatList(Pins[0]);
    if (List == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = AudAddFormat(Device, Circuit, List, &AudFormatPcm16);
    if (NT_SUCCESS(Status)) {
        Status = AudAddFormat(Device, Circuit, List, &AudFormatPcm32);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = AcxCircuitAddPins(Circuit, Pins, ARRAYSIZE(Pins));
    if (NT_SUCCESS(Status)) {
        Dev->Render = Circuit;
    }

    return Status;
}

/* ================================================================================================================
 * Stream: ring, clock, state.
 * ================================================================================================================ */

static VOID AudRingClean(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONG Packet)
{
    if (Ctx->Dev->Cfg.CacheMaint && Packet < Ctx->PacketCount && Ctx->PacketMdl[Packet] != NULL) {
        KeFlushIoBuffers(Ctx->PacketMdl[Packet], FALSE, TRUE);
    }
}

static VOID AudRingCleanAll(_In_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONG I;

    for (I = 0; I < Ctx->PacketCount; I++) {
        AudRingClean(Ctx, I);
    }
}

static VOID AudRingFree(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONG I;

    for (I = 0; I < AUD_MAX_PACKETS; I++) {
        if (Ctx->PacketMdl[I] != NULL) {
            IoFreeMdl(Ctx->PacketMdl[I]);
            Ctx->PacketMdl[I] = NULL;
        }
    }

    if (Ctx->RingAlloc != NULL) {
        MmFreeContiguousMemory(Ctx->RingAlloc);
        Ctx->RingAlloc = NULL;
    }

    Ctx->PacketCount = 0;
    Ctx->RingBytes   = 0;
    Ctx->RingHwUsable = FALSE;
}

static ULONGLONG AudNowHns(_In_ PAUD_STREAM_CONTEXT Ctx)
{
    return KSCONVERT_PERFORMANCE_TIME(Ctx->QpcFrequency.QuadPart, KeQueryPerformanceCounter(NULL));
}

/* Sample CStreamEngine::UpdatePosition, glitch time subtracted as time. */
static VOID AudUpdatePosition(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONGLONG Elapsed;

    if (Ctx->State != AcxStreamStateRun) {
        return;
    }

    Elapsed = AudNowHns(Ctx) - Ctx->StartTime;
    Elapsed = (Elapsed > Ctx->GlitchAdjust) ? Elapsed - Ctx->GlitchAdjust : 0;
    Ctx->Position = Ctx->StartPosition + Elapsed * Ctx->BytesPerSec / AUD_HNS_PER_SEC;
}

/* Relative timer delay (negative HNS) until the current packet ends; >= 0 means the time already passed. */
static LONGLONG AudNextDelay(_In_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONGLONG NextStart = ((ULONGLONG)(ULONG)Ctx->CurrentPacket + 1) * Ctx->PacketSize;
    ULONGLONG FromPause = (NextStart > Ctx->StartPosition) ? NextStart - Ctx->StartPosition : 0;
    ULONGLONG When      = Ctx->StartTime + Ctx->GlitchAdjust + FromPause * AUD_HNS_PER_SEC / Ctx->BytesPerSec;

    return -(LONGLONG)(When - AudNowHns(Ctx));
}

_Use_decl_annotations_
VOID AudEvtStreamTimer(WDFTIMER Timer)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetTimerContext(Timer)->Stream;
    ULONG               Completed;
    LONGLONG            Delay;
    ULONG               HwOff;

    for (;;) {
        /* The packet the DMA moves to next was written by the client while this one played: push it to DRAM. */
        AudRingCleanAll(Ctx);

        Completed = (ULONG)InterlockedIncrement(&Ctx->CurrentPacket) - 1;
        (VOID)AcxRtStreamNotifyPacketComplete(Ctx->Stream, Completed, (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart);

        if (Ctx->HwStarted && (Completed % AUD_HW_LOG_PACKETS) == 0 && NT_SUCCESS(AboxFrameCount(Ctx->Dev, &HwOff))) {
            AudLog(Ctx->Dev->Log, "stream: packet %lu, RDMA0 frame counter 0x%03lX\r\n", Completed, HwOff);
        }

        Delay = AudNextDelay(Ctx);
        if (Delay < 0) {
            (VOID)WdfTimerStart(Timer, Delay);
            return;
        }

        /* Late (debugger, long DPC): absorb the lost time and complete the next packet now (sample glitch path). */
        Ctx->GlitchAdjust += (ULONGLONG)Delay;
        Ctx->Glitches++;
    }
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamAllocateRtPackets(ACXSTREAM Stream, ULONG PacketCount, ULONG PacketSize, PACX_RTPACKET *Packets)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    PHYSICAL_ADDRESS    Low;
    PHYSICAL_ADDRESS    High;
    PHYSICAL_ADDRESS    Skip;
    PACX_RTPACKET       P;
    ULONG               AllocPer;
    ULONG               I;

    PAGED_CODE();

    *Packets = NULL;
    if (PacketCount == 0 || PacketCount > AUD_MAX_PACKETS || PacketSize == 0 || PacketSize > (1u << 20)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->RingAlloc != NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    AllocPer = (ULONG)ROUND_TO_PAGES(PacketSize);
    Low.QuadPart  = 0;
    High.QuadPart = 0xFFFFFFFFLL;   /* RDMA BUF_STR/BUF_END are 32-bit SFRs */
    Skip.QuadPart = 0;
    Ctx->RingAllocBytes = (SIZE_T)AllocPer * PacketCount;
    Ctx->RingAlloc = (PUCHAR)MmAllocateContiguousNodeMemory(Ctx->RingAllocBytes, Low, High, Skip, PAGE_READWRITE,
                                                            MM_ANY_NODE_OK);
    if (Ctx->RingAlloc == NULL) {
        AudLog(Ctx->Dev->Log, "stream: no contiguous memory below 4 GiB for %lu x %lu B\r\n", PacketCount, PacketSize);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Ctx->RingAlloc, Ctx->RingAllocBytes);

    P = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * PacketCount, AUD_POOL_TAG);
    if (P == NULL) {
        AudRingFree(Ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ctx->PacketCount = PacketCount;
    Ctx->PacketSize  = PacketSize;
    Ctx->FirstPacketOffset = AllocPer - PacketSize;
    for (I = 0; I < PacketCount; I++) {
        Ctx->PacketMdl[I] = IoAllocateMdl(Ctx->RingAlloc + (SIZE_T)I * AllocPer, AllocPer, FALSE, FALSE, NULL);
        if (Ctx->PacketMdl[I] == NULL) {
            ExFreePoolWithTag(P, AUD_POOL_TAG);
            AudRingFree(Ctx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(Ctx->PacketMdl[I]);

        ACX_RTPACKET_INIT(&P[I]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&P[I].RtPacketBuffer, Ctx->PacketMdl[I], AllocPer);
        P[I].RtPacketOffset = (I == 0) ? Ctx->FirstPacketOffset : 0;
        P[I].RtPacketSize   = PacketSize;
    }

    Ctx->RingPhys = MmGetPhysicalAddress(Ctx->RingAlloc);
    Ctx->RingPhys.QuadPart += Ctx->FirstPacketOffset;
    Ctx->RingBytes    = PacketSize * PacketCount;
    Ctx->RingHwUsable = ((ULONGLONG)Ctx->RingPhys.QuadPart + Ctx->RingBytes <= 0x100000000ULL) &&
                        (Ctx->RingPhys.QuadPart % AUD_DMA_ALIGN) == 0 && (PacketSize % AUD_DMA_ALIGN) == 0;
    AudRingCleanAll(Ctx);

    AudLog(Ctx->Dev->Log, "stream: %lu packets x %lu B (%lu bit), ring phys 0x%I64X + 0x%lX, %s\r\n", PacketCount,
           PacketSize, Ctx->BitsPerSample, Ctx->RingPhys.QuadPart, Ctx->RingBytes,
           Ctx->RingHwUsable ? "RDMA-usable" : "NOT RDMA-usable (timer only)");
    *Packets = P;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID AudEvtStreamFreeRtPackets(ACXSTREAM Stream, PACX_RTPACKET Packets, ULONG PacketCount)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    UNREFERENCED_PARAMETER(PacketCount);
    PAGED_CODE();

    if (Ctx->HwStarted) {
        /* Never free memory a DMA may still read. */
        AmpPowerDown(Ctx->Dev);
        AboxStop(Ctx->Dev);
        Ctx->HwStarted = FALSE;
    }

    AudRingFree(Ctx);
    if (Packets != NULL) {
        ExFreePoolWithTag(Packets, AUD_POOL_TAG);
    }
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamPrepareHardware(ACXSTREAM Stream)
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

    WDF_TIMER_CONFIG_INIT(&TimerCfg, AudEvtStreamTimer);
    TimerCfg.AutomaticSerialization = TRUE;
    TimerCfg.UseHighResolutionTimer = WdfTrue;
    TimerCfg.Period = 0;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_TIMER_CONTEXT);
    Attr.ParentObject = Stream;
    Status = WdfTimerCreate(&TimerCfg, &Attr, &Ctx->Timer);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    AudGetTimerContext(Ctx->Timer)->Stream = Ctx;

    Ctx->State = AcxStreamStatePause;
    AudLog(Ctx->Dev->Log, "stream: prepared (%lu Hz, %lu bit, block %lu)\r\n", AUD_RATE, Ctx->BitsPerSample, Ctx->BlockAlign);
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamReleaseHardware(ACXSTREAM Stream)
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

    if (Ctx->HwStarted) {
        AmpPowerDown(Ctx->Dev);
        AboxStop(Ctx->Dev);
        Ctx->HwStarted = FALSE;
    }

    AudLog(Ctx->Dev->Log, "stream: released after %lu packets, %lu glitches, %lu late writes\r\n",
           (ULONG)Ctx->CurrentPacket, Ctx->Glitches, Ctx->LateWrites);
    Ctx->Position      = 0;
    Ctx->GlitchAdjust  = 0;
    Ctx->CurrentPacket = 0;
    Ctx->State         = AcxStreamStateStop;
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

/*
 * Hardware start for one Run. Order: stage-1 clocks (the amp PLL locks on BCLK, amp notes row 6.1), then the amps
 * (PUP_DONE poll, up to 100 ms), and the RDMA + SPK_ENABLE LAST, so the caller can take the timer start time right
 * after the DMA starts - the timer clock tells the client which packet to fill and must not lag the DMA.
 * Any failure leaves the timer-only stream running.
 */
static VOID AudHwStart(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    PAUD_DEVICE_CONTEXT Dev = Ctx->Dev;
    NTSTATUS            Status;
    NTSTATUS            AmpStatus;

    if (!Dev->Cfg.AboxEnable) {
        return;
    }
    if (!Ctx->RingHwUsable) {
        AudLog(Dev->Log, "stream: ring not RDMA-usable - hardware not started\r\n");
        return;
    }

    Status = AboxInit(Dev);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "stream: ABOX stage 1 not ready (%08lX) - timer-only\r\n", Status);
        return;
    }

    AmpStatus = AmpPowerUp(Dev);
    Status = AboxStart(Dev, Ctx->RingPhys, Ctx->RingBytes, Ctx->PacketSize, AUD_RATE, AUD_CHANNELS, Ctx->BitsPerSample);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "stream: ABOX DMA not started (%08lX) - amps back off, timer-only\r\n", Status);
        AmpPowerDown(Dev);
        return;
    }

    Ctx->HwStarted = TRUE;
    AudLog(Dev->Log, "stream: ABOX running, amps %08lX\r\n", AmpStatus);
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamRun(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    LONGLONG            Delay;

    PAGED_CODE();

    if (Ctx->State == AcxStreamStateRun) {
        return STATUS_SUCCESS;
    }
    if (Ctx->State != AcxStreamStatePause || Ctx->PacketCount == 0) {
        return STATUS_INVALID_STATE_TRANSITION;
    }

    AudRingCleanAll(Ctx);
    AudHwStart(Ctx);

    Ctx->StartTime     = AudNowHns(Ctx);
    Ctx->StartPosition = Ctx->Position;
    Ctx->GlitchAdjust  = 0;
    Ctx->State         = AcxStreamStateRun;
    Delay = AudNextDelay(Ctx);
    (VOID)WdfTimerStart(Ctx->Timer, (Delay < 0) ? Delay : -1);

    AudLog(Ctx->Dev->Log, "stream: run at packet %lu, position %I64u B, hardware %s\r\n", (ULONG)Ctx->CurrentPacket,
           Ctx->Position, Ctx->HwStarted ? "on" : "off (timer clock only)");
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamPause(ACXSTREAM Stream)
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
    AudUpdatePosition(Ctx);
    Ctx->State = AcxStreamStatePause;

    /* The RDMA would loop over stale packets while paused: stop the hardware, restart it on Run. */
    if (Ctx->HwStarted) {
        AmpPowerDown(Ctx->Dev);
        AboxStop(Ctx->Dev);
        Ctx->HwStarted = FALSE;
    }

    AudLog(Ctx->Dev->Log, "stream: pause at packet %lu, position %I64u B, %lu glitches\r\n", (ULONG)Ctx->CurrentPacket,
           Ctx->Position, Ctx->Glitches);
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamGetHwLatency(ACXSTREAM Stream, ULONG *FifoSize, ULONG *Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    PAGED_CODE();

    *FifoSize = 128;   /* sample value; the RDMA FIFO depth is unknown (stage 2) */
    *Delay    = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamSetRenderPacket(ACXSTREAM Stream, ULONG Packet, ULONG Flags, ULONG EosPacketLength)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    ULONG               Current;

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(EosPacketLength);
    PAGED_CODE();

    if (Ctx->PacketCount == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    AudRingClean(Ctx, Packet % Ctx->PacketCount);
    Current = (ULONG)InterlockedCompareExchange(&Ctx->CurrentPacket, -1, -1);
    if (Packet <= Current) {
        Ctx->LateWrites++;
        return STATUS_DATA_LATE_ERROR;
    }
    if (Packet > Current + 1) {
        return STATUS_DATA_OVERRUN;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamGetCurrentPacket(ACXSTREAM Stream, PULONG CurrentPacket)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    *CurrentPacket = (ULONG)InterlockedCompareExchange(&Ctx->CurrentPacket, -1, -1);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamGetPresentationPosition(ACXSTREAM Stream, PULONGLONG PositionInBlocks, PULONGLONG QPCPosition)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    AudUpdatePosition(Ctx);
    *PositionInBlocks = Ctx->Position / Ctx->BlockAlign;
    *QPCPosition      = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID AudEvtStreamCleanup(WDFOBJECT Object)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext((ACXSTREAM)Object);

    /* FreeRtPackets normally ran already; this only catches a stream torn down without it. */
    if (Ctx->HwStarted) {
        AmpPowerDown(Ctx->Dev);
        AboxStop(Ctx->Dev);
        Ctx->HwStarted = FALSE;
    }
    AudRingFree(Ctx);
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtCircuitCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN Pin, PACXSTREAM_INIT StreamInit,
                                         ACXDATAFORMAT StreamFormat, const GUID *SignalProcessingMode,
                                         ACXOBJECTBAG VarArguments)
{
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
    Cb.EvtAcxStreamPrepareHardware = AudEvtStreamPrepareHardware;
    Cb.EvtAcxStreamReleaseHardware = AudEvtStreamReleaseHardware;
    Cb.EvtAcxStreamRun             = AudEvtStreamRun;
    Cb.EvtAcxStreamPause           = AudEvtStreamPause;
    Status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &Cb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    ACX_RT_STREAM_CALLBACKS_INIT(&Rt);
    Rt.EvtAcxStreamGetHwLatency            = AudEvtStreamGetHwLatency;
    Rt.EvtAcxStreamAllocateRtPackets       = AudEvtStreamAllocateRtPackets;
    Rt.EvtAcxStreamFreeRtPackets           = AudEvtStreamFreeRtPackets;
    Rt.EvtAcxStreamSetRenderPacket         = AudEvtStreamSetRenderPacket;
    Rt.EvtAcxStreamGetCurrentPacket        = AudEvtStreamGetCurrentPacket;
    Rt.EvtAcxStreamGetPresentationPosition = AudEvtStreamGetPresentationPosition;
    Status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &Rt);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_STREAM_CONTEXT);
    Attr.EvtCleanupCallback = AudEvtStreamCleanup;
    Status = AcxRtStreamCreate(Device, Circuit, &Attr, &StreamInit, &Stream);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx = AudGetStreamContext(Stream);
    Ctx->Dev         = AudGetCircuitContext(Circuit)->Dev;
    Ctx->Stream      = Stream;
    Ctx->Format      = StreamFormat;
    Ctx->BlockAlign  = AcxDataFormatGetBlockAlign(StreamFormat);
    Ctx->BytesPerSec = AcxDataFormatGetAverageBytesPerSec(StreamFormat);
    Wfx = (PWAVEFORMATEXTENSIBLE)AcxDataFormatGetWaveFormatExtensible(StreamFormat);
    Ctx->BitsPerSample = (Wfx != NULL) ? Wfx->Format.wBitsPerSample : 0;
    Ctx->State = AcxStreamStateStop;
    KeQueryPerformanceCounter(&Ctx->QpcFrequency);

    if (Ctx->BlockAlign == 0 || Ctx->BytesPerSec == 0) {
        return STATUS_INVALID_PARAMETER;   /* the stream object is deleted by ACX on failure */
    }

    AudLog(Ctx->Dev->Log, "stream: created, %lu B/s, block %lu, %lu bit\r\n", Ctx->BytesPerSec, Ctx->BlockAlign,
           Ctx->BitsPerSample);
    return STATUS_SUCCESS;
}
