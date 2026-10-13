# WDDM docs research (2026-10-13, sub-agent, learn.microsoft.com only)
Question: why DWM's frames on the full MaliG76Wddm adapter never reach the panel; rebootless driver restart.
Full sub-agent report in the session transcript; key facts with sources:

## Primaries / SetDisplayMode
- UMD SetDisplayMode(DXGI) must call pfnSetDisplayModeCb with the primary allocation of hResource+SubResourceIndex;
  on D3DDDIERR_INCOMPATIBLEPRIVATEFORMAT convert + retry. In D3D11 the callback may only be called inside
  SetDisplayModeDXGI. "The allocation must be marked as a primary."
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_setdisplaymodecb
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_setdisplaymode
  -> our stub was the bug (Mesa 0028 implements it; INCOMPATIBLEPRIVATEFORMAT not handled yet).
- D3DKMTPresent rejects a Flip if "D3DKMTSetDisplayMode was not previously called" / source not primary; a Blt with a
  destination needs primaries. https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtpresent
- Primary allocations: VidPnSourceId only for primaries; KMD must not set Cached/PermanentSysMem/ExistingSysMem/...;
  write segments must be CPU-accessible. .../d3dukmdt/ns-d3dukmdt-_d3dddi_allocationinfo, .../_dxgk_allocationinfoflags
- NO_SCANOUT: "the runtime will not use flip-style presentation"; hDst may be NULL ("kernel mode will determine").
  .../dxgiddi/ns-dxgiddi-dxgi_ddi_primary_desc. Docs silent on which surface DXGI blts to.
- Docs silent on dxgkrnl's exact PRESENT_MODE_CHANGED / NOT_EXCLUSIVE_MODE_OWNER conditions for presents
  (NOT_EXCLUSIVE_MODE_OWNER documented for D3DKMTSetDisplayMode: device lacks exclusive view ownership).

## What DWM needs (WDDM 1.x)
- DWM flips, vsync-synchronized. FlipCaps.FlipIndependent must be 1 for WDDM 1.3; immediate flip + interval 1 required.
  .../d3dkmddi/ns-d3dkmddi-_dxgk_flipcaps
- SetVidPnSourceAddress: ContextCount 0 = mode switch; only primaries fitting the committed VidPN; FlipOnVSyncMmIo ->
  DIRQL, latch at next vsync. .../d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress
- CRTC_VSYNC every retrace once enabled, CrtcVsync.PhysicalAddress = the address actually displayed (non-NULL),
  then DxgkCbNotifyDpc. .../_dxgkargcb_notify_interrupt_data, .../nc-d3dkmddi-dxgkddi_controlinterrupt
- DMA_COMPLETED with the SubmissionFenceId of the latest completed DMA buffer. .../display/supplying-fence-identifiers
- Blt presents: prior work must be drainable (no present behind an unsatisfied GPU wait).
- Inferred: a flip retires on a vsync reporting its address; a blt on its DMA fence -> check both in our KMD.
- NoGPUAccess sync message: WDDM 2.0 monitored-fence flag; likely DXGI/DWM's own object, one-time; low priority.

## Rebootless stop/start (user's idea)
- PnP stop/start mandatory for WDDM 1.2+: StopDeviceAndReleasePostDisplayOwnership (cap SupportNonVGA) must hand a
  linear, CPU-mapped 32bpp frame buffer to BasicDisplay; else StopDevice. Start: DxgkCbAcquirePostDisplayOwnership.
  https://learn.microsoft.com/en-us/windows-hardware/drivers/display/plug-and-play--pnp--start-and-stop-cases
  https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkddi_stop_device_and_release_post_display_ownership
- SetPowerState(D3, shutdown) "should never fail". Triggers: pnputil /restart-device, /disable-device, /enable-device.
- Pitfalls (inferred): shutdown/stop waits for GPU idle -> missing fences/vsyncs can hang it (our full-mode restart
  hang); display-only vs full is chosen in DriverEntry (DxgkInitializeDisplayOnlyDriver vs DxgkInitialize) -> needs a
  real unload; UMD DLL stays locked until DWM recreates its device.

## Ranked fixes (from the docs)
1. Test 0028 (SetDisplayModeCb), log its HRESULT, handle INCOMPATIBLEPRIVATEFORMAT.
2. Once SetDisplayMode works, drop NO_SCANOUT (DWM is flip-based).
3. KMD SetVidPnSourceAddress: accept the DXGI primary, ContextCount==0 mode switch, latch at vsync.
4. Vsync: CRTC_VSYNC with the displayed address each vblank; FlipIndependent=1, sane MaxQueuedFlipOnVSync.
5. DMA_COMPLETED for every submission incl. present DMA buffers.
6. Audit the stop/shutdown path (SupportNonVGA, StopDeviceAndReleasePostDisplayOwnership, SetPowerState D3).
