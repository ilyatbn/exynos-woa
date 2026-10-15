# MaliG76Wddm: WDDM 1.3 -> WDDM 2.0 design (research, 2026-10-15)

Goal: Windows 11 DXGI tracked-fence (d3d11 CreateFence) returns NULL on our WDDM 1.3
adapter -> Windows Terminal / XAML SwapChainPanel crash (notes/thermal-clocks.md).
User decision: move the KMD to WDDM 2.0. This file is the design; FACT = cited,
inference = marked.

## Progress log
- started; inventory of drivers/MaliG76Wddm next.
- inventory read: adapter.c, command.c, memory.c, maligpu.c (AS0), maliwddm_abi.h, Mesa malikm_drm.c +
  d3d10umd Adapter.cpp/State.h. Section 1 written below.

## 1. Inventory of the current KMD (FACT, from the tree at d6573af)

Files (drivers/MaliG76Wddm): adapter.c 873 (DriverEntry, caps, QAI, ISR/DPC), command.c 848 (contexts, Render,
Present, SubmitCommand, job queue, soft vsync, TDR DDIs), memory.c 447 (segment, allocations, paging), maligpu.c 534
(Mali regs, MMU AS0, job slots), display.c 970 (VidPN, SetVidPnSourceAddress, display-only caps), panel.c 516
(DSIM/brightness), pram.c 269 (trace), include/maliwddm_abi.h (KMD<->UMD private ABI).

Init / interface version
- Full adapter: `DRIVER_INITIALIZATION_DATA.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3` (adapter.c:779),
  `DxgkInitialize` (adapter.c:837). Display-only mode: KMDOD, `DXGKDDI_INTERFACE_VERSION_WIN8` (adapter.c:744).
- DDIs filled (adapter.c:780-836): AddDevice, StartDevice, StopDevice, RemoveDevice, DispatchIoRequest,
  InterruptRoutine, DpcRoutine, QueryChildRelations/Status, QueryDeviceDescriptor, SetPowerState, ResetDevice,
  Unload, QueryInterface, QueryAdapterInfo, Escape, CreateAllocation, DestroyAllocation, DescribeAllocation,
  GetStandardAllocationDriverData, OpenAllocation, CloseAllocation, BuildPagingBuffer, CreateDevice, DestroyDevice,
  CreateContext, DestroyContext, Render, RenderKm (returns NOT_SUPPORTED, command.c:829), Present, Patch,
  SubmitCommand, PreemptCommand (no-op, command.c:684), QueryCurrentFence, ResetFromTimeout, RestartFromTimeout,
  CollectDbgInfo, GetNodeMetadata, ResetEngine, QueryEngineStatus, QueryDependentEngineGroup, CancelCommand,
  ControlInterrupt (CRTC_VSYNC only, command.c:785), GetScanLine, pointer DDIs, VidPN DDIs, SetVidPnSourceAddress,
  StopDeviceAndReleasePostDisplayOwnership, SystemDisplayEnable/Write.
- NOT present (all WDDM 2.0 additions): CreateProcess/DestroyProcess, SetRootPageTable, GetRootPageTableSize,
  MapCpuHostAperture, UpdatePageTable-style paging ops, CreateAllocation2-era GPU VA, SignalMonitoredFence-related
  paging ops, SubmitCommandVirtual, CalibrateGpuClock, QueryNodeInfo... (rg over adapter.c/MaliG76Wddm.h).

DXGK_DRIVERCAPS (adapter.c:358-392, `DriverCaps`)
- WDDMVersion = DXGKDDI_WDDMv1_3 (367); HighestAcceptableAddress = Mali PA bits (369); MaxAllocationListSlotId 256
  (370); SchedulingCaps.MultiEngineAware 1, PreemptionAware 1 (371-373; preemption disabled system-wide by the ESP
  script `EnablePreemption 0`, esp/extra-gpu-g10.cmd:72); GpuEngineTopology.NbAsymetricProcessingNodes 1 (374);
  FlipCaps.FlipOnVSyncMmIo if Parameters\FlipMmio (378); PresentationCaps.DriverSupportsCddDwmInterop (385);
  SupportNonVGA, SupportSmoothRotation (386-389). Nothing set in MemoryManagementCaps (zeroed, 366) -> no
  VirtualAddressingSupported/IoMmuSupported, no PagingNode, no SupportDirectFlip/FlipIndependent.
- QAI also answers QUERYSEGMENT3 (memory.c:17) and HISTORYBUFFERPRECISION=64 (adapter.c:433).

Memory model (memory.c:1-130, maligpu.c:184-200, 392-430)
- One **aperture** segment (Flags.Aperture, CpuVisible by Parameters\SegmentCpuVisible, not cache coherent),
  1 GiB (MaliG76Wddm.h:32), BaseAddress nominal MK_VA_START 0x2000000 (memory.c:31). VidMm owns residency.
