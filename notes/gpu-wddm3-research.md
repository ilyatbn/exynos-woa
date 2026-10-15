# GPU: WDDM 2.0 → 3.1 research (target: highest interface the phone's build accepts)

Started 2026-10-15 after G15 S1 was blocked (notes/gpu-wddm2-design.md runs 1-6: any 2.x interface without real
GPU VA is failed by dxgkrnl right after DRIVERCAPS). User direction: "go for the top if we can".
Phone OS: Windows 11 build 22621/22631 (22H2/23H2) -> WDDM 3.1 max (UNVERIFIED mapping, to confirm below).
Rules: documented sources only (Learn, WDK headers, HLK requirement docs, open-source code); no static RE of OS
binaries. Every finding is marked FACT (with URL / header:line) or INFERENCE.

## Questions
- Q1 HLK / certification: which WDDM 2.0..3.1 features are MANDATORY for a full graphics KMD (GPU VA model,
  monitored fences, residency, D3D12 UMD, preemption granularity, per-engine TDR, ...)?
- Q2 Is a D3D12 UMD required for a 2.x/3.x full graphics driver to load (vs. only for certification)?
- Q3 Complete GpuMmu surface: DDIs, QAI types (GPUMMUCAPS, PAGETABLELEVELDESC, ...), paging-buffer operations
  (UPDATEPAGETABLE, FLUSHTLB, COPYPAGETABLE, ...), caps flags, callbacks.
- Q4 Residency + monitored fence surface for the UMD (Mesa winsys) and KMD.
- Q5 WDDM version -> Windows build mapping; what 2.1..3.1 add; which are opt-in caps.
- Q6 Open-source full WDDM 2.x/3.x KMDs to compare against.
- Q7 Mali G76 MMU fit with GpuMmu page-table-level descriptors (48-bit VA, 40-bit PA, 4 KB, LPAE-style).

## Progress log
- 2026-10-15 file created; research agent launched for Q1-Q6, Q7 by hand (panfrost/kbase sources).

## Q7 Mali G76 MMU vs GpuMmu (by hand, from our own KMD + KD values)
- FACT (KD, every boot): GpuId 0x72110000 (G76), MmuFeatures 0x2830 -> VA bits 0x30 = 48, PA bits 0x28 = 40;
  AsPresent 0xff = 8 hardware address spaces (AS0..AS7).
- FACT (drivers/MaliG76Wddm/maligpu.c:310-330, maligpu.h:140-146): the 1.3 KMD already builds Mali LPAE ("legacy",
  panfrost ARM_MALI_LPAE) 4-level tables for AS0: L0 -> L1 -> L2 -> L3, 4 KB tables of 512 x 8-byte descriptors,
  TABLE = 0x3, PAGE = 0x2C5 (ATTRINDX 1, RW, outer shareable), XN bit 54; TRANSTAB = L0 PA | 0x7, MEMATTR
  0x00888d88; flush via AS_COMMAND LOCK + FLUSH_PT (maligpu.c:386-388). One global VA space today.
- INFERENCE: this is exactly the shape GpuMmu abstracts (multi-level, fixed 512-entry 4 KB levels, 8-byte PTEs,
  resizable root): page-table level descriptors = 4 levels x 9 bits + 12-bit page offset = 48-bit VA, matches
  MmuFeatures. The KMD would translate VidMm's DXGK_PTE (PFN + flags) into Mali descriptors in UPDATEPAGETABLE.
- INFERENCE: GpuMmu gives every process its own root page table (DxgkDdiSetRootPageTable per context). Mali has 8
  AS slots, so >7 processes with GPU contexts need AS-slot multiplexing at submit time (bind the context's root to
  a free AS, evict the LRU one) - what kbase/panfrost already do on Linux (panfrost_mmu_as_get). Process count on
  this desktop (DWM + a few apps) is usually under 8, but the multiplexing is mandatory for correctness.
- OPEN: AArch64-format tables (TRANSCFG) vs the legacy LPAE format we use - irrelevant to GpuMmu (driver-owned
  format) as long as level sizes stay 4 KB/512 entries.
