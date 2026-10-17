# G17 - hardware video for Windows apps: D3D11 video DDI in the Mali UMD on top of the MFC

User decision 2026-10-17: Windows apps first (Media Foundation / msmpeg2vdec use D3D11 DXVA when the driver offers it;
no FL minimum). Browsers need FL 11_0 -> parkinglot/fl11-chrome-video.md. Research: notes/gpu-browser-video-research.md
"G17 research 2026-10-17". MFC side done: drivers/Exynos9820Mfc + MfcIoctl.h (stateful H.264 Annex-B decode, NV12 out,
bit-exact, 1080p 77 fps from user mode - notes/d10-mfc-decode-flow.md V3).

## Shape
- UMD (d3d10umd) exposes the video function table through RetrieveSubObject (MS Learn PFND3D10DDI_RETRIEVESUBOBJECT:
  ID 2 = D3D11_1DDI_VIDEODEVICEFUNCS, ID 3 = D3DWDDM2_0DDI_VIDEODEVICEFUNCS) - our runtime binding is WDDM 2.0.
- Decoder: profile D3D11_DECODER_PROFILE_H264_VLD_NOFGT, NV12 output views, ConfigBitstreamRaw 2 (short slices).
  Stateless -> stateful bridge: per picture, rebuild SPS/PPS NALs from DXVA_PicParams_H264 + DXVA_Qmatrix_H264 (+ the
  pps_id read from the slice header) when they change, then feed SPS/PPS/slices (Annex-B) to the MFC; display delay 0;
  copy the MFC's NV12 into the app's output view at EndFrame. POC type 1 -> unsupported (fail the config).
- Video processor: NV12 -> RGB (+ scale) for the MF renderer (VideoProcessorBlt) via Mesa's vl_compositor shaders.
- MFC access from the UMD: the KMDF device's IOCTLs (open the device interface from the app process; AppContainer
  callers (Photos, Media Player are packaged!) need an ACL with S-1-15-2-2 on the device interface - MFT research).

## Rungs (one commit each, each validated on the device)
- V6a  video DDI skeleton: RetrieveSubObject tables, profile/config/format queries, decoder + output view objects,
  NV12 resources in the UMD; test tool d3dvdec (ID3D11VideoDevice enumeration + CreateVideoDecoder) on the phone.
- V6b  DXVA H.264 -> Annex-B (SPS/PPS synthesis) + MFC IOCTL decode + NV12 copy at EndFrame; d3dvdec decodes a
  known clip through ID3D11VideoContext with Microsoft's DXVA structures, output hashes = mfcdec / libavcodec.
- V6c  video processor (NV12 -> BGRA blit) via vl_compositor; d3dvdec compares against WARP's VideoProcessorBlt.
- V6d  device ACL for AppContainer callers; Media Player / Photos play an H.264 file on the MFC (MF picks DXVA:
  check with ETW Media Foundation / msmpeg2vdec decode-mode), CPU load before/after.

## Progress log
- 2026-10-17 04:50 plan written.
- 2026-10-17 08:30 V6a code (Mesa working copy, mali-umd-0083 maliumd 9f2fd7eb, not yet on the phone):
  Video.cpp (H.264 VLD_NOFGT profile, NV12, 1 config ConfigBitstreamRaw 2 / DXVA_NoEncrypt, 4 buffer types, decoder +
  output view objects, Begin/Submit(+Submit1 WDDM2.0)/End record buffers - no decode yet), VideoRetrieveSubObject
  for SubDeviceID 3 (D3DWDDM2_0DDI_VIDEO_INPUT) and 2 (D3D11_1DDI_VIDEO_INPUT), hooked in CreateDevice via
  ppfnRetrieveSubObject for level >= 11.1 (D3D10UMD_NO_VIDEO=1 hides it). 74 generated stubs
  (tools/mesa-build/gen-video-ddi.py; VOID ones report E_NOTIMPL through SetError). NV12 resources: one R8 surface
  W x 1.5H (Resource::nv12_height), CheckFormatSupport(NV12) = DECODER_OUTPUT only. Test tool
  drivers/MaliG76Wddm/tools/d3dvdec (VIDEO_SUPPORT device, profiles, NV12, configs, decoder, NV12 texture, output view).
- 2026-10-17 (0083d, maliumd 09feef63 on the phone) d3dvdec: device VIDEO_SUPPORT OK FL 10_1, ID3D11VideoDevice OK,
  1 profile, NV12 OK, 1 config, CreateVideoDecoder -> 0x8000ffff (E_UNEXPECTED). UMD log ends after
  "buffer info 0": no index 1 query, no CreateResource, no Vid_CreateVideoDecoder, no video stub, no SetError.
