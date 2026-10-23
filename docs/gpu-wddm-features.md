# Mali-G76 WDDM feature inventory (WDDM 2.0 - 3.2)

What each WDDM version from 2.0 to 3.2 added, which parts our Galaxy S10e driver has (KMD `drivers/MaliG76Wddm`,
UMD = Mesa d3d10umd + Panfrost built from `tools/mesa-build/patches`, `maliumd*.dll`), and what each feature could
do for this phone. Written 2026-10-23 against KMD 1.0.72.1 (`MaliG76Wddm.inf` DriverVer) on `main`.

Status values:
- **In**: implemented and used.
- **Partial**: some of it exists, for example a registry switch that is off by default, a stub, or an emulation.
- **Not impl.**: not implemented.
- **N/A**: does not apply to this hardware or OS.

Evidence is `file:line` in `drivers/MaliG76Wddm/` unless another path is given.
Feature lists come from Microsoft Learn only (sources below). No Windows binary was examined (AGENTS rule 11).

## What we report today

| Item | Value | Evidence |
|---|---|---|
| Default mode (INF install) | display-only KMDOD, `DXGKDDI_INTERFACE_VERSION_WIN8`; full adapter only with `Parameters\GpuEnable 1` | `MaliG76Wddm.inf:8-11`, `adapter.c:991` |
| Full adapter, `Parameters\Wddm2 = 0` | interface `DXGKDDI_INTERFACE_VERSION_WDDM1_3`, `DXGK_DRIVERCAPS.WDDMVersion = DXGKDDI_WDDMv1_3` | `adapter.c:421`, `adapter.c:1026-1028` |
| Full adapter, `Parameters\Wddm2 = 5` (**the phone's setting**, HANDOFF.md:1588) | interface `DXGKDDI_INTERFACE_VERSION_WDDM2_0` (0x5023), `WDDMVersion = DXGKDDI_WDDMv2` (0x2000): dxdiag shows "WDDM 2.0" | `adapter.c:421`, `adapter.c:955`, `adapter.c:965-971` |
| Probe-only override | `Parameters\Probe\Ver` can report 2.1-2.9 (0x6003-0xE003). Runs 6 and 10 did not keep 2.x > 2.0 working | `adapter.c:37`, `notes/gpu-wddm2-design.md` runs 6-10 |
| Engines | node 0 = Mali job manager (3D, **physical** mode, allocation and patch lists); node 1 = software "CPU copy" node (GpuMmu paging node, page-table operations are no-ops) | `command.c:917-935`, `adapter.c:511-527` |
| UMD DDI | D3D10/10.1/11.0/11.1/WDDM1_3/**WDDM2_0** D3D11 DDI, feature level **11_0** (G34), D3D11 video DDI (H.264 decode on the MFC); **no D3D12 UMD** | `tools/mesa-build/patches/0071-d3d11-ddi-wddm2.patch`, `0083-0091`, `0218-fl11-default.patch` |
| Highest WDDM the phone's OS accepts | Windows 11 22621/22631 = **WDDM 3.1** (23H2 "WDDM version didn't change"). 3.2 needs 24H2 | `notes/gpu-wddm3-research.md` Q5 |

Note: `Wddm2 = 5` is set through the registry (`tools/phone/wddm2/w2set.ps1`), not by the INF. A fresh install comes up as
display-only, then WDDM 1.3 once `GpuEnable` is set. `DXGK_DRIVERCAPS.WDDMVersion` is a load gate: dxgkrnl validates the
caps and DDIs against it. Claiming a version means implementing *all of that version's mandatory items and those of
every earlier version* (Learn [wddm-2-1-features](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-2-1-features),
"Determining WDDM Version").

## Sources (learn.microsoft.com)

- WDDM overview and version table: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-display-driver-model-design-guide
- 2.0/2.1/2.5/2.6: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-prior-wddm-2-x-versions
- 2.7: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-windows-10-display-and-graphics-drivers
- 3.0/3.1: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/what-s-new-for-windows-11-display-and-graphics-drivers
- 3.2: https://learn.microsoft.com/en-us/windows-hardware/drivers/driver-changes-for-windows-11-version-24h2
- `DXGK_FEATURE_ID`: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ne-d3dukmdt-dxgk_feature_id
- Hardware flip queue: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/hardware-flip-queue
- Required/optional caps per version (HLK WHCP 22H2 and Learn caps tables) are already summarised in `notes/gpu-wddm3-research.md` Q1.

Key quotes:
- 2.0: "Each process gets its own GPU virtual address space"; "The video memory manager ensures that allocations are
  resident in memory before submitting command buffers" (prior-2.x page).
- 2.6: "Collect diagnostic info ... This new feature is a requirement in WDDM 2.6" (prior-2.x page).
- 3.0 HW flip queue: "allows the CPU and parts of the GPU to transition to lower power states while the display
  controller is processing multiple queued frames, improving power efficiency of video playback" and "Windows
  currently requires GPU hardware scheduling to be enabled in order for basic hardware flip queue to be enabled on
  officially released drivers" (hardware-flip-queue).
- 3.2 native fence: "Conditional interrupt notification only for GPU fence signals that have CPU waiters, enabling
  substantial power savings" (24H2 page). User-mode submission "is an in-progress feature that isn't yet enabled for
  final use" (same page).

## Baseline inherited from WDDM 1.x (still required of a 2.x full-graphics driver)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Per-engine TDR (ResetEngine, QueryEngineStatus, QueryDependentEngineGroup) | In | `adapter.c:455`, `command.c:940-955`, `command.c:1026` | A hung Mali job resets the GPU without killing the desktop. |
| Preemption caps | Partial | `adapter.c:461` (DMA_BUFFER_BOUNDARY); `command.c:854` no-op; `EnablePreemption 0` written by `esp/extra-gpu-g10.cmd:72` | A game frame can block DWM until its job chain ends, which shows as stutter. Mali JM soft-stop could give job-chain-boundary preemption. |
| Flip on VSync via MMIO | Partial | `adapter.c:432`; `display.c:1054` latches a **CPU copy** into the GOP framebuffer at a 16 ms **software** vsync (`command.c:605-623`) | Every frame costs a full-screen copy and the vsync is not the panel's. A real DECON base-address flip would remove the copy (battery, smoothness). |
| DirectFlip (`SupportDirectFlip`, segment `Flags.DirectFlip`) | Partial | `adapter.c:65`/`436`, `memory.c:43`/`76`; default 0 (phone: 1 per HANDOFF.md:1588); UMD `CheckDirectFlipSupport` answers FALSE (`adapter.c:61-62`) | Fullscreen apps and video could skip DWM composition (one fewer full-screen pass), but only once flips are real scan-out changes. |
| Independent flip (`FlipCaps.FlipIndependent`) | Partial | `adapter.c:74`/`434`, default 0 | Same benefit as DirectFlip, applied to windowed apps that cover the screen. |
| CDD/DWM interop | In | `adapter.c:443` | Needed so that windowed D3D presents reach DWM (H4c). |
| Multiplane overlay (WDDM 1.3) | Not impl. | no `*MultiPlaneOverlay*` DDI in `adapter.c:1032-1104` | DECON has several hardware windows (count UNVERIFIED). Video or game planes could scan out with no composition (battery, browser video). |
| Dirty rects | Partial | display-only path `display.c:1012`; full path passes `pDstSubRects` to the present copy (`command.c:184-186`) | The present copies only the changed rectangles. Full-mode flips still copy the whole frame. |
| Brightness interface v2 | In | `panel.c:596-620` | Windows brightness slider and fades. |
| Runtime power management (F-states, `DxgkDdiSetPowerComponentFState`) | Not impl. | not in `adapter.c` DDI table | The GPU could power-gate at idle (battery). Clocks are handled separately by the G19 ACPM driver. |

## WDDM 2.0 (Windows 10 1507): what we claim today

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| GPU virtual addressing, **GpuMmu** model | Partial | `adapter.c:457-458`, `adapter.c:528-548` (GPUMMUCAPS: 4 levels, 48-bit, CPU_VIRTUAL), `adapter.c:549-565`; real GPU work is on **physical** node 0 with `AccessedPhysically` allocations (`memory.c:134`); node-1 page-table operations are no-ops (`memory.c:447-458`) | A per-process Mali address space (8 AS slots) would remove KMD patching and copying of DMA buffers. That means less CPU time per draw call (games) and isolation between apps. |
| IoMmu model | N/A | never set | The Mali has its own MMU; the SMMU path is not used for the GPU. |
| Per-process GPU VA (CreateProcess/DestroyProcess, SetRootPageTable, GetRootPageTableSize) | Partial (stubs) | `adapter.c:1078-1087` | Needed for true GpuMmu, see the row above. |
| `SubmitCommandVirtual` (VA DMA buffers) | Partial (stub) | `adapter.c:1086` | The UMD would submit job chains in place, with no `Render`/`Patch` copy. |
| Driver residency (MakeResident/Evict, paging queue, TrimResidencySet) | In (UMD) | `tools/mesa-build/patches/0069-wddm2-residency.patch`; `Dxgi_TrimResidencySet` is a no-op (`0071` patch:709); KMD `NOTIFY_RESIDENCY` accepted (`memory.c:459`) | Required by 2.0. A real trim would let a backgrounded game give memory back (6-8 GB phone). |
| Monitored fences / context monitoring | In (CPU-signalled) | the UMD uses `pfnSignalSynchronizationObject2Cb` (`0105-wait-bo-per-bo.patch:40`); the KMD reports only `DMA_COMPLETED` (`command.c:397`), no `MONITORED_FENCE_SIGNALED` interrupt | DXGI tracked fences work, which fixed Terminal and DComp swap chains (G15 S4). GPU-written fences would cut CPU wakeups. |
| Lock2 / CPU-visible-on-demand | Partial | `memory.c:130` `CpuVisible`; `CpuVisibleOnDemand` behind a switch, default 0 (`adapter.c:73`, `memory.c:145`) | Saves process address space for 32-bit games (NFSU2). |
| Allocation-list-free submission (`NoPatchingRequired`) | Partial | set only for VA contexts (`command.c:61`); Mali contexts still patch (`command.c:64-68`, `command.c:376`) | Smaller submissions and less KMD work per frame. |
| 64-bit fence atomics (`DXGK_VIDSCHCAPS.No64BitAtomics`) | Not impl. | not set | Only matters once the GPU writes fences. |
| Tiled resources (D3D11.2; HLK "Mandatory" at 2.0) | Not impl. | UMD stubs `D11Stub_UpdateTileMappings` etc. (`0071` patch:1345-1375) | Sparse textures for a few games. Low value on this GPU. |
| D3D12 UMD | Not impl. | no D3D12 entry points in any patch; all four `UserModeDriverName` slots are `maliumd.dll` (`MaliG76Wddm.inf:79`) | D3D12-only games and the Edge/Chrome Graphite rasteriser (G16) would need it. A big job (Mesa has no D3D12 frontend; see "Top candidates"). |
| Preemption granularity reported | In (DMA buffer) | `adapter.c:461-462` | See the 1.x row. |
| `CalibrateGpuClock` / `SetStablePowerState` | Partial (stubs) | `adapter.c:1078-1081` | Load gates (run 9). A real calibration would give correct GPU timestamps for profilers and D3D11 timestamp queries. |

## WDDM 2.1 (Windows 10 1607): not claimed

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Offer/Reclaim improvements (mandatory if 2.1) | Not impl. | no Offer/Reclaim callbacks in the UMD patches | Background apps give up GPU memory; matters on a 6-8 GB phone. |
| 64 KB pages / 2 MB PTE (optional) | Not impl. | GPUMMUCAPS leaves 64K leaf size 0 (`adapter.c:540-545`) | Fewer TLB misses for big textures. Mali LPAE 64 KB support is UNVERIFIED. |
| Present batching / GameDVR `SyncToken` (mandatory if 2.1) | Not impl. | not in the UMD | Game recording. Low value. |
| `MaxQueuedMultiPlaneOverlayFlipVSync` (flip-queue latency) | Not impl. | not set in `adapter.c:412-465` | Lower latency with interval-0 games. Needs real flips first. |
| Driver Store install (mandatory if 2.1) | In | INF uses `%13%` (DriverStore) `MaliG76Wddm.inf:107` | Safe updates. |
| NV12 cross-process camera sharing (mandatory if 2.1) | Partial | NV12 plane views exist (`0221-nv12-plane-views.patch`); shared-handle NV12 has no planes (MILESTONES G35 follow-ups) | Camera apps; Teams/Edge video capture. |
| Shader Model 6.0 / DXIL | N/A | D3D12-only | Needs D3D12. |
| HW protected content (PlayReady 3) | N/A | MFC secure path not exposed | Netflix HD. Out of scope. |

## WDDM 2.2 (Windows 10 1703)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| MPO3 / `SetVidPnSourceAddressWithMultiPlaneOverlay3` (HLK "must" at 2.2) | Not impl. | no DDI | Overlay planes for video and games (battery). This is the base for the 3.0 HW flip queue. |
| Periodic frame notification | Not impl. | no DDI | Lets DWM align frames with the panel. Smoothness. |
| `SetTimingsFromVidPn`, display wire format, 4HWID, reduced black screen | Not impl. | `CommitVidPn` path only (`display.c:854`) | Faster mode switches with no black flash (G18 HDMI). |
| `DXGK_INTERRUPT_MONITORED_FENCE_SIGNALED` | Not impl. | `command.c:397` only DMA_COMPLETED | GPU-signalled fences (power, latency). |
| VR/MR features | N/A | | |

## WDDM 2.3 / 2.4 (Windows 10 1709 / 1803)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| HDR / advanced colour, colour space transforms | Not impl. | no `DXGK_COLORIMETRY` or HDR caps; panel EDID is synthesised SDR (`display.c:436`) | The AMOLED panel is wide-gamut, so colour-correct sRGB would be possible. HDR video is low value. |
| GPU paravirtualization (GPU-P) | N/A | | VMs/WSL on a phone. Out of scope. |
| IOMMU GPU isolation | N/A | the Mali MMU is used | |
| `CrossAdapterResource` cap (HLK at 2.4) | Not impl. | not in DRIVERCAPS | IDD (S10eIdd, IddCx 1.4-1.6) and Basic Render sharing. |
| UMD->KMD private data fuzz robustness | Partial | escapes check size (`adapter.c:645` MwEscape); the job-chain contents are not validated | Security hardening. |

## WDDM 2.5 (Windows 10 1809)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Hardware queues, `DxgkDdiSignalMonitoredFence` | Not impl. | not in DDI table | A prerequisite for HWS (2.7). |
| Display synchronization (integrated display descriptor 2) | Not impl. | | Panel timing reporting. Low value. |
| DXR raytracing | N/A | D3D12 + hardware | |

## WDDM 2.6 (Windows 10 1903)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| `DxgkDdiCollectDiagnosticInfo` (**required** at 2.6) | Not impl. | only `CollectDbgInfo` (`command.c:903`) | Better TDR dumps. Cheap to add, and it is a gate for claiming 2.6+. |
| Super Wet Ink (front-buffer scan-out transforms) | Not impl. | | Pen latency. N/A (no pen). |
| Variable rate shading | N/A | D3D12 + hardware | |
| Background processing (D3D12 UMD) | N/A | | |
| Driver hot update | Not impl. | | Swap the KMD without a reboot. Would speed up development, but has risks. |

## WDDM 2.7 (Windows 10 2004)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Hardware-accelerated GPU scheduling (HWS) | N/A (hardware) | Mali-G76 is a Bifrost **job manager** GPU with no firmware scheduler (INFERENCE: firmware CSF arrived with later Valhall parts) | Would have meant lower scheduling latency, but there is no firmware to run the queue. |
| Mesh shaders, DXR 1.1, sampler feedback | N/A | D3D12 | |
| DP AUX/I2C interface | Not impl. | G18 P3 (HDMI/DP alt mode) is in progress | External display EDID and link over USB-C. |

## WDDM 2.8 / 2.9 (Windows 10 20H2 / Server 2022)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Virtual refresh rate (2.9, quoted in hardware-flip-queue) | Not impl. | single 60 Hz mode (`display.c:637-700`) | 24/30 Hz video at a matched cadence (smoother browser video). The panel is fixed 60 Hz: UNVERIFIED whether DECON/panel can drop rate. |

## WDDM 3.0 (Windows 11 21H2)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Hardware flip queue (`MaxHwQueuedFlips`, `DxgkDdiCancelFlips`, flip queue log) | Not impl. | | Queues decoded video frames to the display so the CPU sleeps, which is the doc's own example of video-playback power saving. Needs MPO3 and real flips. Learn says released drivers also need HWS, which we cannot do (2.7). |
| Signal a CPU event from KMD (`DXGK_FEATURE_KMD_SIGNAL_CPU_EVENT`) | Not impl. | | Wakes a UMD thread directly from the job IRQ instead of polling. This could replace `wddm_idle` polling in the malikm shim (lower CPU use). |
| IOMMU DMA remapping | N/A | | |
| D3D12 enhanced barriers, D3D12 video encode | N/A | no D3D12 UMD; MFC encode is not exposed | MFC H.264 *encode* through a D3D11/MFT path would serve camera apps and Teams (not a WDDM item). |
| USB4 DisplayPort | N/A | no USB4 | |
| WSL GPU (DXCore UMD) | N/A | | |

## WDDM 3.1 (Windows 11 22H2/23H2, the phone's maximum)

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Share backing store with KMD (`DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD`) | Not impl. | | The KMD reads UMD allocations without a mapping. Low value. |
| `DXGK_VIDSCHCAPS.NativeGpuFence` caps (header-level 3.1) | Not impl. | | Groundwork for 3.2 native fences. |
| Shader Model 6.7 | N/A | D3D12 | |

## WDDM 3.2 (Windows 11 24H2): not available on 22621/22631

| Feature | Status | Evidence | What it does / could do here |
|---|---|---|---|
| Feature query DDI (`DxgkCbQueryFeatureSupport`, `DXGK_FEATURE_*`) | Not impl. | | The opt-in mechanism for all 3.x features. |
| Native GPU fence (`DXGK_FEATURE_NATIVE_FENCE`) | N/A until 24H2 | | GPU-to-GPU waits and interrupts only when a CPU waiter exists ("substantial power savings"). |
| User-mode work submission | N/A | "isn't yet enabled for final use" | |
| Dirty bit tracking, GPU-P live migration | N/A | | |
| Kernel-mode testing, enhanced TDR debugging, allocation notification | N/A until 24H2 | | |
| AV1 encode, work graphs, SM 6.8 | N/A | D3D12 | |
| Other `DXGK_FEATURE_ID`s (`SUPPRESSVSYNC_INTERRUPTS`, `PANEL_BUFFER_CONTROL`, `PAGE_BASED_MEMORY_MANAGER`, `OPPORTUNISTIC_64KB_PAGES`, ...) | N/A | listed in the enum page; most have no Learn feature page | `PANEL_BUFFER_CONTROL` (panel self refresh) is interesting for a command-mode AMOLED (G14), but undocumented: do not use. |

## Top candidates to implement next (benefit / effort)

1. **Real scan-out flips on DECON** (program the window base address at the hardware vsync instead of the CPU copy
   plus 16 ms software vsync; `display.c:1054`, `command.c:605`). This removes a full-frame copy every frame,
   which helps battery, smoothness and video. Effort: medium (DECON is partly mapped from G14/G18; window-address registers UNVERIFIED). It is the
   prerequisite for 2-5.
2. **`DxgkDdiCollectDiagnosticInfo` + `CalibrateGpuClock` for real.** Cheap. Better TDR dumps, correct GPU
   timestamps. It also removes two gates on the road to claiming a 2.x version above 2.0.
3. **DirectFlip / independent flip on by default** (UMD `CheckDirectFlipSupport` true for scan-out-compatible
   buffers). Once item 1 is in, fullscreen games and video skip DWM composition. Effort: low after item 1.
4. **Real GpuMmu on the Mali node** (per-process AS from `SetRootPageTable`, AS-slot multiplexing over 8 slots,
   `SubmitCommandVirtual`, `NoPatchingRequired`). This removes Render/Patch copies and patch lists, cutting CPU per
   draw call, and gives isolation. Effort: high (the stubs and 4-level LPAE tables exist, `notes/gpu-wddm3-research.md` Q7).
5. **Multiplane overlay (MPO2/MPO3) on DECON windows.** Video and browser video planes scan out without composition,
   which is the largest battery item for video. It also unlocks claiming 2.2 and the 3.0 flip queue. Effort: high
   (VidPN plane caps, per-plane flips, CSC for NV12 if DECON supports it, UNVERIFIED).
6. **GPU-signalled monitored fences** (Mali WRITE_VALUE job to the fence VA plus `MONITORED_FENCE_SIGNALED`
   interrupt; or 3.0 KMD-signalled CPU events). Fewer CPU wakeups and lower frame latency. Effort: medium, needs item 4
   for the fence's GPU VA.
7. **Offer/Reclaim and real `TrimResidencySet`.** Memory goes back from background apps, so fewer out-of-memory
   failures in 32-bit games. Effort: medium (UMD only).
8. **Runtime power management (F-states) for the GPU.** The G3D domain powers off at idle (battery). Effort: medium;
   it overlaps G19.

Not worth chasing on this hardware: HWS and the HW flip queue as shipped (no firmware scheduler), GPU-P, IOMMU
remapping, everything D3D12-only unless a D3D12 UMD is started (a separate, large project; Learn makes D3D12 optional
for WDDM 2.0: "Graphics drivers can optionally implement WDDM 2.0 if they do not support D3D12", HLK quote in
`notes/gpu-wddm3-research.md` Q1).
