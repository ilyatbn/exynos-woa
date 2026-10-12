# D8 audio - Windows A5: shared-mode mix format + endpoint volume (Exynos9820Audio)

Problem (runs audB4-B6, notes/d8-audio-win-m1.md): endpoint "Speakers (Galaxy S10e Audio)" Started, but WASAPI
IAudioClient::GetMixFormat -> 0x80070032 (ERROR_NOT_SUPPORTED) and stream open fails/hangs.
Constraints: no phone access in this task (code/build/package only); hardware access stays behind
Parameters AmpInit/AmpEnable/AboxEnable; no hardware constant changes.

## Progress log

- 2026-10-12 start: read d8-audio-win-m1.md, Exynos9820Audio.{h,inf}, Render.c, Driver.c (driver 1.0.1.3).
- No device logs of audB4-B6 exist on the host (grep of s10e-artifacts + notes for "stream: created"/"trace " =
  only the .sys binaries); the only record is d8-audio-win-m1.md:190 ("streams are created (16 + 32 bit probing)
  but GetMixFormat fails 0x80070032 and the tone hangs"). Root cause must be derived from code vs reference.
- Reference fetched (untrusted data): microsoft/Windows-driver-samples 2dc3fd3a0cc84a2933f2194e7ec0871584979071,
  sparse audio/Acx -> scratchpad\acx-samples\wds\audio\Acx\Samples. Cited as `ACX/<path>:<line>`.

### Comparison driver vs sample (task 1)

| Item | Sample | Exynos9820Audio 1.0.1.3 | Verdict |
|---|---|---|---|
| Host pin | Sink/Sink, KSCATEGORY_AUDIO (ACX/Common/RenderCircuit.cpp CodecR_CreateRenderCircuit "Create Render Pin") | same (Render.c:270-279) | equal |
| Bridge pin | Source/None, KSNODETYPE_SPEAKER + jack (AtapiInternal, Front, PrimaryBox, IntegratedDevice) | same (Render.c:281-307) | equal |
| Format list | AcxPinGetRawDataFormatList(host) + 44.1k/16 + 48k/16 stereo (RenderCircuit.cpp Render_AllocateSupportedFormats; Inc/AudioFormats.h:89 Pcm48000c2) | raw list 48k/16 + 48k/32 stereo, same struct shape | equal shape; no explicit default in either |
| Default format / PROPOSEDATAFORMAT | not set; MS doc "ACX audio data formats": ACX implements GetDefaultDataFormat/ProposeDataFormat itself from the lists; default = first in list unless AcxDataFormatListAssignDefaultDataFormat | not set | equal (handled by ACX) |
| EvtAcxPinSetDataFormat | registered, returns STATUS_NOT_SUPPORTED (doc: only needed for DSP circuits whose in/out formats differ) | not registered | not a cause |
| **Elements** | **always volume + mute** (RenderCircuit.cpp "Create mute and volume elements"; AcxCircuitAddElements), connected by ACX default chain host -> volume -> mute -> bridge (AcxCircuitAddConnections doc: "By default ACX connects the elements starting from the sink circuit pin and ending with the source circuit pin") | **no elements unless HwVolume = 1 (default 0)** | **only structural divergence**: with zero elements the "default chain" has no element to connect; nothing in the doc or sample covers a direct host->bridge connection, so the host pin may never be connected to the bridge (speaker) pin in the KS topology the AEB walks |
| Audio engine element | none (RenderCircuit.cpp header: "render circuit with no offload") | none | shared mode does NOT need AcxAudioEngine (only offload circuits, sample's DSP variants) |
| Stream | AcxRtStreamCreate + notifications, timer clock (Common/StreamEngine.cpp) | same callbacks set | equal |
| INF | KSCATEGORY_AUDIO/RENDER/REALTIME on "Speaker0", EP\0 Association=KSNODETYPE_ANY + EventDriven, no FX/APO, no KS.Registration include (AudioCodec/Driver/AudioCodec.inf) | identical set | equal; neither pins a device format |
| Device | power policy S0 idle, circuit added in PrepareHardware | no idle policy, circuit added in PrepareHardware | not a format cause |

### Root cause (task 1) - evidence and confidence

- Device-format source, MS doc "PKEY_AudioEngine_OEMFormat" (learn.microsoft.com/.../audio/pkey-audioengine-oemformat):
  "identifies the default stream format for an audio endpoint device ... The global audio engine uses a device's
  default format to connect to the device for shared-mode operation. The INF file that installs the device loads the
  device's default format into the registry." GetMixFormat returns the engine format derived from it. Neither the
  ACX sample INF nor ours sets it, so the AEB must derive it from the KS topology: endpoint (bridge pin, KSNODETYPE_SPEAKER)
  -> topology path -> host (streaming) pin -> its data formats.
- Our circuit (HwVolume = 0, the default and the run setting) has ZERO elements. ACX builds the KS connections itself
  ("By default ACX connects the elements starting from the sink circuit pin and ending with the source circuit pin",
  AcxCircuitAddConnections doc). Every ACX sample render circuit has >= 1 element (AudioCodec: volume + mute); no
  reference shows the zero-element case. If ACX emits no host->bridge connection for it, the endpoint exists (the
  bridge pin + jack make it) but has no streaming pin behind it -> the engine has no device format ->
  GetMixFormat/Initialize fail with the KS STATUS_NOT_SUPPORTED mapped to 0x80070032. The 16/32-bit stream creates
  in audB4 are consistent with a format probe of the host pin that still leaves the endpoint unassociated.
- Confidence: this is the ONLY structural divergence from the sample (table above); it is NOT proven on the device
  (no log of the topology). Two independent changes therefore close it, both device-checkable in one run:
  (a) always create volume + mute elements = the sample's topology (host -> volume -> mute -> bridge);
  (b) INF PKEY_AudioEngine_OEMFormat = 48 kHz 16-bit stereo (the doc's own bytes, identical to AudFormatPcm16), so
      the engine has a default format without probing.
  Plus (c) AcxDataFormatListAssignDefaultDataFormat(raw list, 48k/16) so the pin's default equals the OEM format.
- Volume (task 3) follows from (a): with a KSNODETYPE_VOLUME node on the endpoint path the endpoint volume (slider,
  volume keys, flyout) is handed to the node instead of audiodg's software gain. The node must therefore DO something
  even with the amp at its fixed -10 dB: HwVolume = 0 -> the driver applies the level + mute as an integer gain while it
  expands client packets into the DMA ring (it already touches every sample there); HwVolume = 1 -> the amp registers
  (unchanged path). No hardware constant changes.

### Plan (one commit each)
- T1 Render.c/.h/Driver.c: elements always; software gain/mute in AudDmaFillSlot for HwVolume = 0; default format.
- T2 INF: PKEY_AudioEngine_OEMFormat (48k/16 stereo) + version 1.0.1.4 (INF + Driver.c AUD_DRIVER_VERSION).
- T3 build + sign -> s10e-artifacts\audio-a5, sha256 here; test script changes only if needed.
- Review each; fixes as separate commits.