- MS docs (rule 13): GetVideoDecoderBufferInfo remarks = runtime validates desc/Index BEFORE calling; Usage is
  D3D11_1DDI_VIDEO_USAGE (0 = PLAYBACK_NORMAL, valid). D3D11DDIARG_CREATERESOURCE has DecoderBufferType (minor header
  version >= 3, valid when BindFlags has D3D11_DDI_BIND_DECODER 0x200) -> decoder buffers are driver resources made
  through CreateResource; ours is never reached. Next: D3D11 debug layer message for the E_UNEXPECTED (tracing only).
- D3D11 debug layer: Graphics Tools (Tools.Graphics.DirectX~~~~0.0.1.0) NotPresent on the phone; Add-WindowsCapability
  over ssh -> 0x80070005 (network logon); running it as a SYSTEM task was declined by the auto-mode classifier ->
  needs the user (Settings > Optional features > Graphics Tools). d3dvdec got a "debug" mode (InfoQueue dump) for then.
- 2026-10-17 tools/phone/wddm2/vdecmatrix.ps1 (UMD tracing switches D3D10UMD_VID_NBUF/USAGE/SIZE, 0083 + CalcPrivate-
  ResourceSize log for BIND_DECODER), measured on the phone:
  - default (Usage 0): E_UNEXPECTED right after buffer info 0, no resource created. NBUF=1 same, SIZE=65536 same.
  - NBUF=0: CreateVideoDecoder OK -> NV12 texture (dim 3 fmt 103 bind 0x200) + output view -> d3dvdec PASS.
  - USAGE=1: E_UNEXPECTED. USAGE=2: all 4 infos queried, the runtime creates 4 buffers (dim 1 fmt 0, Width = Size,
    bind 0x200 DECODER, usage 2 DYNAMIC, cpu 0x1 WRITE), decoder created, PASS. USAGE=3: same with usage 3 STAGING,
    cpu 0x3, PASS.
  => the runtime reads D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO.Usage as a D3D10_DDI_RESOURCE_USAGE (0 DEFAULT,
  1 IMMUTABLE, 2 DYNAMIC, 3 STAGING) - the buffer resources it creates carry that usage - and rejects the two
  non-mappable ones (GetDecoderBuffer needs a CPU map). The MS Learn page names D3D11_1DDI_VIDEO_USAGE (PLAYBACK_NORMAL
  = 0), which is what we returned: doc and runtime disagree; measured behaviour wins. Fix: Usage = D3D10_DDI_USAGE_DYNAMIC
  (app writes, decoder reads - the D3D11 dynamic contract), tracing switches removed.
- 2026-10-17 V6a DONE: 0083 final (maliumd c642af77) on the phone: d3dvdec PASS (CreateVideoDecoder OK, the runtime
  creates the 4 DYNAMIC decoder buffers, NV12 BIND_DECODER texture, output view). Regression: d3dtri cube in the user
  session 36.5 fps, 0 present failures (DWM keeps the old DLL until the next boot). Mesa 0f47367 ->
  tools/mesa-build/patches/0083-video-ddi.patch. Next: V6b.
- Review of f8000ad / Mesa 0083 (rule 4):
  - F1 SERIOUS: every D3D11.1+ device now advertises H264_VLD_NOFGT while EndFrame decodes nothing -> MF apps
    (msmpeg2vdec in Media Player/Photos) would choose DXVA and show empty frames instead of software decode. Silent
    regression. Fix (0084): profile count 0 unless decode works; dev gate D3D10UMD_VIDEO_DEV=1 for d3dvdec until V6b.
  - F2: vdecmatrix.ps1 header names D3D10UMD_VID_* switches that the committed 0083 no longer has -> fix header.
  - Checked OK: VIDEO_INPUT.Relocate ignored (MS Learn: "whether the functions can be relocated" - our table never
    moves); only the video hook sets ppfnRetrieveSubObject (rg); unknown SubDeviceIDs -> E_NOTIMPL;
    CalcPrivateResourceSize pMipInfoList deref only on creation args (always present); getenv switches gone from
    Video.cpp (rg getenv: none).
- F1 fixed (0084, maliumd 6151a3bc on the phone): vdecmatrix default -> "decoder profiles: 0" (d3dvdec exit 3, MF keeps
  software decode); D3D10UMD_VIDEO_DEV=1 -> PASS. F2 fixed: vdecmatrix.ps1 header/cases now match the UMD.
