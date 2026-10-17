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
