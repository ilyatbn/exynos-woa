# G20 - lower (scaled) desktop resolutions, e.g. 720x1520 on the 1080x2280 panel

User 2026-10-17: "yes to attempting to add a lower 720p resolution scale to see if that helps later in games".

## Facts so far
- The panel (S6E3FA7, command-mode DSI) runs only at its native 1080x2280; MaliG76Wddm exposes ONE source mode and
  one target mode (display.c header: "one source, one target, one mode, identity transforms").
- Present path today: DWM's 1080x2280 primary -> UMD d3d10_scanout_present GPU blit into the scan-out BO (1:1), or
  the rotated path (landscape), or the KMD CPU copy (DoPresent). A smaller source needs a stretch in that blit.
- Candidate mode: 720x1520 (exactly 2/3 of 1080x2280, same 19:9 aspect).

## Plan
1. MS Learn: how a WDDM driver offers a source mode smaller than the target (VidPN present path scaling,
   D3DKMDT_VPPS_*, source/target mode sets in EnumVidPnCofuncModality / IsSupportedVidPn / CommitVidPn).
2. KMD display.c: add the scaled source mode(s) + path scaling support.
3. UMD d3d10_gdi.c: stretch blit (LINEAR) when the primary is smaller than the scan-out; KMD DoPresent fallback.
4. Test with nogpu.flg recovery ready (a VidPN mistake can leave the display black at boot).

## Progress log
- MS Learn D3DKMDT_VIDPN_PRESENT_PATH_SCALING (https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/
  ne-d3dkmdt-_d3dkmdt_vidpn_present_path_scaling): "D3DKMDT_VPPS_IDENTITY ... available only if the path's source mode
  has the same spatial resolution as the path's target mode"; "D3DKMDT_VPPS_STRETCHED Indicates that the source content
  is scaled to fit the path's target"; ASPECTRATIOCENTEREDMAX keeps the aspect (Win7+); "If a driver cannot support
  the requested scaling value on the specified path, its calls to DxgkDdiCommitVidPn should return
  STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED". => a 720x1520 source on the 1080x2280 target needs the path's
  ScalingSupport to include Stretched (and/or AspectRatioCenteredMax - same aspect, same picture).
- MS Learn "Querying VidPN Hardware Capabilities" (https://learn.microsoft.com/en-us/windows-hardware/drivers/display/
  querying-vidpnhardware-capabilities): table row "Hardware cannot perform cloning, scaling, or rotation
  transformations. These operations are performed by the driver." -> DriverRotation=1, DriverScaling=1. So the OS does
  NOT scale for us: whichever way it is reported, the driver produces the scaled picture (HW plane scaler or an
  intermediate blit). Our case = driver blit, report DriverScaling=1 on a scaled path (and DriverRotation=1 when rotated,
  matching what our rotate blit already does). "the driver is required to set ... Reserved ... to 0".
- Code read 2026-10-17 (persisted before coding):
  - display.c: FillSourceMode = Fb w/h/pitch; IsSupportedVidPn + CommitVidPn accept only Fb-sized pinned source;
    AddSourceMode adds one mode; Cofunc ScalingSupport = Identity only; Recommend pins native identity;
    QueryVidPnHWCapability zeroes everything.
  - memory.c: shared primaries are sized from pCreateSharedPrimarySurfaceData (720x1520 primary = 720x1520 alloc, ok).
    MwEscapeScanout reports the Fb (scan-out) size to the UMD - unchanged (the scan-out stays 1080x2280).
  - command.c DoPresent: 1:1 copies clipped to the Fb (ToScanout flips + Blt/Fill to a primary). Latch (MMIO flip at
    vsync, DISPATCH) calls DoPresent with the whole primary. GpuPresented primaries skip the CPU copy.
  - Mesa d3d10_gdi.c d3d10_scanout_present: rotated path draws a full-target quad (scales by construction, NEAREST);
    unrotated path = pipe->blit with dst box == src box, refused (CPU path) unless res == scan-out size.
- Design:
  - KMD, Parameters\ScaledModes (default 0, full adapter only): source mode set = native (Fb pitch) + 2/3 size
    (720x1520, stride w*4). Cofunctional: scaling pinned STRETCHED/ARCM -> only the scaled mode; IDENTITY pinned -> only
    native; source pinned scaled -> ScalingSupport {Stretched, AspectRatioCenteredMax}; pinned native -> {Identity};
    nothing pinned -> all three. Commit stores SrcW/SrcH. HWCapability DriverScaling=1 on a scaled path (rotation bit
    left as before - out of scope). DoPresent: nearest-neighbour scale when the primary is smaller than the Fb.
  - UMD: unrotated blit -> dst box = whole scan-out, LINEAR filter when sizes differ; rotated quad -> accept smaller
    res, LINEAR when scaled.