- **The KMD owns ONE Mali address space (AS0) for all processes** (maligpu.c:193-200 writes AS0 TRANSTAB from
  Gpu->L0Pa). Each allocation gets a fixed GPU VA at DxgkDdiCreateAllocation (MkVaAlloc, memory.c:71) for its whole
  life; on DXGK_OPERATION_MAP_APERTURE_SEGMENT the MDL pages are mapped at that VA into the Mali page table on the
  CPU (memory.c:309-345), unmapped on UNMAP_APERTURE_SEGMENT; paging buffers stay empty (memory.c:44-46).
  So the Mali already runs on GPU virtual addresses with KMD-written page tables; the "aperture" offset VidMm picks
  is unused. No per-process isolation.
- The UMD learns each allocation's GPU VA through `MW_ESCAPE_ALLOC_INFO` (malikm_drm.c:695-697).

Submission / fences / interrupts (command.c)
- CreateContext: DmaBufferSize 16 KiB, DmaBufferSegmentSet 0 (system memory), DmaBufferPrivateDataSize =
  sizeof(MW_DMA_PRIVATE), AllocationListSize = PatchLocationListSize = 256 (command.c:46-58).
- UMD command buffer = array of MW_COMMAND {Type=JOB, Slot 0/1, Jc = GPU VA of the first job header}
  (maliwddm_abi.h:43-50, max 32). DxgkDdiRender copies them into the DMA private data, writes one marker word in
  the DMA buffer, and emits **one patch location per allocation-list entry** purely so WDDM 1.x VidMm makes them
  resident (command.c:76-123). DxgkDdiPatch does nothing (command.c:296-307).
- DxgkDdiPresent records a CPU blt/fill op; SubmitCommand executes presents on the CPU (DoPresent) and queues job
  chains with Arg->SubmissionFenceId (command.c:652-681) into a 16-deep ring processed at DIRQL via
  DxgkCbSynchronizeExecution (SyncSubmit command.c:405). Kick() starts one chain at a time with MkJobStart;
  Reap() on job IRQ or a 1 ms poll timer (PollDpc) with a 2 s per-chain timeout + hard stop + GPU re-bringup
  (command.c:328-399).
- Completion: `DXGK_INTERRUPT_DMA_COMPLETED` with `DmaCompleted.SubmissionFenceId`, node 0 engine 0, via
  DxgkCbNotifyInterrupt, then DxgkCbQueueDpc -> DpcRoutine -> DxgkCbNotifyDpc (command.c:311-321,
  adapter.c:345-350). QueryCurrentFence = LastCompletedFence (command.c:692).
- Vsync: software timer reporting DXGK_INTERRUPT_CRTC_VSYNC (command.c:481-501), ControlInterrupt CRTC_VSYNC only.
- Preemption: none (PreemptCommand no-op; EnablePreemption=0). TDR: ResetFromTimeout/ResetEngine re-bring-up.

UMD side (Mesa 26.2 d3d10umd + panfrost, external tree C:\Users\ilya\code\mali\mesa-26.2-g76)
- d3d10umd advertises **only the D3D10.0 DDI**: State.h:39-40 `SUPPORT_D3D10_1 0`, `SUPPORT_D3D11 0`; OpenAdapter10
  accepts D3D10_0_* interface versions (Adapter.cpp:126-145). So d3d11.dll talks to a feature-level-10_0 D3D10 DDI.
- Submission (include/win32-compat/malikm_drm.c:960-1000): pfnRenderCb with command buffer + allocation list +
  patch list; completion via pfnSignalSynchronizationObject2Cb(EnqueueCpuEvent). Also pfnAllocateCb,
  pfnDeallocateCb, pfnLockCb/UnlockCb (BOs stay locked = CPU mmap), pfnEscapeCb, pfnPresentCb,
  pfnSetDisplayModeCb. No GPU VA callbacks, no paging queue, no MakeResident.
