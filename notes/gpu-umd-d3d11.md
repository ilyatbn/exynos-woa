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
- 22:45 ROOT CAUSE of the missing tracked fence found with ETW (tools/phone/wddm2/etwswap.ps1, DXGI provider,
  decoded with tracerpt): on WARP DXGI logs BindBackBuffersToDwm + DWMRedirection NewRedirected=true for the
  composition swap chain; on the Mali neither - because d3d10umd's CreateDevice returns DXGI_STATUS_NO_REDIRECTION
  (Device.cpp, inherited from the VMware driver). Mesa 0072 adds D3D10UMD_REDIRECT=1 (S_OK = DXGI may redirect).
  With it: d3dswap composition runs to "done", Windows Terminal stays alive (45 s) - the tracked-fence crash is gone.
  New problem with redirection: DWM loses devices while composing redirected buffers (devlost.ps1: 0 drops/boot
  without redirection, 3-7 per DWM instance with it) and fail-fasts (dwmcore CD3DDevice::TranslateDXGIorD3DError
  InContext, 0x8898008D) -> DWM restart loop when set machine-wide. DxgKrnl ETW (etwswap -Redirect -Kernel):
  VidSchMarkDeviceAsError x3 - DWM: JournalEntry "Render" 0xC0000022 (STATUS_ACCESS_DENIED from the render
  callback), "Removing device" 0x887A002B, reason 6 from user mode; two more devices reason 14 from kernel.
  INFERENCE: a DWM submission names an allocation handle not owned by the submitting device - the malikm shim lists
  every BO it tracks, and redirected buffers are opened by DWM on several runtime devices (bindings). Next: per-device
  ownership of allocation-list entries in the shim (only handles opened/created on that device). The redirect switch
  stays off machine-wide (registry value removed) until then; GpuMmu (Wddm2 = 5) and DirectFlip are not needed for
  the fence (cause was the UMD) - keep Wddm2 = 5 for now (works), revisit.
- 23:00 Method change (user: debug differently, keep traces in a gitignored folder -> traces/): no more looking
  inside Windows binaries. Instrument our own boundary instead: every runtime (KT) callback the malikm shim makes
  goes through a logging wrapper (HRESULT + allocation handles + device/binding), with a ring of the last calls
  dumped on the first failure of each kind. Goal: name the call that gets STATUS_ACCESS_DENIED in DWM with redirected
  swap chains, and the handle in it. DXGI/D3D11/DxgKrnl ETW stays as the outside view.
- 23:05 Mesa 0072/0073 (s10e-artifacts\mali-umd-0073, maliumd db4a5e4a): D3D10UMD_REDIRECT, SRV + CheckFormatSupport +
  SetError logging, all runtime callbacks through logging wrappers (proof-of-life lines show them live in DWM).
  Behaviour (tools/phone/wddm2/dwmwatch.ps1, devlost.ps1): with a redirected composition swap chain live, DWM keeps
  running for 90 s / 280 frames (presents S_OK) but recreates its D3D devices constantly (152 drops in that run, 22
  in a 60-frame run) and eventually fail-fasts; app exit itself is survived. NONE of the shim's runtime calls fails
  in DWM during this (no CB FAIL, no SetError) - the device loss is not caused by a call we make. Without
  redirection: 0 drops. Traces kept in traces/wddm2-2026-10-16 (gitignored).
  Status of the S4 goal: redirection = tracked fence works (Terminal/composition swap chains run), but DWM is not
  stable with redirected surfaces yet -> redirect stays off by default.
- 20:10 (2026-10-16) G16 first look, tools/phone/wddm2/browsertest.ps1 (Edge, chrome_debug.log): every GPU process
  dies in init - "eglCreateContext ES 3.0 failed with error EGL_BAD_ATTRIBUTE. ES version fallback is disabled" ->
  CollectGraphicsInfo failed -> "Exiting GPU process". Chromium now requires GLES 3.0 from ANGLE; ANGLE D3D11 gives
  ES 2.0 on FL 10_0 (renderer11_utils GetMaximumClientVersion; research note item 1). After 3 tries Edge falls back
  to a software GPU process (--use-angle= empty). So FL 10_0 = no browser GPU at all; need FL 10_1 (or ANGLE
  feature allowES3OnFL10_0 as a test). Log: traces/wddm2-2026-10-16/edge1.txt.