- 2026-10-15 agent: sources = public MicrosoftDocs/windows-driver-docs (display/*.md, = Learn source) and
  MicrosoftDocs/windows-driver-docs-ddi (d3dkmddi/d3dumddi/d3d10umddi reference), sparse clones in the session
  scratchpad; WDK 10.0.26100.0 headers locally.

## Agent findings (Q1-Q6)

### Version constants (FACT)
- FACT shared\d3dukmdt.h:40-54: WDDM2_0 0x5023, 2_1 0x6003, 2_2 0x700A, 2_3 0x8001, 2_4 0x9006, 2_5 0xA00B,
  2_6 0xB004, 2_7 0xC004, 2_8 0xD001, 2_9 0xE003, 3_0 0xF003, 3_1 0x10004, 3_2 0x11007 (WDK 26100).
- FACT Learn gpu-virtual-memory-in-wddm-2-0: "Individual engines of a GPU can operate in either physical or virtual
  mode: In physical mode, the scheduling model remains the same as it is with WDDM v1.x." and "WDDM v2 supports two
  distinct models ... GpuMmu and IoMmu. A driver must opt-in to support either or both of the models. A single GPU
  node can support both modes simultaneously."
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0

### Q5 WDDM version -> Windows build, features per version (written first, it frames Q1-Q4)
- FACT Learn "WDDM overview" table (display/windows-vista-display-driver-model-design-guide.md, ms.date 11/19/2025,
  page marked `ai-usage: ai-assisted`): 2.0 = Win10 1507, 2.1 = 1607, 2.2 = 1703, 2.3 = 1709, 2.4 = 1803,
  2.5 = 1809, 2.6 = 1903, 2.7 = 2004, 3.0 = Win11 21H2, 3.1 = Win11 22H2, 3.2 = Win11 24H2. (2.8/2.9 not in the
  table; hybrid-system-ddi.md: "Windows Server 2022 (WDDM 2.9)".)
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-display-driver-model-design-guide
- FACT Learn what-s-new-for-windows-11-display-and-graphics-drivers: "Windows 11, version 23H2 ... The WDDM version
  didn't change." and "Windows 11, version 22H2 (WDDM 3.1)". -> build 22621 (22H2) and 22631 (23H2) both = WDDM 3.1
  max. https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-windows-11-display-and-graphics-drivers
- FACT (inconsistency in Learn): iommu-dma-remapping.md: "introduced in Windows 11 22H2 (WDDM 3.0)" and
  enhanced-barriers.md: "Windows 11, version 22H2 WDK (WDDM 3.0)" vs the table/what's-new saying 3.0 = 21H2.
  INFERENCE: the 21H2 = 3.0 / 22H2 = 3.1 mapping is the one stated by both the overview table and the what's-new
  page; the two outliers look like doc errors. Either way 22621/22631 accept <= 3.1 (0x10004); 3.2 (0x11007) is
  24H2+ only.
- FACT per-version feature lists (Learn what's-new pages, all phrased "drivers CAN support", i.e. opt-in):
  - 2.0: GPU virtual addressing, driver residency, context monitoring (monitored fences) (wddm-2-0-and-windows-10.md).
  - 2.1: requirements table (wddm-2-1-features.md): Offer/reclaim improvements = Mandatory; Video memory management
    (64KB pages / 2MB PTE) = Optional; HW protected content = Select HW; GameDVR (present batching:
    PFND3DDDI_SYNCTOKENCB / pfnSyncToken) = Mandatory; Indirect display = Select HW; Driver store / side-by-side
    install = Mandatory; DirectX memory surface sharing for camera (NV12 cross-process shareable) = Mandatory.
    Quote: "WDDM 2.1 itself is optional. If implemented, it's a collection of mandatory and optional driver
    capabilities. A driver that supports any of these WDDM 2.1 capabilities must support all of the mandatory ones.
    ... Dxgkrnl checks for consistency in the capabilities and DDIs."
  - 2.2-2.5: VR/MR, HDR/colour, GPU-P + IOMMU isolation (2.4), DXR + display sync (2.5) - D3D12/display features.
  - 2.6: "Collect diagnostic info ... This new feature is a requirement in WDDM 2.6" (DXGKDDI_COLLECTDIAGNOSTICINFO);
    also SuperWetInk, VRS, background processing, hot update; HW scheduling.
  - 2.7: mesh shaders, DXR1.1, sampler feedback. 3.0: enhanced barriers, D3D12 video encode, USB4 DP, HW flip
    queue, IOMMU DMA remapping, CPU event signaled from KMD. 3.1: sharing backing store with KMD.
  - 3.2 (24H2, NOT available on 22621/22631): feature query DDI, native GPU fences, kernel-mode WDDM testing, ...
- FACT the key gate, wddm-2-1-features.md "Determining WDDM Version": "Dxgkrnl doesn't use the WDDMVersion cap as
  a way to determine which features are supported; that task is left to other caps or DDI presence. However, if the
  driver reports WDDM 2.1 support through the WDDMVersion cap, Dxgkrnl does validate that the caps or DDIs required
  by WDDM 2.1 are present and fail to create the adapter if they aren't. Inconsistent caps result in failure to
  create adapter or segment." https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-1-features
  INFERENCE: this is the documented mechanism behind our code 43 after DRIVERCAPS: DRIVERCAPS.WDDMVersion (and the
  interface version) promise a feature set that the caps/DDI table don't back. Each version's "mandatory" list is
  therefore a LOAD requirement, not only an HLK one.

### Q1 Mandatory features for a WDDM 2.0..3.1 full-graphics KMD
Source: WHCP "Specifications and Policies, Windows 11 version 22H2", Components and Peripherals.pdf (public zip
linked from https://learn.microsoft.com/en-us/windows-hardware/design/compatibility/whcp-specifications-policies ,
fwlink 2196181 -> download.microsoft.com .../WHCP_Documents_Windows 11, version 22H2.zip). The old
learn.microsoft.com/previous-versions/.../devicegraphics-requirements page stops at WDDM 1.3 (no WDDM2x IDs).
- FACT Device.Graphics.WDDM20.CoreRequirement (applies to "Windows 11 Client x64, Windows 11 Client ARM64",
  Terms: If-Implemented):
  - "1. Core.MemoryResidencyModel ... Drivers must explicitly create one or more paging queues for each D3D Device.
    Synchronization of operations of the paging queues must be done using Monitored Fences. Asynchronous Trim
    callbacks can occur which the UMD must handle by evicting resources ... A residency list of resources in memory
    must be maintained by the UMD for each D3D Device ... For compatibility with D3D11 and earlier versions, the UMD
    must manage residency based on Bind and Unbind operations."
    Table, Full Graphics column: MakeResident and Evict = Mandatory; Lock2 support = Mandatory; Monitored Fence
    support = Mandatory; GPU Sync Points = Mandatory; Trim Callback support = Mandatory; Offer/Reclaim = Mandatory.
  - "2. Virtual Memory: ... All engines that implement GPU virtual addressing must support capabilities defined in
    the GPU Virtual Memory DDI Specification ... It is mandatory to support single GPU address space per GPU logical
    adapter per process. Engines that support virtual addressing must support GPUMMU model for virtual addressing and
    must not have any dependency on physical addressing. Support for the IOMMU model of virtual addressing is
    optional. Drivers must support multi-level page tables and must support a mixture of 4KB and 64KB page tables.
    Display and Capture surfaces for integrated GPUs must be addressable via the Aperture Segment."
    Table: Virtual paging / Multi-level page tables / side-by-side 4K/64K page tables / page-based memory segment
    management = "If VA is implemented"; Tiled Resources (DX11.1+) = Mandatory; Programmable CPU host aperture and
    LDA = If applicable.
  - "3. Map Default" (D3D11/12 CPU-visible default textures: WB if CPU-read, WC if CPU-write "except on cache
    coherent UMA designs"; 16-byte aligned base; standard swizzle aligned to 4KB/64KB).
- FACT Device.Graphics.WDDM20.Core.Base (applies to "Windows 11 Client x64" only): "Graphics drivers can optionally
  implement WDDM 2.0 if they do not support D3D12. However, D3D12 support requires WDDM 2.0. ... The implementation
  of WDDM2.0 is optional although it is required for implementing a driver that supports D3D12." Its D3D table
  lists per-DX-class UMD DDIs; D3D12 UMD DDI appears only in the D3D12 row.
- FACT Device.Graphics.WDDM21.CoreRequirement (If-Implemented): Offer/Reclaim changes "must"; single-range VPR ->
  move paging DDIs; "If the hardware supports large page PTEs, e.g. 64KB, these ... should be implemented";
  UpdateAllocationProperties "optional". WDDM21.FlipQueueLatency: 64-bit PresentID per MPO flip,
  MaxQueuedMultiPlaneOverlayFlipVSync in DXGK_DRIVERCAPS ("must set ... to 1" if one flip in flight), GPU clock
  values in the VSync ISR callback.
- FACT Device.Graphics.WDDM22: "WDDM 2.2 is an optional version level ... If a driver implements WDDM 2.2 then it
  must implement all of the WDDM 2.1 Requirements. WDDM 2.0 drivers will continue to run on the newer Windows 10
  builds". WDDM22.DisplayRender.* (4HWID, PeriodicFrameNotification, DisplayWireFormatInfo, MPO3, SetTimings,
  DisplayDetection, ReducedBlackScreen) are "If implementing WDDM 2.2, full graphics drivers must implement".
- FACT WDDM24.CoreRequirements: UMD->KMD private data must survive fuzzing (paravirtualization security);
  WDDM24.Render.CrossAdapterResource: "set the KMD DXGK_VIDMMCAPS::CrossAdapterResource cap to true" (D3D9-12 UMDs).
- FACT WDDM26 (optional level): OneCore-compiled KMD + DX12 UMD; Containers (DX11 and DX12 workloads in Windows
  containers). WDDM27: GPU-P for non-DX APIs, Display.Diagnostics, MipiDsi. WDDM28/29: "optional version level";
  2.9 DList -> QueryDListForApplication2. WDDM30: optional level; SM6.6 optional; Render.CoreRequirement (hybrid
  integrated -> CrossAdapterResourceScanout); SelfRefreshMemorySupport (GPUs with VRAM); vGPU back-compat and WSL
  (only "for GPUs that support para-virtualization"); DisplayPortAux/Sbm.
- FACT the 22H2 spec has NO Device.Graphics.WDDM31.* entries (grep of the whole text).
- FACT apart from WDDM20.CoreRequirement, every WDDM2x/3x requirement above says "Applies to Windows 11 Client x64"
  only (not ARM64).
- FACT Learn wddm-driver-and-feature-caps.md (WDDM 1.2 table, still the base set): Full graphics = M for
  DXGK_DRIVERCAPS.WDDMVersion, SupportNonVGA, SupportSmoothRotation, PreemptionCaps, FlipOnVSyncMmIo,
  SupportPerEngineTDR, SupportDirectFlip, SupportKernelModeCommandBuffer (GDI HW accel).
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-driver-and-feature-caps
- INFERENCE Q1 summary: certification requires, for 2.0: GpuMmu on every VA engine with 4K+64K multi-level page
  tables, single VA space per process, paging queues + monitored fences, MakeResident/Evict/Trim/Lock2/Offer-
  Reclaim, tiled resources (DX11.1+). Above 2.0 the "mandatory" items are mostly display/D3D12/virtualization
  features, each level being an optional level that must include its predecessors. HLK is not a load gate; the
  load gate is dxgkrnl's caps/DDI consistency check (Q5).

### Q2 D3D12 UMD - required to load? UMD registration; WDDM 2.0 D3D11 UMD surface
- FACT HLK Device.Graphics.WDDM20.Core.Base (22H2 C&P spec, quoted in Q1): "Graphics drivers can optionally
  implement WDDM 2.0 if they do not support D3D12. However, D3D12 support requires WDDM 2.0." -> a WDDM 2.0 driver
  shipping only D3D9/10/11 UMD DDIs is an explicitly anticipated configuration, even for certification.
- FACT Learn what-s-new-for-prior-wddm-2-x-versions: "The WDDM 2.0 driver model is a pre-requisite for D3D12."
  (dependency is D3D12 -> WDDM2, not the reverse). wddm-2-1-features.md: "WDDM 2.1 supports the following D3D
  versions: D3D9, D3D10, D3D10.1, D3D11, D3D11.x, D3D12".
- FACT HLK from WDDM 2.6 up pulls DX12 in: WDDM26.OneCore ("DX12 UMDs must be compiled against the OneCore libs")
  and WDDM26.Containers ("load and run DirectX 11 and DirectX 12 workloads within Windows containers").
  INFERENCE: for certification, 2.6+ effectively presumes a DX12 UMD; for loading, no documented source says the
  KMD's adapter start checks for a D3D12 UMD (the UMD is only loaded by the runtime at device creation,
  loading-a-user-mode-display-driver.md: "The Direct3D runtime obtains the user-mode display driver's DLL name from
  the registry in order to load the user-mode display driver in the runtime's process space.").
- FACT registration (Learn enabling-support-for-the-direct3d-version-11-ddi.md): "you must specify the name of the
  DLL that contains the version 11 DDI as the third entry in the list of user-mode display driver names", example
  `HKR,, UserModeDriverName, %REG_MULTI_SZ%, umd.dll, umd.dll, umd.dll`. Learn wddm-2-1-features.md INF example
  for driver-store installs uses FOUR entries: `HKR,,UserModeDriverName,%REG_MULTI_SZ%,%13%\myUMD64.dll,
  %13%\myUMD64.dll, %13%\myUMD64.dll, %13%\myUMD64.dll` (+ UserModeDriverNameWoW). The meaning of the 4th slot is not
  stated on Learn. INFERENCE: slot 4 = D3D12 UMD; a driver without D3D12 lists 3 entries (or repeats the D3D11 DLL,
  which would then be asked for a D3D12 OpenAdapter12 export it doesn't have -> D3D12 device creation fails, D3D11
  unaffected). UNVERIFIED - check against an open-source INF (Q6) before relying on it.
- FACT WDDM 2.1+ INF: "WDDM 2.1 graphics drivers must use a different OS INF install section", run-from-driver-store
  (DestinationDirs = 13), decoration `NTamd64.10.0...14310` (wddm-2-1-features.md "Graphics INF Requirements").
- FACT D3D11 UMD DDI versions tied to WDDM (um\d3d10umddi.h:7804-7860): D3DWDDM2_0_DDI_MINOR_VERSION 17 build 5
  (header minor >=5), redefined to minor 32 build 6..9 for later header revisions;
  `D3DWDDM2_0_DDI_SUPPORTED = (INTERFACE_VERSION << 32) | (BUILD_VERSION << 16)`; D3DWDDM2_1 minor 34 (:7865);
  WDDM2_2..2_9/3_x follow (:7874-7946+). Device table: `D3DWDDM2_0DDI_DEVICEFUNCS* pWDDM2_0DeviceFuncs; // in/out:
  (Use when Interface == D3DWDDM2_0_DDI_INTERFACE_VERSION)` (d3d10umddi.h:7140), struct at :5457/:5672/:5873.
- FACT UMD callbacks gained in WDDM 2.0 (um\d3dumddi.h, D3DDDI_DEVICECALLBACKS, block
  `#if (D3D_UMD_INTERFACE_VERSION >= D3D_UMD_INTERFACE_VERSION_WDDM2_0)`): pfnMakeResidentCb, pfnEvictCb,
  pfnWaitForSynchronizationObjectFromCpuCb, pfnSignalSynchronizationObjectFromCpuCb,
  pfnWaitForSynchronizationObjectFromGpuCb, pfnSignalSynchronizationObjectFromGpuCb, pfnCreatePagingQueueCb,
  pfnDestroyPagingQueueCb, pfnLock2Cb, pfnUnlock2Cb, pfnInvalidateCacheCb, pfnReserveGpuVirtualAddressCb,
  pfnMapGpuVirtualAddressCb, pfnFreeGpuVirtualAddressCb, pfnUpdateGpuVirtualAddressCb, pfnCreateContextVirtualCb,
  pfnSubmitCommandCb, pfnDeallocate2Cb, pfnSignalSynchronizationObjectFromGpu2Cb, pfnReclaimAllocations2Cb,
  pfnGetResourcePresentPrivateDriverDataCb. WDDM2_1_1: pfnUpdateAllocationPropertyCb, pfnOfferAllocations2Cb;
  2_1_2: pfnReclaimAllocations3Cb, pfnAcquireResourceCb (SyncToken); 2_1_3: pfnReleaseResourceCb; 2_2_1: HW queue
  callbacks (CreateHwContext/HwQueue, SubmitCommandToHwQueue, ...); 2_4_2 SubmitPresentBltToHwQueueCb; 2_5_2
  SubmitPresentToHwQueueCb; 2_6_4 SubmitHistorySequenceCb.
- FACT HLK WDDM20.CoreRequirement (Q1): the D3D11 UMD must keep a per-device residency list, create >=1 paging
  queue per device, sync with monitored fences, handle async Trim, and "For compatibility with D3D11 and earlier
  versions, the UMD must manage residency based on Bind and Unbind operations."
- INFERENCE (important for us): the D3D11 UMD's command submission path in a WDDM 2.0 driver can stay the legacy
  pfnRenderCb with allocation/patch lists IF the context's node is in physical mode (Learn quote in "Version
  constants": "In physical mode, the scheduling model remains the same as it is with WDDM v1.x. The UMD continues to
  generate the allocation and patch location lists"). Residency (MakeResident/Evict/paging queue) is still a
  WDDM 2.0 UMD duty regardless of engine mode ("[virtual mode UMD] is still responsible for managing the residency"
  - for physical mode Learn driver-residency page, see Q4).

### Q3 GpuMmu model surface (headers = WDK 10.0.26100.0; Learn DDI ref = windows-driver-docs-ddi)
Caps / opt-in
- FACT shared\d3dkmddi.h:2255 DXGK_VIDMMCAPS (WDDM2_0 block): VirtualAddressingSupported, GpuMmuSupported,
  IoMmuSupported, ReplicateGdiContent, NonCpuVisiblePrimary; then 2_2 ParavirtualizationSupported; 2_4
  IoMmuSecureModeSupported, DisableSelfRefreshVRAMInS3; 2_7 IoMmuSecureModeRequired; 2_9 MapAperture2Supported,
  CrossAdapterResourceTexture/Scanout; 3_1 AlwaysPoweredVRAM; + UINT PagingNode.
  Learn ns-d3dkmddi-_dxgk_vidmmcaps: "To express support for GPU virtual memory addressing, the driver should set
  the VirtualAddressingSupported cap and GpuMmuSupported or IoMmuSupported caps. GpuMmuSupported and IoMmuSupported
  cannot be set at the same time." (NB contradicts the concept page "opt-in to support either or both" - the DDI
  ref is the stricter one; use it.)
- FACT d3dkmddi.h DXGK_DRIVERCAPS WDDM2_0 additions: HybridIntegrated, InternalGpuVirtualAddressRangeStart/End,
  SupportSurpriseRemoval; 2_1: SupportMultiPlaneOverlayImmediateFlip, CursorScaledWithMultiPlaneOverlayPlane0,
  HybridAcpiChainingRequired, MaxQueuedMultiPlaneOverlayFlipVSync; 2_4 MiscCaps union (SupportContextlessPresent,
  Detachable, 2_5 VirtualGpuOnly, 2_6 ComputeOnly, 2_7 IndependentVidPnVSyncControl, 2_8
  NoHybridDiscreteDListDllSupport, 3_0 DisplayableSupport); 2_9 MaxHwQueuedFlips, HwQueuedFlipCaps.
  DXGK_WDDMVERSION enum: v2 0x2000, 2_1 0x2100, ... 2_9 0x2900, 3_0 0x3000, 3_1 0x3100, 3_2 0x3200.
  (Learn DRIVERCAPS ref still says of WDDMVersion: "If a driver supports Windows 7 or later features ... this member
  is reserved and should be set to zero" - stale; wddm-2-1-features.md says drivers report 2.1 through it.)
- FACT d3dkmddi.h:2113 DXGK_GPUMMUCAPS: bits ReadOnlyMemorySupported, NoExecuteMemorySupported, ZeroInPteSupported,
  ExplicitPageTableInvalidation, CacheCoherentMemorySupported, PageTableUpdateRequireAddressSpaceIdle,
  LargePageSupported, DualPteSupported; 2_1 AllowNonAlignedLargePageAddress, SysMem64KBPageSupported; 2_6
  InvalidTlbEntriesNotCached; 2_9 SysMemLargePageSupported; 3_1 CachedPageTables; then DXGK_PAGETABLEUPDATEMODE
  PageTableUpdateMode {CPU_VIRTUAL, GPU_VIRTUAL, GPU_PHYSICAL} (:2055), VirtualAddressBitCount,
  LeafPageTableSizeFor64KPagesInBytes, PageTableLevelCount, LegacyBehaviors.SourcePageTableVaInTransfer.
  Learn: "PageTableLevelCount ... minimum value is 2 (DXGK_MIN_PAGE_TABLE_LEVEL_COUNT) ... When PageTableLevelCount
  is 2, the root page table is dynamically resizable ... DxgkDdiGetRootPageTableSize. When >2, all page table levels
  have a fixed size"; "When page directories are located in a local GPU memory segment, the update mode cannot be set
  to DXGK_PAGETABLEUPDATE_CPU_VIRTUAL"; PageTableUpdateRequireAddressSpaceIdle = VidMm suspends all contexts sharing
  the address space while PTEs are modified / TLB invalidated.
- FACT d3dkmddi.h:2072 DXGK_PAGE_TABLE_LEVEL_DESC {PageTableIndexBitCount, PageTableSegmentId,
  PagingProcessPageTableSegmentId, PageTableSizeInBytes, PageTableAlignmentInBytes ("0 means the page size of the
  memory segment")}; one per level via DXGKQAITYPE_PAGETABLELEVELDESC.
- FACT QAI types (d3dkmddi.h _DXGK_QUERYADAPTERINFOTYPE, WDDM2_0 block): QUERYSEGMENT4 = 11, SEGMENTMEMORYSTATE = 12,
  GPUMMUCAPS = 13, PAGETABLELEVELDESC = 14, PHYSICALADAPTERCAPS = 15, DISPLAY_DRIVERCAPS_EXTENSION = 16. Later:
  2_4 NODEPERFDATA 24, ADAPTERPERFDATA(_CAPS) 25/26, GPUVERSION 27; 2_5 DEVICE_TYPE_CAPS 28; 2_6 WDDMDEVICECAPS 29,
  GPUPCAPS 30, SCANOUT_CAPS 33; 2_9 PHYSICAL_MEMORY_CAPS 34, IOMMU_CAPS 35; 3_1 HARDWARERESERVEDRANGES2 36,
  NATIVE_FENCE_CAPS 37, USERMODESUBMISSION_CAPS 38; 3_2 = 39..48.
- FACT d3dukmdt.h:315 DXGK_PTE: Valid, Zero, CacheCoherent, ReadOnly, NoExecute, Segment:5, LargePage,
  PhysicalAdapterIndex:6, PageTablePageSize:2, SystemReserved0, Reserved:44; union PageAddress/PageTableAddress
  ("High 52 bits of 64 bit physical address"). The KMD converts this to its own HW PTE format.

DDIs (km\dispmprt.h DRIVER_INITIALIZATION_DATA, WDDM2_0 block ~:2856)
- FACT DxgkDdiRenderGdi, DxgkDdiSubmitCommandVirtual, DxgkDdiSetRootPageTable, DxgkDdiGetRootPageTableSize,
  DxgkDdiMapCpuHostAperture, DxgkDdiUnmapCpuHostAperture, DxgkDdiCheckMultiPlaneOverlaySupport2,
  DxgkDdiCreateProcess, DxgkDdiDestroyProcess, DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2, Reserved1/2,
  DxgkDdiPowerRuntimeSetDeviceHandle, DxgkDdiSetStablePowerState, DxgkDdiSetVideoProtectedRegion.
  WDDM2_1: CheckMPOSupport3, SetVidPnSourceAddressWithMPO3, PostMultiPlaneOverlayPresent,
  ValidateUpdateAllocationProperty, ControlModeBehavior, UpdateMonitorLinkInfo. 2_2: HW context/queue DDIs +
  display (PeriodicFrameNotification, SetTimingsFromVidPn, SetTargetGamma, ..., ExchangePreStartInfo,
  GetMultiPlaneOverlayCaps, GetPostCompositionCaps). 2_3 UpdateHwContextState, protected sessions. 2_4
  SetSchedulingLogBuffer, SetupPriorityBands, NotifyFocusPresent, SetContextSchedulingProperties, Suspend/ResumeContext,
  SetVirtualMachineData, Begin/EndExclusiveAccess, QueryDiagnosticTypesSupport, ControlDiagnosticReporting,
  ResumeHwEngine. 2_5 SignalMonitoredFence, PresentToHwQueue, ValidateSubmitCommand, ... 2_6 Save/RestoreMemoryFor
  HotUpdate, CollectDiagnosticInfo. 2_7 ControlInterrupt3. 2_9 flip-queue DDIs.
- FACT arg structs: DXGKARG_SETROOTPAGETABLE {hContext, D3DGPU_PHYSICAL_ADDRESS Address, NumEntries} (:5114);
  DXGKARG_GETROOTPAGETABLESIZE {NumberOfPte in/out, PhysicalAdapterIndex} (:5133); DXGKARG_SUBMITCOMMANDVIRTUAL
  {hContext, DmaBufferVirtualAddress, DmaBufferSize, pDmaBufferPrivateData, DmaBufferPrivateDataSize,
  DmaBufferUmdPrivateDataSize, SubmissionFenceId, VidPnSourceId, FlipInterval, Flags, EngineOrdinal, NodeOrdinal}
  (:5218); DXGKARG_CREATEPROCESS {hDxgkProcess, hKmdProcess out, Flags, NumPasid, pPasid, [2_4] hKmdVmWorkerProcess,
  ProcessName} (:5179).
  Learn: SetRootPageTable "is used to notify a context when its associated root page table is resized or moved ...
  level 1 synchronization ... The targeted context is guaranteed to be idled"; GetRootPageTableSize "is called only
  when DXGK_GPUMMUCAPS::PageTableLevelCount is two"; SubmitCommandVirtual "The driver is responsible for making sure
  the correct address space is restored ahead of submitting a particular DMA buffer", STATUS_INVALID_PARAMETER puts
  the device in error, "All other return values will lead to an OS bugcheck". CreateProcess: "During process
  creation, the kernel-mode driver is allowed to call DxgkCbReserveVirtualAddressRange".
- FACT DXGK_CREATECONTEXTFLAGS gains `VirtualAddressing : 1` in WDDM2_0 (d3dkmddi.h:1511-1521) - the per-context
  marker that a context runs in virtual mode (UMD side: pfnCreateContextVirtualCb). Learn lists no per-node
  "physical/virtual" cap. INFERENCE: the mode is chosen per context by the UMD (CreateContext vs CreateContextVirtual).

Paging-buffer operations (d3dkmddi.h _DXGK_BUILDPAGINGBUFFER_OPERATION)
- FACT values: TRANSFER 0, FILL 1, DISCARD_CONTENT 2, READ_PHYSICAL 3, WRITE_PHYSICAL 4, MAP_APERTURE_SEGMENT 5,
  UNMAP_APERTURE_SEGMENT 6, SPECIAL_LOCK_TRANSFER 7, VIRTUAL_TRANSFER 8, VIRTUAL_FILL 9, INIT_CONTEXT_RESOURCE 10,
  [2_0] UPDATE_PAGE_TABLE 11, FLUSH_TLB 12, UPDATE_CONTEXT_ALLOCATION 13, COPY_PAGE_TABLE_ENTRIES 14,
  NOTIFY_RESIDENCY 15, [2_2] SIGNAL_MONITORED_FENCE 16, [2_9] MAP_APERTURE_SEGMENT2 17, NOTIFY_FENCE_RESIDENCY 18,
  [3_2] MAP_MMU 19, UNMAP_MMU 20, NOTIFY_RESIDENCY2 21, NOTIFY_ALLOC 22.
  NB the "WDDMv1 only"/"WDDMv2 only" tags on 0-9 are Win7 _ADVSCH_-era labels and inconsistent (header says
  VIRTUAL_TRANSFER "WDDMv2 Only"; Learn says "WDDMv1 only"). Learn gpu-segments.md (current): "Starting in WDDM 2.0,
  VidMm manages a memory segment as a pool of physical pages ... copied into and out of a memory segment using
  Fill/Transfer/Discard/FillVirtual/TransferVirtual paging operations." INFERENCE: a GpuMmu driver must implement
  TransferVirtual/FillVirtual + UpdatePageTable + FlushTlb at minimum, and keep Transfer/Fill/Discard for physically
  accessed allocations.
- FACT structs (d3dkmddi.h:4685-4783): FLUSHTLB {RootPageTableAddress, hProcess, Start/EndVirtualAddress};
  UPDATEPAGETABLE {PageTableLevel, hAllocation, DXGK_PAGETABLEUPDATEADDRESS PageTableAddress (CpuVirtual |
  GpuPhysical | GpuVirtual union), DXGK_PTE* pPageTableEntries, StartIndex, NumPageTableEntries, Flags {Repeat,
  InitialUpdate, NotifyEviction, Use64KBPages, [3_1] NativeFence}, DriverProtection, AllocationOffsetInBytes,
  hProcess, UpdateMode, pPageTableEntries64KB, FirstPteVirtualAddress}; FILLVIRTUAL {hAllocation,
  AllocationOffsetInBytes, FillSizeInBytes, FillPattern, DestinationVirtualAddress}; TRANSFERVIRTUAL {hAllocation,
  AllocationOffsetInBytes, TransferSizeInBytes, Source/DestinationVirtualAddress, SourcePageTable,
  TransferDirection, Flags {Src64KBPages, Dst64KBPages}, DestinationPageTable}; NOTIFYRESIDENCY {hAllocation,
  PhysicalAddress, Resident}; COPYPAGETABLEENTRIES {NumRanges, pRanges}; UPDATECONTEXTALLOCATION {ContextAllocation
  VA, size, private data}; [2_2] SIGNALMONITOREDFENCE {MonitoredFenceGpuVa, MonitoredFenceValue}.
  DXGKARG_BUILDPAGINGBUFFER gains `D3DGPU_VIRTUAL_ADDRESS DmaBufferGpuVirtualAddress; UINT DmaBufferWriteOffset;`
  in WDDM2_0 - paging buffers themselves execute at a GPU VA.
- FACT Learn system-paging-process.md: "The paging process has its own GPU virtual address space, GPU contexts and
  direct memory access (DMA) buffers (called paging buffers). It has its own page tables which are pinned in physical
  memory ... The DirectX graphics kernel initializes enough page tables ... to cover the 1 GB virtual address space.
  The scratch area is used to temporary map allocations during transfer and fill operations"; paging-process page
  tables are initialised with "PageTableUpdateMode ... forced to CPU_VIRTUAL and must be completed immediately using
  the CPU (the paging buffer should not be used)."
  Learn examples.md: a transfer = UpdatePageTable(src scratch VA) + UpdatePageTable(dst) + FlushTLB(paging root) +
  TransferVirtual, "submitted ... in the paging process context".
  INFERENCE (important): with GpuMmu the PAGING engine must run in virtual mode (its own root page table,
  DmaBufferGpuVirtualAddress). "Physical mode" survives only for render/other engines and only for
  AccessedPhysically allocations. For Mali this means the paging engine (CPU copies or a Mali job) needs the MMU
  address space of the paging process programmed - or the driver does paging on the CPU (PageTableUpdateMode
  CPU_VIRTUAL + CPU-executed transfers; whether CPU-side paging is allowed for all ops is UNVERIFIED in Learn).

Callbacks (km\dispmprt.h DXGKRNL_INTERFACE, WDDM2_0 block :2106)
- FACT DxgkCbMapContextAllocation, DxgkCbUpdateContextAllocation, DxgkCbReserveGpuVirtualAddressRange,
  DxgkCbAcquireHandleData, DxgkCbReleaseHandleData, DxgkCbHardwareContentProtectionTeardown. 2_1
  DxgkCbMultiPlaneOverlayDisabled, DxgkCbMitigatedRangeUpdate; 2_2 DxgkCbInvalidateHwContext, ...; 2_4 contiguous
  memory / MDL / IOMMU map callbacks, DxgkCbReportDiagnostic; 2_5 DxgkCbSignalEvent; 2_6 DxgkCbIsFeatureEnabled,
  DxgkCbSaveMemoryForHotUpdate. DXGKARGCB_RESERVEGPUVIRTUALADDRESSRANGE (d3dkmddi.h:8708) {hDxgkProcess,
  SizeInBytes ("Must be multiple of the leaf page table coverage size"), Alignment, StartVirtualAddress out,
  BaseAddress, AllowUserModeMapping}.

Physical-mode engines on a GpuMmu adapter
- FACT Learn gpu-virtual-memory-in-wddm-2-0: "Individual engines of a GPU can operate in either physical or virtual
  mode"; gpu-segments.md "Accessing allocations by physical address": "GPU engines that don't support GPU virtual
  addressing need to access allocations through their physical addresses ... the KMD must explicitly identify
  allocations that require a rendering engine to access them physically ... DXGK_ALLOCATIONINFOFLAGS2::
  AccessedPhysically ... Allocations created this way can be referenced through the allocation list on engines,
  operating in the physical addressing mode. For allocations that don't have this flag set ... Allocations created
  this way can't be referenced through the allocation list. Any command buffer submission referencing the
  allocation that way will be rejected." Also "In WDDM 2.0, a single aperture segment must be reported."
- FACT HLK WDDM20.CoreRequirement: "Engines that support virtual addressing must support GPUMMU model ... and must
  not have any dependency on physical addressing" (HLK, not load).
- FACT Learn gpu-virtual-address.md: GPUVA low bits 0-11 = byte offset; per-level PageTableIndexBitCount; "Before a
  graphics context of a process can be set for execution, KMD's DxgkDdiSetRootPageTable function is called";
  64KB-segment pages: "the bottom 16 bits of the virtual address and the physical address are guaranteed to match".
  support-for-64kb-pages.md: single-PTE mode (level-1 PTE PageTablePageSize selects 4K or 64K leaf) or dual-PTE mode
  (DualPteSupported).
- INFERENCE mandatory-for-GpuMmu set (DDI side): VIDMMCAPS{VirtualAddressingSupported, GpuMmuSupported, PagingNode};
  QAI GPUMMUCAPS + PAGETABLELEVELDESC (whether QUERYSEGMENT4 is also required is UNVERIFIED); DDIs SetRootPageTable,
  GetRootPageTableSize (when 2 levels), SubmitCommandVirtual (if any virtual context), CreateProcess/DestroyProcess;
  paging ops UpdatePageTable, FlushTlb, TransferVirtual, FillVirtual, CopyPageTableEntries (root PT resize),
  NotifyResidency (UNVERIFIED whether always sent), UpdateContextAllocation (only with context allocations).
  MapCpuHostAperture only with a CPU host aperture.

### Q4 Residency + monitored fences (KMD and UMD side)
Residency
- FACT Learn residency-overview.md: "residency is being moved to an explicit list on the device instead of the
  per-command buffer list. The video memory manager will ensure that all allocations on a particular device residency
  requirement list are resident before any contexts belonging to that device are scheduled"; UMD gets MakeResident /
  Evict "as well as be required to implement a new TrimResidency callback"; MakeResident/Evict are ref-counted.
  "**Important** Residency in the WDDM v2 is controlled exclusively by the device residency requirement list. This
  is true across all engines of the GPU and for every API."
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/residency-overview
- FACT same page, physical-mode engines: "For engines which don't support GPU virtual addressing, the allocation list
  and patch location list will continue to exist, however, they will be used purely for patching purposes and will no
  longer have any control over residency ... any references to allocations that are not resident will cause the GPU
  scheduler to reject the submission and put the device in error (lost). This mode of operation is considered legacy
  and we expect all GPU engines to get support for GPU virtual addressing in future hardware releases. It is expected
  that this mode of operation will be dropped in future versions of the WDDM."
  Table: no-VA engine = allocation list full size (patching only) + patch list full size; VA engine with
  DXGK_CONTEXTINFO_NO_PATCHING_REQUIRED = allocation list "16 entries. References the primary surface, if any, being
  written to ... The primary surface must already be on the device residency requirement list or the reference will
  be rejected"; "No GPU virtual address support + hardware scheduling" = no lists.
- FACT d3dkmddi.h:1550-1581 DXGK_CONTEXTINFO (returned by DxgkDdiCreateContext) gains in WDDM2_0
  `DXGK_CONTEXTINFO_CAPS Caps {NoPatchingRequired, DriverManagesResidency, UseIoMmu}` and
  `ULONG PagingCompanionNodeId // Zero based node index to use with context paging operations`.
- FACT Learn device-paging-queues.md: "Each graphics device has a dedicated paging queue ... A device paging fence
  object is associated with the queue and every operation gets assigned a unique fence value that gets signaled when
  the video memory manager completes the operation. An operation that can be done immediately ... returns a device
  paging fence value of zero. The device paging fence is a regular monitored fence object". MapGpuVirtualAddress /
  ReserveGpuVirtualAddress / MakeResident all return paging fence values (per-process-gpu-virtual-address-spaces.md).
- FACT Learn process-residency-budgets.md: pre-D3D12 "the budget is handled by the user mode driver in the form of
  Trim notifications and MakeResident failures with STATUS_NO_MEMORY"; KMD marks budgeted segments with
  DXGK_SEGMENTFLAGS2::ApplicationTarget.
- FACT access-to-non-resident-allocation (Learn table in driver-residency-in-wddm-2-0.md): "GPU access to
  allocations that aren't resident is illegal and will result in the device removed".
- FACT allocation-usage-tracking (same table): "With the allocation list going away, VidMm no longer has visibility
  into the allocations being referenced ... the UMD has to handle the synchronization with respect to direct CPU
  access to allocations and renaming." (-> Lock2 + UMD-side tracking.)
Monitored fences / context monitoring
- FACT Learn context-monitoring.md: created with pfnCreateSynchronizationObject2Cb, Info.Type =
  D3DDDI_MONITORED_FENCE; returns hSyncObject, FenceValueCPUVirtualAddress (read-only for CPU, "Adapters that support
  IoMmu should use this address for GPU access") and FenceValueGPUVirtualAddress ("To signal the fence, the GPU is
  allowed to write directly to this GPU virtual address. IoMmu GPUs shouldn't use this address").
  "GPUs should declare whether they're capable of atomically updating 64-bit values ... DXGK_VIDSCHCAPS::
  No64BitAtomics ... If a GPU is capable of only updating 32-bit values atomically, the OS handles the fence
  wraparound". GPU signal: "If a GPU engine isn't capable of writing to a monitored fence using its virtual address,
  the UMD uses the SignalSynchronizationObjectFromGpuCb callback to queue a software signal packet to the GPU
  context." "When a command buffer completes execution on the GPU, Dxgkrnl: Goes through the list of fence objects
  with pending waits ... Reads their current fence value ... Determines whether there are any waits that need to be
  unwaited." GPU wait = WaitForSynchronizationObjectFromGpuCb (scheduler-side dependency); CPU signal/wait callbacks.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/context-monitoring
- FACT d3dkmddi.h:713 DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED = 11 (Learn: "Raise this interrupt type when the
  monitored fence is signaled"), payload `MonitoredFenceSignaled {NodeOrdinal, EngineOrdinal}` under WDDM2_2
  (d3dkmddi.h:1093-1098; Learn "Supported starting with Windows 10, version 1703"); :716
  PERIODIC_MONITORED_FENCE_SIGNALED = 14.
- FACT d3dkmddi.h:~2000-2020 DXGK_VIDSCHCAPS WDDM2_0 adds No64BitAtomics, LowIrqlPreemptCommand; 2_3
  HwQueuePacketCap:4; 3_1 NativeGpuFence, OptimizedNativeFenceSignaledInterrupt.
- FACT DxgkDdiSignalMonitoredFence is a WDDM2_5 DDI (dispmprt.h, used with HW queues); paging-op
  SIGNAL_MONITORED_FENCE is WDDM2_2.
- INFERENCE (basic monitored fence): with no GPU-side fence write, nothing new is needed in the KMD beyond normal
  DMA-buffer completion (DXGK_INTERRUPT_DMA_COMPLETED with the submission fence id); Dxgkrnl evaluates monitored
  fences at each completion, and the UMD uses SignalSynchronizationObjectFromGpuCb for GPU-ordered signals. A GPU
  write to FenceValueGPUVirtualAddress needs a VA-mode context (the address is a GPU VA in the process space).
Minimum for a GpuMmu driver that keeps legacy DMA-buffer submission
- FACT UMD submission per mode: physical context -> pfnRenderCb with allocation/patch lists -> KMD DxgkDdiRender
  (+ DxgkDdiPatch) -> DxgkDdiSubmitCommand; virtual context (pfnCreateContextVirtualCb, DXGK_CREATECONTEXTFLAGS::
  VirtualAddressing) -> pfnSubmitCommandCb -> KMD DxgkDdiSubmitCommandVirtual (DmaBufferVirtualAddress; DMA buffer is
  a UMD allocation mapped in the process VA - no KMD copy/patch step; Learn: STATUS_INVALID_PARAMETER for malformed
  DMA "This behavior is different from a call to DxgkDdiSubmitCommand, where no error is allowed to be returned due
  to the ability to validate the data in a prior DxgkDdiRender call").
- INFERENCE minimum KMD set for "GpuMmu + packet scheduling, no HW queues": DRIVERCAPS.WDDMVersion = 0x2000 with
  interface 0x5023, VIDMMCAPS{VirtualAddressingSupported, GpuMmuSupported}, GPUMMUCAPS + PAGETABLELEVELDESC,
  SetRootPageTable/GetRootPageTableSize, CreateProcess/DestroyProcess, SubmitCommandVirtual (+ keep
  Render/Patch/SubmitCommand for physical contexts), BuildPagingBuffer for UpdatePageTable/FlushTlb/TransferVirtual/
  FillVirtual/CopyPageTableEntries/NotifyResidency, DXGK_CONTEXTINFO.Caps.NoPatchingRequired on VA contexts,
  DMA_COMPLETED interrupts as today. Monitored fence interrupt / SignalMonitoredFence / HW queues are NOT needed
  for 2.0 (they are 2.2/2.5 HW-scheduling features). UMD side: paging queue per device, MakeResident/Evict list,
  TrimResidencySet, Lock2/Unlock2, monitored-fence waits on paging fence values, Reserve/Map/FreeGpuVirtualAddress,
  CreateContextVirtual + SubmitCommandCb.

### Q6 Open-source full-graphics WDDM KMDs
- FACT virtio-win viogpu3d (Mesa virgl + d3d10umd, WDDM full graphics, unmerged PR):
  https://github.com/virtio-win/kvm-guest-drivers-windows/pull/943 , head max8rr8/kvm-guest-drivers-windows branch
  `viogpu3d`, files viogpu/viogpu3d/{driver.cpp, viogpu_adapter.cpp, viogpu3d.inx}. It is WDDM 1.3, not 2.x:
  driver.cpp:83 `InitialData.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;`, viogpu_adapter.cpp:491
  `pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_3;`. INF (viogpu3d.inx:78) registers a 4-slot UMD list with the same
  Mesa D3D10 UMD in every slot: `HKR,, UserModeDriverName, %REG_MULTI_SZ%, %13%\viogpu_d3d10.dll,
  %13%\viogpu_d3d10.dll,%13%\viogpu_d3d10.dll,%13%\viogpu_d3d10.dll` - so 4 entries of a non-D3D12 UMD is a
  working pattern in practice (on WDDM 1.3).
- FACT VirtualBox Guest Additions VBoxWddm (full graphics, Gallium/SVGA UMDs):
  https://github.com/VirtualBox/virtualbox/blob/main/src/VBox/Additions/win/Graphics/Video/mp/wddm/VBoxMPWddm.cpp
  - WDDM 1.2: :1870/:1874 `pCaps->WDDMVersion = DXGKDDI_WDDMv1_2;`, :5330 `DriverInitializationData.Version =
  DXGKDDI_INTERFACE_VERSION_WIN8;`. No GpuMmu.
- FACT (closest match to us) qemus/virtio-drivers-windows - VirtIO-GPU "full (render + display) WDDM 2.0
  kernel-mode driver" in Rust, AGPL-3.0, UMD = Mesa-based D3D11 at https://github.com/anonymix007/virtio-d3d11
  (README: "usermode part of full WDDM 2.0 driver"; upstream KMD repo anonymix007/virtio-drivers-windows-rs).
  https://github.com/qemus/virtio-drivers-windows (commit 154ddf08, 2026-10-05). Files: gpu-wddm/src/lib.rs (DDI
  table, BuildPagingBuffer, create_context), gpu-wddm/src/adapter.rs (QueryAdapterInfo), gpu-wddm/src/process.rs
  (PAGE_TABLE_DESC), virtio_wddm_rs.inx.
  - lib.rs:165 `initial_data.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;`; adapter.rs:1372
    `driver_caps.WDDMVersion = DXGKDDI_WDDMv2;`; :1398-1399 VirtualAddressingSupported + GpuMmuSupported (comment
    "GPU MMU emulation is currently broken"); PagingNode = COPY engine; SupportPerEngineTDR = false;
    MultiEngineAware + PreemptionAware; preemption DMA_BUFFER_BOUNDARY; FlipOnVSyncMmIo etc.
  - GPUMMUCAPS (adapter.rs:~1556): ExplicitPageTableInvalidation = 1, PageTableUpdateMode = CPU_VIRTUAL,
    VirtualAddressBitCount = 48, PageTableLevelCount = 3; process.rs:165 level desc PageTableIndexBitCount 12,
    PageTableSegmentId 0, PageTableAlignmentInBytes 4096 - a software-emulated GpuMmu (Learn: ExplicitPageTable
    Invalidation "is typically used by a software driver that needs to emulate page table").
  - MIXED MODE: fill_metadata (adapter.rs:1005-1025) sets DXGK_NODEMETADATA.GpuMmuSupported only on the Copy node
    and "Other" nodes; the Graphics node and a "PhysicalOther" node leave it 0 (physical). create_context
    (lib.rs:1159-1235): Graphics/PhysicalOther contexts get DmaBufferSize 1 MB, AllocationListSize 1024,
    PatchLocationListSize 1024 (legacy Render/Patch path); Copy/Other contexts get DmaBufferSize 0, no lists,
    Caps.NoPatchingRequired + Caps.DriverManagesResidency (virtual path).
  - DDI table (lib.rs:160-260) includes BOTH DxgkDdiRender, DxgkDdiPatch, DxgkDdiSubmitCommand AND RenderGdi,
    SubmitCommandVirtual, SetRootPageTable, GetRootPageTableSize, Map/UnmapCpuHostAperture, CreateProcess,
    DestroyProcess, CheckMPOSupport2, SetVidPnSourceAddressWithMPO2, PowerRuntimeSetDeviceHandle,
    SetStablePowerState, SetVideoProtectedRegion, CalibrateGpuClock, FormatHistoryBuffer, GetNodeMetadata,
    Acquire/ReleaseSwizzlingRange, ... (the whole WDDM2_0 block populated).
  - paging ops handled (lib.rs:867-1096): MAP/UNMAP_APERTURE_SEGMENT, FILL, DISCARD_CONTENT, TRANSFER,
    VIRTUAL_TRANSFER, VIRTUAL_FILL, UPDATE_PAGE_TABLE, FLUSH_TLB, COPY_PAGE_TABLE_ENTRIES, NOTIFY_RESIDENCY.
  - INF virtio_wddm_rs.inx:70: `HKR,, UserModeDriverName, %REG_MULTI_SZ%, %13%\dx11um_virtio.dll,
    %13%\dx11um_virtio.dll,%13%\dx11um_virtio.dll,%13%\dx11um_virtio.dll` - a WDDM 2.0 adapter with a D3D11-only
    Mesa UMD in all four slots. (README gallery shows a running desktop; whether D3D11 rendering is stable is not
    stated.) INFERENCE: this is open-source evidence that WDDM 2.0 + GpuMmu caps + a physical 3D node + no D3D12
    UMD is a loadable configuration - i.e. exactly the shape a Mali first step could take, with the paging
    (copy) node virtual.
- FACT D-Ogi/amdgpu-wddm - open WDDM stack for AMD gfx1013 (ASRock BC-250), license PolyForm Noncommercial
  (docs/adr/0004; GitHub "NOASSERTION") - READ ONLY for us, do not copy code. Active (pushed 2026-10-15).
  https://github.com/D-Ogi/amdgpu-wddm , driver/kmd/wddm.c (full table behind EnableFullWddm), driver/kmd/vidmm.c
  (page tables behind EnableGpuVa), driver/kmd/bc250kmd.h, driver/kmd/bc250kmd.inf.
  - bc250kmd.h:62/85 compiled at DXGKDDI_INTERFACE_VERSION 0xE003 (WDDM2_9); wddm.c:9187 Data->Version =
    DXGKDDI_INTERFACE_VERSION_WDDM2_9 but wddm.c:4127 `caps->WDDMVersion = DXGKDDI_WDDMv2;` with the comment
    (wddm.c:9099) "every WDDM 2.1-3.1 member stays NULL while WDDMVersion is 2.0". INFERENCE: the interface
    version and the DRIVERCAPS.WDDMVersion are decoupled in practice; the feature-set promise is WDDMVersion
    (consistent with Learn wddm-2-1-features.md quoted in Q5).
  - DRIVERCAPS: VirtualAddressingSupported + GpuMmuSupported, PagingNode = SDMA node, SupportNonVGA,
    SupportSmoothRotation, SupportPerEngineTDR, SupportDirectFlip = TRUE, MultiEngineAware + PreemptionAware +
    LowIrqlPreemptCommand, preemption DMA_BUFFER_BOUNDARY. GPUMMUCAPS: CacheCoherentMemorySupported,
    ZeroInPteSupported, PageTableUpdateMode = GPU_PHYSICAL, 4 levels, LeafPageTableSizeFor64KPagesInBytes = 0.
    Answers DXGKQAITYPE_PHYSICALADAPTERCAPS (Flags.GpuMmuSupported) and per-node DXGK_NODEMETADATA.GpuMmuSupported.
  - DDI table (wddm.c:9186-9265): no DxgkDdiRender / DxgkDdiPatch (wddm.c:4626 "the table has no DxgkDdiPatch and
    no patch-location list"); has SubmitCommand + SubmitCommandVirtual, Create/DestroyProcess, Get/SetRootPageTable,
    GetNodeMetadata, CalibrateGpuClock, FormatHistoryBuffer, SetStablePowerState, QueryDependentEngineGroup,
    QueryEngineStatus, ResetEngine, CollectDbgInfo, Present, ControlInterrupt, GetScanLine.
  - Paging ops: TRANSFER, FILL, MAP/UNMAP_APERTURE_SEGMENT, VIRTUAL_TRANSFER, VIRTUAL_FILL, UPDATE_PAGE_TABLE,
    FLUSH_TLB, COPY_PAGE_TABLE_ENTRIES.
  - INF registers 3 UMD slots (bc250kmd.inf:106 `UserModeDriverName, 0x00010000, bc250umd.dll, bc250umd.dll,
    bc250umd.dll`), deliberately no WoW list.
  - CAUTION: several comments in that repo cite "static reading of 26100.9549" (dxgkrnl). Per our rules none of
    those dxgkrnl-behaviour claims are used here; only the repo's own code choices are cited.
- FACT iliyahaddad/WDDM-2.0-Driver-For-ATI-AMD-Series-Radeon-HD5000 - self-described research WDDM 2.0 KMD for
  Evergreen; the search snippet states it "was never built with a real WDK and never run on hardware" - no evidence
  value. https://github.com/iliyahaddad/WDDM-2.0-Driver-For-ATI-AMD-Series-Radeon-HD5000
- INFERENCE nothing else found: Microsoft's public samples are display-only (KMDOD) or IddCx; ReactOS has no
  full-graphics WDDM 2.x KMD; Mesa ships UMD-side code only (d3d10umd frontend, used by viogpu3d and virtio-d3d11).
- FACT per-node VA cap (missed in Q3, added here): shared\d3dkmdt.h:2003-2019 DXGK_NODEMETADATA {EngineType,
  FriendlyName, Flags, [WDDM2_0] BOOLEAN GpuMmuSupported, BOOLEAN IoMmuSupported}; Learn ns-d3dkmdt-_dxgk_nodemetadata:
  GpuMmuSupported "indicates whether the graphics engines of the node support the GpuMmu model". So the
  physical/virtual choice IS declared per node (DxgkDdiGetNodeMetadata) as well as per context (Q3 correction).
  Adapter-level twin: d3dkmddi.h:2168 DXGK_PHYSICALADAPTERFLAGS {IoMmuSupported, GpuMmuSupported, [2_1]
  MovePagingSupported, VPRPagingContextRequired, [2_2] AllowHardwareProtectedNoVpr, [2_7]
  VirtualCopyEngineSupported, [3_1] GpuVaIommuRequired/Global/CacheCoherent} via DXGKQAITYPE_PHYSICALADAPTERCAPS.
- FACT Learn mcdm-implementation-guidelines.md (compute-only drivers, but the only Learn page that lists DDIs by
  feature): "If physical addressing is used, pointers to the following functions must also be provided:
  DxgkDdiPatch, DxgkDdiRender, DxgkDdiSubmitCommand"; "If GPU virtual addressing is used ...: DxgkDdiCreateProcess,
  DxgkDdiDestroyProcess, DxgkDdiGetRootPageTableSize, DxgkDdiSetRootPageTable, DxgkDdiSubmitCommandVirtual";
  minimum list includes DxgkDdiCalibrateGpuClock, DxgkDdiFormatHistoryBuffer, DxgkDdiGetNodeMetadata,
  DxgkDdiSetStablePowerState, DxgkDdiSetVirtualMachineData, DxgkDdiQueryDependentEngineGroup,
  DxgkDdiQueryEngineStatus, DxgkDdiResetEngine, DxgkDdiCollectDbgInfo, ...; QAI: "If GPUVA is supported, the
  following DXGK_QUERYADAPTERINFOTYPE must be supported: DXGKQAITYPE_GPUMMUCAPS, DXGKQAITYPE_PAGETABLELEVELDESC";
  QUERYSEGMENT4 must be supported, QUERYSEGMENT/2/3 must not (MCDM); DRIVERCAPS.WDDMVersion "Must be set to
  DXGKDDI_WDDMv2_6 or later"; SchedulingCaps MultiEngineAware TRUE, PreemptionAware TRUE, LowIrqlPreemptCommand
  TRUE; "VirtualAddressingSupported ... If this member is set, the driver must also set GpuMmuSupported and/or
  IoMmuSupported"; "Memory Management: Virtual addressing is required. Physical addressing support might be enabled
  in the future." https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mcdm-implementation-guidelines
  INFERENCE: for a full-graphics driver this list is the best documented approximation of what dxgkrnl's
  consistency check wants per feature; note especially the minimum-list DDIs our WDDM 1.3 table may lack
  (CalibrateGpuClock, FormatHistoryBuffer, SetStablePowerState, SetVirtualMachineData) - run 4 of G15 S1 already
  added CalibrateGpuClock/SetStablePowerState stubs.

### Cross-check against G15 S1 (most actionable finding)
- FACT drivers/MaliG76Wddm/adapter.c:379-381: in every S1 run the KMD reported `caps->WDDMVersion = MwWddm2 ?
  (DXGK_WDDMVERSION)0 : DXGKDDI_WDDMv1_3;` i.e. WDDMVersion = 0 together with a 2.x interface version, following
  the Learn DXGK_DRIVERCAPS page ("If a driver supports Windows 7 or later features ... this member is reserved and
  should be set to zero").
- FACT both open-source WDDM 2.0 KMDs found (Q6) report `WDDMVersion = DXGKDDI_WDDMv2` (0x2000): qemus/virtio
  adapter.rs:1372, amdgpu-wddm wddm.c:4127. Learn wddm-2-1-features.md: "Drivers report WDDM 2.1 support through
  DXGK_DRIVERCAPS::WDDMVersion"; MCDM guidelines: WDDMVersion "Must be set to DXGKDDI_WDDMv2_6 or later". The
  "reserved, set to zero" sentence on the DRIVERCAPS page is a Win7-era leftover contradicted by three newer Learn
  pages.
- INFERENCE: WDDMVersion = 0 with a 2.x interface is the most likely documented-level cause of the code 43 right
  after DXGKQAITYPE_DRIVERCAPS (dxgkrnl validates "caps or DDIs required by" the reported WDDM version and fails the
  adapter on inconsistency - wddm-2-1-features.md). UNVERIFIED - next probe: S1 run 7 = Wddm2=1 (interface 0x5023)
  with WDDMVersion = DXGKDDI_WDDMv2 (0x2000), first in pure physical mode, then with the run-5 VA caps + GpuMmu stubs.

## Synthesis
Minimal mandatory set for a WDDM 2.0 GpuMmu driver (load-level, documented + open-source evidence)
1. DriverEntry: DRIVER_INITIALIZATION_DATA.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0 (0x5023) (newer interface
   values are fine if WDDMVersion stays 2.0 - amdgpu-wddm compiles at 2.9 and reports 2.0).
2. DRIVERCAPS: WDDMVersion = DXGKDDI_WDDMv2 (0x2000) - NOT 0; MemoryManagementCaps.VirtualAddressingSupported +
   GpuMmuSupported (never with IoMmuSupported), PagingNode; SchedulingCaps MultiEngineAware + PreemptionAware;
   PreemptionCaps; the WDDM 1.2 full-graphics M caps (SupportNonVGA, SupportSmoothRotation, SupportPerEngineTDR,
   SupportDirectFlip, FlipOnVSyncMmIo, GDI caps); InternalGpuVirtualAddressRangeStart/End (0/0 = whole range).
3. QAI: GPUMMUCAPS (VirtualAddressBitCount, PageTableLevelCount >= 2, PageTableUpdateMode,
   LeafPageTableSizeFor64KPagesInBytes or 0), PAGETABLELEVELDESC per level, QUERYSEGMENT4 (with exactly one aperture
   segment), PHYSICALADAPTERCAPS (Flags.GpuMmuSupported, NumExecutionNodes, PagingNodeIndex,
   DxgkPhysicalAdapterHandle), plus per-node DXGK_NODEMETADATA.GpuMmuSupported from GetNodeMetadata.
4. DDIs: CreateProcess, DestroyProcess, SetRootPageTable, GetRootPageTableSize (only called with 2 levels but listed
   as required with GPUVA), SubmitCommandVirtual; keep Render/Patch/SubmitCommand while any node is physical; the
   WDDM2_0 table entries the MCDM minimum list names (CalibrateGpuClock, FormatHistoryBuffer, SetStablePowerState,
   GetNodeMetadata, QueryDependentEngineGroup, QueryEngineStatus, ResetEngine, CollectDbgInfo).
5. BuildPagingBuffer: UPDATE_PAGE_TABLE, FLUSH_TLB, VIRTUAL_TRANSFER, VIRTUAL_FILL, COPY_PAGE_TABLE_ENTRIES,
   NOTIFY_RESIDENCY, plus the existing TRANSFER/FILL/DISCARD/MAP/UNMAP_APERTURE for physically-accessed
   allocations. The paging engine executes in the paging process's GPU VA space (system-paging-process.md), so the
   paging node must be a GpuMmu node; a CPU-emulated paging node with PageTableUpdateMode = CPU_VIRTUAL and
   ExplicitPageTableInvalidation is what the virtio driver does.
6. Physical engines stay allowed: render node with GpuMmuSupported = 0 in its node metadata keeps allocation +
   patch lists, AccessedPhysically allocations (DXGK_ALLOCATIONINFOFLAGS2), and DxgkDdiRender/Patch/SubmitCommand.
   Residency is still driven only by the device residency list (UMD MakeResident/Evict).
7. UMD (D3D11 at D3DWDDM2_0_DDI_INTERFACE_VERSION = (11 << 16) | 17, or | 32 with newer headers): paging queue per device, residency
   list via MakeResident/Evict + TrimResidencySet, Lock2/Unlock2, monitored-fence waits on paging fence values;
   CreateContextVirtual + SubmitCommandCb + Reserve/Map/FreeGpuVirtualAddressCb only for virtual-node contexts.
   HLK for 2.0 additionally wants 4K+64K page tables, tiled resources (DX11.1+), Offer/Reclaim - not load gates.

What each step up to 3.1 adds (all optional "levels"; each level must include the previous level's mandatory items)
- 2.1 (0x6003 / 0x2100): MANDATORY if claimed: Offer/Reclaim changes (ReclaimAllocations3Cb, OfferAllocations2Cb),
  present batching (SyncToken callbacks), driver-store INF + new install section, NV12 cross-process camera sharing,
  MPO PresentID / MaxQueuedMultiPlaneOverlayFlipVSync (HLK FlipQueueLatency). Optional: 64KB / 2MB PTE.
- 2.2 (0x700A): all 2.1 + display features (HLK "full graphics drivers must implement": 4HWID,
  PeriodicFrameNotification, wire format, MPO3, SetTimings, DisplayDetection, ReducedBlackScreen) + many D3D12 caps;
  HW-queue DDIs appear (optional).
- 2.3/2.4: HDR/colour, GPU-P/IOMMU isolation, CrossAdapterResource cap "set ... to true" (HLK), UMD->KMD fuzzing
  robustness; 2.5 DXR, display sync, DxgkDdiSignalMonitoredFence (HW queues); 2.6: CollectDiagnosticInfo "is a
  requirement in WDDM 2.6", OneCore + containers (DX11 and DX12 workloads); 2.7 GPU-P for non-DX, MIPI-DSI
  diagnostics; 2.8/2.9 no headline mandatory item (2.9 DList v2 if DList); 3.0 self-refresh VRAM (VRAM GPUs), WSL /
  vGPU back-compat only "for GPUs that support para-virtualization", DisplayableSupport cap; 3.1 (0x10004 /
  0x3100): sharing backing store with KMD (opt-in), CachedPageTables, NativeGpuFence caps (opt-in). The 22H2 HLK has
  no WDDM31 requirement IDs at all.
- Max for 22621/22631 = WDDM 3.1 (23H2 "WDDM version didn't change").

D3D12 answer
- Not required to load, and not even required to certify WDDM 2.0: HLK WDDM20.Core.Base "Graphics drivers can
  optionally implement WDDM 2.0 if they do not support D3D12". From 2.6 the HLK presumes a DX12 UMD (OneCore,
  containers). Open-source precedent: qemus/virtio WDDM 2.0 KMD with a Mesa D3D11-only UMD registered in all four
  UserModeDriverName slots.

Open questions not settled from documented sources
- Whether WDDMVersion = 0x2000 alone clears our code 43 (needs S1 run 7).
- Exact meaning of UserModeDriverName slot 4 (assumed D3D12; Learn never says) and whether listing a D3D11-only DLL
  there is harmless on 22621 (virtio does it; viogpu3d does it on WDDM 1.3).
- Which caps/DDIs dxgkrnl checks per reported WDDMVersion (Learn names the mechanism, not the list; MCDM list is the
  best proxy). Whether QUERYSEGMENT4 is required for full-graphics 2.0 (required for MCDM).
- Whether CPU-executed paging (no GPU paging engine) is acceptable to VidMm beyond page-table updates.
- Per-level ordering of PAGETABLELEVELDESC LevelIndex (amdgpu-wddm notes the docs leave it open; Learn says level 0
  = leaf).
- Learn self-contradictions recorded above: GpuMmu+IoMmu "either or both" vs "cannot be set at the same time";
  WDDM 3.0 = 21H2 vs "22H2 (WDDM 3.0)"; DRIVERCAPS.WDDMVersion "reserved, zero" vs report-via-WDDMVersion.
- 2026-10-15 agent: Q1-Q6 done, synthesis written. Sources fetched into the session scratchpad only (WHCP 22H2 zip,
  docs repos, viogpu3d, VirtualBox, amdgpu-wddm, qemus/virtio); nothing derived from Windows binaries.
- 2026-10-15 21:05 agent Q1-Q6 + synthesis in. Lead verified by hand on Learn wddm-2-1-features 'Determining WDDM Version': drivers report the level via DXGK_DRIVERCAPS::WDDMVersion and dxgkrnl validates caps/DDIs against it (contradicts the DRIVERCAPS ref page 'reserved, set to zero' that S1 runs 1-6 followed: WDDMVersion = 0). Proposed S1 run 7: interface 0x5023 + WDDMVersion 0x2000 (physical), then + run-5 VA caps/stubs - volatile probe + restart-device.
