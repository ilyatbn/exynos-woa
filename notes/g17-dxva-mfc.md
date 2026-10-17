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
