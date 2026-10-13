/*
 * Render.c - the ACX render circuit "Speaker0" and its WaveRT (ACX RT) stream.
 *
 * Shape: Microsoft Windows-driver-samples audio/Acx/Samples (tree 2dc3fd3a): Common/RenderCircuit.cpp
 * (circuit, host + bridge pin, jack, formats) and Common/StreamEngine.cpp (RT packets, one-shot high-resolution
 * WDF timer per packet). Differences, each for a reason in notes/d8-audio-win-m1.md:
 *   - the endpoint is 48 kHz stereo 16/32-bit, but the validated UAIF1 frame is 4 x 32-bit slots, so the packet timer
 *     expands each client packet into a separate non-cached DMA ring one period ahead of the RDMA (see "Stream");
 *   - the packet clock is the RDMA frame counter while the hardware runs (QPC otherwise);
 *   - volume + mute elements always (the sample's topology; A5: the zero-element circuit is the suspected cause of the
 *     missing mix format - not device-proven, notes/d8-audio-win-a5.md). Endpoint volume (slider, flyout, volume keys) lands on them: HwVolume = 0 (default)
 *     applies it in software while expanding packets into the DMA ring, HwVolume = 1 on the amp registers.
 *
 * The hardware sequence is device-validated in UEFI (boot #99); this driver is NOT VALIDATED UNDER WINDOWS.
 */

#include "Exynos9820Audio.h"

/* {B2C5E9A1-3D74-4F0E-9C1B-9820A0D1C35E}: component id of this circuit (vendor-specific, project-local). */
static const GUID AudRenderComponentGuid =
    { 0xb2c5e9a1, 0x3d74, 0x4f0e, { 0x9c, 0x1b, 0x98, 0x20, 0xa0, 0xd1, 0xc3, 0x5e } };

/* AUDIO_SIGNALPROCESSINGMODE_DEFAULT {C18E2F7E-933D-4965-B7D1-1EEF228D2AF3} (ksmedia.h:8424), local copy. */
static const GUID AudModeDefault =
    { 0xc18e2f7e, 0x933d, 0x4965, { 0xb7, 0xd1, 0x1e, 0xef, 0x22, 0x8d, 0x2a, 0xf3 } };

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

/* The pin's formats for ReqLog.c's intersection: 32 -> AudFormatPcm32, anything else -> AudFormatPcm16. */
const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *AudHostFormat(_In_ ULONG Bits)
{
    return (Bits == 32) ? &AudFormatPcm32 : &AudFormatPcm16;
}

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
EVT_ACX_CIRCUIT_POWER_UP             AudEvtCircuitPowerUp;
EVT_ACX_CIRCUIT_POWER_DOWN           AudEvtCircuitPowerDown;
EVT_WDF_OBJECT_CONTEXT_CLEANUP       AudEvtStreamCleanup;

/* ================================================================================================================
 * Volume / mute elements. HwVolume = 1: the CS35L40 digital volume / mute (AmpI2c.c). HwVolume = 0 (default): the
 * driver's software gain (AudDmaFillSlot); the amps stay at their fixed level and unmuted.
 * ================================================================================================================ */

/* Q30 gain for 0 .. -96 dB in AUD_VOLUME_STEP (0.5 dB) steps: round(2^30 * 10^(-n/40)), n = 0..192 (computed, not a
   hardware value). */
#define AUD_SW_GAIN_STEPS  193u
static const LONG AudGainQ30[AUD_SW_GAIN_STEPS] = {
    1073741824, 1013677647, 956973408, 903441154, 852903448, 805192776, 760150998, 717628817,
    677485290, 639587356, 603809400, 570032831, 538145694, 508042296, 479622855, 452793173,
    427464319, 403552340, 380977976, 359666402, 339546978, 320553018, 302621563, 285693178,
    269711752, 254624313, 240380852, 226934158, 214239660, 202255281, 190941298, 180260209,
    170176611, 160657080, 151670064, 143185773, 135176087, 127614455, 120475814, 113736503,
    107374182, 101367765, 95697341, 90344115, 85290345, 80519278, 76015100, 71762882,
    67748529, 63958736, 60380940, 57003283, 53814569, 50804230, 47962285, 45279317,
    42746432, 40355234, 38097798, 35966640, 33954698, 32055302, 30262156, 28569318,
    26971175, 25462431, 24038085, 22693416, 21423966, 20225528, 19094130, 18026021,
    17017661, 16065708, 15167006, 14318577, 13517609, 12761445, 12047581, 11373650,
    10737418, 10136776, 9569734, 9034412, 8529034, 8051928, 7601510, 7176288,
    6774853, 6395874, 6038094, 5700328, 5381457, 5080423, 4796229, 4527932,
    4274643, 4035523, 3809780, 3596664, 3395470, 3205530, 3026216, 2856932,
    2697118, 2546243, 2403809, 2269342, 2142397, 2022553, 1909413, 1802602,
    1701766, 1606571, 1516701, 1431858, 1351761, 1276145, 1204758, 1137365,
    1073742, 1013678, 956973, 903441, 852903, 805193, 760151, 717629,
    677485, 639587, 603809, 570033, 538146, 508042, 479623, 452793,
    427464, 403552, 380978, 359666, 339547, 320553, 302622, 285693,
    269712, 254624, 240381, 226934, 214240, 202255, 190941, 180260,
    170177, 160657, 151670, 143186, 135176, 127614, 120476, 113737,
    107374, 101368, 95697, 90344, 85290, 80519, 76015, 71763,
    67749, 63959, 60381, 57003, 53815, 50804, 47962, 45279,
    42746, 40355, 38098, 35967, 33955, 32055, 30262, 28569,
    26971, 25462, 24038, 22693, 21424, 20226, 19094, 18026,
    17018
};

