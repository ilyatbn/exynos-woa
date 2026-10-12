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
- T1 code: Render.c - volume + mute elements always (AudCreateElements unconditional); HwVolume = 0: AssignLevel/Mute
  store per-channel level -> Q30 gain (193-entry 0.5 dB table, round(2^30*10^(-n/40)), checked n=12 -> 0.501, n=40 ->
  0.1000) and SwMuted; AudDmaFillSlot samples the gains once per period and multiplies (64-bit, >> 30), mute = silence;
  HwVolume = 1 path unchanged (amp). Format list: 48k/16 via AcxDataFormatListAssignDefaultDataFormat (default), 48k/32
  added. Driver.c: AudSwVolumeInit(-20 dB) at device add. Build (VS18 MSBuild, ARM64 Release, /W4 /WX): exit 0.
- Review of T1 (48422d9), adversarial. Checked: element creation order = sample (AcxCircuitAddElements before
  AcxCircuitAddPins, RenderCircuit.cpp); restart path (Driver.c:267 re-create) rebuilds the elements, SwLevel lives in
  the device context so the level survives; overflow: 16-bit path |L| <= 2^31, gain <= 2^30 -> product < 2^61 in
  LONGLONG; gain applied only inside `if (Src != NULL)`; mute -> Src = NULL -> zeros in slots 0/1; Steps for an exact
  -0.5n dB level = n (python check of the table: n=0 1073741824, n=12 0.50119, n=40 0.10000, n=192 17018 = -96 dB);
  HwVolume = 0 never writes Dev->Muted, so AmpPowerUp's AmpMuteLocked(Dev->Muted) stays unmuted and AmpTargetSteps stays
  fixed -10 dB (AmpI2c.c:703, :799); AcxDataFormatListAssignDefaultDataFormat is in the km\1.0 header (table index 63),
  build links against 1.0. Finding R1: comments in Render.c (header + AudRenderCircuitCreate) and Exynos9820Audio.h
  stated the zero-element circuit as the proven cause ("the endpoint had no mix format", "needed for the endpoint's
  format") - it is a suspicion, not device-proven. Fixed in its own commit (wording only).
- T2: INF EP\0 PKEY_AudioEngine_OEMFormat {E4870E26-3CC5-4CD2-BA46-CA0A9A70ED04},3 REG_BINARY = the doc's 48-byte
  value (decoded: VT_BLOB 0x41, 40 B, tag FFFE, 2 ch, 48000, 192000 B/s, align 4, 16 bit, cbSize 22, valid 16, mask 3,
  SUBTYPE_PCM) = AudFormatPcm16. Version 1.0.1.4 (INF DriverVer + Driver.c AUD_DRIVER_VERSION). InfVerif /v: INF is VALID.
  Note: the OEM format is written only when the endpoint is created; an endpoint left from 1.0.1.x keeps its property
  store - the test must remove the old device/driver first (pnputil /remove-device + /delete-driver) or the reflashed
  Windows starts clean.
- Review of T2 (INF + version), adversarial. Checked: the 48 bytes decoded field by field against AudFormatPcm16
  (Render.c:32-46) - equal; key GUID/pid copied from the doc's Strings section; REG_BINARY flag 0x00000001 (INF AddReg
  FLG_ADDREG_BINVALUETYPE); version strings: only INF DriverVer and Driver.c AUD_DRIVER_VERSION carry it (resource.rc
  uses common.ver) - both 1.0.1.4 (rg); InfVerif VALID. Finding R2: the T2 note stated as fact that the OEM format is
  only applied at endpoint creation - that is an inference (the doc only says the INF loads it into the registry and
  the engine uses it). Corrected here: INFERENCE, unverified. The INF AddReg on the interface key is re-run by
  pnputil /add-driver /install of a newer version, so the value itself is current after the update.
- T4 test scripts: audio-test.ps1 logs "vol: endpoint volume scalar/dB/mute/hw support/range" (IAudioEndpointVolume,
  vtable order per endpointvolume.h) at start and after every key step; extra-audio.cmd mirrors driver lines with
  volume/mute/render circuit/version into the PRAM marks too. Both CRLF. Host check: the C# compiled with Add-Type on
  this PC and Vol() read the PC's default endpoint: "scalar 0.70 = -5.4 dB, mute 0, hw support 0x7, range -96.0..0.0
  step 0.13 (hr 0/0/0/0/0)" (read-only call; no playback, no Initialize on the PC).