- 20:15 Edge with --enable-angle-features=allowES3OnFL10_0 (browsertest.ps1 -Flags): ANGLE now creates the ES3
  context on our UMD (GPU process gets past CollectGraphicsInfo, presents/raster start), then dies 3x with
  FATAL skia_output_surface_impl.cc:1285 (surface 2280x1026 format=4 color_type=4 backend_format.isValid()=0
  backend()=5 AsGLFormat=0 willGlFBO0=1), preceded by "CopySharedImage: Dest shared image is not writable" /
  "Unable to initialize SkSurface"; exit 0x80000003 -> after 3 crashes Edge runs with a software GPU process.
  UMD log of the GPU process (browsertest.ps1 -UmdLog: MALIKM_TGSI_DIR + --disable-gpu-sandbox): CheckFormatSupport
  answers 0x7 (SAMPLE|RT|BLEND) for colour formats, 0x1 for depth, 0 for BC1-BC5 (70-84) and no MSAA bits at all
  (CheckMultisampleQualityLevels = 0 for every count, inherited "XXX: Disable MSAA"). No CB FAIL. --vmodule gives no
  extra lines in Edge release. Logs: traces/wddm2-2026-10-16/edge{2,3,4}-debug.log, malikm-msedge.exe-1580.txt.
  Research on the CHECK running (agent). Candidate causes: missing BC/MSAA required by FL 10_0 make ANGLE's ES3 caps
  incomplete; the real fix is FL 10_1 (ANGLE ES3 without the hack) - both need MSAA in the UMD.

## FL 10_1 plan (G16 prerequisite; 2026-10-16 20:25)
Chromium needs GLES 3.0 from ANGLE = D3D FL 10_1 (renderer11_utils GetMaximumClientVersion). What 10_1 adds over
10_0 (MS Learn "Direct3D feature levels" table + D3D10.1 DDI) and where d3d10umd stands (source read today):
- F1 MSAA: State.h SUPPORT_MSAA 0, CheckMultisampleQualityLevels returns 0 always, ResourceResolveSubResource is
  LOG_UNSUPPORTED. 10_1 mandates 4x on RT formats. -> quality levels from screen->is_format_supported(samples),
  MULTISAMPLE_* format bits, resolve with pipe->blit. Resource creation already passes SampleDesc.Count.
- F2 SM 4.1 opcodes: ShaderParse/ShaderTGSI mark LOD, GATHER4, SAMPLE_POS, SAMPLE_INFO unsupported. TGSI has
  LODQ, TG4, TXQS; SAMPLE_POS needs the standard D3D sample pattern table (to be sourced, not guessed).
- F3 TextureCubeArray SRVs/declarations.
- F4 independent per-RT blend (check the D3D11 blend-state path).
- F5 report D3D11DDI_3DPIPELINELEVEL_10_1 (D3D11.cpp:234) once F1-F4 hold; test with d3dswap + Edge without the
  allowES3OnFL10_0 flag.
- 20:40 User rule (AGENTS.md 12): native only. The Edge allowES3OnFL10_0 run and D3D10UMD_REDIRECT=1 are diagnostic
  only; G16 = real FL 10_1 in the UMD, G15 = redirection by default with a stable DWM.
- 20:45 F1 baseline, new tool drivers/MaliG76Wddm/tools/d3dmsaa (mali-umd-0073): device FL 0xa000; RGBA8 support
  0x111ef3f3, quality levels 0 at 2/4/8/16x, CreateTexture2D 4x = E_INVALIDARG (the runtime rejects counts with 0
  quality levels). WARP control: FL 0xb000, 1 level at 2-16x, clear + ResolveSubresource PASS (pixel 0xff4080ff).
  F1 build: mali-umd-0074 (maliumd 4e1bd2ab): SUPPORT_MSAA 1, quality levels from is_format_supported(samples),
  ResourceResolveSubResource via pipe->blit.
- 20:50 Research (agent, Chromium 140.0.7339.0 / Skia / ANGLE sources online) on the Edge FATAL: it is the
  LOG_IF(FATAL, !characterization.isValid()) in SkiaOutputSurfaceImpl::CreateGrSurfaceCharacterizationCurrentFrame;
  color_type 4 = kRGBA_8888, backend 5 = kUnsupported (default-constructed GrBackendFormat). Chain: ANGLE
  renderer11_utils GenerateTextureFormatCaps fills sampleCounts (incl. 1) only if CheckFormatSupport has
  D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET (optional for R8G8B8A8 at FL10_0, dxgi_support_table) -> our driver
  reports none -> GL_NUM_SAMPLE_COUNTS(RGBA8) = 0 -> Skia GrGLCaps setupSampleCounts leaves the table empty ->
  RGBA8 not renderable -> no default backend format. So F1 (MSAA) is the fix for this crash too; ANGLE comments:
  conformant ES3 only on FL 10.1+.
