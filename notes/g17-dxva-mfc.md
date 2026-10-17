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
- 2026-10-17 V6c1: tools/mfc/mfplay (Media Engine frame server on the Mali: Play + OnVideoStreamTick +
  TransferVideoFrame into a BGRA RT, muted, staging readback hashes) + mfplayq.ps1. c720.mp4 (1 s, 30 frames):
  - both without and with the DXVA profile: playback runs and ends, frames correct-looking (frame 0/1 hashes equal in
    both runs), BUT MF calls CreateVideoProcessorEnum ~25x (our stub -> E_NOTIMPL) and falls back to its CPU
    conversion: only 5 (sw decode) / 9 (MFC decode) of 30 frames reach the texture in ~2.5 s (2-3 fps delivered).
  - with the profile the MFC decodes all pictures (~5 ms each) -> the bottleneck is the missing video processor.
  => V6c2: D3D11 video processor DDI (enum + caps + processor + input/output views + VideoProcessorBlt NV12->BGRA
  with scaling on the GPU).
- V6c2 design: 48 VP members in D3DWDDM2_0DDI_VIDEODEVICEFUNCS (enum/caps/format checks, processor, ~25 VOID
  setters - the stubs SetError(E_NOTIMPL) on VOIDs = device removal once MF reaches them -, input/output views, Blt,
  WDDM2.0 additions ColorSpace1/Mirror/ShaderUsage/BehaviorHints/FormatConversion). New file VideoProc.cpp.
  Blt = an internal draw on the app's context: the frontend binds state straight to the pipe (cso only for vertex
  elements), so the draw must restore what it touches from Device tracking (fb, samplers, views, SO, bound_vs exist;
  blend/dsa/raster/viewport/scissor/fs/gs/CB to be tracked) - or use its own resources only.
- V6c2 code (maliumd 92ad5b80): VideoProc.cpp (all 48 VP members; caps: NV12 in, BGRA/RGBA/BGRX out, 1 stream,
  1 rate-conversion entry (progressive, no refs), YCbCr matrix + range conversion; setters store src/dst/target
  rects, background, colour spaces (CS1 too); extensions E_NOTIMPL), VideoCsc.c (NIR compute kernel: txf luma +
  2x2-block chroma from the R8 NV12 surface, 3x4 matrix from UBO 0, image store RGBA8; workgroup 8x8), Blt =
  kernel -> processor-owned RGBA8 picture -> pipe->blit (scale/format/dst rect) + clear_render_target background.
  VP members NOT behind D3D10UMD_VIDEO_DEV (decode profile only) - decide after validation.
- 2026-10-17 V6c2 on the phone (0088, maliumd d457a64f):
  - first run: panfrost assert "s->info.first_ubo_is_default_ubo" (pan_shader.c:200) -> the kernel sets it. Fixed.
  - mfplay c720 (sw decode and MFC decode): VP created (1280x720 -> output), Blt runs, center pixels of frames 0-4 equal
    MF's CPU conversion exactly (0,119,155 / 21,131,189 / 108,68,0 ...); full-frame hashes differ (chroma
    upsampling / rounding of the 2x2-block kernel vs MF's filter) - not bit-exact by design.
  - the earlier "5 frames / 2.3 s" was mfplay reading back EVERY frame (staging copy + map) on a 1 s clip -> mfplay
    -noread (frame 0 only).
  - test.mp4 (1080p H.264, 17.5 s, 24 fps) -Dev -NoRead: MFC decodes, VP converts: 332 frames in 15 s, blts every
    41.7 ms (= 24 fps, real time; the average includes start-up), Blt CPU time ~3.5 ms (first 14.5 ms incl. kernel
    compile). Interlaced enumerators refused (MF probes them first, then progressive).
- Review of fccd4d1 / 0088 (rule 4), checked in code:
  - no app state touched: compute state + sampler view/image/CB on the compute stage only (unbound after dispatch);
    blit/clear are driver-internal (panfrost util_blitter save/restore). D3D FL10_x never binds compute.
  - the CB user_buffer points at a stack struct: panfrost consumes it inside launch_grid (descriptor emission), and
    it is unbound before return -> no dangling pointer.
  - GetVideoProcessorCustomRate / FilterRange SetError: unreachable for valid calls (CustomRateCount 0, FilterCaps 0;
    the runtime validates indices against the caps first - MS Learn remarks of the D3D11 API methods).
  - input views refuse non-NV12 (consistent with CheckVideoProcessorFormat: only NV12 INPUT).
  - not bit-exact vs MF's CPU converter (chroma 2x2 block, no interpolation) - by design, center pixels equal.
  - FOUND: VideoProc.h claimed "generated from VideoProc.cpp" but the generator lived only in the scratchpad (rule:
    debug tools in repo) -> tools/mesa-build/gen-videoproc-h.py, header regenerated with it (0089).
  - Unvalidated: background fill / letterboxing (dst != target), studio-RGB output, BGRX output, 601 content, real
    apps (Media Player / Photos: AppContainer -> V6d).
- 2026-10-17 V6d: tools/mfc/acrun (CreateAppContainerProfile + CreateProcess with SECURITY_CAPABILITIES, no
  capabilities) + actest.ps1 (-Setup fixture C:\S10eTmp\ac readable by S-1-15-2-1) + actestu.ps1 (runs it in the
  user's session: from SSH an AppContainer child dies 0xC0000142, no window station).
  - mftest -d3d11 in the AppContainer, -Dev: UMD "MFC open failed 5" (ACCESS_DENIED) -> MF software decode
    (texture bind 0), 22 frames reported (30 outside - to recheck after the fix).
  - mfcacl.ps1: DEVPKEY_Device_SecuritySDS empty (class default; no AppContainer ACE).
  - MS Learn: device security = INF DDInstall.HW AddReg "HKR,,Security,,<SDDL>" (whole stack, at device start);
    after installation via the device installation functions (DEVPKEY_Device_SecuritySDS / SPDRP_SECURITY_SDS).
    The HSA custom-capability route is for apps that open devices themselves - not our case (a system UMD in any app).
  - INF 1.0.0.8: Security "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)".
    Attack surface note: sandboxed apps can now send bitstreams to the MFC driver/firmware (same exposure as a GPU
    vendor's D3D11 decode path); the driver checks window sizes (MFC_MAX_WINDOW) - the firmware parses the stream.
- mfcacl.ps1 -Set (CM_Set_DevNode_PropertyW DEVPKEY_Device_SecuritySDS, CR 0): pnputil /properties shows the SDDL
  (Get-PnpDeviceProperty does not list that key - readback switched to pnputil). Effective at the next device start
  -> clean reboot (no live device restart: the D10 live driver swap ended in 0x122).
- 08:4x after the reboot still "MFC open failed 5". mfcsd.ps1 (new: CreateFile READ_CONTROL on the interface path +
  GetSecurityInfo): device object DACL = D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1201bf;;;WD)(A;;0x1200a9;;;RC) = the
  default; the property had not applied. pnputil showed only {4d1ebee8...},26 printed by GUID, no
  DEVPKEY_Device_Security: CM_Set_DevNode_PropertyW stored a plain property. mfcacl.ps1 rewritten to the documented
  SetupDiSetDeviceRegistryPropertyW(SPDRP_SECURITY_SDS): now DEVPKEY_Device_Security (binary, named) =
  D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GWGR;;;WD)(A;;GWGR;;;AC)(A;;GWGR;;;S-1-15-2-2); stray [26] deleted (-CleanStray).
  Rebooting again.
- 2026-10-17 08:49 V6d1 DONE: after the reboot mfcsd.ps1 -> device object DACL
  D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x12019f;;;WD)(A;;0x12019f;;;AC)(A;;0x12019f;;;S-1-15-2-2). actestu -Dev: inside the
  AppContainer the UMD opens the MFC ("decoder 1280x720 created, MFC session open"), 30 frames decoded on the MFC
  (decoded 30 failed 0), texture array 12 bind 0x200 (DXVA path); before: ACCESS_DENIED -> software, 22 frames.
  Fresh installs get the same descriptor from INF 1.0.0.8 [Mfc_Install.NT.HW] (not reinstalled on the phone: the
  live driver reinstall risk, D10 0x122) - the phone's devnode got it through mfcacl.ps1 -Set (documented
  post-install path), so INF and devnode agree.
- Review of 3c547af (rule 4): infverif /w on the INF: only the pre-existing DIRID 12 (System32\drivers) errors,
  nothing on the Security AddReg; package + catalog build. Found: (1) the default descriptor's RC (restricted
  token) ACE was dropped -> restricted-token sandboxes would lose access; (2) GRGW was more than needed - every MFC
  IOCTL is FILE_ANY_ACCESS. Fixed: UMD opens GENERIC_READ (0090, maliumd b4f226ff), mfcdec + MFT too, INF 1.0.0.9
  "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)(A;;GR;;;AC)(A;;GR;;;S-1-15-2-2)(A;;GR;;;RC)", devnode via mfcacl -Set.
  After reboot: device object DACL D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)(A;;FR;;;AC)(A;;FR;;;S-1-15-2-2)(A;;FR;;;RC);
  AppContainer decode 30/30 on the MFC; ordertest (mfcdec read-only handle): 60 frames, decode order, identical.
- 2026-10-17 V6d2 (0091, maliumd 1cb628cb): the D3D10UMD_VIDEO_DEV gate is gone; the profile is exposed when the
  MFC device interface is present (CM_Get_Device_Interface_List_Size, once per process; absent -> hidden, callers
  keep software). No environment variables: mfd3d c1080 -Rows 1080 -> DXVA texture array, 60/60 identical;
  actestu (AppContainer) -> MFC session open, 30/30 decoded. (-Dev switches of the test scripts are now no-ops.)
