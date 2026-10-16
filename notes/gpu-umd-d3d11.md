# UMD: Mesa d3d10umd -> D3D11 DDI (design + progress, 2026-10-16)

Why (all four user goals hang on it):
- Windows Terminal / XAML SwapChainPanel / DirectComposition presents die on a NULL DXGI tracked fence even on the
  WDDM 2.0 adapter, while app-level ID3D11Fence and D3DKMT monitored fences work (notes/gpu-wddm2-design.md 21:08,
  tools d3dfence). INFERENCE: d3d11.dll runs our D3D10.0-DDI UMD on its downlevel path, which has no tracked fence.
  Browsers composite through DirectComposition (notes/gpu-browser-video-research.md) -> the same path.
- Browser HW video needs FL 11_0 + the D3D11.1 video DDI (D3D11_1DDI_VIDEODEVICEFUNCS); FL is a UMD question.

## Facts (WDK 10.0.26100 um/d3d10umddi.h)
- Device tables: D3D10_1DDI_DEVICEFUNCS 105 entries (what d3d10umd fills today, Device.cpp), D3D11DDI_DEVICEFUNCS
  152, D3D11_1DDI_DEVICEFUNCS 157, D3DWDDM1_3DDI_DEVICEFUNCS 166, D3DWDDM2_0DDI_DEVICEFUNCS 177.
- 10.1 -> WDDM2.0 diff (script in this note's progress log): 27 entries change argument type (constant-buffer
  setters -> D3D11_1 with offsets, SetRenderTargets with UAVs, CopyRegion/UpdateSubresourceUP with copy flags, Flush
  with flags, CreateResource/views/shaders with D3D11 arg structs incl. signatures, blend/rasterizer states,
  CheckMultisampleQualityLevels with flags), ~70 new (indirect draws, HS/DS/CS stages, UAVs, deferred contexts and
  command lists, Discard, ClearView, tiled resources, markers, WDDM2.0 view/query creators, FlushWDDM2_0,
  GetResourceLayout, SetHardwareProtection, CheckDirectFlipSupport, AssignDebugBinary, DynamicConstantBuffer...).
- d3d10umd today: OpenAdapter10/10_2 accept D3D10_0* only; GetSupportedVersions lists D3D10_0*; GetCaps zeroes;
  State.h SUPPORT_D3D10_1 0 / SUPPORT_D3D11 0 guard nothing but the version lists.
- Prior art (MIT, Turing Software LLC): wddm-ref/virtio-d3d11/thirdparty/triton/src/virtio/neptune/triton -
  tritonDDI.c per-tier DEVICEFUNCS tables + OpenAdapter10_2 + GetSupportedVersions (D3D11_0, D3D11_1, WDDM1_3,
  WDDM2_0..2_3) + GetCaps. It translates the DDI to a D3D11 API device (DXVK), so its bodies do not apply; its tables,
  version/caps handling and comments are the reference for semantics.

## Plan (one commit each, every step device-tested: d3dtri cube + DWM desktop + d3dfence)
- D1 D3D11_0 interface at pipeline level 10_0: GetSupportedVersions/OpenAdapter accept D3D11_0(_7); GetCaps answers
  THREADING (no deferred contexts, no concurrent creates), SHADER (no doubles), 3DPIPELINESUPPORT (10_0),
  D3D11_OPTIONS* zero; CreateDevice fills D3D11DDI_DEVICEFUNCS: unchanged entries = the 10.1 functions, changed
  entries = wrappers converting to the existing implementation, new entries = stubs (SetErrorCb E_NOTIMPL where a
  call is illegal at FL 10_0). Observation: runtime picks D3D11_0 (log), FL still 10_0, desktop + cube unchanged.
- D2 D3D11_1 + DXGI 1.2 DDI (pfnPresent1 etc. in DxgiFns.cpp). Observation as D1.
- D3 WDDM1_3 + WDDM2_0 DDI interfaces (new view/query creators, FlushWDDM2_0, GetResourceLayout). Observation:
  Windows Terminal survives (tracked fence non-NULL) - the S4 goal.
- D4 FL 10_1 (WebGL2 in ANGLE), then FL 11_0: compute shaders + UAVs over gallium (panfrost has CS/images/SSBOs);
  tessellation stages are not in panfrost -> decision needed (refuse HS/DS creation while reporting 11_0, or stay
  at 10_1). Observation: chrome://gpu.
- D5 D3D11_1DDI_VIDEODEVICEFUNCS over the MFC IOCTLs (H.264 VLD, NV12) - G17.

## Progress log
- 2026-10-16 21:15 design written; table diff done with awk over d3d10umddi.h (10.1 lines 1855-1980 vs WDDM2.0
  5457-5666).