- 2026-10-17 V6b design input: DXVA wants each picture in its own output surface at EndFrame (decode order); our MFC
  session returns display order (D_DISPLAY_DELAY never written = -1). LineageOS exynos9820 mfc (facts, no code):
  mfc_regs.h D_DISPLAY_DELAY 0xF0B8, D_DEC_OPTIONS 0xF0B4 bits: DISPLAY_DELAY_EN 3, FMO_ASO 4, IDR_DECODING 6,
  CONCEAL 8, PARALLEL_DISABLE 11, REALLOC 13, SPECIAL_PARSING 15, THUMBNAIL 16, DECODING_ORDER_ENABLE 17;
  mfc_cmd.c seq header: display_delay >= 0 -> EN bit + write D_DISPLAY_DELAY; decoding_order -> bit 17.
  D_DECODED_STATUS 0xF644 [2:0] 0 DECODING_ONLY 1 DECODING_DISPLAY 2 DISPLAY_ONLY 3 EMPTY.
  -> V6b1: MFC_OPEN_IN flag "decode order" (delay 0 + bit 17), mfcdec option, verify frame-type order on a B clip.
- V6b1 code: Exynos9820Mfc 1.0.0.7 (sys 5B65121A..., s10e-artifacts/mfc-1.0.0.7): MFC_OPEN_IN.Flags (optional; 8-byte
  input = 0) with MFC_OPEN_DECODE_ORDER -> SEQ_HEADER writes D_DISPLAY_DELAY 0 + D_DEC_OPTIONS 0xA400|bit3|bit17.
  mfcdec -order. Baseline (driver 1.0.0.6, display order) c1080.h264: 60 frames, types I B P B P ..., 0 errors,
  C:\S10eTmp\mfc\base-c1080.txt. Deploy via TWRP (pnputil live swap = 0x122 in D10): tools/phone/totwrp.cmd (new).
- 07:5x first V6b1 try: ordertest.ps1 -> -order gave the SAME display order (types 1323...). drvhash.ps1 (new): the
  service loads C:\Windows\System32\drivers\Exynos9820Mfc.sys (5B0F295E, 10/15 = 1.0.0.6), sys-deploy only swapped the
  DriverStore copy (5B65121A). sys-deploy.sh now also replaces the System32\drivers copy when it exists. Redo.
- 2026-10-17 08:05 V6b1 DONE (Exynos9820Mfc 1.0.0.7 loaded from System32\drivers, 5B65121A): ordertest.ps1 c1080 ->
  display 1323232..., -order 1232323... (I P B P B = decode order), same 60 picture hashes, 0 errors, display order
  identical to the 1.0.0.6 baseline; -order needs 62 calls instead of 64 (no display-only drain steps). After the
  reboot (UMD 0084 in DWM): cube 36.5 fps, 0 present failures. Battery 17 %.
- Review of f056296 (rule 4):
  - D_DISPLAY_DELAY persists in the MFC registers (0xF000.. kept until reset): a display-order session AFTER an
    order session was untested -> ordertest.ps1 -OrderFirst: display order still identical to baseline, same set. OK.
  - Old callers: Exynos9820MfcMft (Mft.cpp:439) and mfcdec build MFC_OPEN_IN by aggregate init -> Flags = 0; driver
    accepts 8-byte inputs (FIELD_OFFSET minimum) - checked in code, both sizes compile against the new header.
  - Unknown flag bits -> STATUS_NOT_SUPPORTED (Driver.c). Self-test session (Driver.c:198) zeroed by MfcSessionInit
    -> display order. OK.
  - Pre-existing, not from this commit: sys-deploy.sh picks the FIRST <inf>_arm64_* DriverStore folder (two exist for
    exynos9820mfc) - harmless here (service loads System32\drivers), noted for DriverStore-run drivers.
  - INF says 1.0.0.7 but the phone's DriverStore INF is still 1.0.0.6 (sys-only swap) - expected for sys-deploy.