- MS Learn 1 (gpu-virtual-memory-in-wddm-2-0): FACT "Individual engines of a GPU can operate in either physical or
  virtual mode: In physical mode, the scheduling model remains the same as it is with WDDM v1.x. The UMD continues
  to generate the allocation and patch location lists ... In virtual mode ... The UMD doesn't generate allocation
  or patch location lists, although it's still responsible for managing the residency". "WDDM v2 supports two
  distinct models ... GpuMmu and IoMmu. A driver must opt-in [DXGK_VIDMMCAPS] to support either or both". IoMmu:
  "Only system memory can be accessed ... CPU and GPU share a common address space and CPU page tables".
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-virtual-memory-in-wddm-2-0
- MS Learn 2 (DXGK_VIDMMCAPS): FACT "To express support for GPU virtual memory addressing, the driver should set
  the VirtualAddressingSupported cap and GpuMmuSupported or IoMmuSupported caps. GpuMmuSupported and IoMmuSupported
  cannot be set at the same time." PagingNode "applies to WDDM 1.x only. In WDDM 2.x, PagingNode was moved to
  DXGK_PHYSICALADAPTERCAPS". (No sentence on this page says VirtualAddressingSupported is mandatory for 2.0.)
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_vidmmcaps
- MS Learn 3 (context-monitoring, "introduced in Windows 10 (WDDM 2.0)"): FACT monitored fence created by the
  runtime via pfnCreateSynchronizationObject2Cb Info.Type = D3DDDI_MONITORED_FENCE; returns hSyncObject,
  FenceValueCPUVirtualAddress (RO), FenceValueGPUVirtualAddress (RW for the GPU; "IoMmu GPUs shouldn't use this").
  **"If a GPU engine isn't capable of writing to a monitored fence using its virtual address, the UMD uses the
  SignalSynchronizationObjectFromGpuCb callback to queue a software signal packet to the GPU context."** "When a
  command buffer completes execution on the GPU, Dxgkrnl: goes through the list of fence objects with pending waits
  ... reads their current fence value". DXGK_VIDSCHCAPS::No64BitAtomics for 32-bit-only GPUs.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/context-monitoring
  -> inference: a KMD whose engine cannot write fences still gets monitored fences (dxgkrnl signals them after the
  DMA_COMPLETED of the preceding submission). The Mali job manager CAN write memory (WRITE_VALUE job) - optional.
- MS Learn 4 (residency-overview): FACT "Residency in the WDDM v2 is controlled exclusively by the device residency
  requirement list. This is true across all engines of the GPU and for every API." "For engines which don't
  support GPU virtual addressing, the allocation list and patch location list will continue to exist, however,
  they will be used purely for patching purposes and will no longer have any control over residency ... any
  references to allocations that are not resident will cause the GPU scheduler to reject the submission and put the
  device in error (lost). This mode of operation is considered legacy ... It is expected that this mode of operation
  will be dropped in future versions of the WDDM." Table: "No GPU virtual address support (require patching,
  default)": allocation list full size, patch list full size. GPU VA support -> DXGK_CONTEXTINFO_NO_PATCHING_REQUIRED,
  16-entry allocation list (primary writes only). New UMD callbacks MakeResident/Evict + TrimResidency.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/residency-overview
  -> consequence: our "patch every allocation so VidMm maps it" trick (command.c:76-89) STOPS working under 2.0:
  the UMD must pfnMakeResidentCb every BO (or create them resident) whatever memory model is chosen.
- MS Learn 5 (pfnMakeResidentCb, D3DDDI_MAKERESIDENT): FACT E_PENDING -> "the user mode driver must wait for the
  PagingFenceValue on the monitored fence sync object associated with hPagingQueue to be signaled prior to
  submitting command buffers ... or a GPU page fault may occur"; hPagingQueue "Paging queue on the device that
  created the input allocations" (no NULL/default documented) -> UMD needs pfnCreatePagingQueueCb once per device.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_makeresidentcb
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-d3dddi_makeresident
- WDK 10.0.26100 headers (local, FACT): d3d10umddi.h:45/49 D3D_UMD_INTERFACE_VERSION_WDDM1_3 0x4002 / WDDM2_0
  0x5002; D3DWDDM2_0_DDI_SUPPORTED (7808..); d3dumddi.h:4535-4551 D3DDDI_DEVICECALLBACKS has pfnMakeResidentCb,
  pfnSignalSynchronizationObjectFromGpuCb, pfnCreatePagingQueueCb, pfnMapGpuVirtualAddressCb, pfnSubmitCommandCb.
- MS Learn 6 (DXGK_DRIVERCAPS): FACT WDDMVersion "If a driver supports Windows 7 or later features
  (DXGKDDI_INTERFACE_VERSION >= ..._WIN7), this member is reserved and should be set to zero" (we set v1_3 at
  adapter.c:367 - harmless today, drop it in the 2.0 work). SupportPerEngineTDR requires QueryDependentEngineGroup,
  QueryEngineStatus, ResetEngine. No DRIVERCAPS member enables monitored fences (no "SupportMonitoredFence" exists).
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_drivercaps
- MS Learn 7 (iommu-model): FACT "the GPU sends a data request to a compliant IOMMU. The request includes a shared
  virtual address and a process address space identifier (PASID). The IOMMU performs the address translation using
  the shared page table." "VidMm automatically registers any process using the GPU with the IOMMU and obtains a
  PASID". https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model
  -> inference: IoMmu is NOT usable: the Mali-G76 translates through its own AS page tables (maligpu.c:193), there is
  no SVA/PASID-capable SMMU in front of G3D exposed to Windows, and Windows ARM64 would not share CPU tables with it.
- MS Learn 8 (D3DDDI_SYNCHRONIZATIONOBJECTINFO2): FACT FenceValueGPUVirtualAddress "If the device doesn't support
  cache coherency with the CPU, it shouldn't write to the fence value using the GPU virtual address. Access to the
  fence value should be done only by the CPU." Monitored fences shared only via NT handles.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_synchronizationobjectinfo2
  -> our Mali runs non-coherent (GpuInfo SELECTED_COHERENCY none, adapter.c:409) => GPU fence writes are OUT; use
  the software-signal path only (UMD pfnSignalSynchronizationObjectFromGpuCb; dxgkrnl signals after DMA_COMPLETED).
  The KMD therefore needs NO DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED (d3dkmddi.h:713) in the first cut.
