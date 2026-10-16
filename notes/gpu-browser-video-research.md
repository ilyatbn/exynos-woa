# G16/G17: what Edge/Chrome need for GPU acceleration and hardware video decode (research, 2026-10-16)

Agent research (web + Chromium/ANGLE sources), sources as cited; INFERENCE marked. Not yet device-checked.

## Progress log
- 2026-10-16 20:50 research report in (below). Next: chrome://gpu / edge://gpu on the WDDM 2.0 adapter once DWM runs.

## Findings
1. ANGLE D3D11 needs FL >= 10_0 (Renderer11.cpp ~1032 "Inputted D3D11 device must be Feature Level 10_0 or
   greater"). GetMaximumClientVersion (renderer11_utils.cpp ~1023-1041): FL 10_1+ -> GLES 3.0; FL 10_0 -> GLES 2.0
   unless caps.allowES3OnFL10_0 (d3d_features.json "allow_ES3_on_FL10_0", added for Firefox). INFERENCE: Chromium
   leaves it off -> on our FL 10_0: GPU compositing/raster + WebGL1 yes, WebGL2 context creation fails.
   DirectComposition (ui/gl/direct_composition_support.cc ~964-1050): D3D11 device + dcomp.dll + DCompositionCreate-
   Device3, no FL check; overlays via IDXGIOutput3::CheckOverlaySupport. ANGLE needs DXGI 1.2 (IDXGIDevice2) to
   present into another process's HWND (Renderer11.cpp ~945-967). INFERENCE: our UMD's DXGI present DDI (flip model /
   pfnPresent1) is the likely failure point; test with --disable-direct-composition.
   Graphite blocklisted below FL 12.0 (software_rendering_list id 188) -> Ganesh on GL/ANGLE. SwiftShader WebGL
   fallback removed in desktop M137 (blink-dev intent) -> failed WebGL = no WebGL.
2. Chrome reads vendor/device from DXGI_ADAPTER_DESC (gpu_info_collector_win.cc ~351-359), driver version from
   IDXGIDevice CheckInterfaceSupport (UMD version). Only blanket Windows block: software_rendering_list id 74,
   vendor 0x1414 (Microsoft) "all" features. Nothing matches vendor 0 / ARM / non-PCI IDs. Action: never report
   0x1414. (gpu_driver_bug_list.json not searched exhaustively - workarounds, not blocks.)
3. Chrome HW video decode = D3D11VideoDecoder only (media/gpu/windows/d3d11_video_decoder_backend.cc): FL >= 11_0
   (~38), ID3D11VideoDevice (~145), ID3D11VideoContext1 (~227), ID3D11Device5 (~64/~206); DXVA profile GUIDs
   (d3d_h264_accelerator.cc ...). The old MFT-based DXVA decoder is gone from main. MF renderer for clear content is
   test-only (kMediaFoundationClearPlayback disabled by default), encrypted MF only on x86-64 -> ARM64 Chrome never
   uses a third-party MFT. Learn D3D11_1DDI_VIDEODEVICEFUNCS: needs the D3D 11.1 UMD DDI (retrieved via
   PFND3D10DDI_RETRIEVESUBOBJECT) -> a D3D10-only UMD cannot expose ID3D11VideoDevice.
4. A hardware MFT (async model, MFT_ENUM_HARDWARE_URL_Attribute, dynamic format change, NV12 out; merit needs a
   Microsoft certificate) serves only MF-pipeline apps (Media Player / Movies & TV), not the browsers.
5. Cheapest paths: (a) browsers: check chrome://gpu on FL 10_0 now (ES2: compositing + WebGL1); D3D10.1 DDI ->
   FL 10_1 -> WebGL2; D3D11.x DDI + FL 11_0 serves browsers and is the prerequisite for video. (b) browser video on
   the MFC: only D3D11_1DDI_VIDEODEVICEFUNCS in the UMD (H.264 VLD, NV12 decoder output views) over the MFC IOCTLs,
   + FL 11_0, ID3D11VideoContext1, ID3D11Device5 (INFERENCE: needs D3D11.3/11.4 DDI + WDDM 2.x monitored fences).
   The same DDI lights up Microsoft's MF H.264 decoder for MF apps. VP9/AV1 stay software unless the MFC does VP9.

## Consequence for the plan (INFERENCE)
G15 (WDDM 2.0) is a prerequisite for ID3D11Device5 fences; G16 first step is measuring chrome://gpu on FL 10_0;
G17 needs the UMD to grow from the D3D10.0 DDI to D3D11.1+ (video DDI) at FL 11_0 - the panfrost G76 supports
GLES 3.2-class features, so FL 11_0 is a UMD/frontend question, not a hardware one (to verify).

## G17 research 2026-10-17 (agent; Chromium main, MS Learn; scratch copies scratchpad/cr/)
- Chromium Windows hardware decode = only media/gpu/windows/d3d_video_decoder.cc (gpu_mojo_media_client_win.cc
  CreatePlatformVideoDecoder). Hardcoded `IsFeatureLevelSupported: device->GetFeatureLevel() >=
  D3D_FEATURE_LEVEL_11_0` in GetSupportedVideoDecoderConfigs and AcquireDeviceResources (kUnsupportedFeatureLevel).
  No switch bypasses it; D3D12 decoder backend is FEATURE_DISABLED_BY_DEFAULT and needs a FL 11_0 D3D12 device.
  Needs ID3D11VideoDevice1 + ID3D11VideoContext1 (CHECK), D3D11_DECODER_PROFILE_H264_VLD_NOFGT, ConfigBitstreamRaw
  2 (DXVA_Slice_H264_Short), NV12 BIND_DECODER|SHADER_RESOURCE shared NT handle textures, zero-copy sampling (no
  video processor). Sends PicParams + Qmatrix + slices with start codes, no SPS/PPS.
- MediaFoundationRenderer (IMFMediaEngine) accepts FL 10_1 but clear playback is kMediaFoundationClearPlayback,
  DISABLED_BY_DEFAULT "for testing purposes" -> a process flag = not a solution (AGENTS rule 12).
- MS Learn: D3D11 video DDI mandatory for WDDM 1.2+ drivers on D3D10/10.1/11 hardware - FL is not the runtime gate;
  table via RetrieveSubObject ID 2 (D3D11_1DDI_VIDEODEVICEFUNCS) / 3 (D3DWDDM2_0DDI_VIDEODEVICEFUNCS).
  msmpeg2vdec uses D3D11 DXVA when offered (no FL minimum stated) -> MF apps (Media Player, Photos) would get MFC
  decode natively from a UMD DXVA bridge.
- Stateless DXVA -> stateful MFC bridge: synthesize SPS/PPS from DXVA_PicParams_H264 + Qmatrix + slice-header pps_id
  (no open-source precedent found; GStreamer h264 bit writer / FFmpeg cbs_h264 as writers); POC type 1 offsets
  missing (fail those streams); MFC display delay 0, copy each frame into the app's output view at EndFrame.
- CONSEQUENCE: native browser hardware video needs D3D FL 11_0 in our UMD (SM5, compute, 8 UAVs, hull/domain
  shaders - the G76 has no tessellation hardware, panfrost exposes none -> emulation). Native MF-app video needs the
  UMD DXVA bridge (works at FL 10_1). Decision for the user (asked 2026-10-17).