- 20:52 mali-umd-0074 on the phone: d3dmsaa D3D11CreateDevice = 0x887a0020 (DXGI_ERROR_DRIVER_INTERNAL_ERROR) at
  any FL - the MSAA change breaks device creation. Investigating with the UMD log.
- 21:00 0074b (quality-level logging): the runtime asks CheckMultisampleQualityLevels for every format in DXGI order
  at x1..x31 during CreateDevice. Format 1 (R32G32B32A32_TYPELESS) -> 0 everywhere (FormatTranslate NONE), format 2
  (R32G32B32A32_FLOAT) caps 0x1f, x2 -> 0, x3 -> 0, x4 -> 1 -> runtime drops the device at once (0x887a0020).
  Hypothesis: quality levels must agree inside a cast family (typeless parent 0, typed member 1 = inconsistent).
  Test: answer typeless formats with a typed member of the family.
- 21:15 0074c (typeless->typed in the quality query) got further: dropped after format 20. 0074d: one helper
  MsaaFamilyFormat (Device.cpp) answers every cast-family member (typeless, typed, D*, depth-as-colour views) for
  one representative; CheckFormatSupport MS bits from the same 4x answer. Dropped after "CheckFormatSupport 21 ->
  0x19" (R32_FLOAT_X8X24_TYPELESS with MULTISAMPLE_RENDERTARGET but no RENDERTARGET) before any quality query of 21
  -> runtime validates caps per format: MSRT only on RT formats or the D* formats themselves. 0074e = that.
- 21:20 0074e: dropped at "CheckFormatSupport 21 -> 0x11" (MULTISAMPLE_LOAD on R32_FLOAT_X8X24_TYPELESS). 0074f: no LOAD on the four depth-as-colour views at level 10_0.
- 21:25 0074f: 21 caps 0x1 then 21 x4 -> 1 -> dropped (19 typeless 0x11 + levels was accepted). Rule: levels > 0 need an MS bit on that format. 0074g: depth-as-colour views answer 0 levels.
- 21:30 0074g: got to format 89 (R10G10B10_XR_BIAS_A2_UNORM, caps 0) and dropped (4x level 1). 0074h: general rule - levels > 0 only if the format's own caps carry an MS bit (CheckFormatSupportCaps split out of the logging wrapper).
- 21:35 0074h: back to the first drop (format 1 typeless 0 levels vs format 2 one). Typeless family roots must answer like their members even without own caps; other formats need own MS caps. 0074i.
- 21:40 0074i (maliumd 1f85c7af) PASSES F1: d3dmsaa on the Mali: CreateDevice FL 0xa000, RGBA8 support 0x117ef3f3,
  quality 2x=0 4x=1 8x=1 16x=1 (RGBA8/BGRA8/D24S8), 4x RT clear + ResolveSubresource + readback 0 of 4096 wrong.
  Runtime rules found from the logs (CreateDevice checks every format x1..x31): levels must agree inside a cast family
  (typeless root included, even with caps 0); a non-root format with levels > 0 must report an MS bit itself; MSRT
  only on RT or D* formats; no MULTISAMPLE_LOAD on depth-as-colour views at level 10_0. Regression: d3dswap hwnd 60
  frames done, DWM unchanged. Mesa commit + tools/mesa-build/patches/0074-msaa.patch.
  Diagnostic only (rule 12): Edge + allowES3OnFL10_0 - the Skia FATAL is gone (research confirmed); the GPU process
  now runs ~13 s, then exits 0xC0000005 (access violation) once, the restarted one stays up. Next: that crash.
- 21:45 Review of 862a94b/0074: (1) MsaaFamilyFormat answers the R32 and R16 families from D32_FLOAT / D16_UNORM
  depth support - a colour R32_FLOAT/R16_* MSAA RT is then created on the strength of the depth answer; follow-up:
  d3dmsaa over all RT formats x 4/8/16 (create + clear + resolve) to prove each claimed level. (2) 2x = 0
  everywhere (panfrost has no 2x) - fine for D3D, 4x is the required count. (3) LOAD is advertised but SM ld2dms
  (LD_MS) translation path is untested - follow-up with F2 shader tests.