- MS Learn 9 (DXGK_BUILDPAGINGBUFFER_OPERATION): FACT MAP/UNMAP_APERTURE_SEGMENT "Common WDDMv1 and WDDMv2";
  TRANSFER/FILL/DISCARD/READ/WRITE_PHYSICAL "WDDMv1 only"; UPDATE_PAGE_TABLE, FLUSH_TLB, UPDATE_CONTEXT_ALLOCATION,
  COPY_PAGE_TABLE_ENTRIES, NOTIFY_RESIDENCY "Available starting with WDDM 2.0"; SIGNAL_MONITORED_FENCE WDDM 2.2.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ne-d3dkmddi-_dxgk_buildpagingbuffer_operation
  -> our BuildPagingBuffer (memory.c:301-360) already only does MAP/UNMAP on the CPU: survives unchanged in 2.0
  physical/aperture mode; must return success (no-op) for NOTIFY_RESIDENCY.
- WDK headers (FACT): dispmprt.h DRIVER_INITIALIZATION_DATA WDDM2_0 block = RenderGdi, SubmitCommandVirtual,
  SetRootPageTable, GetRootPageTableSize, Map/UnmapCpuHostAperture, CheckMultiPlaneOverlaySupport2, Create/
  DestroyProcess, SetVidPnSourceAddressWithMultiPlaneOverlay2, PowerRuntimeSetDeviceHandle, SetStablePowerState,
  SetVideoProtectedRegion. d3dkmddi.h: QAI types QUERYSEGMENT4 (11), GPUMMUCAPS (13), PAGETABLELEVELDESC (14),
  PHYSICALADAPTERCAPS (15); DXGK_PHYSICALADAPTERCAPS {NumExecutionNodes, PagingNodeIndex,
  DxgkPhysicalAdapterHandle, Flags} (2205); DXGK_VIDSCHCAPS adds No64BitAtomics, LowIrqlPreemptCommand under 2.0
  (2009-2012). Which of these 2.0 DDIs dxgkrnl insists on for a physical-only adapter is NOT documented on the pages
  read (inference: GpuMmu-only DDIs are needed only with GpuMmuSupported).
- MS Learn 10 (allocation-usage-tracking): FACT VidMm "is no longer in a position to track allocation usage ...
  This responsibility now falls to the user-mode driver"; Lock synchronization (no-overwrite/discard renaming, wait
  for busy allocations) is the UMD's; "For now, the UMD continues to need to call LockCb/UnlockCb ... In most cases,
  the UMD is able to keep the allocation mapped for its entire lifetime"; on non-I/O-coherent Arm the UMD must
  "Invalidate Cache" cacheable allocations. https://learn.microsoft.com/en-us/windows-hardware/drivers/display/allocation-usage-tracking
  -> our UMD already keeps BOs locked for life and syncs by WaitForIdle/CPU events (malikm_drm.c:695-703), and the
  segment is not cache coherent (memory.c:36 -> uncached CPU views), so no cache-invalidate work is expected.
- MS Learn 11 (pfnSignalSynchronizationObjectFromGpuCb): FACT "For WDDM v2 drivers, existing
  pfnSignalSynchronizationObjectCb and pfnSignalSynchronizationObject2Cb callbacks are deprecated" -> malikm_drm.c:983
  (EnqueueCpuEvent via ...Object2Cb) should move to FromGpuCb + a per-device monitored fence (later step).
- FACT (WDK um/d3d10umddi.h, 8344 lines): no "fence" identifier at all in the D3D10/11 UMD DDI; fences exist only in
  d3d11_4.h (API) and d3d12umddi.h. -> inference: ID3D11Fence / DXGI's tracked fence are implemented by d3d11.dll
  itself over D3DKMT monitored fences (consistent with the RE in thermal-clocks.md 18:45: "a runtime sync call").
  So the UMD needs no new fence DDI; what is missing is dxgkrnl agreeing to create a monitored fence on our adapter.

## 2. What WDDM 2.0 means for this KMD (synthesis of the log above)

2.1 Interface. `DRIVER_INITIALIZATION_DATA.Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0`, which exposes the
WDDM2_0 block of DDIs (dispmprt.h, listed in the log). DRIVERCAPS.WDDMVersion should be 0 for WIN7+ interfaces
(FACT, DXGK_DRIVERCAPS page). There is **no "SupportMonitoredFence" cap** (FACT, DRIVERCAPS member list): monitored
fences come with the 2.0 interface itself (FACT, context-monitoring "introduced in Windows 10 (WDDM 2.0)").

2.2 Memory model choice.
- **IoMmu: does not fit** (FACT: needs a PASID-capable IOMMU and shared CPU page tables; inference: the G76 walks its
  own AS page tables, maligpu.c:193-200, and nothing exposes an SVA SMMU for G3D to Windows).
