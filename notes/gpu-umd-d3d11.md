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
- 21:40 D1 done = Mesa 0070 (s10e-artifacts\mali-umd-0070, maliumd ff3c3486): D3D11.cpp/D3D11.h + generated
  d3d11_stubs.inc/d3d11_fill.inc (tools/mesa-build/gen-d3d11-ddi.py: 150 members = 93 reused, 10 wrapped, 47 stubs;
  D3D10PSGP-only members skipped; 5 Device.cpp statics assigned there). Device (new process d3dfence): GetCaps 130
  (3D pipeline), 128 (threading), 129 (shader) answered, CreateDevice interface 0x000b000b (D3D11_0) version
  0x00020009, FL 10_0, fences/shared fences still ok. Terminal still dies at d3d11+0x46f4 -> D3D11_0 alone does not
  give the tracked fence; next D2/D3 (D3D11_1 + WDDM1_3/WDDM2_0 tables). DWM on 0070 = next boot.
- 22:10 D2/D3 built (Mesa 0071 work in progress): D3D11_1, WDDM1_3, WDDM2_0 tables + DXGI 1.2-1.4. WDDM2_0 device
  creation needed GPUVA_CAPS != 0 (and MEMORY_ARCHITECTURE UMA) - zero = D3D11CreateDevice 0x887a0020. All four
  levels create devices (D3D10UMD_MAX_DDI bisect), fences work at each. Terminal on WDDM2_0 (interface 0x000b0020):
  still the NULL tracked fence. Parser fix: min-precision extended operand tokens on dst/relative operands
  (ShaderParse.c, seen with WDDM1_3: assert at :430).
- New repro drivers/MaliG76Wddm/tools/d3dswap (composition swap chain + DComp, user session): dies on the 3rd
  frame exactly like Terminal; both presents return DXGI_STATUS_OCCLUDED (0x087a0001). Live user-mode debugging
  with the ARM64 cdb on the phone (tools/phone/wddm2/dbgrun.cmd + dbg-*.txt): BindToDwm is called during swap chain
  creation and returns S_OK, but no tracked fence gets created; the app-level ID3D11Device5::CreateFence path works.
  Working hypothesis: DWM does not take the swap chain (OCCLUDED), so DXGI never sets up the DWM-bound fence -
  next: our side of that (DWM's UMD log, KMD OpenAllocation/handle failures while d3dswap runs).
- 22:30 Composition vs HWND (tools/phone/wddm2/swapmatrix.ps1, d3dswap FRAMES comp|hwnd seq|disc BUFFERS [warp]),
  DWM on the WDDM2_0 UMD (after reboot; presents now S_OK, before: DXGI_STATUS_OCCLUDED with DWM on D3D11_0):
  comp seq 2 / comp seq 3 / comp disc 2 = die after BufferCount presents (NULL tracked fence); hwnd seq 2,
  hwnd disc 2, hwnd disc 3 = all frames done. Control: comp seq 2 on WARP (Basic Render) on the phone = done.
  => only composition swap chains on OUR adapter miss the tracked fence (Terminal SwapChainPanel, DirectComposition
  presentation incl. browsers). d3dfence on both adapters: monitored fence GPU VA 0 on both (not the difference);
  KMTQAITYPE_WDDM_2_0_CAPS Mali 0x1 (Support64BitAtomics) vs Basic Render 0x5 (Support64BitAtomics |
  IoMmuSupported); DRIVERVERSION 0x7d0 vs 0xc1c.
  INFERENCE (evidence: the only working adapter is VA-capable; Learn: physical mode is "legacy"): the composition
  path wants a GPU-VA-capable adapter. Next = KMD GpuMmu in the mixed form of qemus/virtio-drivers-windows (a GpuMmu
  copy/paging node, the 3D node stays physical: UMD submission unchanged) - notes/gpu-wddm2-design.md S7a.