- KMD 1.0.67.1 built (s10e-artifacts\mali-kmd-67, sys 9aa5ab58): Parameters\ScaledModes (default 0) - display.c
  ScaledOn/SourceKind/PinnedSourceKind, AddSourceMode cofunctional with the path scaling, Cofunc scaling support by
  pinned source, Commit stores SrcW/SrcH (MwLog CommitSrcWxH), HWCapability DriverScaling=1 on a scaled path;
  command.c ScalePresent (nearest, CPU fallback; gated on ScaledModes). Device test pending (needs the UMD stretch too,
  else GPU-presented frames of a 720x1520 primary fall to the CPU path = still visible via ScalePresent).
- Review of ab0ebf2 (KMD):
  - checked: ScaledModes=0 keeps the old behaviour - SourceKind 1 == the old size test, AddSourceMode adds native only
    (addScaled needs ScaledOn), Cofunc support Identity only, HWCaps zero (ScaledOn gate), DoPresent scale gated.
  - checked: GpuPresented freeze risk - Mesa malikm_primary_present marks a primary GPU-presented only on the GPU
    path (bgrx NULL); a refused (size-mismatch) frame goes present_copy -> KMD DoPresent -> ScalePresent. No freeze.
  - checked: ScalePresent bounds - source row y*VH/fh in [top, bottom) for y in [ceil(top*fh/VH), ceil(bottom*fh/VH)),
    likewise x; r clipped to the primary and source, so indices stay inside.
  - FOUND: IsSupportedVidPn accepts a pinned scaled source on a path whose scaling is pinned IDENTITY (invalid per the
    MS Learn sentence above). -> follow-up commit.
  - NOTE (for the UMD task): the rotated GPU present passes so->width0/height0 to malikm_primary_present, which must
    equal the kernel primary (source mode, unrotated) size; with a scaled mode that is (res->height0, res->width0),
    not the scan-out size.
  - SrcW/SrcH are only logged (CommitSrcWxH), DoPresent uses the primary's own size - stated in the header comment.
- Follow-up (review of ab0ebf2): IsSupportedVidPn rejects a scaled pinned source with an IDENTITY path for source 0.
  KMD 1.0.67.1 rebuilt (s10e-artifacts\mali-kmd-67, sys b6012665).
- Review of d7bfd3f: loop ends on NO_MORE_ELEMENTS (!= SUCCESS -> next NULL); empty topology skips. Nothing found.
- Mesa 0103 (511cf83, patches/0103-scaled-mode-stretch.patch): d3d10_scanout_present accepts a primary no larger than the
  scan-out with the same aspect: plain blit dst box = whole scan-out, LINEAR when sizes differ; rotating quad LINEAR
  when scaled. Caught while writing it: (1) a single "fits" using transposed scan-out dims for rotated modes would
  have sent landscape to the CPU path - DWM pre-rotates into a 1080x2280 primary marked rotation 2
  (notes/gpu-hw-path.md 19:01), the plain blit is the live landscape path; split into fitsN (plain) / fitsT
  (transposed quad, practically unused). (2) malikm_primary_present checks the kernel primary = the presented
  resource's own size (Resource.cpp:485 malikm_primary_create(templat.width0, templat.height0)), so the quad branch
  now passes res dims (was so dims - equal for rot 3, and the 2/4 branch never ran).
  Built: s10e-artifacts\mali-umd-0103 (maliumd 8abbced3), mali-umd-0103-x86 (maliumd32 3ff1b027).
- Review of 8717649 / Mesa 0103: no other primary-vs-scan-out size check in malikm_drm.c (only the scan-out BO import, 1758-1769); panfrost blit = util_blitter_blit (scaled blits supported). Nothing found.
- Deployed (Windows live, 2026-10-17 ~20:35): UMD 0103 ARM64 via wddm2dbg.sh umdswap (8abbced3, loads at next boot),
  x86 maliumd32 3ff1b027 into SysWOW64 (old renamed .old); Parameters\ScaledModes = 1 (inactive until KMD 67 boots).
  KMD 67 still to swap from TWRP (kmd-deploy.sh).