static LONG AudSwGainForLevel(_In_ LONG LevelKs)
{
    LONG Steps;

    if (LevelKs >= AUD_VOLUME_MAX) {
        return AudGainQ30[0];
    }
    if (LevelKs <= AUD_VOLUME_MIN) {
        return AudGainQ30[AUD_SW_GAIN_STEPS - 1];
    }
    Steps = (-LevelKs + AUD_VOLUME_STEP / 2) / AUD_VOLUME_STEP;   /* nearest 0.5 dB */
    return AudGainQ30[(Steps < (LONG)AUD_SW_GAIN_STEPS) ? Steps : (LONG)AUD_SW_GAIN_STEPS - 1];
}

static VOID AudSwSetLevel(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ ULONG Channel, _In_ LONG LevelKs)
{
    if (LevelKs > AUD_VOLUME_MAX) {
        LevelKs = AUD_VOLUME_MAX;
    }
    if (LevelKs < AUD_VOLUME_MIN) {
        LevelKs = AUD_VOLUME_MIN;
    }
    Dev->SwLevel[Channel] = LevelKs;
    InterlockedExchange(&Dev->SwGainQ30[Channel], AudSwGainForLevel(LevelKs));
}

/* Device creation: both channels at LevelKs until Windows assigns the endpoint's persisted level; unmuted. */
VOID AudSwVolumeInit(_Inout_ PAUD_DEVICE_CONTEXT Dev, _In_ LONG LevelKs)
{
    ULONG Ch;

    for (Ch = 0; Ch < AUD_CHANNELS; Ch++) {
        AudSwSetLevel(Dev, Ch, LevelKs);
    }
    InterlockedExchange(&Dev->SwMuted, 0);
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtVolumeAssignLevel(ACXVOLUME Volume, ULONG Channel, LONG VolumeLevel, ACX_VOLUME_CURVE_TYPE CurveType,
                                       ULONGLONG CurveDuration)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Volume)->Dev;
    ULONG               Ch;

    UNREFERENCED_PARAMETER(CurveType);
    UNREFERENCED_PARAMETER(CurveDuration);
    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    AudLog(Dev->Log, "volume ch %ld <- %ld/65536 dB (%s)\r\n", (LONG)Channel, VolumeLevel,
           Dev->Cfg.HwVolume ? "amp" : "software");
    if (Dev->Cfg.HwVolume) {
        /* One level for both amps (each amp plays one slot of the stereo frame); the last assignment wins. */
        return AmpSetVolume(Dev, VolumeLevel);
    }

    for (Ch = 0; Ch < AUD_CHANNELS; Ch++) {
        if (Channel == AUD_ALL_CHANNELS || Channel == Ch) {
            AudSwSetLevel(Dev, Ch, VolumeLevel);
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtVolumeRetrieveLevel(ACXVOLUME Volume, ULONG Channel, LONG *VolumeLevel)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Volume)->Dev;

    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Dev->Cfg.HwVolume) {
        *VolumeLevel = Dev->VolumeLevel;
    } else {
        *VolumeLevel = Dev->SwLevel[(Channel == AUD_ALL_CHANNELS) ? 0 : Channel];   /* sample: channel 0 for "all" */
    }
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

    /* One mute state for the endpoint (the endpoint mute is not per channel); the last assignment wins. */
    AudLog(Dev->Log, "mute ch %ld <- %lu (%s)\r\n", (LONG)Channel, State, Dev->Cfg.HwVolume ? "amp" : "software");
    if (Dev->Cfg.HwVolume) {
        Dev->Muted = (State != 0) ? 1 : 0;
        return AmpMute(Dev, State != 0);
    }

    InterlockedExchange(&Dev->SwMuted, (State != 0) ? 1 : 0);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtMuteRetrieveState(ACXMUTE Mute, ULONG Channel, ULONG *State)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetElementContext(Mute)->Dev;

    PAGED_CODE();

    if (Channel != AUD_ALL_CHANNELS && Channel >= AUD_CHANNELS) {
        return STATUS_INVALID_PARAMETER;
    }

    *State = Dev->Cfg.HwVolume ? Dev->Muted : (ULONG)Dev->SwMuted;
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

    Status = AcxCircuitAddElements(Circuit, Elements, ARRAYSIZE(Elements));
    if (NT_SUCCESS(Status)) {
        Dev->VolumeElementId = AcxElementGetId(Elements[0]);   /* ReqLog.c CHANNEL_CONFIG target (review R7) */
    }
    return Status;
}

/* ================================================================================================================
 * Circuit.
 * ================================================================================================================ */

static NTSTATUS AudAddFormat(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXDATAFORMATLIST List,
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

    /* AcxDataFormatListAssignDefaultDataFormat adds the format and makes it the default ("The default format must be
       present in the list, else it will be added", MS doc "ACX audio data formats"). */
    return Default ? AcxDataFormatListAssignDefaultDataFormat(List, Format) : AcxDataFormatListAddDataFormat(List, Format);
}