- 2026-10-17 09:0x V6d2 real app: Photos (Microsoft.Windows.Photos 2026.11090, Arm64, AppContainer) playing
  test.mp4 (tools/mfc/photostest.ps1: copies the clip to the user's Videos, ms-photos:viewer, probes the MFC).
  - while Photos plays, a second MFC session is refused (IOCTL_MFC_OPEN 170 ERROR_BUSY) -> Photos holds the decoder;
    a few s after Photos exits the MFC is free again (mfcdec 30/30) - the "busy right after kill" is teardown.
  - machine-wide trace (umdtrace.ps1, then -Off), photoslog.ps1: Photos' UMD log: "MFC present - profile exposed",
    VP 1920x1080 created, decoder 1920x1088 "MFC session open", frames I/P/B at ~8 ms, VP blts 1920x1080 ->
    1133x637 (its window) at ~4.5 ms CPU; blt 100 -> 200 in 6.7 s = ~15 blt/s (clip is 24 fps) -> frames likely
    dropped somewhere (present/composition or MF late-frame drop): follow-up.
  - photostest's CPU figure (297 ms in every run) is not trustworthy (same value for 10/15/22 s) - drop it.
  - GDI screenshot (shot.ps1) all white, wddm2dbg fb dump all black (stale scan-out register on the WDDM2 path?),
    PC webcam (camshot.ps1) returns empty JPEGs now -> no visual confirmation yet (needs the user or a working
    capture path).
- Review of f29670e: photostest printed a CPU figure that is not trustworthy (identical 297 ms for 10/15/22 s
  windows) while its header advertised it -> removed (follow-up commit); the evidence is the MFC ownership probe and
  the UMD log.
- 2026-10-17 09:2x Photos frame rate investigation (temporary UMD timing builds, machine-wide trace on/off):
  - decode timeline (every-100th + window logs): frames 0-39 at ~12 fps incl. start, then frames 40->100 in 16 s
    (~3.8 fps) while VP blts continue at ~16-20/s -> MF quality management skips decodes because the renderer is late;
    the VP re-blits old frames. Not the GPU clock: identical at G3D 702 MHz (governor stopped, then restarted).
  - only one shim idle wait (485 ms) in a whole run -> not the cause.
  - _Present timing (presents 100-139, both Photos swap chains 637x1133 video + 1152x1152 UI): flush 0.1-3 ms, GPU
    wait < 0.6 ms, gdi_present window search 0.8-4.6 ms (never finds a window: wasted, "none drawn"), runtime
    allocation present ~0.1 ms (OK). Present intervals of the video chain: 31 / 62 ms multiples.
  - hypothesis "KMD soft vsync timer (KeSetTimerEx 16 ms) rounds to 31.25 ms at the default clock tick" -> measured
    with drivers/MaliG76Wddm/tools/vblank (D3DKMTWaitForVerticalBlankEvent, user session via tools/phone/userrun.ps1):
    mean 16.13 ms (62.0 Hz), min 8.92, max 39.08 ms -> NOT 32 Hz; jittery (timer-based) but 60 Hz on average.
  - open: why Photos' presenter paces at 31/62 ms (DXGI present statistics / composition token completion on this
    stack?). mfplay (frame server, no presents) runs real time. Next step: present-statistics / frame latency
    waitable behaviour of composition swap chains on the maliwddm runtime (MS Learn first).
  Phone UMD after the cleanup: maliumd 9d9ca53a (0092, no timing code).