- 2026-10-17 V6b2: tools/mfc/mftest -d3d11 (Source Reader + IMFDXGIDeviceManager on a Mali VIDEO_SUPPORT device) and
  tools/mfc/mfd3d.ps1 (sysmem vs d3d11 hashes + UMD video log).
  - profile hidden (0084 default): MF decodes in software into our NV12 textures (bind 0, then STAGING readback):
    30/30 frames, hashes IDENTICAL to sysmem -> NV12 texture upload/copy/map path correct.
  - D3D10UMD_VIDEO_DEV=1, 0084: MF calls QueryVideoCapabilities (WDDM 2.0 DDI, MS Learn: "WDDM 2.0 or later drivers
    must implement") -> our stub SetError(E_NOTIMPL) -> device removed, ReadSample 0x887a0005.
  - 0085 (maliumd d5f7aca8): QueryVideoCapabilities implemented (DECODER_CAPS 0 for H.264 <= 1920x1088, no
    downsampling) -> Microsoft's decoder takes the DXVA path: CreateResource NV12 1280x720 ARRAY 12 bind 0x200,
    CreateVideoDecoder 1280x720 raw 2, 30 frames each with PP 1040 B / IQ 224 B / slice control 110 B (11 short
    slices x 10 B) / bitstream (39040, 20352, ...), DECODER_CAPS re-asked every frame, decoder destroyed after 30.
    Hashes differ (no decode yet, expected). Next V6b3: the decode (SPS/PPS synthesis + MFC + NV12 copy).
- Review of 749e3bf / 0085: DECODER_CAPS treats pCryptoType = NULL or DXVA_NoEncrypt as clear; a pointer to GUID_NULL
  would read as UNSUPPORTED - not seen (MF got 0), fold into V6b3 (accept GUID_NULL). Histogram query (2.4) -> SetError
  only reachable through 2.4 tables we do not expose. Generator: 73 stubs (was 74), wddm2_0 fill points at
  Vid_QueryVideoCapabilities (git diff of the .inc). mfd3d.ps1 hash split checked against mftest's line format.
- V6b3 code (Mesa working copy, maliumd 679fa413): VideoH264.c/.h (SPS/PPS synthesis from DXVA_PicParams_H264 +
  DXVA_Qmatrix_H264: High profile, level = smallest of Table A-1 holding frame size and num_ref_frames, gaps allowed,
  cropping to the decoder's output size, no VUI; PPS id from the first slice header; scaling lists explicit in the
  PPS when not flat; POC type 1 / FMO / non-4:2:0-8bit refused). dxva.h does not compile as C++ under clang-cl
  ((~0)<<n enum masks) -> DXVA structs only in the .c, sizes static_asserted (1040/224/10 = what MF submits).
  Video.cpp: MFC session per decoder (cfgmgr32 interface list -> CreateFile -> IOCTL_MFC_OPEN DECODE_ORDER; failure
  -> CreateVideoDecoder E_FAIL = caller's software fallback), buffers copied at SubmitBuffers (pipe_buffer_read),
  EndFrame = SPS/PPS when changed (SPS change after pictures -> session restart) + slice NALs -> IOCTL_MFC_DECODE
  loop -> texture_subdata of luma + CbCr into the output view's array slice. Tracing: the Annex-B stream goes to
  %MALIKM_TGSI_DIR%\vdec-<pid>-<obj>.h264.
- 2026-10-17 V6b3 DONE on the phone (0086, maliumd 679fa413, driver 1.0.0.7), mfd3d.ps1 -Dev:
  - c720.mp4 (30 frames): the MFC decodes every picture (first call = header, consumed 15 = synthesized SPS+PPS;
    then ~5 ms per picture), hashes IDENTICAL to the sysmem (Microsoft software) decode, in order.
  - c1080.mp4 (60 frames, I P B P B decode order): decoded 60 failed 0, ~10 ms per picture. Full 1088 rows differ
    from software; -Rows 1080 (mftest -rows, new): all 60 IDENTICAL, frame 0 = mfcdec/libavcodec 0x26ffa70f. The 8 rows
    under the crop (1080..1087) are not defined output - software and MFC fill them differently.
  - mftest -d3d11 fps (20.9 at 1080p vs 40 sysmem) includes MF's per-frame readback to system memory (staging copy
    + map) - not a playback number; measure with a renderer (V6c/V6d).
- Review of 5f6fd71 / 0086 (rule 4), checked in the code:
  - header loop: Consumed 0 + HEADER/NOT_CONSUMED -> same window again, Consumed 0 otherwise -> stop (no spin; <= 8
    tries); driver reports Consumed = window once a picture's NALs are used (Decode.c) -> loop ends on FRAME.
  - SPS change after pictures -> MfcRestart (CLOSE + OPEN, PPS cache cleared); a failed reopen leaves the handle
    without a session -> later frames counted as failed, logged; no device removal (EndFrame never SetErrors).
  - WriteOutput: rows = min(texture luma height, MFC height), CbCr source at Width*Height of the MFC picture ->
    correct for 1080-row textures fed from 1088-row pictures (not yet run: MF used 1088 textures).
  - CreateVideoDecoder: MfcOpen failure -> E_FAIL before any allocation; VidReserve failure closes the handle.
    The device is opened exclusively (share 0) + one MFC session device-wide -> a second decoder (any process)
    gets E_FAIL = that caller decodes in software.
  - Unvalidated (stay Guided): interlaced/MBAFF (mb_adaptive only inferred from MbaffFrameFlag), PPS-id switching,
    seeks, AppContainer callers (V6d), streams with scaling matrices (c720/c1080 are flat).
  - Fixed (0087, maliumd d50ade0a): DECODER_CAPS treated a GUID_NULL crypto type as encrypted. Rerun after the fix:
    c720 30/30 and c1080 (-Rows 1080) 60/60 identical.