- 21:50 The Edge GPU-process crash (crashpad dump, traces/.../edgegpu.dmp, cdb stack only): d3d11 WaitTrackedFenceImpl
  <- dxgi CDXGISwapChain::AcquireBuffer <- OMSetRenderTargets <- msedge rx::StateManager11 - the same tracked-fence
  crash as Windows Terminal (composition swap chain, no DXGI redirection). => browsers are blocked on G15 too:
  redirection must be on by default and DWM stable with it. Back to G15.
- 22:00 G15 fresh DWM log with 0074i + a redirected d3dswap (dwmwatch -Redirect, traces/.../dwm-redir-1332.txt):
  DWM binding 0 opens the app's 400x300 buffers (OpenResource 0xc0001e40/0x80002a80 -> BOs 117/118), within 16 ms
  the runtime frees them and every other DWM resource ("gone"), binding 1 is created, binding 0 destroyed = the device
  loss, right after the first opened redirected buffers appear. No CB FAIL: the runtime's RenderCb returns >= 0 to
  us and handles the error itself (INFERENCE: device marked lost inside the runtime, ETW "Render 0xC0000022").
  Shim behaviour (malikm_drm.c SUBMIT, H2): every submission lists the batch BOs AND every other live BO of the
  binding - including opened redirected buffers that, in the flip model, belong to the producing app between
  presents. HYPOTHESIS: DxgKrnl refuses (ACCESS_DENIED) a consumer submission that references a buffer it does not
  currently own. 0075: opened allocations (WBO_OPENED) are listed only when the batch uses them.
- 22:10 0075 result (reboot, dwmwatch -Redirect, traces/.../dwm-redir-0075.txt): better but not fixed - the app
  reached frame 46 (was 5) before DWM died, but the pattern stays: every new DWM device opens the two 400x300 app
  buffers and is dropped ~300 ms later. In the batch path opened BOs are still listed with WriteOperation = 1 (all
  batch BOs are, Panfrost has no per-BO write info). 0075b: opened BOs listed as reads.
- 22:20 0075b (maliumd d7e7d42f) FIXES the DWM device loss: reboot, dwmwatch -Redirect 60 frames: DWM opens the
  app's two buffers, 0 device drops (devlost: CreateDevice 1 dropped 0), app runs to "done", DWM pid unchanged.
  0075 alone: 23 drops. ROOT CAUSE: the shim listed DWM's opened (other-process) buffers as WRITES in its
  submissions - a flip-model consumer may only read the producer's buffers; DxgKrnl refused the Render
  (STATUS_ACCESS_DENIED) and the runtime removed the device. (Write-listing came from H2's "batch BOs = written".)
- 22:25 400-frame redirected composition run: done in ~10 s, DWM pid unchanged, 0 drops. Mesa commit + patches/0075-opened-bo-read.patch. Next (0076): redirection on by default (rule 12: no env switch), D3D10UMD_NO_REDIRECT=1 only for diagnosis.
- 22:35 Rule 13 docs check (retroactive) for the F1 MSAA rules. MS Learn PFND3D10DDI_CHECKMULTISAMPLEQUALITYLEVELS
  (learn.microsoft.com/.../nc-d3d10umddi-pfnd3d10ddi_checkmultisamplequalitylevels): "If the driver receives 1 in
  SampleCount, the driver always returns 1"; "0 or greater than 32 ... always returns 0"; unsupported count -> 0.
  PFND3D10DDI_CHECKFORMATSUPPORT: BLENDABLE "only if the format can be a render target"; MULTISAMPLE_RENDERTARGET =
  "render target with a sample count that is greater than one"; NOT_SUPPORTED only for the listed formats (incl.
  R10G10B10_XR_BIAS_A2_UNORM) "only this bit should be set". Neither page states the cast-family / caps-vs-levels
  consistency that CreateDevice enforces - those stay as measured (0074 logs). Our code matches the documented
  rules (x1 -> 1, >32 -> 0). Follow-up: R10G10B10_XR_BIAS_A2_UNORM returns 0 caps; the doc says NOT_SUPPORTED only
  if unsupported - it is "creation only" in the inherited comment, leave.
- 22:40 0076 (maliumd 107bdb1e, redirection default) NATIVE: reboot, d3dswap composition 300 frames done, Windows Terminal alive 40 s and rendering (scan-out traces/wddm2/wt-0076.png: PowerShell prompt drawn), DWM 0 drops. G15 S4 reached. dwmwatch/etwswap switch flipped to -NoRedirect.