- drivers/MaliG76Wddm/tools/dwmrate (DwmGetCompositionTimingInfo over N s, user session): refresh rate
  26000000/427882 = 60.8 Hz; DWM frames and refreshes ~0.2/s both idle and during Photos playback (frames displayed
  0) -> DWM is not composing new frames while Photos presents (or these counters are not maintained on this stack).
  Session active, not locked (no LogonUI), governor ScreenOffs 0, powercfg /requests empty.
  shot.ps1 now returns the same all-white 8614 B image with Photos closed -> GDI capture is blank in general today,
  not evidence about Photos. Visible playback in Photos is UNVERIFIED (needs the user's eyes): decode + VP work
  (UMD log), presentation/composition of its swap chain on this stack is the open question.
- 09:2x display state: the cube ran at 416 fps (presents occluded = display off) before wake.ps1 and at 33.6 / 37.0
  fps after it (still 37 after 60 s idle); powercfg VIDEOIDLE = never on AC/DC. Some Photos runs had the display
  off, but the last run (display awake) shows the same decode curve: frame 100 at +19.8 s, blts ~16-20/s.
  => Not display-off, not GPU clock, not decode/VP cost, not present CPU cost. Remaining suspect: composition of
  Photos' (XAML SwapChainPanel / composition) swap chain on this stack - DWM composes ~0.2 frames/s (dwmrate), so
  buffers return late and MF's quality control skips decodes. Same family as HANDOFF H5 ("DWM never consumes
  redirected present tokens on this stack"). Separate milestone (composition swap chains), needs the user's eyes.
- 2026-10-17 09:30 robustness clips (tools/mfc/mkclip.py gained an optional x264-params argument; host PyAV,
  s10e-artifacts/mfc-clips/*.h264 + .mp4): cqm (High, cqm=jvt scaling matrices), base (Baseline, CAVLC, no B),
  main (Main, no 8x8), pyr (High, 3 B, ref=4, b-pyramid=normal, weightp=2), odd (854x480: cropping).
  To run on the phone through MF (mfd3d.ps1: Microsoft software decode vs our DXVA/MFC, same MF pipeline).
- 09:38 soak (tools/phone/wddm2/soak.sh 10, UMD 0092 in new processes, DWM on 0090): VERDICT OK, DWM 1332 alive,
  0 crashes over 10 min.
- 09:4x cliptest.ps1 (MF software vs our DXVA/MFC, all via the DXVA path, bind 0x200): base, cqm (scaling matrices ->
  explicit PPS lists), main, pyr (ref 4, b-pyramid, weightp) IDENTICAL in order (60/60). odd (854x480): MF reports
  864x480 frames, the 10 padding columns differ; mftest -cols / mfd3d -Cols (new): -Cols 854 -> 60/60 IDENTICAL,
  frame 0 0x6ea9628c / 0x340ec26d both. Cropping, CQM, Baseline/Main/High, B-pyramid, weighted prediction validated.
  Still unvalidated: interlaced/MBAFF, multiple PPS ids, seeks, resolution change mid-stream, POC type 1 (refused).
- Review of 04651ac: mfd3d/mfplayq/actest(u) still documented and set D3D10UMD_VIDEO_DEV (dead since 0091), and
  vdecmatrix defaulted to a VIDEO_DEV case -> switch removed from all of them (follow-up commit); rerun after the
  cleanup: mfd3d c720 30/30 identical, vdecmatrix PASS (1 profile without any env), actestu MFC session in the
  AppContainer, 30/30 decoded.
- 2026-10-17 ~11:00 USER: "photos took forever to load and is barely running" -> confirms the slowness is real and
  not limited to video: the whole Photos UI (WinUI/XAML, DComp composition) is slow. Investigating the composition
  present path next.
- 2026-10-17 11:1x SYSTEMIC: tools/phone/wddm2/swaprate.ps1 (d3dswap now times Present(1,0); user session, display
  awake): composition swap chain flip-sequential 2 buffers 300 presents = 7.9/s (slowest 250 ms); plain window
  swap chain flip-sequential 2 buffers = 7.7/s (slowest 268 ms). Every DWM-composed flip-model swap chain runs at
  ~8 presents/s on this stack -> Photos (and any flip-model app) is slow regardless of video. Our UMD's _Present is
  ~2 ms (earlier timing), so the 125 ms per present is spent waiting in the runtime for a buffer DWM has not
  released (flip queue) - DWM consumes these surfaces at ~8 Hz. (GDI-drawn windows: d3dtri cube 33-37 fps.)
- 11:2x USER is installing Graphics Tools (D3D11 debug layer) on the phone - in progress.
- 11:3x ROOT CAUSE (drivers/MaliG76Wddm/tools/kmdstats, new: MW_ESCAPE_STATS rates): idle 0 presents; with a
  composition swap chain presenting: KMD presents 6.6/s, ALL CPU copies (PresentBlts 6.8/s, PresentGpu 0), GPU
  renders/submits/irqs 167/s, queue empty. DWM's flips to the primary are latched at the soft vsync (command.c Latch)
  by DoPresent MW_PRESENT_BLT = RtlCopyMemory of the whole 1080x2280 primary (~9.8 MB) into the GOP framebuffer at
  DISPATCH level in the DPC -> DWM paced to ~7 frames/s for anything that needs a new DWM frame (flip-model apps,
  Photos). The H4a GPU path (Src->GpuPresented: the UMD already copied the frame on the GPU) is not taken.
- 11:4x why no GPU present: the display is landscape now (tools/phone/orient.ps1: 1013x480 logical, rotated).
  2026-10-14 notes (gpu-hw-path.md 19:01): in landscape DWM pre-rotates into a 1080x2280 primary and the identity-size
  GPU blit path is used ("primary is GPU-presented", Mesa 0067 fixed the stale kprim gpu flag); 10-16 DWM log
  (traces/wddm2-2026-10-16/dwm-redir-1332.txt) shows "scanout present N: blit+wait 0 ms" = GPU path. Today the KMD
  reports PresentGpu 0 / PresentBlts 6.8 per s -> DWM's presents are on the CPU path again. Needs DWM's UMD log:
  reboot with machine-wide umdtrace (after the user's Graphics Tools install finishes).
- 12:0x USER: Graphics Tools install finished with "couldn't add" (likely no Windows Update source; tools/phone/wuoff.cmd exists) - debug layer unavailable. Proceeding with the traced reboot (DWM UMD log).
- 12:1x after the traced reboot (DWM log machine-wide): DWM "primary 1080x2280 rotation 2", "scanout imported: yes",
  "primary 0x40001480 is GPU-presented", "scanout present N: blit+wait 0-16 ms". kmdstats during d3dswap comp:
  presents 58.4/s ALL GPU (cpu copy 0), d3dswap composition swap chain 300 presents = 56.1/s (was 7.9).
  => the CPU-copy state was acquired during the long session (boot 08:51, several UMD swaps, display off/on
  episodes), not the steady state. Trigger UNKNOWN - candidates: display off/on or a mode change recreating DWM's
  primaries without the GPU-presented mark. Watch: kmdstats PresentBlts vs PresentGpu.
- USER asked to run DISM /Online /Add-Capability Tools.Graphics.DirectX: Error 5 Access denied (ssh token High
  Mandatory, Administrators). CBS.log: "Failed to download FOD from WU [E_ACCESSDENIED]"
  (WindowsUpdateDownloadFromUUP 0x80070005). Phone still has policy SetDisableUXWUAccess=1 and UsoSvc disabled
  (tools/phone/wuoff.cmd) -> the feature download from Windows Update is refused.
- 12:3x Photos stall analysis (TEMP timing build maliumd 46d82894): EndFrame 12-17 ms (NV12 write 3-5 ms); gaps
  between EndFrames 30-60 ms, runs of 100+ ms and stalls of 4.2 s and 9.4 s, then catch-up bursts. During a stall
  Photos keeps presenting the video swap chain at ~16-20/s (gdi present lines every 47-62 ms) and the VP re-blits:
  MF's quality control stops decoding because the presenter is late. A 24 fps clip on 60 Hz needs presents locked to
  vsync; ours is a jittery soft timer (vblank tool: 8.9-39 ms intervals). Also: gdi_present enumerates all windows
  on every present (1-4 ms) and never finds one for composition swap chains.
- MS Learn "High-Resolution Timers" (kernel/high-resolution-timers): "To create a high-resolution timer, a WDM
  driver calls the ExAllocateTimer routine and sets the EX_TIMER_HIGH_RESOLUTION flag ... the timer can expire as late
  as about a millisecond after the specified expiration time but it never expires early." "drivers should avoid
  setting the period of a long-running high-resolution timer to a value that is less than the default interval
  between system clock ticks" (16.67 ms > 15.6 ms default tick: OK; the timer runs only while vsync is enabled).
- 12:4x fixes built: KMD 1.0.62.1 (s10e-artifacts/kmd-1.0.62.1, sys 6131BCD3): soft vsync on a high-resolution
  EX_TIMER (ExAllocateTimer EX_TIMER_HIGH_RESOLUTION, ExSetTimer period 166667 x 100 ns), KTIMER fallback,
  ExDeleteTimer at RemoveDevice. UMD (maliumd 2be0d7c9): gdi_present remembers up to 4 window-less sizes and searches
  them again only every 120th present; the TEMP EndFrame timing removed. Deploy: UMD live, KMD from TWRP.
- 12:3x KMD 1.0.62.1 loaded (drvhash 6131BCD3). vblankq.ps1 (new): normal priority mean 16.94 ms, p10/p50/p90 16.66/16.77/17.54, max 22.7, 0 of 240 > 25 ms; time critical 15.58-18.50 ms. (Old KMD: 8.9-39 ms.) Mean 59.0 Hz: HR timer expires up to ~1 ms late, never early.
- 12:4x after KMD 1.0.62.1 + UMD 0093: swaprate composition 58.9/s (slowest 36 ms), window 59.2/s (vsync-locked).
  Photos still ~5 fps decode / ~15 blt/s (frame 100 at +20.6 s) -> DWM, vsync and decode are fine; the limit is in
  Photos' own render loop (~66 ms per video frame). Next suspect: event queries (QueryGetData with DO_NOT_FLUSH
  blocks in our UMD: wait = Flags & DO_NOT_FLUSH) - TEMP timing next.
- 12:5x Photos: TEMP event-query timing -> no event-query polls at all (not the cause). tools/phone/cputop.ps1:
  Photos 115 % of one core (6 cores), svchost 26, MsMpEng 26, dwm 18, System 14; CPUs already at max clocks, GPU at
  the 260 MHz floor. tools/phone/threadtop.ps1: no Photos thread pegged (busiest 21 % / 18 %, most threads waiting).
  => Photos waits on something per frame, it is not CPU-bound. Open.
  Phone after cleanup: KMD 1.0.62.1, UMD 0093 (maliumd fede1bcd; same source as 2be0d7c9, rebuilt), machine-wide trace OFF (DWM keeps tracing until the next boot).
- 12:5x USER turned the screen off and on, asked whether the fallback happened. Measured right after:
  kmdstats during d3dswap comp: KMD presents 6.6/s and 9.0/s, ALL GPU (cpu copy 0) - right after the reboot the same
  test gave 58.4/s; d3dswap itself still 58.0/s. vblankq: 59 Hz, time-critical p10/p90 16.70/17.35 ms (vsync timer
  survived). No lock screen (no LogonUI). PRAM heartbeat over 7.0 s: vsync +413 (59 Hz), p (presents + latches)
  +323 = 46/s. => the screen off/on did NOT bring back the CPU-copy path, but DWM's way of presenting changed
  (DdiPresent 58/s -> 7-9/s, the rest as vsync-latched flips). The earlier CPU-copy state has another trigger
  (candidates left: UMD swaps while DWM runs, a rotation/mode change, long uptime).
- 13:0x USER: mpv instead of Photos -> "nothing is playing, blank screen". tools/mfc/mpvtest.ps1 (new): mpv
  (C:\S10eTmp\mpv, D10 V5 custom llvm-mingw build) --hwdec=d3d11va --vo=gpu --gpu-api=d3d11 --d3d11-warp=no:
  vo/gpu/d3d11 on the Mali at FL 10_1, flip model, R8G8B8A8 swap chain, rgba16hf FBOs, shaderc ok;
  "Unsupported hwdec: d3d11va" (this ffmpeg build has no D3D11VA) -> software decode; "first video frame after
  restart shown" at 0.87 s, then the log stops at 1.06 s, status stuck at V 00:00:00 Dropped 2 -> mpv's GPU render
  loop hangs after the first frame (the D10 T4 "mpv black on Mali").
- 13:1x the hung mpv (pid 5148) cannot be killed: taskkill "SUCCESS" but the process stays with 1 thread waiting
  (WaitReason Executive) = stuck in a kernel wait during process exit; GPU queue empty (fence 203183/203183); KMD DDI
  ring balanced (no DDI inside our driver). Later mpv runs collide with it (no log). Reboot needed.
- 13:1x after the reboot, mpv with machine-wide trace (malikm-mpv.exe-7732): FL 10_1 device, 3 present buffers
  1704x959 (shared BOs), first presents go through gdi_present (its window found) at 62-63 ms each (CPU readback +
  SetDIBits), then "idle 10015 ms via event, ret -110": the shim's wddm_idle waited 10 s for ddi.last_event and timed
  out, while the KMD reports every fence complete -> the event the shim waits on is never signalled (no GPU hang).
  Just before: "CB ok? Signal2 hr 0" x2 (mpv's device signals a monitored fence through the runtime). mpv stays hung
  (pid 7732). Also: windowed hwnd swap chains present by GPU->CPU readback + GDI = 63 ms per 1704x959 frame (~15 fps
  cap for windowed apps like mpv) - separate item.
- 13:2x analysis: the shim's per-submission completion event is EnqueueCpuEvent on the context; it never fires ->
  the context is stalled behind a wait. mpv hung after exactly 3 presents = its BufferCount. Its presents were drawn
  by gdi_present (window of that size found) and the runtime present was skipped (H5 0063 rule: "drawn into its window
  -> no runtime present", from when DWM did not consume redirected presents). Flip model then waits for buffer
  releases that never come -> context stall. d3dswap hwnd ran fine because its window size differs from the buffers
  (no gdi match -> runtime present). Test: MALIKM_NO_GDI_PRESENT=1 machine-wide (diagnosis only, rule 12).
- 13:3x with MALIKM_NO_GDI_PRESENT=1 (machine, diagnosis) after a reboot: mpv --vo=gpu d3d11 on the Mali plays and
  exits normally ("Exiting... (End of file)", no stuck process); presents go to the runtime ("present allocation N:
  hr 0"), V reached 00:00:09 of the 10 s window, no drops in the status line. => gdi_present's skip of the runtime
  present is what hangs flip-model windowed swap chains. Native fix = UMD default (not the env var).
- 13:4x UMD default GDI off (maliumd eb4985ff), env var removed: mpv plays + exits natively (End of file, 0 stuck). BUT windowed d3dtri cube: 4.8 fps via the runtime present (was 33-37 via gdi_present) -> the runtime present is slow for its swap chain kind; need to keep GDI for that kind only.
- 13:5x swap chain kinds in the UMD trace: d3dtri cube (blt model, DISCARD) buffers misc 0x8 DISCARD_ON_PRESENT,
  not shared; mpv (flip model) buffers misc 0x20002 SHARED | DISPLAYABLE_SURFACE (DWM opens them). Mesa 0094
  (maliumd d8c86f68): Resource::flip_model = BIND_PRESENT && MISC_SHARED; gdi_present only for non-flip-model buffers.
  Natively (no env): d3dtri cube 35.7 fps (GDI path), mpv --vo=gpu d3d11 plays to EOF and exits (0 stuck),
  swaprate comp 59.1/s, hwnd 59.3/s. MALIKM_NO_GDI_PRESENT machine variable removed.
- 14:0x mpv on the phone with its config (tools/mfc/ffmpeg/mpv.conf now: hwdec=exynosmfc-copy, vo=gpu, gpu-api=d3d11;
  the WARP interim removed; old config kept as portable_config/mpv.conf.warp-20261017): first run software decode -
  "h264_exynos: cannot open the Exynos9820Mfc device (5)": the ffmpeg wrapper opened GENERIC_READ|GENERIC_WRITE and
  the device grants users read only since INF 1.0.0.9 (the V6d1 review fixed UMD/mfcdec/MFT but missed this wrapper).
  h264_exynos.c -> GENERIC_READ; incremental llvm-mingw rebuild (build-ffmpeg.sh), avcodec-63.dll 519acdf4 deployed
  (old kept as .prev-20261017). mpvtest test.mp4 15 s: "Using hardware decoding (exynosmfc-copy)", Mali FL 10_1
  renderer, played to 00:00:14 of the window, no Dropped counter, exits at EOF. => mpv: MFC decode + Mali render.
- 14:1x USER: mpv black screen - audio plays, Task Manager shows GPU use, nothing renders. => the runtime present of a
  windowed flip-model swap chain is not composed by DWM on this stack (the H4c finding that made gdi_present exist);
  0094 removed the only visible path for them. Next: flip-model buffers get BOTH the GDI draw (visible) and the
  runtime present (DXGI buffer accounting, no hang). GDI readback ~63 ms per 1704x959 frame = interim.
- 14:2x TEMP pixel log in gdi_present: mpv's presented frames are opaque black (0xff000000 at center/quarter/3-quarter, frames 0-3), GDI draw succeeds -> mpv RENDERS black on our UMD (D10 T4), presentation is not the (only) problem. Bisect mpv render options next.
- 14:3x mpvbisect.ps1: fbo rgba8 / profile=fast / dither off / all of them -> still black. --background-color=#ff0000 --video-zoom=-2 -> center, quarter, 3/4 ALL red: the background clear reaches the frame, the video draw writes nothing (full-size video = all black).
- 14:4x tools d3dvbring (new, mpv's vertex ring as repro): discard/no-overwrite at offsets 0/96/192/4096, shared RT, scissor enabled - ALL PASS on the Mali (= WARP). mpv dumb mode (--gpu-dumb-mode=yes, one direct pass) + red background + zoom -2: center still red -> the single video draw writes nothing. Next: replay mpv's own HLSL from its log.
- 14:5x d3dvbring variants all PASS on the Mali (= WARP): shared RT, scissor enabled, z = w = 1 (mpv's VS writes
  gl_Position = float4(pos, 1, 1)). mpv's dumb-mode shaders extracted from its log (vo/gpu/d3d11 HLSL source):
  VS = 3 TEXCOORD float2 inputs, y flip; PS = Sample t1 (.x luma) + t2 (.xy chroma), cbuffer b0 (row_major float3x3
  colour matrix + offsets, packoffsets c0..c13), dither texture t0 sampled at FragCoord/64, floor() quantisation.
  Next: replay that PS (its textures/formats + cbuffer layout) in a repro.
- 15:0x tools d3dmpvps (new): mpv's dumb-mode VS/PS verbatim + mpv's inputs (R8 / R8G8 / R16G16B16A16_FLOAT, identity
  matrix cbuffer, cull NONE + scissor): WARP center 0xff808080 (written), Mali 0xff0000ff = clear left = NOTHING
  WRITTEN -> mpv's black window reproduced outside mpv. (First repro version without CullMode NONE failed on WARP
  too: mpv's VS flips y = reversed winding.) Suspect: culling despite CULL_NONE.
- 15:1x d3dmpvps bisect on the Mali: greenps (mpv VS + constant PS) FAIL, noflip FAIL, noflip+greenps FAIL ->
  the vertex stage. posfirst (same VS, SV_Position moved to the FRONT of the output struct = output register o0)
  + greenps -> 0xff00ff00 written; posfirst + mpv's real PS -> 0xff808080 = WARP's value. ROOT CAUSE: our UMD
  mishandles a VS whose SV_Position is not output register o0 (mpv's SPIRV-Cross HLSL declares it last: o2).
- 15:2x CORRECTION from D3DDisassemble of mpv's VS (fxc vs_4_1, PC): the two float2 varyings are PACKED into one
  register: `dcl_output o0.xy` (TEXCOORD1) + `dcl_output o0.zw` (TEXCOORD2), `dcl_output_siv o1.xyzw, position`.
  posfirst: `dcl_output_siv o0 position`, `dcl_output o1.xy` + `o1.zw` (packing in both). So the failing shape is
  "a (packed) generic output register BEFORE the position register", not "position at o2". ShaderTGSI.c DCL_OUTPUT /
  DCL_OUTPUT_SIV / dcl_base_output read correct (GENERIC index = register, ureg dedups the second o0 decl, writemask
  per component). TGSI dump (ST_DEBUG=tgsi) is compiled out in release (Debug.h st_debug 0). Next: Panfrost's
  runtime shader dump (BIFROST_MESA_DEBUG=shaders) on d3dmpvps default vs posfirst.
- 15:3x REAL ROOT CAUSE (BIFROST_MESA_DEBUG=shaders on d3dmpvps over ssh, dumps in scratchpad): mpv's VS NIR has
  `out_1 (VARYING_SLOT_POS, driver_location 1)`, `out_0 (VAR0, 0)` - tgsi_to_nir keeps TGSI declaration order. The
  IDVS position shader then stores POS with `LEA_ATTR_IMM attribute_index:1` + ST_CVT (a plain varying write);
  posfirst stores it via the Bifrost position fast path `ST_CVT ..., r58, r59, #0x105e000` (POS_RESULT_PTR).
  bifrost_compile.c bi_emit_lea_attr: fast path only when `nir_src_is_zero(intr->src[0])` = varying slot 0.
  The GLSL path never hits this (st sorts outputs by location, POS = 0). Not a d3d10umd translation bug.
  Fix (Mesa 0095, pan_shader.c): TGSI VS -> nir_assign_io_var_locations(nir, nir_var_shader_out) before nir_lower_io
  (ttn emits store_deref, so lower_io picks the new driver_location). XFB unaffected: TGSI SO never reaches
  pan_nir_lower_xfb (ttn does not set has_transform_feedback_varyings). FS links by location (posfirst + real PS gave
  WARP's value with differently ordered generics), so the FS needs nothing.
- 15:4x Mesa 0095 built (maliumd 2eaf5a39, still includes the uncommitted flip-model DxgiFns + TEMP pixel log),
  umdswap OK. d3dmpvps: default 0xff808080 written (= WARP), greenps 0xff00ff00, posfirst 0xff808080, noflip
  0xff808080; d3dvbring PASS. mpv via mpvbisect (4 s test.mp4, frame 3 presented pixels): hwdec=exynosmfc-copy
  center 0xffa6afb6 quarter 0xff020202 3/4 0xff060405; hwdec=no same -> mpv's frames now carry the video (were
  0xff000000 at all three points).
- 15:5x REVIEW of fffe833 / Mesa 2e961fd (0095): (1) claim "GLSL path sorts by location": st_program.c:397
  nir_recompute_io_bases assigns bases in location order -> true (comment names no wrong function). (2) ttn writes
  outputs with store_deref (tgsi_to_nir.c:2404/2508), so nir_lower_io picks up the re-assigned driver_location -
  confirmed by the run (default variant now written). (3) FS linkage by location: posfirst + real PS matched WARP
  before the fix with reordered generics -> no FS change needed. (4) XFB: ttn never sets
  has_transform_feedback_varyings, pan_nir_lower_xfb unreachable for TGSI -> unaffected (TGSI SO was already not
  wired on panfrost: separate gap, not touched). (5) Regression sweep on the phone: d3dtri PASS, d3dmsaa PASS,
  d3dvbring PASS, d3dfill all phases presented (user session), d3dswap 64.8/s; d3dsm41 lod 1/8 differ = the known
  Bifrost lod gap (notes/gpu-umd-d3d11.md:276, 0080c), not new. No follow-up found.
- 15:6x On-screen check attempts (user away, asked for a screenshot): GDI CopyFromScreen in the user session = all
  white (8614 B, same as the 10-16 photos-2 shot -> GDI readback is blind on the GPU-present desktop); scan-out dump
  (wddm2dbg fb, base reg 0x19071040 -> 0xca000000) all black while frames flow (DSIM counter moving) -> that window
  is not the one being flipped on the H4b path; PC webcam: Access denied 0x80070005 = PC privacy "let desktop apps
  access camera" = Deny (user setting, left alone). New: tools/mfc/mpvshot.ps1 (mpv + GDI shot in ONE userrun -
  a second userrun/runuser2 while mpv plays collides on the task), tools/mfc/umdlogtail.ps1.
- 15:7x FINDING from malikm-mpv.exe-5684 (mpvshot -After 25 run, 1704x959 window): TEMP frames 1-3 carry the video
  (center 0xffa6afb6) but frames 100 and 200 are 0xff000000 at all three points -> mpv's frames go BLACK after the
  first few presents. 0095 fixed the first frames only; something later (MFC frames after the start? the swap chain
  buffer rotation? mpv's interpolation/redraw path?) renders black. Next: TEMP frame-100 raw dump (UMD b4c6a989).
- 15:8x CORRECTION of 15:7x: the TEMP frame-100 raw dump (frame-5596-1704x959.bgra -> traces/g17/mpv-frame100.png,
  not committed: traces/ is ignored) is a CORRECT video frame (night scene: the ramp + car; centre / quarter / 3/4
  sample points fall on black background, hence 0xff000000). mpv's frames carry the video throughout - nothing goes
  black. Image sent to the user. Panel itself still not seen (no capture path: GDI white, scan-out window black,
  webcam blocked by PC privacy). TEMP frame dump removed from the UMD again (not committed, never in Mesa history).
- 15:9x USER (MALIKM_NO_GDI_PRESENT=1 for mpv only, diagnosis): "works but still kinda bad" -> DWM DOES compose the
  runtime present of mpv's windowed flip-model chain. The 14:1x conclusion ("not composed, H4c") was drawn while mpv
  rendered black (the 0095 bug) and is WRONG. Native fix = committed 0094 as is (flip-model buffers skip gdi_present,
  runtime present only): the uncommitted "GDI draw + runtime present" edit and the TEMP pixel log reverted, never
  committed. UMD f212a640 (= Mesa 2e961fd, 0095) deployed. "No sound" = mpvshot/mpvtest pass --no-audio (test only).
  USER earlier with the GDI draw: "working but very choppy" (GDI readback 63-78 ms/frame -> ~13 fps).
- 16:0x Smoothness measurements, native path (UMD f212a640, test.mp4 1080p23.976, exynosmfc-copy, gpu/d3d11):
  tools/mfc/mpvstats.lua (new, --script): every second decdrop=0 delayed=0 vf-fps=23.98, time-pos tracks real time
  -> mpv delivers every frame on time. kmdstats 10 s during playback: presents 37.6/s all GPU-path (idle desktop 0),
  renders = submits = jobs 1017/s, irqs 1016/s. cputop 8 s: dwm 45.9 %, mpv 27.3 %, explorer 19.9 %, MsMpEng 16 %
  of one core (6 cores) - not CPU-bound; mpv threads 16.9 / 11.3 %, none pegged. => DWM composes at ~38 Hz, not 60:
  24 fps video on an uneven ~38 Hz composition = judder. Suspect: ~1000 tiny GPU jobs/s (per-submission latency /
  serialisation), who issues them is next.
- 16:1x Clock test (S10eGovernor stopped, s10epowerctl set g3d 702000, gpuclock 702.0 MHz before / during / after):
  mpv playback kmdstats 10 s presents 37.9/s, jobs 1011/s - SAME as at ~410 MHz (37.6 / 1017) -> not GPU-clock bound.
  Governor restarted (sc start), back in control. Governor policy note: with no touch for 5 s it steps the GPU to
  half ceiling - video playback has no touch (separate follow-up, not the judder cause per this test).
- 16:2x Interrupts: maligpu.c MkIsr is the only Gpu->Irqs++ (the 1 ms PollDpc path -> Reap -> MkJobTakeStatus reads
  JOB_INT_RAWSTAT, never MkIsr), so kmdstats irqs 1016/s = real ISRs ~ jobs: completion is interrupt-driven, the
  "1 ms poll caps jobs at ~1000/s" idea is wrong. Per-process submits (UMD tlog "submit N"): mpv ~460/s (~19 per
  video frame), DWM ~560-1450/s. DWM present path (d3d10_gdi.c d3d10_scanout_present): full-screen blit into the
  scan-out BO + fence_finish (CPU wait) + present, every composition.
- 16:3x tools/mfc/mpvrate.sh (new; first version hung in its wait loop: empty -Opts made mpvshot fail -> fixed, 60 s
  start deadline; the stopped bash loop survived TaskStop and kept launching mpv - killed). Clean runs after that:
  default config / --profile=fast / --gpu-dumb-mode=yes / dumb + --window-scale=0.5 -> presents 0.0 in ALL (jobs 485
  / 293 / 293 / 293, GPU 400 / 260 MHz): DWM composed nothing in these runs although it composed 37.6/s earlier with
  the same command. Display timeout is 0 (never), governor ScreenOffs 0, DSIM line counter moving. Unknown: what the
  screen shows (mpv window covered / screen turned off with the power button?) - asking the user.
- 16:4x USER: "still choppy" (looked at Windows, not mpv, during the 0-present runs). DWM's own UMD log: no present
  from 11:38 to ~12:01 phone time (the 4 zero runs), resumed when the user picked the phone up -> DWM idle while mpv
  played, cause unknown (separate oddity, not chased).
- 16:5x DWM pacing instrumentation (d3d10_gdi.c d3d10_scanout_present, QPC; tlog "pacing N" per 300 presents: blit+wait
  and runtime-present mean/max, gaps between presents in vsync periods). UMD fafaa8c5, dwm restarted (taskkill, auto
  relaunch). mpv default config: blit+wait mean 3.4-3.7 ms (max ~10.5), present call 0.8 ms (max 7.3);
  gaps 1v 120-137 / 2v 160-161 / 3v 2-6 of 300 -> half of DWM's frames slip a vsync; the time is lost BEFORE the
  present (composition waiting on the GPU), not in the present path.
- 16:6x mpv option bisect (DIAGNOSIS ONLY, rule 12), DWM presents/s and gaps 1v/2v of 300:
  default 36.7 (120/160) | --profile=fast 56.2 (285/15) | --gpu-dumb-mode=yes 54.5 (275/22) |
  --fbo-format=rgba8 35.0 (105/184) | --linear-downscaling=no 35.6 | --correct-downscaling=no 37.2 | --dither-depth=no
  34.9 | --dscale=bilinear 35.9 (131/159) | --cscale=bilinear 42.3 (193/105) | --scale=bilinear 43.9 (191/105).
  Jobs/s went UP with profile=fast (1390) -> GPU time of mpv's default lanczos-family scaler passes (many texture
  taps/pixel), not job count. Same frame rate at GPU 702 MHz (16:1x) -> suspect memory bandwidth: MIF clock is
  untouched under Windows, rate UNKNOWN (clocks-dvfs.md:61); Samsung's G3D table pairs 702 MHz with MIF >= 2093000
  (gpu-odin-rom.md F2). Next task: read the MIF rate (verified constants only), then raise it if low.
- 16:7x REVIEW 0096: first window = 299 gaps for 300 presents (no gap before the first) - matches the logged sums; limitation: the rotated branch (landscape) returns before the pacing block -> landscape compositions are not measured (documented, no code change).
- 16:8x ZERO-PRESENT runs explained: the blank screensaver (scrnsave.scr, ScreenSaveTimeOut 120, active) was running
  - DWM composes nothing under it (memory note cube-fps-screensaver). mpvrate.sh now dismisses it before each run.
- 16:9x S10ePower MIF control (driver version 2, build in scratchpad, C:\S10eTmp\S10ePower.sys; old kept as
  S10ePower.sys.pre-mif / s10epowerctl.exe.pre-mif): domain 0 (dvfs_mif, cal_acpm_dvfs_exynos9820.h via
  gpu-m16-g3don.md:37), only the verified devfreq_mif ends 2093000 / 421000 (m15-perf.md:98), no thermal cap; state
  struct appends MifCurKhz/MifReqKhz, v1 callers get the v1 prefix. Device: `s10epowerctl set mif 2093000` -> ACPM
  reply echoes 2093000, 0 fails. memcpy (s10eperf) 9142 before / 8426 after (the earlier 4116 was another clock state)
  - CPU memcpy does not show MIF.
- 17:0x Diagnosis (UMD eb29deff, TEMP MALIKM_NO_LINEAR_CONVERT in pan_resource.c, mpv only): tiled instead of linear
  video planes -> 35.0/s gaps 111/187 = no change -> texture layout ruled out (TEMP switch to be removed).
- 17:1x kmdstats busy (new mode: samples MW_ESCAPE_STATS ~500/s, share with Running != 0, queue depth): idle 0 %;
  mpv default 67.2 % (queued <= 2); --profile=fast 47.8 % -> the GPU is not saturated; DWM's composition waits behind
  mpv's long scaler jobs in the single in-order queue.
- 17:2x CLOCKS ARE THE CAUSE (governor stopped for the run, restarted after): G3D 702 MHz + MIF 2093000 -> DWM 53.0
  presents/s, busy 54.4 %, gaps 1v 251-274 / 2v 26-47 of 300 (vs 103-120 / 160-187 at the governor's ~410 MHz).
  G3D 702 alone (MIF as left by sboot, 16:1x) 37.9/s; MIF 2093000 alone (G3D ~410) 34.8/s -> both needed, as in
  Samsung's G3D table (702/650 MHz <-> MIF 2093000, gpu-odin-rom.md F2). The governor drops G3D to half ceiling after
  5 s without touch (video playback has no touch) and never sets MIF.
  Next (native fix): S10eGovernor - G3D from GPU load (not only touch), MIF 2093000 while the screen is on and the
  GPU is above its floor, 421000 when the screen is off / GPU at floor.
- 17:3x REVIEW c14bf78 (S10ePower v2): rates between the two verified ends round DOWN to 421000 (documented in ctl help); the driver never lowers MIF after a set - stays at 2093000 until the governor manages it (battery) -> governor task. REVIEW e59037a (kmdstats busy): header said ~1000x/s, measured ~500/s (2487 samples / 5 s) -> comment fixed in its own commit.
- 17:4x S10eGovernor fix (native, no switches; s10egovernor.exe deployed, old kept as s10egovernor.exe.pre-mif):
  (1) GpuBusy = SUM of the per-process "GPU Engine(*engtype_3D)" instances, capped at 100 % (was the busiest single
  instance = mpv's share only); (2) MIF 2093000 with the screen on, 421000 off, 2093000 on exit (SetMif, verified rates
  only; per-G3D-step MIF needs the full devfreq_mif ladder - read it from TWRP sysfs next time in TWRP);
  (3) GpuMaxBusyPct (registry, default 50): summed 3D load >= 50 % -> G3D ceiling.
  Measured, mpv default config: MIF 421000 + G3D 702 (my baseline mistake, no governor) 9.5 presents/s (gaps 1v 115,
  "more" 64) -> MIF is decisive. Governor (1)+(2): G3D 572, 43.6/s, 1v 200-201 / 2v 99. + (3): G3D 702, 47.5/s,
  1v 237-239 / 2v 57-63 of 300; after playback G3D back to 260, MIF stays 2093000 (screen on); G3D 52 C.
  Deploy gotcha: `cd /d C:\S10eTmp && ...` over ssh failed ("cannot find the path") - use full paths.
- 17:5x REVIEW cb8e9e0: SetMif marked the rate as set on ANY failed IOCTL (meant for an old driver) -> one transient ACPM failure would have left MIF unset all session. Fix: MIF support probed once (GET_STATE size >= v2 struct, stat MifControl), failed requests retried at most every 5 s. Device: MIF forced to 421000, governor restarted -> MifControl 1, CurMif 2093000, driver MIF cur 2093000. Other review points (no change): any app >= 50 % GPU load runs at 702 MHz (thermal guard 83/88 C caps it); MIF at 2093000 whenever the screen is on (Linux does the same here) - battery cost, refine with the full MIF ladder.
- 17:6x TEMP MALIKM_NO_LINEAR_CONVERT reverted (never committed); clean UMD 71ef0e38 (= Mesa 1337e37, 0096) deployed (umdswap; DWM keeps its loaded copy until it restarts).
- 17:7x USER: "exactly the same as before, not smooth at all" + NEW: quick-settings icons (Wi-Fi, battery saver,
  hotspot) missing or tiny. DWM pacing during the user's own BBB playback: mostly 3v gaps (1v 0-54 / 2v 78-158 /
  3v 95-216 of 300) - much worse than my no-audio runs. One run overlapped (user's mpv + mine) - void.
  bbb1080.mov = h264 1920x1080 24 fps + aac 48 kHz (C:\Users\S10e\Videos). My BBB run with audio (volume 5, launched
  via userrun = not foreground): presents 10.1/s, GPU busy 26.6 %, mpvstats decdrop 374 by t=28 s (mpv drops frames
  to keep A/V sync), delayed 0. mpv log: one wasapi under-run at start, then "Audio/Video desynchronisation detected"
  at 2.95 s. coretop (new tools/phone/coretop.ps1) BBB no audio: presents 18.8, CPU 13 %, MID 507 MHz; with audio:
  presents 3.5, cpu4 51 %, G3D 260 (governor sees no GPU load). tools/mfc/decbench.ps1 (new): exynosmfc-copy decodes
  BBB at 72.5 fps untimed (3x real time) -> decoder throughput is not the limit. => mpv mostly WAITS (low CPU, low
  GPU, few presents). Next: app-side present pacing log (UMD 49d9cb7a, TEMP until reviewed) during the user's own
  foreground playback.
- 17:8x ROOT CAUSE CANDIDATE (strong): GPU memory. App present pacing (UMD 49d9cb7a, user's own foreground BBB run):
  mpv's present does NOT block (flush+wait mean 0.2 ms, present call 0.15-0.27 ms) but mpv's presents are irregular
  (per 120: 18-28 gaps of 100-250 ms, 6-9 > 250 ms); DWM blit+wait max 997 ms / 497 ms. No TDR/GPU events
  (tools/phone/gpuevents.ps1). kmdstats (now prints failures / allocations / maps / paging ops) during BBB: presents
  1.6-1.7/s, failed 0, timeouts 0, resets 0, NEW allocations 15 but MAP_APERTURE_SEGMENT 611 / UNMAP 606 in 15 s
  (idle: 0) = VidMm residency THRASH. tools/phone/gpumem.ps1 (GPU Adapter/Process Memory counters): adapter shared
  usage 891 MB idle / 910 MB playing, committed 1350 / 1490 MB; DWM (pid 2056, running since its 12:03 restart)
  857-890 MB; mpv 147 MB. Segment = MW_SEGMENT_SIZE 1 GB (MaliG76Wddm.h:32), CommitLimit 1 GB. DWM + mpv > 1 GB ->
  every context switch evicts the other's set (malikm lists EVERY live BO per submit: DWM ~180, mpv ~55).
  DWM's ~880 MB is abnormal -> leak/accumulation in DWM's UMD (Panfrost BO cache? redirection surfaces? scan-out
  imports?). Possibly also the missing quick-settings icons (allocation failures). Next: DWM growth over time.
- 17:9x DWM GPU memory flat at idle (852 MB committed / 886 MB shared over 60 s, gpumem.ps1 -Name dwm) - not a
  running leak. DWM's tlog: allocations listed per submit ~103 before the mpv runs, 160-203 during/after them, and it
  stays there. Mechanism: malikm_drm.c submit lists EVERY live BO (H2), including BOs parked in Panfrost's BO cache;
  the shim ignored MADVISE (retained = 1 only). Fix (Mesa 0097, UMD fecdbce3): MADVISE DONTNEED -> wbo.idle (not
  listed), WILLNEED / use in a batch -> listed again; pages never discarded (retained 1). Fake (non-WDDM) path
  untouched. Deployed + phone reboot (fresh DWM, also clears the shell icon state after my dwm taskkill).
- 18:0x VERIFIED after reboot (UMD fecdbce3 = Mesa 0097 + 0098 log): DWM GPU memory 190 MB at boot+61 s, 225 MB
  playing (was ~880). First run at boot+61 s: presents 2.5/s (post-boot startup; mpv's tlog showed no present -
  not repeated). Settled run, BBB 1080p24 with audio (volume 5), mpvstats + log: presents 24.8/s (= the video's 24
  fps; DWM composes per changed frame), maps 5 / unmaps 5 in 12 s (= new allocations, thrash gone), GPU busy 47.2 %,
  G3D 377 MHz (governor), MIF 2093000, decdrop 0 over 28.5 s, delayed 0; app pacing 480/600: present gaps 120/120
  in 25-50 ms (wait mean 146-184 us, present 114-137 us). => the choppy playback was VidMm residency thrash caused
  by listing Panfrost's cached BOs. Mesa 0097 (b1dd130, madvise) and 0098 (08dba8e, app pacing log) committed.
- 18:1x REVIEW Mesa 0097: make_evictable's only caller is panfrost_bo_cache_put (pan_bo.c:275) after the last reference; fetch calls make_unevictable (WILLNEED) before reuse (pan_bo.c:220) -> no batch reaches an idle BO unlisted; in-flight work listed it in its own submission. Cached BOs still hold system memory (only out of the residency set) - Panfrost's 1 s stale eviction runs on cache_put. No follow-up. Review 0098: TEMP-free, log every 120 presents only.
- 18:2x 16:34 local: phone restarted - tools/phone/lastboot.ps1 (new): User32 1074 at 13:33:34 phone time, RuntimeBroker on behalf of user S10e (= Start menu restart by the user), no 6008/41/1001 -> clean, not a crash.
- 18:3x USER after reboot + own restarts: "nothing changed" (still choppy). The user's own runs (mpv pids 920, 7940,
  7932; umdlogtail -ProcId): app pacing 111-118 of 120 presents in 25-50 ms, 2-4 per 120 at 100-250 ms -> mpv
  delivers almost every frame on time in the user's playback too. My BBB run at that point: mpv 120/120 on time, maps
  110 = allocations 109 (no thrash) BUT KMD presents 11.1/s -> DWM put only ~11 of 24 frames/s on screen.
  DWM has written NO tlog since the 13:10 reboot (pids 1328/1316; fopen in C:\S10eTmp\tlog, ACL grants Authenticated
  Users modify; other processes log) - the "DWM pacing" lines I quoted after the reboot were stale (pid 2056) - my
  "24.8/s fixed" verdict rested on KMD presents only. DWM loads maliumd + D3D10Warp; GPU Engine counters
  (tools/phone/gpuengine.ps1, new): DWM 18-21 % 3D on the Mali adapter, ~0 on the Basic Render one -> DWM composes on
  the Mali. GPU memory now DWM 400 MB, Explorer 221 MB, pid 6208 127 MB (adapter shared 778 MB) - grows again after
  boot. vblank (user session) 59.0 Hz p50 16.81 ms (vsync fine). DWM_TIMING_INFO refreshes ~0.2/s = same as before
  this stack (line 264), not meaningful. "Idle" now: DWM presents 13.8/s, 267 jobs/s, GPU busy 5 %, G3D 260 MHz, DWM
  CPU 24 %; Edge open + a "Setup" process at 44 % CPU (user installing a game) -> measurements contaminated, paused.
  Open: why DWM composes only ~11-14/s now (CPU/latency-bound, GPU nearly idle) vs 47-56/s before the reboot.
- 18:4x Boot-time DWM writes no tlog (pids 1328/1316 after the 13:10/13:34/13:40 boots) although it loads the same
  System32 maliumd (tools/phone/umdwhich.ps1: dwm/explorer/msedge all fecdbce3) - a taskkill-restarted DWM does log
  (pid 3924) -> the boot DWM probably lacks the machine env (MALIKM_TGSI_DIR) at its start [INFER]. Mesa shim: Render
  timing in the "submit N" line (UMD 5fd5c82a, TEMP until reviewed). FRESH DWM (3924, just restarted) + BBB with
  audio: KMD presents 25.0/s, maps 4 = allocations 4, DWM Render mean 77-150 us max 288-903 us per submit with
  133-143 BOs listed, DWM GPU memory 286 MB. => fresh DWM composes every video frame; the user's ~11/s state builds
  up over time (DWM memory 190 -> 400+ MB, live BOs growing). Next: measure the same test periodically to catch the
  degradation and what grows with it.
- 18:5x USER: "maybe I'm seeing the same frames multiple times" + fresh DWM (25 presents/s) still "same, not good".
  NEW tools flipseq (drivers/MaliG76Wddm/tools/flipseq: maximized window, flip-discard 3 buffers, 42 ms frames,
  frame number painted as B8G8R8A8 G/B) + TEMP DWM hook "seqpix" (d3d10_gdi.c d3d10_scanout_present: centre pixel of
  every composed frame while C:\S10eTmp\seqpix.flg exists; UMD e7bde40b) + tools/mfc/seqpix.py decoder.
  RESULT (DWM pid 8204): 243 frames carry a number; shown sequence 2 1 2 1 3 5 4 6 8 7 9 11 10 12 14 13 15 ...
  -> in every group of 3 the 2nd and 3rd frames are SWAPPED: DWM composes buffer X's content when buffer Y is
  presented (2 of the 3 swap-chain buffers mixed up in DWM's view). 80 steps back over 240 frames = every third frame
  displayed out of order -> the visible stutter. Timing metrics could not show it. Next: DWM-side OpenResource /
  shared-allocation mapping (three same-size buffers).
- 18:6x CAUSE of the out-of-order frames: d3d10umd _RotateResourceIdentities (upstream) COPIES contents around the
  ring (res[i] <- res[i+1], last <- old 0) while every Resource keeps its own kernel allocation (kmt_present) -> right
  after Present it overwrites the buffer just handed to DWM (DWM composes at the next vsync) with an older frame;
  also 2 full-frame GPU copies per present. MS Learn (DXGI_DDI_BASE_FUNCTIONS, pfnRotateResourceIdentities,
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_functions):
  "must exchange identities ... X, Y, and Z ... must rotate those handles to refer to Y, Z, and X ... the user-mode
  display driver should swap the kernel handles of the corresponding resources. However, the driver should not swap
  the corresponding runtime (RT) handles." / "views can refer to resources ... the driver must re-create such views.
  Also, the driver might be required to reapply currently bound views." -> fix: rotate the identities (pipe storage +
  kernel allocation fields), re-create views of the rotated resources, rebind bound ones.
- 18:7x Mesa 0099 attempt (identity rotation + view registry, UMD 6ac70770) deployed + `taskkill dwm` at ~14:06 phone
  time -> DWM did not come back for 7 min (screen black). tools/phone/dwmevents.ps1 (new): taskkills at 13:57 and
  14:00 were followed by "Dwminit: process has exited ... Restart count 1" + 9027 in the same second; the 14:06 kill
  logged its exit only at 14:13:41, right after I restored e7bde40b (umdswap from System32\*.old) -> the killed DWM
  (pid 8204, running the OLD e7bde40b with the seqpix hook) hung in teardown; the new UMD was never loaded = 0099
  untested, not proven bad. Recovered by `shutdown /r` (DWM up after 92 s). Separate issue: process teardown hangs
  (also seen with mpv earlier: unkillable, WaitReason Executive). Next: deploy 0099 + reboot (no taskkill), flipseq.
- 18:8x FIXED (frame order): Mesa 0099 (db5fb49) deployed + reboot (DWM loads it at boot, no taskkill): DWM pid 1344
  logs "RotateResourceIdentities: 3 resources, identities rotated (no copy)"; flipseq 240 frames -> seqpix sequence
  0 1 2 3 ... 174 ... in order: steps back 0, skipped 0, repeats 3 (DWM recomposing while waiting). Was 3 5 4 6 8 7 ..
  (80 steps back / 240). Mesa 0100 (b6aae71) = Render timing log. seqpix TEMP not committed (d3d10_gdi.c reverted;
  the deployed 6ac70770 still has it, flag off = one GetFileAttributes per 60 presents).
  REVIEW 0099: rotates the whole driver Resource (storage, kmt_*, transfers, so_target); RTV/SRV registry repoints
  views + bound fb + bound sampler views. Gaps (documented, not fixed): UAVs on swap-chain buffers and bound
  stream-output targets are not remapped (unusual for back buffers; UAVs barely exist at FL 10_1). Create error
  paths leave rot_dev NULL -> destroy safe; n > 16 refused (DXGI max BufferCount 16).
- 18:9x USER: "Yeah that fixed it" (mpv video smooth after Mesa 0099). VRAM still growing (open): 15 min after the
  14:16 boot DWM 1344 total committed 492 MB, Explorer 204 MB, adapter 915 MB committed; DWM submits list 290-314
  live BOs (Render mean 254-331 us, max ~1.2 ms) vs ~140 at a fresh start.

## G-game: 3D game on CPU (2026-10-17, user: "barely runs, 100% CPU, no GPU")
- Desktop shortcut -> C:\Users\S10e\Downloads\Game.Dev.Story.v2.54\...\KairoGames.exe (tools/phone/lnk.ps1, new):
  Unity 2019.4.33f1 IL2CPP, 32-bit x86 (UnityCrashHandler32.exe) -> runs under x86 emulation on ARM64.
- Unity Player.log (AppData\LocalLow\Kairosoft\...): "Direct3D 11.0 [level 11.1], Renderer: Microsoft Basic Render
  Driver" = WARP on the CPU. Cause: a 32-bit process cannot load our ARM64 maliumd.dll; the INF names only
  UserModeDriverName (MaliG76Wddm.inf:60), no 32-bit UMD -> device creation on the Mali adapter impossible -> WARP.
- MS Learn (WDDM 64-Bit Issues, https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-windows-
  vista-display-driver-64-bit-issues): "a 32-bit user-mode display driver must be provided in addition to the 64-bit
  user-mode display driver"; INF "HKR,, UserModeDriverNameWow, %REG_MULTI_SZ%, Xxx.dll", DLL copied to
  %systemroot%\SysWOW64; private data to the KMD must not use pointer-size types and must keep 64-bit alignment.
- Plan: (1) x86 (i686) build of the Mesa UMD (llvm-mingw i686 target); (2) ABI audit of MW_* private structs and
  escapes for 32/64 layout; (3) test install (SysWOW64 + UserModeDriverNameWow) with a 32-bit D3D11 test app,
  then the game; (4) INF + package (native). x64-emulated apps (ARM64EC/ARM64X UMD) = separate later task.
- Step 1 done: build-mesa-g76.ps1 stage x86 (clang-cl --target=i686-pc-windows-msvc, vcvarsamd64_x86, cross file with
  needs_exe_wrapper = false, -Dprecomp-compiler=enabled so the x86 panfrost_compile writes the shader blobs with x86
  struct layout) built first try: s10e-artifacts\mali-g12-x86\maliumd32.dll 3af65bb5 (machine 14C x86), imports
  VCRUNTIME140 + api-ms-win-crt-*; phone has SysWOW64\vcruntime140.dll (2020 build - may need vc_redist.x86).
- Step 2 ABI audit: include/maliwddm_abi.h has only mw_u32/mw_u64 fields, no pointers/SIZE_T/HANDLE; MSVC-ABI x86
  aligns 64-bit members to 8 like x64 -> MW_ALLOCATION_INFO, MW_COMMAND, MW_ESCAPE_* identical in the 32-bit UMD
  and the 64-bit KMD. (Pointer-free by design; CPU views from Lock are process-local.)
- Step 3 (test install): maliumd32.dll -> SysWOW64, HKLM\...\Class\{4d36e968-...}\0000 UserModeDriverNameWow =
  maliumd32.dll x4 (reg add; INF not changed yet). Before reboot: game still on Basic Render (no tlog for it) ->
  dxgkrnl reads the value at adapter start. After reboot: Unity Player.log "Direct3D 11.0 [level 10.1], Renderer: Arm
  Mali-G76 MP12" + malikm-KairoGames.exe-1888 tlog = the x86 UMD works (ABI ok, vcruntime140 of 2020 sufficient).
- Game then dies: stderr "DRM_IOCTL_PANFROST_CREATE_BO failed (err=12)"; tlog "Allocate hr 0x8876017c"
  (D3DERR_OUTOFVIDEOMEMORY) after ~7100 allocations (MakeResident counter 0x1be1). The one aperture segment is 1 GB
  (MW_SEGMENT_SIZE; MaliG76Wddm.h:31: must stay below 4 GB - 32 MB, MK_VA_START..MK_VA_END is ONE 4 GB GPU address
  space for all processes) and DWM + Explorer alone held ~915 MB committed before the game started.
  => the game is blocked by the VRAM problem: (a) DWM's growth (500-900 MB), (b) the 1 GB segment (could be ~3.9 GB).
- VRAM: DWM BO breakdown (shim log, UMD bc9c9b6f): fresh DWM own 66 (89 MB) idle 9 (11 MB) opened 3 (14 MB); dxgkrnl
  DWM committed grows 248 -> 267 -> 258 -> 310 MB over 4 mpv runs while the shim's view stays ~150 MB -> the growth
  is outside the shim's table. Cause 1: closed BOs wait in wdead until 64 entries / an idle wait (up to 63 dead
  allocations kept). Mesa 0101 (prompt destroy when the last submission completed + 32 MB cap; UMD f036180b,
  x86 9485c689): "dead 0" after every run. Cause 2 (open): per mpv session DWM OpenResources 3 x 1704x959 + 1 x
  1024x832 but only 2 come back through DestroyResource ("gone") -> +19 MB per app session (192 -> 211 -> 230 ->
  248 -> 267 MB over 4 runs after 0101). The runtime never destroys the third buffer (deferred? still bound?).
- Game after 0101 (DWM ~270 MB): still "CB FAIL Allocate hr 0x8876017c" + "panfrost_batch_init failed" - the game
  alone needs more than the remaining part of the 1 GB segment -> raise MW_SEGMENT_SIZE (KMD; < 4 GB - 32 MB).
- REVIEW 0101: prompt destroy only when no submission is outstanding (last_event signalled or none yet), poll mode (last_unsignalled) keeps the old thresholds; all under wddm_lock (no half-made submission). No follow-up.
- KMD 1.0.63.1 (s10e-artifacts\mali-kmd-63, sys 4c1a9072): MW_SEGMENT_SIZE 1 GiB -> 2.5 GiB (about half of the
  5.24 GiB RAM, the usual iGPU share; a limit, VidMm commits on use; < 4 GB - 32 MB VA limit). Deploy: TWRP +
  tools/phone/kmd-deploy.sh (old sys kept in C:\S10eTmp\kmd-prev).
- 32-bit game: create-bo FAIL even for 4 KB noexec BOs (TEMP log in the x86 UMD cb433cc9); KMD last alloc va 0x0f3a0000 (not VA-exhausted on the GPU side); system commit 2.9 of ~6.9 GB (tools/phone/memcommit.ps1). Game VirtualMemorySize 3085 -> 4031 MB within seconds while its working set stays ~250 MB = the 32-bit process's 4 GB CPU address space runs full -> allocations (CPU-visible, mapped into the process) fail with 0x8876017c. Next: which regions (tools vaprobe).
- vaprobe (new, drivers/MaliG76Wddm/tools/vaprobe: VirtualQueryEx walk, totals by type + largest AllocationBase
  groups) on the game at ~4 GB VM: private reserved-only 3522 MB, ~110 regions of exactly 32 MB with ~0 committed
  (+ the 2 TB mapped region = WoW64/emulator reservation above 4 GB, irrelevant). mpv (ARM64, same UMD): ~47 x 32 MB
  private reserved-only regions + the 4 GB CFG bitmap -> the 32 MB reservations come with OUR stack, roughly one per
  GPU allocation (mpv ~55 BOs, game ~90-110). No MEM_RESERVE in the shim/Mesa (only the non-WDDM fake path). The
  shim locks every BO at creation (pfnLockCb, kept mapped). Hypothesis: each lock of an aperture allocation costs a
  32 MB CPU VA reservation in the process (done by the runtime/VidMm). Rule 11: not investigated inside Windows;
  behavioural test next (lock vs no lock, count the 32 MB regions).
- vatest (new, drivers/MaliG76Wddm/tools/vatest, ARM64, behavioural - rule 11): empty 32 MB private reservations in
  its own address space: before device 0, device 5, +40 DEFAULT no data 46, +40 DEFAULT with data 88, +40 DYNAMIC
  mapped 129, after releasing all 120 textures still 129 -> ONE 32 MB reservation per GPU buffer, never returned
  while the buffer lives - and released textures' BOs live on in Panfrost's BO cache; the shim keeps every BO locked
  (pfnLockCb at MAP, "the Lock stays until GEM_CLOSE"). Next test: shim UNMAP really unlocks + Panfrost unmaps BOs it
  parks in its cache (re-mmap at fetch) -> does the count drop?
- Test: shim UNMAP really unlocks (UMD c2d3b2a8, x86 f12bbdbe; Panfrost munmaps a BO before caching it) -> vatest
  unchanged (46/88/129, 129 after release) -> the reservation is NOT the Lock. vatest size sweep: 20 x 16x16 textures
  -> +21 empty 32 MB reservations (committed +0.2 MB); 20 x 2048x2048 (16 MB) -> +1 empty reservation, committed +325 MB
  -> every GPU allocation takes CPU address space in 32 MB steps for its lifetime (small ones ~empty 32 MB regions,
  large ones mostly committed). 64-bit processes do not notice; a 32-bit one with a few hundred small BOs (Panfrost
  pools, staging, cached BOs) fills 4 GB. Fix direction: SUB-ALLOCATION in the shim (pack small BOs into >= 32 MB
  allocations) - also cuts allocation-list length per submission (DWM). The unlock-on-unmap change is NOT the fix
  (kept out of commits unless shown useful).
- Phone back on committed code: UMD 7b20fbf6 (ARM64, = Mesa 91b4559/0101) via umdswap, x86 8cd52259 in SysWOW64; DWM still runs the boot-time f036180b (same source minus a comment). KMD 1.0.63.1 (2.5 GiB segment) deployed from TWRP.
- RESEARCH (user: "research online about this"): MS Learn DXGK_ALLOCATIONINFOFLAGS_WDDM2_0
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0):
  "CpuVisibleOnDemand - When this member is set, the video memory manager will try to allocate the virtual address
  for the allocation when it is mapped/locked. This will result in the allocation not having a permanent virtual
  address, which reduces the amount of virtual address space used. It behaves similarly to the CpuVisible flag in
  that it's an allocation directly accessible by the CPU, but it just gets the virtual address for it 'on demand'
  instead of being permanently assigned." -> our KMD sets CpuVisible on every allocation (memory.c NewAllocation) =
  a permanent CPU VA per allocation = the 32 MB reservations (behaviourally: vatest). Documented fix: CpuVisibleOnDemand
  (WDDM 2.x FlagsWddm2). Other findings: freedreno (Mesa) has a sub-allocation heap for small BOs (MR 20263; source
  src/freedreno/drm/freedreno_bo_heap.c: util_vma_heap over equal-size blocks, no shared BOs, needs userspace fencing)
  = the fallback design; MS docs do not name the 32 MB figure itself.
- KMD 1.0.64.1 (s10e-artifacts\mali-kmd-64, sys f9d98bf4): Parameters\CpuVisibleOnDemand (default 1 = CpuVisible + CpuVisibleOnDemand (d3dkmddi.h 0x00040000), 2 = on-demand only, 0 = off), WDDM 2 mode only (phone Wddm2 = 5). Test: vatest 32 MB reservation counts.
- CpuVisibleOnDemand results: mode 1 (CpuVisible + OnDemand) -> vatest unchanged (129); mode 1 + shim
  unlock-on-unmap (UMD 6ad89c26) -> unchanged; mode 2 (OnDemand only, CpuVisible cleared) -> 32 MB reservations GONE
  (1 / 1 / 1 / 1 / 1 instead of 5 / 46 / 88 / 129), DYNAMIC Map works -> dxgkrnl honours the flag for our WDDM 2 KMD,
  and CpuVisible itself forces the permanent CPU VA. BUT mode 2 desktop: DWM up (40 MB), LogonUI 305 MB, explorer
  never started (session stuck at logon / LogonUI) -> some allocations need CpuVisible (primaries?). Next: mode 3 =
  OnDemand only, except MW_ALLOC_PRIMARY keeps CpuVisible.
- KMD 1.0.65.1 (mali-kmd-65, sys aadeabdd) mode 3 (on-demand only except MW_ALLOC_PRIMARY) + registry
  CpuVisibleOnDemand = 3: Windows STUCK at the boot spinner (user, 18:4x); no ping/ssh/adb. Rollback: user forces
  TWRP, kmd-deploy.sh mali-kmd-63 (1.0.63.1, no on-demand flag, last good boot). Registry value 3 stays (1.0.63.1
  ignores it).
- Recovery: forced reset left NTFS dirty -> TWRP mounts it read-only (kmd-deploy refused, correctly - no forced rw /
  fsck). Instead: ESP (FAT, by-name/cache) S10EDIAG\nogpu.flg from TWRP -> Windows booted on Basic Display (68 s),
  registry CpuVisibleOnDemand = 0, nogpu.flg removed from S:, clean restart -> desktop on the Mali (84 s) with KMD
  1.0.65.1 in mode 0 (= 1.0.63.1 behaviour). Samsung debug_history: only normal boot / recovery entries (no crash).
  Source default of the parameter set to 0 (a bad default must not be able to hang a boot).
- Plan: on-demand only for allocations the shim creates for Panfrost BOs, selected by a private MW_ALLOCATION_INFO
  flag from the UMD - primaries, CDD/shadow/shared and other runtime allocations keep CpuVisible.
- KMD 1.0.66.1 (mali-kmd-66, sys 7647eda9) mode 4 = on-demand only for MW_ALLOC_CPU_ONDEMAND allocations (ABI flag 0x4, both headers); shim CREATE_BO sets it (+ unlock-on-unmap, UMD cd78f3da / x86 bda7eb10). Registry CpuVisibleOnDemand = 4. Deploying (UMDs live, KMD via TWRP).
- Mode 4 + shim marking ALL Panfrost BOs (UMD cd78f3da): vatest 32 MB reservations 1 (fixed), game VM ~1 GB (fixed),
  BUT the display: frozen logon screen, then "a frame every few seconds" (user) - DWM (64-bit) with on-demand BOs
  composes terribly. Mode 0 restored by reg + reboot.
- FINAL shape (UMD 8982a14d ARM64 / 168ca705 x86, KMD 1.0.66.1 mode 4 via registry): the shim marks
  MW_ALLOC_CPU_ONDEMAND and unlocks on unmap ONLY in the 32-bit build (#if !defined(_WIN64)); 64-bit processes
  (DWM, mpv) unchanged. Result: desktop normal after reboot (81 s); game: VM flat at 639 MB (was -> 4031 MB and dying),
  no CREATE_BO failures, Unity "Direct3D 11.0 [level 10.1], Renderer: Arm Mali-G76 MP12"; KairoGames 95 % of one core
  (x86 emulation), KMD presents 5.5/s, GPU engine ~0 for the game at that moment (loading?). User check pending.