- Review of T4 (audio-test/extra-audio): Vol() vtable order re-checked against endpointvolume.h order (Register,
  Unregister, GetChannelCount, SetMasterVolumeLevel, ...Scalar, GetMasterVolumeLevel, ...Scalar, SetChannelVolumeLevel,
  ...Scalar, GetChannelVolumeLevel, ...Scalar, SetMute, GetMute, GetVolumeStepInfo, StepUp, StepDown,
  QueryHardwareSupport, GetVolumeRange) - and proven by the host run returning sane values (range -96..0 matches that
  PC's codec); both files CRLF (`file`); the findstr edit is the only extra-audio change (git diff). No findings.
- T3 package (build-package.ps1 -Out s10e-artifacts\audio-a5): MSBuild exit 0, Inf2Cat 10_NI/10_GE ARM64, sys + cat
  signed with WDK test cert 5DD5DEE1747244C5F5F66F41D94C0A59C4443E48 (signtool verify shows that signer; chain ends in
  the untrusted test root on the host, same as audio-a4), sys = ARM64 (dumpbin), InfVerif /v VALID, DriverVer 1.0.1.4.
    0763C1949AB21A9870DD125565ED8C44CC7FF87D4EE1A2C37FBFADF092B66136  Exynos9820Audio.sys
    7C6D4273E82BA0608C47CFC42BC4017F88D7D7A57BBB7E3E127B1A308F029377  Exynos9820Audio.inf
    DF9FD410D4E4D87A9645212E9004A6EAB3FE5E95D9EE632274897841ACF54F98  exynos9820audio.cat
- Status: NOT device-validated (Guided). Windows was reflashed (HANDOFF §32), so the endpoint is created fresh.

### Device test (for the main session)
1. ESP \S10EDIAG\drivers\Exynos9820Audio\ = audio-a5 (3 files); \S10EDIAG\audio-test.ps1 + run-as-user.ps1 from
   drivers/S10eDiag/esp; extra.cmd = extra-audio.cmd. Staged first: audio-stage-A.cmd as audio-stage.cmd (hardware off,
   QPC clock) - proves the mix format / stream / volume independent of the hardware; then stage D (all on) with the mic.
2. Success in audiotest.txt: `diag: ... mix format tag 0xFFFE 2 ch 48000 Hz 32 bit; Initialize hr 0x00000000`
   (engine float mix of the 48k/16 device format); `vol: ... hw support 0x3, range -96.0..0.0 step 0.50` and the scalar
   following the keys (~0.50 / ~0.30 / ~0.70, mute 1 after the mute key); `played 96000 frames at 48000 Hz (float, 2 ch)`
   x4 (no STALLED/Initialize lines).
3. Success in run.txt: `Exynos9820Audio 1.0.1.4`, `render circuit "Speaker0": 00000000 (volume + mute elements in
   software)`, `volume ch <n> <- ...` / `mute ch <n> <- 1 (software)` lines matching the key steps (n = 0/1 per channel or -1 =
   all channels - which one Windows uses is not known), `trace
   AllocateRtPackets 2 x 1920` (or similar), `stream: run at packet 0 ...`, `stream: released after N packets` with N
   ~200 per 2 s tone. Stage D adds `RDMA0 running (hardware clock)`, `amps 00000000`, and the mic hears A > B < C, D silent.
- Review of T3 (package + test text): sha256 re-read from the files (Get-FileHash in build-package output) = notes; signer thumbprint of the .cat = 5DD5DEE1...; Finding R3: the expected run-log line `volume ch -1` assumed Windows assigns the master level with the all-channels id - unverified (it may set ch 0 and ch 1). Fixed in this commit (expectation widened). Also noted: ~100 key presses x 2 channels of volume lines (~10 KB) fit the 64 KB run log.

## A6 (2026-10-13): device result of a5 + boot crash

Device result (coordinator, runs in scratchpad\runs\audA5\last\audio, \audA5b, \audcrash):
- Live install (stage A, 1.0.1.4): run.txt = render circuit with sw volume + mute; one probe round: stream created 16 bit
  -> AllocateRtPackets 2 x 1920 -> GetHwLatency -> FreeRtPackets, same for 32 bit (2 x 3840); NOTHING afterwards (no
  PrepareHardware/Run, no stream at test time). audiotest: GetMixFormat 0x80070032, **IAudioEndpointVolume Activate
  0x80070032 too**, Initialize 0x80070032. => the elements/OEM format changed nothing; the a5 hypothesis is refuted.
  audB4 (1.0.1.2, no elements) showed the identical probe pattern (two rounds).
- Every later boot with the driver loaded at boot dies ~50 s after power-on. audA5b marks.txt (PRAM ring, boot after the
  live install): "S10eDiag loaded" 23346 ms, **"BUGCHECK callback ... BC 00000000000000D1, BC P1 FFFF838715923000"** at
  23627 ms. audcrash run.txt ends at "D0Entry from D4".

### Boot crash root cause (code-proven, not device-proven)
- 0xD1 DRIVER_IRQL_NOT_LESS_OR_EQUAL with a page-aligned P1 = a pageable page touched at DISPATCH_LEVEL.
- RunLog.c AudLogFlush (unchanged since A1): `Copy = ExAllocatePool2(POOL_FLAG_PAGED, 64 KiB + 64)` then
  `KeAcquireSpinLock(&Log->Lock)` (-> DISPATCH) and `RtlCopyMemory(Copy, Log->Text, Used)` = writes to fresh paged
  pool at DISPATCH. A 64 KiB paged allocation is a big-pool allocation whose pages can be demand-zero (not resident);
  the first touch then faults at DISPATCH -> 0xD1 with P1 = the first non-resident page (page aligned). Whether it
  faults depends on pool state, which explains "live install fine, boot crashes" and also the audB2 0xD1 on
  restart-device and the boot loops after audB5 that HANDOFF §30 put on NTFS corruption.
- Same pattern in drivers/MaliG76Probe/MaliG76Probe.c:217-225 (paged Copy filled under C->Log->Lock) - reported to
  the coordinator, not changed here (other driver).
- Fix: non-paged snapshot buffer.

### Endpoint rejection - new analysis
- ACX version: the a1 note "No 1.1 runtime in 22621" (d8-audio-win-m1.md:21) has no evidence behind it (only the
  file name Acx01000.sys, which is the major-1 runtime for every minor, like Wdf01000). MS doc "ACX version
  information": 1.1 = "current ... recommended", supported from Windows 10 2004 (KMDF >= 1.31); **1.0 = "Pre-release
  ... not recommended ... used in early development and testing"**. The ACX sample builds 1.1
  (AudioCodec.vcxproj:35-38). We bind 1.0.
- Header diff km\acx\km\1.0 vs 1.1 (WDK 26100): besides 11 new DDIs, **ACX_PIN_CONFIG gains `MaxStreams`** (1.1
  ACX_PIN_CONFIG_INIT sets it to AcxInstanceIndeterminate) and ACX_PIN_CALLBACKS gains a callback. A 1.0-sized
  ACX_PIN_CONFIG gives the runtime no MaxStreams; what it assumes for a pre-release client is unknown - if it is 0/1
  instance, KSPROPERTY_PIN_CINSTANCES would tell AudioSrv the host pin cannot be opened by the engine, which fits
  "AEB probe succeeds, every later AudioSrv use fails with NOT_SUPPORTED, no further pin create". UNVERIFIED.
- Mounted the phone image (win-w5 vhdx, read-only): audiosrv/AudioEndpointBuilder/audiodg/AudioEng/AudioKSE/AudioSes/
  MMDevAPI/AudioSrvPolicyManager present (22621.1/.2506), ks.inf/wdmaudio.inf present -> not a stripped audio stack.
  No inbox driver binds ACX (no "Acx" bind string in System32\drivers\*.sys except Acx01000.sys itself).
- Plan A6: (T5) RunLog non-paged flush; (T6) ACX 1.1 like the sample (+ build-package -AcxMinor so a 1.0 fallback
  package can be built if 1.1 does not bind); (T7) log every KS request to the circuit and streams (ACX request
  preprocess callbacks, set/id/verb/item + the dispatch status) and every ACX callback with its status; version 1.0.1.5.
- T5: RunLog.c AudLogFlush snapshot buffer POOL_FLAG_PAGED -> POOL_FLAG_NON_PAGED (the only POOL_FLAG_PAGED in the
  driver - rg). Build exit 0.
- T6: vcxproj ACX_VERSION_MINOR 0 -> 1 (km\acx\km\1.1 headers + 1.1 acxstub.lib), build-package.ps1 -AcxMinor (default
  1, 0 = old binding as fallback), run-log header prints "ACX 1.<minor>". Both builds exit 0 (/W4 /WX); bind info read
  from the binaries (_Acx_BIND_INFO RVA 0xe250 per map): minor 1 -> version {1,1,0}, AcxFunctionCount 221; minor 0 ->
  {1,0,0}, 210 (= AcxFunctionTableNumEntries of each header). No source changes needed (1.1 is a superset; ACX_PIN_CONFIG_INIT
  now sets MaxStreams = AcxInstanceIndeterminate).
- Review of T5: rg POOL_FLAG_PAGED in the driver -> none left; every other buffer touched at DISPATCH (stream context,
  packets: MmAllocateContiguousNodeMemory, DMA ring, run log) is non-paged; no #pragma alloc_text/PAGE sections in the
  driver (PAGED_CODE() asserts only) -> no paged code either. No findings.
- T7 diagnostics: ReqLog.c (new) - ACX request preprocess hooks on the circuit (AcxCircuitInitAssignAcxRequestPreprocessCallback,
  AcxRequestTypeAny, Set NULL, AcxItemIdNone) and on every stream (AcxStreamInitAssignAcxRequestPreprocessCallback);
  each hook copies ACX_REQUEST_PARAMETERS (AcxRequestGetParameters), hands the request back with
  Acx{Circuit,Stream}DispatchAcxRequest exactly once and logs `req C|S #n prop <Set>.<Id> <verb> <pin|elem|obj>#<id> ctl
  <cb> val <cb> {Data1-Data2-Data3} -> <status>` (method/event/create variants); cap 2000 lines, flush every 32 lines
  and after every NT_ERROR status. A refused hook is logged ("circuit: request preprocess hook <status>") and the
  driver continues. Every stream callback is now an entry point that calls the implementation and traces
  `cb <Name>(args) -> <status>` (cap 400/stream; rare callbacks flushed always, frequent ones every 16th line or on
  error); CreateStream traced with pin/format/mode/status; circuit PowerUp/PowerDown callbacks registered (as the sample)
  and logged. Run log 64 -> 256 KiB. Builds: ACX 1.1 exit 0, ACX 1.0 (-AcxMinor 0) exit 0.
- Review of T7 (adversarial). Hook arguments checked against the MS DDI page for
  AcxCircuitInitAssignAcxRequestPreprocessCallback, whose own example registers exactly (AcxRequestTypeAny, NULL,
  AcxItemIdNone) on a render circuit, and the page says multiple registrations are allowed (first match wins); the
  ACX sample's CircuitHelper.cpp:504/533/610 returns unhandled requests with AcxCircuitDispatchAcxRequest the same
  way. Each hook dispatches exactly once on every path and never touches the request afterwards (the logged fields
  come from the local ACX_REQUEST_PARAMETERS copy; Control/Value pointers are not dereferenced). Wrappers: output
  pointers (FifoSize/Delay/CurrentPacket/PositionInBlocks) are caller-owned and always written by the implementations
  (all return STATUS_SUCCESS paths only); Ctx is still valid after FreeRtPackets (stream context). Flushes only at
  PASSIVE (KeGetCurrentIrql check in AUD_TRACE; hooks are PASSIVE by contract). Cost noted: an NT_ERROR status flushes the
  whole log (<= 256 KiB, write-through) - the endpoint builder's unsupported-property probes each cause one; accepted
  for a diagnostic build. No findings.
- extra-audio.cmd: findstr also mirrors "cb Create/Alloc/Stream/Run/Pause/Free/GetHw/Circuit", "hook" and "-> C"
  (error statuses) into the PRAM marks; CRLF kept. The full run.txt is still copied to the ESP.
- T8 packages (1.0.1.5, INF DriverVer 10/13/2026; signed 5DD5DEE1... on sys + cat; InfVerif VALID; bind info read from
  each .sys: a6 = ACX {1,1} 221 functions, a6-acx10 = ACX {1,0} 210):
  s10e-artifacts\audio-a6 (ACX 1.1 - test this first):
    1FC04A57F382F04DD823B98F91B38DC77BEBF5E7A8D7F0A07F6DBE3FD46DDC0D  Exynos9820Audio.sys
    65D8D9EF59072BD94E335BA442F0238CB15765AF5C2687EF5C73362D9E87016D  Exynos9820Audio.inf
    83EE6527BF6C96D9D37D344F4FC6BAF3AC83714E5B9FFA80DA9D4934EC307B76  exynos9820audio.cat
  s10e-artifacts\audio-a6-acx10 (same source, ACX 1.0 binding - only if a6 does not start, e.g. Code 10/31 because the
  22621 runtime refuses minor 1):
    F9BB94ED76ECF017C125D7497F7F82B98B089EEFB204E9B6E3FC68F115DD0293  Exynos9820Audio.sys
    65D8D9EF59072BD94E335BA442F0238CB15765AF5C2687EF5C73362D9E87016D  Exynos9820Audio.inf
    EE5DF629459F0175B9FB28467C276B66AE3EFEB0F822690ECAC0D45EF0F96B52  exynos9820audio.cat

### A6 device test
1. Stage A (hardware off) live install of audio-a6 + audio-test.ps1 (unchanged from a5). Then reboot with the driver
   loaded at boot (no noaudio.flg) - the 0xD1 fix is proven when Windows reaches the desktop and run.txt continues past
   "D0Entry from D4" (and the PRAM marks have no "BC 00000000000000D1").
2. If the device shows a problem code and run.txt has no "Exynos9820Audio 1.0.1.5 (ACX 1.1" line: ACX 1.1 did not bind ->
   repeat with audio-a6-acx10 (the log line then says ACX 1.0).
3. Read run.txt: "circuit: request preprocess hook 00000000"; the `req C ...` lines before and after the probe streams
   (`cb CreateStream ... -> 00000000`, `cb AllocateRtPackets`, `cb GetHwLatency`, `cb FreeRtPackets`) and every
   `-> C00000BB` (NOT_SUPPORTED) / other error line around the time of the WASAPI test are the evidence for the
   0x80070032; success = GetMixFormat 0 in audiotest.txt and `cb StreamPrepareHardware -> 00000000`, `cb Run -> 00000000`
   in run.txt.

## A7 (2026-10-13): a6 device result and the request log

Runs audA6s/t/u (scratchpad\runs\audA6u\last\audio\run.txt): 1.0.1.5 ACX 1.1 binds and loads at boot, no 0xD1 (T5
proven). Still GetMixFormat / EndpointVolume Activate 0x80070032 (state 1).
- **Limit of the a6 log: "-> 00000000" on every req is the return of Acx{Circuit,Stream}DispatchAcxRequest = the
  dispatch was accepted, NOT the request's completion status** - proof: size queries (val 0) such as Pin.3/Topology.1
  normally complete STATUS_BUFFER_OVERFLOW and are followed by a sized query, and those show "-> 00000000" too.
- KS sizes on ARM64: KSDATAFORMAT 64 B, +WAVEFORMATEXTENSIBLE 40 = 104 (= ACX's default format, Pin.14 get val 104),
  KSDATARANGE_AUDIO 84 -> 88 aligned, 2 ranges + KSMULTIPLE_ITEM = 184 (= Pin.3 val 184).
- One endpoint-builder round (#341-#357, repeated ~83 times in 32 s, i.e. a retry loop): Pin.14
  PROPOSEDATAFORMAT get (size, then 104 B = our default) -> 12 x Pin.14 set (104 B variants) -> Pin.3 DATARANGES (size,
  184 B) -> **Pin.4 DATAINTERSECTION get, Control = our own 184 B range list, Value 0 (size query) and never a sized
  follow-up** -> next round. Every other sized query in the log is followed by its data query, so the intersection
  size query is the step that fails, and it ends every round. A failed intersection = the engine cannot derive a
  device format from the pin = no mix format = AudioSrv refuses the endpoint (GetMixFormat/Activate 0x80070032).
- The probe streams (16/32 bit create, RT buffer, latency) succeed once at 1.08 s, before the first round; the rounds
  never reach a pin create again.
- Why ACX fails its own ranges is unknown (no ACX symbols/source); the fix does not depend on it: KS defines the
  intersection precisely (DATAINTERSECTION: input KSMULTIPLE_ITEM of KSDATARANGEs, output one KSDATAFORMAT; size query
  = STATUS_BUFFER_OVERFLOW + Information), and the ACX sample's CircuitHelper.cpp:483-620 shows the driver completing a
  property in the preprocess hook itself (WdfRequestCompleteWithInformation, BUFFER_OVERFLOW size convention).

### Plan A7
- T9 log: per-key (set, id, verb, item type, item id) counters - first 6 of each logged, then every 100th, plus a
  closing histogram; the format of every Pin.14 set is decoded into the log (first 48).
- T10 Parameters\KsOverride (default 1): the hook completes Pin.4 DATAINTERSECTION on the host pin itself (first range
  that admits 48 kHz / 2 ch / 16 (else 32) bit PCM -> AudFormatPcm16/32, KS size-query convention) and Pin.14
  PROPOSEDATAFORMAT set on the host pin for WAVEFORMATEX/EXTENSIBLE formats (SUCCESS for exactly our two formats, else
  STATUS_NO_MATCH); everything else still goes to ACX. KsOverride = 0 restores the a6 behaviour (pure ACX).
- T11 version 1.0.1.6, package audio-a7.
- T9 (log): ReqLog.c - per-key counters (96 keys: set Data1, id, verb, item type/id; spin lock), each key logged for
  its first 6 occurrences and then every 100th with "x<count>", NT_ERROR always; the request status is now tagged
  "disp" (dispatch return, not completion); every Pin.14 PROPOSEDATAFORMAT set on a pin is decoded before ACX gets it
  (`fmt C #n set pin#k: size .. flags .. sub {..} spec {..} EXT ch Hz bits/valid align avg mask sub` or the
  WAVEFORMATEX / no-wave variants; first 48). Build exit 0.
- T10: Parameters\KsOverride (default 1, INF NOCLOBBER, Driver.c AudReadConfig). The circuit hook completes two
  host-pin properties itself (pin id from AcxPinGetId(Pins[0]), logged "circuit: host pin id .."):
  DATAINTERSECTION get (walk KSMULTIPLE_ITEM ranges, 8-byte aligned, skip attribute lists; first range admitting
  48 kHz/2 ch/16 (else 32) bit PCM; ValueCb 0 -> STATUS_BUFFER_OVERFLOW + Information 104; < 104 -> BUFFER_TOO_SMALL;
  else copy AudFormatPcm16/32 (Render.c AudHostFormat), SUCCESS, 104; no range -> STATUS_NO_MATCH; each range logged
  "ks: intersection range ..") and PROPOSEDATAFORMAT set with a WAVEFORMATEX header (SUCCESS only for 48 kHz stereo
  16/32-bit PCM, tag PCM or EXTENSIBLE/PCM with valid = container bits and mask 0/3; else NO_MATCH; formats without a
  wave header still go to ACX). Completion with WdfRequestCompleteWithInformation as in the ACX sample's
  CircuitHelper.cpp:615-618; logged as `req c ... done <status> info <n>`. Sizes checked against the a6 log:
  sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) = 104 (= ACX default-format get), KSDATARANGE_AUDIO 84 -> 88 aligned, 2
  ranges + header = 184 (= DATARANGES/intersection control). Build exit 0.
- Review of T10 (adversarial). Checked: completion only on the two handled paths, every other path dispatches to ACX
  exactly once; KsOverride/pin id/set GUID (full compare) gate; PROPOSE without a wave header -> ACX; output copy only
  when ValueCb >= 104; NO_MATCH on no range. **Finding R4 (real):** the range walk advances Cur by the 8-byte-aligned
  FormatSize, which can step up to 7 bytes past End on the last item; the next iteration's `(SIZE_T)(End - Cur)` then
  wraps to a huge value and the loop would read a KSDATARANGE beyond the control buffer (Count larger than the real
  number of ranges). Fixed in its own commit: `Cur >= End` tested before the subtraction (range and attribute-list
  checks). Build exit 0.
- Review of T9 (log de-dup): key compare covers all six fields; full table (96) -> Count 0 -> logged (never silently
  dropped); NT_ERROR always logged; decoded format reads stay inside ValueCb (KSDATAFORMAT 64 B checked, WAVEFORMATEX
  after it, EXTENSIBLE only when ValueCb >= 104). No findings.
- T11 package s10e-artifacts\audio-a7 (1.0.1.6, ACX 1.1 - bind info {1,1} 221; signed 5DD5DEE1... sys + cat;
  InfVerif VALID; INF adds KsOverride NOCLOBBER 1):
    4EBB8D0C428982DFC74045012210FC68B07EB90F966F94B13DEF1CA057312A61  Exynos9820Audio.sys
    0B12D732B20F921C1BA398E17D0747580DC54810D855A3ED82B53A073CB9C6EE  Exynos9820Audio.inf
    C0FDAC4BBB77A0ADE59F341EF7357E1D7DD7E1FC95B274EE5CCE62470F6338C7  exynos9820audio.cat

### A7 device test
Same swap as a6 (AUD0 hidden, delete old oemN, stage a7, unhide) + stage A Parameters; audio-test.ps1 unchanged.
Expected in run.txt: `Exynos9820Audio 1.0.1.6 (ACX 1.1`, `circuit: host pin id 0, bridge pin id 1, KsOverride 1`,
`fmt C #n set pin#0: ...` (what the endpoint builder proposes), `ks: intersection range 0: size 88 ... -> 16 bit`,
`req c x1 prop Pin.4 get pin#0 ctl 184 val 0 ... done 80000005 info 104` followed by a sized
`... val 104 ... done 00000000 info 104` (the follow-up a6 never got). Success: audiotest GetMixFormat hr 0 (tag 0xFFFE
2 ch 48000 32-bit float mix), vol: hw support 0x3, `played 96000 frames`, run.txt `cb StreamPrepareHardware -> 0`,
`cb Run -> 0`. If the round still repeats with the sized intersection answered: the next failing step is visible as the
last request before the repeat. A/B: Parameters\KsOverride = 0 = a6 behaviour.
- Review of T11 / earlier HANDOFF commits: finding R5 - the three HANDOFF §32 audio addenda (a5, a6, a7) were written
  through a Python string where "\a" in "s10e-artifacts\audio-aN" became a BEL byte (0x07): the package paths read
  "s10e-artifacts<BEL>udio-aN". Fixed in its own commit (byte replace, 0 BEL bytes left; notes file checked: none).

## A8 (2026-10-13): a7 device result

Run audA7 (scratchpad\runs\audA7\last\audio): 1.0.1.6 loads at boot; KsOverride works - host-pin DATAINTERSECTION now
completes (size query `done 80000005 info 104`, sized `done 00000000 info 104`, `ks: intersection range 0: size 88 ...
-> 16 bit`). New endpoint {2b1f414a-...} state 1. WASAPI error changed: **0x88890008 AUDCLNT_E_UNSUPPORTED_FORMAT** on
GetMixFormat, IAudioEndpointVolume Activate and Initialize (a6: 0x80070032) - the intersection was a real blocker,
something after it still is.
- Key maxima (de-dup counters): Pin.14 set (ours) 2047, Pin.14 get / Pin.3 get / Pin.4 get 300 each, everything else
  <= 6. One round (repeats ~150 times, ~0.17 s apart, from 1.2 s to the 87 s log end, also without WASAPI calls):
  Pin.14 get default (ACX, size + 104 B) -> 12 Pin.14 sets (ours) -> Pin.3 DATARANGES (size + 184 B) -> Pin.4
  intersection (ours, size + 104 B) -> next round. So the decision that fails is taken in user mode after a round
  whose every answer is now well-formed.
- Decoded proposals (rounds 1-4 identical, #9-#20): 2 ch 48 kHz 32/24/16, 2 ch 44.1 kHz 32/24/16, 6 ch (mask 3F)
  44.1/48 kHz 32/24/16 - all EXTENSIBLE PCM, valid = container. Accepted: 2 ch 48 kHz 32/32 and 16/16 (no NO_MATCH line
  for #9/#11); the other 10 -> C0000272 (correct for our pin).
- Startup only (once, to ACX): 64-byte KSDATAFORMAT headers (specifier WAVEFORMATEX, no wave data) with subtypes PCM,
  0x92 (AC3-over-SPDIF) and 0x164 (WMA-SPDIF); one with flags 0x760065; one MUSIC/MIDI {E725D360}/{1D262760} probe whose
  intersection we answered NO_MATCH (correct, not audio). ACX's answers to the 64-byte ones are not visible.
- Not determinable from the KS log: which device format AudioSrv stored/chose and why it calls it unsupported. Next
  data must come from user mode: the endpoint property store (PKEY_AudioEngine_DeviceFormat {F19F064D-082C-4E27-BC73-
  6882A1BB8E4C},0 / OEMFormat {E4870E26-...},3, under HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\
  Render\{id}\Properties), the Microsoft-Windows-Audio event channels, IsFormatSupported, and an EXCLUSIVE-mode open
  (bypasses the engine's device format entirely).
- Safety (user): run audA7 ran with AmpInit/AmpEnable/AboxEnable = 1 because deleting the old package wiped the staged
  values and the INF defaults were 1 (nothing played: no stream ran). INF + driver defaults go to 0; test tone 0.25 ->
  0.03 amplitude, key steps ~10 % / ~6 % / ~16 %.

### Plan A8 (one commit each)
- T12 defaults AmpInit/AmpEnable/AboxEnable = 0 (INF AddReg + Driver.c AudReadConfig + header comments).
- T13 audio-test.ps1: amplitude 0.03, key steps 10/6/16 %; dump the endpoint property store format keys; IsFormatSupported
  (shared) for 48k/16 PCM, 48k/16 EXT, 48k float; one exclusive-mode 48 kHz 16-bit tone (quiet) before the shared tones.
- T14 extra-audio.cmd: MMDevices Render registry dump + Microsoft-Windows-Audio/* event channels.
- T15 driver: log ACX's own default + list formats at circuit creation; decode each DISTINCT proposed format (not just
  the first 48) with our verdict; version 1.0.1.7.
- T16 package audio-a8.
- T12: hardware-stage defaults OFF everywhere: INF AddReg AmpInit/AmpEnable/AboxEnable 0 (NOCLOBBER kept),
  Driver.c AudReadConfig defaults 0, header comments; HwVolume 0 / Stereo 0 / KsOverride 1 unchanged. Build exit 0,
  InfVerif VALID. The staged scripts (audio-stage-A..D.cmd) still set every value explicitly.
- T13 audio-test.ps1 (CRLF): amplitude constant Wasapi.Amp = 0.03 (shared tones and the exclusive tone); keys 0 then
  +5 (~10 %), -2 (~6 %), +5 (~16 %), mute, unmute -3 (~10 %); new `props:` (endpoint property store: every blob and
  every {F19F064D} engine / {E4870E26} OEM / {3D6E1656} / {1DA5D803} endpoint key), `formats:` (shared
  IsFormatSupported for 48k PCM16, EXT16, EXT float32 + device period) and an EXCLUSIVE-mode 1 kHz 48k/16 EXT tone
  (1.5 s, timer-driven, Initialize(period x4, period)) before tone A. Host check (this PC, Realtek endpoint, read-only
  calls only): compiled with Add-Type; `formats:` all hr 0, period 106667/30000; `props:` reference of a WORKING
  endpoint - {F19F064D},0 DeviceFormat = OEMFormat {E4870E26},3 = 48k/16 EXT blob (FEFF0200 80BB0000 00EE0200 0400 1000
  1600 1000 03000000 + PCM GUID), {3D6E1656},2/3/6/7 + {E4870E26},0 = 48k float32 EXT (the mix formats), {1DA5D803},8 =
  KSNODETYPE_SPEAKER, ,2 = KSNODETYPE_ANY, ,7 = 1 (event driven). On the phone the same dump shows which of these
  AudioSrv failed to write.
