# G18 P4 - the external display as a second WDDM target in MaliG76Wddm (design)

Part of notes/g18-hdmi-prep.md. Hardware side: notes/g18-p3-dp-bringup.md (DP controller + DECON_T), H2 alt mode
(notes/g18-h2-altmode-research.md). This note is the Windows-side design; nothing here is implemented yet.

## Today (drivers/MaliG76Wddm, read 2026-10-20)
- adapter.c:296-297 StartDevice: `*Sources = 1; *Children = 1;`.
- display.c:349 MwQueryChildRelations: one child, UID 0, D3DKMDT_VOT_INTERNAL, HpdAwarenessAlwaysConnected.
- display.c:367 MwQueryChildStatus: StatusConnection -> always TRUE.
- display.c:436 MwQueryDeviceDescriptor: one synthetic EDID (BuildEdid, the panel's GOP mode), ChildUid ignored.
- VidPN (display.c:457-966): "one source, one target, one mode, identity everything" - VidPnSourceId 0 /
  VidPnTargetId 0 hard-coded (RecommendFunctionalVidPn :589-590, IsSupportedVidPn :493), CommitVidPn programs the
  one scan-out (DPU DMA G0 under DECON_F). SetVidPnSourceAddress :1054 flips the panel's IDMA.

## MS Learn (rule 13) - what the DDI requires
- DxgkDdiStartDevice, NumberOfChildren: "This count must include potential child devices as well as child devices
  that are currently present. For example, if docking a portable computer will make additional video outputs
  available, those video outputs must be counted regardless of whether the portable computer is currently docked."
  NumberOfVideoPresentSources: "the number of video present sources supported by the display adapter."
  (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_start_device)
  -> the DP output is counted from the first boot, connected or not; the counts never change at run time.
- DxgkCbIndicateChildStatus: "The display miniport driver's DPC for ISR calls DxgkCbIndicateChildStatus when ... An
  external device (typically a monitor) has been connected to one of the display adapter's child devices that has an
  HPD awareness value of HpdAwarenessInterruptible. In this case, the display miniport driver sets ChildStatus->Type
  to StatusConnection and sets ChildStatus.HotPlug.Connected to TRUE." (and FALSE on disconnect); IRQL <=
  DISPATCH_LEVEL. (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkcb_indicate_child_status)
  -> our HPD source is not a GPU interrupt but the max77705 Attention VDO (polled by S10eBattery / a future CCIC
  driver); the KMD calls DxgkCbIndicateChildStatus from a work item / DPC when it learns of the change - the callback
  is allowed up to DISPATCH_LEVEL, so a passive-level caller is fine (INFERRED: "DPC for ISR" describes the typical
  caller, not a requirement - the IRQL row is the requirement).

## Design
### Topology
- 2 video present sources (0 = panel, 1 = external), 2 children:
  - UID 0: unchanged (internal DSI panel, DECON_F, always connected).
  - UID 1: D3DKMDT_VOT_DISPLAYPORT_EXTERNAL, HpdAwarenessInterruptible, MonitorOrientationAwareness NONE.
- Paths allowed: (0 -> 0), (1 -> 1), and clone (0 -> 0 + 0 -> 1). Not allowed: source 1 on the panel or a source on
  two targets other than clone of 0. Clone needs the external target rotated relative to the panel (panel is
  portrait-first: clone path ContentTransformation Rotation Offset270 / Rotate90 - notes/g18-hdmi-dp-alt.md).
  First milestone = extend only (1 -> 1); clone after it.

### Child status / EDID
- KMD state: DpConnected (bool), DpEdid[256] + length, DpModes (parsed DTDs / CEA VICs we accept: 1920x1080p60 VIC 16
  first, then 1280x720p60 VIC 4, 640x480p60 VIC 1 as the always-safe fallback).
- MwQueryChildStatus(UID 1) -> DpConnected. MwQueryDeviceDescriptor(UID 1) -> the real EDID read over AUX (P3);
  STATUS_MONITOR_NO_DESCRIPTOR while none. UID 0 keeps BuildEdid.
- Who tells the KMD: a private IOCTL / escape on the KMD (set connected + EDID) driven by the CCIC side first (user-
  mode service gluing S10eBattery's alt-mode events to the KMD), later an in-kernel interface between the two drivers.
  HPD low -> DpConnected FALSE + DxgkCbIndicateChildStatus(FALSE) before tearing down the link.

### VidPN
- Every function that hard-codes source/target 0 becomes per-path: IsSupportedVidPn accepts the allowed paths above;
  EnumVidPnCofuncModality fills source mode sets per source (source 1 = DP modes) and target mode sets per target
  (target 1 = the EDID-derived modes); RecommendFunctionalVidPn stays panel-only (Windows adds the external path
  itself on hot plug through the monitor's preferred mode).
- CommitVidPn per source: source 0 -> existing DECON_F/G0 path; source 1 -> DP link up (if not), DECON_T + its DMA
  channel programmed for the pinned mode, AFTER link training succeeded (P3 section 5); a commit that removes path 1
  stops DECON_T video and drops the link.
- SetVidPnSourceAddress(source 1) -> DECON_T's IDMA base flip, vsync from DECON_T frame-done (INTREQ__DPU_DECON2_*
  195/196, d6 P3) -> DxgkCbNotifyInterrupt VSYNC for source 1.
- Display-only mode (MwModeDisplayOnly) keeps one source (no change) - only the full adapter grows.

### Order of implementation (each a commit, each validated on the device)
1. P4b (can be done before any DP hardware works): counts 2/2, child UID 1 reported but always disconnected
   (QueryChildStatus FALSE, no descriptor). Validation: desktop unchanged, Display settings still one monitor, dxgkrnl
   accepts the second child (no Code 43, no extra commits with zero paths in the PRAM trace).
2. P4c (still no DP hardware): a diagnosis-only registry value Parameters\SimDpMonitor=1 makes child 1 "connected"
   with a fixed 1920x1080p60 EDID and no scan-out (source 1 frames go nowhere). Validates the multi-source VidPN code:
   Windows extends the desktop to a phantom monitor, the VidPN commits two paths, SetVidPnSourceAddress arrives for
   source 1. Rule 12: diagnosis switch only, off by default, never a delivered behaviour. RISK: the shell may move
   windows to the phantom monitor; the test script turns it off again (registry + restart device or reboot).
3. P4d (needs H2 + P3 working): real HPD/EDID from the CCIC + AUX, DECON_T programming, link training.
4. Clone (Offset270).

## Hazards
- Changing NumberOfChildren/Sources changes what dxgkrnl stores for the adapter (monitor DB); roll back = previous
  KMD package (kmdswap / TWRP driver swap, notes on driver-swap-from-twrp).
- DECON_T / DP block registers must never be touched while the DPU domain / clocks are off (P3 section 1).
- Any DP-PHY lane write is a USB-host boot hazard (combo PHY shared with USB3, d6 P2).