/*
 * Host pin (sink, streaming) -> volume -> mute -> bridge pin (source, KSNODETYPE_SPEAKER, integrated jack): exactly the
 * ACX AudioCodec sample (Common/RenderCircuit.cpp CodecR_CreateRenderCircuit), connections by ACX's default chain.
 * Created in EvtDeviceAdd, added to the device in PrepareHardware (the sample's split).
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
    ACX_CIRCUIT_PNPPOWER_CALLBACKS PowerCb;
    NTSTATUS              PreStatus;

    PAGED_CODE();

    Init = AcxCircuitInitAllocate(Device);
    if (Init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    AcxCircuitInitSetComponentId(Init, &AudRenderComponentGuid);
    Status = AcxCircuitInitAssignName(Init, &AudRenderCircuitName);
    if (NT_SUCCESS(Status)) {
        AcxCircuitInitSetCircuitType(Init, AcxCircuitTypeRender);
        ACX_CIRCUIT_PNPPOWER_CALLBACKS_INIT(&PowerCb);
        PowerCb.EvtAcxCircuitPowerUp   = AudEvtCircuitPowerUp;
        PowerCb.EvtAcxCircuitPowerDown = AudEvtCircuitPowerDown;
        AcxCircuitInitSetAcxCircuitPnpPowerCallbacks(Init, &PowerCb);
        Status = AcxCircuitInitAssignAcxCreateStreamCallback(Init, AudEvtCircuitCreateStream);
    }
    if (!NT_SUCCESS(Status)) {
        AcxCircuitInitFree(Init);
        return Status;
    }

    /* A6 diagnostics: every KS request on the circuit (filter + pin factories + elements; ReqLog.c). A refusal only
       costs the log. */
    PreStatus = AcxCircuitInitAssignAcxRequestPreprocessCallback(Init, AudEvtCircuitRequestPreprocess, (ACXCONTEXT)Dev,
                                                                 AcxRequestTypeAny, NULL, AcxItemIdNone);
    AudLog(Dev->Log, "circuit: request preprocess hook %08lX\r\n", PreStatus);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, AUD_CIRCUIT_CONTEXT);
    Status = AcxCircuitCreate(Device, &Attr, &Init, &Circuit);
    if (!NT_SUCCESS(Status)) {
        AcxCircuitInitFree(Init);
        return Status;
    }
    AudGetCircuitContext(Circuit)->Dev = Dev;

    /* Always (A5): the sample's topology (suspected: the host pin must reach the bridge pin through elements for the
       endpoint to get a format) and the carrier of the endpoint volume / mute. */
    Status = AudCreateElements(Circuit, Dev);
    if (!NT_SUCCESS(Status)) {
        return Status;
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
    Status = AudAddFormat(Device, Circuit, List, &AudFormatPcm16, TRUE);   /* default; = INF PKEY_AudioEngine_OEMFormat */
    if (NT_SUCCESS(Status)) {
        Status = AudAddFormat(Device, Circuit, List, &AudFormatPcm32, FALSE);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* A10 (Parameters\DefaultMode = 1, default): the same two formats as an AUDIO_SIGNALPROCESSINGMODE_DEFAULT list, so
       the host pin offers RAW + DEFAULT (the shared engine streams in DEFAULT mode when the pin has it). Optional:
       a failure is logged and the pin keeps RAW only (the a9 shape). */
    if (Dev->Cfg.DefaultMode) {
        ACX_DATAFORMAT_LIST_CONFIG ListCfg;
        ACXDATAFORMATLIST          ModeList = NULL;
        NTSTATUS                   ModeStatus;

        ACX_DATAFORMAT_LIST_CONFIG_INIT(&ListCfg);
        WDF_OBJECT_ATTRIBUTES_INIT(&Attr);
        Attr.ParentObject = Pins[0];
        ModeStatus = AcxDataFormatListCreate(Device, &Attr, &ListCfg, &ModeList);
        if (NT_SUCCESS(ModeStatus)) {
            ModeStatus = AudAddFormat(Device, Circuit, ModeList, &AudFormatPcm16, TRUE);
        }
        if (NT_SUCCESS(ModeStatus)) {
            ModeStatus = AudAddFormat(Device, Circuit, ModeList, &AudFormatPcm32, FALSE);
        }
        if (NT_SUCCESS(ModeStatus)) {
            ModeStatus = AcxPinAssignModeDataFormatList(Pins[0], &AudModeDefault, ModeList);
        }
        AudLog(Dev->Log, "circuit: DEFAULT-mode format list on the host pin: %08lX\r\n", ModeStatus);
    }

    {
        /* A8: what ACX itself holds as the host pin's default (it answers PROPOSEDATAFORMAT get with it). */
        ACXDATAFORMAT     Def = NULL;
        NTSTATUS          DefStatus = AcxDataFormatListRetrieveDefaultDataFormat(List, &Def);
        const KSDATAFORMAT *Ks = (NT_SUCCESS(DefStatus) && Def != NULL) ? (const KSDATAFORMAT *)AcxDataFormatGetKsDataFormat(Def)
                                                                       : NULL;

        if (Ks != NULL) {
            AudLog(Dev->Log, "circuit: ACX default format %lu Hz %lu ch %lu/%lu bit align %lu mask %lX; KSDATAFORMAT size %lu flags %lX "
                   "samplesize %lu sub {%08lX} spec {%08lX}\r\n", AcxDataFormatGetSampleRate(Def), AcxDataFormatGetChannelsCount(Def),
                   AcxDataFormatGetBitsPerSample(Def), AcxDataFormatGetValidBitsPerSample(Def), AcxDataFormatGetBlockAlign(Def),
                   AcxDataFormatGetChannelMask(Def), Ks->FormatSize, Ks->Flags, Ks->SampleSize, Ks->SubFormat.Data1,
                   Ks->Specifier.Data1);
        } else {
            AudLog(Dev->Log, "circuit: ACX default format not retrievable %08lX\r\n", DefStatus);
        }
    }

    Status = AcxCircuitAddPins(Circuit, Pins, ARRAYSIZE(Pins));
    if (NT_SUCCESS(Status)) {
        Dev->Render    = Circuit;
        Dev->HostPinId = AcxPinGetId(Pins[0]);
        AudLog(Dev->Log, "circuit: host pin id %lu, bridge pin id %lu, volume element id %lu, KsOverride %lu\r\n",
               Dev->HostPinId, AcxPinGetId(Pins[1]), Dev->VolumeElementId, Dev->Cfg.KsOverride);
    }

    return Status;
}

/* ================================================================================================================
 * Stream: client packets, DMA ring, clock, state.
 *
 * Client side (what ACX maps to audiodg): <= 2 packets of 48 kHz stereo 16/32-bit, CPU-only memory.
 * Hardware side: a NON-CACHED contiguous ring below 4 GiB of AUD_DMA_SLOTS periods, each period = one packet's
 * frames x 16 bytes (UAIF1 frame: 4 x 32-bit slots, left-justified; slot 1 = loudspeaker amp, slot 0 = earpiece amp).
 * Non-cached: the CPU writes it, only the RDMA reads it, so no cache maintenance and no aliasing with a user mapping.
 *
 * Timeline (per Run, j = periods elapsed, k0 = CurrentPacket at Run):
 *   at Run       : ring slots zeroed, packet k0 expanded into slot 1, RDMA started at slot 0 (one period of silence);
 *   boundary j>=1: packet k0+j-1 is complete (the RDMA is now playing it), CurrentPacket = k0+j, and packet k0+j
 *                  (written by the client during the previous period) is expanded into slot (j+1) % AUD_DMA_SLOTS,
 *                  one full period before the RDMA reaches it.
 * Boundaries come from the RDMA ring position (STATUS[27:12] + [11:0], AboxRingPosition) while the hardware runs, so the client
 * follows the audio PLL, not the QPC; without hardware (disabled or failed) the QPC drives the same timeline.
 * ================================================================================================================ */

#define AUD_DMA_SLOTS        4u     /* periods in the DMA ring: reading j, writing j+1, two spare */

static PUCHAR AudPacketVa(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONG Packet)
{
    ULONG Index = Packet % Ctx->PacketCount;

    return Ctx->PacketBuf[Index] + ((Index == 0) ? Ctx->FirstPacketOffset : 0);
}

/* Fill one DMA slot. Explicit 32-bit volatile stores: the ring is non-cached (possibly Device memory on ARM64), where
   the DC ZVA / unaligned accesses a memset/memcpy may emit would fault. */
static VOID AudDmaFillSlot(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONG Slot, _In_opt_ PUCHAR Src)
{
    volatile LONG *Dst = (volatile LONG *)(Ctx->DmaVa + (SIZE_T)(Slot % AUD_DMA_SLOTS) * Ctx->DmaSlotBytes);
    PAUD_DEVICE_CONTEXT Dev = Ctx->Dev;
    ULONG          F;
    LONG           L;
    LONG           R;
    LONG           GainL = 1 << 30;
    LONG           GainR = 1 << 30;

    /* Endpoint volume / mute in software (HwVolume = 0), sampled once per period. */
    if (!Dev->Cfg.HwVolume) {
        if (Dev->SwMuted) {
            Src = NULL;
        } else {
            GainL = Dev->SwGainQ30[0];
            GainR = Dev->SwGainQ30[1];
        }
    }

    for (F = 0; F < Ctx->FramesPerPacket; F++) {
        L = 0;
        R = 0;
        if (Src != NULL) {
            if (Ctx->BitsPerSample == 16) {
                L = (LONG)((PSHORT)Src)[2 * F] * 65536;          /* left-justified in the 32-bit slot */
                R = (LONG)((PSHORT)Src)[2 * F + 1] * 65536;
            } else {
                L = ((PLONG)Src)[2 * F];
                R = ((PLONG)Src)[2 * F + 1];
            }
            /* Q30 gain <= 1.0: |result| <= |sample|, no overflow; arithmetic shift of the 64-bit product. */
            L = (LONG)(((LONGLONG)L * GainL) >> 30);
            R = (LONG)(((LONGLONG)R * GainR) >> 30);
        }

        if (Ctx->Dev->Cfg.Stereo) {
            Dst[4 * F + 0] = L;                                  /* earpiece amp @0x41 */
            Dst[4 * F + 1] = R;                                  /* loudspeaker amp @0x40 */
        } else {
            Dst[4 * F + 0] = 0;
            Dst[4 * F + 1] = (L / 2) + (R / 2);                  /* mono mix on the loudspeaker */
        }
        Dst[4 * F + 2] = 0;
        Dst[4 * F + 3] = 0;
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
        if (Ctx->PacketBuf[I] != NULL) {
            ExFreePoolWithTag(Ctx->PacketBuf[I], AUD_POOL_TAG);
            Ctx->PacketBuf[I] = NULL;
        }
    }

    if (Ctx->DmaVa != NULL) {
        if (Ctx->Dev->AboxDmaStuck) {
            /* The RDMA never reported idle: leaking 4 periods beats a DMA reading freed memory (UEFI :621-625). */
            AudLog(Ctx->Dev->Log, "stream: RDMA0 still busy - DMA ring 0x%I64X leaked\r\n", Ctx->DmaPhys.QuadPart);
        } else {
            MmFreeContiguousMemory(Ctx->DmaVa);
        }
        Ctx->DmaVa = NULL;
    }

    Ctx->PacketCount = 0;
}

/* Callback trace (runs audB4, A6): one line per stream callback with its result, capped per stream. Flushed when Flush is
   set (rare callbacks), on an error status, and every 16th line - frequent position/packet callbacks stay cheap. */
#define AUD_TRACE_MAX 400
#define AUD_TRACE(Ctx, Flush, Status, ...)                                                        \
    do {                                                                                          \
        if ((Ctx)->Traces < AUD_TRACE_MAX) {                                                      \
            (Ctx)->Traces++;                                                                      \
            AudLog((Ctx)->Dev->Log, __VA_ARGS__);                                                 \
            if (KeGetCurrentIrql() == PASSIVE_LEVEL &&                                            \
                ((Flush) || NT_ERROR(Status) || ((Ctx)->Traces % 16) == 0)) {                     \
                AudLogFlush((Ctx)->Dev->Log);                                                     \
            }                                                                                     \
        }                                                                                         \
    } while (0)

static ULONGLONG AudNowHns(_In_ PAUD_STREAM_CONTEXT Ctx)
{
    return KSCONVERT_PERFORMANCE_TIME(Ctx->QpcFrequency.QuadPart, KeQueryPerformanceCounter(NULL));
}

/* Frames since this Run: RDMA ring position accumulated modulo the ring (must be sampled at least once per ring =
   AUD_DMA_SLOTS periods - the packet timer samples every period) while the hardware runs, else QPC. <= DISPATCH. */
static ULONGLONG AudElapsedFrames(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONG     Count;
    ULONG     Raw;
    ULONGLONG Frames;
    KIRQL     OldIrql;

    if (!Ctx->HwStarted) {
        return (AudNowHns(Ctx) - Ctx->StartTime) * AUD_RATE / AUD_HNS_PER_SEC;
    }

    KeAcquireSpinLock(&Ctx->HwLock, &OldIrql);
    /* A12: the RDMA reports its position in the RING (AboxRingPosition), so the advance is taken modulo the ring
       size; the timer samples every period and the ring holds AUD_DMA_SLOTS periods, so a wrap is never missed. */
    if (NT_SUCCESS(AboxRingPosition(Ctx->Dev, &Count, &Raw))) {
        ULONG RingFrames = Ctx->DmaBytes / AUD_HW_FRAME_BYTES;

        Ctx->HwLastRaw = Raw;
        if (RingFrames != 0 && Count < RingFrames) {
            Ctx->HwFrames += (Count + RingFrames - Ctx->HwLastPos) % RingFrames;
            Ctx->HwLastPos = Count;
        } else {
            Ctx->HwBadPos++;
        }
    }
    Frames = Ctx->HwFrames;
    KeReleaseSpinLock(&Ctx->HwLock, OldIrql);
    return Frames;
}

/* Bytes the listener has heard since the stream started (the silent first period of a hardware run excluded). */
static VOID AudUpdatePosition(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    ULONGLONG Frames;

    if (Ctx->State != AcxStreamStateRun) {
        return;
    }

    Frames = AudElapsedFrames(Ctx);
    if (Ctx->HwStarted) {
        Frames = (Frames > Ctx->FramesPerPacket) ? Frames - Ctx->FramesPerPacket : 0;
    }
    Ctx->Position = Ctx->StartPosition + Frames * Ctx->BlockAlign;
}

/* Relative timer due time (negative HNS) for the next boundary; never shorter than 1 ms. */
static LONGLONG AudNextDue(_In_ PAUD_STREAM_CONTEXT Ctx, _In_ ULONGLONG Frames)
{
    ULONGLONG Next = ((ULONGLONG)Ctx->Periods + 1) * Ctx->FramesPerPacket;
    ULONGLONG Hns  = (Next > Frames) ? (Next - Frames) * AUD_HNS_PER_SEC / AUD_RATE : 0;

    if (Ctx->HwStarted) {
        Hns += 5000;                                   /* 0.5 ms past the boundary: no wake-up just before it */
    }
    return -(LONGLONG)((Hns < 10000) ? 10000 : Hns);
}

_Use_decl_annotations_
VOID AudEvtStreamTimer(WDFTIMER Timer)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetTimerContext(Timer)->Stream;
    ULONGLONG           Frames;
    ULONG               Done = 0;
    ULONG               Completed;

    Frames = AudElapsedFrames(Ctx);
    while (Frames >= ((ULONGLONG)Ctx->Periods + 1) * Ctx->FramesPerPacket) {
        Ctx->Periods++;
        Completed = (ULONG)InterlockedIncrement(&Ctx->CurrentPacket) - 1;
        if (Ctx->HwStarted) {
            AudDmaFillSlot(Ctx, Ctx->Periods + 1, AudPacketVa(Ctx, Completed + 1));
            KeMemoryBarrier();
        }
        (VOID)AcxRtStreamNotifyPacketComplete(Ctx->Stream, Completed, (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart);
        Done++;

        if (Ctx->HwStarted && (Completed % AUD_HW_LOG_PACKETS) == 0) {
            AudLog(Ctx->Dev->Log, "stream: packet %lu complete, RDMA frames %I64u (pos %lu, STATUS %08lX, bad %lu)\r\n",
                   Completed, Frames, Ctx->HwLastPos, Ctx->HwLastRaw, Ctx->HwBadPos);
        }
    }

    if (Done > 1) {
        Ctx->Glitches++;                               /* late wake-up: several boundaries in one pass */
    }

    (VOID)WdfTimerStart(Timer, AudNextDue(Ctx, Frames));
}

static NTSTATUS AudStreamAllocateRtPackets(ACXSTREAM Stream, ULONG PacketCount, ULONG PacketSize, PACX_RTPACKET *Packets)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    PHYSICAL_ADDRESS    Low;
    PHYSICAL_ADDRESS    High;
    PHYSICAL_ADDRESS    Skip;
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

    Low.QuadPart  = 0;
    High.QuadPart = 0xFFFFFFFFLL;   /* RDMA BUF_STR/BUF_END are 32-bit SFRs (only the DMA ring needs it) */
    Skip.QuadPart = 0;

    /* Client packets: page-rounded, packet 0 at offset (alloc - size) - the ACX sample's layout AND allocation
       (StreamEngine.cpp: ExAllocatePool2(POOL_FLAG_NON_PAGED) per packet, IoAllocateMdl(.., FALSE, TRUE, ..),
       MmBuildMdlForNonPagedPool). A11: the endpoint's format probe (create, RT buffer, latency, close) fails on an
       ACX-side answer; packet memory was the remaining difference to the sample in that path. ExAllocatePool2 zeroes. */
    Ctx->AllocPerPacket = (ULONG)ROUND_TO_PAGES(PacketSize);
    for (I = 0; I < PacketCount; I++) {
        Ctx->PacketBuf[I] = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Ctx->AllocPerPacket, AUD_POOL_TAG);
        if (Ctx->PacketBuf[I] == NULL) {
            AudRingFree(Ctx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* DMA ring: AUD_DMA_SLOTS periods of 4 x 32-bit frames, non-cached. A failure only costs the hardware path. */
    Ctx->FramesPerPacket = PacketSize / Ctx->BlockAlign;
    Ctx->DmaSlotBytes    = Ctx->FramesPerPacket * AUD_HW_FRAME_BYTES;
    Ctx->DmaBytes        = Ctx->DmaSlotBytes * AUD_DMA_SLOTS;
    Ctx->DmaVa = (PUCHAR)MmAllocateContiguousNodeMemory(Ctx->DmaBytes, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE,
                                                        MM_ANY_NODE_OK);
    Ctx->DmaPhys.QuadPart = (Ctx->DmaVa != NULL) ? MmGetPhysicalAddress(Ctx->DmaVa).QuadPart : 0;

    P = (PACX_RTPACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * PacketCount, AUD_POOL_TAG);
    if (P == NULL) {
        AudRingFree(Ctx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ctx->PacketCount = PacketCount;
    Ctx->PacketSize  = PacketSize;
    Ctx->FirstPacketOffset = Ctx->AllocPerPacket - PacketSize;
    for (I = 0; I < PacketCount; I++) {
        Ctx->PacketMdl[I] = IoAllocateMdl(Ctx->PacketBuf[I], Ctx->AllocPerPacket, FALSE, TRUE, NULL);
        if (Ctx->PacketMdl[I] == NULL) {
            ExFreePoolWithTag(P, AUD_POOL_TAG);
            AudRingFree(Ctx);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(Ctx->PacketMdl[I]);

        ACX_RTPACKET_INIT(&P[I]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&P[I].RtPacketBuffer, Ctx->PacketMdl[I], Ctx->AllocPerPacket);
        P[I].RtPacketOffset = (I == 0) ? Ctx->FirstPacketOffset : 0;
        P[I].RtPacketSize   = PacketSize;
    }

    AudLog(Ctx->Dev->Log, "stream: %lu packets x %lu B (%lu frames, %lu bit); DMA ring %s 0x%I64X + 0x%lX (%u x 0x%lX)\r\n",
           PacketCount, PacketSize, Ctx->FramesPerPacket, Ctx->BitsPerSample, (Ctx->DmaVa != NULL) ? "at" : "NOT allocated,",
           Ctx->DmaPhys.QuadPart, Ctx->DmaBytes, AUD_DMA_SLOTS, Ctx->DmaSlotBytes);
    *Packets = P;
    return STATUS_SUCCESS;
}

static VOID AudStreamFreeRtPackets(ACXSTREAM Stream, PACX_RTPACKET Packets, ULONG PacketCount)
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

static NTSTATUS AudStreamPrepareHardware(ACXSTREAM Stream)
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

static VOID AudHwStop(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    if (Ctx->HwStarted) {
        AmpPowerDown(Ctx->Dev);   /* amps first, with UAIF1 still clocked (AudioSpeaker.c :597-604) */
        AboxStop(Ctx->Dev);
        Ctx->HwStarted = FALSE;
    }
}

static NTSTATUS AudStreamReleaseHardware(ACXSTREAM Stream)
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
    AudHwStop(Ctx);

    AudLog(Ctx->Dev->Log, "stream: released after %lu packets, %lu late timer passes, %lu late writes\r\n",
           (ULONG)Ctx->CurrentPacket, Ctx->Glitches, Ctx->LateWrites);
    Ctx->Position      = 0;
    Ctx->CurrentPacket = 0;
    Ctx->State         = AcxStreamStateStop;
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

/*
 * Hardware start for one Run, in the boot #99 order: stage-1 clocks, then the RDMA with data (slot 0 silence, slot 1
 * = the current packet), and only then - from AudEvtStreamRun, with the packet timer already running - the amps,
 * which need data on UAIF1 for PUP_DONE. Any failure leaves the QPC-clocked stream running, silent.
 */
static VOID AudHwStart(_Inout_ PAUD_STREAM_CONTEXT Ctx)
{
    PAUD_DEVICE_CONTEXT Dev = Ctx->Dev;
    NTSTATUS            Status;
    ULONG               Slot;
    ULONG               Count = 0;

    if (!Dev->Cfg.AboxEnable) {
        return;
    }
    if (Ctx->DmaVa == NULL) {
        AudLog(Dev->Log, "stream: no DMA ring - hardware not started\r\n");
        return;
    }

    Status = AboxInit(Dev);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "stream: ABOX stage 1 not ready (%08lX) - silent\r\n", Status);
        return;
    }

    for (Slot = 0; Slot < AUD_DMA_SLOTS; Slot++) {
        AudDmaFillSlot(Ctx, Slot, (Slot == 1) ? AudPacketVa(Ctx, (ULONG)Ctx->CurrentPacket) : NULL);
    }
    KeMemoryBarrier();

    Status = AboxStart(Dev, Ctx->DmaPhys, Ctx->DmaBytes, Ctx->DmaSlotBytes, AUD_RATE, AUD_HW_SLOTS, 32);
    if (!NT_SUCCESS(Status)) {
        AudLog(Dev->Log, "stream: ABOX DMA not started (%08lX) - silent\r\n", Status);
        return;
    }

    (VOID)AboxRingPosition(Dev, &Count, &Ctx->HwLastRaw);
    Ctx->HwLastPos = Count;
    Ctx->HwBadPos  = 0;
    Ctx->HwFrames  = 0;
    Ctx->HwStarted = TRUE;
}

static NTSTATUS AudStreamRun(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            AmpStatus = STATUS_NOT_SUPPORTED;

    PAGED_CODE();

    if (Ctx->State == AcxStreamStateRun) {
        return STATUS_SUCCESS;
    }
    if (Ctx->State != AcxStreamStatePause || Ctx->PacketCount == 0) {
        return STATUS_INVALID_STATE_TRANSITION;
    }

    Ctx->Periods       = 0;
    Ctx->StartPosition = Ctx->Position;
    AudHwStart(Ctx);
    Ctx->StartTime = AudNowHns(Ctx);
    Ctx->State     = AcxStreamStateRun;
    (VOID)WdfTimerStart(Ctx->Timer, AudNextDue(Ctx, 0));

    if (Ctx->HwStarted) {
        AmpStatus = AmpPowerUp(Ctx->Dev);
    }

    AudLog(Ctx->Dev->Log, "stream: run at packet %lu, position %I64u B, %s, amps %08lX\r\n", (ULONG)Ctx->CurrentPacket,
           Ctx->Position, Ctx->HwStarted ? "RDMA0 running (hardware clock)" : "no hardware (QPC clock, silent)", AmpStatus);
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

static NTSTATUS AudStreamPause(ACXSTREAM Stream)
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
    AudHwStop(Ctx);   /* the RDMA would loop over stale periods while paused; Run restarts it */

    AudLog(Ctx->Dev->Log, "stream: pause at packet %lu, position %I64u B, %lu late timer passes\r\n",
           (ULONG)Ctx->CurrentPacket, Ctx->Position, Ctx->Glitches);
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

static NTSTATUS AudStreamGetHwLatency(ACXSTREAM Stream, ULONG *FifoSize, ULONG *Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    PAGED_CODE();

    /* A11: exactly the ACX sample's values (StreamEngine.cpp GetHWLatency: FifoSize 128, Delay 0). Until a10 Delay was
       one period (100000 x 100 ns at 10 ms packets) for the DMA ring lead; it is answered in the endpoint's per-format
       probe (HWLATENCY) that keeps failing, so it goes back to the sample's value. The lead is already excluded from
       the reported position (AudUpdatePosition), so only the latency report loses 10 ms. */
    *FifoSize = 128;
    *Delay    = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS AudStreamSetRenderPacket(ACXSTREAM Stream, ULONG Packet, ULONG Flags, ULONG EosPacketLength)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    ULONG               Current;

    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(EosPacketLength);
    PAGED_CODE();

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

static NTSTATUS AudStreamGetCurrentPacket(ACXSTREAM Stream, PULONG CurrentPacket)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    *CurrentPacket = (ULONG)InterlockedCompareExchange(&Ctx->CurrentPacket, -1, -1);
    return STATUS_SUCCESS;
}

static NTSTATUS AudStreamGetPresentationPosition(ACXSTREAM Stream, PULONGLONG PositionInBlocks, PULONGLONG QPCPosition)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    PAGED_CODE();

    AudUpdatePosition(Ctx);
    *PositionInBlocks = Ctx->Position / Ctx->BlockAlign;
    *QPCPosition      = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

/* ================================================================================================================
 * ACX entry points: each calls the implementation above and traces the call with its result (A6 diagnostics).
 * ================================================================================================================ */

_Use_decl_annotations_
NTSTATUS AudEvtStreamAllocateRtPackets(ACXSTREAM Stream, ULONG PacketCount, ULONG PacketSize, PACX_RTPACKET *Packets)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamAllocateRtPackets(Stream, PacketCount, PacketSize, Packets);

    AUD_TRACE(Ctx, TRUE, Status, "cb AllocateRtPackets(%lu x %lu) -> %08lX\r\n", PacketCount, PacketSize, Status);
    return Status;
}

_Use_decl_annotations_
VOID AudEvtStreamFreeRtPackets(ACXSTREAM Stream, PACX_RTPACKET Packets, ULONG PacketCount)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);

    AudStreamFreeRtPackets(Stream, Packets, PacketCount);
    AUD_TRACE(Ctx, TRUE, STATUS_SUCCESS, "cb FreeRtPackets(%lu)\r\n", PacketCount);
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamPrepareHardware(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamPrepareHardware(Stream);

    AUD_TRACE(Ctx, TRUE, Status, "cb StreamPrepareHardware -> %08lX (state %d)\r\n", Status, (int)Ctx->State);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamReleaseHardware(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamReleaseHardware(Stream);

    AUD_TRACE(Ctx, TRUE, Status, "cb StreamReleaseHardware -> %08lX (state %d)\r\n", Status, (int)Ctx->State);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamRun(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamRun(Stream);

    AUD_TRACE(Ctx, TRUE, Status, "cb Run -> %08lX (state %d, packets %lu)\r\n", Status, (int)Ctx->State, Ctx->PacketCount);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamPause(ACXSTREAM Stream)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamPause(Stream);

    AUD_TRACE(Ctx, TRUE, Status, "cb Pause -> %08lX (state %d)\r\n", Status, (int)Ctx->State);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamGetHwLatency(ACXSTREAM Stream, ULONG *FifoSize, ULONG *Delay)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamGetHwLatency(Stream, FifoSize, Delay);

    AUD_TRACE(Ctx, TRUE, Status, "cb GetHwLatency -> %08lX (fifo %lu, delay %lu)\r\n", Status, *FifoSize, *Delay);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamSetRenderPacket(ACXSTREAM Stream, ULONG Packet, ULONG Flags, ULONG EosPacketLength)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamSetRenderPacket(Stream, Packet, Flags, EosPacketLength);

    AUD_TRACE(Ctx, FALSE, Status, "cb SetRenderPacket(%lu, flags %lX, eos %lu) -> %08lX (current %ld)\r\n", Packet, Flags,
              EosPacketLength, Status, Ctx->CurrentPacket);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamGetCurrentPacket(ACXSTREAM Stream, PULONG CurrentPacket)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamGetCurrentPacket(Stream, CurrentPacket);

    AUD_TRACE(Ctx, FALSE, Status, "cb GetCurrentPacket -> %08lX (%lu)\r\n", Status, *CurrentPacket);
    return Status;
}

_Use_decl_annotations_
NTSTATUS AudEvtStreamGetPresentationPosition(ACXSTREAM Stream, PULONGLONG PositionInBlocks, PULONGLONG QPCPosition)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext(Stream);
    NTSTATUS            Status = AudStreamGetPresentationPosition(Stream, PositionInBlocks, QPCPosition);

    AUD_TRACE(Ctx, FALSE, Status, "cb GetPresentationPosition -> %08lX (%I64u blocks)\r\n", Status, *PositionInBlocks);
    return Status;
}

_Use_decl_annotations_
VOID AudEvtStreamCleanup(WDFOBJECT Object)
{
    PAUD_STREAM_CONTEXT Ctx = AudGetStreamContext((ACXSTREAM)Object);

    /* FreeRtPackets normally ran already; this only catches a stream torn down without it. */
    if (Ctx->Dev != NULL) {
        AudHwStop(Ctx);
        AudRingFree(Ctx);
    }
}

static NTSTATUS AudCircuitCreateStream(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXPIN Pin,
                                       _In_ PACXSTREAM_INIT StreamInit, _In_ ACXDATAFORMAT StreamFormat,
                                       _In_ const GUID *SignalProcessingMode, _In_ ACXOBJECTBAG VarArguments)
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

    /* A6 diagnostics: every KS request on this stream (ReqLog.c). A refusal only costs the log. */
    Status = AcxStreamInitAssignAcxRequestPreprocessCallback(StreamInit, AudEvtStreamRequestPreprocess,
                                                             (ACXCONTEXT)AudGetCircuitContext(Circuit)->Dev,
                                                             AcxRequestTypeAny, NULL, AcxItemIdNone);
    if (!NT_SUCCESS(Status)) {
        AudLog(AudGetCircuitContext(Circuit)->Dev->Log, "stream: request preprocess hook refused %08lX\r\n", Status);
    }

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
    KeInitializeSpinLock(&Ctx->HwLock);
    KeQueryPerformanceCounter(&Ctx->QpcFrequency);

    /* AudDmaFillSlot understands exactly the two formats of the pin's list. */
    if (Ctx->BlockAlign != AUD_CHANNELS * (Ctx->BitsPerSample / 8) || (Ctx->BitsPerSample != 16 && Ctx->BitsPerSample != 32) ||
        Ctx->BytesPerSec != AUD_RATE * Ctx->BlockAlign) {
        AudLog(Ctx->Dev->Log, "stream: format %lu B/s block %lu %lu bit not supported\r\n", Ctx->BytesPerSec,
               Ctx->BlockAlign, Ctx->BitsPerSample);
        AudLogFlush(Ctx->Dev->Log);
        return STATUS_NOT_SUPPORTED;   /* the stream object is deleted by ACX on failure */
    }

    AudLog(Ctx->Dev->Log, "stream: created, %lu B/s, block %lu, %lu bit\r\n", Ctx->BytesPerSec, Ctx->BlockAlign,
           Ctx->BitsPerSample);
    AudLogFlush(Ctx->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS NTAPI AudEvtCircuitCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN Pin, PACXSTREAM_INIT StreamInit,
                                         ACXDATAFORMAT StreamFormat, const GUID *SignalProcessingMode,
                                         ACXOBJECTBAG VarArguments)
{
    PAUD_DEVICE_CONTEXT Dev = AudGetCircuitContext(Circuit)->Dev;
    NTSTATUS            Status;

    PAGED_CODE();

    Status = AudCircuitCreateStream(Device, Circuit, Pin, StreamInit, StreamFormat, SignalProcessingMode, VarArguments);
    AudLog(Dev->Log, "cb CreateStream(pin %lu, %lu Hz %lu ch %lu bit, mode {%08lX}) -> %08lX\r\n", AcxPinGetId(Pin),
           AcxDataFormatGetSampleRate(StreamFormat), AcxDataFormatGetChannelsCount(StreamFormat),
           AcxDataFormatGetBitsPerSample(StreamFormat), (SignalProcessingMode != NULL) ? SignalProcessingMode->Data1 : 0,
           Status);
    AudLogFlush(Dev->Log);
    return Status;
}

/* Circuit power callbacks (the sample registers them too): logged only. */
_Use_decl_annotations_
NTSTATUS AudEvtCircuitPowerUp(WDFDEVICE Device, ACXCIRCUIT Circuit, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Device);
    PAGED_CODE();

    AudLog(AudGetCircuitContext(Circuit)->Dev->Log, "cb CircuitPowerUp from D%d\r\n", (int)PreviousState - (int)WdfPowerDeviceD0);
    AudLogFlush(AudGetCircuitContext(Circuit)->Dev->Log);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS AudEvtCircuitPowerDown(WDFDEVICE Device, ACXCIRCUIT Circuit, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    PAGED_CODE();

    AudLog(AudGetCircuitContext(Circuit)->Dev->Log, "cb CircuitPowerDown to D%d\r\n", (int)TargetState - (int)WdfPowerDeviceD0);
    AudLogFlush(AudGetCircuitContext(Circuit)->Dev->Log);
    return STATUS_SUCCESS;
}