- Test tool: drivers/S10eDiag/S10eMode.c (s10emode list | WxH [save] | native [save]; NOT saved by default, so a reboot
  returns to the saved mode) + tools/phone/g20/mode.sh (runs it in the user's session). Baseline on KMD 66:
  "current 2280x1080 @60 Hz orientation 1", one mode (landscape; EnumDisplaySettings lists modes in the current
  orientation - the "native" swap was removed for that).
- 20:27 KMD 67 (b6012665) deployed from TWRP via totwrp.cmd + kmd-deploy.sh (prev 7647eda9 in C:\S10eTmp\kmd-prev). Rebooting to Windows.
- 20:33 KMD 67 booted clean (ScaledModes 1). s10emode list: 2280x1080 + 1520x720 (x2, landscape). Native scan-out dump OK (traces/wddm2/g20native.png; first dump black = screen timed out -> tools/phone/g20/wake.ps1).
- 20:34 **720p scaled mode WORKS on the device**: s10emode 1520x720 (landscape, not saved) -> "test 0 -> 0"; scan-out
  dump traces/wddm2/g20scaled.png = the desktop at 1520x720 stretched over the whole panel, right orientation,
  colours right (icons same as native; the wallpaper itself changed). DWM runs UMD 8abbced3 (0103); kmdstats on the
  idle desktop: presents gpu 0.2/s, cpu copy 0.0 -> the GPU stretch path (0103 plain blit), not the KMD CPU scale.
  Not yet seen: the user's eyes, portrait 720x1520, a game at 720p, the Settings > Display resolution list.
- 20:38 Game Dev Story fps comparison (user: "launch game dev story and check if theres a difference in fps before
  and after"), tools/phone/g20/gamebench.sh (raw: traces/wddm2/g20game*.txt/png). 720p (1520x720, maximized window,
  gameplay): KMD presents 26.6 / 25.0 / 23.4 per s (all GPU), renders ~420/s; KairoGames GPU 3D 26.6 %, DWM 3.3 %;
  CPU cpu4 68.8 / cpu5 42.1 / cpu0 34.4 %.
- 20:41 native (2280x1080) run: presents 19.2 / 20.2 / 19.6 per s, renders ~235/s, KairoGames GPU 28.5 %, DWM 2.9 %,
  cpu4 66.3 / cpu5 63.5 %. BUT the scene differs: native dump = title screen, the 720p dump = gameplay (Continue was
  pressed during that run) -> not comparable; re-running 720p on the title screen.
- 20:44 720p title-screen re-run: presents 19.8 / 20.0 / 20.4 per s, renders ~240/s, KairoGames GPU 20.7 %, DWM 2.5 %,
  cpu4 68.5 %. => title screen: native 19.2-20.2 vs 720p 19.8-20.4 = NO fps difference; GPU load 28.5 -> 20.7 %.
  Next: gameplay at both (gamebench.sh CLICK "0.5 0.74" = Continue, click.ps1).
- 20:56-21:00 GAMEPLAY comparison, identical scene (autosave -> fiscal-year dialog; clicks "0.5 0.74 0.51 0.52 0.51
  0.52": Continue, select save, confirm), dumps traces/wddm2/g20playnative.png / g20play720.png:
  | mode       | presents/s       | renders/s | game GPU 3D | cpu4 / cpu5   |
  | 2280x1080  | 15.0 / 15.8 / 14.8 | ~273    | 29.0 %      | 72.5 / 65.3 % |
  | 1520x720   | 19.8 / 20.0 / 20.0 | ~359    | 26.0 %      | 68.2 / 59.1 % |
  => gameplay +30 % fps at 720p (title screen: no difference, ~20 both). ~18 render submissions per frame at both.
- Bottleneck reading (user: "why 15 fps and 30 % GPU?"): GPU 3D engine 26-29 % busy = the GPU waits for work; the
  busiest cores are the two A75 MID cores at 60-75 % (the x86-emulated Unity main + render threads, handing frames to
  each other), LITTLE cores mostly idle; 2.25x fewer pixels gave only +30 % -> mostly CPU/per-frame-latency bound
  (emulated x86 + Unity + D3D11 runtime + our UMD), with a serialized GPU part per frame (GPU time adds to the frame,
  does not overlap). Lead to check: ~18 Render DDI submissions per frame (each a kernel round trip + Mali job chain).