- **GpuMmu: fits the hardware** (inference): G76 has its own MMU, 4 KiB pages, multilevel tables, several address
  spaces (AS_PRESENT, maligpu.c:102) -> per-process root tables + DxgkDdiSetRootPageTable per context. But it moves
  VA ownership to VidMm/UMD (FACT, gpummu-model: "VidMm doesn't automatically assign GPU virtual addresses ... This
  responsibility falls on the UMD") and needs SubmitCommandVirtual, UPDATE_PAGE_TABLE/FLUSH_TLB paging ops,
  GPUMMUCAPS/PAGETABLELEVELDESC, Create/DestroyProcess, and a UMD on pfnSubmitCommandCb + MapGpuVirtualAddressCb.
- **Neither (physical engine, "legacy" packet model): documented** (FACT, gpu-virtual-memory-in-wddm-2-0: "In
  physical mode, the scheduling model remains the same as it is with WDDM v1.x. The UMD continues to generate the
  allocation and patch location lists"; residency-overview table row "No GPU virtual address support (require
  patching, default)"). Our engine *is* virtually addressed internally, but dxgkrnl only needs to see physical-mode
  semantics: one aperture segment, MAP/UNMAP_APERTURE_SEGMENT (both "Common WDDMv1 and WDDMv2", FACT), Render/
  Patch/SubmitCommand with SubmissionFenceId and DMA_COMPLETED (unchanged code path, inference). Caveat (FACT,
  residency-overview): "This mode of operation is considered legacy ... expected that this mode ... will be dropped
  in future versions of the WDDM" - the docs read do not say it was dropped by Windows 11 23H2/24H2 (UNVERIFIED on
  device; the S10e runs 22631 per the ISO name in C:\Users\ilya\code, also UNVERIFIED).

2.3 Monitored fences. Runtime creates them (pfnCreateSynchronizationObject2Cb / D3DKMT, Type MONITORED_FENCE).
GPU signal either by writing FenceValueGPUVirtualAddress or by a software packet
(pfnSignalSynchronizationObjectFromGpuCb), after which dxgkrnl reads/advances the value when the preceding command
buffer completes (FACT, context-monitoring). A non-coherent device "shouldn't write to the fence value using the GPU
virtual address" (FACT, SYNCHRONIZATIONOBJECTINFO2) -> **our KMD signals nothing itself**; DMA_COMPLETED as today is
sufficient (inference). DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED (d3dkmddi.h:713) and DXGK_OPERATION_SIGNAL_
MONITORED_FENCE (WDDM 2.2) are not needed for a 2.0 software-signal driver (inference from the two FACTs).

2.4 Residency - the real breaking change. WDDM 2.0: "Residency ... is controlled exclusively by the device
residency requirement list ... across all engines ... and for every API"; in physical mode non-resident references
make the scheduler "reject the submission and put the device in error (lost)" (FACT). Our current mechanism -
**KMD patches every allocation so VidMm maps it** (command.c:76-89, written because WinPE runs 13-14 faulted
without it) - stops having any effect. The UMD must create a paging queue (pfnCreatePagingQueueCb), call
pfnMakeResidentCb (hPagingQueue mandatory, E_PENDING -> wait for PagingFenceValue on the queue's monitored fence),
and pfnEvictCb (FACT, MakeResident pages). The paging fence advances when our paging-buffer submission completes;
SubmitCommand already completes jobless (paging) submissions immediately via Kick() (command.c:326-337) -> works
unchanged (inference).

2.5 UMD DDI version. Our d3d10umd exposes only D3D10.0 DDI (State.h:39-40). The WDDM 2.0 UMD level is
D3D_UMD_INTERFACE_VERSION_WDDM2_0 / D3DWDDM2_0_DDI_* (FACT, header). **Not found in the docs read: whether the
d3d11/dxgi runtime refuses a D3D10.0-only UMD on a WDDM 2.0 KMD** (UNVERIFIED - first device test answers it). The
new callbacks (pfnMakeResidentCb, pfnCreatePagingQueueCb, pfnSignalSynchronizationObjectFromGpuCb, ...) are members
of D3DDDI_DEVICECALLBACKS (d3dumddi.h:4535-4551), handed to any UMD the runtime opens; inference: they are non-NULL
whenever the KMD is 2.0, so one UMD binary can detect the KMD level at runtime (NULL check, or D3DKMTQueryAdapterInfo
KMTQAITYPE_WDDM_2_0_CAPS) and keep working on the 1.3 KMD for A/B.

## 3. Intermediate options

- **WDDM 1.3 cap that makes DXGI skip the tracked fence: none documented.** DXGK_DRIVERCAPS has no fence-related
  member (FACT, member list), DXGI's BindToDwm/CreateTrackedFenceExtended behaviour is not on Learn at all (FACT: no
  page found; the knowledge comes from our own static RE, thermal-clocks.md 18:45). The UMD cannot intercept it either:
  the fence is created by d3d11.dll, not through a UMD DDI (FACT: no fence DDI in d3d10umddi.h). Dead end.
- **WDDM 2.0 + IoMmu, keep our addressing: not possible** - IoMmu means the GPU uses CPU VAs through a PASID IOMMU
  (FACT, iommu-model); it is the opposite of "keep our addressing".
- **WDDM 2.0 physical-mode ("legacy" packet scheduling) keeping our own Mali VA + AS0 tables: documented and the
  recommended first target** (FACT that the mode exists, sections 2.2/2.4; inference that dxgkrnl on Win11 still
  starts such an adapter - must be device-checked first). It keeps Render/Patch/Present/SubmitCommand, the patch-
  list ABI, the job queue and MW_ESCAPE_ALLOC_INFO VAs. The only mandatory UMD change is explicit residency.
- Later, optional: GpuMmu (per-process isolation, no allocation lists, D3D12-style UMD). Not needed for the
  tracked fence (inference: monitored fences are interface-level, section 2.3).
- Workarounds without KMD work (already known): Terminal "rendering.software", Console Host as default terminal.

## 4. Staged plan (one commit per step; each step ends with a device observation)

Every KMD step is behind a new **Parameters\Wddm2 (default 0)** read in DriverEntry next to GpuEnable/FullAdapter
(adapter.c:707-725), selecting the init table/version - rollback is a registry value from the ESP script or TWRP,
not a reflash. Caps answers branch on a global `MwWddm2` like the existing switches. 1.3 path stays byte-identical
when the switch is 0.

S0 - read-only device facts (no code). `ver`/winver build on the phone; current DxgKrnl ETW for the full adapter
start (already have the g18etw recipe). Observation: build number recorded; baseline QAI types requested by dxgkrnl
from the PRAM trace (QAI type is already traced).

S1 - KMD: report 2.0, physical mode, minimal. InitFull: Version WDDM2_0 when MwWddm2; DriverCaps: WDDMVersion 0,
MemoryManagementCaps all zero (no VirtualAddressingSupported), keep scheduling caps; add QAI answers:
PHYSICALADAPTERCAPS {NumExecutionNodes 1, PagingNodeIndex 0, DxgkPhysicalAdapterHandle = our adapter (exact
expected value UNVERIFIED - read the DXGK_PHYSICALADAPTERCAPS page before coding), Flags 0}; QUERYSEGMENT4 (same
single aperture segment as QUERYSEGMENT3 in DXGK_SEGMENTDESCRIPTOR4 with SegmentDescriptorStride); any other
unknown QAI type -> trace + STATUS_NOT_SUPPORTED. BuildPagingBuffer: NOTIFY_RESIDENCY -> success no-op.
Observation: PRAM shows DxgkInitialize 0, StartDevice ok, the set of QAI types dxgkrnl now asks (that list is the
ground truth for what 2.0 needs on this build); Device Manager no code 43. Desktop may still be wrong (UMD not
updated) - expected. Abort criterion: code 43 / DxgkInitialize != 0 -> log ETW reason, add the DDI it names.

S2 - check the runtime accepts our D3D10.0 UMD on a 2.0 KMD. m5test / d3dtri run with MALIKM_TRACE: does
OpenAdapter10 get called, are pfnMakeResidentCb/pfnCreatePagingQueueCb non-NULL (log them). Observation: d3dtri
either renders (unlikely - residency) or reports device removed; DWM state. If the runtime refuses the adapter ->
UMD must advertise a D3D11/WDDM2 DDI level (big; re-plan).

S3 - UMD residency (Mesa patch 00xx, malikm_drm.c): on bind create one paging queue per device; MakeResident on
every BO created (CREATE_BO path ~line 899) and opened (OpenResource 0034/0037/0049), loop on E_OUTOFMEMORY per the
doc, CPU-wait the paging fence on E_PENDING (pfnWaitForSynchronizationObjectFromCpuCb); Evict at GEM_CLOSE before
Deallocate. Present surfaces + primaries likewise. Keep the allocation list as is (still needed for patching
semantics and primary-write tracking). Observation: cube d3dtri 10 s at the 1.3-era fps (~28-30 real / earlier
391 at 572 MHz), zero MMU faults in MW_ESCAPE_STATS, no "device removed".

S4 - tracked-fence validation. Windows Terminal (DelegationTerminal back to Terminal for the test only), a XAML
SwapChainPanel app; WER LocalDumps folder C:\S10eTmp\dumps stays empty. Observation: Terminal stays up > 5 min,
typing works, no new d3d11 WaitTrackedFenceImpl dumps.

S5 - sync clean-up: replace deprecated SignalSynchronizationObject2Cb(EnqueueCpuEvent) with a per-device monitored
fence + pfnSignalSynchronizationObjectFromGpuCb and CPU waits on its value (FenceValueCPUVirtualAddress), giving
real per-submission WAIT_BO instead of WaitForIdle. Observation: same fps, fewer KMD idle waits.

S6 - KMD hygiene for 2.0: drop the "patch every allocation for residency" comment/logic only if S3 proves it
unnecessary (it is still needed if dxgkrnl validates patch lists); remove WDDMVersion v1_3; FlipIndependent etc. as
separate tasks already listed in wddm-docs-research.md.

S7 (optional, separate milestone) - GpuMmu: per-process Mali AS, SubmitCommandVirtual, UMD on pfnSubmitCommandCb.

## Risks

- R1 boot hang/black screen (notes/gpu-full-adapter-hang.md: full adapter went black, Windows alive, due to missing
  vsync/UMD device). A 2.0 start that succeeds but whose DWM device keeps getting "device lost" (S1 before S3) can
  loop DWM restarts or TDR -> 0x116/0x117. Mitigation: S1 and S3 flashed **together** for the first desktop boot, or
  S1 tested with Wddm2=1 only for one boot with wait.txt + collect and auto-revert of Parameters\Wddm2 by the ESP
  script on the next boot (same pattern as GpuEnable). WDT (82 s) only catches hard hangs, not a black DWM.
- R2 dxgkrnl on this build may require DDIs the docs do not list for physical mode (e.g. CalibrateGpuClock,
  Create/DestroyProcess) -> code 43, visible in ETW; cheap to iterate behind the switch.
- R3 the d3d11 runtime may refuse a D3D10.0-only UMD on a 2.0 KMD (UNVERIFIED) -> would force a D3DWDDM2_0 DDI
  surface in d3d10umd (large).
- R4 residency bugs = GPU page faults on our side (AS0 unmapped on evict) -> Reap() fault path resets the GPU and
  drops the submission (command.c:377-392); visible in stats, not a hang. Evict-while-busy must be avoided (VidMm
  defers destruction, FACT allocation-usage-tracking; eviction timing UNVERIFIED).
- R5 "legacy" physical mode may be removed in a future WDDM (FACT, residency-overview) - acceptable for 23H2/24H2.
- R6 shared surfaces DWM opens must be MakeResident by DWM's instance of our UMD; missing one = DWM device lost
  -> black desktop (same symptom class as R1).

## Size (inference)

S1 ~150-250 lines KMD (caps, 2 QAI types, switch, init table). S3 ~150-250 lines in malikm_drm.c (+ OpenResource
paths). S5 ~100 lines. S2/S4 test-only. Total for "tracked fence works": ~2-4 sessions incl. device runs.
GpuMmu (S7): ~1500-2500 lines across KMD + winsys, multi-week.
- 2026-10-15 review: line cites re-checked with rg (adapter.c:409 coherency, command.c:326 Kick / 377 fault path,
  memory.c:301 BuildPagingBuffer) and corrected. Design sections 2-4 complete. Not committed (research task).
- 19:15 G15 S1 device run 1: KMD 1.0.47.1 (e6b762b, sys a9b8d9b7) swapped into the DriverStore (one-shot SYSTEM task), Parameters\Wddm2 = 1, reboot with KD (kd-wddm2a.log).
- 19:20 S1 run 1 result: KMD 1.0.47.1 with Wddm2=1 - DriverEntry ok, AddDevice ok, StartDevice ok (GPU up, Started 1),
  dxgkrnl's ONLY query = QAI_1 DRIVERCAPS (success, 0x240 B), then Device Manager code 43 CM_PROB_FAILED_POST_START
  (no QUERYSEGMENT4 / PHYSICALADAPTERCAPS asked). Reproduced with pnputil /restart-device under a DxgKrnl ETW session
  (all keywords, level 5): PnP START_DEVICE returns 0, then QUERY_PNP_DEVICE_STATE marks the device failed; the trace
  carries no reason event (only an AzureTriage "Adapter AddDevice has completed"). => dxgkrnl rejects the adapter
  in its post-start checks right after reading DRIVERCAPS. Reverted to Wddm2=0 (reboot).
  Next (documented/empirical only - no further static RE of OS binaries): compare our DRIVERCAPS/init table against
  the documented WDDM 2.0 requirements (Learn "WDDM 2.0 and Windows 10" feature list, DXGK_VIDSCHCAPS/preemption
  docs) and toggle candidates one per run behind Parameters (SupportPerEngineTDR, PreemptionCaps, ...).
- 19:30 S1 run 2 prep: KMD 1.0.48.1 (build 48, sys 10e5181f) - Wddm2 only: DRIVERCAPS.SupportPerEngineTDR = 1
  (Learn "TDR in Windows 8 and later": Mandatory for full graphics drivers; ResetEngine/QueryEngineStatus/
  QueryDependentEngineGroup already implemented in command.c) + PreemptionCaps DMA_BUFFER_BOUNDARY (graphics and
  compute; Learn "GPU preemption": WDDM 1.2+ KMDs use the Windows 8 preemption model). Swapped in via the SYSTEM
  task (hash checked on the device). 1.3 path unchanged.
- 19:28 S1 run 2 result: KMD 1.0.48.1 (Build 0x30 in KD) with Wddm2=1 - identical to run 1: StartDevice ok, QAI_1
  DRIVERCAPS (0x240 B, success) is dxgkrnl's only query, then code 43 CM_PROB_FAILED_POST_START. => SupportPerEngineTDR
  and PreemptionCaps are NOT the reason. Reverting Parameters\Wddm2 = 0 (reboot). Next: check the documented WDDM 2.0
  VA requirement (whether physical/"no VA" mode is accepted at all for a 2.0 KMD).
- 19:32 Learn "GPU virtual memory in WDDM 2.0" (FACT, verbatim): "Individual engines of a GPU can operate in either
  physical or virtual mode" ... "WDDM v2 supports two distinct models for GPU virtual addressing, GpuMmu and IoMmu.
  A driver must opt-in to support either or both of the models." => candidate: a 2.0 adapter has to declare
  DXGK_VIDMMCAPS.VirtualAddressingSupported + GpuMmuSupported and/or IoMmuSupported even when its engines run in
  physical mode; ours declares none. Run 3 = probe: Parameters\Wddm2Va=1 sets VirtualAddressingSupported+GpuMmuSupported;
  success criterion = dxgkrnl issues a further QAI (GPUMMUCAPS etc.) instead of code 43 straight after DRIVERCAPS.
- 19:40 S1 run 3: KMD 1.0.49.1 (9bd1376, sys 286df754) swapped in (hash checked); 1.3 boot on build 48 was Started. Parameters\Wddm2 = 2, reboot with KD.
- 19:47 S1 run 3 result: Wddm2=2 (VA+GpuMmu caps) - identical: QAI_1 DRIVERCAPS only, code 43. => the caps content does not matter; dxgkrnl rejects the 2.0 adapter before any 2.0 QAI. Next: the DRIVER_INITIALIZATION_DATA WDDM2_0 block (which 2.0 DDIs are NULL vs documented-required).
- 19:58 S1 run 4 prep: KMD 1.0.50.1 (sys 608a44ea) - Parameters\Wddm2 = 3 registers no-op CalibrateGpuClock / CreateProcess / DestroyProcess / SetStablePowerState (Learn marks none mandatory; empirical). Wddm2 = 2's VA caps NOT included.
- 20:05 S1 run 4 result: Wddm2=3 (stub CalibrateGpuClock/CreateProcess/DestroyProcess/SetStablePowerState) - identical, no stub called, code 43. Note: run 3 declared GpuMmuSupported WITHOUT the GpuMmu DDIs (SetRootPageTable/GetRootPageTableSize) - a caps-vs-DDI check right after DRIVERCAPS would fail it too, so the VA-opt-in hypothesis (Learn) is not refuted yet. Run 5 = VA caps + GpuMmu DDI stubs; risky (dxgkrnl may proceed to GpuMmu paging ops) -> probe value read from a VOLATILE key (gone after any reboot) and triggered by pnputil /restart-device, persistent Wddm2 stays 0.
- 20:15 S1 run 5 prep: KMD 1.0.51.1 (sys 5a00b79a) - Wddm2 = 4 (honoured only from the volatile Parameters\Probe key) = VA+GpuMmu caps + run-4 stubs + GetRootPageTableSize/SetRootPageTable/SubmitCommandVirtual/RenderGdi stubs. Procedure: persistent Wddm2 = 0, boot 1.3, create volatile Probe\Wddm2 = 4, pnputil /restart-device.
- 20:25 S1 run 5 result: volatile Probe\Wddm2=4 + pnputil /restart-device (DriverEntry re-ran: second Build 0x33 line) - VA/GpuMmu caps + GpuMmu DDI stubs: identical, QAI_1 only, code 43, no stub called. Volatile-probe procedure itself works (1.3 Started before, restart reloaded the image). => 5 runs: neither caps content nor the 1.3/2.0 DDI table changes the outcome. Next: Debug Print Filter (VIDEO/DEFAULT masks, documented DbgPrintEx filtering) to see whether dxgkrnl prints its reason to KD.
- 20:35 Debug Print Filter VIDEO=0xffffffff DEFAULT=0xf (reboot) + volatile probe Wddm2=4 + restart-device: code 43 again; KD shows NO dxgkrnl/dxgmms print at all (only D3D11 'Removing Device', DispBroker target removed, Mesa CREATE_BO ENOMEM from processes on the dying device). => retail dxgkrnl gives no reason on KD either. Filter removed, probe key deleted, 1.3 restored by restart-device.
- 20:45 S1 run 6 prep: KMD 1.0.52.1 (sys 56d3d000) - volatile Probe\Ver overrides the reported interface version (WDDM2_0..2_9) when Wddm2 is on; KD line 'InterfaceVersion'. Probing Wddm2=1 with Ver 0x6003 (2.1) and 0x9006 (2.4) via restart-device, no reboot.
